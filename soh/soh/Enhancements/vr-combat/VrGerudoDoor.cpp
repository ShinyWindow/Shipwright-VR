extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "overlays/actors/ovl_Door_Gerudo/z_door_gerudo.h"
extern PlayState* gPlayState;
}

#include "VrCombat.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include <libultraship/bridge/consolevariablebridge.h>
#include <vr_interface.h>
#include <algorithm>
#include <cmath>
#include <cstring>

// Gerudo Fortress cell grates are not Door_Shutter actors, but physically they are the same
// interaction: two hands may grab anywhere on the unlocked slab and lift it vertically. The
// DoorGerudo actor remains responsible for the small-key cost, save switch and final open state.
namespace {

constexpr float kHalfWidth = 52.0f;
constexpr float kGrabMinY = 22.0f;
constexpr float kGrabMaxY = 145.0f;
constexpr float kHoverDepth = 28.0f;
constexpr float kRaisedHeight = 200.0f;
constexpr float kReleaseLiftMeters = 0.30f;

struct HandState {
    DoorGerudo* door = nullptr;
    uint32_t frame = 0;
    bool active = false;
    bool latched = false;
    float score = 1e30f;
    float localX = 0.0f;
    float localY = 0.0f;
    float localZ = 0.0f;
    float face = 1.0f;
};

DoorGerudo* sDoor = nullptr;
HandState sHands[2];
bool sGripNow[2] = {};
bool sGripPrev[2] = { true, true };
bool sGripFresh[2] = {};
uint32_t sInputFrame = UINT32_MAX;
float sStartMeanY = 0.0f;
float sStartHeight = 0.0f;
bool sMoving = false;
bool sLiftArmed = false;
bool sSoundPlayed = false;

float WorldScale() {
    const float scale = VR_GetWorldScale();
    return scale < 1.0f ? 35.0f : scale;
}

void Clear() {
    sDoor = nullptr;
    sHands[0] = {};
    sHands[1] = {};
    sMoving = false;
    sLiftArmed = false;
    sSoundPlayed = false;
}

void BeginInputFrame(PlayState* play) {
    if (sInputFrame == play->state.frames) return;
    sInputFrame = play->state.frames;
    for (int hand = 0; hand < 2; ++hand) {
        sGripNow[hand] = (VR_GetControllerButton(hand) & VR_BTN_GRIP) != 0;
        sGripFresh[hand] = sGripNow[hand] && !sGripPrev[hand];
        sGripPrev[hand] = sGripNow[hand];
        if (!sHands[hand].latched) sHands[hand] = {};
    }
}

bool Eligible(DoorGerudo* door, PlayState* play) {
    if (door == nullptr || play == nullptr || !CVarGetInteger("gVrPhysicalDoors", 1) ||
        !VR_IsInitialized() || !VR_GetFirstPerson() || VR_IsFlatScreen() ||
        !CVarGetInteger("gVrMotionHands", 1) || play->csCtx.state != CS_STATE_IDLE ||
        play->transitionTrigger != TRANS_TRIGGER_OFF || door->unk_166 != 0) {
        return false;
    }
    Player* player = GET_PLAYER(play);
    return player != nullptr && !(player->stateFlags1 &
        (PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_IN_WATER | PLAYER_STATE1_DEAD |
         PLAYER_STATE1_IN_CUTSCENE));
}

bool HandContact(DoorGerudo* door, int hand, float* localX, float* localY, float* localZ,
                 float* handY, float* face) {
    float matrix[4][4];
    if (!VR_GetHandMatrix(hand, matrix)) return false;
    *handY = matrix[3][1];
    const float sn = Math_SinS(door->dyna.actor.shape.rot.y);
    const float cs = Math_CosS(door->dyna.actor.shape.rot.y);
    const float length = VrHand_ChildSized() ? 548.0f : 862.0f;
    float best = 1e30f;
    bool found = false;
    for (int point = 0; point < 3; ++point) {
        const float along = length * 0.5f * point;
        const float px = matrix[3][0] + matrix[0][0] * along;
        const float py = matrix[3][1] + matrix[0][1] * along;
        const float pz = matrix[3][2] + matrix[0][2] * along;
        const float dx = px - door->dyna.actor.world.pos.x;
        const float dz = pz - door->dyna.actor.world.pos.z;
        const float x = dx * cs - dz * sn;
        const float z = dx * sn + dz * cs;
        const float y = py - door->dyna.actor.world.pos.y;
        if (fabsf(x) <= kHalfWidth && y >= kGrabMinY && y <= kGrabMaxY &&
            fabsf(z) <= kHoverDepth && fabsf(z) < best) {
            best = fabsf(z);
            *localX = x;
            *localY = y;
            *localZ = z;
            *face = z >= 0.0f ? 1.0f : -1.0f;
            found = true;
        }
    }
    return found;
}

void ForgetActor(Actor* actor) {
    if (actor != nullptr && actor == reinterpret_cast<Actor*>(sDoor)) Clear();
}

} // namespace

