extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
extern PlayState* gPlayState;
}
#include "VrCombat.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <cmath>

// World-space pause menu (gVrPauseWorldSpace, VR first person). User spec (October 2, 2026):
// "have the game pause but continue to render, so everything is frozen, and you can still look
// around, and the inventory screens open up in world space ... the 4 screens actually appear
// around you like you're inside of this, and it rotates around you as you use it".
//
// The vanilla pause scene is already a box: KaleidoScope_DrawPages places the four pages at
// WREG(3)/100 = 93.55 units from an origin, each with its own modelview, and page changes ORBIT
// THE CAMERA (func_808265BC moves pauseCtx->eye along a square, always looking at the origin).
// In stereo the interpreter replaces every projection load with the eye's view x projection and
// takes modelview loads as world matrices, so a page drawn with a world-space modelview simply
// lands in the world. This module supplies that modelview:
//
//   base = Translate(head) . RotateY(yaw0 + spin) . Scale(s) . Translate(-view x eye)
//
// head/yaw0 = the head position and facing on the first menu draw of a pause (anchored; the box
// never follows the head). The rest is the INVERSE of vanilla's own camera: vanilla views the box
// with LookAt(eye -> origin) = RotateY(spin) . Translate(-eye), spin = -atan2(eye.x, eye.z), where
// eye is pauseCtx->eye orbiting a square 64 units out. So the player's head IS vanilla's camera:
// standing 64 units back from the box center (near the rear page), the front page 157.55 away,
// and a page change spins AND dollies the box exactly as vanilla's camera moves (the box turns, the
// player stays). view = gVrPauseViewBack (0 = head at the box center, 1 = vanilla's camera spot);
// s = gVrPauseRadius metres over the 93.55-unit page distance.
// Everything the vanilla page draws after its placement (flip-in tilt, cursor, icons, map marks)
// rides the page matrix verbatim. The name panel keeps vanilla's on-screen layout relative to the
// front page and is placed in front of it so stereo depth agrees with the draw order.
//
// Rendering order (z_kaleido_scope_PAL.c KaleidoScope_Draw): the menu is built as a sub-display
// list called from the end of POLY_XLU, after the frozen world's translucent pass: world dim
// (a world-anchored box around the head, no Z), then the pages (vanilla render modes, no Z), so
// nothing in the world ever draws over them. The capture backdrop (z_play.c) is skipped.

