extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
extern PlayState* gPlayState;
}
#include "VrCombat.h"
#include "VrPocket.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <vr_interface.h>
#include <cmath>
#include <cstring>

// Physical carrying (VR first person, selector mode). User spec (October 4, 2026): pots, small rocks,
// bushes, crates, bomb flowers, bombs lying on the ground and cuccos are picked up by grabbing them —
// instantly, "almost parented by my hand" — held in one hand or both, and thrown with real throwing
// physics, like the bombs. Everything else is the base game: the carry offer (range, facing, strength,
// the object's own "can I be lifted now" rules), the carrying state, the object's reaction to being
// lifted (the bush reveals what's under it, the bomb flower lights, the cucco lets you glide) and its
// flight once thrown. Only three things change:
//   PICK UP  a fresh grip with the hand on an object that offered itself to be carried = the lift,
//            done on the spot (Player_VrCarryPickUp: the attach frame of the vanilla lift, no
//            animation). Whatever was in Link's hands is put away first, as vanilla does.
//   HOLD     the object keeps the pose it had relative to the grabbing hand (VrPocket::Grab). The
//            other hand can join with a fresh grip on it: then it rides both hands (the average of
//            the two rigid poses). Letting go with one hand hands it to the other without a jump.
//            At draw time it is welded to the live hand (Actor_Draw -> gVrMtxWeldHand), so it moves at
//            headset rate rather than the 20 Hz game tick.
//   THROW    opening the last hand releases it with the hand's velocity at that moment (plus Link's own
//            ground velocity), matched to the object's gravity so it lands where the same throw lands
//            in real life, and capped (Player_VrCarryRelease: velocity, then the vanilla post-throw
//            detach).
// HEAVY: the silver boulder (gVrPhysCarryHeavy). A fresh grip with a hand on it LATCHES that hand
// (pinned to the rock, drawn there); it lifts only when both hands are latched — then the vanilla
// boulder action takes over (Link rooted, Player_VrCarryPickUpHeavy), the rock trails the two hands
// with a heavy lag, and opening either grip throws it (heavily: gravity-matched like the light ones,
// own scale and cap). The gauntlet pillar is VrBlock's (a face, like the push blocks).

