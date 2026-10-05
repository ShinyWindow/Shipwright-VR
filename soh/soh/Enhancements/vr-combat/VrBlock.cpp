extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
extern PlayState* gPlayState;
}
#include "VrCombat.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <vr_interface.h>
#include <cmath>
#include <cstring>

// Physical block pushing (VR first person, motion hands). User spec (October 4, 2026): "you gotta
// grab a block with both hands, and that attaches you to it, then you can push it with your hands
// and pull it, and walking forward and backward like in the original game works too. it should
// have weight to it, like pushing the block with your hands shouldn't be able to push it much
// faster or maybe not even faster at all than before".
//
// INTENT INJECTION ONLY. The vanilla grab is untouched: Player_ActionHandler_5 squares Link up to
// the face, the hold/push/pull actions drive dyna.unk_150, Obj_Oshihiki steps 20 units at <= 2
// units/tick and rests 10 ticks. That cadence IS the weight; nothing here can move a block faster.
// The hands only stand in for two controls:
//   A button   both hands on the face + both grips squeezed FRESH there = A held at the grab
//              handler and at the hold gate; either grip opening, or a hand dragged
//              gVrBlockPullOff from where it took hold, = A released.
//   stick      hands pressed in past gVrBlockPushCm = stick forward, drawn back = stick back.
//              Pressure is measured in the player's OWN frame (hand relative to the head, along
//              Link's facing) against the hold pose at the grab, so Link and the block walking
//              forward together never reads as anything. Holding the press = holding the stick =
//              stepping on at the vanilla cadence. A pushed stick always wins.
// While attached by hand the rendered hands ride the block's face (z_player_lib.c draws the pinned
// matrix at the game rate with the block instead of the live controller), the block's slide rumbles
// both hands and each landed step thunks.