namespace {

// Vanilla kaleido geometry (z_construct.c WREG(3) = 9355; KaleidoScope_SetView eyes).
constexpr float kPageDist = 93.55f;       // page plane distance from the box origin
constexpr float kEyeToFrontPage = 157.55f; // vanilla eye (0, 0, 64) to the front page: 64 + 93.55
constexpr float kEyeToPanel = 208.0f;      // name panel: Translate(0, 0, -144) seen from (0, 0, 64)
constexpr float kPi = 3.14159265358979f;
constexpr float kPauseLeanMeters = 0.5f;   // head translation honored while frozen
constexpr int kResumeGraceFrames = 6;      // lean allowance kept while the body catches up
constexpr float kDimHalfMeters = 4.0f;     // dim box half-size: the head stays inside it

bool sAnchored = false;
float sCenter[3] = { 0.0f, 0.0f, 0.0f };
float sYaw0 = 0.0f;
float sSpin = 0.0f; // unwrapped, so the interpolated RotateY never takes the long way round
bool sWasWorldSpace = false;
int sResumeGrace = 0;

float WorldScale() {
    const float s = VR_GetWorldScale();
    return s < 1.0f ? 35.0f : s;
}

float BoxScale() {
    return CVarGetFloat("gVrPauseRadius", 1.3f) * WorldScale() / kPageDist;
}

float PageScale() {
    return CVarGetFloat("gVrPausePageScale", 100.0f) / 100.0f;
}

float SpinRaw(const PauseContext* pauseCtx) {
    return -atan2f(pauseCtx->eye.x, pauseCtx->eye.z);
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

// How far back toward vanilla's camera spot the head stands: 0 = box center, 1 = the vanilla eye.
float ViewBack() {
    return CVarGetFloat("gVrPauseViewBack", 100.0f) / 100.0f;
}

// Head to the front page, in vanilla units: 93.55 at the box center, 157.55 at vanilla's eye.
float EyeToFrontPage() {
    return kPageDist + (kEyeToFrontPage - kPageDist) * ViewBack();
}

// Box base into the matrix stack (MTXMODE_NEW). box = false for the parts vanilla draws under the
// FIXED view (the name panel): eye-space at the head, facing the player's original front; they
// never turn or move.
void ApplyBase(bool box) {
    const float s = BoxScale();
    const float lift = CVarGetFloat("gVrPauseHeightCm", 0.0f) * 0.01f * WorldScale();
    Matrix_Translate(sCenter[0], sCenter[1] + lift, sCenter[2], MTXMODE_NEW);
    Matrix_RotateY(sYaw0 + (box ? sSpin : 0.0f), MTXMODE_APPLY);
    Matrix_Scale(s, s, s, MTXMODE_APPLY);
    if (box && gPlayState != NULL) {
        const float k = ViewBack();
        const PauseContext* pauseCtx = &gPlayState->pauseCtx;
        Matrix_Translate(-pauseCtx->eye.x * k, 0.0f, -pauseCtx->eye.z * k, MTXMODE_APPLY);
    }
}

void Anchor(const PauseContext* pauseCtx) {
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
    // RotateY(yaw0) maps the box's -Z (the front page) onto the flattened facing.
    sYaw0 = atan2f(-hx, -hz);
    sCenter[0] = eye[0];
    sCenter[1] = eye[1];
    sCenter[2] = eye[2];
    sSpin = SpinRaw(pauseCtx);
    sAnchored = true;
}

} // namespace

// Darkens the frozen world behind the pages: a box around the head, drawn without Z into the
// current list right before the pages, alpha following the menu's own fade. extern "C"
// linkage is load-bearing: the block-scope FrameInterpolation declarations inside OPEN_DISPS.
extern "C" void VrPause_DrawDim(PlayState* play) {
    const float pct = CVarGetFloat("gVrPauseDim", 40.0f);
    if (pct <= 0.0f) {
        return;
    }
    const PauseContext* pauseCtx = &play->pauseCtx;
    float a = (pct / 100.0f) * 255.0f * ((float)pauseCtx->alpha / 255.0f);
    if (a > 255.0f) {
        a = 255.0f;
    }
    if (a < 1.0f) {
        return;
    }

    GraphicsContext* gfxCtx = play->state.gfxCtx;
    Vtx* vtx = (Vtx*)Graph_Alloc(gfxCtx, 8 * sizeof(Vtx));
    for (int i = 0; i < 8; i++) {
        Vtx_t* v = &vtx[i].v;
        v->ob[0] = (i & 1) ? 100 : -100;
        v->ob[1] = (i & 2) ? 100 : -100;
        v->ob[2] = (i & 4) ? 100 : -100;
        v->flag = 0;
        v->tc[0] = v->tc[1] = 0;
        v->cn[0] = v->cn[1] = v->cn[2] = 0;
        v->cn[3] = 255;
    }
    const float half = kDimHalfMeters * WorldScale() / 100.0f;
    Matrix_Translate(sCenter[0], sCenter[1], sCenter[2], MTXMODE_NEW);
    Matrix_Scale(half, half, half, MTXMODE_APPLY);

    OPEN_DISPS(gfxCtx);
    gDPPipeSync(POLY_OPA_DISP++);
    gSPTexture(POLY_OPA_DISP++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gSPLoadGeometryMode(POLY_OPA_DISP++, 0);
    gDPSetOtherMode(POLY_OPA_DISP++,
                    G_AD_DISABLE | G_CD_DISABLE | G_CK_NONE | G_TC_FILT | G_TF_POINT | G_TT_NONE | G_TL_TILE |
                        G_TD_CLAMP | G_TP_NONE | G_CYC_1CYCLE | G_PM_NPRIMITIVE,
                    G_AC_NONE | G_ZS_PRIM | G_RM_XLU_SURF | G_RM_XLU_SURF2);
    gDPSetCombineMode(POLY_OPA_DISP++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 0, 0, 0, (u8)a);
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPVertex(POLY_OPA_DISP++, (uintptr_t)vtx, 8, 0);
    // Vertex index bits: 1 = +X, 2 = +Y, 4 = +Z. No culling: from inside, each view ray leaves
    // through exactly one face, so the dim is a single uniform layer.
    gSP2Triangles(POLY_OPA_DISP++, 0, 2, 6, 0, 0, 6, 4, 0);
    gSP2Triangles(POLY_OPA_DISP++, 1, 5, 7, 0, 1, 7, 3, 0);
    gSP2Triangles(POLY_OPA_DISP++, 0, 4, 5, 0, 0, 5, 1, 0);
    gSP2Triangles(POLY_OPA_DISP++, 2, 3, 7, 0, 2, 7, 6, 0);
    gSP2Triangles(POLY_OPA_DISP++, 0, 1, 3, 0, 0, 3, 2, 0);
    gSP2Triangles(POLY_OPA_DISP++, 4, 6, 7, 0, 4, 7, 5, 0);
    gDPPipeSync(POLY_OPA_DISP++);
    CLOSE_DISPS(gfxCtx);
}

extern "C" bool VrPause_WorldSpace(void) {
    if (gPlayState == NULL || !VR_IsInitialized() || !VR_GetFirstPerson()) {
        return false;
    }
    if (!CVarGetInteger("gVrPauseWorldSpace", 1)) {
        return false;
    }
    const PauseContext* pauseCtx = &gPlayState->pauseCtx;
    if (pauseCtx->state == 0 || pauseCtx->debugState != 0) {
        return false;
    }
    // Game over (states 8..0x11) is world space too: its pages are the same 3D pages, and its
    // "GAME OVER" title (screen-space texture rectangles) goes onto the HUD frame's virtual screen
    // (VrPause_GameOverRects).
    return true;
}

static bool InGameOver(void) {
    return gPlayState != NULL && gPlayState->pauseCtx.state >= 8 && gPlayState->pauseCtx.state <= 0x11;
}

extern "C" bool VrPause_InputMenuMode(void) {
    // The world-space file select is a menu too: the right stick stays its C-stick there.
    return VR_IsFlatScreen() || VrPause_WorldSpace() || VrFileSelect_WorldSpace();
}

// The HUD frame of the flat menu, in the world: vanilla shows hearts + magic top-left, the
// C buttons with Return / Save / Decide top-right and rupees bottom-left around the front page, all
// in screen space under a 60 deg (4:3) view. Every pause element here keeps vanilla's on-screen
// angle (tangent) times r = (157.55 / head-to-front-page) x page scale, the same factor
// VrPause_PanelMatrix uses, so a 4:3 "virtual screen" whose height spans 2 d tan(30 deg) r at
// depth d frames the front page exactly like the TV did. It sits at the name panel's depth, facing
// the player's original front, and never spins (vanilla's HUD stays put while the pages turn).
// The library draws the HUD image on it.
// The game-over screen's texture rectangles ("GAME OVER") land on the same virtual screen, at its
// TV size (the HUD canvas sliders only resize the HUD quad), so they sit where the TV showed them
// relative to the pages.
static void PublishHudPanel(bool gameOverRects) {
    const float s = BoxScale();
    const float lift = CVarGetFloat("gVrPauseHeightCm", 0.0f) * 0.01f * WorldScale();
    const float frontDist = EyeToFrontPage();
    const float r = (kEyeToFrontPage / frontDist) * PageScale();
    const float depth = frontDist * CVarGetFloat("gVrPausePanelDepth", 60.0f) / 100.0f; // box units
    const float h = 2.0f * depth * r * tanf(30.0f * kPi / 180.0f) * s;
    const float w = h * (4.0f / 3.0f);
    const float center[3] = { sCenter[0] - sinf(sYaw0) * depth * s, sCenter[1] + lift,
                              sCenter[2] - cosf(sYaw0) * depth * s };
    VR_SetHudWorldPanel(1, center, sYaw0, w, h);
    if (gameOverRects) {
        MtxF panel;
        Matrix_Push();
        Matrix_Translate(center[0], center[1], center[2], MTXMODE_NEW);
        Matrix_RotateY(sYaw0, MTXMODE_APPLY);
        Matrix_Scale(w * 0.5f, h * 0.5f, 1.0f, MTXMODE_APPLY);
        Matrix_Get(&panel);
        Matrix_Pop();
        VR_SetRectWorldPanel(1, &panel.mf[0][0]);
    }
}

static bool sGameOverRects = false;

extern "C" bool VrPause_GameOverRects(void) {
    return sGameOverRects;
}

extern "C" bool VrPause_FrameSync(void) {
    const bool worldSpace = VrPause_WorldSpace();
    sGameOverRects = worldSpace && sAnchored && InGameOver();
    if (worldSpace && sAnchored) {
        PublishHudPanel(sGameOverRects);
    } else {
        const float zero[3] = { 0.0f, 0.0f, 0.0f };
        VR_SetHudWorldPanel(0, zero, 0.0f, 0.0f, 0.0f);
    }
    if (!worldSpace) {
        if (sWasWorldSpace) {
            sResumeGrace = kResumeGraceFrames;
        } else if (sResumeGrace > 0) {
            sResumeGrace--;
        }
        sAnchored = false;
    }
    sWasWorldSpace = worldSpace;
    // The right stick is the C-stick in the menu (item assignment), exactly as on the flat panel:
    // it must not also turn the player inside a world-anchored box.
    VR_SetTurnSuppressed(worldSpace);
    return worldSpace;
}

extern "C" float VrPause_LeanClamp(void) {
    if (VrPause_WorldSpace() || sResumeGrace > 0) {
        return kPauseLeanMeters * WorldScale();
    }
    return 0.1f;
}

extern "C" void VrPause_RefreshCullView(PlayState* play) {
    float e[3], f[3], u[3];
    VR_GetCameraPose(e, f, u);
    Vec3f eye = { e[0], e[1], e[2] };
    Vec3f at = { e[0] + (f[0] * 100.0f), e[1] + (f[1] * 100.0f), e[2] + (f[2] * 100.0f) };
    Vec3f up = { u[0], u[1], u[2] };
    play->view.fovy = VR_GetCullingFovy();
    View_SetScale(&play->view, 1.0f);
    func_800AA358(&play->view, &eye, &at, &up);
}

extern "C" void VrPause_BeginDraw(PlayState* play) {
    const PauseContext* pauseCtx = &play->pauseCtx;
    if (!sAnchored) {
        Anchor(pauseCtx);
    } else {
        sSpin += WrapPi(SpinRaw(pauseCtx) - sSpin);
    }
    VrPause_DrawDim(play);
}

extern "C" void VrPause_PageTranslate(float x, float y, float z) {
    // The page size scales about the page's own pivot. The vertical term scales too: during the
    // open/close flip the pivot is the page's bottom edge (WREG(2) = -62.4 with the vertices
    // raised by offsetY 80 x 0.78), at rest its center; scaling both keeps the two framings
    // identical, so the page doesn't jump when the flip ends.
    const float ps = PageScale();
    ApplyBase(true);
    Matrix_Translate(x, y * ps, z, MTXMODE_APPLY);
    Matrix_Scale(ps, ps, ps, MTXMODE_APPLY);
}

extern "C" void VrPause_PanelMatrix(void) {
    // Vanilla draws the panel 208 units in front of its eye, at the bottom of the screen just
    // under the front page (157.55 units away). The head sits EyeToFrontPage() from that page, so
    // the page looks (157.55 / that) x page scale as big as in vanilla; the panel is scaled by the
    // same factor in screen terms (tangent space) to stay just under the page edge, and placed at
    // gVrPausePanelDepth percent of the head-to-page distance: in FRONT of the page, so its stereo
    // depth agrees with the draw order. Scaling about the eye keeps every element at its angle.
    const float frontDist = EyeToFrontPage();
    const float r = (kEyeToFrontPage / frontDist) * PageScale();
    const float depth = frontDist * CVarGetFloat("gVrPausePanelDepth", 60.0f) / 100.0f;
    const float k = r * depth / kEyeToPanel;
    ApplyBase(false);
    Matrix_Translate(0.0f, 0.0f, -depth, MTXMODE_APPLY);
    Matrix_Scale(k, k, k, MTXMODE_APPLY);
}
