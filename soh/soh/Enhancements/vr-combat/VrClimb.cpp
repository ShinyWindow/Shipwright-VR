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

// Physical climbing (VR first person, motion hands). User spec (October 4, 2026): ladders work
// "identical to what we do for climbing vines" (grab anywhere on the surface, no rungs); "when you
// reach the top, similarly to how half life alyx works, when you let go, it puts you on top" with
// "the same interaction as the base game and sound effect of climbing over a ledge"; "letting go,
// keep your momentum, like i should be able to throw myself up a little bit, but not insanely";
// physical climbing optional.
//
// The VR climbing convention (Climbey / Boneworks / Stride / The Climb): a squeezed hand takes hold
// of the world, the body moves opposite to it 1:1, the LAST hand to take hold drives (the other
// takes over, re-based, when it lets go), gravity is off while anything holds, letting go of
// everything keeps the body's motion.
//
// What is climbable is the game's own data: the SurfaceType wall flags of the polygon under the hand
// (0x02 ladder, 0x04 ladder top, 0x08 vines / fences / climbable rock) on a steep polygon — the same
// test the vanilla climb uses. The vanilla climb action stays the state machine (z_player.c,
// Player_Action_8084BF1C): this module only replaces the stick's step animations with the hand's
// motion while a hand holds. The wall glue, bg check, moving-wall carry, randomizer gate, top /
// bottom transitions and their sounds are vanilla's.
//
// Feel: the body moves at the game's 20 Hz, but the view is locked to the gripping hand at headset
// rate (VR_SetClimbViewLock): between ticks the camera follows the arm, so the hand stays where it
// took hold and there is no rubber band behind a pull (the camera bob of spring-bodied climbers is
// what makes VR climbing sickening; this one is rigid).