namespace {

constexpr float kInsideCm = 30.0f;       // a hand this far INSIDE the face still counts as on it
constexpr float kLatchExitScale = 1.5f;  // a fresh-grip latch survives out to this x the reach
constexpr float kNotMeasured = -999.0f;

bool sGripPrev[2] = { true, true }; // fresh-press tracking: a grip held from before counts as nothing
bool sLatched[2] = { false, false };
bool sAtWall[2] = { false, false };
bool sReachTickArmed[2] = { true, true };
bool sPushable = false;
// Gauntlet pillars (Bg_Heavy_Block) take the same two-hand grab; ActionHandler_5 then starts the vanilla
// lift (gVrPhysCarryHeavy). Armed = both hands ready on a pillar this tick; ByHand = the lift that
// followed was theirs (it is held overhead while both grips stay closed).
bool sPillarArmed = false;
bool sPillarByHand = false;
bool sPillarBuzzed = false;
bool sReady = false;
bool sGateOk = false;
uint32_t sTickFrame = 0;

int sAction = 0;           // Player_VrBlockAction as of this tick
bool sHandGrab = false;    // attached by the hands (else an A-button grab: the hands do nothing)
bool sPulledOff = false;
float sRef[2][3];          // hand - eye at the grab (world units)
int sIntent = 0;
s32 sBgId = BGCHECK_SCENE; // what the hands hold: a dyna (actor + bgId) or scene geometry
Actor* sBlock = nullptr;
float sBlockLast[3];
bool sSliding = false;
float sPlaneN[3];          // the face at the grab (world), for the pin snap
float sPlaneD = 0.0f;
bool sPinValid[2] = { false, false };
float sPinLocal[2][16];    // the hand matrix in the block's frame (MtxF layout)

VrBlockDebug sDebug = { 2, 0, { 0, 0 }, { kNotMeasured, kNotMeasured }, { 0, 0 }, 0, 0, 0.0f, 8.0f, 15.0f, 0,
                        0.0f, 35.0f, 0, 0, -1 };

float WorldScale() {
    const float ws = VR_GetWorldScale();
    return ws < 1.0f ? 35.0f : ws;
}

void Haptic(int hand, float amplitude, float durationMs) {
    const float k = CVarGetInteger("gVrBlockHaptics", 100) * 0.01f;
    if (k > 0.0f) {
        VR_TriggerHaptic(hand, fminf(amplitude * k, 1.0f), 0.0f, durationMs);
    }
}

void HapticBoth(float amplitude, float durationMs) {
    Haptic(VR_HAND_LEFT, amplitude, durationMs);
    Haptic(VR_HAND_RIGHT, amplitude, durationMs);
}

// 0 armed; 1 off; 2 not VR first person with motion hands; 3 the game owns Link.
int Gate(PlayState* play, Player* player) {
    if (!CVarGetInteger("gVrPhysBlockPush", 1)) {
        return 1;
    }
    if (!VR_IsInitialized() || !VR_GetFirstPerson() || VR_IsFlatScreen() || !CVarGetInteger("gVrMotionHands", 1)) {
        return 2;
    }
    if (Player_InBlockingCsMode(play, player) || play->csCtx.state != CS_STATE_IDLE ||
        play->transitionTrigger != TRANS_TRIGGER_OFF ||
        (player->stateFlags1 & (PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_IN_WATER))) {
        return 3;
    }
    return 0;
}

// The touched face's plane: unit normal (out of the face, toward Link) and offset, n.p + d = 0.
bool WallPlane(Player* player, float n[3], float* d) {
    const CollisionPoly* poly = player->actor.wallPoly;
    if (poly == nullptr) {
        return false;
    }
    n[0] = COLPOLY_GET_NORMAL(poly->normal.x);
    n[1] = COLPOLY_GET_NORMAL(poly->normal.y);
    n[2] = COLPOLY_GET_NORMAL(poly->normal.z);
    *d = poly->dist;
    return true;
}

// Hand relative to the head (world units). Both poses carry the same camera anchor, so this is the
// player's own frame: Link (and the block) walking never shows up in it, only the arms do.
bool HandRel(int hand, float out[3]) {
    float p[3], q[4], eye[3], fwd[3], up[3];
    if (!VR_GetHandPose(hand, p, q)) {
        return false;
    }
    VR_GetCameraPose(eye, fwd, up);
    for (int i = 0; i < 3; i++) {
        out[i] = p[i] - eye[i];
    }
    return true;
}

bool IsHeavyBlock(PlayState* play, Player* player) {
    if (player->actor.wallBgId == BGCHECK_SCENE) {
        return false;
    }
    DynaPolyActor* dyna = DynaPoly_GetActor(&play->colCtx, player->actor.wallBgId);
    return dyna != nullptr && dyna->actor.id == ACTOR_BG_HEAVY_BLOCK;
}

// The block the hands hold, still alive at the bgId taken at the grab; nullptr for scene walls.
Actor* LiveBlock(PlayState* play) {
    if (sBgId == BGCHECK_SCENE || sBlock == nullptr) {
        return nullptr;
    }
    DynaPolyActor* dyna = DynaPoly_GetActor(&play->colCtx, sBgId);
    return (dyna != nullptr && &dyna->actor == sBlock) ? sBlock : nullptr;
}

// The frame the pins live in: the block's position + yaw, or the world for a scene wall.
bool BlockFrame(PlayState* play, float pos[3], s16* yaw) {
    if (sBgId == BGCHECK_SCENE) {
        pos[0] = pos[1] = pos[2] = 0.0f;
        *yaw = 0;
        return true;
    }
    Actor* block = LiveBlock(play);
    if (block == nullptr) {
        return false;
    }
    pos[0] = block->world.pos.x;
    pos[1] = block->world.pos.y;
    pos[2] = block->world.pos.z;
    *yaw = block->shape.rot.y;
    return true;
}

// Rotate (x, z) about Y by yaw (game convention: yaw 0 faces +Z, 0x4000 faces +X).
void RotY(s16 yaw, const float in[3], float out[3]) {
    const float s = Math_SinS(yaw), c = Math_CosS(yaw);
    const float x = in[0], z = in[2];
    out[0] = x * c + z * s;
    out[1] = in[1];
    out[2] = -x * s + z * c;
}

void ClearGrab() {
    sHandGrab = false;
    sPulledOff = false;
    sIntent = 0;
    sBgId = BGCHECK_SCENE;
    sBlock = nullptr;
    sSliding = false;
    sPinValid[0] = sPinValid[1] = false;
}

void ClearAll() {
    ClearGrab();
    sLatched[0] = sLatched[1] = false;
    sAtWall[0] = sAtWall[1] = false;
    sReachTickArmed[0] = sReachTickArmed[1] = true;
    sPushable = false;
    sReady = false;
    sAction = 0;
}

// The hand as you SEE it, against the face: the signed plane distance (units) of its closest point —
// wrist, middle or fingertips of the drawn hand (limb +X, adult 862 / child 548 model units, through
// the hand matrix, so the real-life hand scale counts). The controller's grip point alone sits back
// in the fist: with only it, the drawn fingers had to go well into the block before it counted.
// wrist = the hand's origin (for the "in front of Link" test). False = untracked.
bool HandFaceDistance(int hand, const float n[3], float d, float* outDist, float wrist[3]) {
    float m[4][4];
    if (!VR_GetHandMatrix(hand, m)) {
        float q[4];
        if (!VR_GetHandPose(hand, wrist, q)) {
            return false;
        }
        *outDist = n[0] * wrist[0] + n[1] * wrist[1] + n[2] * wrist[2] + d;
        return true;
    }
    const float len = VrHand_ChildSized() ? 548.0f : 862.0f;
    float best = 1e9f;
    for (int i = 0; i <= 2; i++) {
        const float x = len * 0.5f * i;
        float pt[3];
        for (int k = 0; k < 3; k++) {
            pt[k] = m[0][k] * x + m[3][k];
        }
        best = fminf(best, n[0] * pt[0] + n[1] * pt[1] + n[2] * pt[2] + d);
        if (i == 0) {
            wrist[0] = pt[0];
            wrist[1] = pt[1];
            wrist[2] = pt[2];
        }
    }
    *outDist = best;
    return true;
}

// Not attached: which hands are on a pushable face, and which grips were squeezed fresh there.
void TickFree(PlayState* play, Player* player, const bool grip[2], const bool fresh[2]) {
    const float ws = WorldScale();
    const float reachCm = CVarGetFloat("gVrBlockGrabReach", 15.0f);
    float n[3], d = 0.0f;
    const bool heavy = IsHeavyBlock(play, player);
    sPushable = Player_VrTouchingPushable(player) && (!heavy || CVarGetInteger("gVrPhysCarryHeavy", 1)) &&
                WallPlane(player, n, &d);
    for (int h = 0; h < 2; h++) {
        float p[3], dist;
        float distCm = kNotMeasured;
        bool at = false;
        if (sPushable && HandFaceDistance(h, n, d, &dist, p)) {
            distCm = dist / ws * 100.0f;
            // In front of Link's body along the way into the face, not beside or behind him.
            const float ahead = -((p[0] - player->actor.world.pos.x) * n[0] + (p[2] - player->actor.world.pos.z) * n[2]);
            at = ahead > 0.0f && distCm <= reachCm && distCm >= -kInsideCm;
        }
        sAtWall[h] = at;
        sDebug.planeCm[h] = distCm;
        const bool far = !sPushable || distCm == kNotMeasured || distCm > reachCm * kLatchExitScale ||
                         distCm < -kInsideCm * kLatchExitScale;
        if (!grip[h] || far) {
            sLatched[h] = false;
        } else if (fresh[h] && at) {
            sLatched[h] = true;
            Haptic(h, 0.3f, 20.0f);
        }
        if (at && sReachTickArmed[h]) {
            Haptic(h, 0.15f, 12.0f);
            sReachTickArmed[h] = false;
        } else if (!at && (far || distCm > reachCm * 1.3f)) {
            sReachTickArmed[h] = true;
        }
    }
    sReady = sPushable && sAtWall[0] && sAtWall[1] && sLatched[0] && sLatched[1];
    if (sReady && heavy) {
        if (Player_GetStrength() >= PLAYER_STR_GOLD_G) {
            sPillarArmed = true;
            HapticBoth(0.8f, 60.0f);
        } else if (!sPillarBuzzed) {
            // No Golden Gauntlets: it won't budge.
            HapticBoth(0.6f, 90.0f);
            sPillarBuzzed = true;
        }
    } else if (!sReady) {
        sPillarBuzzed = false;
    }
}

// First tick attached. A grab the hands made (both latched grips still down) takes the hold pose
// as the pressure reference and the block as the pins' frame; anything else was the A button.
void BeginGrab(PlayState* play, Player* player, const bool grip[2]) {
    ClearGrab();
    if (!(grip[0] && grip[1] && sLatched[0] && sLatched[1])) {
        return;
    }
    if (!HandRel(0, sRef[0]) || !HandRel(1, sRef[1])) {
        return;
    }
    if (!WallPlane(player, sPlaneN, &sPlaneD)) {
        return;
    }
    sBgId = player->actor.wallBgId;
    if (sBgId != BGCHECK_SCENE) {
        DynaPolyActor* dyna = DynaPoly_GetActor(&play->colCtx, sBgId);
        if (dyna == nullptr) {
            sBgId = BGCHECK_SCENE;
            return;
        }
        sBlock = &dyna->actor;
        sBlockLast[0] = sBlock->world.pos.x;
        sBlockLast[1] = sBlock->world.pos.y;
        sBlockLast[2] = sBlock->world.pos.z;
    }
    sHandGrab = true;
    sDebug.steps = 0;
    HapticBoth(0.6f, 40.0f);
}

// Attached by hand: pressure -> intent, pull-off, the block's slide and step haptics.
void TickHeld(PlayState* play, Player* player) {
    const float ws = WorldScale();
    const float pushCm = CVarGetFloat("gVrBlockPushCm", 8.0f);
    const float pullOffCm = CVarGetFloat("gVrBlockPullOff", 35.0f);
    float rel[2][3];
    if (HandRel(0, rel[0]) && HandRel(1, rel[1])) {
        // Along Link's facing — locked square to the face for the whole grab.
        const float fx = Math_SinS(player->actor.shape.rot.y), fz = Math_CosS(player->actor.shape.rot.y);
        float pressure = 0.0f, drift = 0.0f;
        for (int h = 0; h < 2; h++) {
            const float dx = rel[h][0] - sRef[h][0], dy = rel[h][1] - sRef[h][1], dz = rel[h][2] - sRef[h][2];
            pressure += (dx * fx + dz * fz) * 0.5f;
            drift = fmaxf(drift, sqrtf(dx * dx + dy * dy + dz * dz));
        }
        const float pressureCm = pressure / ws * 100.0f;
        const float driftCm = drift / ws * 100.0f;
        sDebug.pressureCm = pressureCm;
        sDebug.driftCm = driftCm;
        // Hysteresis: engage past the threshold, let go of it at half.
        if (sIntent > 0) {
            sIntent = (pressureCm > pushCm * 0.5f) ? 1 : 0;
        } else if (sIntent < 0) {
            sIntent = (pressureCm < -pushCm * 0.5f) ? -1 : 0;
        }
        if (sIntent == 0) {
            sIntent = (pressureCm > pushCm) ? 1 : ((pressureCm < -pushCm) ? -1 : 0);
        }
        if (!sPulledOff && driftCm > pullOffCm) {
            sPulledOff = true;
            HapticBoth(0.25f, 20.0f);
        }
    } else {
        sIntent = 0; // untracked: hold, never a phantom push
    }

    // The block's own motion (last tick — blocks update after Link): slide rumble while it moves,
    // a thunk when a step lands.
    Actor* block = LiveBlock(play);
    bool moving = false;
    if (block != nullptr) {
        const float dx = block->world.pos.x - sBlockLast[0], dy = block->world.pos.y - sBlockLast[1],
                    dz = block->world.pos.z - sBlockLast[2];
        moving = (dx * dx + dy * dy + dz * dz) > 0.05f * 0.05f;
        sBlockLast[0] = block->world.pos.x;
        sBlockLast[1] = block->world.pos.y;
        sBlockLast[2] = block->world.pos.z;
    }
    if (moving) {
        HapticBoth(0.25f, 60.0f);
    } else if (sSliding) {
        HapticBoth(0.8f, 60.0f);
        sDebug.steps++;
    }
    sSliding = moving;
    sDebug.blockMoving = moving ? 1 : 0;
}

} // namespace

