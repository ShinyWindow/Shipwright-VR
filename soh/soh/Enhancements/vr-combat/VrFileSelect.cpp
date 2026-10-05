extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
}
#include "VrCombat.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <cmath>

// World-space file select (gVrFileSelectWorld). User spec (October 4, 2026): "the save select menu
// should also be in world space like the inventory is, like you should be in the scene and the menu
// should be directly in front of you."
//
// The vanilla file select is already a scene: SkyboxDraw_Draw puts a sky cube around an eye that
// slowly orbits the origin, then the menu window (a 3D mesh) is drawn under a FIXED view, eye
// (0, 0, 64) looking down -Z, at Translate(0, 0, -93.6) . Scale(0.78), i.e. 157.6 units in front of
// that eye; menu changes flip it about its own X axis (windowRot). In a stereo pass the interpreter
// replaces every projection load with the eye's view x projection and takes modelview loads as world
// matrices (the world-space pause menu's trick, VrPause.cpp), so the file select only has to supply
// world-space modelviews:
//
//   window base = Translate(head + lift) . RotateY(yaw0) . Scale(s) . Translate(0, 0, -64)
//                 . Translate(0, 0, -93.6) . Scale(size)        (then vanilla's Scale(0.78) . RotateX)
//
// head/yaw0 = the head position and facing on the first file-select frame (anchored, never
// follows). The Translate(0, 0, -64) puts vanilla's eye on the player's head, so the window keeps its
// vanilla framing; s = gVrFileSelectDistance metres over the 157.6-unit eye-to-window distance.
// The sky is centred on the live head (it reads as infinitely far) and its slow drift becomes a yaw
// of the sky cube: vanilla's orbiting camera turned around the sky, here the sky turns around you.
//
// The window's 2D parts (title logo on the quest page, seed icons, randomizer settings text, stick
// prompts, the controls line, the rando save-version warning) are screen-space texture rectangles.
// In a stereo pass those would cover each eye's whole view; VR_SetRectWorldPanel maps them instead
// onto a virtual 4:3 TV screen standing in the window's plane, sized to the vanilla 60-degree view,
// so they land exactly where the TV showed them relative to the window. Fill rectangles (the
// screen fade) stay full-eye.

namespace {

constexpr float kVanillaEyeZ = 64.0f;     // FileChoose_SetView(this, 0, 0, 64)
constexpr float kWindowZ = -93.6f;        // Matrix_Translate(0, 0, -93.6f) at every window site
constexpr float kEyeToWindow = 157.6f;    // 64 + 93.6
constexpr float kTanHalfFovy = 0.57735027f; // View_Init fovy 60, aspect 4:3 (320x240)
constexpr float kPi = 3.14159265358979f;

bool sActive = false;   // FileChoose_Init .. FileChoose_Destroy
bool sAnchored = false;
float sHead[3] = { 0.0f, 0.0f, 0.0f };
float sYaw0 = 0.0f;
float sSkyYaw = 0.0f;   // unwrapped, so the interpolated sky rotation never takes the long way round

float WorldScale() {
    const float s = VR_GetWorldScale();
    return s < 1.0f ? 35.0f : s;
}

float WrapPi(float a) {
    while (a > kPi) {
        a -= 2.0f * kPi;
    }
    while (a < -kPi) {
        a += 2.0f * kPi;
    }
    return a;
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
    // RotateY(yaw0) maps the window's -Z (straight ahead of vanilla's eye) onto the flattened facing.
    sYaw0 = atan2f(-hx, -hz);
    sHead[0] = eye[0];
    sHead[1] = eye[1];
    sHead[2] = eye[2];
    sAnchored = true;
}

// The window base into the matrix stack (MTXMODE_NEW): everything up to vanilla's Scale(0.78).
void ApplyWindowBase() {
    const float s = CVarGetFloat("gVrFileSelectDistance", 1.6f) * WorldScale() / kEyeToWindow;
    const float size = CVarGetFloat("gVrFileSelectScale", 100.0f) / 100.0f;
    const float lift = CVarGetFloat("gVrFileSelectHeightCm", 0.0f) * 0.01f * WorldScale();
    Matrix_Translate(sHead[0], sHead[1] + lift, sHead[2], MTXMODE_NEW);
    Matrix_RotateY(sYaw0, MTXMODE_APPLY);
    Matrix_Scale(s, s, s, MTXMODE_APPLY);
    Matrix_Translate(0.0f, 0.0f, kWindowZ - kVanillaEyeZ, MTXMODE_APPLY);
    Matrix_Scale(size, size, size, MTXMODE_APPLY);
}

// The virtual TV screen for texture rectangles: rect NDC (x, y) in [-1, 1] -> the window plane,
// spanning vanilla's 60-degree 4:3 view at the window's distance.
void PublishRectPanel() {
    const float halfH = kEyeToWindow * kTanHalfFovy;
    const float halfW = halfH * (4.0f / 3.0f);
    MtxF panel;
    Matrix_Push();
    ApplyWindowBase();
    Matrix_Scale(halfW, halfH, 1.0f, MTXMODE_APPLY);
    Matrix_Get(&panel);
    Matrix_Pop();
    VR_SetRectWorldPanel(1, &panel.mf[0][0]);
}

} // namespace