namespace {

constexpr float kInsideCm = 25.0f;      // a hand this far INSIDE the surface still holds it
constexpr float kMaxStepUnits = 25.0f;  // body move cap per tick: a tracking glitch never flings Link
constexpr float kJumpCm = 20.0f;        // the driving hand moving more than this in one tick is a glitch
constexpr float kRungUnits = 15.0f;     // vanilla's ladder rung pitch (func_8083EC18's grid)
constexpr float kMountSlackUnits = 30.0f; // Link's centre may be this much beyond his wall radius
constexpr float kNotMeasured = -999.0f;

bool sGripPrev[2] = { true, true }; // fresh-press tracking: a grip held from before counts as nothing
bool sLatched[2] = { false, false };
int sLatchOrder[2] = { 0, 0 };
int sLatchCounter = 0;
bool sOnSurface[2] = { false, false };
bool sReachArmed[2] = { true, true };
VrClimbHit sHit[2];
float sProbeSnap[2][3];     // this tick's probe: the move that puts the hand on the surface
float sSnap[2][3];          // taken at the grab: while the hand holds, it is drawn this far from the controller
bool sSnapValid[2] = { false, false };
int sPendingMount = -1;
bool sGateOk = false;
uint32_t sTickFrame = 0;

bool sDriving = false;
int sAnchor = -1;
float sRef[3] = { 0.0f, 0.0f, 0.0f };
bool sRefValid = false;
uint32_t sStepFrame = 0;
float sLastMove[3] = { 0.0f, 0.0f, 0.0f };
float sHist[2][3];
int sHistCount = 0;
float sTravel = 0.0f;
bool sBumpArmed = true;
bool sAirborneRelease = false;

VrClimbDebug sDebug = { 2, { 0, 0 }, { 0, 0 }, { kNotMeasured, kNotMeasured }, { 0, 0 }, -1, 0, 0, 0.0f, 0.0f, 0.0f,
                        -1, 0.0f, 0 };

float WorldScale() {
    const float ws = VR_GetWorldScale();
    return ws < 1.0f ? 35.0f : ws;
}

float CmToUnits(float cm) {
    return cm * 0.01f * WorldScale();
}

void Haptic(int hand, float amplitude, float durationMs) {
    const float k = CVarGetInteger("gVrClimbHaptics", 100) * 0.01f;
    if (k > 0.0f && hand >= 0 && hand <= 1) {
        VR_TriggerHaptic(hand, fminf(amplitude * k, 1.0f), 0.0f, durationMs);
    }
}

void HapticBoth(float amplitude, float durationMs) {
    Haptic(VR_HAND_LEFT, amplitude, durationMs);
    Haptic(VR_HAND_RIGHT, amplitude, durationMs);
}

// 0 armed; 1 off; 2 not VR first person with motion hands; 3 the game owns Link.
int Gate(PlayState* play, Player* player) {
    if (!CVarGetInteger("gVrPhysClimb", 1)) {
        return 1;
    }
    if (!VR_IsInitialized() || !VR_GetFirstPerson() || VR_IsFlatScreen() || !CVarGetInteger("gVrMotionHands", 1)) {
        return 2;
    }
    if (Player_InBlockingCsMode(play, player) || play->csCtx.state != CS_STATE_IDLE ||
        play->transitionTrigger != TRANS_TRIGGER_OFF || (player->stateFlags1 & PLAYER_STATE1_ON_HORSE)) {
        return 3;
    }
    return 0;
}

bool Climbable(PlayState* play, CollisionPoly* poly, s32 bgId, int32_t* flagsOut) {
    if (poly == nullptr || ABS(poly->normal.y) >= 600) {
        return false;
    }
    const s32 flags = func_80041DB8(&play->colCtx, poly, bgId);
    *flagsOut = flags;
    return (flags & (0x02 | 0x04 | 0x08)) != 0;
}

// Is this hand on (or just short of) a climbable surface? Measured the way you see it: the
// perpendicular distance from the surface's plane to the CLOSEST part of the drawn hand — wrist,
// middle or fingertips (limb +X, adult 862 / child 548 model units, through the hand matrix so the
// real-life hand scale counts) — so fingertips on the ladder texture count, from any angle. The
// surface is found with short segments through the middle of the hand: eight level ones around it
// (a wall in any direction, overhead reaches included) and one from the eye through it (a wall you
// reach toward at a slant). The nearest climbable hit wins. snap = the move that puts the hand's
// closest point on the surface (out of it when reached in, onto it when short): while the hand
// holds, it is drawn there.
bool Probe(PlayState* play, int hand, VrClimbHit* hit, float* distCm, int32_t* flags, float snap[3]) {
    float m[4][4];
    if (!VR_GetHandMatrix(hand, m)) {
        return false;
    }
    const float len = VrHand_ChildSized() ? 548.0f : 862.0f;
    Vec3f pts[3];
    for (int i = 0; i < 3; i++) {
        const float x = len * 0.5f * i;
        pts[i].x = m[3][0] + m[0][0] * x;
        pts[i].y = m[3][1] + m[0][1] * x;
        pts[i].z = m[3][2] + m[0][2] * x;
    }
    const Vec3f& mid = pts[1];
    const float halfUnits = sqrtf(m[0][0] * m[0][0] + m[0][1] * m[0][1] + m[0][2] * m[0][2]) * len * 0.5f;
    const float reach = CmToUnits(CVarGetFloat("gVrClimbGrabReach", 25.0f));
    const float inside = CmToUnits(kInsideCm);
    const float span = reach + halfUnits; // from the hand's middle, past its far end by the reach
    float eye[3], fwd[3], up[3];
    VR_GetCameraPose(eye, fwd, up);

    bool found = false;
    float best = 1e9f;
    *flags = 0;
    for (int pass = 0; pass < 9; pass++) {
        float dir[3];
        if (pass < 8) {
            const float ang = pass * (3.14159265f / 4.0f);
            dir[0] = sinf(ang);
            dir[1] = 0.0f;
            dir[2] = cosf(ang);
        } else {
            dir[0] = mid.x - eye[0];
            dir[1] = mid.y - eye[1];
            dir[2] = mid.z - eye[2];
            const float l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (l < 1.0f) {
                continue;
            }
            for (int k = 0; k < 3; k++) {
                dir[k] /= l;
            }
        }
        Vec3f a, b, res;
        a.x = mid.x - dir[0] * inside;
        a.y = mid.y - dir[1] * inside;
        a.z = mid.z - dir[2] * inside;
        b.x = mid.x + dir[0] * span;
        b.y = mid.y + dir[1] * span;
        b.z = mid.z + dir[2] * span;
        CollisionPoly* poly = nullptr;
        s32 bgId = BGCHECK_SCENE;
        if (!BgCheck_EntityLineTest1(&play->colCtx, &a, &b, &res, &poly, true, false, false, true, &bgId)) {
            continue;
        }
        int32_t f = 0;
        if (!Climbable(play, poly, bgId, &f)) {
            if (!found) {
                *flags = f;
            }
            continue;
        }
        const float n[3] = { COLPOLY_GET_NORMAL(poly->normal.x), COLPOLY_GET_NORMAL(poly->normal.y),
                             COLPOLY_GET_NORMAL(poly->normal.z) };
        float dmin = 1e9f;
        for (int i = 0; i < 3; i++) {
            dmin = fminf(dmin, n[0] * pts[i].x + n[1] * pts[i].y + n[2] * pts[i].z + poly->dist);
        }
        if (dmin > reach || dmin < -inside || fabsf(dmin) >= best) {
            continue;
        }
        best = fabsf(dmin);
        found = true;
        hit->poly = poly;
        hit->bgId = bgId;
        hit->pos[0] = res.x;
        hit->pos[1] = res.y;
        hit->pos[2] = res.z;
        hit->hand = hand;
        *distCm = dmin / WorldScale() * 100.0f;
        *flags = f;
        for (int k = 0; k < 3; k++) {
            snap[k] = -n[k] * dmin;
        }
    }
    return found;
}

// Vanilla's own entry conditions that don't depend on the stick (func_8083EC18): out of the water,
// or wading shallow enough, or walking the bottom in iron boots; never with something in his arms.
bool CanMount(PlayState* play, Player* player, const VrClimbHit& hit) {
    if (player->stateFlags1 & (PLAYER_STATE1_CARRYING_ACTOR | PLAYER_STATE1_CLIMBING_LADDER)) {
        return false;
    }
    if ((player->stateFlags1 & PLAYER_STATE1_IN_WATER) && (player->currentBoots != PLAYER_BOOTS_IRON) &&
        !(player->actor.yDistToWater < player->ageProperties->unk_2C)) {
        return false;
    }
    // Link is pulled square onto the wall when he takes hold: only from about where he stands.
    const CollisionPoly* poly = (const CollisionPoly*)hit.poly;
    const float nx = COLPOLY_GET_NORMAL(poly->normal.x), ny = COLPOLY_GET_NORMAL(poly->normal.y),
                nz = COLPOLY_GET_NORMAL(poly->normal.z);
    const float dist = nx * player->actor.world.pos.x + ny * player->actor.world.pos.y +
                       nz * player->actor.world.pos.z + poly->dist;
    return dist > -5.0f && dist < player->ageProperties->wallCheckRadius + kMountSlackUnits;
}

void EndDrive() {
    if (sDriving || sAnchor >= 0) {
        VR_SetClimbViewLock(-1, nullptr, nullptr, 0);
    }
    sDriving = false;
    sAnchor = -1;
    sRefValid = false;
    sLastMove[0] = sLastMove[1] = sLastMove[2] = 0.0f;
}

void ClearAll() {
    EndDrive();
    sLatched[0] = sLatched[1] = false;
    sSnapValid[0] = sSnapValid[1] = false;
    sOnSurface[0] = sOnSurface[1] = false;
    sReachArmed[0] = sReachArmed[1] = true;
    sPendingMount = -1;
    sHistCount = 0;
    sTravel = 0.0f;
    sAirborneRelease = false;
}

int LatestLatched() {
    int best = -1;
    for (int h = 0; h < 2; h++) {
        if (sLatched[h] && (best < 0 || sLatchOrder[h] > sLatchOrder[best])) {
            best = h;
        }
    }
    return best;
}

} // namespace