extern "C" void VrBlock_Tick(PlayState* play, Player* player) {
    if (play == nullptr || player == nullptr || player != GET_PLAYER(play)) {
        return; // the co-op partner has no hands
    }
    sTickFrame = play->state.frames;
    sDebug.reachCm = CVarGetFloat("gVrBlockGrabReach", 15.0f);
    sDebug.pushCm = CVarGetFloat("gVrBlockPushCm", 8.0f);
    sDebug.pullOffCm = CVarGetFloat("gVrBlockPullOff", 35.0f);

    bool grip[2], fresh[2];
    for (int h = 0; h < 2; h++) {
        grip[h] = (VR_GetControllerButton(h) & VR_BTN_GRIP) != 0;
        fresh[h] = grip[h] && !sGripPrev[h];
        sGripPrev[h] = grip[h];
    }

    // A pillar lift right after both hands were ready on it was theirs. Tracked before the gate: the lift
    // itself is a cutscene for the rest of this module.
    if (Player_VrPillarLift(player)) {
        if (sPillarArmed) {
            sPillarByHand = true;
        }
    } else {
        sPillarByHand = false;
    }
    sPillarArmed = false;

    const int gate = Gate(play, player);
    sDebug.gate = gate;
    if (gate != 0) {
        sGateOk = false;
        ClearAll();
        sDebug.pushable = 0;
        sDebug.action = 0;
        sDebug.handGrab = 0;
        sDebug.intent = 0;
        sDebug.blockMoving = 0;
        sDebug.planeCm[0] = sDebug.planeCm[1] = kNotMeasured;
        sDebug.atWall[0] = sDebug.atWall[1] = 0;
        sDebug.latched[0] = sDebug.latched[1] = 0;
        return;
    }
    sGateOk = true;

    const int action = Player_VrBlockAction(player);
    if (action != 0 && sAction == 0) {
        BeginGrab(play, player, grip);
    } else if (action == 0 && sAction != 0) {
        // Let go (the vanilla gate saw the grip open / the pull-off, or Link left the grab).
        if (sHandGrab) {
            sDebug.lastRelease = sPulledOff ? 1 : ((!grip[0] || !grip[1]) ? 0 : 2);
            HapticBoth(0.2f, 15.0f);
        }
        ClearGrab();
        // Both grips need a fresh squeeze to grab again; the hands are still at the face, so no
        // re-entry tick either.
        sLatched[0] = sLatched[1] = false;
        sReachTickArmed[0] = sReachTickArmed[1] = false;
    }
    sAction = action;

    if (action == 0) {
        TickFree(play, player, grip, fresh);
    } else {
        sReady = false;
        sAtWall[0] = sAtWall[1] = false;
        if (sHandGrab) {
            TickHeld(play, player);
        } else {
            sIntent = 0;
        }
    }

    sDebug.pushable = sPushable ? 1 : 0;
    sDebug.action = action;
    sDebug.handGrab = sHandGrab ? 1 : 0;
    sDebug.intent = sIntent;
    for (int h = 0; h < 2; h++) {
        sDebug.atWall[h] = sAtWall[h] ? 1 : 0;
        sDebug.latched[h] = sLatched[h] ? 1 : 0;
    }
    if (!sHandGrab) {
        sDebug.pressureCm = 0.0f;
        sDebug.driftCm = 0.0f;
        sDebug.blockMoving = 0;
    }
}