extern "C" int32_t VrGerudoDoor_Tick(DoorGerudo* door, PlayState* play) {
    if (!Eligible(door, play)) {
        if (door == sDoor) Clear();
        return -1;
    }
    if (sDoor != nullptr && sDoor != door) return 0;
    BeginInputFrame(play);

    float handY[2] = { NAN, NAN };
    for (int hand = 0; hand < 2; ++hand) {
        float x = 0.0f, y = 0.0f, z = 0.0f, face = 1.0f;
        const bool near = HandContact(door, hand, &x, &y, &z, &handY[hand], &face);
        if (sHands[hand].latched && sDoor == door) {
            if (!std::isfinite(handY[hand])) {
                sHands[hand].latched = false;
                continue;
            }
            sHands[hand].frame = play->state.frames;
            sHands[hand].active = true;
            if (!sGripNow[hand]) {
                sHands[hand].latched = false;
            }
        } else if (near && sDoor == nullptr) {
            HandState& state = sHands[hand];
            state.door = door;
            state.frame = play->state.frames;
            state.active = true;
            state.score = fabsf(z);
            state.localX = x;
            state.localY = y;
            state.localZ = face * 3.0f;
            state.face = face;
            if (sGripFresh[hand]) {
                sDoor = door;
                state.latched = true;
                VR_TriggerHaptic(hand, 0.3f, 0.0f, 25.0f);
            }
        } else if (near && sDoor == door && !sHands[hand].latched) {
            HandState& state = sHands[hand];
            state.door = door;
            state.frame = play->state.frames;
            state.active = true;
            state.score = fabsf(z);
            state.localX = x;
            state.localY = y;
            state.localZ = face * 3.0f;
            state.face = face;
            if (sGripFresh[hand]) {
                state.latched = true;
                VR_TriggerHaptic(hand, 0.3f, 0.0f, 25.0f);
            }
        }
    }

    const bool bothHeld = sDoor == door && sHands[0].latched && sHands[1].latched &&
                          sGripNow[0] && sGripNow[1];
    if (!bothHeld) {
        if (sMoving && sLiftArmed) {
            door->dyna.actor.velocity.y = 1.0f;
            sHands[0] = {};
            sHands[1] = {};
            sMoving = false;
            sLiftArmed = false;
            VR_TriggerHaptic(VR_HAND_LEFT, 0.75f, 0.0f, 45.0f);
            VR_TriggerHaptic(VR_HAND_RIGHT, 0.75f, 0.0f, 45.0f);
            Clear();
            return 2;
        }
        sMoving = false;
        if (sDoor == door && !sHands[0].latched && !sHands[1].latched) Clear();
        return 1;
    }

    const float meanY = (handY[0] + handY[1]) * 0.5f;
    if (!sMoving) {
        sMoving = true;
        sStartMeanY = meanY;
        sStartHeight = door->dyna.actor.world.pos.y;
    }
    const float handLift = std::max(0.0f, meanY - sStartMeanY);
    if (handLift >= WorldScale() * kReleaseLiftMeters / 3.0f) sLiftArmed = true;
    const float target = std::clamp(sStartHeight + handLift * 3.0f, door->dyna.actor.home.pos.y,
                                    door->dyna.actor.home.pos.y + kRaisedHeight);
    if (target > door->dyna.actor.world.pos.y && !sSoundPlayed) {
        Audio_PlayActorSound2(&door->dyna.actor, NA_SE_EV_SLIDE_DOOR_OPEN);
        sSoundPlayed = true;
    }
    door->dyna.actor.world.pos.y = target;
    if (target >= door->dyna.actor.home.pos.y + kRaisedHeight - 5.0f) {
        door->dyna.actor.world.pos.y = door->dyna.actor.home.pos.y + kRaisedHeight;
        door->dyna.actor.velocity.y = 1.0f;
        sHands[0] = {};
        sHands[1] = {};
        sMoving = false;
        sLiftArmed = false;
        VR_TriggerHaptic(VR_HAND_LEFT, 0.75f, 0.0f, 45.0f);
        VR_TriggerHaptic(VR_HAND_RIGHT, 0.75f, 0.0f, 45.0f);
        Clear();
        return 2;
    }
    return 1;
}