extern "C" void VrClimb_Tick(PlayState* play, Player* player) {
    if (play == nullptr || player == nullptr || player != GET_PLAYER(play)) {
        return; // the co-op partner has no hands
    }
    sTickFrame = play->state.frames;

    bool grip[2], fresh[2];
    for (int h = 0; h < 2; h++) {
        grip[h] = (VR_GetControllerButton(h) & VR_BTN_GRIP) != 0;
        fresh[h] = grip[h] && !sGripPrev[h];
        sGripPrev[h] = grip[h];
    }

    const int gate = Gate(play, player);
    sDebug.gate = gate;
    if (gate != 0) {
        sGateOk = false;
        ClearAll();
        sDebug.onSurface[0] = sDebug.onSurface[1] = 0;
        sDebug.latched[0] = sDebug.latched[1] = 0;
        sDebug.surfCm[0] = sDebug.surfCm[1] = kNotMeasured;
        sDebug.anchor = -1;
        sDebug.driving = 0;
        sDebug.climbing = 0;
        return;
    }
    sGateOk = true;

    const bool climbing = (player->stateFlags1 & PLAYER_STATE1_CLIMBING_LADDER) != 0;
    if (!climbing) {
        // Out of the climb (climbed over, stepped off, dropped, knocked off): nothing holds, and a
        // mount offered last tick that no seam took is gone with its latch.
        EndDrive();
        sLatched[0] = sLatched[1] = false;
        sSnapValid[0] = sSnapValid[1] = false;
    }
    sPendingMount = -1;
    if (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) {
        sAirborneRelease = false;
    }

    for (int h = 0; h < 2; h++) {
        VrClimbHit hit;
        float distCm = kNotMeasured;
        int32_t flags = 0;
        // A holding hand needs no probe (it is pinned where it took hold).
        const bool on = !sLatched[h] && Probe(play, h, &hit, &distCm, &flags, sProbeSnap[h]);
        sOnSurface[h] = on;
        sDebug.surfCm[h] = on ? distCm : kNotMeasured;
        sDebug.surfFlags[h] = flags;
        if (on) {
            sHit[h] = hit;
            if (sReachArmed[h] && !sLatched[h]) {
                Haptic(h, 0.15f, 12.0f);
                sReachArmed[h] = false;
            }
        } else {
            sReachArmed[h] = true;
        }

        if (!grip[h]) {
            sLatched[h] = false;
        } else if (fresh[h] && on && !sLatched[h]) {
            if (climbing) {
                sLatched[h] = true;
                sLatchOrder[h] = ++sLatchCounter;
                Haptic(h, 0.4f, 25.0f);
            } else if (sPendingMount < 0 && CanMount(play, player, hit)) {
                sLatched[h] = true;
                sLatchOrder[h] = ++sLatchCounter;
                sPendingMount = h;
            }
            if (sLatched[h]) {
                for (int k = 0; k < 3; k++) {
                    sSnap[h][k] = sProbeSnap[h][k];
                }
                sSnapValid[h] = true;
            }
        }
        if (!sLatched[h]) {
            sSnapValid[h] = false;
        }
    }

    if (!sDriving) {
        VR_SetClimbViewLock(-1, nullptr, nullptr, 0);
    }

    sDebug.climbing = climbing ? 1 : 0;
    for (int h = 0; h < 2; h++) {
        sDebug.onSurface[h] = sOnSurface[h] ? 1 : 0;
        sDebug.latched[h] = sLatched[h] ? 1 : 0;
    }
    sDebug.driving = sDriving ? 1 : 0;
    sDebug.anchor = sAnchor;
}