namespace {

struct Liftable {
    s16 id;
    float radius, height, yShift; // the actor's own collider cylinder (world units)
};

// The light liftables: every actor that takes the vanilla normal lift. Sizes are their colliders.
constexpr Liftable kLiftables[] = {
    { ACTOR_OBJ_TSUBO, 9.0f, 26.0f, 0.0f },  { ACTOR_EN_ISHI, 10.0f, 18.0f, -2.0f },
    { ACTOR_EN_KUSA, 12.0f, 44.0f, 0.0f },   { ACTOR_OBJ_KIBAKO, 12.0f, 27.0f, 0.0f },
    { ACTOR_EN_BOMBF, 9.0f, 18.0f, 10.0f },  { ACTOR_EN_BOM, 6.0f, 11.0f, 14.0f },
    { ACTOR_EN_NIW, 15.0f, 25.0f, 4.0f },
};

// The silver boulder (En_Ishi, params & 1): the vanilla heavy lift. Its collider cylinder.
constexpr Liftable kBoulder = { ACTOR_EN_ISHI, 55.0f, 70.0f, 0.0f };

constexpr int kMaxOffers = 16;
struct Offer {
    Actor* actor;
    u32 frame;
};
Offer sOffers[kMaxOffers];
int sOfferCount = 0;

struct HandHold {
    bool holding = false;
    VrPocket::Grab grab;
};

bool sGripPrev[2] = { true, true };
Actor* sHeld = nullptr;
HandHold sHands[2];
int sPrimary = -1;      // the hand the draw welds to
int sReleasedAgo[2] = { 99, 99 }; // ticks since each hand let go (two-hand throws)
VrCarryDebug sDebug = { 2, 0, -1, 0, 0.0f, -1, -1.0f, 0, 0 };

// Heavy (the boulder).
bool sHeavy = false;            // sHeld is the boulder: two hands, lagging pose, Link rooted
Actor* sLatchActor = nullptr;   // the boulder the hands are latched on (before and while lifted)
bool sLatched[2] = { false, false };
bool sTooHeavyBuzzed = false;
float sHeavyPos[3];
float sHeavyAx[3][3];           // the lagging pose: position + model X / Y / Z
bool sPinValid[2] = { false, false };
float sPinLocal[2][16];         // a latched hand's matrix in the boulder's frame (MtxF layout)

float WorldScale() {
    return VrPocket::WorldScale();
}

int SwordHand() {
    return CVarGetInteger("gVrLeftHanded", 0) ? VR_HAND_LEFT : VR_HAND_RIGHT;
}

const Liftable* FindLiftable(Actor* actor) {
    if (actor == nullptr) {
        return nullptr;
    }
    // The silver boulder is a rock too, but it is heavy.
    if (actor->id == ACTOR_EN_ISHI && (actor->params & 1) == 1) {
        return nullptr;
    }
    for (const Liftable& l : kLiftables) {
        if (l.id == actor->id) {
            return &l;
        }
    }
    return nullptr;
}

const Liftable* FindHeavy(Actor* actor) {
    return (actor != nullptr && actor->id == ACTOR_EN_ISHI && (actor->params & 1) == 1) ? &kBoulder : nullptr;
}

bool Active(Player* player) {
    return player != nullptr && gPlayState != nullptr && player == GET_PLAYER(gPlayState) &&
           CVarGetInteger("gVrPhysCarry", 1) && VrItemSelect_ModeActive() && CVarGetInteger("gVrMotionHands", 1) &&
           gPlayState->bombchuBowlingStatus == 0 && gPlayState->shootingGalleryStatus == 0 &&
           !(player->stateFlags1 & (PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_IN_WATER));
}

// shape.rot (YXZ, as Actor_Draw composes it) -> the model's X / Y / Z axes in world.
void AxesFromRot(const Vec3s& rot, float x[3], float y[3], float z[3]) {
    const float sx = Math_SinS(rot.x), cx = Math_CosS(rot.x);
    const float sy = Math_SinS(rot.y), cy = Math_CosS(rot.y);
    const float sz = Math_SinS(rot.z), cz = Math_CosS(rot.z);
    // R = Ry * Rx * Rz; columns are R applied to the unit axes.
    auto apply = [&](float a, float b, float c, float out[3]) {
        // Rx
        const float b1 = b * cx - c * sx, c1 = b * sx + c * cx;
        // Ry
        out[0] = a * cy + c1 * sy;
        out[1] = b1;
        out[2] = -a * sy + c1 * cy;
    };
    apply(cz, sz, 0.0f, x);   // Rz * (1,0,0)
    apply(-sz, cz, 0.0f, y);  // Rz * (0,1,0)
    apply(0.0f, 0.0f, 1.0f, z);
}

void Normalize(float v[3]) {
    const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

void Cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

void OrthoAxes(float x[3], float y[3], float z[3]) {
    Normalize(z);
    Cross(y, z, x);
    Normalize(x);
    Cross(z, x, y);
}

// The point an actor's model actually turns about. Actor_Draw translates to world.pos + (0, yOffset x
// scale.y, 0) in WORLD space and only then rotates by shape.rot, so the model pivots about that point,
// not about world.pos. Rigid poses are kept about the pivot; world.pos is derived back from it.
float PivotLift(const Actor* a) {
    return a->shape.yOffset * a->scale.y;
}

void PivotOf(const Actor* a, float out[3]) {
    out[0] = a->world.pos.x;
    out[1] = a->world.pos.y + PivotLift(a);
    out[2] = a->world.pos.z;
}

// Quaternions (x, y, z, w), the controller convention of VrPocket.
void QMul(const float a[4], const float b[4], float out[4]) {
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

void QNormalize(float q[4]) {
    const float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l > 1e-6f) {
        for (int k = 0; k < 4; k++) {
            q[k] /= l;
        }
    } else {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
    }
}

// The shortest rotation taking unit vector a onto unit vector b.
void QFromTo(const float a[3], const float b[3], float out[4]) {
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (d < -0.9999f) {
        // Opposite: half a turn about any axis perpendicular to a.
        float axis[3];
        const float ref[3] = { fabsf(a[0]) < 0.9f ? 1.0f : 0.0f, fabsf(a[0]) < 0.9f ? 0.0f : 1.0f, 0.0f };
        Cross(a, ref, axis);
        Normalize(axis);
        out[0] = axis[0];
        out[1] = axis[1];
        out[2] = axis[2];
        out[3] = 0.0f;
        return;
    }
    float c[3];
    Cross(a, b, c);
    out[0] = c[0];
    out[1] = c[1];
    out[2] = c[2];
    out[3] = 1.0f + d;
    QNormalize(out);
}

// Two hands on one object: its reference pose and both hands at the moment the second hand took hold.
// While both hold, the object follows where the HANDS are — the point between them and the line joining
// them — turning about that midpoint, never swinging about either wrist. (Averaging two one-hand rigid
// poses did that: a big rock's middle is far from each hand, so the small wrist pitch everyone makes while
// lifting levered it far higher than the hands went.) The roll about the line between the hands is the
// average of the two hands' own turns.
struct TwoHandRef {
    bool valid = false;
    float mid0[3];
    float axis0[3];
    float q0[2][4];
    float pivot0[3];
    float ax0[3][3];
};
TwoHandRef sTwo;

void CaptureTwo() {
    sTwo.valid = false;
    if (sHeld == nullptr || !sHands[0].holding || !sHands[1].holding) {
        return;
    }
    float p[2][3];
    for (int h = 0; h < 2; h++) {
        if (!VR_GetHandPose(h, p[h], sTwo.q0[h])) {
            return;
        }
    }
    for (int k = 0; k < 3; k++) {
        sTwo.mid0[k] = (p[0][k] + p[1][k]) * 0.5f;
        sTwo.axis0[k] = p[1][k] - p[0][k];
    }
    if (sTwo.axis0[0] * sTwo.axis0[0] + sTwo.axis0[1] * sTwo.axis0[1] + sTwo.axis0[2] * sTwo.axis0[2] < 1e-4f) {
        return; // hands on top of each other: no line to follow (the one-hand poses are used)
    }
    Normalize(sTwo.axis0);
    PivotOf(sHeld, sTwo.pivot0);
    AxesFromRot(sHeld->shape.rot, sTwo.ax0[0], sTwo.ax0[1], sTwo.ax0[2]);
    sTwo.valid = true;
}

bool TwoHandPose(float pos[3], float x[3], float y[3], float z[3]) {
    float p[2][3], q[2][4];
    for (int h = 0; h < 2; h++) {
        if (!VR_GetHandPose(h, p[h], q[h])) {
            return false;
        }
    }
    float mid[3], axis[3];
    for (int k = 0; k < 3; k++) {
        mid[k] = (p[0][k] + p[1][k]) * 0.5f;
        axis[k] = p[1][k] - p[0][k];
    }
    if (axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2] < 1e-4f) {
        return false;
    }
    Normalize(axis);
    // Each hand's turn since the reference, averaged (same hemisphere first).
    float d[2][4];
    for (int h = 0; h < 2; h++) {
        const float conj[4] = { -sTwo.q0[h][0], -sTwo.q0[h][1], -sTwo.q0[h][2], sTwo.q0[h][3] };
        QMul(q[h], conj, d[h]);
    }
    if (d[0][0] * d[1][0] + d[0][1] * d[1][1] + d[0][2] * d[1][2] + d[0][3] * d[1][3] < 0.0f) {
        for (int k = 0; k < 4; k++) {
            d[1][k] = -d[1][k];
        }
    }
    float avg[4] = { d[0][0] + d[1][0], d[0][1] + d[1][1], d[0][2] + d[1][2], d[0][3] + d[1][3] };
    QNormalize(avg);
    // Then the line between the hands decides the swing exactly: take the averaged turn's image of the
    // old line onto the new line.
    float turned[3], swing[4], r[4];
    VrPocket::QuatRot(avg, sTwo.axis0, turned);
    Normalize(turned);
    QFromTo(turned, axis, swing);
    QMul(swing, avg, r);
    float off[3] = { sTwo.pivot0[0] - sTwo.mid0[0], sTwo.pivot0[1] - sTwo.mid0[1], sTwo.pivot0[2] - sTwo.mid0[2] };
    float offR[3];
    VrPocket::QuatRot(r, off, offR);
    for (int k = 0; k < 3; k++) {
        pos[k] = mid[k] + offR[k];
    }
    VrPocket::QuatRot(r, sTwo.ax0[0], x);
    VrPocket::QuatRot(r, sTwo.ax0[1], y);
    VrPocket::QuatRot(r, sTwo.ax0[2], z);
    return true;
}

// The object's pose now (its pivot + model axes): rigid in one hand, or following both hands.
bool CurrentPose(float pos[3], float x[3], float y[3], float z[3]) {
    if (sHands[0].holding && sHands[1].holding && sTwo.valid && TwoHandPose(pos, x, y, z)) {
        OrthoAxes(x, y, z);
        return true;
    }
    int n = 0;
    float p[3] = {}, ax[3] = {}, ay[3] = {}, az[3] = {};
    for (int h = 0; h < 2; h++) {
        if (!sHands[h].holding) {
            continue;
        }
        float hp[3], hx[3], hy[3], hz[3];
        if (!sHands[h].grab.Replay(h, hp, hx, hy, hz)) {
            continue;
        }
        for (int k = 0; k < 3; k++) {
            p[k] += hp[k];
            ax[k] += hx[k];
            ay[k] += hy[k];
            az[k] += hz[k];
        }
        n++;
    }
    if (n == 0) {
        return false;
    }
    for (int k = 0; k < 3; k++) {
        pos[k] = p[k] / n;
    }
    OrthoAxes(ax, ay, az);
    memcpy(x, ax, sizeof(ax));
    memcpy(y, ay, sizeof(ay));
    memcpy(z, az, sizeof(az));
    return true;
}

// Record the held object's current pose (about its pivot) in a hand's frame.
void CaptureHand(int hand) {
    float x[3], y[3], z[3], pos[3];
    AxesFromRot(sHeld->shape.rot, x, y, z);
    PivotOf(sHeld, pos);
    sHands[hand].grab.Capture(hand, pos, x, y, z);
    sHands[hand].holding = sHands[hand].grab.valid;
}

void SetPose(const float pivot[3], const float x[3], const float y[3], const float z[3]) {
    sHeld->world.pos = { pivot[0], pivot[1] - PivotLift(sHeld), pivot[2] };
    VrPocket::SetRotFromAxes(&sHeld->shape.rot, x, y, z);
}

void ApplyPose() {
    float pos[3], x[3], y[3], z[3];
    if (sHeld == nullptr || !CurrentPose(pos, x, y, z)) {
        return;
    }
    SetPose(pos, x, y, z);
}

// Closest point of the drawn hand (wrist, middle, fingertips) to a vertical cylinder; cm.
float HandToCylinderCm(int hand, const Actor* actor, const Liftable& l) {
    float pts[3][3];
    int count = 0;
    float m[4][4];
    if (VR_GetHandMatrix(hand, m)) {
        const float len = VrHand_ChildSized() ? 548.0f : 862.0f;
        for (int i = 0; i <= 2; i++) {
            for (int k = 0; k < 3; k++) {
                pts[count][k] = m[0][k] * len * 0.5f * i + m[3][k];
            }
            count++;
        }
    } else {
        float q[4];
        if (!VR_GetHandPose(hand, pts[0], q)) {
            return 1e9f;
        }
        count = 1;
    }
    const float base = actor->world.pos.y + l.yShift;
    float best = 1e9f;
    for (int i = 0; i < count; i++) {
        const float dx = pts[i][0] - actor->world.pos.x, dz = pts[i][2] - actor->world.pos.z;
        const float dxz = fmaxf(sqrtf(dx * dx + dz * dz) - l.radius, 0.0f);
        const float dy = fmaxf(fmaxf(base - pts[i][1], pts[i][1] - (base + l.height)), 0.0f);
        best = fminf(best, sqrtf(dxz * dxz + dy * dy));
    }
    return best / WorldScale() * 100.0f;
}

// While held the object can be at any angle: reach it as a sphere around its middle.
float HandToHeldCm(int hand, const Liftable& l) {
    float p[3], q[4];
    if (!VR_GetHandPose(hand, p, q)) {
        return 1e9f;
    }
    float x[3], y[3], z[3];
    AxesFromRot(sHeld->shape.rot, x, y, z);
    const float mid = l.yShift + l.height * 0.5f;
    const float c[3] = { sHeld->world.pos.x + y[0] * mid, sHeld->world.pos.y + y[1] * mid,
                         sHeld->world.pos.z + y[2] * mid };
    const float r = fmaxf(l.radius, l.height * 0.5f);
    const float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
    return fmaxf(sqrtf(dx * dx + dy * dy + dz * dz) - r, 0.0f) / WorldScale() * 100.0f;
}

// The offered liftable this hand is on, closest first; nullptr = none within reach.
Actor* CandidateFor(Player* player, int hand, float* outCm) {
    const float reachCm = CVarGetFloat("gVrCarryReachCm", 10.0f);
    const u32 now = gPlayState->state.frames;
    Actor* best = nullptr;
    float bestCm = reachCm;
    auto consider = [&](Actor* a) {
        const Liftable* l = FindLiftable(a);
        if (l == nullptr || a->update == nullptr || a->parent != nullptr) {
            return;
        }
        const float cm = HandToCylinderCm(hand, a, *l);
        if (cm <= bestCm) {
            bestCm = cm;
            best = a;
        }
    };
    for (int i = 0; i < sOfferCount; i++) {
        if (now - sOffers[i].frame <= 1) {
            consider(sOffers[i].actor);
        }
    }
    if (player->interactRangeActor != nullptr && player->getItemId == GI_NONE) {
        consider(player->interactRangeActor);
    }
    if (outCm != nullptr) {
        *outCm = bestCm;
    }
    return best;
}

// The offered boulder this hand is on; nullptr = none within reach.
Actor* HeavyCandidateFor(Player* player, int hand, float* outCm) {
    const float reachCm = CVarGetFloat("gVrCarryReachCm", 10.0f);
    const u32 now = gPlayState->state.frames;
    Actor* best = nullptr;
    float bestCm = reachCm;
    for (int i = 0; i < sOfferCount; i++) {
        Actor* a = sOffers[i].actor;
        if (now - sOffers[i].frame > 1 || FindHeavy(a) == nullptr || a->update == nullptr || a->parent != nullptr) {
            continue;
        }
        const float cm = HandToCylinderCm(hand, a, kBoulder);
        if (cm <= bestCm) {
            bestCm = cm;
            best = a;
        }
    }
    if (outCm != nullptr) {
        *outCm = bestCm;
    }
    return best;
}

void ClearLatch() {
    sLatchActor = nullptr;
    sLatched[0] = sLatched[1] = false;
    sPinValid[0] = sPinValid[1] = false;
    sTooHeavyBuzzed = false;
}

// The boulder trails the two hands: each tick it covers gVrCarryHeavyFollow of the way to where the
// hands hold it. Written to the actor here and re-written in the player draw (vanilla places it).
void StepHeavyPose() {
    float pos[3], x[3], y[3], z[3];
    if (!CurrentPose(pos, x, y, z)) {
        return;
    }
    const float k = CVarGetFloat("gVrCarryHeavyFollow", 0.3f);
    for (int i = 0; i < 3; i++) {
        sHeavyPos[i] += (pos[i] - sHeavyPos[i]) * k;
        sHeavyAx[0][i] += (x[i] - sHeavyAx[0][i]) * k;
        sHeavyAx[1][i] += (y[i] - sHeavyAx[1][i]) * k;
        sHeavyAx[2][i] += (z[i] - sHeavyAx[2][i]) * k;
    }
    OrthoAxes(sHeavyAx[0], sHeavyAx[1], sHeavyAx[2]);
}

void WriteHeavyPose() {
    if (sHeld == nullptr) {
        return;
    }
    SetPose(sHeavyPos, sHeavyAx[0], sHeavyAx[1], sHeavyAx[2]);
}

// The hand's velocity at the moment it let go (m/s): the runtime's current velocity together with the
// headset-rate samples of the last 50 ms, averaged. Only the hand's own linear motion — the object is
// carried IN the hand. (Round 1 added the wrist's spin times the hand -> object-origin distance and
// took the fastest sample of the last 100 ms: the origin of a pot or bush is its base, up to 40 cm
// from the hand, so every flick of the wrist added metres per second sideways, and the fastest sample
// came from earlier in the arc, pointing elsewhere.)
bool HandThrowVelocity(int hand, float out[3]) {
    float sum[3] = {}, lin[3], ang[3];
    int n = 0;
    if (VR_GetHandVelocity(hand, lin, ang)) {
        for (int k = 0; k < 3; k++) {
            sum[k] += lin[k];
        }
        n++;
    }
    const VrCombat::TickPath& path = VrCombat::GetTickPath(hand);
    if (path.count > 0) {
        const uint64_t latest = path.samples[path.count - 1].timeNs;
        for (int i = 0; i < path.count; i++) {
            if (latest - path.samples[i].timeNs > 50000000ULL) {
                continue;
            }
            for (int k = 0; k < 3; k++) {
                sum[k] += path.samples[i].linVelMps[k];
            }
            n++;
        }
    }
    if (n == 0) {
        return false;
    }
    for (int k = 0; k < 3; k++) {
        out[k] = sum[k] / n;
    }
    return true;
}

void ClearHold() {
    sTwo.valid = false;
    sHeavy = false;
    sHeld = nullptr;
    sHands[0].holding = sHands[1].holding = false;
    sHands[0].grab.Clear();
    sHands[1].grab.Clear();
    sPrimary = -1;
}

// The last hand let go: throw (or drop) with the object's own velocity.
void Throw(PlayState* play, Player* player, int hand) {
    float v[3] = {}, vh[3];
    int n = 0;
    for (int h = 0; h < 2; h++) {
        // Both hands count when the other let go within the last couple of ticks (a two-hand throw);
        // the boulder is always thrown by both.
        if ((h == hand || sHeavy || sReleasedAgo[h] <= 2) && HandThrowVelocity(h, vh)) {
            for (int k = 0; k < 3; k++) {
                v[k] += vh[k];
            }
            n++;
        }
    }
    float mps = 0.0f;
    if (n > 0) {
        for (float& c : v) {
            c /= n;
        }
        mps = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    }
    // m/s -> units/tick (world scale / 20 Hz). Then matched to the object's own gravity so it lands where
    // the same throw lands in real life: range goes with v^2 / g, so v is scaled by sqrt(g_object / g_real)
    // (a pot's -1.2 units/tick^2 is ~13 m/s^2 at 36 units/m: x1.17). gVrCarryThrowScale on top (1 = real).
    const float ws = WorldScale();
    float k = ws / 20.0f * (sHeavy ? CVarGetFloat("gVrCarryHeavyThrowScale", 1.0f)
                                   : CVarGetFloat("gVrCarryThrowScale", 1.0f));
    const float gObj = -sHeld->gravity * 400.0f / ws; // m/s^2
    if (gObj > 0.5f) {
        k *= sqrtf(gObj / 9.81f);
    }
    float vel[3] = { v[0] * k, v[1] * k, v[2] * k };
    // Thrown from a moving body: the hand velocity is free of the stick's locomotion, so Link's own
    // ground velocity is added back (as vanilla's throw adds his linearVelocity).
    vel[0] += player->actor.velocity.x;
    vel[2] += player->actor.velocity.z;
    const float speed = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    // The boulder never leaves faster than the base game's own heave (10 forward, 20 up ~ 22).
    const float cap = sHeavy ? CVarGetFloat("gVrCarryHeavyThrowMax", 22.0f) : CVarGetFloat("gVrCarryThrowMax", 25.0f);
    if (speed > cap) {
        for (float& c : vel) {
            c *= cap / speed;
        }
    }
    // A gentle open is a drop: no speed at all (a crate then sets down, as vanilla's put-down).
    if (speed < CVarGetFloat("gVrCarryDropSpeed", 1.5f)) {
        vel[0] = vel[1] = vel[2] = 0.0f;
    }
    sDebug.lastReleaseMps = mps;
    sDebug.lastRelease = (vel[0] != 0.0f || vel[1] != 0.0f || vel[2] != 0.0f) ? 1 : 0;
    Player_VrCarryRelease(play, player, vel);
    if (sHeavy) {
        VR_TriggerHaptic(VR_HAND_LEFT, 0.6f, 0.0f, 50.0f);
        VR_TriggerHaptic(VR_HAND_RIGHT, 0.6f, 0.0f, 50.0f);
        ClearLatch();
    } else {
        VR_TriggerHaptic(hand, 0.35f, 0.0f, 25.0f);
    }
    ClearHold();
}

} // namespace

