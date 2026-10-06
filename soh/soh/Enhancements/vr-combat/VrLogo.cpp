extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
}
#include "VrCombat.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <cmath>

// World-space N64 logo screen (gVrLogoWorld). User spec (October 5, 2026): everything in world
// space; the boot logo ("the n64 logo and it says libultraship") was the last screen still on the
// flat panel.
//
// Vanilla (z_title.c) draws the logo under a FIXED view: eye (0, 150, 300) looking at the origin,
// fovy 30, then the N64 logo mesh at Translate(-53, -5, 0) . RotateY(spin), and the shimmering text
// as sixteen 2-px texture-rectangle strips. In a stereo pass the interpreter replaces every projection
// load (the view's lookAt included) with the eye's view x projection and takes modelview loads as
// world matrices (the world-space pause / file select trick), so the logo only has to supply a
// world-space modelview:
//
//   logo base = Translate(head + lift) . RotateY(yaw0) . Scale(s) . LookAt(vanilla eye -> origin)
//
// The LookAt puts vanilla's eye on the player's head looking straight ahead, so the logo keeps its
// vanilla framing; s = gVrLogoDistance metres over vanilla's 335.4-unit eye-to-origin distance.
// head/yaw0 = the head and facing on the first logo frame (anchored, never follows). The text strip
// goes through VR_SetRectWorldPanel onto a virtual 4:3 TV screen standing at the look-at point,
// sized to vanilla's 30-degree view, so it lands exactly where the TV showed it beside the logo. The
// fade (a fill rectangle) stays full-eye, and the black clear is the void around it.

namespace {

constexpr float kEyeY = 150.0f; // Title_SetupView(this, 0, 150, 300), looking at the origin
constexpr float kEyeZ = 300.0f;
constexpr float kTanHalfFovy = 0.26794919f; // func_800AA460(view, 30, ...): tan(15 deg), 4:3
constexpr float kPi = 3.14159265358979f;

bool sActive = false; // Title_Init .. Title_Destroy
bool sAnchored = false;
float sHead[3] = { 0.0f, 0.0f, 0.0f };
float sYaw0 = 0.0f;

float WorldScale() {
    const float s = VR_GetWorldScale();
    return s < 1.0f ? 35.0f : s;
}

float EyeDist() {
    return std::sqrt(kEyeY * kEyeY + kEyeZ * kEyeZ);
}

void Anchor() {
    float eye[3], fwd[3], up[3];
    VR_GetCameraPose(eye, fwd, up);
    float hx = fwd[0];
    float hz = fwd[2];
    if ((hx * hx) + (hz * hz) < 0.09f) {
        // Looking nearly straight down (or up): the head's up vector carries the facing.
        const float sign = (fwd[1] < 0.0f) ? 1.0f : -1.0f;
        hx = up[0] * sign;
        hz = up[2] * sign;
    }
    // RotateY(yaw0) maps -Z (straight ahead of vanilla's eye) onto the flattened facing.
    sYaw0 = atan2f(-hx, -hz);
    sHead[0] = eye[0];
    sHead[1] = eye[1];
    sHead[2] = eye[2];
    sAnchored = true;
}

// The player's head frame, scaled to vanilla units: vanilla's eye at the head, looking along -Z.
void ApplyHeadFrame() {
    const float s = CVarGetFloat("gVrLogoDistance", 2.5f) * WorldScale() / EyeDist();
    const float lift = CVarGetFloat("gVrLogoHeightCm", 0.0f) * 0.01f * WorldScale();
    Matrix_Translate(sHead[0], sHead[1] + lift, sHead[2], MTXMODE_NEW);
    Matrix_RotateY(sYaw0, MTXMODE_APPLY);
    Matrix_Scale(s, s, s, MTXMODE_APPLY);
}

// The text strip's virtual TV screen: rect NDC (x, y) in [-1, 1] -> the plane through the look-at
// point, facing the eye, spanning vanilla's 30-degree 4:3 view at that distance.
void PublishRectPanel() {
    const float halfH = EyeDist() * kTanHalfFovy;
    const float halfW = halfH * (4.0f / 3.0f);
    MtxF panel;
    Matrix_Push();
    ApplyHeadFrame();
    Matrix_Translate(0.0f, 0.0f, -EyeDist(), MTXMODE_APPLY);
    Matrix_Scale(halfW, halfH, 1.0f, MTXMODE_APPLY);
    Matrix_Get(&panel);
    Matrix_Pop();
    VR_SetRectWorldPanel(1, &panel.mf[0][0]);
}

} // namespace

extern "C" void VrLogo_SetActive(bool active) {
    sActive = active;
    sAnchored = false;
    if (!active) {
        VR_SetRectWorldPanel(0, nullptr);
    }
}

extern "C" bool VrLogo_WorldSpace(void) {
    return sActive && VR_IsInitialized() && CVarGetInteger("gVrLogoWorld", 1);
}

extern "C" bool VrLogo_FrameSync(void) {
    const bool worldSpace = VrLogo_WorldSpace();
    if (!worldSpace) {
        sAnchored = false;
    } else {
        // Anchored in the world: an artificial turn would swing the playspace out from under it.
        VR_SetTurnSuppressed(1);
    }
    return worldSpace;
}

extern "C" void VrLogo_BeginFrame(void) {
    if (!VrLogo_WorldSpace()) {
        return;
    }
    // No Play_Draw runs here: do its per-frame VR camera duties. The playspace is world-aligned
    // (first person, no base yaw) with its origin at the world origin, so the head IS the HMD pose.
    VR_ClearHandMatrices();
    VR_SetFirstPerson(true);
    if (!sAnchored) {
        // Twice: also overwrite the previous anchor, so the cut never interpolates across a tick.
        VR_SetCameraAnchor(0.0f, 0.0f, 0.0f);
        VR_SetCameraAnchor(0.0f, 0.0f, 0.0f);
        Anchor();
    } else {
        VR_SetCameraAnchor(0.0f, 0.0f, 0.0f);
    }
    PublishRectPanel();
}

extern "C" void VrLogo_ModelTranslate(float x, float y, float z) {
    if (!VrLogo_WorldSpace() || !sAnchored) {
        Matrix_Translate(x, y, z, MTXMODE_NEW);
        return;
    }
    // Vanilla's view (the same guLookAtF its View builds), carried onto the head frame.
    MtxF lookAt;
    guLookAtF(lookAt.mf, 0.0f, kEyeY, kEyeZ, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    ApplyHeadFrame();
    Matrix_Mult(&lookAt, MTXMODE_APPLY);
    Matrix_Translate(x, y, z, MTXMODE_APPLY);
}