extern "C" bool VrClimb_TakeMount(PlayState* play, Player* player, VrClimbHit* out) {
    if (!sGateOk || sPendingMount < 0 || out == nullptr || play == nullptr || player != GET_PLAYER(play) ||
        (uint32_t)play->state.frames != sTickFrame) {
        return false;
    }
    *out = sHit[sPendingMount];
    Haptic(sPendingMount, 0.5f, 30.0f);
    sPendingMount = -1;
    sAirborneRelease = false;
    sDebug.mounts++;
    return true;
}

// The anchor hand's tracked motion since the last tick, reversed: the body's move (full 3D — the
// part along the wall's normal is the distance from the wall, the rest slides along it; the climb
// code in z_player.c splits them). Capped per tick so a tracking glitch never flings Link.
static bool TakeDelta(int hand, float outMove[3]) {
    float t[3];
    if (!VR_GetHandTracked(hand, t)) {
        // Not actually tracked (IMU-only drift, occluded overhead): hold still, and measure afresh
        // from wherever the hand is once tracking returns instead of jumping the body there.
        sRefValid = false;
        return false;
    }
    float d[3] = { t[0] - sRef[0], t[1] - sRef[1], t[2] - sRef[2] };
    const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len > CmToUnits(kJumpCm)) {
        // A glitch (tracking reacquired far away, a bad sample): re-base, don't move.
        for (int i = 0; i < 3; i++) {
            sRef[i] = t[i];
        }
        return false;
    }
    const float k = len > kMaxStepUnits ? kMaxStepUnits / len : 1.0f;
    for (int i = 0; i < 3; i++) {
        outMove[i] = -d[i] * k;
        sRef[i] = t[i];
    }
    return true;
}