extern "C" bool VrGerudoDoor_GripConsumed(int32_t hand, uint16_t mask) {
    // Pad bindings are resolved before actors refresh their contact state for the new gameplay
    // frame. Keep the most recent hover alive across that boundary so the grip that actually takes
    // the grate cannot also leak through as B/R and draw Link's sword or shield.
    const bool hovering = hand >= 0 && hand < 2 && gPlayState != nullptr && sHands[hand].active &&
                          (uint32_t)(gPlayState->state.frames - sHands[hand].frame) <= 2;
    return (mask & VR_BTN_GRIP) && hand >= 0 && hand < 2 &&
           (hovering || sHands[hand].latched || sMoving);
}

extern "C" bool VrGerudoDoor_HoldsHand(int32_t hand) {
    return hand >= 0 && hand < 2 && gPlayState != nullptr && sHands[hand].active &&
           sHands[hand].frame == gPlayState->state.frames;
}

extern "C" bool VrGerudoDoor_PinnedHandMatrix(Player* player, int32_t hand, float* mf16) {
    if (player == nullptr || mf16 == nullptr || hand < 0 || hand > 1 || gPlayState == nullptr ||
        player != GET_PLAYER(gPlayState)) {
        return false;
    }
    const HandState& state = sHands[hand];
    if (!state.active || state.frame != gPlayState->state.frames || state.door == nullptr) return false;
    DoorGerudo* door = state.door;
    const float sn = Math_SinS(door->dyna.actor.shape.rot.y);
    const float cs = Math_CosS(door->dyna.actor.shape.rot.y);
    const float sx = sqrtf(mf16[0] * mf16[0] + mf16[1] * mf16[1] + mf16[2] * mf16[2]);
    const float sy = sqrtf(mf16[4] * mf16[4] + mf16[5] * mf16[5] + mf16[6] * mf16[6]);
    const float sz = sqrtf(mf16[8] * mf16[8] + mf16[9] * mf16[9] + mf16[10] * mf16[10]);
    const float palmTurn = hand == VR_HAND_LEFT ? 1.0f : -1.0f;
    mf16[0] = state.face * palmTurn * cs * sx;
    mf16[1] = 0.0f;
    mf16[2] = -state.face * palmTurn * sn * sx;
    mf16[4] = 0.0f;
    mf16[5] = sy;
    mf16[6] = 0.0f;
    mf16[8] = state.face * palmTurn * sn * sz;
    mf16[9] = 0.0f;
    mf16[10] = state.face * palmTurn * cs * sz;
    const float halfLength = (VrHand_ChildSized() ? 548.0f : 862.0f) * 0.5f;
    mf16[12] = door->dyna.actor.world.pos.x + state.localX * cs + state.localZ * sn;
    mf16[13] = door->dyna.actor.world.pos.y + state.localY - sy * halfLength;
    mf16[14] = door->dyna.actor.world.pos.z - state.localX * sn + state.localZ * cs;
    return true;
}

namespace {
void RegisterVrGerudoDoor() {
    COND_HOOK(OnSceneInit, true, [](int16_t) { Clear(); });
    COND_HOOK(OnActorDestroy, true, [](void* actor) { ForgetActor(static_cast<Actor*>(actor)); });
}
static RegisterShipInitFunc initVrGerudoDoor(RegisterVrGerudoDoor);
} // namespace
