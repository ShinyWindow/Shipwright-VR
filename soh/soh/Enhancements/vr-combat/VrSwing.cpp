extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "sfx.h"
extern PlayState* gPlayState;
MtxF* Matrix_GetCurrent(void);
}

#include "VrCombat.h"

#include "soh/cvar_prefixes.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <vr_interface.h>
#include <algorithm>
#include <cmath>

// Physical melee: the sword hits because it physically swept through the target fast enough.
//
// Per draw frame (from the L_HAND PostLimbDraw seam, live controller matrix on the stack):
//  1. Blade tip speed is measured over this tick's headset-rate hand path (m/s, world-scale and
//     age independent).
//  2. A hysteresis tier machine runs: IDLE below gVrPhysArmSpeed; ARMED (windup — enemies see
//     meleeWeaponState -1 and start their guard/dodge reactions) above it; HOT above
//     gVrPhysHitSpeed; back to IDLE when the swing decays below gVrPhysReArmSpeed.
//  3. While HOT, the tick's blade motion is registered as swept AT quads — the same collider
//     construction the vanilla game uses (quad = line at t0 -> line at t1, two skewed edge lines
//     so even a straight stab has quad area), but sub-sampled up to 3 time segments per tick so
//     a fast arc keeps its shape instead of cutting the corner.
//  4. Damage class: normal HOT swings carry the weapon's slash dmgFlags; swings past
//     gVrPhysHeavySpeed carry its jump-slash dmgFlags (~2x in enemy damage tables) — every
//     enemy's authored damage table and vulnerability set keeps working untouched.
//  5. On a recorded hit the tier drops back to ARMED: one strike per swing, re-cross the hit
//     speed to strike again (enemy i-frames additionally gate repeats on their side).
//
// Historical blade positions are reconstructed exactly: the blade's grip-local geometry
// (calibration, mirroring and model scale baked in) is extracted once per frame by expressing
// the live matrix output in the newest grip pose's frame, then re-applied to each older grip
// sample. The one approximation is the camera anchor, which moved slightly within the tick.

namespace {

constexpr int kMaxQuads = 6; // 3 time segments x 2 edge lines

ColliderQuad sQuads[kMaxQuads];
PlayState* sQuadsPlay = nullptr;
int sQuadsUsed = 0;

enum SwingTier { TIER_IDLE = 0, TIER_ARMED, TIER_HOT };
int sTier = TIER_IDLE;
float sTickTipSpeed = 0.0f;  // max blade-midpoint speed measured this tick, m/s
float sTickHandSpeed = 0.0f; // max raw hand speed this tick, m/s (the anti-wrist-flick floor)

// Last frame's edge-line endpoints (world), the fallback sweep when a tick has too few samples.
bool sHaveBladePrev = false;
Vec3f sPrevTip[2];
Vec3f sPrevBase[2];

// Mirror of the player melee quad init (D_80854650, z_player.c) with TOUCH_NEAREST from the
// start: each quad damages only its nearest victim, like every vanilla sword quad.
ColliderQuadInit sVrQuadInit = {
    {
        COLTYPE_NONE,
        AT_ON | AT_TYPE_PLAYER,
        AC_NONE,
        OC1_NONE,
        OC2_TYPE_PLAYER,
        COLSHAPE_QUAD,
    },
    {
        ELEMTYPE_UNK2,
        { 0x00000100, 0x00, 0x01 },
        { 0xFFCFFFFF, 0x00, 0x00 },
        TOUCH_ON | TOUCH_NEAREST,
        BUMP_NONE,
        OCELEM_NONE,
    },
    { { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } } },
};

// Per-weapon [slash, jump-slash] dmgFlags, rows matching vanilla D_80854488 (z_player.c):
// Master, Kokiri (also broken Giant's Knife), Biggoron, Deku Stick.
constexpr uint32_t kDmgFlags[4][2] = {
    { 0x00000200, 0x08000000 },
    { 0x00000100, 0x02000000 },
    { 0x00000400, 0x04000000 },
    { 0x00000002, 0x08000000 },
};