extern "C" int32_t VrClimb_Step(PlayState* play, Player* player, float outMove[3]) {
    outMove[0] = outMove[1] = outMove[2] = 0.0f;
    if (!sGateOk || play == nullptr || player != GET_PLAYER(play)) {
        EndDrive();
        return 0;
    }
    const uint32_t frames = play->state.frames;
    const int anchor = LatestLatched();
    if (anchor < 0) {
        if (sDriving) {
            // The last hand let go this tick: its motion up to now still moves the body (otherwise
            // the view, locked to it until now, would hop back by a tick of pull), then resolve.
            if (sRefValid && frames - sStepFrame <= 1) {
                TakeDelta(sAnchor, outMove);
            }
            for (int i = 0; i < 3; i++) {
                sLastMove[i] = outMove[i];
            }
            EndDrive();
            sDebug.driving = 0;
            sDebug.anchor = -1;
            return 2;
        }
        return 0;
    }

    // The view held still meanwhile (the hand lost tracking or jumped, or the system recentered):
    // re-base instead of moving the body by it.
    if (VR_ConsumeClimbDiscontinuity()) {
        sRefValid = false;
    }
    // Take hold (first tick), a new hand taking over, or back after a gap (pause): re-base, no move.
    if (!sDriving || anchor != sAnchor || !sRefValid || frames - sStepFrame > 1) {
        if (!sDriving) {
            sHistCount = 0;
            sTravel = 0.0f;
            sBumpArmed = true;
        }
        sDriving = true;
        sAnchor = anchor;
        sRefValid = VR_GetHandTracked(anchor, sRef);
    } else {
        TakeDelta(anchor, outMove);
    }
    sStepFrame = frames;
    for (int i = 0; i < 3; i++) {
        sLastMove[i] = outMove[i];
    }
    if (sRefValid) {
        VR_SetClimbViewLock(anchor, sRef, nullptr, 1);
    }
    sDebug.moveCm =
        sqrtf(outMove[0] * outMove[0] + outMove[1] * outMove[1] + outMove[2] * outMove[2]) / WorldScale() * 100.0f;
    sDebug.driving = 1;
    sDebug.anchor = anchor;
    return 1;
}

extern "C" bool VrClimb_Moved(Player* player, const float achieved[3], float wallDistUnits) {
    sDebug.wallDistCm = wallDistUnits / WorldScale() * 100.0f;
    if (sHistCount == 2) {
        for (int i = 0; i < 3; i++) {
            sHist[0][i] = sHist[1][i];
        }
        sHistCount = 1;
    }
    for (int i = 0; i < 3; i++) {
        sHist[sHistCount][i] = achieved[i];
    }
    sHistCount++;

    const float len = sqrtf(achieved[0] * achieved[0] + achieved[1] * achieved[1] + achieved[2] * achieved[2]);
    const float want = sqrtf(sLastMove[0] * sLastMove[0] + sLastMove[1] * sLastMove[1] + sLastMove[2] * sLastMove[2]);
    sDebug.achievedCm = len / WorldScale() * 100.0f;
    // Pulling against the end of the climb (top, floor, the edge of the vines, a ceiling): one bump.
    if (want - len > 1.0f) {
        if (sBumpArmed) {
            Haptic(sAnchor, 0.25f, 20.0f);
            sBumpArmed = false;
        }
    } else if (want > 0.5f) {
        sBumpArmed = true;
    }
    sTravel += len;
    if (sTravel >= kRungUnits) {
        sTravel = fmodf(sTravel, kRungUnits);
        Haptic(sAnchor, 0.12f, 10.0f);
        return true;
    }
    return false;
}

