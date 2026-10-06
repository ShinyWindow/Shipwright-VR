extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "objects/gameplay_keep/gameplay_keep.h"
#include "src/overlays/actors/ovl_En_Boom/z_en_boom.h"
extern PlayState* gPlayState;
}
#include "VrCombat.h"
#include "VrPocket.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/frame_interpolation.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <cmath>

// Physical boomerang (VR first person, selector mode). User spec (September 16, 2026): "just like
// bombchu, when you select the item, a pocket shows up in front of you for boomerang. you grab
// that, and can throw it. it should follow vanilla gameplay mechanics after thrown (if possible)
// and when it comes back to you, you should be able to physically catch it, or if you don't catch
// it, it should just go back to that pocket in front of you."
//
// Why this is its own module and not a VrItemThrow adapter: in vanilla the boomerang has NO held
// actor. Selecting it sets PLAYER_STATE1_USING_BOOMERANG and puts a hand display list in Link's
// fist; the EnBoom actor exists only from frame 6 of the throw animation until the 40-unit return
// to Link's head, where it clears PLAYER_STATE1_BOOMERANG_THROWN and kills itself. Everything the
// bomb/nut/chu path does (heldActor, CARRYING_ACTOR, Player_VrGrabItem/ReleaseItem, the carry pin)
// has nothing to hold. So the carry here is VIRTUAL: a small state machine and a model draw.
//
// POCKET  The boomerang is selected in normal play and nothing is in flight: gBoomerangRefDL (the
//         flying actor's own model, always loaded) sits at the bombchu's pocket point, shrunk by
//         gVrBoomerangPreviewScale, level, and Link's fist model is hidden (the VR-drawn model IS
//         the boomerang). The trigger mirror stands down so nothing starts the vanilla aim stance.
// HELD    A FRESH grip press by either hand within reach takes it: the presented pose is captured
//         in the hand's frame (VrPocket::Grab) and replayed every tick, so it rides the hand
//         rigidly. The carry hand's grip release with hand speed >= gVrBoomerangThrowMinSpeed is
//         the throw; a still release, switching items, the sword chord, F9, pause-and-change and
//         a save-state load all put it BACK IN THE POCKET (behavior plan: switching never silently
//         turns into a throw). A catch straight out of the air lands here too, in the default
//         at-the-hand pose, ready to throw again without re-grabbing.
// THROWN  Player_VrThrowBoomerang (z_player.c) spawns EnBoom at the hand, flying along the hand's
//         velocity (yaw AND pitch), with vanilla's lock-on homing as the aim assist and vanilla
//         everything after that: speed, stun, rupee/token fetch, wall bounce, the 20-tick outbound
//         leg. The module then feeds the actor the CATCH HAND every tick (EnBoom vr* fields): the
//         throwing hand, or the other hand when its grip is closed and it is closer. On the return
//         leg the actor homes on that hand instead of Link's head and arrives when its travel
//         sweeps the catch sphere (gVrBoomerangCatchRadius). Grip closed as it arrives = CAUGHT:
//         the actor finishes its return exactly as vanilla (deposits a fetched item, clears the
//         flag, dies) and the boomerang is in that hand. Grip open = missed: same vanilla end, the
//         pocket re-presents it. Only one boomerang exists at a time, as vanilla: the pocket stays
//         hidden while anything is in flight (ours, a vanilla throw, a save-state flight).
// Returning while another item is selected: vanilla already lets the flag drop and the actor die
// silently and leaves the new item in hand (behavior plan: recover without forcing a catch or
// changing equipment); this module adds the catch sound and a light haptic on the sword hand.
//
// Input ownership: the grip loses its normal binding where it grabs (near the pocket), carries
// (the carry hand) or catches (the throwing hand all flight, both hands on the return leg). The
// trigger mirror is off while pocketed/carried and back on in flight so FastBoomerang's recall
// still works from the trigger — after the trigger has been seen up once, so a trigger held
// through the throw can't recall it on the spot. Fallback: gVrPhysBoomerang=0 restores the trigger
// aim-and-throw from Link's facing (no pocket).

