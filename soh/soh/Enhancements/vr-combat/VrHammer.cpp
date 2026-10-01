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

// Physical Megaton Hammer — the grip half (VR first person, physical combat). User spec (September
// 30, 2026): "it should be a two handed item that feels heavy", one-handed "heavy but usable", the
// off hand joins with a squeeze on the handle, and wall hits keep the vanilla thud without the
// recoil shove.
//
// The hammer itself rides the melee feed (VrSwing.cpp, FeedHammer): the head is a held-object sim
// with a torque-limited orientation spring, so it trails the hands and carries its momentum past
// where they stop. This file owns the SECOND hand:
//
// ONE HAND  The lead hand (Link's L_HAND, the sword hand) holds the handle. The off hand is free;
//           within gVrHammerGripReach of the handle it gets a reach tick, and its grip loses the R
//           binding there so a squeeze takes the handle instead of doing anything else.
// TWO HANDS A FRESH grip press within reach takes hold at the nearest point on the handle (kept a
//           minimum span from the lead fist, or the aim between the hands degenerates). The next
//           feed pushes that point to the sim as the secondary grip: the head is re-aimed along the
//           line between the hands with the firm two-hand spring, and Link's off hand is drawn
//           pinned to the handle. Only letting go of the grip returns to one hand — however
//           fast the swing, however far the lagging head puts the pin from the real hand.
//
// The quick-swap chord (both grips = draw the sword) never counts a grip this module owns: taking
// the handle while the lead hand happens to squeeze must not stow the hammer (VrItemSelect.cpp).