extern "C" void VrFileSelect_SetActive(bool active) {
    sActive = active;
    sAnchored = false;
    if (!active) {
        VR_SetRectWorldPanel(0, nullptr);
    }
}

extern "C" bool VrFileSelect_WorldSpace(void) {
    return sActive && VR_IsInitialized() && CVarGetInteger("gVrFileSelectWorld", 1);
}

extern "C" bool VrFileSelect_FrameSync(void) {
    const bool worldSpace = VrFileSelect_WorldSpace();
    if (!worldSpace) {
        VR_SetRectWorldPanel(0, nullptr);
        sAnchored = false;
    } else {
        // The window is anchored in the world: an artificial turn would swing the playspace out
        // from under it. VrPause_FrameSync (called first) writes this flag every frame, so only
        // the "on" side is needed here.
        VR_SetTurnSuppressed(1);
    }
    return worldSpace;
}

extern "C" void VrFileSelect_BeginFrame(void) {
    if (!VrFileSelect_WorldSpace()) {
        return;
    }
    // No Play_Draw runs here: do its per-frame VR camera duties. The playspace is world-aligned
    // (first person, no base yaw) with its origin at the world origin, so the head IS the HMD pose.
    VR_ClearHandMatrices();
    VR_SetFirstPerson(true);
    if (!sAnchored) {
        // Twice: also overwrite the previous anchor, so the cut from the title demo (or the game)
        // never interpolates across a tick.
        VR_SetCameraAnchor(0.0f, 0.0f, 0.0f);
        VR_SetCameraAnchor(0.0f, 0.0f, 0.0f);
        Anchor();
    } else {
        VR_SetCameraAnchor(0.0f, 0.0f, 0.0f);
    }
    PublishRectPanel();
}

extern "C" void VrFileSelect_WindowTranslate(void) {
    if (!VrFileSelect_WorldSpace() || !sAnchored) {
        Matrix_Translate(0.0f, 0.0f, kWindowZ, MTXMODE_NEW);
        return;
    }
    ApplyWindowBase();
}

extern "C" void VrFileSelect_SkyPose(int16_t orbit, float* eyeX, float* eyeY, float* eyeZ, float* skyRotY) {
    if (!VrFileSelect_WorldSpace() || !sAnchored) {
        return;
    }
    float eye[3], fwd[3], up[3];
    VR_GetCameraPose(eye, fwd, up);
    *eyeX = eye[0];
    *eyeY = eye[1];
    *eyeZ = eye[2];
    // Vanilla looks from eye = RotateY(-theta) (1000, y, 1000) at the origin, i.e. along
    // RotateY(pi/4 - theta) (0, 0, -1). Turning the sky by yaw0 - pi/4 + theta shows that same
    // stretch of sky straight ahead of where the player faced, drifting as the vanilla camera did.
    const float theta = (float)orbit * (kPi / 32768.0f);
    const float target = sYaw0 - (kPi / 4.0f) + theta;
    sSkyYaw += WrapPi(target - sSkyYaw);
    *skyRotY = sSkyYaw;
}