namespace {

enum class State { None, Pocket, Held, Thrown };

State sState = State::None;
int sCarryHand = -1;                // HELD: the hand the boomerang rides
int sThrowHand = -1;                // THROWN: the hand that threw it (the default catch hand)
bool sGripPrev[2] = { true, true }; // fresh-press tracking: a grip held from before counts as nothing
int sObservedItem = -1;
VrPocket::Grab sGrab;       // HELD: rigid grab transform (invalid = default at-the-hand pose, e.g. after a catch)
EnBoom* sThrown = nullptr;  // THROWN: the actor we spawned; dereferenced only once FindThrown finds it linked
bool sTriggerLatched = false; // THROWN: trigger was down at the throw; the mirror stays down until it lifts
bool sReturnLeg = false;      // THROWN: cached each tick for padmgr (no actor deref off the tick)
int sPocketInterpKey; // address only: frame-interpolation identities for the two draws
int sHeldInterpKey;
VrBoomerangDebug sDebug = { 0, 0, -1, -1, 0.0f, 1.0f, -1, 0, 0.0f, 0.0f, 0, -1 };

int SwordHandIdx() {
    return CVarGetInteger("gVrLeftHanded", 0) ? VR_HAND_LEFT : VR_HAND_RIGHT;
}

bool SwapChord() {
    const auto mask = (uint16_t)CVarGetInteger("gVrItemSelSwapInput", VR_BTN_GRIP);
    return mask && (VR_GetControllerButton(0) & mask) && (VR_GetControllerButton(1) & mask);
}

bool GripDown(int hand) {
    return hand >= 0 && (VR_GetControllerButton(hand) & VR_BTN_GRIP) != 0;
}

float CmToUnits(float cm) {
    return cm * 0.01f * VrPocket::WorldScale();
}

int StateIndex() {
    switch (sState) {
        case State::Pocket: return 1;
        case State::Held: return 2;
        case State::Thrown: return 3;
        default: return 0;
    }
}

// The thrown actor if it is still linked in the actor list — alive, or killed this frame and
// awaiting deletion (its fields are still readable then) — else nullptr. The pointer is only
// compared, never dereferenced, until it is found, so a freed or recycled slot can't be read.
EnBoom* FindThrown(PlayState* play) {
    if (sThrown == nullptr || play == nullptr) return nullptr;
    for (Actor* actor = play->actorCtx.actorLists[ACTORCAT_MISC].head; actor != nullptr; actor = actor->next) {
        if (actor == &sThrown->actor) return actor->id == ACTOR_EN_BOOM ? sThrown : nullptr;
    }
    return nullptr;
}

void ClearCarry() {
    sCarryHand = -1;
    sGrab.Clear();
}

void EnterPocket() {
    sState = State::Pocket;
    ClearCarry();
}

void EnterNone() {
    sState = State::None;
    ClearCarry();
    sGripPrev[0] = sGripPrev[1] = true;
}

bool HeldPose(float position[3], float left[3], float up[3], float forward[3]) {
    return sCarryHand >= 0 && sGrab.Replay(sCarryHand, position, left, up, forward);
}

// Mean hand velocity over the last 100 ms of this tick's path (physical m/s; the runtime's
// velocity as a fallback). The same recipe as the bomb throw, so the two feel alike.
void ReleaseVelocity(int hand, float velocity[3]) {
    velocity[0] = velocity[1] = velocity[2] = 0.0f;
    const auto& path = VrCombat::GetTickPath(hand);
    int samples = 0;
    if (path.count) {
        const auto latest = path.samples[path.count - 1].timeNs;
        for (int i = 0; i < path.count; ++i) {
            if (latest - path.samples[i].timeNs > 100000000ULL) continue;
            for (int axis = 0; axis < 3; ++axis) velocity[axis] += path.samples[i].linVelMps[axis];
            ++samples;
        }
    }
    if (samples) {
        for (int axis = 0; axis < 3; ++axis) velocity[axis] /= samples;
    } else {
        float angular[3];
        if (!VR_GetHandVelocity(hand, velocity, angular)) velocity[0] = velocity[1] = velocity[2] = 0.0f;
    }
}

// The catch hand this tick: the throwing hand, unless the other hand has its grip closed and is
// closer to the boomerang. -1 when neither hand is tracked. handPosOut = that hand's controller.
int CatchHand(const EnBoom* boom, float handPosOut[3]) {
    float pos[2][3], rot[4];
    const bool tracked[2] = { VR_GetHandPose(0, pos[0], rot), VR_GetHandPose(1, pos[1], rot) };
    int hand = sThrowHand >= 0 ? sThrowHand : SwordHandIdx();
    const int other = 1 - hand;
    auto distSq = [&](int h) {
        const float dx = pos[h][0] - boom->actor.world.pos.x;
        const float dy = pos[h][1] - boom->actor.world.pos.y;
        const float dz = pos[h][2] - boom->actor.world.pos.z;
        return dx * dx + dy * dy + dz * dz;
    };
    if (!tracked[hand]) {
        if (!tracked[other]) return -1;
        hand = other;
    } else if (tracked[other] && GripDown(other) && distSq(other) < distSq(hand)) {
        hand = other;
    }
    for (int i = 0; i < 3; ++i) handPosOut[i] = pos[hand][i];
    return hand;
}

// THROWN, actor alive: hand the actor its home for this tick. It only uses it on the return leg,
// and consumes it, so a module that stops feeding leaves the actor on the vanilla return.
void FeedReturn(Player* player, EnBoom* boom) {
    float handPos[3];
    const int hand = CatchHand(boom, handPos);
    sReturnLeg = boom->returnTimer == 0 || player->boomerangQuickRecall;
    sDebug.returnLeg = sReturnLeg ? 1 : 0;
    sDebug.catchRadiusCm = CVarGetFloat("gVrBoomerangCatchRadius", 20.0f);
    if (hand < 0) {
        boom->vrHomeValid = false;
        sDebug.catchArmed = 0;
        sDebug.handDistanceCm = -1.0f;
        return;
    }
    boom->vrHomePos = { handPos[0], handPos[1], handPos[2] };
    boom->vrHomeValid = true;
    boom->vrCatchHand = (s8)hand;
    boom->vrArriveRadius = CmToUnits(sDebug.catchRadiusCm);
    boom->vrCatchArmed = GripDown(hand);
    sDebug.catchArmed = boom->vrCatchArmed ? 1 : 0;
    const float dx = handPos[0] - boom->actor.world.pos.x;
    const float dy = handPos[1] - boom->actor.world.pos.y;
    const float dz = handPos[2] - boom->actor.world.pos.z;
    sDebug.handDistanceCm = std::sqrt(dx * dx + dy * dy + dz * dz) / (0.01f * VrPocket::WorldScale());
}

// The flight ended: the actor is killed (return finished, ours or vanilla's) or gone (scene
// change). Where is the boomerang now?
void EndFlight(Player* player, EnBoom* boom, bool covers) {
    const bool caught = boom != nullptr && boom->vrCaught;
    const int catchHand = boom != nullptr ? boom->vrCatchHand : -1;
    sThrown = nullptr;
    sReturnLeg = false;
    sTriggerLatched = false;
    // Grip edges were not tracked during the flight: a hand closed to catch must not read as a
    // fresh press on the pocket that re-presents after a miss.
    sGripPrev[0] = sGripPrev[1] = true;
    if (covers && caught && catchHand >= 0 && catchHand <= 1) {
        // In the hand that closed on it. Vanilla's in-flight upper action plays the catch sound
        // itself on the flag drop while the boomerang is selected, so only the haptic is ours.
        sState = State::Held;
        sCarryHand = catchHand;
        sGrab.Clear();
        sGripPrev[catchHand] = true; // the grip is closed on it: opening the hand is the next release
        VR_TriggerHaptic(catchHand, 0.8f, 0.0f, 60.0f);
        sDebug.lastReturn = 1;
    } else if (covers) {
        EnterPocket();
        sDebug.lastReturn = 0;
    } else {
        // Another item is selected: vanilla lets the flag drop and the actor die in silence and
        // keeps the new item in hand. Keep that outcome; add the feedback vanilla skips.
        Player_PlaySfx(&player->actor, NA_SE_PL_CATCH_BOOMERANG);
        VR_TriggerHaptic(SwordHandIdx(), 0.3f, 0.0f, 30.0f);
        EnterNone();
        sDebug.lastReturn = 2;
    }
}

} // namespace

