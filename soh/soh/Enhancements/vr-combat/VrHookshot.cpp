extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
extern PlayState* gPlayState;
}
#include "VrCombat.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/frame_interpolation.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <vr_interface.h>
#include <cmath>
#include <cstring>

// Hookshot / Longshot (VR first person). User spec (September 30, 2026): "hookshot is basically
// just a point and shoot. let's just make sure that it's clear what direction we're shooting.
// maybe add an option to have a laser pointer in the direction that you're pointing the
// hookshot. and also make sure the hookshot is in the user's dominant hand."
//
// Dominant hand. Vanilla holds the hookshot in Link's RIGHT hand model (R_HAND limb), and motion
// hands drive R_HAND from the OFF-hand controller (the bow/shield hand). While the hookshot model
// is out (gVrHookshotSwordHand), the limb override in z_player_lib.c swaps the two limbs: R_HAND
// (hookshot) rides the sword-hand controller and L_HAND (open hand) the other. Each limb then sits
// on its own side, so neither hand is mirrored — a right-hand model on the right controller, the
// same picture the mirrored sword hand gives. Everything that asks "which controller holds the
// hookshot" asks VrHookshot_RightLimbHand: the aim ray (Player_VrAimHeldProjectile), the trigger
// mirror (VrItemSelect's HeldItemVrHand) and classic-mode fire (VrCombat's AimHand).
//
// Direction. The hook flies from its world.pos along world.rot, which Player_VrAimHeldProjectile
// writes every draw and ArmsHook reads verbatim at the shot: by default (gVrHookshotBarrelAim)
// the vanilla in-hand transform — seated in the hookshot model's barrel, pointing along it, rolled
// with it — otherwise the controller's aim ray; the hookshot trim sliders apply on top. While the
// hook is idle in the hand, ArmsHook_Draw welds it (and its chain stub) to the live hand like the
// laser, so it no longer trails the hookshot. Both aids trace the hook's exact line:
//  - the vanilla reticle (while the trigger holds the aim) is re-framed onto it in z_player_lib.c,
//    so SoH's hookshot reticle colors still apply;
//  - the laser (gVrHookshotLaser) is drawn here whenever the hook is idle in the hand: a thin beam
//    out to the first surface within the hook's reach (the vanilla reticle's range and reach
//    cheat), GREEN when that surface takes the hook, RED when the hook would bounce off, fading
//    out at full reach when nothing is in range. A dot marks the surface.
// The laser is registered as a CHILD of the live hand matrix (the bowstring technique), so it
// stays welded to the controller at headset rate; only its length and color update at 20 Hz.

namespace {

int SwordHand() {
    return CVarGetInteger("gVrLeftHanded", 0) ? VR_HAND_LEFT : VR_HAND_RIGHT;
}

// This frame's aim line, noted by z_player_lib.c right after it aimed the idle hook.
struct AimNote {
    s32 frame = -1;
    int hand = 0;
    bool hasHandMtx = false;
    MtxF handMtx;  // the 20 Hz hand-limb snapshot the laser's hand-local transform is taken against
    Vec3f origin;  // the hook's world.pos
    Vec3s rot;     // the hook's world.rot (flight direction)
    float range = 0.0f; // world units
    const Actor* hook = nullptr; // the idle hook this line belongs to (WeldIdleHook)
};
AimNote sNote;

constexpr int kLaserMaxGfx = 32;
Gfx sLaserDl[kLaserMaxGfx];
Vtx sLaserVtx[12];

Vtx LaserVtx(float x, float y, float z, u8 r, u8 g, u8 b, u8 a) {
    Vtx v = {};
    v.v.ob[0] = (s16)lroundf(x);
    v.v.ob[1] = (s16)lroundf(y);
    v.v.ob[2] = (s16)lroundf(z);
    v.v.cn[0] = r;
    v.v.cn[1] = g;
    v.v.cn[2] = b;
    v.v.cn[3] = a;
    return v;
}

} // namespace

extern "C" bool VrHookshot_InSwordHand(Player* player) {
    if (player == nullptr || gPlayState == nullptr || player != GET_PLAYER(gPlayState)) {
        return false;
    }
    if (!CVarGetInteger("gVrHookshotSwordHand", 1) || !CVarGetInteger("gVrMotionHands", 1)) {
        return false;
    }
    if (!VR_IsInitialized() || !VR_GetFirstPerson()) {
        return false;
    }
    // The model group, not heldItemAction: this is exactly when the hookshot MODEL is in the
    // R_HAND limb, so the swap can never move some other right-hand item (bow, ocarina, shield).
    return player->rightHandType == PLAYER_MODELTYPE_RH_HOOKSHOT;
}

