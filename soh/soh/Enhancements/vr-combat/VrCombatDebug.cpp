#include "VrCombat.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"

#include <cmath>
#include <vector>
#include <vr_interface.h>

#include "z64.h"
#include "variables.h"
#include "functions.h"
#include "macros.h"

extern "C" {
extern PlayState* gPlayState;
}

// In-world debug overlay for VR physical combat: a velocity arrow per hand (color = swing-speed
// tier) plus the tick's drained hand-path samples, drawn into the world display list (colViewer
// pattern: build Gfx on OnPlayDrawEnd, emit into POLY_XLU_DISP) so everything sits at real depth
// in both eyes. Positions come from the 20 Hz tick snapshot, so markers trail the live-rendered
// hands slightly — fine for a diagnostic.

#define qs105(n) ((int16_t)((n) * 0x0020))
#define gdSPDefVtxN(x, y, z, s, t, nx, ny, nz, ca)                                            \
    {                                                                                         \
        .n = {.ob = { x, y, z }, .tc = { qs105(s), qs105(t) }, .n = { nx, ny, nz }, .a = ca } \
    }
#define G_CC_PRIMITIVE_ENVA 0, 0, 0, PRIMITIVE, 0, 0, 0, ENVIRONMENT

namespace {

// Fixed capacities, reserved once: gsSPVertex bakes raw pointers into the vertex vector, so it
// must NEVER reallocate mid-build. PushQuad refuses to grow past capacity instead.
constexpr size_t kMaxQuads = 192;
std::vector<Gfx> sDl;
std::vector<Vtx> sVtx;

// Visual gain: game units of arrow length per m/s of hand speed. ~35 units/m world scale means
// this draws a 1 m/s swing as ~0.6 m of arrow — long enough to read, short enough to not clutter.
constexpr float kUnitsPerMps = 22.0f;

inline Vec3f vsub(const Vec3f& a, const Vec3f& b) {
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}
inline Vec3f vadd(const Vec3f& a, const Vec3f& b) {
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}
inline Vec3f vscale(const Vec3f& a, float s) {
    return { a.x * s, a.y * s, a.z * s };
}
inline Vec3f vcross(const Vec3f& a, const Vec3f& b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline float vlen(const Vec3f& a) {
    return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
}
inline Vec3f vnorm(const Vec3f& a) {
    const float m = vlen(a);
    return (m > 1e-6f) ? vscale(a, 1.0f / m) : Vec3f{ 0.0f, 1.0f, 0.0f };
}

void PushQuad(const Vec3f& a, const Vec3f& b, const Vec3f& c, const Vec3f& d) {
    if (sVtx.size() + 4 > sVtx.capacity()) {
        return; // never reallocate — baked gsSPVertex pointers would dangle
    }
    sVtx.push_back(gdSPDefVtxN((short)a.x, (short)a.y, (short)a.z, 0, 0, 0, 127, 0, 0xFF));
    sVtx.push_back(gdSPDefVtxN((short)b.x, (short)b.y, (short)b.z, 0, 0, 0, 127, 0, 0xFF));
    sVtx.push_back(gdSPDefVtxN((short)c.x, (short)c.y, (short)c.z, 0, 0, 0, 127, 0, 0xFF));
    sVtx.push_back(gdSPDefVtxN((short)d.x, (short)d.y, (short)d.z, 0, 0, 0, 127, 0, 0xFF));
    sDl.push_back(gsSPVertex((uintptr_t)&sVtx.at(sVtx.size() - 4), 4, 0));
    sDl.push_back(gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0));
}

// Camera-facing tapered ribbon from `from` to `to` (an arrow without a separate head: the taper
// shows direction).
void PushRibbon(const Vec3f& from, const Vec3f& to, const Vec3f& camEye, float halfWidth) {
    const Vec3f dir = vsub(to, from);
    if (vlen(dir) < 0.5f) {
        return;
    }
    const Vec3f side = vnorm(vcross(vnorm(dir), vnorm(vsub(camEye, from))));
    const Vec3f s0 = vscale(side, halfWidth);
    const Vec3f s1 = vscale(side, halfWidth * 0.25f);
    PushQuad(vsub(from, s0), vadd(from, s0), vadd(to, s1), vsub(to, s1));
}

// Small camera-facing diamond marker.
void PushMarker(const Vec3f& at, const Vec3f& camEye, float radius) {
    const Vec3f toCam = vnorm(vsub(camEye, at));
    // Any two axes perpendicular to the view direction will do.
    Vec3f up = { 0.0f, 1.0f, 0.0f };
    Vec3f right = vnorm(vcross(up, toCam));
    up = vnorm(vcross(toCam, right));
    PushQuad(vadd(at, vscale(right, -radius)), vadd(at, vscale(up, radius)), vadd(at, vscale(right, radius)),
             vadd(at, vscale(up, -radius)));
}

void PushSetupGfx() {
    const uint32_t rm = Z_CMP | IM_RD | CVG_DST_FULL | FORCE_BL | ZMODE_XLU;
    const uint32_t blc1 = GBL_c1(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA);
    const uint32_t blc2 = GBL_c2(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA);
    sDl.push_back(gsSPTexture(0, 0, 0, G_TX_RENDERTILE, G_OFF));
    sDl.push_back(gsDPSetCycleType(G_CYC_1CYCLE));
    sDl.push_back(gsDPSetRenderMode(rm | blc1, rm | blc2));
    sDl.push_back(gsDPSetCombineMode(G_CC_PRIMITIVE_ENVA, G_CC_PRIMITIVE_ENVA));
    sDl.push_back(gsDPSetEnvColor(255, 255, 255, 200));
    sDl.push_back(gsSPClearGeometryMode(G_CULL_BOTH | G_LIGHTING));
    sDl.push_back(gsSPMatrix(&gMtxClear, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH));
}

} // namespace