// The light STATIC liftables the small body collider leans over (cuccos and bombs move on their own:
// they keep shoving as vanilla). Sitting where they were placed (no parent).
extern "C" bool VrCarry_LightObjectCylinder(Actor* actor, float* radius, float* height, float* yShift) {
    const Liftable* l = FindLiftable(actor);
    if (l == nullptr || actor->id == ACTOR_EN_NIW || actor->id == ACTOR_EN_BOM || actor->parent != nullptr ||
        actor->update == nullptr) {
        return false;
    }
    *radius = l->radius;
    *height = l->height;
    *yShift = l->yShift;
    return true;
}

extern "C" void VrCarry_NoteOffer(Actor* actor) {
    if (gPlayState == nullptr || (FindLiftable(actor) == nullptr && FindHeavy(actor) == nullptr)) {
        return;
    }
    const u32 now = gPlayState->state.frames;
    // Drop stale entries (older than last frame) and duplicates.
    int w = 0;
    for (int i = 0; i < sOfferCount; i++) {
        if (now - sOffers[i].frame <= 1 && sOffers[i].actor != actor) {
            sOffers[w++] = sOffers[i];
        }
    }
    sOfferCount = w;
    if (sOfferCount < kMaxOffers) {
        sOffers[sOfferCount++] = { actor, now };
    }
}