extern "C" bool VrBoomerang_Covers(Player* player) {
    return VrItemSelect_ModeActive() && CVarGetInteger("gVrPhysBoomerang", 1) && player != nullptr &&
           player->actor.category == ACTORCAT_PLAYER && player->heldItemAction == PLAYER_IA_BOOMERANG &&
           player->heldItemAction == player->itemAction && gPlayState != nullptr &&
           gPlayState->bombchuBowlingStatus == 0 && gPlayState->shootingGalleryStatus == 0 &&
           !(player->stateFlags1 & (PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_IN_WATER));
}

extern "C" void VrBoomerang_Reset(void) {
    EnterNone();
    sObservedItem = -1;
    sThrown = nullptr;
    sThrowHand = -1;
    sTriggerLatched = false;
    sReturnLeg = false;
}

extern "C" bool VrBoomerang_PreviewIsModel(void) {
    return sState == State::Pocket;
}

extern "C" bool VrBoomerang_HidesHandModel(void) {
    return sState != State::None;
}

extern "C" bool VrBoomerang_TriggerStandsDown(void) {
    return sState == State::Pocket || sState == State::Held || (sState == State::Thrown && sTriggerLatched);
}

extern "C" bool VrBoomerang_GripConsumed(int32_t hand, uint16_t mask) {
    if (!(mask & VR_BTN_GRIP) || gPlayState == nullptr || VrOcarina_InPlay() || hand < 0 || hand > 1) {
        return false;
    }
    switch (sState) {
        case State::Pocket: {
            // Only a hand close enough to grab loses its binding — a grip press elsewhere keeps
            // Z-target and friends.
            float point[3];
            return VrPocket::Point(point) && VrPocket::HandNear(hand, point);
        }
        case State::Held:
            return hand == sCarryHand;
        case State::Thrown:
            // Closing the hand to catch must not fire the grip's binding: the throwing hand for
            // the whole flight, either hand once it is on its way back.
            return hand == sThrowHand || sReturnLeg;
        default:
            return false;
    }
}