extern "C" bool VrBlock_PillarHold(Player* player) {
    return sPillarByHand && gPlayState != nullptr && player == GET_PLAYER(gPlayState) &&
           (VR_GetControllerButton(VR_HAND_LEFT) & VR_BTN_GRIP) && (VR_GetControllerButton(VR_HAND_RIGHT) & VR_BTN_GRIP);
}

extern "C" bool VrBlock_GrabHeld(Player* player) {
    if (!sGateOk || gPlayState == nullptr || player != GET_PLAYER(gPlayState)) {
        return false;
    }
    if (Player_VrBlockAction(player) == 0) {
        return sReady; // the grab handler: both hands on the face, both grips squeezed fresh there
    }
    // The hold gate: holding on while both grips stay closed and neither hand has been pulled away.
    return sHandGrab && !sPulledOff && (VR_GetControllerButton(VR_HAND_LEFT) & VR_BTN_GRIP) &&
           (VR_GetControllerButton(VR_HAND_RIGHT) & VR_BTN_GRIP);
}

extern "C" int32_t VrBlock_Intent(Player* player, int32_t stickIntent) {
    if (stickIntent != 0) {
        return stickIntent; // the stick works exactly as in the original, and wins
    }
    if (!sGateOk || !sHandGrab || gPlayState == nullptr || player != GET_PLAYER(gPlayState)) {
        return 0;
    }
    return sIntent;
}