extern "C" void VrCarry_Tick(PlayState* play, Player* player) {
    if (play == nullptr || player == nullptr || player != GET_PLAYER(play)) {
        return;
    }
    bool grip[2], fresh[2];
    for (int h = 0; h < 2; h++) {
        grip[h] = (VR_GetControllerButton(h) & VR_BTN_GRIP) != 0;
        fresh[h] = grip[h] && !sGripPrev[h];
        sGripPrev[h] = grip[h];
        if (sReleasedAgo[h] < 99) {
            sReleasedAgo[h]++;
        }
    }
    if (!Active(player)) {
        sDebug.gate = CVarGetInteger("gVrPhysCarry", 1) ? 2 : 1;
        ClearHold(); // a carry in progress stays the game's (vanilla placement resumes)
        ClearLatch();
        sDebug.holding = 0;
        sDebug.heavy = 0;
        return;
    }
    sDebug.gate = 0;

    // Ours no longer (thrown by A/B, switched away, burst, exploded, scene change): forget it.
    if (sHeld != nullptr && (player->heldActor != sHeld || sHeld->parent != &player->actor ||
                             !(player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR) || sHeld->update == nullptr)) {
        ClearHold();
    }

    if (sHeld != nullptr && sHeavy) {
        // The boulder: either hand opening throws it.
        if (!grip[0] || !grip[1]) {
            Throw(play, player, !grip[0] ? 0 : 1);
        } else {
            StepHeavyPose();
            WriteHeavyPose();
        }
    } else if (sHeld != nullptr) {
        const Liftable* l = FindLiftable(sHeld);
        // Hands letting go: hand over to the other one, or throw with the last.
        for (int h = 0; h < 2 && sHeld != nullptr; h++) {
            if (!sHands[h].holding || grip[h]) {
                continue;
            }
            const int other = h ^ 1;
            if (sHands[other].holding) {
                ApplyPose();
                sHands[h].holding = false;
                sReleasedAgo[h] = 0;
                CaptureHand(other); // re-anchored on the remaining hand, from the pose it has now
                sTwo.valid = false;
                sPrimary = other;
                VR_TriggerHaptic(h, 0.2f, 0.0f, 15.0f);
            } else {
                sReleasedAgo[h] = 0;
                Throw(play, player, h);
            }
        }
        // The free hand joining with a fresh grip on the object.
        for (int h = 0; h < 2 && sHeld != nullptr && l != nullptr; h++) {
            if (!sHands[h].holding && fresh[h] && HandToHeldCm(h, *l) <= CVarGetFloat("gVrCarryReachCm", 10.0f)) {
                ApplyPose();
                CaptureHand(h);
                CaptureTwo();
                if (sHands[h].holding) {
                    VR_TriggerHaptic(h, 0.4f, 0.0f, 25.0f);
                }
            }
        }
        ApplyPose();
    } else if (player->heldActor == nullptr && !(player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR)) {
        const bool heavyOn = CVarGetInteger("gVrPhysCarryHeavy", 1) != 0;
        // Latched hands: an open grip, a boulder that left, or a hand dragged away lets go.
        for (int h = 0; h < 2; h++) {
            if (sLatched[h] && (!grip[h] || !heavyOn || sLatchActor == nullptr || sLatchActor->update == nullptr ||
                                sLatchActor->parent != nullptr ||
                                HandToCylinderCm(h, sLatchActor, kBoulder) > CVarGetFloat("gVrBlockPullOff", 35.0f))) {
                sLatched[h] = false;
                sPinValid[h] = false;
            }
        }
        if (!sLatched[0] && !sLatched[1]) {
            ClearLatch();
        }
        // Pick up: a fresh grip with the hand on an offered liftable (sword hand first on a tie); on a
        // boulder, the grip latches that hand instead.
        const int first = SwordHand();
        for (int i = 0; i < 2; i++) {
            const int h = i == 0 ? first : first ^ 1;
            float cm = 0.0f;
            Actor* candidate = CandidateFor(player, h, &cm);
            sDebug.nearestCm = candidate != nullptr ? cm : -1.0f;
            if (!fresh[h]) {
                continue;
            }
            if (candidate == nullptr) {
                Actor* rock = (heavyOn && !sLatched[h]) ? HeavyCandidateFor(player, h, nullptr) : nullptr;
                if (rock != nullptr && (sLatchActor == nullptr || sLatchActor == rock)) {
                    sLatchActor = rock;
                    sLatched[h] = true;
                    sPinValid[h] = false;
                    VR_TriggerHaptic(h, 0.45f, 0.0f, 30.0f);
                }
                continue;
            }
            const int result = Player_VrCarryPickUp(play, player, candidate);
            if (result == 1) {
                ClearLatch();
                sHeld = candidate;
                sHeld->bgCheckFlags &= 0xFF00;
                CaptureHand(h);
                sPrimary = h;
                sDebug.lastPickUp = candidate->id;
                VR_TriggerHaptic(h, 0.5f, 0.0f, 35.0f);
                ApplyPose();
                break;
            }
            if (result < 0) {
                // Too heavy without the bracelet: two short pulses, nothing lifted.
                VR_TriggerHaptic(h, 0.6f, 0.0f, 80.0f);
            }
        }
        // Both hands on the boulder: lift it (the vanilla boulder action, without its animation).
        if (sHeld == nullptr && sLatchActor != nullptr && sLatched[0] && sLatched[1]) {
            const int result = Player_VrCarryPickUpHeavy(play, player, sLatchActor);
            if (result == 1) {
                sHeld = sLatchActor;
                sHeavy = true;
                sHeld->bgCheckFlags &= 0xFF00;
                CaptureHand(0);
                CaptureHand(1);
                CaptureTwo();
                sPrimary = SwordHand();
                PivotOf(sHeld, sHeavyPos);
                AxesFromRot(sHeld->shape.rot, sHeavyAx[0], sHeavyAx[1], sHeavyAx[2]);
                sDebug.lastPickUp = sHeld->id;
                VR_TriggerHaptic(VR_HAND_LEFT, 0.8f, 0.0f, 60.0f);
                VR_TriggerHaptic(VR_HAND_RIGHT, 0.8f, 0.0f, 60.0f);
            } else if (result < 0 && !sTooHeavyBuzzed) {
                // No Silver Gauntlets: it won't budge.
                VR_TriggerHaptic(VR_HAND_LEFT, 0.6f, 0.0f, 90.0f);
                VR_TriggerHaptic(VR_HAND_RIGHT, 0.6f, 0.0f, 90.0f);
                sTooHeavyBuzzed = true;
            }
        }
    }
    sDebug.heavy = sHeavy ? 3 : ((sLatched[0] && sLatched[1]) ? 2 : ((sLatched[0] || sLatched[1]) ? 1 : 0));
    sDebug.holding = sHeld != nullptr ? 1 : 0;
    sDebug.hands = (sHands[0].holding ? 1 : 0) | (sHands[1].holding ? 2 : 0);
    sDebug.primary = sPrimary;
}