namespace {

VrCombat::HammerGrip sGrip = { false, -1, { 0.0f, 0.0f, 0.0f } };
bool sHaveHandle = false;
float sButtLocal[3] = { 0.0f, 0.0f, 0.0f }; // grip-local handle ends (world units), from the last feed
float sNeckLocal[3] = { 0.0f, 0.0f, 0.0f };
bool sGripPrev[2] = { true, true }; // fresh-press tracking: a grip held from before counts as nothing
bool sInReach = false;
bool sReachTickArmed = true;
VrHammerDebug sDebug = { 2, 0, 0, -1.0f, 10.0f, 0.0f, 0, 0.0f, -1, 0, 2.5f, 3.0f };

int LeadHand() {
    return CVarGetInteger("gVrLeftHanded", 0) ? VR_HAND_LEFT : VR_HAND_RIGHT;
}

float WorldScale() {
    const float ws = VR_GetWorldScale();
    return ws < 1.0f ? 35.0f : ws;
}

void QRot(const float q[4], const float v[3], float out[3]) {
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

float Dist(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Parameter (0..1) of the point on segment a..b closest to p.
float SegParam(const float p[3], const float a[3], const float b[3]) {
    const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    if (len2 < 1e-6f) {
        return 0.0f;
    }
    const float t = ((p[0] - a[0]) * ab[0] + (p[1] - a[1]) * ab[1] + (p[2] - a[2]) * ab[2]) / len2;
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

void Lerp(const float a[3], const float b[3], float t, float out[3]) {
    for (int i = 0; i < 3; i++) {
        out[i] = a[i] + (b[i] - a[i]) * t;
    }
}

bool HammerHeld(Player* player) {
    return player != nullptr && CVarGetInteger("gVrPhysHammer", 1) && VrCombat_Active() &&
           VrCombat_MeleeCovered(player) && Player_GetMeleeWeaponHeld(player) == 5;
}

void LetGo(int offHand) {
    if (sGrip.twoHand && offHand >= 0) {
        VR_TriggerHaptic(offHand, 0.25f, 0.0f, 20.0f);
    }
    sGrip.twoHand = false;
}

} // namespace

namespace VrCombat {

const HammerGrip& Hammer_GetGrip() {
    return sGrip;
}

void Hammer_SetHandleLocal(const float buttLocal[3], const float neckLocal[3]) {
    memcpy(sButtLocal, buttLocal, sizeof(sButtLocal));
    memcpy(sNeckLocal, neckLocal, sizeof(sNeckLocal));
    sHaveHandle = true;
}

void Hammer_Reset() {
    sGrip.twoHand = false;
    sGrip.offHand = -1;
    sHaveHandle = false;
    sInReach = false;
    sReachTickArmed = true;
    sGripPrev[0] = sGripPrev[1] = true;
    sDebug.twoHand = 0;
    sDebug.offInReach = 0;
    sDebug.offHandCm = -1.0f;
    sDebug.headMps = 0.0f;
    sDebug.tier = 0;
}

void Hammer_NoteSwing(float headMps, int tier) {
    sDebug.headMps = headMps;
    sDebug.tier = tier;
}

void Hammer_NoteImpact(float approachMps, int kind, int strike) {
    sDebug.lastImpactMps = approachMps;
    sDebug.lastImpact = kind;
    sDebug.lastStrike = strike;
}

// Once per game tick while physical combat is active (Swing_OnPlayerUpdate), BEFORE the tick's
// hand paths are drained — so the raw off-hand sample read below is the latest of last tick.
void Hammer_Tick(PlayState* play, Player* player) {
    (void)play;
    const float reachCm = CVarGetFloat("gVrHammerGripReach", 10.0f);
    sDebug.reachCm = reachCm;
    sDebug.poundSpeed = CVarGetFloat("gVrHammerPoundSpeed", 2.5f);
    sDebug.hitSpeed = CVarGetFloat("gVrHammerHitSpeed", 3.0f);
    if (!CVarGetInteger("gVrPhysHammer", 1)) {
        sDebug.gate = 1;
        Hammer_Reset();
        return;
    }
    if (!HammerHeld(player)) {
        sDebug.gate = 2;
        Hammer_Reset();
        return;
    }
    sDebug.gate = 0;

    const int lead = LeadHand();
    const int off = lead ^ 1;
    const bool gripNow = (VR_GetControllerButton(off) & VR_BTN_GRIP) != 0;
    const bool fresh = gripNow && !sGripPrev[off];
    sGripPrev[off] = gripNow;

    // The handle in the world, from the SERVED lead-hand pose (the simulated grip — the frame the
    // feed extracted the handle geometry in).
    float lp[3], lq[4];
    if (!sHaveHandle || !VR_GetHandPose(lead, lp, lq)) {
        LetGo(off);
        sInReach = false;
        sDebug.twoHand = 0;
        sDebug.offInReach = 0;
        sDebug.offHandCm = -1.0f;
        return;
    }
    float butt[3], neck[3], rot[3];
    QRot(lq, sButtLocal, rot);
    butt[0] = lp[0] + rot[0];
    butt[1] = lp[1] + rot[1];
    butt[2] = lp[2] + rot[2];
    QRot(lq, sNeckLocal, rot);
    neck[0] = lp[0] + rot[0];
    neck[1] = lp[1] + rot[1];
    neck[2] = lp[2] + rot[2];

    // The off hand's REAL position. While two-handed its served pose is pinned to the handle, so
    // the raw tracked path is the only honest source (latest sample of last tick).
    float op[3], oq[4];
    const TickPath& path = GetTickPath(off);
    if (sGrip.twoHand && path.count > 0) {
        memcpy(op, path.samples[path.count - 1].pos, sizeof(op));
    } else if (!VR_GetHandPose(off, op, oq)) {
        LetGo(off);
        sInReach = false;
        sDebug.twoHand = 0;
        sDebug.offInReach = 0;
        sDebug.offHandCm = -1.0f;
        return;
    }

    const float ws = WorldScale();
    const float t = SegParam(op, butt, neck);
    float nearest[3];
    Lerp(butt, neck, t, nearest);
    const float distCm = Dist(op, nearest) / ws * 100.0f;
    sDebug.offHandCm = distCm;
    // With the ShieldTwoHanded cheat the shield rides the off hand: it can't also take the handle.
    const bool offBusy = VrCombat_ShieldHeld(player);

    if (sGrip.twoHand) {
        // Only opening the hand lets go. (A pull-off distance rule was tried first and dropped
        // the handle mid-swing: the heavy head deliberately lags the hands, so on any fast swing
        // the real off hand runs well ahead of its pinned point on the simulated handle.)
        if (!gripNow || offBusy) {
            LetGo(off);
            sReachTickArmed = false; // the hand is still at the handle: no re-entry tick
        }
        sInReach = false;
    } else {
        sInReach = !offBusy && distCm <= reachCm;
        if (sInReach && sReachTickArmed) {
            VR_TriggerHaptic(off, 0.15f, 0.0f, 12.0f);
            sReachTickArmed = false;
        } else if (!sInReach && distCm > reachCm * 1.3f) {
            sReachTickArmed = true;
        }
        if (fresh && sInReach) {
            // Take hold at the nearest point on the handle, kept a minimum span from the lead
            // grip (the lead grip's own place along the handle is its projection): closer than
            // that the line between the hands gives the sim no stable aim.
            const float len = Dist(sButtLocal, sNeckLocal);
            const float zero[3] = { 0.0f, 0.0f, 0.0f };
            const float tLead = SegParam(zero, sButtLocal, sNeckLocal);
            const float minSpan = CVarGetFloat("gVrHammerGripMinSpan", 12.0f) * 0.01f * ws;
            float tGrab = t;
            if (len > 1e-3f && fabsf(tGrab - tLead) * len < minSpan) {
                const float dir = (tGrab >= tLead) ? 1.0f : -1.0f;
                tGrab = tLead + dir * minSpan / len;
                if (tGrab < 0.0f || tGrab > 1.0f) {
                    // No room on that side of the fist: take the other side, where there is more.
                    tGrab = tLead - dir * minSpan / len;
                }
                tGrab = tGrab < 0.0f ? 0.0f : (tGrab > 1.0f ? 1.0f : tGrab);
            }
            Lerp(sButtLocal, sNeckLocal, tGrab, sGrip.secondaryLocal);
            sGrip.twoHand = true;
            sGrip.offHand = off;
            sInReach = false;
            VR_TriggerHaptic(off, 0.5f, 0.0f, 35.0f);
            VR_TriggerHaptic(lead, 0.3f, 0.0f, 25.0f);
        }
    }
    sDebug.twoHand = sGrip.twoHand ? 1 : 0;
    sDebug.offInReach = sInReach ? 1 : 0;
}

} // namespace VrCombat

extern "C" bool VrHammer_GripConsumed(int32_t hand, uint16_t mask) {
    if (!(mask & VR_BTN_GRIP) || gPlayState == nullptr || hand != (LeadHand() ^ 1)) {
        return false;
    }
    if (!HammerHeld(GET_PLAYER(gPlayState))) {
        return false;
    }
    return sGrip.twoHand || sInReach;
}

extern "C" void VrHammer_GetDebug(VrHammerDebug* out) {
    if (out != nullptr) {
        *out = sDebug;
    }
}