// extern "C" linkage is load-bearing: OPEN_DISPS/CLOSE_DISPS re-declare the FrameInterpolation_
// Record* functions at BLOCK scope, and a block-scope declaration inherits the enclosing
// function's language linkage — inside a C++ function it would mangle and fail to link against
// the C definitions. colViewer's DrawColViewer is extern "C" for the same reason.
extern "C" void VrCombat_DrawDebugOverlay(void) {
    if (gPlayState == nullptr || !VR_IsInitialized() || !VR_GetFirstPerson()) {
        return;
    }

    sDl.clear();
    sVtx.clear();
    if (sVtx.capacity() < kMaxQuads * 4) {
        sVtx.reserve(kMaxQuads * 4);
        sDl.reserve(kMaxQuads * 2 + 16);
    }

    PushSetupGfx();

    // The overlay draws world-space geometry ON the blade (outline, contact markers) — it must
    // never be harvested as collision geometry or the blade collides with its own debug draw.
    sDl.push_back(gsSPVrPhysMask(1));

    const Vec3f camEye = gPlayState->view.eye;

    for (int hand = 0; hand < 2; hand++) {
        float pos[3];
        float quat[4];
        float lin[3];
        float ang[3];
        const bool posed = VR_GetHandPose(hand, pos, quat);
        const bool haveVel = VR_GetHandVelocity(hand, lin, ang);
        if (!posed) {
            continue;
        }
        const Vec3f handPos = { pos[0], pos[1], pos[2] };

        // Tick path breadcrumbs (validates the headset-rate history pipeline end to end).
        sDl.push_back(gsDPSetPrimColor(0, 0, 90, 170, 255, 255));
        const VrCombat::TickPath& path = VrCombat::GetTickPath(hand);
        for (int i = 0; i < path.count; i++) {
            const VrHandSample& s = path.samples[i];
            PushMarker({ s.pos[0], s.pos[1], s.pos[2] }, camEye, 0.9f);
        }

        // Velocity arrow, colored by the swing tiers (hand speed vs the real threshold CVars —
        // the tracker gates on blade TIP speed, which runs higher than the hand on arm swings,
        // so treat the colors as a floor, not the exact gate).
        if (haveVel) {
            const float kPreviewArmSpeed = CVarGetFloat("gVrPhysArmSpeed", 2.0f);
            const float kPreviewHitSpeed = CVarGetFloat("gVrPhysHitSpeed", 5.0f);
            const float speed = sqrtf(lin[0] * lin[0] + lin[1] * lin[1] + lin[2] * lin[2]);
            if (speed >= kPreviewHitSpeed) {
                sDl.push_back(gsDPSetPrimColor(0, 0, 255, 60, 60, 255));
            } else if (speed >= kPreviewArmSpeed) {
                sDl.push_back(gsDPSetPrimColor(0, 0, 255, 220, 40, 255));
            } else {
                sDl.push_back(gsDPSetPrimColor(0, 0, 60, 255, 90, 255));
            }
            const Vec3f tip = vadd(handPos, vscale({ lin[0], lin[1], lin[2] }, kUnitsPerMps));
            PushRibbon(handPos, tip, camEye, 1.6f);
        }

        // Hand anchor marker.
        sDl.push_back(gsDPSetPrimColor(0, 0, 255, 255, 255, 255));
        PushMarker(handPos, camEye, 1.3f);
    }

    // The blade's contact-primitive set: everything the sword physically collides with this
    // tick. Blue-gray = level geometry, orange = hard colliders (armor/shields), green = enemy
    // bodies. Planes draw as patches at the probe point.
    VrContactPrim prims[VR_PHYS_MAX_CONTACT_PRIMS];
    const int primCount = VrCombat::Swing_GetDebugContactPrims(prims, VR_PHYS_MAX_CONTACT_PRIMS);
    for (int i = 0; i < primCount; i++) {
        const int kind = prims[i].id >> 12;
        if (kind == 2) {
            sDl.push_back(gsDPSetPrimColor(0, 0, 255, 150, 40, 255));
        } else if (kind == 3) {
            sDl.push_back(gsDPSetPrimColor(0, 0, 110, 255, 110, 255));
        } else {
            sDl.push_back(gsDPSetPrimColor(0, 0, 150, 160, 255, 255));
        }
        const Vec3f a = { prims[i].a[0], prims[i].a[1], prims[i].a[2] };
        const Vec3f b = { prims[i].b[0], prims[i].b[1], prims[i].b[2] };
        if (prims[i].type == VR_PHYS_PRIM_TRI) {
            // The actual level polygon the blade collides with (bounded — what you see is
            // exactly where the constraint ends).
            const Vec3f c = { prims[i].c[0], prims[i].c[1], prims[i].c[2] };
            PushQuad(a, b, c, c);
        } else if (prims[i].type == VR_PHYS_PRIM_PLANE) {
            // b is the unit normal; build an in-plane patch around the probe point.
            Vec3f up = (b.y > 0.9f || b.y < -0.9f) ? Vec3f{ 1.0f, 0.0f, 0.0f } : Vec3f{ 0.0f, 1.0f, 0.0f };
            const Vec3f t1 = vnorm(vcross(b, up));
            const Vec3f t2 = vcross(b, t1);
            const Vec3f e1 = vscale(t1, 16.0f);
            const Vec3f e2 = vscale(t2, 16.0f);
            PushQuad(vsub(vsub(a, e1), e2), vsub(vadd(a, e1), e2), vadd(vadd(a, e1), e2), vadd(vsub(a, e1), e2));
        } else if (prims[i].type == VR_PHYS_PRIM_CAPSULE) {
            PushRibbon(a, b, camEye, prims[i].radius);
        } else {
            PushMarker(a, camEye, prims[i].radius);
        }
    }

    // Visual-mesh mode: the harvested triangles the solver is actually colliding against
    // (magenta wireframe) — the ground truth of what the blade "sees" this frame.
    {
        float meshTris[32 * 9];
        const int meshTriCount = VR_PhysGetMeshDebugTris(meshTris, 32);
        if (meshTriCount > 0) {
            sDl.push_back(gsDPSetPrimColor(0, 0, 235, 80, 235, 255));
            for (int i = 0; i < meshTriCount; i++) {
                const float* t = &meshTris[i * 9];
                const Vec3f a = { t[0], t[1], t[2] };
                const Vec3f b = { t[3], t[4], t[5] };
                const Vec3f c = { t[6], t[7], t[8] };
                PushRibbon(a, b, camEye, 0.5f);
                PushRibbon(b, c, camEye, 0.5f);
                PushRibbon(c, a, camEye, 0.5f);
            }
        }
    }

    // Puppet-tracked limbs: white dots = limbs the blade can manipulate; a dot turns red and
    // grows while its limb is being displaced. If an actor shows no dots, it isn't tracked.
    {
        float limbPos[64 * 3];
        float limbOff[64];
        const int limbCount = VrCombat::Swing_GetDebugPuppetLimbs(limbPos, limbOff, 64);
        for (int i = 0; i < limbCount; i++) {
            const bool active = limbOff[i] > 0.5f;
            if (active) {
                sDl.push_back(gsDPSetPrimColor(0, 0, 255, 70, 70, 255));
            } else {
                sDl.push_back(gsDPSetPrimColor(0, 0, 235, 235, 235, 255));
            }
            PushMarker({ limbPos[i * 3], limbPos[i * 3 + 1], limbPos[i * 3 + 2] }, camEye,
                       active ? 1.6f : 0.8f);
        }
    }

    // The physical blade rectangle (cyan outline): the exact collider the solver sweeps.
    float bladePts[5 * 3];
    if (VrCombat::Swing_GetDebugBladeOutline(bladePts) == 5) {
        sDl.push_back(gsDPSetPrimColor(0, 0, 60, 235, 255, 255));
        for (int i = 0; i < 5; i++) {
            const int j = (i + 1) % 5;
            PushRibbon({ bladePts[i * 3], bladePts[i * 3 + 1], bladePts[i * 3 + 2] },
                       { bladePts[j * 3], bladePts[j * 3 + 1], bladePts[j * 3 + 2] }, camEye, 0.6f);
        }
    }

    // Active blade contacts from the sim (yellow: point + normal), the ground truth of what the
    // solver is actually pressing against right now.
    float contactPos[4 * 3];
    float contactNrm[4 * 3];
    const int contactCount = VR_PhysGetContacts(VR_PHYS_SLOT_WEAPON, contactPos, contactNrm, 4);
    if (contactCount > 0) {
        sDl.push_back(gsDPSetPrimColor(0, 0, 255, 235, 40, 255));
        for (int i = 0; i < contactCount; i++) {
            const Vec3f cpos = { contactPos[i * 3], contactPos[i * 3 + 1], contactPos[i * 3 + 2] };
            const Vec3f cend = { cpos.x + contactNrm[i * 3] * 9.0f, cpos.y + contactNrm[i * 3 + 1] * 9.0f,
                                 cpos.z + contactNrm[i * 3 + 2] * 9.0f };
            PushMarker(cpos, camEye, 1.2f);
            PushRibbon(cpos, cend, camEye, 0.8f);
        }
    }

    // This draw's registered damage quads (sweep + strike), in red.
    float quadVerts[6 * 12];
    const int quadCount = VrCombat::Swing_GetDebugQuads(quadVerts, 6);
    if (quadCount > 0) {
        sDl.push_back(gsDPSetPrimColor(0, 0, 255, 60, 60, 255));
        for (int i = 0; i < quadCount; i++) {
            const float* q = &quadVerts[i * 12];
            // ColliderQuad vertex order: draw as 0,2,3,1 (the colViewer convention).
            PushQuad({ q[0], q[1], q[2] }, { q[6], q[7], q[8] }, { q[9], q[10], q[11] }, { q[3], q[4], q[5] });
        }
    }

    // The shield's BLOCK collider (cyan): the exact quad enemy attacks are judged against —
    // deliberately INSET from the visible shield by the Shield Collider Scale slider, so the
    // gap between the steel you see and the cyan you see is exactly the rim that doesn't count.
    {
        Player* player = GET_PLAYER(gPlayState);
        if (player != NULL && VrCombat_ShieldHeld(player)) {
            const Vec3f* v = player->shieldQuad.dim.quad;
            sDl.push_back(gsDPSetPrimColor(0, 0, 60, 220, 255, 255));
            PushQuad(v[0], v[2], v[3], v[1]);
        }
    }

    sDl.push_back(gsSPVrPhysMask(0));
    sDl.push_back(gsSPEndDisplayList());

    OPEN_DISPS(gPlayState->state.gfxCtx);
    gSPDisplayList(POLY_XLU_DISP++, sDl.data());
    CLOSE_DISPS(gPlayState->state.gfxCtx);
}

static void RegisterVrCombatDebug() {
    COND_HOOK(OnPlayDrawEnd, CVarGetInteger("gVrPhysCombatDebug", 0), VrCombat_DrawDebugOverlay);
}

static RegisterShipInitFunc initFunc(RegisterVrCombatDebug, { "gVrPhysCombatDebug" });