extern "C" void VrCarry_UpdateCarryPose(Player* player) {
    if (sHeld != nullptr && Active(player) && player->heldActor == sHeld) {
        if (sHeavy) {
            WriteHeavyPose();
        } else {
            ApplyPose();
        }
    }
}

// Latched / lifting hands are drawn on the boulder where they took hold: the hand matrix (scale and
// mirror included) captured in the boulder's frame at the first draw, rebuilt on its pose every draw
// (so they ride the lagging rock). Not live-substituted.
extern "C" bool VrCarry_PinnedHandMatrix(Player* player, int32_t vrHand, float* mf16) {
    if (vrHand < 0 || vrHand > 1 || mf16 == nullptr || !sLatched[vrHand] || sLatchActor == nullptr ||
        sLatchActor->update == nullptr || !Active(player)) {
        return false;
    }
    // The boulder's frame: while lifted, OUR pose (sHeavyPos / sHeavyAx), never its actor fields.
    // The player draw does the L_HAND limb (the right controller) before the R_HAND limb (the left one),
    // and in between vanilla's carry code in L_HAND's PostLimbDraw rewrites the held actor's shape.rot
    // from Link's facing; reading the actor here made the left hand's pin follow the head ("flailing").
    // Our pose is put back on the actor later in the same draw (VrCarry_UpdateCarryPose).
    float ax[3][3];
    float o[3];
    if (sHeavy && sHeld == sLatchActor) {
        memcpy(ax, sHeavyAx, sizeof(ax));
        memcpy(o, sHeavyPos, sizeof(o));
    } else {
        AxesFromRot(sLatchActor->shape.rot, ax[0], ax[1], ax[2]);
        PivotOf(sLatchActor, o); // the frame the model turns in
    }
    float* local = sPinLocal[vrHand];
    if (!sPinValid[vrHand]) {
        // World -> boulder frame: dot with each boulder axis (R^T).
        for (int c = 0; c < 4; c++) {
            float v[3] = { mf16[c * 4 + 0], mf16[c * 4 + 1], mf16[c * 4 + 2] };
            if (c == 3) {
                for (int k = 0; k < 3; k++) {
                    v[k] -= o[k];
                }
            }
            for (int k = 0; k < 3; k++) {
                local[c * 4 + k] = v[0] * ax[k][0] + v[1] * ax[k][1] + v[2] * ax[k][2];
            }
            local[c * 4 + 3] = mf16[c * 4 + 3];
        }
        sPinValid[vrHand] = true;
    }
    for (int c = 0; c < 4; c++) {
        for (int k = 0; k < 3; k++) {
            mf16[c * 4 + k] = local[c * 4 + 0] * ax[0][k] + local[c * 4 + 1] * ax[1][k] + local[c * 4 + 2] * ax[2][k] +
                              (c == 3 ? o[k] : 0.0f);
        }
        mf16[c * 4 + 3] = local[c * 4 + 3];
    }
    return true;
}