struct Q4 {
    float x, y, z, w;
};
inline Q4 qconj(const Q4& q) {
    return { -q.x, -q.y, -q.z, q.w };
}
inline Vec3f qrot(const Q4& q, const Vec3f& v) {
    const float tx = 2.0f * (q.y * v.z - q.z * v.y);
    const float ty = 2.0f * (q.z * v.x - q.x * v.z);
    const float tz = 2.0f * (q.x * v.y - q.y * v.x);
    return { v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz),
             v.z + q.w * tz + (q.x * ty - q.y * tx) };
}
inline Vec3f vsub(const Vec3f& a, const Vec3f& b) {
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}
inline Vec3f vadd(const Vec3f& a, const Vec3f& b) {
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}
inline Vec3f vscale(const Vec3f& a, float s) {
    return { a.x * s, a.y * s, a.z * s };
}
inline float vdot(const Vec3f& a, const Vec3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline float vdist(const Vec3f& a, const Vec3f& b) {
    const Vec3f d = vsub(a, b);
    return sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
}
inline Vec3f vcross(const Vec3f& a, const Vec3f& b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline Vec3f vnorm(const Vec3f& a) {
    const float m = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
    return (m > 1e-6f) ? vscale(a, 1.0f / m) : Vec3f{ 0.0f, 1.0f, 0.0f };
}

// Physical impacts recorded by the sim that must become damage. With hard-stop collision the
// blade halts AT an enemy's collider surface, so the swept quads only graze it — instead, each
// damaging contact registers a small "strike quad" poked through the surface at the impact
// point on the next draw, and the vanilla AT-vs-AC pipeline (damage tables, effects, drops)
// does the rest. TOUCH_NEAREST makes it hit exactly the struck target.
struct PendingStrike {
    Vec3f pos;
    Vec3f normal;
    uint32_t dmgFlags;
    // Hammer strikes PIERCE the struck surface (a quad standing across it) instead of lying flat
    // inside it: a blunt head stops ON a surface, and flat colliders there — the rusted switch's
    // top tris — are only met by a quad that crosses their plane.
    bool pierce;
};
PendingStrike sPendingStrikes[4];
int sPendingStrikeCount = 0;

// Contact-prim id tags: what kind of thing a prim is (echoed back in contact events, so impact
// sounds/effects can match the material exactly like the vanilla sword-recoil path does).
constexpr int kPrimKindWall = 1;  // low bits = func_80041F10 surface sfx material
constexpr int kPrimKindHard = 2;  // low bits = collider colType (metal/wood/hard)
constexpr int kPrimKindFlesh = 3; // enemy body — silent on touch; damage sounds come from AT/AC
inline int PrimId(int kind, int detail) {
    return (kind << 12) | (detail & 0xFFF);
}
inline int PrimKind(int id) {
    return id >> 12;
}
inline int PrimDetail(int id) {
    return id & 0xFFF;
}

// Copy of the last prim set + registered quads for the debug overlay.
VrContactPrim sDebugPrims[VR_PHYS_MAX_CONTACT_PRIMS];
int sDebugPrimCount = 0;

// ---- M3 limb puppetry ----
// Each tracked NPC/enemy is a "puppet": its limb world positions are recorded during draw
// (the clean animated pose), and every tick each limb is solved against the blade as a small
// sphere. A limb the blade intrudes on is offset outward exactly as far as the intrusion, so
// it RESTS on the blade and follows it while touching (blade under an arm, moving up = the
// arm rides the blade). Released limbs spring back to the animation; hits kick the offsets.
// The offsets exist only at draw time — animation, hitboxes and AI never see them.
constexpr int kPuppetLimbs = 26;
constexpr int kPuppetCount = 4;
struct Puppet {
    Actor* actor;
    int lastSeenTick;
    Vec3f lastPos[kPuppetLimbs]; // world, clean animated pose from last draw
    u8 posValid[kPuppetLimbs];
    Vec3f offset[kPuppetLimbs]; // translation applied to the limb matrix at draw
    Vec3f rotOff[kPuppetLimbs]; // world axis*angle rotation about the limb's own joint
};
Puppet sPuppets[kPuppetCount] = {};
int sPuppetTick = 0;
bool sWarpActive = false;
MtxF sWarpSavedMtx;

// Rotate world-space vector v by axis-angle w (Rodrigues).
inline Vec3f RotateAxisAngle(const Vec3f& v, const Vec3f& w) {
    const float angle = sqrtf(w.x * w.x + w.y * w.y + w.z * w.z);
    if (angle < 1e-4f) {
        return v;
    }
    const Vec3f k = vscale(w, 1.0f / angle);
    const float c = cosf(angle);
    const float s = sinf(angle);
    const Vec3f kxv = vcross(k, v);
    const float kdv = vdot(k, v);
    return vadd(vadd(vscale(v, c), vscale(kxv, s)), vscale(k, kdv * (1.0f - c)));
}

Puppet* FindPuppet(Actor* actor) {
    for (Puppet& p : sPuppets) {
        if (p.actor == actor) {
            return &p;
        }
    }
    return nullptr;
}

void EnsurePuppet(Actor* actor) {
    if (FindPuppet(actor) != nullptr) {
        return;
    }
    // Claim the stalest slot (never-seen slots have lastSeenTick 0 and win).
    Puppet* best = &sPuppets[0];
    for (Puppet& p : sPuppets) {
        if (p.lastSeenTick < best->lastSeenTick) {
            best = &p;
        }
    }
    memset(best, 0, sizeof(*best));
    best->actor = actor;
    best->lastSeenTick = sPuppetTick;
}

// A "bone" is a consecutive limb pair with a plausible length (skeleton arrays are parent-child
// ordered; the gate rejects depth-first sibling jumps). Bone capsules built from these are how
// the blade collides with living bodies.
constexpr float kBoneMinLen = 3.0f;
constexpr float kBoneMaxLen = 40.0f;
constexpr int kMaxBoneCapsules = 20;

// True when this actor's puppet recorded at least one plausible bone last draw — bone-capsule
// collision covers it, so its fat AC body prim must NOT be pushed (the invisible barrel).
bool PuppetHasBones(Actor* actor) {
    Puppet* p = FindPuppet(actor);
    if (p == nullptr) {
        return false;
    }
    for (int i = 0; i + 1 < kPuppetLimbs; i++) {
        if (!p->posValid[i] || !p->posValid[i + 1]) {
            continue;
        }
        const float blen = vdist(p->lastPos[i], p->lastPos[i + 1]);
        if (blen > kBoneMinLen && blen < kBoneMaxLen) {
            return true;
        }
    }
    return false;
}

// Kick the limbs near a hit: an instant offset impulse through the same puppet channel,
// decaying via the normal spring-back. This IS the hit reaction.
void PuppetHitKick(Actor* victim, const Vec3f& hitPos, const Vec3f& dir, float strength) {
    Puppet* p = FindPuppet(victim);
    if (p == nullptr) {
        return;
    }
    for (int i = 0; i < kPuppetLimbs; i++) {
        if (!p->posValid[i]) {
            continue;
        }
        const float dist = vdist(p->lastPos[i], hitPos);
        float fall = 1.0f - dist / 75.0f;
        fall = fall < 0.0f ? 0.0f : fall;
        fall = 0.3f + 0.7f * fall;
        p->offset[i] = vadd(p->offset[i], vscale(dir, strength * fall));
    }
}

// Knockback / press-push impulses queued at draw time and applied during the player's UPDATE:
// the collision system zeroes every actor's displacement in the damage pass at the top of the
// tick (CollisionCheck_ResetDamage), so a displacement written at draw time is wiped before
// the victim ever consumes it. Written from the player's update, it lands the same tick
// (players update before enemies) AFTER the reset. Pointers validated against the live actor
// lists before use.
struct PendingImpulse {
    Actor* actor;
    Vec3f d;
};
PendingImpulse sImpulses[8] = {};
int sImpulseCount = 0;

void QueueImpulse(Actor* victim, float dx, float dz) {
    for (int i = 0; i < sImpulseCount; i++) {
        if (sImpulses[i].actor == victim) {
            sImpulses[i].d.x += dx;
            sImpulses[i].d.z += dz;
            return;
        }
    }
    if (sImpulseCount < 8) {
        sImpulses[sImpulseCount].actor = victim;
        sImpulses[sImpulseCount].d = { dx, 0.0f, dz };
        sImpulseCount++;
    }
}

// A stored Actor* is only safe to dereference if it is still in the live actor lists — a victim
// recorded by the collision pass can be freed before we read it back (a sword-struck switch or
// BG actor that kills itself updates BEFORE the player does).
bool ActorIsLive(PlayState* play, Actor* target) {
    if (target == nullptr) {
        return false;
    }
    for (size_t c = 0; c < ARRAY_COUNT(play->actorCtx.actorLists); c++) {
        for (Actor* a = play->actorCtx.actorLists[c].head; a != NULL; a = a->next) {
            if (a == target) {
                return true;
            }
        }
    }
    return false;
}

// The per-tick puppet solve: every recorded limb vs the blade segment. Called from FeedMelee
// with the effective (sim) blade endpoints, world units. radius < 0 = the blade's own thickness
// (the hammer passes its head radius).
void PuppetSolve(const Vec3f& base, const Vec3f& tip, float radius = -1.0f) {
    const float limbR = CVarGetFloat("gVrPhysLimbRadius", 9.0f);
    const float bladeR = radius >= 0.0f ? radius : CVarGetFloat("gVrPhysBladeThickness", 0.4f) + 1.5f;
    const float maxPush = CVarGetFloat("gVrPhysLimbPushMax", 22.0f);
    const Vec3f bladeDir = vnorm(vsub(tip, base));
    const float bladeLen = vdist(base, tip);
    for (Puppet& p : sPuppets) {
        if (p.actor == nullptr || p.lastSeenTick != sPuppetTick) {
            continue; // only actors the contact gather validated THIS tick
        }
        for (int i = 0; i < kPuppetLimbs; i++) {
            if (!p.posValid[i]) {
                continue;
            }
            float t = vdot(vsub(p.lastPos[i], base), bladeDir);
            t = t < 0.0f ? 0.0f : (t > bladeLen ? bladeLen : t);
            const Vec3f onBlade = vadd(base, vscale(bladeDir, t));
            const Vec3f d = vsub(p.lastPos[i], onBlade);
            const float dist = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
            const float pen = (limbR + bladeR) - dist;
            Vec3f target = { 0.0f, 0.0f, 0.0f };
            Vec3f rotTarget = { 0.0f, 0.0f, 0.0f };
            if (pen > 0.0f && dist > 1e-3f) {
                const float push = pen > maxPush ? maxPush : pen;
                const Vec3f pushDir = vscale(d, 1.0f / dist);
                target = vscale(pushDir, push);
                // Articulation: the limb also ROTATES about its joint, tilting away around
                // the axis perpendicular to (bone direction x push). Bone direction is
                // approximated from the neighbouring limb (skeleton arrays are parent-child
                // ordered); lever = bone length.
                Vec3f boneDir = { 0.0f, 0.0f, 0.0f };
                float lever = 12.0f;
                if (i + 1 < kPuppetLimbs && p.posValid[i + 1]) {
                    const Vec3f toNext = vsub(p.lastPos[i + 1], p.lastPos[i]);
                    const float ln = sqrtf(toNext.x * toNext.x + toNext.y * toNext.y + toNext.z * toNext.z);
                    if (ln > 3.0f && ln < 40.0f) {
                        boneDir = vscale(toNext, 1.0f / ln);
                        lever = ln;
                    }
                }
                if (boneDir.x == 0.0f && boneDir.y == 0.0f && boneDir.z == 0.0f && i > 0 &&
                    p.posValid[i - 1]) {
                    const Vec3f fromPrev = vsub(p.lastPos[i], p.lastPos[i - 1]);
                    const float lp = sqrtf(fromPrev.x * fromPrev.x + fromPrev.y * fromPrev.y +
                                           fromPrev.z * fromPrev.z);
                    if (lp > 3.0f && lp < 40.0f) {
                        boneDir = vscale(fromPrev, 1.0f / lp);
                        lever = lp;
                    }
                }
                if (boneDir.x != 0.0f || boneDir.y != 0.0f || boneDir.z != 0.0f) {
                    const Vec3f axis = vcross(boneDir, pushDir);
                    const float axisLen = sqrtf(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
                    if (axisLen > 0.1f) {
                        float angle = push / lever; // rad: displacement over the bone lever
                        const float maxAngle = 0.7f;
                        angle = angle > maxAngle ? maxAngle : angle;
                        rotTarget = vscale(vscale(axis, 1.0f / axisLen), angle);
                    }
                }
            }
            // Resistance: limbs fight the blade — they yield only partially and lag behind
            // the push, while spring-back to the animation stays firm. 0 = ragdoll-loose,
            // 1 = barely movable.
            const float resist = CVarGetFloat("gVrPhysLimbResist", 0.5f);
            if (pen > 0.0f) {
                target = vscale(target, 1.0f - 0.5f * resist);
                rotTarget = vscale(rotTarget, 1.0f - 0.5f * resist);
            }
            const float k = (pen > 0.0f) ? (0.15f + 0.45f * (1.0f - resist)) : 0.35f;
            p.offset[i] = vadd(p.offset[i], vscale(vsub(target, p.offset[i]), k));
            p.rotOff[i] = vadd(p.rotOff[i], vscale(vsub(rotTarget, p.rotOff[i]), k));
        }
    }
}

// Physical blade outline for the overlay (world units): rootA, cornerA, tip, cornerB, rootB —
// the flat rectangle the solver actually collides, with its tapered point.
float sDebugBlade[5][3];
bool sDebugBladeValid = false;

inline int SwordHand() {
    // The sword rides Link's L_HAND limb, which motion-hands drives with the player's RIGHT
    // controller in right-handed mode (see Player_OverrideLimbDrawGameplayVRFirstPerson).
    return CVarGetInteger("gVrLeftHanded", 0) ? VR_HAND_LEFT : VR_HAND_RIGHT;
}

// Blade length in hand-model units (sliders are in game units; model = x100 before actor scale).
// Defaults match the visual blades (the vanilla trail tips): Kokiri 30, Master 40, Biggoron 55.
float BladeLengthModelUnits(Player* player) {
    const s32 held = Player_GetMeleeWeaponHeld(player);
    if (held == 3 && Player_HoldsBrokenKnife(player)) {
        return 1500.0f; // broken Giant's Knife stub
    }
    switch (held) {
        case 1:
            return CVarGetFloat("gVrPhysBladeLenMaster", 35.0f) * 100.0f;
        case 2:
            return CVarGetFloat("gVrPhysBladeLenKokiri", 18.0f) * 100.0f;
        case 3:
            return CVarGetFloat("gVrPhysBladeLenBiggoron", 55.0f) * 100.0f;
        case 4:
            // Vanilla stick length is unk_85C * 5000 model units (50 game units whole,
            // halved once snapped) — the collider follows the broken stub.
            return CVarGetFloat("gVrPhysBladeLenStick", 50.0f) * 100.0f * player->unk_85C;
        default:
            return 3000.0f;
    }
}

// Closest point on segment a0a1 to segment b0b1 (returned point lies on the FIRST segment).
inline Vec3f ClosestOnSegToSeg(const Vec3f& a0, const Vec3f& a1, const Vec3f& b0, const Vec3f& b1) {
    const Vec3f d1 = vsub(a1, a0);
    const Vec3f d2 = vsub(b1, b0);
    const Vec3f r = vsub(a0, b0);
    const float A = vdot(d1, d1), E = vdot(d2, d2), F = vdot(d2, r), C = vdot(d1, r);
    float s = 0.0f;
    if (A > 1e-9f) {
        if (E > 1e-9f) {
            const float B = vdot(d1, d2);
            const float denom = A * E - B * B;
            s = (denom > 1e-9f) ? (B * F - C * E) / denom : 0.0f;
        } else {
            s = -C / A;
        }
        s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
    }
    return vadd(a0, vscale(d1, s));
}

inline Vec3f rotY(const Vec3f& v, float rad) {
    const float c = cosf(rad);
    const float s = sinf(rad);
    return { c * v.x + s * v.z, v.y, -s * v.x + c * v.z };
}
inline bool PointInTri(const Vec3f& p, const Vec3f& a, const Vec3f& b, const Vec3f& c, const Vec3f& n) {
    if (vdot(vcross(vsub(b, a), vsub(p, a)), n) < 0.0f) {
        return false;
    }
    if (vdot(vcross(vsub(c, b), vsub(p, b)), n) < 0.0f) {
        return false;
    }
    return vdot(vcross(vsub(a, c), vsub(p, c)), n) >= 0.0f;
}

inline float SegPointDist2(const Vec3f& a, const Vec3f& b, const Vec3f& p) {
    const Vec3f ab = vsub(b, a);
    const float len2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    float t = 0.0f;
    if (len2 > 1e-6f) {
        t = ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y + (p.z - a.z) * ab.z) / len2;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }
    const Vec3f q = vadd(a, vscale(ab, t));
    const Vec3f d = vsub(p, q);
    return d.x * d.x + d.y * d.y + d.z * d.z;
}

inline float PointTriDist2(const Vec3f& p, const Vec3f& a, const Vec3f& b, const Vec3f& c, const Vec3f& n) {
    const float sd = vdot(vsub(p, a), n);
    const Vec3f proj = vsub(p, vscale(n, sd));
    if (PointInTri(proj, a, b, c, n)) {
        return sd * sd;
    }
    const float e0 = SegPointDist2(a, b, p);
    const float e1 = SegPointDist2(b, c, p);
    const float e2 = SegPointDist2(c, a, p);
    return fminf(e0, fminf(e1, e2));
}

// True distance^2 from the blade segment to a triangle, plus how far the blade's nearer endpoint
// sits in FRONT of the face (negative = behind). Centroid distance — the old ranking — is wrong
// for level geometry: a big wall triangle you're leaning on has a distant centroid and loses to
// small irrelevant tris, so the kept set churned every tick.
inline float SegTriDist2(const Vec3f& s0, const Vec3f& s1, const Vec3f& a, const Vec3f& b, const Vec3f& c,
                         const Vec3f& n, float& outFrontMost) {
    const float d0 = vdot(vsub(s0, a), n);
    const float d1 = vdot(vsub(s1, a), n);
    outFrontMost = fmaxf(d0, d1);
    if ((d0 > 0.0f) != (d1 > 0.0f)) {
        const float t = d0 / (d0 - d1);
        const Vec3f x = vadd(s0, vscale(vsub(s1, s0), t));
        if (PointInTri(x, a, b, c, n)) {
            return 0.0f;
        }
    }
    float best = fminf(PointTriDist2(s0, a, b, c, n), PointTriDist2(s1, a, b, c, n));
    best = fminf(best, SegPointDist2(s0, s1, a));
    best = fminf(best, SegPointDist2(s0, s1, b));
    best = fminf(best, SegPointDist2(s0, s1, c));
    return best;
}

// Push this tick's contact primitives: what the virtual blade must NOT pass through — the blade
// is always a solid object, at any speed. Level geometry (short line probes around the blade)
// and hard actor colliders (AC_HARD — armor, carapaces, enemy shields). Enemy BODIES are only
// solid via what the player can SEE: in visual-mesh mode the rendered model is harvested and
// the fat vanilla AC cylinder is NOT pushed (it reads as an invisible barrel the blade stops
// on in mid-air); in collision-mesh mode the AC prims are all there is, so they stay. Damage
// is unaffected either way — fast swings pass through and the swept quads hit the real
// collider. Breakables (pots, bushes) are deliberately NOT solid: the blade passes through
// and the swept quads smash them.
void PushContactPrims(PlayState* play, Player* player, const Vec3f& base, const Vec3f& tip) {
    VrContactPrim prims[VR_PHYS_MAX_CONTACT_PRIMS];
    int n = 0;
    memset(prims, 0, sizeof(prims));

    // ---- Level geometry ----
    // PROXIMITY gather over the scene's static polygons: cheap integer AABB reject against the
    // blade's inflated bounds, keep the nearest N by distance to the blade segment. Ray-based
    // discovery flickered whenever the blade lay parallel to the surfaces that mattered
    // (pressing the blade's SIDE against a 90-degree corner: rays along the blade graze past
    // both faces) — and a face that blinks out of the contact set for one tick lets the spring
    // bury the blade, then the snap-back when it reappears is the corner jitter. Proximity is
    // orientation-independent: every nearby face constrains, every tick, unconditionally.
    const Vec3f bladeVec = vsub(tip, base);
    const Vec3f mid = vscale(vadd(base, tip), 0.5f);

    // EXPERIMENTAL: collide with the rendered geometry instead of the collision mesh. The
    // renderer harvests every triangle it draws near the blade (world space), so the blade
    // touches exactly what the player sees — including animated enemy models. Scene-poly and
    // dyna gathering below are skipped in this mode; actor colliders (damage/AC_HARD identity)
    // are still pushed for events and materials.
    const bool visualMesh = CVarGetInteger("gVrPhysVisualMesh", 1) != 0;
    {
        const float regionCenter[3] = { mid.x, mid.y, mid.z };
        VR_PhysSetMeshRegion(regionCenter, CVarGetFloat("gVrPhysMeshRadius", 150.0f),
                             visualMesh ? 1 : 0);
    }

    // Ranked by TRUE segment-to-triangle distance, back-facing surfaces rejected, and sticky
    // across ticks. All three matter — a capture of the corner jitter showed the old gather
    // saturated at its cap with 17 different triangle sets per second, and feeding the solver
    // opposing normals (+x and -x simultaneously) from the far side of a neighbouring wall.
    constexpr int kMaxLevelTris = 16;
    struct LevelCand {
        float d2;
        float rank; // d2 biased by stickiness — ranking only, never a real distance
        CollisionPoly* poly;
    };
    LevelCand cands[kMaxLevelTris];
    int candCount = 0;

    // Triangles chosen last tick: preferred while they remain plausible, so the constraint set
    // stays put instead of swapping members every tick (which dropped contacts to zero and let
    // the spring lunge, then snap back — the jitter cycle).
    static CollisionPoly* sStickyPolys[kMaxLevelTris];
    static int sStickyCount = 0;
    static PlayState* sStickyPlay = NULL;
    if (sStickyPlay != play) {
        sStickyPlay = play;
        sStickyCount = 0;
    }

    CollisionHeader* colHeader = play->colCtx.colHeader;
    if (!visualMesh && colHeader != NULL && colHeader->polyList != NULL && colHeader->vtxList != NULL) {
        const float kMargin = 20.0f;
        // How far behind a face the blade may be and still be constrained by it. Shallow: a face
        // the blade is deeply behind belongs to another volume (the outside of this wall, the
        // ceiling of the room below) and pushing off it drives the blade the wrong way.
        const float kBackMargin = 4.0f;
        const float minX = fminf(base.x, tip.x) - kMargin;
        const float maxX = fmaxf(base.x, tip.x) + kMargin;
        const float minY = fminf(base.y, tip.y) - kMargin;
        const float maxY = fmaxf(base.y, tip.y) + kMargin;
        const float minZ = fminf(base.z, tip.z) - kMargin;
        const float maxZ = fmaxf(base.z, tip.z) + kMargin;

        for (int pi = 0; pi < colHeader->numPolygons; pi++) {
            CollisionPoly* poly = &colHeader->polyList[pi];
            const Vec3s* va = &colHeader->vtxList[COLPOLY_VTX_INDEX(poly->flags_vIA)];
            const Vec3s* vb = &colHeader->vtxList[COLPOLY_VTX_INDEX(poly->flags_vIB)];
            const Vec3s* vc = &colHeader->vtxList[COLPOLY_VTX_INDEX(poly->vIC)];
            const s16 txMin = va->x < vb->x ? (va->x < vc->x ? va->x : vc->x) : (vb->x < vc->x ? vb->x : vc->x);
            const s16 txMax = va->x > vb->x ? (va->x > vc->x ? va->x : vc->x) : (vb->x > vc->x ? vb->x : vc->x);
            if (txMax < minX || txMin > maxX) {
                continue;
            }
            const s16 tyMin = va->y < vb->y ? (va->y < vc->y ? va->y : vc->y) : (vb->y < vc->y ? vb->y : vc->y);
            const s16 tyMax = va->y > vb->y ? (va->y > vc->y ? va->y : vc->y) : (vb->y > vc->y ? vb->y : vc->y);
            if (tyMax < minY || tyMin > maxY) {
                continue;
            }
            const s16 tzMin = va->z < vb->z ? (va->z < vc->z ? va->z : vc->z) : (vb->z < vc->z ? vb->z : vc->z);
            const s16 tzMax = va->z > vb->z ? (va->z > vc->z ? va->z : vc->z) : (vb->z > vc->z ? vb->z : vc->z);
            if (tzMax < minZ || tzMin > maxZ) {
                continue;
            }

            const Vec3f p0 = { (float)va->x, (float)va->y, (float)va->z };
            const Vec3f p1 = { (float)vb->x, (float)vb->y, (float)vb->z };
            const Vec3f p2 = { (float)vc->x, (float)vc->y, (float)vc->z };
            const Vec3f nrm = { COLPOLY_GET_NORMAL(poly->normal.x), COLPOLY_GET_NORMAL(poly->normal.y),
                                COLPOLY_GET_NORMAL(poly->normal.z) };
            float frontMost;
            const float d2 = SegTriDist2(base, tip, p0, p1, p2, nrm, frontMost);
            if (frontMost < -kBackMargin) {
                continue; // blade is entirely behind this face: it belongs to another volume
            }

            float rank = d2;
            for (int s = 0; s < sStickyCount; s++) {
                if (sStickyPolys[s] == poly) {
                    rank *= 0.5f; // hysteresis: keep what already constrains the blade
                    break;
                }
            }
            if (candCount < kMaxLevelTris) {
                cands[candCount].d2 = d2;
                cands[candCount].rank = rank;
                cands[candCount].poly = poly;
                candCount++;
            } else {
                int worst = 0;
                for (int cIdx = 1; cIdx < kMaxLevelTris; cIdx++) {
                    if (cands[cIdx].rank > cands[worst].rank) {
                        worst = cIdx;
                    }
                }
                if (rank < cands[worst].rank) {
                    cands[worst].d2 = d2;
                    cands[worst].rank = rank;
                    cands[worst].poly = poly;
                }
            }
        }

        for (int cIdx = 0; cIdx < candCount && n < VR_PHYS_MAX_CONTACT_PRIMS; cIdx++) {
            CollisionPoly* poly = cands[cIdx].poly;
            const Vec3s* va = &colHeader->vtxList[COLPOLY_VTX_INDEX(poly->flags_vIA)];
            const Vec3s* vb = &colHeader->vtxList[COLPOLY_VTX_INDEX(poly->flags_vIB)];
            const Vec3s* vc = &colHeader->vtxList[COLPOLY_VTX_INDEX(poly->vIC)];
            VrContactPrim& pr = prims[n++];
            pr.type = VR_PHYS_PRIM_TRI;
            pr.a[0] = va->x;
            pr.a[1] = va->y;
            pr.a[2] = va->z;
            pr.b[0] = vb->x;
            pr.b[1] = vb->y;
            pr.b[2] = vb->z;
            pr.c[0] = vc->x;
            pr.c[1] = vc->y;
            pr.c[2] = vc->z;
            pr.radius = 0.0f;
            // Surface sfx material (0xA wood, 0xB soft dirt, else hard stone) rides along so the
            // impact handler can play the same sound the vanilla recoil would.
            pr.id = PrimId(kPrimKindWall, (int)func_80041F10(&play->colCtx, poly, BGCHECK_SCENE));
        }

        sStickyCount = candCount < kMaxLevelTris ? candCount : kMaxLevelTris;
        for (int cIdx = 0; cIdx < sStickyCount; cIdx++) {
            sStickyPolys[cIdx] = cands[cIdx].poly;
        }
    }

    // Dyna geometry (moving platforms, drawbridges) isn't in the static poly list — a few probe
    // rays supplement it. Static hits dedup against the proximity set by poly pointer.
    // (Skipped in visual-mesh mode: dyna pieces are rendered geometry and get harvested.)
    if (!visualMesh) {
    const float kFan = 0.45f; // ~26 degrees
    const Vec3f upNudge = { 0.0f, 12.0f, 0.0f };
    const Vec3f downReach = { 0.0f, -70.0f, 0.0f };
    Vec3f probeFrom[5];
    Vec3f probeTo[5];
    probeFrom[0] = base;
    probeTo[0] = vadd(base, vscale(bladeVec, 1.35f));
    probeFrom[1] = base;
    probeTo[1] = vadd(base, vscale(rotY(bladeVec, kFan), 1.35f));
    probeFrom[2] = base;
    probeTo[2] = vadd(base, vscale(rotY(bladeVec, -kFan), 1.35f));
    probeFrom[3] = vadd(tip, upNudge);
    probeTo[3] = vadd(tip, downReach);
    probeFrom[4] = vadd(mid, upNudge);
    probeTo[4] = vadd(mid, downReach);

    CollisionPoly* seenPolys[5];
    int seenCount = 0;
    for (int i = 0; i < 5 && n < VR_PHYS_MAX_CONTACT_PRIMS; i++) {
        Vec3f hitPos;
        CollisionPoly* poly = NULL;
        s32 bgId;
        if (!BgCheck_EntityLineTest1(&play->colCtx, &probeFrom[i], &probeTo[i], &hitPos, &poly, true, true, false,
                                     true, &bgId) ||
            poly == NULL) {
            continue;
        }
        bool dup = false;
        for (int s = 0; s < seenCount; s++) {
            if (seenPolys[s] == poly) {
                dup = true;
                break;
            }
        }
        for (int cIdx = 0; cIdx < candCount && !dup; cIdx++) {
            if (cands[cIdx].poly == poly) {
                dup = true;
            }
        }
        if (dup) {
            continue;
        }
        seenPolys[seenCount++] = poly;

        Vec3f verts[3];
        CollisionPoly_GetVerticesByBgId(poly, bgId, &play->colCtx, verts);
        VrContactPrim& pr = prims[n++];
        pr.type = VR_PHYS_PRIM_TRI;
        pr.a[0] = verts[0].x;
        pr.a[1] = verts[0].y;
        pr.a[2] = verts[0].z;
        pr.b[0] = verts[1].x;
        pr.b[1] = verts[1].y;
        pr.b[2] = verts[1].z;
        pr.c[0] = verts[2].x;
        pr.c[1] = verts[2].y;
        pr.c[2] = verts[2].z;
        pr.radius = 0.0f;
        pr.id = PrimId(kPrimKindWall, (int)func_80041F10(&play->colCtx, poly, bgId));
    }
    } // !visualMesh

    // Actor colliders, from this tick's AC registrations (populated: enemies updated before draw).
    CollisionCheckContext* colChk = &play->colChkCtx;
    Actor* pressedActors[8];
    int pressedCount = 0;
    for (int i = 0; i < colChk->colACCount && n < VR_PHYS_MAX_CONTACT_PRIMS; i++) {
        Collider* col = colChk->colAC[i];
        if (col == NULL || col->actor == NULL || col->actor == &player->actor) {
            continue;
        }
        const bool hard = (col->acFlags & AC_HARD) != 0;
        const bool enemyBody =
            (col->actor->category == ACTORCAT_ENEMY) || (col->actor->category == ACTORCAT_BOSS);
        const bool npcBody = col->actor->category == ACTORCAT_NPC;
        if (!hard && !enemyBody && !npcBody) {
            continue;
        }
        if (Math_Vec3f_DistXYZ(&col->actor->world.pos, &player->actor.world.pos) > 400.0f) {
            continue;
        }
        const int primId = hard ? PrimId(kPrimKindHard, col->colType) : PrimId(kPrimKindFlesh, 0);

        // M3: enemies are PUSHED by the blade pressing on them (not just struck). When the
        // blade surface is within a couple of units of the enemy's collider surface, feed the
        // one-tick displacement channel — the same post-update positional impulse vanilla
        // collision pushes use, so enemy AI and animations stay untouched. Ground-plane only,
        // ONCE per actor per tick (a JntSph body has many elements in reach at once); bosses
        // and MASS_IMMOVABLE actors never budge. Vanilla mass is otherwise ignored — see the
        // hit-feel note in Swing_OnPlayerUpdate.
        auto pressPush = [&](const Vec3f& surfCenter, float surfRadius) {
            // Enemies only: NPCs' limbs react but their bodies stay planted.
            if (!enemyBody || hard || col->actor->category == ACTORCAT_BOSS ||
                col->actor->colChkInfo.mass == MASS_IMMOVABLE) {
                return;
            }
            for (int pi = 0; pi < pressedCount; pi++) {
                if (pressedActors[pi] == col->actor) {
                    return; // this actor already took its press this tick
                }
            }
            const Vec3f bladeDir = vnorm(vsub(tip, base));
            const float bladeLen = vdist(base, tip);
            float t = vdot(vsub(surfCenter, base), bladeDir);
            t = t < 0.0f ? 0.0f : (t > bladeLen ? bladeLen : t);
            const Vec3f closest = vadd(base, vscale(bladeDir, t));
            Vec3f away3 = vsub(surfCenter, closest);
            const float d3 = sqrtf(away3.x * away3.x + away3.y * away3.y + away3.z * away3.z);
            const float bladeR = CVarGetFloat("gVrPhysBladeThickness", 0.4f);
            const float reach = surfRadius + bladeR + 2.0f;
            if (d3 < 1e-3f || d3 > reach) {
                return; // blade surface not actually pressing the collider surface
            }
            Vec3f away = { away3.x, 0.0f, away3.z };
            const float d = sqrtf(away.x * away.x + away.z * away.z);
            if (d > 1e-3f) {
                const float push = CVarGetFloat("gVrPhysPressPush", 0.0f);
                // Queued: written here (draw time) it would be zeroed before consumption.
                QueueImpulse(col->actor, (away.x / d) * push, (away.z / d) * push);
                if (pressedCount < 8) {
                    pressedActors[pressedCount++] = col->actor;
                }
            }

            // Limb-level press deformation is handled by the puppet solve (PuppetSolve),
            // which pushes individual limbs off the blade continuously.
        };

        // Track this body as a puppet: its limbs become directly manipulable by the blade.
        if ((enemyBody || npcBody) && !hard && col->actor->category != ACTORCAT_BOSS) {
            EnsurePuppet(col->actor);
            Puppet* pp = FindPuppet(col->actor);
            if (pp != nullptr) {
                pp->lastSeenTick = sPuppetTick;
            }
        }

        // Bodies are solid via BONE CAPSULES fitted to their recorded skeletons (appended
        // after these loops) — pushing the fat AC cylinder/spheres too would wrap them in an
        // invisible barrel. Actors with no recorded skeleton (no skelanime hooks in their
        // draw path) fall back to their AC prim so they stay solid. AC_HARD (armor,
        // carapaces) is always solid with its material identity. Press detection and puppet
        // tracking above run off the AC colliders regardless.
        const bool solidPrim = hard || !PuppetHasBones(col->actor);
        if (col->shape == COLSHAPE_CYLINDER) {
            ColliderCylinder* cyl = (ColliderCylinder*)col;
            if (cyl->dim.radius <= 0) {
                continue;
            }
            const float baseX = (float)cyl->dim.pos.x;
            const float baseY = (float)(cyl->dim.pos.y + cyl->dim.yShift);
            const float baseZ = (float)cyl->dim.pos.z;
            const Vec3f cylMid = { baseX, baseY + (float)cyl->dim.height * 0.5f, baseZ };
            pressPush(cylMid, (float)cyl->dim.radius);
            if (solidPrim) {
                VrContactPrim& pr = prims[n++];
                pr.type = VR_PHYS_PRIM_CAPSULE;
                pr.a[0] = baseX;
                pr.a[1] = baseY;
                pr.a[2] = baseZ;
                pr.b[0] = baseX;
                pr.b[1] = baseY + (float)cyl->dim.height;
                pr.b[2] = baseZ;
                pr.radius = (float)cyl->dim.radius;
                pr.id = primId;
            }
        } else if (col->shape == COLSHAPE_JNTSPH) {
            ColliderJntSph* jntSph = (ColliderJntSph*)col;
            for (int e = 0; e < jntSph->count && n < VR_PHYS_MAX_CONTACT_PRIMS; e++) {
                ColliderJntSphElement* el = &jntSph->elements[e];
                if (el->dim.worldSphere.radius <= 0) {
                    continue;
                }
                const Vec3f sphC = { (float)el->dim.worldSphere.center.x,
                                     (float)el->dim.worldSphere.center.y,
                                     (float)el->dim.worldSphere.center.z };
                pressPush(sphC, (float)el->dim.worldSphere.radius);
                if (solidPrim) {
                    VrContactPrim& pr = prims[n++];
                    pr.type = VR_PHYS_PRIM_SPHERE;
                    pr.a[0] = sphC.x;
                    pr.a[1] = sphC.y;
                    pr.a[2] = sphC.z;
                    pr.b[0] = pr.b[1] = pr.b[2] = 0.0f;
                    pr.radius = (float)el->dim.worldSphere.radius;
                    pr.id = primId;
                }
            }
        }
    }

    // NPCs never register AC colliders (they're bump-only, OC list) — gather them from there
    // so friendly bodies are solid to the blade and puppet-able. Enemies were handled above.
    for (int i = 0; i < colChk->colOCCount && n < VR_PHYS_MAX_CONTACT_PRIMS; i++) {
        Collider* col = colChk->colOC[i];
        if (col == NULL || col->actor == NULL || col->actor->category != ACTORCAT_NPC ||
            col->shape != COLSHAPE_CYLINDER) {
            continue;
        }
        if (Math_Vec3f_DistXYZ(&col->actor->world.pos, &player->actor.world.pos) > 400.0f) {
            continue;
        }
        // NO contact prim for NPCs: their bump cylinder is far fatter than the visible body,
        // and pushing it makes an invisible barrel the blade can't enter. In visual-mesh mode
        // the RENDERED body already collides at mesh precision, and the limb puppetry works
        // directly on recorded limb positions — no prim needed. NPCs also stay planted: no
        // press nudge, no hit knockback (townsfolk, not combatants).
        EnsurePuppet(col->actor);
        Puppet* pp = FindPuppet(col->actor);
        if (pp != nullptr) {
            pp->lastSeenTick = sPuppetTick;
        }
    }

    // Pacified (frozen) enemies never run their update, so they stop registering AC colliders
    // and would fall out of the puppet set — track them straight from the actor list instead.
    if (CVarGetInteger("gVrPhysPacifist", 0)) {
        for (Actor* a = play->actorCtx.actorLists[ACTORCAT_ENEMY].head; a != NULL; a = a->next) {
            if (Math_Vec3f_DistXYZ(&a->world.pos, &player->actor.world.pos) > 400.0f) {
                continue;
            }
            EnsurePuppet(a);
            Puppet* pp = FindPuppet(a);
            if (pp != nullptr) {
                pp->lastSeenTick = sPuppetTick;
            }
        }
    }

    // ---- Bone capsules: how the blade collides with living bodies ----
    // A capsule per bone, from the limb positions the puppet system already records during
    // draw. Character render meshes are unsealed multi-shell tri soup that churns the
    // solver's contact set (the enemy-jitter bug) and the AC colliders are invisible
    // barrels; skeleton-fitted capsules are smooth, convex and continuous — N64 characters
    // are practically made of them. Built from the CLEAN animated pose (the same positions
    // the puppet solve uses): flesh visually yields via the puppet while the body under it
    // stays firm, so there is no spring-vs-spring feedback. Nearest bones win the budget.
    {
        const float boneR = CVarGetFloat("gVrPhysBodyCapsuleRadius", 4.5f);
        struct BoneCand {
            float d2;
            const Vec3f* a;
            const Vec3f* b;
        };
        BoneCand bones[kMaxBoneCapsules];
        int boneCount = 0;
        for (Puppet& p : sPuppets) {
            if (p.actor == nullptr || p.lastSeenTick != sPuppetTick) {
                continue;
            }
            for (int i = 0; i + 1 < kPuppetLimbs; i++) {
                if (!p.posValid[i] || !p.posValid[i + 1]) {
                    continue;
                }
                const float blen = vdist(p.lastPos[i], p.lastPos[i + 1]);
                if (blen <= kBoneMinLen || blen >= kBoneMaxLen) {
                    continue; // consecutive-index sibling jump, not a bone
                }
                const Vec3f onBlade = ClosestOnSegToSeg(base, tip, p.lastPos[i], p.lastPos[i + 1]);
                const float d2 = SegPointDist2(p.lastPos[i], p.lastPos[i + 1], onBlade);
                if (boneCount < kMaxBoneCapsules) {
                    bones[boneCount++] = { d2, &p.lastPos[i], &p.lastPos[i + 1] };
                } else {
                    int worst = 0;
                    for (int c = 1; c < kMaxBoneCapsules; c++) {
                        if (bones[c].d2 > bones[worst].d2) {
                            worst = c;
                        }
                    }
                    if (d2 < bones[worst].d2) {
                        bones[worst] = { d2, &p.lastPos[i], &p.lastPos[i + 1] };
                    }
                }
            }
        }
        for (int bi = 0; bi < boneCount && n < VR_PHYS_MAX_CONTACT_PRIMS; bi++) {
            VrContactPrim& pr = prims[n++];
            pr.type = VR_PHYS_PRIM_CAPSULE;
            pr.a[0] = bones[bi].a->x;
            pr.a[1] = bones[bi].a->y;
            pr.a[2] = bones[bi].a->z;
            pr.b[0] = bones[bi].b->x;
            pr.b[1] = bones[bi].b->y;
            pr.b[2] = bones[bi].b->z;
            pr.radius = boneR;
            pr.id = PrimId(kPrimKindFlesh, 0);
        }
    }

    VR_PhysSetContactPrims(prims, n);

    // Snapshot for the debug overlay (drawn at OnPlayDrawEnd, same tick).
    memcpy(sDebugPrims, prims, sizeof(VrContactPrim) * n);
    sDebugPrimCount = n;
}

void EnsureQuads(PlayState* play, Player* player) {
    if (sQuadsPlay == play) {
        return;
    }
    for (int i = 0; i < kMaxQuads; i++) {
        Collider_InitQuad(play, &sQuads[i]);
        Collider_SetQuad(play, &sQuads[i], &player->actor, &sVrQuadInit);
    }
    sQuadsPlay = play;
    sQuadsUsed = 0;
    sTier = TIER_IDLE;
    sHaveBladePrev = false;
    // New scene: puppet/impulse Actor* keys from the previous scene must not survive to match
    // recycled allocations in this one.
    memset(sPuppets, 0, sizeof(sPuppets));
    sImpulseCount = 0;
}

// Reconstruct a world-space blade point at an older grip sample: local = the point expressed in
// the reference (newest) grip frame, re-applied at the sample's pose.
inline Vec3f AtSample(const VrHandSample& s, const Vec3f& local) {
    const Q4 q = { s.quat[0], s.quat[1], s.quat[2], s.quat[3] };
    const Vec3f p = { s.pos[0], s.pos[1], s.pos[2] };
    return vadd(p, qrot(q, local));
}

void RegisterQuad(PlayState* play, uint32_t dmgFlags, const Vec3f& newBase, const Vec3f& newTip,
                  const Vec3f& prevBase, const Vec3f& prevTip) {
    if (sQuadsUsed >= kMaxQuads) {
        return;
    }
    // Zero-motion frames register nothing (the vanilla exact-equality skip, with slop).
    if (vdist(newTip, prevTip) + vdist(newBase, prevBase) < 0.1f) {
        return;
    }
    ColliderQuad* quad = &sQuads[sQuadsUsed++];
    quad->info.toucher.dmgFlags = dmgFlags;
    // DMG_DEKU_STICK hits sound wooden, mirroring vanilla func_80837918.
    quad->info.toucherFlags =
        (dmgFlags == 0x00000002) ? (TOUCH_ON | TOUCH_NEAREST | TOUCH_SFX_WOOD) : (TOUCH_ON | TOUCH_NEAREST);
    // Vanilla vertex order (func_80090480): newBase, newTip, prevBase, prevTip.
    Collider_SetQuadVertices(quad, const_cast<Vec3f*>(&newBase), const_cast<Vec3f*>(&newTip),
                             const_cast<Vec3f*>(&prevBase), const_cast<Vec3f*>(&prevTip));
    CollisionCheck_SetAT(play, &play->colChkCtx, &quad->base);
}

// Physical strikes recorded last tick become damage now, hitting exactly the struck target via
// TOUCH_NEAREST. A blade strike is a small quad lying just inside the surface at the impact point;
// a hammer strike stands ACROSS the surface (6 units out to 8 in), so it also meets flat
// colliders lying on the surface the head stopped on.
void RegisterPendingStrikes(PlayState* play) {
    for (int i = 0; i < sPendingStrikeCount && sQuadsUsed < kMaxQuads; i++) {
        const PendingStrike& st = sPendingStrikes[i];
        const Vec3f up = (st.normal.y > 0.9f || st.normal.y < -0.9f) ? Vec3f{ 1.0f, 0.0f, 0.0f }
                                                                     : Vec3f{ 0.0f, 1.0f, 0.0f };
        const Vec3f t1 = vnorm(vcross(st.normal, up));
        const Vec3f t2 = vcross(st.normal, t1);
        const Vec3f e1 = vscale(t1, 9.0f);
        if (st.pierce) {
            const Vec3f out = vadd(st.pos, vscale(st.normal, 6.0f));
            const Vec3f in = vsub(st.pos, vscale(st.normal, 8.0f));
            RegisterQuad(play, st.dmgFlags, vsub(out, e1), vadd(out, e1), vsub(in, e1), vadd(in, e1));
            continue;
        }
        const Vec3f c = vsub(st.pos, vscale(st.normal, 6.0f)); // pushed inside the surface
        const Vec3f e2 = vscale(t2, 9.0f);
        RegisterQuad(play, st.dmgFlags, vsub(vsub(c, e1), e2), vsub(vadd(c, e1), e2), vadd(vsub(c, e1), e2),
                     vadd(vadd(c, e1), e2));
    }
    sPendingStrikeCount = 0;
}

// ---- Megaton Hammer ----
// Geometry in L_HAND model space, decoded from the hammer's own display lists
// (gLinkAdultLeftHandHoldingHammerNear/FarDL in object_link_boy): the handle runs along +X
// through the fist (butt verts at x = -940, axis height y = 375); the head is an octagonal drum
// centred x = 1888 whose axis runs along Y — the flat striking face at y = +1157, a spike tip at
// y = -584 — about 430 in radius. At Link's scale that is a head ~19 units out from the fist.
constexpr float kHamHandleY = 375.0f;
constexpr float kHamButtX = -940.0f;
constexpr float kHamNeckX = 1400.0f; // top of the grabbable handle, just short of the head
constexpr float kHamHeadX = 1888.0f;
constexpr float kHamHeadZ = 5.0f;
constexpr float kHamHeadR = 420.0f;
// Collision capsule along the head axis: spike tip and face, each inset by the radius.
constexpr float kHamAxisLo = -170.0f;
constexpr float kHamAxisHi = 740.0f;
constexpr float kHamHeadMidY = 0.5f * (kHamAxisLo + kHamAxisHi);
// The vanilla hammer dmgFlags row (D_80854488 row 4) is DMG_HAMMER_SWING / DMG_HAMMER_JUMP. The
// heavy class carries BOTH bits: enemies (whose bumpers take both) resolve the highest matching
// bit — the jump damage — while swing-only targets still answer. The Fire Temple totem pieces
// (BgHidanDalm) bump on DMG_HAMMER_SWING alone, so a jump-only heavy blow left them untouched.
constexpr uint32_t kHammerDmg[2] = { 0x00000040, 0x40000040 };

// This tick's simulated-head path. Drained in the player UPDATE (Swing_OnPlayerUpdate) rather
// than at draw like the sword's, so the same tick's contact events can read the head's
// pre-impact velocity from it. No XR frames step between the update and the draw, so it is the
// same set of samples the draw would have drained.
VrBladeSample sHammerPath[16];
int sHammerPathCount = 0;
float sHammerLastVel[3] = { 0.0f, 0.0f, 0.0f }; // newest sample of the previous snapshot
int sPoundCooldown = 0;

inline Vec3f ModelPt(float x, float y, float z) {
    Vec3f m = { x, y, z };
    Vec3f w;
    Matrix_MultVec3f(&m, &w);
    return w;
}

inline Vec3f SamplePt(const VrBladeSample& s, const Vec3f& local) {
    const Q4 q = { s.quat[0], s.quat[1], s.quat[2], s.quat[3] };
    return vadd(Vec3f{ s.gripPos[0], s.gripPos[1], s.gripPos[2] }, qrot(q, local));
}

// The swing trail at headset rate. The trail (EffectBlure) records one edge per 20 Hz tick, but the
// blade renders live, so the trail's newest edge sat up to a tick behind the blade (a 25 cm gap on a
// fast swing) and its shape was 50 ms chords. Two parts:
//  - SHAPE: before the tick's own edge, a few of this tick's simulated blade samples (headset rate)
//    go in as edges too, as many as the 16-element table holds for the trail's lifetime;
//  - HEAD: the tick's edge is remembered with the hand that drew it, and EffectBlure_Draw adds one
//    more quad from that edge to the blade's LIVE edge, welded to the controller like the bowstring
//    (VrCombat_TrailLiveEdge), so the trail stays attached to the blade every frame.
struct TrailHead {
    bool valid;
    uint32_t frame;
    const void* blure;
    int hand;
    Vec3f tip;
    Vec3f base;
};
TrailHead sTrailHead = {};

void FeedTrail(PlayState* play, Player* player, int hand, const VrBladeSample* samples, int n,
               const Vec3f& tipLocal, const Vec3f& baseLocal) {
    EffectBlure* blure = (EffectBlure*)Effect_GetByIndex(player->meleeWeaponEffectIndex);
    if (blure == nullptr) {
        return;
    }
    if (n >= 2 && CVarGetInteger("gVrTrailSubTick", 1)) {
        // Elements live elemDuration + 1 ticks; each tick adds extra + 1.
        int extra = 16 / (blure->elemDuration + 1) - 1;
        extra = std::min(extra, 3);
        extra = std::min(extra, 16 - blure->numElements - 1); // the tick's own edge always fits
        extra = std::min(extra, n - 1);
        for (int m = 1; m <= extra; m++) {
            const int idx = std::clamp((m * n) / (extra + 1) - 1, 0, n - 2);
            Vec3f t = SamplePt(samples[idx], tipLocal);
            Vec3f b = SamplePt(samples[idx], baseLocal);
            EffectBlure_AddVertex(blure, &t, &b);
        }
    }
    const s32 before = blure->numElements;
    EffectBlure_AddVertex(blure, &player->meleeWeaponInfo[0].tip, &player->meleeWeaponInfo[0].base);
    if (blure->numElements > before) {
        sTrailHead = { true, (uint32_t)play->state.frames, blure, hand, player->meleeWeaponInfo[0].tip,
                       player->meleeWeaponInfo[0].base };
    }
}

// The hammer's half of the melee feed (same seam, same live hand matrix). What differs from a
// blade: the head is HEAVY — soft, torque-limited orientation springs (firm when the off hand
// holds the handle) plus gravity droop, so it trails the hands and carries through; it never
// passes through anything; swing tiers read the SIMULATED head's speed (the weight itself is the
// anti-wrist-flick rule); and the damage quads sweep the head's real cross-section — a face-first
// blow moves the head along its own axis, where a swept axis line would have no area at all.
void FeedHammer(PlayState* play, Player* player) {
    const int hand = SwordHand();
    float worldScale = VR_GetWorldScale();
    if (worldScale < 1.0f) {
        worldScale = 35.0f;
    }

    // World points through the live (served) hand matrix — mirroring and Link's scale included.
    const Vec3f grip0 = ModelPt(0.0f, kHamHandleY, 0.0f);
    const Vec3f butt0 = ModelPt(kHamButtX, kHamHandleY, 0.0f);
    const Vec3f neck0 = ModelPt(kHamNeckX, kHamHandleY, 0.0f);
    const Vec3f headLo0 = ModelPt(kHamHeadX, kHamAxisLo, kHamHeadZ);
    const Vec3f headHi0 = ModelPt(kHamHeadX, kHamAxisHi, kHamHeadZ);
    const Vec3f headC0 = ModelPt(kHamHeadX, kHamHeadMidY, kHamHeadZ);
    const Vec3f headIn0 = ModelPt(kHamHeadX - kHamHeadR, kHamHeadMidY, kHamHeadZ);  // rim toward the fist
    const Vec3f headOut0 = ModelPt(kHamHeadX + kHamHeadR, kHamHeadMidY, kHamHeadZ); // rim past the handle
    const float headR = vdist(headC0, headOut0);
    const Vec3f rimOff = vsub(headOut0, headC0); // world: along the handle, head radius long
    const Vec3f axisDir = vnorm(vsub(headHi0, headLo0));

    float effPosArr[3];
    float effQuatArr[4];
    const bool haveEff = VR_GetHandPose(hand, effPosArr, effQuatArr);
    const Q4 effInv = qconj({ effQuatArr[0], effQuatArr[1], effQuatArr[2], effQuatArr[3] });
    const Vec3f effPos = { effPosArr[0], effPosArr[1], effPosArr[2] };
    auto local = [&](const Vec3f& w) { return qrot(effInv, vsub(w, effPos)); };

    // ---- 1. The heavy head (held-object sim) ----
    if (haveEff) {
        const Vec3f buttL = local(butt0);
        const Vec3f neckL = local(neck0);
        const float buttArr[3] = { buttL.x, buttL.y, buttL.z };
        const float neckArr[3] = { neckL.x, neckL.y, neckL.z };
        VrCombat::Hammer_SetHandleLocal(buttArr, neckArr);

        const VrCombat::HammerGrip& grip = VrCombat::Hammer_GetGrip();
        const bool two = grip.twoHand;
        const Vec3f loL = local(headLo0);
        const Vec3f hiL = local(headHi0);
        const Vec3f cL = local(headC0);
        VrHeldObjectDesc desc = {};
        desc.primaryHand = hand;
        desc.secondaryHand = two ? grip.offHand : -1;
        desc.linFreqHz = CVarGetFloat("gVrHammerLinFreq", 12.0f);
        desc.linZeta = 1.0f;
        // Weight = the torque limit: velocity feed-forward lets ANY spring track a steady swing
        // exactly, so how heavy it feels is how fast the head can be spun up and stopped (suite:
        // a hard 100-degree wrist snap trails ~35 deg and overshoots ~37 one-handed, ~5 two-handed).
        desc.angFreqHz = two ? CVarGetFloat("gVrHammer2HFreq", 9.0f) : CVarGetFloat("gVrHammer1HFreq", 6.0f);
        desc.angZeta = two ? CVarGetFloat("gVrHammer2HZeta", 0.8f) : CVarGetFloat("gVrHammer1HZeta", 0.7f);
        desc.maxAngAccel = two ? CVarGetFloat("gVrHammer2HAccel", 200.0f) : CVarGetFloat("gVrHammer1HAccel", 80.0f);
        desc.maxAccelMps2 = CVarGetFloat("gVrPhysMaxAccel", 400.0f);
        desc.gripLocalRootM[0] = loL.x / worldScale;
        desc.gripLocalRootM[1] = loL.y / worldScale;
        desc.gripLocalRootM[2] = loL.z / worldScale;
        desc.gripLocalTipM[0] = hiL.x / worldScale;
        desc.gripLocalTipM[1] = hiL.y / worldScale;
        desc.gripLocalTipM[2] = hiL.z / worldScale;
        desc.bladeRadiusM = headR / worldScale; // a round capsule: the drum, not a flat blade
        desc.touchToleranceM = CVarGetFloat("gVrPhysTouchTolerance", 0.3f) / worldScale;
        desc.contactEnabled = 1;
        desc.friction = CVarGetFloat("gVrHammerFriction", 0.6f);
        desc.pivotOnly = 1;
        desc.passthroughSpeedMps = 0.0f; // never cuts through: it stops dead on whatever it meets
        desc.gripLocalSecondaryM[0] = grip.secondaryLocal[0] / worldScale;
        desc.gripLocalSecondaryM[1] = grip.secondaryLocal[1] / worldScale;
        desc.gripLocalSecondaryM[2] = grip.secondaryLocal[2] / worldScale;
        desc.gripLocalComM[0] = cL.x / worldScale;
        desc.gripLocalComM[1] = cL.y / worldScale;
        desc.gripLocalComM[2] = cL.z / worldScale;
        desc.gravityScale = two ? CVarGetFloat("gVrHammer2HDroop", 1.0f) : CVarGetFloat("gVrHammer1HDroop", 2.0f);
        VR_PhysSetObject(VR_PHYS_SLOT_WEAPON, &desc);
        PushContactPrims(play, player, headLo0, headHi0);
        PuppetSolve(headLo0, headHi0, headR);

        // Overlay outline: the head's side profile (the capsule the solver collides).
        const Vec3f pts[5] = { vsub(headLo0, rimOff), vsub(headHi0, rimOff), vadd(headHi0, vscale(axisDir, headR)),
                               vadd(headHi0, rimOff), vadd(headLo0, rimOff) };
        for (int i = 0; i < 5; i++) {
            sDebugBlade[i][0] = pts[i].x;
            sDebugBlade[i][1] = pts[i].y;
            sDebugBlade[i][2] = pts[i].z;
        }
        sDebugBladeValid = true;
    } else {
        VR_PhysSetObject(VR_PHYS_SLOT_WEAPON, NULL);
        VR_PhysSetContactPrims(NULL, 0);
        VR_PhysSetMeshRegion(NULL, 0.0f, 0);
        sDebugBladeValid = false;
    }

    // ---- 2. Swing speed: the SIMULATED head, not the wrist ----
    float headSpd = 0.0f;
    for (int i = 0; i < sHammerPathCount; i++) {
        const float* v = sHammerPath[i].midVelMps;
        const float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        headSpd = s > headSpd ? s : headSpd;
    }
    if (sHammerPathCount == 0) {
        float lin[3];
        float ang[3];
        if (VR_GetHandVelocity(hand, lin, ang)) {
            headSpd = sqrtf(lin[0] * lin[0] + lin[1] * lin[1] + lin[2] * lin[2]);
        }
    }
    sTickTipSpeed = headSpd;
    sTickHandSpeed = headSpd;
    const float armSpeed = CVarGetFloat("gVrHammerArmSpeed", 1.5f);
    const float hitSpeed = CVarGetFloat("gVrHammerHitSpeed", 3.0f);
    const float heavySpeed = CVarGetFloat("gVrHammerHeavySpeed", 5.5f);
    const float reArmSpeed = CVarGetFloat("gVrPhysReArmSpeed", 0.8f);
    if (sTier == TIER_IDLE) {
        if (headSpd >= hitSpeed) {
            sTier = TIER_HOT;
        } else if (headSpd >= armSpeed) {
            sTier = TIER_ARMED;
        }
    } else {
        if (sTier == TIER_ARMED && headSpd >= hitSpeed) {
            sTier = TIER_HOT;
        }
        if (headSpd < reArmSpeed) {
            sTier = TIER_IDLE;
        }
    }
    VrCombat::Hammer_NoteSwing(headSpd, sTier);

    // ---- 3. Mirror the melee state (enemy windup AI, swing SFX — the hammer's own whoosh) ----
    // The animation id must read as a HAMMER attack: the Fire Temple totem pieces (BgHidanDalm)
    // only break when meleeWeaponAnimation is HAMMER_FORWARD or HAMMER_SIDE at the moment they
    // read their AC hit — a tick after the blow, when the stopped head may already be idle — so
    // it is held for as long as the hammer is out, never reset to a sword value. Overhead blows
    // report FORWARD, everything else SIDE (the only other readers, the sign's cut shape and
    // spin-attack checks at >= 24, see nothing unusual either way).
    {
        float vy = 0.0f;
        float vh = 0.0f;
        float best = 0.0f;
        for (int i = 0; i < sHammerPathCount; i++) {
            const float* v = sHammerPath[i].midVelMps;
            const float s = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            if (s > best) {
                best = s;
                vy = fabsf(v[1]);
                vh = sqrtf(v[0] * v[0] + v[2] * v[2]);
            }
        }
        if (best > 0.0f) {
            player->meleeWeaponAnimation = (vy > vh) ? PLAYER_MWA_HAMMER_FORWARD : PLAYER_MWA_HAMMER_SIDE;
        } else if (player->meleeWeaponAnimation != PLAYER_MWA_HAMMER_FORWARD &&
                   player->meleeWeaponAnimation != PLAYER_MWA_HAMMER_SIDE) {
            player->meleeWeaponAnimation = PLAYER_MWA_HAMMER_FORWARD;
        }
    }
    if (sTier == TIER_IDLE) {
        player->meleeWeaponState = 0;
    } else {
        VrCombat_SetMeleeWeaponState(player, (sTier == TIER_HOT) ? 1 : -1);
    }

    // ---- 4. Trail: grip to the far rim of the head, like the vanilla hammer's ----
    if (sTier != TIER_IDLE) {
        if (func_80090480(play, NULL, &player->meleeWeaponInfo[0], const_cast<Vec3f*>(&headOut0),
                          const_cast<Vec3f*>(&grip0)) &&
            !CVarGetInteger(CVAR_ENHANCEMENT("DisableLinkSwordTrail"), 0)) {
            FeedTrail(play, player, hand, sHammerPath, haveEff ? sHammerPathCount : 0, local(headOut0), local(grip0));
        }
    } else {
        player->meleeWeaponInfo[0].active = 0;
        player->meleeWeaponInfo[1].active = 0;
        player->meleeWeaponInfo[2].active = 0;
    }

    // ---- 5. Damage ----
    sQuadsUsed = 0;
    RegisterPendingStrikes(play);
    if (sTier == TIER_HOT) {
        const uint32_t dmgFlags = kHammerDmg[headSpd >= heavySpeed ? 1 : 0];
        if (sHammerPathCount >= 2 && haveEff) {
            // The head's cross-line (through the drum centre, along the handle) swept between
            // simulated samples: a face-first blow sweeps it broadside.
            const Vec3f inL = local(headIn0);
            const Vec3f outL = local(headOut0);
            int segments = sHammerPathCount - 1;
            if (segments > kMaxQuads - 2) {
                segments = kMaxQuads - 2;
            }
            int prevIdx = 0;
            for (int k = 1; k <= segments; k++) {
                const int idx = (k * (sHammerPathCount - 1)) / segments;
                if (idx <= prevIdx) {
                    continue;
                }
                RegisterQuad(play, dmgFlags, SamplePt(sHammerPath[idx], inL), SamplePt(sHammerPath[idx], outL),
                             SamplePt(sHammerPath[prevIdx], inL), SamplePt(sHammerPath[prevIdx], outL));
                prevIdx = idx;
            }
        } else if (sHaveBladePrev) {
            RegisterQuad(play, dmgFlags, headIn0, headOut0, sPrevBase[0], sPrevTip[0]);
        }
        // ...plus the head's side profile at the current pose (the blade's "stab quad").
        RegisterQuad(play, dmgFlags, vsub(headLo0, rimOff), vsub(headHi0, rimOff), vadd(headLo0, rimOff),
                     vadd(headHi0, rimOff));
    }
    sPrevTip[0] = headOut0;
    sPrevBase[0] = headIn0;
    sHaveBladePrev = true;
    // Consumed (a draw without an update — the pause screen — must not replay it). The newest
    // head velocity stays behind for a contact on the very first step of the next window.
    if (sHammerPathCount > 0) {
        memcpy(sHammerLastVel, sHammerPath[sHammerPathCount - 1].midVelMps, sizeof(sHammerLastVel));
    }
    sHammerPathCount = 0;
}

// A contact event of the hammer's head (drained in the player update). The approach speed is the
// simulated head's velocity INTO the surface just before it touched — the sample of the contact
// step itself already carries the stop. Enough of it on a floor = the vanilla ground pound; on a
// wall = the vanilla wall strike (thud, quake, hit-stop — no recoil shove); into anything at hit
// speed = a piercing strike that lands the hammer's damage on exactly what it struck.
void HammerContact(PlayState* play, Player* player, const VrContactEvent& ev) {
    const Vec3f n = { ev.normal[0], ev.normal[1], ev.normal[2] };
    auto into = [&](const float* v) { return -(v[0] * n.x + v[1] * n.y + v[2] * n.z); };
    float approach = 0.0f;
    int before = 0;
    for (int i = sHammerPathCount - 1; i >= 0 && before < 2; i--) {
        if (sHammerPath[i].timeNs >= ev.timeNs) {
            continue;
        }
        const float a = into(sHammerPath[i].midVelMps);
        approach = a > approach ? a : approach;
        before++;
    }
    if (before == 0) {
        approach = into(sHammerLastVel); // contact on the first step of this tick's window
    }
    approach = approach > 0.0f ? approach : 0.0f;

    const int kind = PrimKind(ev.primId);
    const float hitSpeed = CVarGetFloat("gVrHammerHitSpeed", 3.0f);
    const float heavySpeed = CVarGetFloat("gVrHammerHeavySpeed", 5.5f);
    const float poundSpeed = CVarGetFloat("gVrHammerPoundSpeed", 2.5f);
    int strike = 0;
    if (approach >= hitSpeed && sPendingStrikeCount < 4) {
        strike = approach >= heavySpeed ? 2 : 1;
        PendingStrike& st = sPendingStrikes[sPendingStrikeCount++];
        st.pos = { ev.pos[0], ev.pos[1], ev.pos[2] };
        st.normal = n;
        st.dmgFlags = kHammerDmg[strike - 1];
        st.pierce = true;
    }

    const VrCombat::HammerGrip& grip = VrCombat::Hammer_GetGrip();
    int result = 0;
    if (kind == kPrimKindFlesh) {
        result = 3; // a body: the strike is the whole answer (vanilla damage reactions and sounds)
    } else if (kind == kPrimKindHard) {
        // Armor, carapaces, shields: a blunt clang off the hard surface, as the sword path does.
        result = 3;
        if (approach > 0.4f) {
            Vec3f pos = { ev.pos[0], ev.pos[1], ev.pos[2] };
            const int detail = PrimDetail(ev.primId);
            if (detail == COLTYPE_WOOD || detail == COLTYPE_TREE) {
                CollisionCheck_SpawnShieldParticlesWood(play, &pos, &player->actor.projectedPos);
            } else {
                CollisionCheck_SpawnShieldParticlesMetalSound(play, &pos, &player->actor.projectedPos);
            }
        }
    } else if (approach >= poundSpeed && sPoundCooldown == 0) {
        const bool ground = n.y >= CVarGetFloat("gVrHammerFloorNormal", 0.6f);
        Player_VrHammerImpact(play, player, ev.pos, ground ? 1 : 0);
        sPoundCooldown = 8; // one pound per blow: a bouncing head must not re-fire it
        // Felt through the whole handle: both hands when both hold it.
        float amp = 0.7f + 0.06f * approach;
        amp = amp > 1.0f ? 1.0f : amp;
        float ms = 70.0f + 15.0f * approach;
        ms = ms > 180.0f ? 180.0f : ms;
        VR_TriggerHaptic(SwordHand(), amp, 0.0f, ms);
        if (grip.twoHand && grip.offHand >= 0) {
            VR_TriggerHaptic(grip.offHand, amp, 0.0f, ms);
        }
        result = ground ? 1 : 2;
    } else if (approach > 0.8f) {
        // Set down firmly but short of a pound: a dull knock, no quake.
        Player_PlaySfx(&player->actor, NA_SE_IT_WALL_HIT_SOFT);
    }
    VrCombat::Hammer_NoteImpact(approach, result, strike);
}

} // namespace

extern "C" bool VrCombat_MeleeCovered(Player* player) {
    if (player->actor.category != ACTORCAT_PLAYER) {
        return false; // co-op partner keeps vanilla behavior
    }
    const s32 held = Player_GetMeleeWeaponHeld(player);
    // 1 = Master, 2 = Kokiri, 3 = Biggoron/Giant's Knife (physical, one-hand feel), 4 = Deku
    // stick (physical: swings, wood SFX, breaks on landed impacts; world collision optional via
    // gVrPhysStickCollision), 5 = Megaton Hammer (physical: heavy simulated head, two-hand grip,
    // ground pound — FeedHammer / VrHammer.cpp; gVrPhysHammer off = vanilla button combat).
    return (held == 1) || (held == 2) || (held == 3) || (held == 4) ||
           (held == 5 && CVarGetInteger("gVrPhysHammer", 1));
}

extern "C" bool VrCombat_MeleeQuadsHit(void) {
    if (sQuadsPlay == nullptr) {
        return false;
    }
    for (int i = 0; i < sQuadsUsed; i++) {
        if (sQuads[i].base.atFlags & AT_HIT) {
            return true;
        }
    }
    return false;
}

extern "C" void VrCombat_FeedMelee(PlayState* play, Player* player) {
    // The world-space pause menu draws Link while the game is frozen. A frozen frame is not a
    // swing: no tier change, no sim feed, no trail, no haptics (before that menu, nothing of Link
    // ran while paused). The sword still renders on the live hand through the limb override.
    if (play->pauseCtx.state != 0 || play->pauseCtx.debugState != 0) {
        return;
    }
    EnsureQuads(play, player);

    // A weapon change invalidates the sparse-tick fallback line (it belongs to the old weapon).
    static s32 sLastHeld = -1;
    const s32 heldNow = Player_GetMeleeWeaponHeld(player);
    if (heldNow != sLastHeld) {
        sLastHeld = heldNow;
        sHaveBladePrev = false;
    }
    if (heldNow == 5) {
        FeedHammer(play, player);
        return;
    }

    // Blade geometry through the live (controller) matrix: ONE line from hilt to visual tip.
    // The vanilla collider fattens this considerably (edge lines extended ~12 units past the tip
    // with ~20 units of lateral spread) as animation-era forgiveness; here the collider is
    // exactly the sword you see, with per-weapon length sliders. The cross points extrude the
    // line sideways by the width slider — they form the stab quad and nothing else.
    const float lenModel = BladeLengthModelUnits(player);
    const float halfWidthModel = CVarGetFloat("gVrPhysBladeWidth", 4.0f) * 50.0f;
    Vec3f tip0, base0, crossTipA, crossBaseA, crossTipB, crossBaseB;
    {
        Vec3f m = { lenModel, 400.0f, 0.0f };
        Matrix_MultVec3f(&m, &tip0);
        m = { 0.0f, 400.0f, 0.0f };
        Matrix_MultVec3f(&m, &base0);
        m = { lenModel, 400.0f, -halfWidthModel };
        Matrix_MultVec3f(&m, &crossTipA);
        m = { 0.0f, 400.0f, -halfWidthModel };
        Matrix_MultVec3f(&m, &crossBaseA);
        m = { lenModel, 400.0f, halfWidthModel };
        Matrix_MultVec3f(&m, &crossTipB);
        m = { 0.0f, 400.0f, halfWidthModel };
        Matrix_MultVec3f(&m, &crossBaseB);
    }

    const int hand = SwordHand();
    const VrCombat::TickPath& path = VrCombat::GetTickPath(hand);

    float worldScale = VR_GetWorldScale();
    if (worldScale < 1.0f) {
        worldScale = 35.0f;
    }

    // ---- 1. Swing speed over the tick (m/s) ----
    // Velocity of the blade's MIDPOINT, evaluated analytically per sample as v_hand + w x r from
    // the runtime's FILTERED velocities. Three properties matter:
    //  - tracked velocities never contain artificial locomotion, snap turns or camera-anchor
    //    motion, so running or turning with the stick cannot read as a swing;
    //  - no finite differencing of per-frame positions, so wrist tracking noise isn't amplified
    //    down the blade's lever arm;
    //  - the midpoint halves the lever a pure wrist-flick gets, and the hand-speed floor below
    //    finishes the job: damaging swings have to come from the arm (GORN/Ancient Dungeon rule).
    // The blade's grip-local geometry is extracted against the EFFECTIVE hand pose — the same
    // (possibly simulated) pose whose matrix produced tips/bases. Extracting against the raw
    // controller would corrupt the geometry the moment the virtual blade deflects on contact.
    // The local vectors are frame-independent constants of the rigid blade, so applying them to
    // raw hand samples below still reconstructs the player's INTENT blade for speed measurement.
    float effPosArr[3];
    float effQuatArr[4];
    const bool haveEff = VR_GetHandPose(hand, effPosArr, effQuatArr);
    const Q4 effInv = qconj({ effQuatArr[0], effQuatArr[1], effQuatArr[2], effQuatArr[3] });
    const Vec3f effPos = { effPosArr[0], effPosArr[1], effPosArr[2] };
    const Vec3f tipLocal0 = haveEff ? qrot(effInv, vsub(tip0, effPos)) : Vec3f{ 0.0f, 0.0f, 0.0f };
    const Vec3f baseLocal0 = haveEff ? qrot(effInv, vsub(base0, effPos)) : Vec3f{ 0.0f, 0.0f, 0.0f };

    sTickTipSpeed = 0.0f;
    sTickHandSpeed = 0.0f;

    if (haveEff && path.count >= 1) {
        const Vec3f midLocal = vscale(vadd(tipLocal0, baseLocal0), 0.5f);

        for (int i = 0; i < path.count; i++) {
            const VrHandSample& s = path.samples[i];
            const Q4 q = { s.quat[0], s.quat[1], s.quat[2], s.quat[3] };
            const Vec3f rM = vscale(qrot(q, midLocal), 1.0f / worldScale); // hand->blade-mid, meters
            const Vec3f v = { s.linVelMps[0] + s.angVelRps[1] * rM.z - s.angVelRps[2] * rM.y,
                              s.linVelMps[1] + s.angVelRps[2] * rM.x - s.angVelRps[0] * rM.z,
                              s.linVelMps[2] + s.angVelRps[0] * rM.y - s.angVelRps[1] * rM.x };
            const float swing = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
            const float handSpd =
                sqrtf(s.linVelMps[0] * s.linVelMps[0] + s.linVelMps[1] * s.linVelMps[1] +
                      s.linVelMps[2] * s.linVelMps[2]);
            if (swing > sTickTipSpeed) {
                sTickTipSpeed = swing;
            }
            if (handSpd > sTickHandSpeed) {
                sTickHandSpeed = handSpd;
            }
        }
    } else {
        // Sparse tick (hitch / hand just tracked): fall back to the hand's own speed.
        float lin[3];
        float ang[3];
        if (VR_GetHandVelocity(hand, lin, ang)) {
            sTickHandSpeed = sTickTipSpeed = sqrtf(lin[0] * lin[0] + lin[1] * lin[1] + lin[2] * lin[2]);
        }
    }

    // ---- 2. Tier hysteresis ----
    const float armSpeed = CVarGetFloat("gVrPhysArmSpeed", 2.0f);
    const float hitSpeed = CVarGetFloat("gVrPhysHitSpeed", 5.0f);
    const float heavySpeed = CVarGetFloat("gVrPhysHeavySpeed", 8.0f);
    const float reArmSpeed = CVarGetFloat("gVrPhysReArmSpeed", 0.8f);
    // Damage additionally requires the HAND itself to move: a stationary-wrist flick can spin
    // the blade fast but never hurts anything (it still arms — trail/SFX/AI windup feedback).
    const float minHandSpeed = CVarGetFloat("gVrPhysMinHandSpeed", 1.2f);
    const bool handCommitted = sTickHandSpeed >= minHandSpeed;

    if (sTier == TIER_IDLE) {
        if (sTickTipSpeed >= hitSpeed && handCommitted) {
            sTier = TIER_HOT;
        } else if (sTickTipSpeed >= armSpeed) {
            sTier = TIER_ARMED;
        }
    } else {
        if (sTier == TIER_ARMED && sTickTipSpeed >= hitSpeed && handCommitted) {
            sTier = TIER_HOT;
        }
        if (sTickTipSpeed < reArmSpeed) {
            sTier = TIER_IDLE;
        }
    }

    // ---- 2b. Held-object sim: the virtual blade with inertia + contact ----
    // The Deku stick's world collision is optional (gVrPhysStickCollision): with it off the
    // stick swings through walls like vanilla — no sim blade, quads from the raw hand path.
    const bool inertiaOn = CVarGetInteger("gVrPhysBladeInertia", 1) != 0 &&
                           !(Player_GetMeleeWeaponHeld(player) == 4 && !CVarGetInteger("gVrPhysStickCollision", 1));
    if (inertiaOn && haveEff) {
        VrHeldObjectDesc desc = {};
        desc.primaryHand = hand;
        desc.secondaryHand = -1;
        desc.linFreqHz = CVarGetFloat("gVrPhysSword1HFreq", 14.0f);
        desc.linZeta = CVarGetFloat("gVrPhysSword1HZeta", 1.0f);
        desc.angZeta = desc.linZeta;
        desc.gripLocalRootM[0] = baseLocal0.x / worldScale;
        desc.gripLocalRootM[1] = baseLocal0.y / worldScale;
        desc.gripLocalRootM[2] = baseLocal0.z / worldScale;
        desc.gripLocalTipM[0] = tipLocal0.x / worldScale;
        desc.gripLocalTipM[1] = tipLocal0.y / worldScale;
        desc.gripLocalTipM[2] = tipLocal0.z / worldScale;
        // Flat-blade cross-section: half-width vector from the damage cross quad's width
        // points, expressed grip-locally the same way as the tip — so the physical rectangle
        // is exactly as wide as the Blade Width slider and lies in the visual blade's plane.
        {
            Vec3f widthLocal0 = qrot(effInv, vsub(crossTipB, tip0));
            // Collider roll: rotate the flat plane about the blade axis (degrees). The model's
            // width axis isn't guaranteed to match the visual flat of every sword — tune with
            // the debug outline visible until the cyan rectangle lies in the blade's plane.
            const float rollDeg = CVarGetFloat("gVrPhysBladeRoll", -90.0f);
            if (rollDeg != 0.0f) {
                const Vec3f axis = vnorm(vsub(tipLocal0, baseLocal0));
                const float half = rollDeg * (3.14159265f / 180.0f) * 0.5f;
                const float s = sinf(half);
                const Q4 rollQ = { axis.x * s, axis.y * s, axis.z * s, cosf(half) };
                widthLocal0 = qrot(rollQ, widthLocal0);
            }
            desc.gripLocalEdgeM[0] = widthLocal0.x / worldScale;
            desc.gripLocalEdgeM[1] = widthLocal0.y / worldScale;
            desc.gripLocalEdgeM[2] = widthLocal0.z / worldScale;
            const float taper = CVarGetFloat("gVrPhysBladeTipTaper", 0.2f);
            desc.tipTaperFrac = taper;

            // Collider position adjustment (game units, blade-local frame): shift the whole
            // physical rectangle along the blade / across the edge / through the flat until
            // it sits exactly on the visible steel. Applies to the collider and the debug
            // outline (which is the alignment tool), never to the damage quads.
            // NO x100 here: tipLocal0/baseLocal0 came OUT of the hand matrix, so this frame
            // is already world units — the model-space x100 convention only applies to values
            // fed INTO the matrix (blade length). Scaling the sliders by 100 made one 0.25
            // step throw the collider a couple of feet.
            const Vec3f fwdDir = vnorm(vsub(tipLocal0, baseLocal0));
            const Vec3f widthDir = vnorm(widthLocal0);
            const Vec3f flatDir = vnorm(vcross(fwdDir, widthDir));
            const Vec3f colShift =
                vadd(vadd(vscale(fwdDir, CVarGetFloat("gVrPhysBladeShiftFwd", 0.0f)),
                          vscale(widthDir, CVarGetFloat("gVrPhysBladeShiftEdge", 1.1f))),
                     vscale(flatDir, CVarGetFloat("gVrPhysBladeShiftFlat", 0.0f)));
            const Vec3f baseAdj = vadd(baseLocal0, colShift);
            const Vec3f tipAdj = vadd(tipLocal0, colShift);
            desc.gripLocalRootM[0] = baseAdj.x / worldScale;
            desc.gripLocalRootM[1] = baseAdj.y / worldScale;
            desc.gripLocalRootM[2] = baseAdj.z / worldScale;
            desc.gripLocalTipM[0] = tipAdj.x / worldScale;
            desc.gripLocalTipM[1] = tipAdj.y / worldScale;
            desc.gripLocalTipM[2] = tipAdj.z / worldScale;

            // Overlay copy of the exact physical rectangle (world units, effective pose).
            const Q4 effQ = { effQuatArr[0], effQuatArr[1], effQuatArr[2], effQuatArr[3] };
            const Vec3f taperLocal = vadd(baseAdj, vscale(vsub(tipAdj, baseAdj), 1.0f - taper));
            const Vec3f pts[5] = { vadd(baseAdj, widthLocal0), vadd(taperLocal, widthLocal0),
                                   tipAdj, vsub(taperLocal, widthLocal0),
                                   vsub(baseAdj, widthLocal0) };
            for (int i = 0; i < 5; i++) {
                const Vec3f w = vadd(effPos, qrot(effQ, pts[i]));
                sDebugBlade[i][0] = w.x;
                sDebugBlade[i][1] = w.y;
                sDebugBlade[i][2] = w.z;
            }
            sDebugBladeValid = true;
        }
        desc.contactEnabled = 1;
        // Low default friction: steel skates along stone/flesh instead of planting.
        desc.friction = CVarGetFloat("gVrPhysBladeFriction", 0.5f);
        // Contact distances are authored in GAME UNITS (what the blade-length sliders use) and
        // converted to meters here, so they mean the same thing at any world scale.
        desc.bladeRadiusM = CVarGetFloat("gVrPhysBladeThickness", 0.4f) / worldScale;
        desc.touchToleranceM = CVarGetFloat("gVrPhysTouchTolerance", 0.3f) / worldScale;
        desc.maxAngAccel = CVarGetFloat("gVrPhysMaxAngAccel", 3000.0f);
        desc.pivotOnly = CVarGetInteger("gVrPhysPivotOnly", 1);
        // Committed swings cut THROUGH instead of snagging; default matches the damage tier
        // ("fast enough to hurt = fast enough to cut through").
        desc.passthroughSpeedMps = CVarGetFloat("gVrPhysPassthroughSpeed", 2.2f);
        desc.cutDragFlesh = CVarGetFloat("gVrPhysCutDragFlesh", 0.73f);
        desc.cutDragWorld = CVarGetFloat("gVrPhysCutDragWorld", 0.73f);
        desc.angFreqHz = CVarGetFloat("gVrPhysSwordAngFreq", 30.0f);
        desc.maxAccelMps2 = CVarGetFloat("gVrPhysMaxAccel", 400.0f);
        // Ancient Dungeon-style cosmetic weight: the RENDERED sword trails fast swings by a
        // few frames of rotation and snaps back with a little overshoot. Collision, damage
        // and swing detection never lag — this is pure feel.
        desc.visualLagS = CVarGetFloat("gVrPhysWeightLagMs", 0.0f) * 0.001f;
        desc.visualSnapHz = CVarGetFloat("gVrPhysWeightSnapHz", 2.0f);
        VR_PhysSetObject(VR_PHYS_SLOT_WEAPON, &desc);
        PushContactPrims(play, player, base0, tip0);
        // Solve the blade against every tracked puppet's limbs (recorded during last draw):
        // limbs the blade intrudes on get pushed out and ride it; released limbs spring back.
        PuppetSolve(base0, tip0);
    } else {
        VR_PhysSetObject(VR_PHYS_SLOT_WEAPON, NULL);
        VR_PhysSetContactPrims(NULL, 0);
        VR_PhysSetMeshRegion(NULL, 0.0f, 0);
        sDebugBladeValid = false;
    }

    // ---- 3. Mirror the melee state for everyone who reads it ----
    // Enemy AI reacts to windups via meleeWeaponState (Stalfos/Gerudo/Wolfos/Dark Link...), and
    // the Ganondorf final blow requires it nonzero. The animation id is pinned to a plain slash
    // so spin-only AI branches stay cold. The setter shim plays the swing SFX on the 0->nonzero
    // edge, exactly like a vanilla attack.
    player->meleeWeaponAnimation = PLAYER_MWA_FORWARD_SLASH_1H;
    if (sTier == TIER_IDLE) {
        player->meleeWeaponState = 0;
    } else {
        VrCombat_SetMeleeWeaponState(player, (sTier == TIER_HOT) ? 1 : -1);
    }

    // This tick's simulated blade samples (drained once; the trail and the damage quads share them).
    VrBladeSample bladePath[16];
    const int bladeN = inertiaOn ? VR_PhysGetBladePath(VR_PHYS_SLOT_WEAPON, bladePath, 16) : 0;

    // ---- 4. Sword trail ----
    if (sTier != TIER_IDLE) {
        if (func_80090480(play, NULL, &player->meleeWeaponInfo[0], &tip0, &base0) &&
            !(player->stateFlags1 & PLAYER_STATE1_SHIELDING) &&
            !CVarGetInteger(CVAR_ENHANCEMENT("DisableLinkSwordTrail"), 0)) {
            FeedTrail(play, player, hand, bladePath, haveEff ? bladeN : 0, tipLocal0, baseLocal0);
        }
    } else {
        player->meleeWeaponInfo[0].active = 0;
        player->meleeWeaponInfo[1].active = 0;
        player->meleeWeaponInfo[2].active = 0;
    }

    // ---- 5. Swept damage quads while HOT ----
    // With the sim active the quads come from the SIMULATED blade's path — a blade stopped at a
    // wall registers nothing along the hand's ghost trajectory. Without the sim (inertia off),
    // the intent blade is reconstructed along the raw hand path as before.
    sQuadsUsed = 0;

    RegisterPendingStrikes(play);

    if (sTier == TIER_HOT) {
        const s32 held = Player_GetMeleeWeaponHeld(player);
        const int row = Player_HoldsBrokenKnife(player) ? 1 : (int)held - 1;
        const uint32_t dmgFlags = kDmgFlags[(row < 0 || row > 3) ? 1 : row][sTickTipSpeed >= heavySpeed ? 1 : 0];

        if (bladeN >= 2) {
            // Sweep quads between consecutive sim blade lines (evenly picked, max 5)...
            int segments = bladeN - 1;
            if (segments > kMaxQuads - 1) {
                segments = kMaxQuads - 1;
            }
            int prevIdx = 0;
            for (int k = 1; k <= segments; k++) {
                const int idx = (k * (bladeN - 1)) / segments;
                if (idx <= prevIdx) {
                    continue;
                }
                const Vec3f rootA = { bladePath[prevIdx].root[0], bladePath[prevIdx].root[1],
                                      bladePath[prevIdx].root[2] };
                const Vec3f tipA = { bladePath[prevIdx].tip[0], bladePath[prevIdx].tip[1],
                                     bladePath[prevIdx].tip[2] };
                const Vec3f rootB = { bladePath[idx].root[0], bladePath[idx].root[1], bladePath[idx].root[2] };
                const Vec3f tipB = { bladePath[idx].tip[0], bladePath[idx].tip[1], bladePath[idx].tip[2] };
                RegisterQuad(play, dmgFlags, rootB, tipB, rootA, tipA);
                prevIdx = idx;
            }
            // ...plus the width cross-section at the current pose, which is what gives a
            // straight STAB a quad with real area.
            RegisterQuad(play, dmgFlags, crossBaseA, crossTipA, crossBaseB, crossTipB);
        } else if (!inertiaOn && haveEff && path.count >= 2) {
            // The intent blade (single line) re-applied along the tick's hand samples.
            int segments = CVarGetInteger("gVrPhysSubQuads", 3);
            if (segments < 1) {
                segments = 1;
            }
            if (segments > 5) {
                segments = 5;
            }
            if (segments > path.count - 1) {
                segments = path.count - 1;
            }

            int prevIdx = 0;
            for (int k = 1; k <= segments; k++) {
                const int idx = (k * (path.count - 1)) / segments;
                if (idx <= prevIdx) {
                    continue;
                }
                const VrHandSample& a = path.samples[prevIdx];
                const VrHandSample& b = path.samples[idx];
                const Vec3f tipA = AtSample(a, tipLocal0);
                const Vec3f baseA = AtSample(a, baseLocal0);
                const Vec3f tipB = AtSample(b, tipLocal0);
                const Vec3f baseB = AtSample(b, baseLocal0);
                RegisterQuad(play, dmgFlags, baseB, tipB, baseA, tipA);
                prevIdx = idx;
            }
            RegisterQuad(play, dmgFlags, crossBaseA, crossTipA, crossBaseB, crossTipB);
        } else if (sHaveBladePrev) {
            // No usable path this tick: one sweep from last frame's blade, plus the stab quad.
            RegisterQuad(play, dmgFlags, base0, tip0, sPrevBase[0], sPrevTip[0]);
            RegisterQuad(play, dmgFlags, crossBaseA, crossTipA, crossBaseB, crossTipB);
        }
    }

    // Latch the current blade line for the sparse-tick fallback.
    sPrevTip[0] = tip0;
    sPrevBase[0] = base0;
    sHaveBladePrev = true;
}

namespace VrCombat {

void Swing_OnPlayerUpdate(PlayState* play, Player* player) {
    // Pacifist testing mode: refresh every enemy's freezeTimer so their updates never run —
    // no AI, no detection, no attacks, animation paused. Uncheck and they thaw in a tick.
    if (CVarGetInteger("gVrPhysPacifist", 0)) {
        for (Actor* a = play->actorCtx.actorLists[ACTORCAT_ENEMY].head; a != NULL; a = a->next) {
            a->freezeTimer = 2;
        }
    }

    // New puppet tick: the contact gather re-marks live puppets during this tick's draw, and
    // stale ones (actor gone or out of range) stop being solved or warped.
    sPuppetTick++;
    for (Puppet& p : sPuppets) {
        if (p.actor != nullptr && p.lastSeenTick + 3 < sPuppetTick) {
            memset(&p, 0, sizeof(p));
        }
    }

    // Apply queued impulses. We're inside the player's update: the damage pass (which zeroes
    // displacement) already ran this tick, and enemies update after the player — so writes
    // here land this very tick. Victims are validated against the live actor lists first; a
    // pointer that isn't there anymore (died from the hit that queued this) is dropped.
    for (int i = 0; i < sImpulseCount; i++) {
        Actor* target = sImpulses[i].actor;
        if (ActorIsLive(play, target)) {
            target->colChkInfo.displacement.x += sImpulses[i].d.x;
            target->colChkInfo.displacement.z += sImpulses[i].d.z;
        }
    }
    sImpulseCount = 0;

    // Weapon switched to something physical combat doesn't cover: release the sim slot so the
    // hand pose reverts to the raw controller (the draw-time feed only runs for covered weapons).
    if (!VrCombat_MeleeCovered(player)) {
        VR_PhysSetObject(VR_PHYS_SLOT_WEAPON, NULL);
        VR_PhysSetContactPrims(NULL, 0);
    }

    // The hammer drains its simulated-head path HERE (see sHammerPath) and runs its grip tick.
    const bool hammer = VrCombat_MeleeCovered(player) && Player_GetMeleeWeaponHeld(player) == 5;
    if (hammer) {
        sHammerPathCount = VR_PhysGetBladePath(VR_PHYS_SLOT_WEAPON, sHammerPath, 16);
    } else {
        sHammerPathCount = 0;
    }
    if (sPoundCooldown > 0) {
        sPoundCooldown--;
    }
    VrCombat::Hammer_Tick(play, player);

    // Contact events from the sim: impact SFX + sparks at the contact point (the matching
    // haptics already fired VR-side with zero latency).
    VrContactEvent events[8];
    const int eventCount = VR_PhysDrainEvents(events, 8);
    for (int i = 0; i < eventCount; i++) {
        if (events[i].type != VR_PHYS_EV_CONTACT_BEGIN || events[i].slot != VR_PHYS_SLOT_WEAPON) {
            continue;
        }
        if (hammer) {
            HammerContact(play, player, events[i]);
            continue;
        }
        Vec3f pos = { events[i].pos[0], events[i].pos[1], events[i].pos[2] };

        // Material-correct impact feedback, mirroring the vanilla sword-recoil path
        // (z_player.c func_80842DF4's wall branch): surface material picks wood particles/sound,
        // soft-dirt thud or hard stone clank; hard colliders read their colType; enemy flesh is
        // silent here (the strike pipeline below produces the vanilla hit sound and effects).
        // Faint brushes (slow approach) stay quiet.
        const int kind = PrimKind(events[i].primId);
        const int detail = PrimDetail(events[i].primId);
        if (events[i].impactMps > 0.4f) {
            if (kind == kPrimKindWall) {
                if (detail == 0xA) {
                    CollisionCheck_SpawnShieldParticlesWood(play, &pos, &player->actor.projectedPos);
                } else {
                    CollisionCheck_SpawnShieldParticles(play, &pos);
                    Player_PlaySfx(&player->actor,
                                   detail == 0xB ? NA_SE_IT_WALL_HIT_SOFT : NA_SE_IT_WALL_HIT_HARD);
                }
            } else if (kind == kPrimKindHard) {
                if (detail == COLTYPE_WOOD || detail == COLTYPE_TREE) {
                    CollisionCheck_SpawnShieldParticlesWood(play, &pos, &player->actor.projectedPos);
                } else {
                    // Metal/hard armor: the metal-shield spark burst with its own clang.
                    CollisionCheck_SpawnShieldParticlesMetalSound(play, &pos, &player->actor.projectedPos);
                }
            }
        }

        // A damaging swing that physically struck something: queue a strike for the next draw.
        // Damage class from the swing that carried the impact.
        if (sTier == TIER_HOT && sPendingStrikeCount < 4) {
            const s32 held = Player_GetMeleeWeaponHeld(player);
            const int row = Player_HoldsBrokenKnife(player) ? 1 : (int)held - 1;
            const float heavySpeed = CVarGetFloat("gVrPhysHeavySpeed", 8.0f);
            PendingStrike& st = sPendingStrikes[sPendingStrikeCount++];
            st.pos = pos;
            st.normal = { events[i].normal[0], events[i].normal[1], events[i].normal[2] };
            st.dmgFlags = kDmgFlags[(row < 0 || row > 3) ? 1 : row][sTickTipSpeed >= heavySpeed ? 1 : 0];
            st.pierce = false;
            // A stick whacked into the world at attack speed snaps exactly like a vanilla
            // wall hit (func_80842DF4's wall branch -> break). Stick only here: durability
            // for actor hits is consumed once at the quad readback below, and a wall strike
            // whose poked-through quad then hits an actor must not consume twice.
            if (player->heldItemAction == PLAYER_IA_DEKU_STICK) {
                VrCombat_MeleeImpactConsume(play, player);
            }
        }
    }

    if (sQuadsPlay != play) {
        return;
    }
    // Read back last tick's quad results (the AT pass ran at the top of this tick).
    bool hit = false;
    for (int i = 0; i < sQuadsUsed; i++) {
        if (!(sQuads[i].base.atFlags & AT_HIT)) {
            continue;
        }
        hit = true;
        // M3 hit feel. Vanilla colChkInfo.mass MOSTLY doesn't mean weight: it encodes "Link
        // can't shove me by walking into me" (a tiny Stalchild carries MASS_HEAVY), so sword
        // impacts scale by category instead — full effect on enemies/NPCs, none on bosses.
        // The one honored value is MASS_IMMOVABLE, which genuinely means "never displaced".
        Actor* victim = sQuads[i].base.at;
        if (ActorIsLive(play, victim) && victim->category != ACTORCAT_BOSS) {
            Vec3f dir = vsub(victim->world.pos, player->actor.world.pos);
            dir.y = 0.0f;
            const float dl = sqrtf(dir.x * dir.x + dir.z * dir.z);
            if (dl > 1e-3f) {
                dir.x /= dl;
                dir.z /= dl;
                // Micro-push, queued: displacement written now would be zeroed by the damage
                // pass before the victim consumes it — it lands from the player UPDATE instead.
                // NPCs are exempt: their limbs react but they stay planted.
                if (victim->category != ACTORCAT_NPC && victim->colChkInfo.mass != MASS_IMMOVABLE) {
                    float mag = sTickTipSpeed * CVarGetFloat("gVrPhysKnockbackScale", 0.0f) * 1.5f;
                    const float cap = CVarGetFloat("gVrPhysKnockbackCap", 8.0f);
                    if (mag > cap) {
                        mag = cap;
                    }
                    QueueImpulse(victim, dir.x * mag, dir.z * mag);
                }

                // Punching-bag flinch: skeleton caves toward the swing around the impact
                // point and springs back. Anchor = the closest point on the CURRENT blade to
                // the victim's vertical axis — NOT the swept quad's centroid, which spans the
                // whole swing arc and can land outside the flinch falloff entirely (the bug
                // that made hit flinch invisible on fast swings).
                const Vec3f axisA = victim->world.pos;
                const Vec3f axisB = { victim->world.pos.x, victim->world.pos.y + 80.0f,
                                      victim->world.pos.z };
                // Blade endpoints from the last draw's outline snapshot (this readback runs
                // during the player's update, one tick after the swing registered).
                Vec3f bladeBase = axisA;
                Vec3f bladeTip = axisB;
                if (sDebugBladeValid) {
                    bladeBase = { (sDebugBlade[0][0] + sDebugBlade[4][0]) * 0.5f,
                                  (sDebugBlade[0][1] + sDebugBlade[4][1]) * 0.5f,
                                  (sDebugBlade[0][2] + sDebugBlade[4][2]) * 0.5f };
                    bladeTip = { sDebugBlade[2][0], sDebugBlade[2][1], sDebugBlade[2][2] };
                }
                const Vec3f hitPos = ClosestOnSegToSeg(bladeBase, bladeTip, axisA, axisB);
                const Vec3f flinchDir = { dir.x, 0.15f, dir.z }; // a touch of lift reads as impact
                const float speedNorm = sTickTipSpeed / 6.0f;
                const float strength = CVarGetFloat("gVrPhysFlinchAmount", 0.0f) *
                                       (0.5f + (speedNorm > 1.0f ? 1.0f : speedNorm) * 0.5f);
                PuppetHitKick(victim, hitPos, flinchDir, strength);
            }
        }
    }
    if (hit) {
        // Hit feel scales with the swing that landed: a threshold graze taps, a full-arm
        // heavy swing thumps long and hard.
        const float amp = 0.45f + 0.09f * sTickTipSpeed;
        VR_TriggerHaptic(SwordHand(), amp > 1.0f ? 1.0f : amp, 0.0f, 45.0f + 12.0f * sTickTipSpeed);
        const VrCombat::HammerGrip& hammerGrip = VrCombat::Hammer_GetGrip();
        if (hammer && hammerGrip.twoHand && hammerGrip.offHand >= 0) {
            VR_TriggerHaptic(hammerGrip.offHand, amp > 1.0f ? 1.0f : amp, 0.0f, 45.0f + 12.0f * sTickTipSpeed);
        }
        if (sTier == TIER_HOT) {
            sTier = TIER_ARMED; // one strike per swing: re-cross the hit speed to strike again
        }
        // Landed hits consume weapon durability exactly like vanilla func_80842DF4's AT_HIT
        // branch: the Deku stick snaps (half-stick effect, ammo, put away) and the unbroken
        // Giant's Knife wears toward breaking. No-op for the other covered weapons.
        VrCombat_MeleeImpactConsume(play, player);
    }
    for (int i = 0; i < sQuadsUsed; i++) {
        Collider_ResetQuadAT(play, &sQuads[i].base);
    }
    sQuadsUsed = 0;
}

int Swing_GetDebugPuppetLimbs(float* outPos3PerLimb, float* outOffsetMag, int maxLimbs) {
    int n = 0;
    for (const Puppet& p : sPuppets) {
        if (p.actor == nullptr || p.lastSeenTick + 2 < sPuppetTick) {
            continue;
        }
        for (int i = 0; i < kPuppetLimbs && n < maxLimbs; i++) {
            if (!p.posValid[i]) {
                continue;
            }
            outPos3PerLimb[n * 3 + 0] = p.lastPos[i].x;
            outPos3PerLimb[n * 3 + 1] = p.lastPos[i].y;
            outPos3PerLimb[n * 3 + 2] = p.lastPos[i].z;
            const Vec3f& o = p.offset[i];
            outOffsetMag[n] = sqrtf(o.x * o.x + o.y * o.y + o.z * o.z);
            n++;
        }
    }
    return n;
}

int Swing_GetDebugBladeOutline(float* outPts5x3) {
    if (!sDebugBladeValid) {
        return 0;
    }
    memcpy(outPts5x3, sDebugBlade, sizeof(sDebugBlade));
    return 5;
}

int Swing_GetDebugContactPrims(VrContactPrim* out, int maxPrims) {
    const int n = sDebugPrimCount < maxPrims ? sDebugPrimCount : maxPrims;
    memcpy(out, sDebugPrims, sizeof(VrContactPrim) * n);
    return n;
}

int Swing_GetDebugQuads(float* outVerts12PerQuad, int maxQuads) {
    const int n = sQuadsUsed < maxQuads ? sQuadsUsed : maxQuads;
    for (int i = 0; i < n; i++) {
        for (int v = 0; v < 4; v++) {
            outVerts12PerQuad[i * 12 + v * 3 + 0] = sQuads[i].dim.quad[v].x;
            outVerts12PerQuad[i * 12 + v * 3 + 1] = sQuads[i].dim.quad[v].y;
            outVerts12PerQuad[i * 12 + v * 3 + 2] = sQuads[i].dim.quad[v].z;
        }
    }
    return n;
}

void Swing_Deactivate(PlayState* play, Player* player) {
    // Falling edge of VrCombat_Active(): hand the melee state back to vanilla cleanly.
    VR_PhysSetObject(VR_PHYS_SLOT_WEAPON, NULL);
    VR_PhysSetContactPrims(NULL, 0);
    VR_PhysSetMeshRegion(NULL, 0.0f, 0);
    sTier = TIER_IDLE;
    sHaveBladePrev = false;
    sPendingStrikeCount = 0;
    sDebugBladeValid = false;
    sHammerPathCount = 0;
    sPoundCooldown = 0;
    VrCombat::Hammer_Reset();
    // Drop the puppets: the skelanime warp hooks keep running while combat is off and match
    // by actor pointer, so a limb displaced at the moment of deactivation would otherwise
    // stay warped forever (nothing solves or decays the offsets anymore).
    memset(sPuppets, 0, sizeof(sPuppets));
    sImpulseCount = 0;
    player->meleeWeaponState = 0;
    player->meleeWeaponInfo[0].active = 0;
    player->meleeWeaponInfo[1].active = 0;
    player->meleeWeaponInfo[2].active = 0;
    if (sQuadsPlay == play) {
        for (int i = 0; i < kMaxQuads; i++) {
            Collider_ResetQuadAT(play, &sQuads[i].base);
        }
    }
    sQuadsUsed = 0;
}

} // namespace VrCombat

// ---- Hit-flinch limb warp (called from z_skelanime.c around every limb matrix conversion) ----
// Begin: if `actorArg` has a live flinch, offset the current matrix's translation by the hit
// direction, scaled by the envelope and by how close this limb is to the impact point. End:
// restore, so child limbs build from the clean hierarchy and get their own falloff-weighted
// offsets — the skeleton caves locally around the hit and springs back.
extern "C" void VrCombat_FlinchWarpBegin(PlayState* play, void* actorArg, int32_t limbIndex) {
    (void)play;
    if (sWarpActive || actorArg == NULL) {
        return;
    }
    Puppet* p = FindPuppet((Actor*)actorArg); // pointer compare only
    if (p == nullptr || limbIndex < 0 || limbIndex >= kPuppetLimbs) {
        return;
    }
    MtxF* cm = Matrix_GetCurrent();
    // Record the clean animated pose BEFORE offsetting: the contact solve must see where the
    // animation put the limb, not where we displaced it (no feedback loops).
    p->lastPos[limbIndex] = { cm->xw, cm->yw, cm->zw };
    p->posValid[limbIndex] = 1;
    const Vec3f& off = p->offset[limbIndex];
    const Vec3f& rot = p->rotOff[limbIndex];
    const float offMag2 = off.x * off.x + off.y * off.y + off.z * off.z;
    const float rotMag2 = rot.x * rot.x + rot.y * rot.y + rot.z * rot.z;
    if (offMag2 < 0.0001f && rotMag2 < 0.00001f) {
        return;
    }
    sWarpSavedMtx = *cm;
    sWarpActive = true;
    // Rotate the limb about its own joint (world-axis rotation of the basis columns; the
    // joint position stays put), then translate.
    if (rotMag2 >= 0.00001f) {
        const Vec3f bx = RotateAxisAngle({ cm->xx, cm->yx, cm->zx }, rot);
        const Vec3f by = RotateAxisAngle({ cm->xy, cm->yy, cm->zy }, rot);
        const Vec3f bz = RotateAxisAngle({ cm->xz, cm->yz, cm->zz }, rot);
        cm->xx = bx.x;
        cm->yx = bx.y;
        cm->zx = bx.z;
        cm->xy = by.x;
        cm->yy = by.y;
        cm->zy = by.z;
        cm->xz = bz.x;
        cm->yz = bz.y;
        cm->zz = bz.z;
    }
    cm->xw += off.x;
    cm->yw += off.y;
    cm->zw += off.z;
}

extern "C" void VrCombat_FlinchWarpEnd(void) {
    if (!sWarpActive) {
        return;
    }
    *Matrix_GetCurrent() = sWarpSavedMtx;
    sWarpActive = false;
}

// The live head of Link's swing trail for this frame (see TrailHead): the blure it belongs to, the
// hand whose matrix drew the blade, and the edge recorded this tick (world units). False unless the
// trail took an edge during this frame's draw.
extern "C" bool VrCombat_TrailLiveEdge(const void* blure, int32_t* hand, float tip[3], float base[3]) {
    if (!sTrailHead.valid || blure != sTrailHead.blure || gPlayState == nullptr ||
        sTrailHead.frame != (uint32_t)gPlayState->state.frames || !CVarGetInteger("gVrTrailLiveHead", 1)) {
        return false;
    }
    *hand = sTrailHead.hand;
    tip[0] = sTrailHead.tip.x;
    tip[1] = sTrailHead.tip.y;
    tip[2] = sTrailHead.tip.z;
    base[0] = sTrailHead.base.x;
    base[1] = sTrailHead.base.y;
    base[2] = sTrailHead.base.z;
    return true;
}