extern "C" int32_t VrHookshot_RightLimbHand(Player* player) {
    const int swordHand = SwordHand();
    return VrHookshot_InSwordHand(player) ? swordHand : (swordHand ^ 1);
}

extern "C" void VrHookshot_TrimAimRay(int32_t vrHand, float* pos3, float* dir3) {
    const float kDeg = 3.14159265358979323846f / 180.0f;
    const float mirror = (vrHand == VR_HAND_LEFT) ? -1.0f : 1.0f;
    const float yaw = CVarGetFloat("gVrHookshotAimYaw", 0.0f) * mirror * kDeg;
    const float pitch = CVarGetFloat("gVrHookshotAimPitch", 0.0f) * kDeg;
    (void)pos3; // the launch point is the barrel (model space); only the direction is trimmed
    if (yaw == 0.0f && pitch == 0.0f) {
        return;
    }

    float fwd[3] = { dir3[0], dir3[1], dir3[2] };
    // The controller's own right axis carries its roll (the aim pose is the grip pose turned
    // about that axis), made exactly perpendicular to the ray; up = right x forward.
    float right[3] = { 1.0f, 0.0f, 0.0f };
    float handPos[3];
    float q[4];
    if (VR_GetHandPose(vrHand, handPos, q)) {
        // Rotate local +X by the quaternion (x, y, z, w).
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        right[0] = 1.0f - 2.0f * (y * y + z * z);
        right[1] = 2.0f * (x * y + w * z);
        right[2] = 2.0f * (x * z - w * y);
    }
    const float along = right[0] * fwd[0] + right[1] * fwd[1] + right[2] * fwd[2];
    for (int i = 0; i < 3; i++) {
        right[i] -= fwd[i] * along;
    }
    float len = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (len < 1e-4f) {
        // Degenerate (pointing along the controller's X): fall back to a horizontal right.
        right[0] = -fwd[2];
        right[1] = 0.0f;
        right[2] = fwd[0];
        len = sqrtf(right[0] * right[0] + right[2] * right[2]);
        if (len < 1e-4f) {
            right[0] = 1.0f;
            len = 1.0f;
        }
    }
    for (int i = 0; i < 3; i++) {
        right[i] /= len;
    }
    const float up[3] = { right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
                          right[0] * fwd[1] - right[1] * fwd[0] };

    // Direction: yaw about up (positive = right), then pitch about the turned right axis
    // (positive = up). Up stays perpendicular to the turned pair, so the pitch is exact.
    for (int i = 0; i < 3; i++) {
        const float d1 = fwd[i] * cosf(yaw) + right[i] * sinf(yaw);
        dir3[i] = d1 * cosf(pitch) + up[i] * sinf(pitch);
    }
}

extern "C" void VrHookshot_NoteAim(Player* player, Actor* hook, int32_t vrHand, const float* handMtxF16,
                                   float rangeUnits) {
    if (gPlayState == nullptr || player == nullptr || hook == nullptr) {
        return;
    }
    sNote.frame = (s32)gPlayState->state.frames;
    sNote.hand = vrHand;
    sNote.hasHandMtx = handMtxF16 != nullptr;
    if (handMtxF16 != nullptr) {
        memcpy(sNote.handMtx.mf, handMtxF16, sizeof(sNote.handMtx.mf));
    }
    sNote.origin = hook->world.pos;
    sNote.rot = hook->world.rot;
    sNote.range = rangeUnits;
    sNote.hook = hook;
}

extern "C" bool VrHookshot_IdleWelded(Actor* hook) {
    return gPlayState != nullptr && hook != nullptr && hook == sNote.hook &&
           sNote.frame == (s32)gPlayState->state.frames && sNote.hasHandMtx;
}

extern "C" void VrHookshot_WeldIdleHook(Actor* hook, const void* mtx) {
    // Only the hook this frame's aim was noted for (idle in the hand, the aim override ran and
    // the hand's 20 Hz snapshot exists): the hand-LOCAL part of the matrix being drawn, against
    // that same snapshot, re-composed with the live hand pose per eye by the interpreter.
    if (mtx == nullptr || !VrHookshot_IdleWelded(hook)) {
        return;
    }
    MtxF cur;
    MtxF inv;
    MtxF local;
    Matrix_Get(&cur);
    SkinMatrix_Invert(&sNote.handMtx, &inv);
    SkinMatrix_MtxFMtxFMult(&inv, &cur, &local);
    VR_RegisterHandChildMatrix(mtx, sNote.hand, &local.mf[0][0]);
}