extern "C" void VrBoomerang_GetDebug(VrBoomerangDebug* out) {
    if (out != nullptr) *out = sDebug;
}

extern "C" void VrBoomerang_Tick(PlayState* play, Player* player) {
    if (play == nullptr || player == nullptr || player->actor.category != ACTORCAT_PLAYER) return;
    const bool covers = VrBoomerang_Covers(player);
    sDebug.throwMinSpeed = CVarGetFloat("gVrBoomerangThrowMinSpeed", 1.0f);

    // 1. Flight tracking runs whatever is selected: the actor is out in the world, and the pocket
    //    (or the hand) is where it lands when the return ends.
    if (sState == State::Thrown) {
        EnBoom* boom = FindThrown(play);
        if (boom != nullptr && boom->actor.update != NULL) {
            FeedReturn(player, boom);
            if (sTriggerLatched && !(VR_GetControllerButton(SwordHandIdx()) & VR_BTN_TRIGGER)) {
                sTriggerLatched = false;
            }
            sDebug.state = 3;
            sDebug.gate = 0;
            sDebug.carryHand = -1;
            sDebug.throwHand = sThrowHand;
            return;
        }
        EndFlight(player, boom, covers);
    }

    // 2. The pocket and the hand need the boomerang selected in normal play.
    int gate = 0;
    if (!CVarGetInteger("gVrPhysBoomerang", 1)) {
        gate = 1;
    } else if (!covers || !VrItemSelect_SelectionAllowed()) {
        gate = 2;
    } else if (play->pauseCtx.state != 0 || player->unk_6AD != 0) {
        gate = 3;
    } else if (player->stateFlags1 & PLAYER_STATE1_BOOMERANG_THROWN) {
        gate = 4; // a boomerang we did not throw is out (vanilla throw, save-state flight): one at a time
    }
    sDebug.gate = gate;
    if (gate == 3) {
        // Paused / Link busy: hold position (a carried boomerang stays carried) and re-arm the
        // grips so whatever happened meanwhile reads as fresh input afterwards.
        sGripPrev[0] = sGripPrev[1] = true;
        sDebug.state = StateIndex();
        return;
    }
    if (gate != 0) {
        EnterNone();
        sDebug.state = 0;
        sDebug.carryHand = -1;
        return;
    }
    if (sState == State::None) EnterPocket();
    if (sObservedItem != player->heldItemId) {
        sObservedItem = player->heldItemId;
        sGripPrev[0] = sGripPrev[1] = true;
    }
    bool pressed[2], released[2];
    for (int hand = 0; hand < 2; ++hand) {
        const bool grip = GripDown(hand);
        pressed[hand] = grip && !sGripPrev[hand];
        released[hand] = !grip && sGripPrev[hand];
        sGripPrev[hand] = grip;
    }
    if (SwapChord() || VrItemSelect_PendingSlot() != -2) {
        // Switching (or the sword chord) never turns into a throw: back to the pocket.
        if (sState == State::Held) EnterPocket();
        sDebug.state = StateIndex();
        sDebug.carryHand = sCarryHand;
        return;
    }

    if (sState == State::Held) {
        const int hand = sCarryHand;
        if (released[hand]) {
            float velocity[3];
            ReleaseVelocity(hand, velocity);
            const float speed = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
            sDebug.lastReleaseSpeed = speed;
            bool thrown = false;
            if (speed >= sDebug.throwMinSpeed) {
                const float dir[3] = { velocity[0] / speed, velocity[1] / speed, velocity[2] / speed };
                float position[3], left[3], up[3], forward[3], rotation[4];
                // Leaves from where the carried model is (the hand); an untracked hand throws nothing.
                if (HeldPose(position, left, up, forward) || VR_GetHandPose(hand, position, rotation)) {
                    EnBoom* boom = Player_VrThrowBoomerang(play, player, position, dir);
                    if (boom != nullptr) {
                        sThrown = boom;
                        sThrowHand = hand;
                        sState = State::Thrown;
                        sReturnLeg = false;
                        // A trigger held through the throw must not read as a fresh item press
                        // (FastBoomerang would recall it on the spot).
                        sTriggerLatched = (VR_GetControllerButton(SwordHandIdx()) & VR_BTN_TRIGGER) != 0;
                        ClearCarry();
                        VR_TriggerHaptic(hand, 0.35f, 0.0f, 25.0f);
                        sDebug.lastRelease = 1;
                        thrown = true;
                    } else {
                        sDebug.lastRelease = 2;
                    }
                } else {
                    sDebug.lastRelease = 2;
                }
            } else {
                sDebug.lastRelease = 0;
            }
            if (!thrown) EnterPocket();
        }
    } else if (sState == State::Pocket) {
        float point[3];
        if (!VrPocket::Point(point)) {
            sDebug.gate = 5;
        } else {
            // Any hand may take it; sword hand checked first on a same-tick tie.
            const int first = SwordHandIdx();
            for (int i = 0; i < 2; ++i) {
                const int hand = i == 0 ? first : 1 - first;
                if (pressed[hand] && VrPocket::HandNear(hand, point)) {
                    float left[3], up[3], forward[3];
                    if (VrPocket::Axes(left, up, forward)) {
                        sGrab.Capture(hand, point, left, up, forward);
                    } else {
                        sGrab.Clear();
                    }
                    sCarryHand = hand;
                    sState = State::Held;
                    VR_TriggerHaptic(hand, 0.5f, 0.0f, 35.0f);
                    break;
                }
            }
        }
    }
    sDebug.state = StateIndex();
    sDebug.carryHand = sCarryHand;
    sDebug.throwHand = sThrowHand;
}