extern "C" bool VrCarry_GripConsumed(int32_t hand, uint16_t mask) {
    if (!(mask & VR_BTN_GRIP) || hand < 0 || hand > 1 || gPlayState == nullptr || VrOcarina_InPlay()) {
        return false;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (!Active(player)) {
        return false;
    }
    if (sHeavy || sLatched[hand]) {
        return true;
    }
    if (sHeld != nullptr) {
        // The holding hand(s), and a free hand close enough to join.
        const Liftable* l = FindLiftable(sHeld);
        return sHands[hand].holding ||
               (l != nullptr && HandToHeldCm(hand, *l) <= CVarGetFloat("gVrCarryReachCm", 10.0f));
    }
    if (player->heldActor != nullptr) {
        return false;
    }
    return CandidateFor(player, hand, nullptr) != nullptr ||
           (CVarGetInteger("gVrPhysCarryHeavy", 1) && HeavyCandidateFor(player, hand, nullptr) != nullptr);
}

extern "C" bool VrCarry_BeginDrawWeld(Actor* actor) {
    if (actor == nullptr || actor != sHeld || sHeavy || sPrimary < 0 || gPlayState == nullptr ||
        !CVarGetInteger("gVrCarryLiveDraw", 1) || !Active(GET_PLAYER(gPlayState))) {
        return false;
    }
    gVrMtxWeldHand = sPrimary;
    return true;
}

extern "C" void VrCarry_EndDrawWeld(void) {
    gVrMtxWeldHand = -1;
}

extern "C" void VrCarry_GetDebug(VrCarryDebug* out) {
    if (out != nullptr) {
        *out = sDebug;
    }
}