extern "C" bool VrHookshot_BarrelAim(void) {
    return CVarGetInteger("gVrHookshotBarrelAim", 1) != 0;
}

// OnPlayDrawEnd. extern "C" linkage is load-bearing for the block-scope FrameInterpolation
// declarations inside OPEN_DISPS (see VrItemSelect_Draw).
extern "C" void VrHookshot_DrawLaser(void) {
    if (gPlayState == nullptr || !CVarGetInteger("gVrHookshotLaser", 1)) {
        return;
    }
    // Only a line noted THIS frame: the hook left the hand (heldActor cleared at the shot), the
    // hookshot was put away, or the aim override stood down — no stale beam.
    if (sNote.frame != (s32)gPlayState->state.frames || sNote.range <= 1.0f) {
        return;
    }

    const float cp = Math_CosS(sNote.rot.x);
    const float sp = Math_SinS(sNote.rot.x);
    const float cy = Math_CosS(sNote.rot.y);
    const float sy = Math_SinS(sNote.rot.y);
    // The flight direction exactly as Actor_SetProjectileSpeed + Actor_MoveXZGravity build it
    // (positive pitch is nose-down), plus a right/up frame around it.
    const Vec3f fwd = { cp * sy, -sp, cp * cy };
    const Vec3f right = { cy, 0.0f, -sy };
    const Vec3f up = { fwd.y * right.z - fwd.z * right.y, fwd.z * right.x - fwd.x * right.z,
                       fwd.x * right.y - fwd.y * right.x };

    Vec3f start = sNote.origin;
    Vec3f end = { start.x + fwd.x * sNote.range, start.y + fwd.y * sNote.range, start.z + fwd.z * sNote.range };
    Vec3f hit;
    CollisionPoly* poly = nullptr;
    s32 bgId = BGCHECK_SCENE;
    // Same test the vanilla reticle runs (walls, floors, ceilings, one-sided polys).
    const bool hitSurface =
        BgCheck_AnyLineTest3(&gPlayState->colCtx, &start, &end, &hit, &poly, true, true, true, true, &bgId);
    const bool hookable =
        hitSurface && poly != nullptr && SurfaceType_IsHookshotSurface(&gPlayState->colCtx, poly, bgId) != 0;
    float len = sNote.range;
    if (hitSurface) {
        len = sqrtf(SQ(hit.x - start.x) + SQ(hit.y - start.y) + SQ(hit.z - start.z));
    }
    if (len < 1.0f) {
        return;
    }

    float ws = VR_GetWorldScale();
    if (ws < 1.0f) {
        ws = 35.0f;
    }
    // Geometry lives in a beam frame: X/Y across the beam in units of a sixteenth of the near
    // half-width, Z along it from 0 (the hook) to 1000 (the end). The far end widens with distance
    // so the beam keeps a visible angular width instead of thinning to shimmering sub-pixels.
    const float nearHalf = 0.0015f * ws;                  // 1.5 mm
    const float farHalf = fmaxf(nearHalf, 0.0012f * len); // ~0.07 degrees seen from the hand
    const float dotHalf = fmaxf(0.01f * ws, 0.006f * len);
    const float unit = nearHalf / 16.0f;
    const float farK = fminf(farHalf / unit, 30000.0f);
    const float dotK = fminf(dotHalf / unit, 30000.0f);

    u8 r;
    u8 g;
    u8 b;
    if (hookable) {
        r = 70, g = 255, b = 130;
    } else {
        r = 255, g = 60, b = 40;
    }
    const u8 nearA = 200;
    const u8 farA = hitSurface ? 170 : 0; // nothing in reach: the beam fades out at full reach

    // Two crossed quads (visible from any side) along the beam, then the end dot facing back up it.
    sLaserVtx[0] = LaserVtx(-16.0f, 0.0f, 0.0f, r, g, b, nearA);
    sLaserVtx[1] = LaserVtx(16.0f, 0.0f, 0.0f, r, g, b, nearA);
    sLaserVtx[2] = LaserVtx(-farK, 0.0f, 1000.0f, r, g, b, farA);
    sLaserVtx[3] = LaserVtx(farK, 0.0f, 1000.0f, r, g, b, farA);
    sLaserVtx[4] = LaserVtx(0.0f, -16.0f, 0.0f, r, g, b, nearA);
    sLaserVtx[5] = LaserVtx(0.0f, 16.0f, 0.0f, r, g, b, nearA);
    sLaserVtx[6] = LaserVtx(0.0f, -farK, 1000.0f, r, g, b, farA);
    sLaserVtx[7] = LaserVtx(0.0f, farK, 1000.0f, r, g, b, farA);
    sLaserVtx[8] = LaserVtx(0.0f, dotK, 995.0f, r, g, b, 235);
    sLaserVtx[9] = LaserVtx(dotK, 0.0f, 995.0f, r, g, b, 235);
    sLaserVtx[10] = LaserVtx(-dotK, 0.0f, 995.0f, r, g, b, 235);
    sLaserVtx[11] = LaserVtx(0.0f, -dotK, 995.0f, r, g, b, 235);

    MtxF beam;
    beam.mf[0][0] = right.x * unit;
    beam.mf[0][1] = right.y * unit;
    beam.mf[0][2] = right.z * unit;
    beam.mf[0][3] = 0.0f;
    beam.mf[1][0] = up.x * unit;
    beam.mf[1][1] = up.y * unit;
    beam.mf[1][2] = up.z * unit;
    beam.mf[1][3] = 0.0f;
    beam.mf[2][0] = fwd.x * (len / 1000.0f);
    beam.mf[2][1] = fwd.y * (len / 1000.0f);
    beam.mf[2][2] = fwd.z * (len / 1000.0f);
    beam.mf[2][3] = 0.0f;
    beam.mf[3][0] = start.x;
    beam.mf[3][1] = start.y;
    beam.mf[3][2] = start.z;
    beam.mf[3][3] = 1.0f;

    Gfx* p = sLaserDl;
    FrameInterpolation_RecordOpenChild((const void*)sLaserDl, 0);
    Matrix_Put(&beam);
    Mtx* beamMtx = MATRIX_NEWMTX(gPlayState->state.gfxCtx);
    FrameInterpolation_RecordCloseChild();
    if (sNote.hasHandMtx) {
        // Weld to the live hand: hand-LOCAL part against the same 20 Hz snapshot the hook's aim
        // was taken from; the interpreter re-composes (live hand pose) x (local) per eye.
        MtxF inv;
        MtxF local;
        SkinMatrix_Invert(&sNote.handMtx, &inv);
        SkinMatrix_MtxFMtxFMult(&inv, &beam, &local);
        VR_RegisterHandChildMatrix((const void*)beamMtx, sNote.hand, &local.mf[0][0]);
    }

    gDPPipeSync(p++);
    gDPSetCycleType(p++, G_CYC_1CYCLE);
    gDPSetRenderMode(p++, G_RM_ZB_XLU_SURF, G_RM_ZB_XLU_SURF2);
    gDPSetCombineMode(p++, G_CC_SHADE, G_CC_SHADE);
    gSPTexture(p++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gSPClearGeometryMode(p++, G_CULL_BOTH | G_LIGHTING | G_FOG | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR);
    gSPSetGeometryMode(p++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
    gSPMatrix(p++, beamMtx, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPVertex(p++, (uintptr_t)&sLaserVtx[0], 8, 0);
    gSP2Triangles(p++, 0, 1, 2, 0, 1, 3, 2, 0);
    gSP2Triangles(p++, 4, 5, 6, 0, 5, 7, 6, 0);
    if (hitSurface) {
        // The dot ignores depth: it sits on the very surface it marks, which would otherwise
        // swallow half of it at any glancing angle.
        gDPPipeSync(p++);
        gDPSetRenderMode(p++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
        gSPClearGeometryMode(p++, G_ZBUFFER);
        gSPVertex(p++, (uintptr_t)&sLaserVtx[8], 4, 0);
        gSP2Triangles(p++, 0, 2, 1, 0, 1, 2, 3, 0);
    }
    gSPEndDisplayList(p++);

    OPEN_DISPS(gPlayState->state.gfxCtx);
    gSPDisplayList(POLY_XLU_DISP++, sLaserDl);
    CLOSE_DISPS(gPlayState->state.gfxCtx);
}

namespace {
void RegisterVrHookshot() {
    COND_HOOK(OnPlayDrawEnd, true, VrHookshot_DrawLaser);
}
} // namespace

static RegisterShipInitFunc initVrHookshot(RegisterVrHookshot);