// The pocket / carried model. Own OnPlayDrawEnd hook with minimal gates (the bombchu preview
// pattern). extern "C" linkage is load-bearing for the block-scope FrameInterpolation
// declarations inside OPEN_DISPS (see VrItemSelect_Draw).
extern "C" void VrBoomerang_Draw(void) {
    if (gPlayState == nullptr || (sState != State::Pocket && sState != State::Held)) return;
    Player* player = GET_PLAYER(gPlayState);
    if (!VrBoomerang_Covers(player) || !VrItemSelect_SelectionAllowed()) return;
    float position[3], left[3], up[3], forward[3];
    float scale = 0.01f; // the flying actor's own scale
    const void* key = &sHeldInterpKey;
    if (sState == State::Pocket) {
        if (!VrPocket::Point(position) || !VrPocket::Axes(left, up, forward)) return;
        scale *= CVarGetFloat("gVrBoomerangPreviewScale", 50.0f) / 100.0f;
        key = &sPocketInterpKey;
    } else if (!HeldPose(position, left, up, forward)) {
        return;
    }
    Vec3s rot;
    VrPocket::SetRotFromAxes(&rot, left, up, forward);

    OPEN_DISPS(gPlayState->state.gfxCtx);
    FrameInterpolation_RecordOpenChild(key, 0);
    Matrix_SetTranslateRotateYXZ(position[0], position[1], position[2], &rot);
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    MtxF cur;
    Matrix_Get(&cur);
    Mtx* mtx = MATRIX_NEWMTX(gPlayState->state.gfxCtx);
    MtxF hand;
    if (sState == State::Held && VR_GetHandMatrix(sCarryHand, hand.mf)) {
        // Welded to the live controller (the held lens's recipe): the hand-LOCAL part against this
        // tick's hand, re-composed per eye with the rendered hand.
        MtxF inv;
        MtxF local;
        SkinMatrix_Invert(&hand, &inv);
        SkinMatrix_MtxFMtxFMult(&inv, &cur, &local);
        VR_RegisterHandChildMatrix(mtx, sCarryHand, &local.mf[0][0]);
    }
    POLY_OPA_DISP = Play_SetFog(gPlayState, POLY_OPA_DISP);
    Gfx_SetupDL_25Opa(gPlayState->state.gfxCtx);
    gSPMatrix(POLY_OPA_DISP++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gBoomerangRefDL);
    FrameInterpolation_RecordCloseChild();
    CLOSE_DISPS(gPlayState->state.gfxCtx);
}

namespace {
void RegisterVrBoomerang() {
    COND_HOOK(OnPlayDrawEnd, true, VrBoomerang_Draw);
}
} // namespace

static RegisterShipInitFunc initVrBoomerang(RegisterVrBoomerang);