extern "C" bool VrBlock_GripConsumed(int32_t hand, uint16_t mask) {
    if (!(mask & VR_BTN_GRIP) || hand < 0 || hand > 1 || !sGateOk || gPlayState == nullptr ||
        VrOcarina_InPlay()) {
        return false;
    }
    // Stale while the player isn't updating (pause, transitions): never hold a binding then.
    if ((uint32_t)gPlayState->state.frames - sTickFrame > 2) {
        return false;
    }
    return sHandGrab || (sPushable && sAtWall[hand]);
}

extern "C" bool VrBlock_PinnedHandMatrix(Player* player, int32_t vrHand, float* mf16) {
    if (vrHand < 0 || vrHand > 1 || mf16 == nullptr || !sGateOk || !sHandGrab || sPulledOff ||
        gPlayState == nullptr || player != GET_PLAYER(gPlayState) || !CVarGetInteger("gVrBlockPinHands", 1) ||
        Player_VrBlockAction(player) == 0) {
        return false;
    }
    float bpos[3];
    s16 yaw;
    if (!BlockFrame(gPlayState, bpos, &yaw)) {
        return false;
    }
    float* local = sPinLocal[vrHand];
    if (!sPinValid[vrHand]) {
        // Take hold where the hand is, slid along the face's normal so the middle of the hand
        // (half of the wrist -> fingertip length along limb +X: adult 862, child 548 model units)
        // rests ON the face.
        float m[16];
        memcpy(m, mf16, sizeof(m));
        const float half = VrHand_ChildSized() ? 274.0f : 431.0f;
        float mid[3];
        for (int k = 0; k < 3; k++) {
            mid[k] = m[0 * 4 + k] * half + m[12 + k];
        }
        const float dist = sPlaneN[0] * mid[0] + sPlaneN[1] * mid[1] + sPlaneN[2] * mid[2] + sPlaneD;
        for (int k = 0; k < 3; k++) {
            m[12 + k] -= dist * sPlaneN[k];
        }
        // Into the block's frame: axes rotated by -yaw, the origin also offset by the block.
        for (int c = 0; c < 3; c++) {
            RotY((s16)-yaw, &m[c * 4], &local[c * 4]);
            local[c * 4 + 3] = m[c * 4 + 3];
        }
        const float rel[3] = { m[12] - bpos[0], m[13] - bpos[1], m[14] - bpos[2] };
        RotY((s16)-yaw, rel, &local[12]);
        local[15] = m[15];
        sPinValid[vrHand] = true;
    }
    for (int c = 0; c < 3; c++) {
        RotY(yaw, &local[c * 4], &mf16[c * 4]);
        mf16[c * 4 + 3] = local[c * 4 + 3];
    }
    float t[3];
    RotY(yaw, &local[12], t);
    mf16[12] = t[0] + bpos[0];
    mf16[13] = t[1] + bpos[1];
    mf16[14] = t[2] + bpos[2];
    mf16[15] = local[15];
    return true;
}

extern "C" void VrBlock_GetDebug(VrBlockDebug* out) {
    if (out != nullptr) {
        *out = sDebug;
    }
}