extern "C" void VrClimb_Launch(Player* player, float outVel[3]) {
    outVel[0] = outVel[1] = outVel[2] = 0.0f;
    if (sHistCount > 0) {
        for (int h = 0; h < sHistCount; h++) {
            for (int i = 0; i < 3; i++) {
                outVel[i] += sHist[h][i] / sHistCount;
            }
        }
    }
    const float k = CVarGetInteger("gVrClimbMomentum", 70) * 0.01f;
    for (int i = 0; i < 3; i++) {
        outVel[i] *= k;
    }
    // "Not insanely": the toss can lift Link at most gVrClimbTossCm (real centimetres, so the same
    // for child and adult), and nothing sideways is faster than that.
    float g = player != nullptr ? -player->actor.gravity : 1.0f;
    if (g < 0.1f) {
        g = 1.0f;
    }
    const float vmax = sqrtf(2.0f * g * CmToUnits(CVarGetFloat("gVrClimbTossCm", 35.0f)));
    outVel[1] = fminf(outVel[1], vmax);
    outVel[1] = fmaxf(outVel[1], -15.0f);
    const float vh = sqrtf(outVel[0] * outVel[0] + outVel[2] * outVel[2]);
    if (vh > vmax && vh > 0.0f) {
        outVel[0] *= vmax / vh;
        outVel[2] *= vmax / vh;
    }
    sDebug.lastTossCm = outVel[1] > 0.0f ? (outVel[1] * outVel[1] / (2.0f * g)) / WorldScale() * 100.0f : 0.0f;
}

extern "C" void VrClimb_NoteResolved(Player* player, int32_t how) {
    sDebug.lastRelease = how;
    sHistCount = 0;
    sTravel = 0.0f;
    sAirborneRelease = (how == 0);
    if (how == 1) {
        HapticBoth(0.5f, 40.0f);
    } else {
        HapticBoth(0.15f, 12.0f);
    }
}

extern "C" bool VrClimb_HandSnapOffset(Player* player, int32_t hand, float out[3]) {
    if (hand < 0 || hand > 1 || !sGateOk || !sLatched[hand] || !sSnapValid[hand] || gPlayState == nullptr ||
        player != GET_PLAYER(gPlayState) || !CVarGetInteger("gVrClimbSnapHands", 1)) {
        return false;
    }
    for (int k = 0; k < 3; k++) {
        out[k] = sSnap[hand][k];
    }
    return true;
}

extern "C" bool VrClimb_Active(Player* player) {
    return sGateOk && gPlayState != nullptr && player == GET_PLAYER(gPlayState);
}

extern "C" bool VrClimb_SuppressVanillaGrab(Player* player) {
    return sGateOk && sAirborneRelease && player != nullptr && !(player->actor.bgCheckFlags & BGCHECKFLAG_GROUND);
}

extern "C" bool VrClimb_BlockWalkInMount(Player* player) {
    return sGateOk && gPlayState != nullptr && player == GET_PLAYER(gPlayState) && !CVarGetInteger("gVrClimbWalkIn", 0);
}

extern "C" float VrClimb_TopWindowUnits(void) {
    return CmToUnits(CVarGetFloat("gVrClimbTopWindowCm", 30.0f));
}

extern "C" bool VrClimb_GripConsumed(int32_t hand, uint16_t mask) {
    if (!(mask & VR_BTN_GRIP) || hand < 0 || hand > 1 || !sGateOk || gPlayState == nullptr || VrOcarina_InPlay()) {
        return false;
    }
    // Stale while the player isn't updating (pause, transitions): never hold a binding then.
    if ((uint32_t)gPlayState->state.frames - sTickFrame > 2) {
        return false;
    }
    return sLatched[hand] || sOnSurface[hand];
}

extern "C" bool VrClimb_FrameSync(void) {
    const bool live = sGateOk && sDriving && gPlayState != nullptr &&
                      (uint32_t)gPlayState->state.frames - sTickFrame <= 1;
    if (!live) {
        VR_SetClimbViewLock(-1, nullptr, nullptr, 0);
        if (sDriving) {
            sRefValid = false; // re-base when the climb resumes: the hand moved meanwhile
        }
    }
    return live;
}

extern "C" void VrClimb_GetDebug(VrClimbDebug* out) {
    if (out != nullptr) {
        *out = sDebug;
    }
}
