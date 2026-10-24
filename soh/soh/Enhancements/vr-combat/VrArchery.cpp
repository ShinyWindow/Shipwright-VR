extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "objects/gameplay_keep/gameplay_keep.h"
#include "overlays/actors/ovl_En_Arrow/z_en_arrow.h"
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
bool ArrowCycleMain(void);
}
#include "VrCombat.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/frame_interpolation.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <vr_interface.h>
#include <cmath>

// Physical archery (VR first person, selector mode; slingshot first, bow inherits): the weapon
// rides the OFF hand (bow/slingshot are right-hand models under motion hands), and the string
// hand pinches the configured input (default trigger) NEAR the weapon to nock. While nocked,
// the weapon's own item button reads held-down in padmgr, so the entire vanilla draw pipeline
// runs untouched — projectile spawned at nock, ammo charged at release, elemental magic already
// deferred to release by the lifecycle work. Releasing the pinch past the minimum draw drops
// the button, and the vanilla release fires the shot along the string-hand -> bow-hand line
// (Player_VrAimHeldProjectile consumes VrArchery_AimSegment). A release short of the minimum
// draw cancels through Player_VrCancelPreparedItem: arrow killed, ammo and magic preserved.
//
// The Fairy Bow has its own profile (see "Fairy Bow profile" below): nock at the braced string,
// shoot from the string hand through the arrow rest on the grip, string drawn to the hand, draw
// length sets arrow power. The slingshot keeps its own rules (nock at the braced pouch, draw =
// hand-to-pouch gap, shot from the string hand through the pouch); both read their geometry off
// the model they render, through the live hand draw matrix.
//
// Deliberate fallbacks: gallery transitions, bombchu bowling and horseback keep their own
// schemes. Live shooting-gallery rounds use this physical interaction with their forced B-slot
// weapon, and gVrPhysArchery=0 restores the previous trigger-to-item-button behavior.

namespace {

bool sNocked = false;
bool sPinchPrev = true; // require a fresh pinch after entry/reset
bool sCycleTriggerPrev = true; // same: entering bow mode with it held must not cycle
float sDrawM = 0.0f;    // current string-hand draw distance, meters

// The firing release is processed by the vanilla pad path a frame AFTER the tick clears the
// nock — and draws in between would fall back to the one-hand ray and overwrite the shot
// direction (the "shots ignore the pull" bug). So the last live pull line is latched at
// release and served for a few ticks, exactly long enough for the vanilla release to fire.
float sAimLatch[6] = {};
bool sAimLatchLive = false; // a nock has produced a valid latch this draw cycle
int sAimLatchTicks = 0;     // >0: keep serving the latch after the nock ended
Vec3f sAimDotTarget = {};
bool sAimDotTargetLive = false;

constexpr int kAimDotMaxGfx = 20;
Gfx sAimDotDl[kAimDotMaxGfx];
Vtx sAimDotVtx[4];

int StringHand() {
    return CVarGetInteger("gVrLeftHanded", 0) ? VR_HAND_LEFT : VR_HAND_RIGHT;
}

int BowHand() {
    return 1 - StringHand();
}

uint16_t PinchMask() {
    return (uint16_t)CVarGetInteger("gVrArcheryNockInput", VR_BTN_TRIGGER);
}

float WorldScale() {
    const float ws = VR_GetWorldScale();
    return ws < 1.0f ? 35.0f : ws;
}

// --- Slingshot geometry -----------------------------------------------------------------------
// Read off the model it renders (object_link_child, measured October 1, 2026). In the bow hand's
// limb frame the fork prongs end at X ~ 605-663, Z = +-395..425; the band DL
// (gLinkChildSlingshotStringDL) is drawn translated to sBowStringData[1] = (606, 236, 0), the
// midpoint between the prong tips, with its ends there (Z = +-343) and its pouch apex authored
// at (-9.5, -1324.5, 55) relative to that root, pulled along -Y (the shot flies along +Y). At
// rest the band is flat, so the pouch sits at the root. (The drawn band maps that apex onto the
// pouch point, z_player_lib.c.)
constexpr float kSlingRoot[3] = { 606.0f, 236.0f, 0.0f }; // model units, R_HAND limb frame

// The nock anchor: the braced pouch (band root) through the live bow-hand draw matrix, so it sits
// on the slingshot at every world scale. The marker shows this point, reach and draw distance
// measure against it, and the shot flies string-hand -> anchor. (Replaces the controller-cm
// sliders tuned September 14, 2026, which landed within about half a centimetre of this point at
// 35 units/m and drifted off it at any other scale.)
bool NockAnchorWorld(float* out3) {
    float m[4][4];
    if (!VR_GetHandMatrix(BowHand(), m)) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        out3[i] = m[0][i] * kSlingRoot[0] + m[1][i] * kSlingRoot[1] + m[2][i] * kSlingRoot[2] + m[3][i];
    }
    return true;
}

// String hand <-> nock anchor gap in meters; negative when either is unavailable.
float DrawGapM() {
    float s[3], rot[4], a[3];
    if (!VR_GetHandPose(StringHand(), s, rot) || !NockAnchorWorld(a)) {
        return -1.0f;
    }
    float d2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        const float d = s[i] - a[i];
        d2 += d * d;
    }
    return std::sqrt(d2) / WorldScale();
}

// --- Fairy Bow profile ------------------------------------------------------------------------
// Like the slingshot, the bow reads its geometry straight off the model it renders (object_link_boy, measured September 30, 2026): in
// the bow hand's limb frame the riser runs along model X (bow tips at X = +-2010, string ends at
// +-1483), the arrow flies along +Y, and the string rests 360.4 behind the grip (sBowStringData).
// The string DL's apex vertex — the nocking point — sits at X = 360 (the top of the fist) and
// 1160 further along -Y at the vanilla pull. So, like every VR bow: the arrow rest is the grip at
// nock height, the arrow runs from the string hand through that rest, the string pulls to the
// hand, and how far you draw decides the shot's power.
constexpr float kBowNockX = 360.0f;     // model units, riser height of the nock / arrow rest
constexpr float kBowStringY = -360.4f;  // model units, braced string behind the grip
// The nocked arrow's nock end, behind its actor origin (world units: model units x actor scale
// 0.01). gArrowNearDL runs from the arrowhead point at X = -396 to the nock end at X = +2001
// (fletching vanes at 1438..1958); gArrowSkel's second limb sits at (-1, 0, 998) and its third
// turns the DL +90 deg about Y (gArrow2Anim frame 0, the pose a held arrow keeps), so the shaft
// points along actor +Z with the tip 1394 ahead and the nock end 1003 behind the origin.
// (Measured October 1, 2026; the earlier 3.96 read the DL backwards and needed a cm trim.)
constexpr float kArrowNockBack = 10.03f;

struct BowFrame {
    float rest[3];       // arrow rest on the riser, world
    float stringRest[3]; // nocking point on the braced string, world
    float fwd[3];        // the bow's own shooting axis (model +Y), world unit vector
    float brace;         // rest <-> braced string, world units
};

bool HoldsBow(Player* player) {
    return player != NULL && player->heldItemAction >= PLAYER_IA_BOW && player->heldItemAction <= PLAYER_IA_BOW_0E;
}

bool HoldsBowNow() {
    if (gPlayState == NULL) {
        return false;
    }
    // Vanilla starts both shooting galleries through ITEM_BOW. Child Link still renders and
    // fires the slingshot, so the gallery's internal item action cannot identify its physical
    // VR geometry: doing so puts the bow's braced nock behind the visible slingshot. Match the
    // weapon model chosen by the game instead.
    if (gPlayState->shootingGalleryStatus > 0) {
        return LINK_IS_ADULT;
    }
    return HoldsBow(GET_PLAYER(gPlayState));
}

bool ShootingGalleryActive() {
    return gPlayState != NULL && gPlayState->shootingGalleryStatus > 0;
}

bool ShootingGalleryWeaponReady() {
    if (!ShootingGalleryActive()) {
        return false;
    }
    const uint8_t item = gSaveContext.equips.buttonItems[0];
    return item == ITEM_SLINGSHOT || item == ITEM_BOW;
}

// Rodrigues rotation of v about unit axis k by the angle with cosine c and sine s.
void RotateAbout(const float k[3], float c, float s, const float v[3], float out[3]) {
    const float kv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
    const float kxv[3] = { k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0] };
    for (int i = 0; i < 3; i++) {
        out[i] = v[i] * c + kxv[i] * s + k[i] * kv * (1.0f - c);
    }
}

// While drawn, the bow points where the two hands say, not where the bow-hand controller points:
// the bow (and Link's hand on it) turns about the grip so its shooting axis runs along the
// string hand -> arrow rest line, as bows do in VR games once the string is drawn. Minimal
// rotation from the controller's own shooting axis, so the wrist's cant (roll) is kept. Eases in
// over the first 10 cm of pull so nocking off-axis never snaps the bow; a string hand at or in
// front of the riser leaves the bow on the controller (no backwards flip). Rotating about the
// grip moves the arrow rest (360 up the riser), so the line is re-solved a few times.
bool AlignBowMatrix(float m[4][4]) {
    if (!sNocked || !CVarGetInteger("gVrBowAlign", 1)) {
        return false;
    }
    float s[3], rot[4];
    if (!VR_GetHandPose(StringHand(), s, rot)) {
        return false;
    }
    const float ylen = std::sqrt(m[1][0] * m[1][0] + m[1][1] * m[1][1] + m[1][2] * m[1][2]);
    if (ylen < 1e-6f) {
        return false;
    }
    const float y0[3] = { m[1][0] / ylen, m[1][1] / ylen, m[1][2] / ylen };
    const float brace = std::fabs(kBowStringY) * ylen;
    const float ws = WorldScale();
    float cols[3][3];
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < 3; i++) {
            cols[c][i] = m[c][i];
        }
    }
    for (int iter = 0; iter < 3; iter++) {
        float d[3];
        float d2 = 0.0f;
        for (int i = 0; i < 3; i++) {
            d[i] = m[3][i] + cols[0][i] * kBowNockX - s[i];
            d2 += d[i] * d[i];
        }
        const float len = std::sqrt(d2);
        if (len < 0.03f * ws) {
            return false;
        }
        for (int i = 0; i < 3; i++) {
            d[i] /= len;
        }
        if (iter == 0 && d[0] * y0[0] + d[1] * y0[1] + d[2] * y0[2] < 0.2f) {
            return false;
        }
        float w = (len - brace) / (0.10f * ws);
        w = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
        float t[3];
        float tl = 0.0f;
        for (int i = 0; i < 3; i++) {
            t[i] = y0[i] * (1.0f - w) + d[i] * w;
            tl += t[i] * t[i];
        }
        tl = std::sqrt(tl);
        if (tl < 1e-6f) {
            return false;
        }
        for (int i = 0; i < 3; i++) {
            t[i] /= tl;
        }
        float k[3] = { y0[1] * t[2] - y0[2] * t[1], y0[2] * t[0] - y0[0] * t[2], y0[0] * t[1] - y0[1] * t[0] };
        const float sn = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
        const float cs = y0[0] * t[0] + y0[1] * t[1] + y0[2] * t[2];
        for (int c = 0; c < 3; c++) {
            if (sn < 1e-6f) {
                for (int i = 0; i < 3; i++) {
                    cols[c][i] = m[c][i];
                }
            } else {
                const float kk[3] = { k[0] / sn, k[1] / sn, k[2] / sn };
                RotateAbout(kk, cs, sn, m[c], cols[c]);
            }
        }
    }
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < 3; i++) {
            m[c][i] = cols[c][i];
        }
    }
    Player* player = gPlayState != NULL ? GET_PLAYER(gPlayState) : NULL;
    if (player != NULL && (player->stateFlags1 & PLAYER_STATE1_ON_HORSE)) {
        // Mounted physical bow is POSITION-ONLY after nocking. The previous minimal rotation
        // still began with the controller's axes, so conflicting wrist orientations could swing
        // the whole shaft sideways. Rebuild all three axes from bow-hand position, string-hand
        // position and world up; no controller quaternion survives this block.
        const float xLen = std::sqrt(m[0][0] * m[0][0] + m[0][1] * m[0][1] + m[0][2] * m[0][2]);
        const float yLen = std::sqrt(m[1][0] * m[1][0] + m[1][1] * m[1][1] + m[1][2] * m[1][2]);
        const float zLen = std::sqrt(m[2][0] * m[2][0] + m[2][1] * m[2][1] + m[2][2] * m[2][2]);
        float stringPos[3], stringRot[4];
        if (xLen > 1e-6f && yLen > 1e-6f && zLen > 1e-6f &&
            VR_GetHandPose(StringHand(), stringPos, stringRot)) {
            float x[3] = { 0.0f, 1.0f, 0.0f };
            float y[3] = {};
            for (int iter = 0; iter < 3; iter++) {
                float rest[3] = { m[3][0] + x[0] * xLen * kBowNockX,
                                  m[3][1] + x[1] * xLen * kBowNockX,
                                  m[3][2] + x[2] * xLen * kBowNockX };
                float yNorm = 0.0f;
                for (int i = 0; i < 3; i++) {
                    y[i] = rest[i] - stringPos[i];
                    yNorm += y[i] * y[i];
                }
                yNorm = std::sqrt(yNorm);
                if (yNorm < 1e-4f) {
                    return false;
                }
                for (int i = 0; i < 3; i++) {
                    y[i] /= yNorm;
                }
                x[0] = -y[0] * y[1];
                x[1] = 1.0f - y[1] * y[1];
                x[2] = -y[2] * y[1];
                const float xNorm = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
                if (xNorm < 1e-4f) {
                    return false;
                }
                for (int i = 0; i < 3; i++) {
                    x[i] /= xNorm;
                }
            }
            float z[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2],
                           x[0] * y[1] - x[1] * y[0] };
            const float oldCross[3] = { m[0][1] * m[1][2] - m[0][2] * m[1][1],
                                        m[0][2] * m[1][0] - m[0][0] * m[1][2],
                                        m[0][0] * m[1][1] - m[0][1] * m[1][0] };
            const float handedness = oldCross[0] * m[2][0] + oldCross[1] * m[2][1] + oldCross[2] * m[2][2];
            for (int i = 0; i < 3; i++) {
                m[0][i] = x[i] * xLen;
                m[1][i] = y[i] * yLen;
                m[2][i] = z[i] * zLen * (handedness < 0.0f ? -1.0f : 1.0f);
            }
        }
    }
    return true;
}

bool GetBowFrame(BowFrame* f) {
    float m[4][4];
    if (!VR_GetHandMatrix(BowHand(), m)) {
        return false;
    }
    AlignBowMatrix(m); // drawn: the frame the bow is actually rendered in

    // Engine MtxF: m[c] is column c (model axis c in world), m[3] the translation.
    auto xf = [&](float x, float y, float z, float* out) {
        for (int i = 0; i < 3; i++) {
            out[i] = m[0][i] * x + m[1][i] * y + m[2][i] * z + m[3][i];
        }
    };
    xf(kBowNockX, 0.0f, 0.0f, f->rest);
    xf(kBowNockX, kBowStringY, 0.0f, f->stringRest);
    const float len = std::sqrt(m[1][0] * m[1][0] + m[1][1] * m[1][1] + m[1][2] * m[1][2]);
    if (len < 1e-6f) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        f->fwd[i] = m[1][i] / len;
    }
    f->brace = std::fabs(kBowStringY) * len; // column length = model-unit -> world scale
    return true;
}

float BowFullDrawM() {
    return CVarGetFloat("gVrBowFullDraw", 45.0f) * 0.01f;
}

// The live nock: the string hand, but the string only stretches so far — past the maximum draw
// the nock (arrow + string apex) stops and the hand keeps going, as bows do in most VR games.
// dir = nock -> rest (the shot direction), drawM = pull beyond brace height. A hand at or in
// front of the riser is no draw at all (it must never read as a pull that fires backwards).
bool BowNock(float* nock3, float* dir3, float* drawM) {
    BowFrame f;
    float s[3], rot[4];
    if (!GetBowFrame(&f) || !VR_GetHandPose(StringHand(), s, rot)) {
        return false;
    }
    float d[3];
    float d2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        d[i] = f.rest[i] - s[i];
        d2 += d[i] * d[i];
    }
    const float len = std::sqrt(d2);
    if (len < 0.03f * WorldScale()) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        dir3[i] = d[i] / len;
    }
    const float along = dir3[0] * f.fwd[0] + dir3[1] * f.fwd[1] + dir3[2] * f.fwd[2];
    float pull = along > 0.0f ? len - f.brace : 0.0f;
    const float maxPull = BowFullDrawM() * 1.25f * WorldScale();
    pull = pull < 0.0f ? 0.0f : (pull > maxPull ? maxPull : pull);
    for (int i = 0; i < 3; i++) {
        nock3[i] = f.rest[i] - dir3[i] * (f.brace + pull);
    }
    *drawM = pull / WorldScale();
    return true;
}

// Where the string hand nocks: the braced string for the bow, the braced pouch for the slingshot.
bool NockTargetWorld(float* out3) {
    if (HoldsBowNow()) {
        BowFrame f;
        if (!GetBowFrame(&f)) {
            return false;
        }
        for (int i = 0; i < 3; i++) {
            out3[i] = f.stringRest[i];
        }
        return true;
    }
    return NockAnchorWorld(out3);
}

bool NearBow() {
    if (HoldsBowNow()) {
        float s[3], rot[4], t[3];
        if (!VR_GetHandPose(StringHand(), s, rot) || !NockTargetWorld(t)) {
            return false;
        }
        float d2 = 0.0f;
        for (int i = 0; i < 3; i++) {
            d2 += (s[i] - t[i]) * (s[i] - t[i]);
        }
        const float r = CVarGetFloat("gVrBowNockRadius", 20.0f) * 0.01f * WorldScale();
        return d2 <= r * r;
    }
    const float gap = DrawGapM();
    return gap >= 0.0f && gap <= CVarGetFloat("gVrArcheryNockRadius", 20.0f) * 0.01f;
}

// Current draw in meters (bow: pull past brace height; slingshot: hand <-> anchor gap).
float CurrentDrawM() {
    if (HoldsBowNow()) {
        float nock[3], dir[3], drawM;
        return BowNock(nock, dir, &drawM) ? drawM : -1.0f;
    }
    return DrawGapM();
}

float sBowShotPower = 0.0f; // latched at a firing bow release, consumed by EnArrow_Shoot
int sBowClick = 0;          // draw-click notch reached this nock (haptic ratchet)
bool sBowFullFelt = false;  // full-draw pulse already given this nock

} // namespace

extern "C" bool VrArchery_Covers(Player* player) {
    const bool gallery = ShootingGalleryActive();
    const bool horseBow = player != NULL && (player->stateFlags1 & PLAYER_STATE1_ON_HORSE) &&
                          VrItemSelect_HorseBowEnabled();
    if (!VR_IsInitialized() || (!VrItemSelect_ModeActive() && !gallery && !horseBow) ||
        !CVarGetInteger("gVrPhysArchery", 1) || player == NULL || player->actor.category != ACTORCAT_PLAYER ||
        gPlayState == NULL) {
        return false;
    }
    if (!gallery && !horseBow && !VrItemSelect_SelectionAllowed()) {
        return false; // scripted control, transitions, ocarina: no nock, no button mask
    }
    if (gPlayState->shootingGalleryStatus < 0 || gPlayState->bombchuBowlingStatus != 0) {
        return false; // gallery transitions and bowling keep their dedicated schemes
    }
    return ShootingGalleryWeaponReady() ||
           (player->heldItemAction >= PLAYER_IA_BOW && player->heldItemAction <= PLAYER_IA_BOW_0E) ||
           player->heldItemAction == PLAYER_IA_SLINGSHOT;
}

extern "C" void VrArchery_Reset(void) {
    sNocked = false;
    sPinchPrev = true;
    sCycleTriggerPrev = true;
    sDrawM = 0.0f;
    sAimLatchLive = false;
    sAimLatchTicks = 0;
    sAimDotTargetLive = false;
    sBowShotPower = 0.0f;
    sBowClick = 0;
    sBowFullFelt = false;
}

// Nock-point icon: a miniature Deku Nut (the classic drop model, gameplay_keep so it is
// always loaded) rendered in-world at the nock anchor while the bow/slingshot is out and no
// nock is drawn. It grows when the string hand is in pinch reach. Hidden by default
// (gVrArcheryShowIcon). DELIBERATELY minimal gates otherwise (mode + cvar + weapon out, none
// of the input-side availability checks): the icon is a check that the nock sits on the model
// and a liveness diagnostic — it must show even when the input gates are the thing that is
// broken. extern "C" linkage is load-bearing for the block-scope FrameInterpolation
// declarations inside OPEN_DISPS (see VrItemSelect_Draw).
extern "C" void VrArchery_DrawNockIcon(void) {
    const bool gallery = ShootingGalleryWeaponReady();
    if (gPlayState == NULL || !CVarGetInteger("gVrArcheryShowIcon", 0) || !CVarGetInteger("gVrPhysArchery", 1) ||
        (!VrItemSelect_ModeActive() && !gallery) || sNocked) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == NULL || (!gallery &&
        !((player->heldItemAction >= PLAYER_IA_BOW && player->heldItemAction <= PLAYER_IA_BOW_0E) ||
          player->heldItemAction == PLAYER_IA_SLINGSHOT))) {
        return;
    }
    float anchor[3];
    if (!NockTargetWorld(anchor)) {
        return;
    }

    OPEN_DISPS(gPlayState->state.gfxCtx);
    FrameInterpolation_RecordOpenChild((const void*)&sNocked, 0);
    Matrix_Translate(anchor[0], anchor[1], anchor[2], MTXMODE_NEW);
    Matrix_ReplaceRotation(&gPlayState->billboardMtxF);
    // Base size = the drop actor's 0.03 scaled by the user's percent; grows when in reach.
    float iconScale = 0.0003f * CVarGetFloat("gVrArcheryIconScale", 25.0f);
    if (NearBow()) {
        iconScale *= 1.4f;
    }
    Matrix_Scale(iconScale, iconScale, iconScale, MTXMODE_APPLY);
    POLY_OPA_DISP = Play_SetFog(gPlayState, POLY_OPA_DISP);
    POLY_OPA_DISP = Gfx_SetupDL_66(POLY_OPA_DISP);
    gSPSegment(POLY_OPA_DISP++, 0x08, (uintptr_t)SEGMENTED_TO_VIRTUAL(gDropDekuNutTex));
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(gPlayState->state.gfxCtx), G_MTX_MODELVIEW | G_MTX_LOAD);
    gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gItemDropDL);
    FrameInterpolation_RecordCloseChild();
    CLOSE_DISPS(gPlayState->state.gfxCtx);
}

// The nock is a held item button: nonzero exactly while nocked, so padmgr ORs the weapon's
// button in as raw state and the vanilla draw/hold/release path just runs (the same trick the
// selector's trigger mirror uses). Mirrors z_player.c sItemButtons order.
extern "C" uint16_t VrArchery_ItemButtonMask(void) {
    if (!sNocked || gPlayState == NULL) {
        return 0;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (!VrArchery_Covers(player)) {
        return 0;
    }
    // The gallery owns a temporary B-button ammo counter and starts the forced weapon from B,
    // regardless of where that item is equipped in the player's ordinary loadout.
    if (ShootingGalleryActive()) {
        return BTN_B;
    }
    static const uint16_t kItemButtons[] = { BTN_B,   BTN_CLEFT, BTN_CDOWN, BTN_CRIGHT,
                                             BTN_DUP, BTN_DDOWN, BTN_DLEFT, BTN_DRIGHT };
    const int slot = player->heldItemButton;
    return (slot >= 0 && slot < (int)(sizeof(kItemButtons) / sizeof(kItemButtons[0]))) ? kItemButtons[slot] : 0;
}

extern "C" bool VrArchery_GalleryOwnsWeapon(void) {
    return gPlayState != NULL && ShootingGalleryActive() && VrArchery_Covers(GET_PLAYER(gPlayState));
}

// Optional world-space aim dot. It follows the exact physical string/pouch line used to place and
// launch the prepared projectile, and sits on the first world surface along that line. No flat-mode
// or classic-control path reaches this function because VrArchery_AimSegment requires a live VR nock.
extern "C" void VrArchery_DrawAimDot(void) {
    if (gPlayState == NULL || !VR_IsInitialized() || !VR_GetFirstPerson() || VR_IsFlatScreen() ||
        !CVarGetInteger("gVrArcheryAimDot", 1) || !sNocked) {
        return;
    }
    float ray[6];
    if (!VrArchery_AimSegment(ray)) {
        return;
    }
    Vec3f start = { ray[0], ray[1], ray[2] };
    Vec3f end;
    Vec3f hit = start;
    CollisionPoly* poly = NULL;
    s32 bgId = BGCHECK_SCENE;
    bool foundHit = false;
    if (HoldsBowNow() && CVarGetInteger("gVrBowDrawPower", 1)) {
        // Match EnArrow's partial-draw flight instead of promising a straight-line hit. This is
        // especially important on Epona, where the player's practical draw was often ~50-60%.
        float norm = BowFullDrawM() > 0.01f ? sDrawM / BowFullDrawM() : 1.0f;
        norm = norm > 1.0f ? 1.0f : norm;
        Player* player = GET_PLAYER(gPlayState);
        const float power = (player != NULL && (player->stateFlags1 & PLAYER_STATE1_ON_HORSE))
                                ? 1.0f
                                : 0.3f + 0.7f * norm;
        const float speed = 150.0f * power;
        const float gravity = power < 0.99f ? -(1.0f - power) / 0.7f : 0.0f;
        Vec3f pos = start;
        Vec3f velocity = { ray[3] * speed, ray[4] * speed, ray[5] * speed };
        const int life = 12 + (power < 0.99f ? (int)(12.0f * ((1.0f - power) / 0.7f)) : 0);
        for (int frame = 0; frame < life; frame++) {
            // EnArrow applies its normal -0.4 gravity once its 12-frame full-power lifetime
            // falls below 7.2. Partial shots retain their stronger release gravity instead.
            const bool horseBow = player != NULL && (player->stateFlags1 & PLAYER_STATE1_ON_HORSE);
            velocity.y += horseBow ? 0.0f : ((power < 0.99f) ? gravity : (frame >= 4 ? -0.4f : 0.0f));
            end = { pos.x + velocity.x, pos.y + velocity.y, pos.z + velocity.z };
            if (BgCheck_AnyLineTest3(&gPlayState->colCtx, &pos, &end, &hit, &poly, true, true, true, true, &bgId)) {
                foundHit = true;
                break;
            }
            pos = end;
        }
        if (!foundHit) {
            hit = end;
        }
    } else {
        end = { start.x + ray[3] * 4000.0f, start.y + ray[4] * 4000.0f,
                start.z + ray[5] * 4000.0f };
        if (!BgCheck_AnyLineTest3(&gPlayState->colCtx, &start, &end, &hit, &poly, true, true, true, true, &bgId)) {
            hit = end;
        }
    }

    const float distance = sqrtf(SQ(hit.x - start.x) + SQ(hit.y - start.y) + SQ(hit.z - start.z));
    sAimDotTarget = hit;
    sAimDotTargetLive = true;
    float ws = WorldScale();
    const float half = fmaxf(0.006f * ws, 0.00375f * distance);
    const float scale = half / 16.0f;
    const s16 p = 16;
    const Vtx dot[4] = {
        { { { 0, p, 0 }, 0, { 0, 0 }, { 255, 235, 70, 245 } } },
        { { { p, 0, 0 }, 0, { 0, 0 }, { 255, 235, 70, 245 } } },
        { { { -p, 0, 0 }, 0, { 0, 0 }, { 255, 235, 70, 245 } } },
        { { { 0, -p, 0 }, 0, { 0, 0 }, { 255, 235, 70, 245 } } },
    };
    for (int i = 0; i < 4; i++) {
        sAimDotVtx[i] = dot[i];
    }

    FrameInterpolation_RecordOpenChild((const void*)sAimDotDl, 0);
    Matrix_Translate(hit.x, hit.y, hit.z, MTXMODE_NEW);
    Matrix_ReplaceRotation(&gPlayState->billboardMtxF);
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    Mtx* dotMtx = MATRIX_NEWMTX(gPlayState->state.gfxCtx);
    FrameInterpolation_RecordCloseChild();

    Gfx* pDl = sAimDotDl;
    gDPPipeSync(pDl++);
    gDPSetCycleType(pDl++, G_CYC_1CYCLE);
    gDPSetRenderMode(pDl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(pDl++, G_CC_SHADE, G_CC_SHADE);
    gSPTexture(pDl++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gSPClearGeometryMode(pDl++, G_CULL_BOTH | G_LIGHTING | G_FOG | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_ZBUFFER);
    gSPSetGeometryMode(pDl++, G_SHADE | G_SHADING_SMOOTH);
    gSPMatrix(pDl++, dotMtx, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPVertex(pDl++, (uintptr_t)sAimDotVtx, 4, 0);
    gSP2Triangles(pDl++, 0, 2, 1, 0, 1, 2, 3, 0);
    gSPEndDisplayList(pDl++);

    OPEN_DISPS(gPlayState->state.gfxCtx);
    gSPDisplayList(POLY_XLU_DISP++, sAimDotDl);
    CLOSE_DISPS(gPlayState->state.gfxCtx);
}

// The string hand's pinch input loses its normal binding only when it is about to nock (near
// the weapon) or currently drawn — a pinch elsewhere keeps whatever it is bound to.
extern "C" bool VrArchery_PinchConsumed(int32_t vrHand, uint16_t vrBtnMask) {
    if (gPlayState == NULL || VrOcarina_InPlay()) {
        return false;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (CVarGetInteger("gVrBowTriggerArrowCycle", 0) && vrHand == BowHand() &&
        (vrBtnMask & VR_BTN_TRIGGER) && HoldsBowNow() && VrArchery_Covers(player)) {
        return true;
    }
    if (vrHand != StringHand() || !(vrBtnMask & PinchMask())) {
        return false;
    }
    if (!VrArchery_Covers(player)) {
        return false;
    }
    return sNocked || NearBow();
}

// While nocked: shot origin at the nock anchor, direction string hand -> anchor — the angle
// you pull the string back to is the angle the shot leaves at, exactly like a real string.
// False when idle or the pull is too short to define a stable line, which lets the caller
// fall back to the one-hand aim ray.
extern "C" bool VrArchery_AimSegment(float* outPosDir6) {
    if (gPlayState == NULL) {
        return false;
    }
    // Post-release window: the shot is leaving through the vanilla release path — keep
    // serving the pull line captured while drawn, or the fallback ray would overwrite it.
    if (!sNocked) {
        if (sAimLatchTicks > 0 && sAimLatchLive) {
            for (int i = 0; i < 6; i++) {
                outPosDir6[i] = sAimLatch[i];
            }
            return true;
        }
        return false;
    }
    if (!VrArchery_Covers(GET_PLAYER(gPlayState))) {
        return false;
    }
    if (HoldsBowNow()) {
        // Bow: the arrow lies on the string at the nock and runs through the arrow rest; its
        // actor origin sits kArrowNockBack ahead of its nock end, so the fletching meets the
        // string and the shot leaves exactly along the nock -> rest line.
        float nock[3], dir[3], drawM;
        if (!BowNock(nock, dir, &drawM)) {
            if (sAimLatchLive) {
                for (int i = 0; i < 6; i++) {
                    outPosDir6[i] = sAimLatch[i];
                }
                return true;
            }
            return false;
        }
        for (int i = 0; i < 3; i++) {
            sAimLatch[i] = nock[i] + dir[i] * kArrowNockBack;
            sAimLatch[3 + i] = dir[i];
        }
        for (int i = 0; i < 6; i++) {
            outPosDir6[i] = sAimLatch[i];
        }
        sAimLatchLive = true;
        return true;
    }
    float s[3], rot[4], a[3];
    if (!VR_GetHandPose(StringHand(), s, rot) || !NockAnchorWorld(a)) {
        return false;
    }
    float d2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        const float d = a[i] - s[i];
        d2 += d * d;
    }
    const float minLine = 0.03f * WorldScale(); // hand at the anchor: no stable aim line
    if (d2 < minLine * minLine) {
        // Too short for a fresh line; the last good one (if any) still stands.
        if (sAimLatchLive) {
            for (int i = 0; i < 6; i++) {
                outPosDir6[i] = sAimLatch[i];
            }
            return true;
        }
        return false;
    }
    const float len = std::sqrt(d2);
    for (int i = 0; i < 3; i++) {
        sAimLatch[i] = a[i];
        sAimLatch[3 + i] = (a[i] - s[i]) / len;
        outPosDir6[i] = sAimLatch[i];
        outPosDir6[3 + i] = sAimLatch[3 + i];
    }
    sAimLatchLive = true;
    return true;
}

// Bow string visual: the live nock point (the string hand, stopped at maximum draw) while a bow
// nock is drawn. The z_player_lib string block shears the string DL so its apex lands here.
extern "C" bool VrArchery_BowStringNock(float* out3) {
    if (!sNocked || gPlayState == NULL || !HoldsBowNow() || !VrArchery_Covers(GET_PLAYER(gPlayState))) {
        return false;
    }
    float dir[3], drawM;
    return BowNock(out3, dir, &drawM);
}

// Slingshot band visual: where the pouch is drawn while a slingshot nock is drawn — the string
// hand, but the band stretches only so far (1.25 x full draw from the braced pouch, the bow's
// rule); past that the pouch stops and the hand keeps going. The z_player_lib band block maps
// the band DL's apex exactly onto this point.
extern "C" bool VrArchery_SlingshotPouch(float* out3) {
    if (!sNocked || gPlayState == NULL || HoldsBowNow() || !VrArchery_Covers(GET_PLAYER(gPlayState))) {
        return false;
    }
    float s[3], rot[4], a[3];
    if (!VR_GetHandPose(StringHand(), s, rot) || !NockAnchorWorld(a)) {
        return false;
    }
    float d[3];
    float d2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        d[i] = s[i] - a[i];
        d2 += d[i] * d[i];
    }
    const float len = std::sqrt(d2);
    const float maxLen = CVarGetFloat("gVrArcheryFullDraw", 45.0f) * 0.01f * 1.25f * WorldScale();
    const float k = (len > maxLen && len > 1e-6f) ? maxLen / len : 1.0f;
    for (int i = 0; i < 3; i++) {
        out3[i] = a[i] + d[i] * k;
    }
    return true;
}

// Bow hand limb draw: replaces the raw controller matrix (engine MtxF layout, 16 floats) with
// the drawn-bow alignment. False = leave the hand on its controller (not the bow hand, no nock
// drawn, alignment off, or the string hand ahead of the riser).
extern "C" bool VrArchery_BowAlignedMatrix(int32_t vrHand, float* mf16) {
    if (gPlayState == NULL || vrHand != BowHand() || !HoldsBowNow() ||
        !VrArchery_Covers(GET_PLAYER(gPlayState))) {
        return false;
    }
    return AlignBowMatrix(reinterpret_cast<float(*)[4]>(mf16));
}

// The nocked arrow is drawn by EnArrow from world.pos/shape.rot, which the aim override writes at
// game rate — so it trailed the bow, which renders at headset rate. While a bow nock is drawn the
// arrow's draw matrix is welded to the bow hand, like the string: bow, string, arrow move as one.
extern "C" int32_t VrArchery_NockedArrowHand(void) {
    if (!sNocked || gPlayState == NULL || !HoldsBowNow() || !VrArchery_Covers(GET_PLAYER(gPlayState))) {
        return -1;
    }
    // Horseback uses the same arrow-to-aligned-bow weld as on foot. AlignBowMatrix has already
    // rebuilt the mounted bow frame from hand positions and world-up, so no controller rotation
    // can leak through this weld.
    return BowHand();
}

extern "C" int32_t VrArchery_NockedArrowHandForActor(Actor* arrow) {
    if (arrow == NULL || gPlayState == NULL) {
        return -1;
    }
    Player* player = GET_PLAYER(gPlayState);
    return player->heldActor == arrow && arrow->parent == &player->actor ? VrArchery_NockedArrowHand() : -1;
}

// Draw strength -> arrow power, the VR-bow convention: a full draw is the vanilla shot exactly;
// a partial draw leaves slower and drops. Consumed once by EnArrow_Shoot at the vanilla release
// that follows a firing bow release; 0 = not a physical bow shot, leave the shot vanilla.
extern "C" float VrArchery_TakeBowShotPower(void) {
    if (sBowShotPower <= 0.0f) {
        sBowShotPower = 0.0f;
        return 0.0f;
    }
    const float p = sBowShotPower;
    sBowShotPower = 0.0f;
    return p;
}

extern "C" bool VrArchery_TakeReleasedAim(float* outPosDir6) {
    if (outPosDir6 == NULL || !sAimLatchLive || sBowShotPower <= 0.0f) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        outPosDir6[i] = sAimLatch[i];
    }
    // The launch path has its copy. It must not leak into a later vanilla projectile.
    sAimLatchLive = false;
    sAimLatchTicks = 0;
    return true;
}

namespace {

void ArcheryTick() {
    if (gPlayState == NULL || !GameInteractor::IsSaveLoaded(true)) {
        VrArchery_Reset();
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (!VrArchery_Covers(player)) {
        VrArchery_Reset();
        return;
    }
    if (gPlayState->pauseCtx.state != 0) {
        // Absorb input edges across the pause; state (nocked/idle) resumes on unpause.
        sPinchPrev = (VR_GetControllerButton(StringHand()) & PinchMask()) != 0;
        return;
    }

    if (sAimLatchTicks > 0 && !sNocked) {
        sAimLatchTicks--;
        if (sAimLatchTicks == 0 && sBowShotPower <= 0.0f) {
            sAimLatchLive = false;
        }
    }

    const bool pinch = (VR_GetControllerButton(StringHand()) & PinchMask()) != 0;
    const bool pressed = pinch && !sPinchPrev;
    sPinchPrev = pinch;

    const bool bow = HoldsBow(player);
    const bool cycleHeld = bow && CVarGetInteger("gVrBowTriggerArrowCycle", 0) &&
                           (VR_GetControllerButton(BowHand()) & VR_BTN_TRIGGER) != 0;
    const bool cyclePressed = cycleHeld && !sCycleTriggerPrev;
    sCycleTriggerPrev = cycleHeld;
    if (cyclePressed && sNocked && ArrowCycleMain()) {
        VR_TriggerHaptic(BowHand(), 0.45f, 0.0f, 25.0f);
    }
    const float gap = CurrentDrawM();
    if (gap >= 0.0f) {
        sDrawM = gap;
    }

    if (!sNocked) {
        // Feelable affordance: a soft tick the moment the string hand enters nock reach
        // (pairs with the visual marker lighting up).
        static bool sWasNear = false;
        const bool near = NearBow();
        if (near && !sWasNear) {
            VR_TriggerHaptic(StringHand(), 0.2f, 0.0f, 15.0f);
        }
        sWasNear = near;
        if (pressed && near) {
            sNocked = true;
            sBowClick = 0;
            sBowFullFelt = false;
            VR_TriggerHaptic(StringHand(), 0.4f, 0.0f, 30.0f);
            VR_TriggerHaptic(BowHand(), 0.25f, 0.0f, 30.0f);
        }
        return;
    }

    if (pinch && bow) {
        // Bow draw: steady string tension that builds with the pull, a light click every few
        // centimeters (the limbs loading), and one firm pulse in both hands at full draw.
        const float full = BowFullDrawM();
        float norm = full > 0.01f ? sDrawM / full : 1.0f;
        if (norm > 1.0f) {
            norm = 1.0f;
        }
        VR_TriggerHaptic(StringHand(), 0.06f + 0.34f * norm, 0.0f, 15.0f + 25.0f * norm);
        VR_TriggerHaptic(BowHand(), 0.04f + 0.16f * norm, 0.0f, 15.0f);
        const int click = (int)(sDrawM / 0.05f);
        if (click > sBowClick && norm < 1.0f) {
            VR_TriggerHaptic(StringHand(), 0.45f, 0.0f, 12.0f);
        }
        sBowClick = click;
        if (norm >= 1.0f && !sBowFullFelt) {
            VR_TriggerHaptic(StringHand(), 0.7f, 0.0f, 35.0f);
            VR_TriggerHaptic(BowHand(), 0.5f, 0.0f, 35.0f);
        }
        sBowFullFelt = norm >= 1.0f;
        return;
    }

    if (pinch) {
        // Draw ramp: tension you can feel, growing toward full draw.
        const float full = CVarGetFloat("gVrArcheryFullDraw", 45.0f) * 0.01f;
        float norm = full > 0.01f ? sDrawM / full : 1.0f;
        if (norm > 1.0f) {
            norm = 1.0f;
        }
        VR_TriggerHaptic(StringHand(), 0.08f + 0.3f * norm, 0.0f, 15.0f + 25.0f * norm);
        return;
    }

    // Pinch is up: state-based release (edge may have been absorbed by a pause).
    const float minDraw =
        bow ? CVarGetFloat("gVrBowMinDraw", 8.0f) * 0.01f : CVarGetFloat("gVrArcheryMinDraw", 10.0f) * 0.01f;
    if (sDrawM >= minDraw) {
        // The nock mask drops with sNocked this tick; the vanilla button release fires the
        // prepared shot NEXT tick — the aim latch keeps serving the drawn pull line through
        // that window so the fallback ray cannot overwrite the direction. Thunk both hands.
        // On foot the arrow detaches on the next update, but the horseback release animation
        // can keep it parented for considerably longer. Preserve the exact dot/string line
        // until that delayed horse arrow reaches EnArrow_Shoot; otherwise it falls back to the
        // mounted animation pose and wrist orientation before velocity is calculated.
        sAimLatchTicks = (player->stateFlags1 & PLAYER_STATE1_ON_HORSE) ? 30 : 4;
        if (bow) {
            // Power from draw: 30% at no draw up to the full vanilla shot at full draw.
            float norm = sDrawM / BowFullDrawM();
            norm = norm > 1.0f ? 1.0f : norm;
            sBowShotPower = (player->stateFlags1 & PLAYER_STATE1_ON_HORSE)
                                ? 1.0f
                                : (CVarGetInteger("gVrBowDrawPower", 1) ? 0.3f + 0.7f * norm : 1.0f);
            // The dot is what the player actually aimed at in the headset. On Epona, a trigger
            // release can land between headset-rate poses and the 20 Hz game update; rebuilding
            // from that update's hand sample occasionally produced a large sideways shot. Make
            // the last displayed world-space dot authoritative for mounted straight-flight aim.
            if ((player->stateFlags1 & PLAYER_STATE1_ON_HORSE) && sAimLatchLive && sAimDotTargetLive) {
                const float dx = sAimDotTarget.x - sAimLatch[0];
                const float dy = sAimDotTarget.y - sAimLatch[1];
                const float dz = sAimDotTarget.z - sAimLatch[2];
                const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (len > 1e-4f) {
                    sAimLatch[3] = dx / len;
                    sAimLatch[4] = dy / len;
                    sAimLatch[5] = dz / len;
                }
            }
            // Commit the physical release to the prepared arrow itself. Mounted Link keeps the
            // arrow parented through a longer animation, during which ordinary archery state can
            // reset and the limb callback can rewrite its pose. Neither may alter this shot.
            if (sAimLatchLive && player->heldActor != NULL && player->heldActor->id == ACTOR_EN_ARROW) {
                EnArrow* arrow = (EnArrow*)player->heldActor;
                arrow->vrPhysicalBowShot = true;
                arrow->vrHorseBowShot = (player->stateFlags1 & PLAYER_STATE1_ON_HORSE) != 0;
                arrow->vrBowPower = sBowShotPower;
                arrow->vrBowAimOrigin = { sAimLatch[0], sAimLatch[1], sAimLatch[2] };
                arrow->vrBowAimDir = { sAimLatch[3], sAimLatch[4], sAimLatch[5] };
                // The actor owns both values now; do not leave a global release that a later
                // unrelated arrow could consume after the mounted animation resets archery.
                sBowShotPower = 0.0f;
            }
            // The string slaps the bow: the bow hand takes the bigger kick.
            VR_TriggerHaptic(BowHand(), 0.6f + 0.4f * norm, 0.0f, 50.0f);
            VR_TriggerHaptic(StringHand(), 0.5f, 0.0f, 30.0f);
        } else {
            VR_TriggerHaptic(BowHand(), 0.8f, 0.0f, 40.0f);
            VR_TriggerHaptic(StringHand(), 0.6f, 0.0f, 40.0f);
        }
    } else {
        // Too short to fire: clean cancel — arrow killed, ammo and magic preserved. No
        // latch: there is no shot in flight to protect.
        Player_VrCancelPreparedItem(gPlayState, player);
        sAimLatchLive = false;
    }
    sNocked = false;
}

void RegisterVrArchery() {
    COND_HOOK(OnPlayerUpdate, true, ArcheryTick);
    COND_HOOK(OnPlayDrawEnd, true, VrArchery_DrawNockIcon);
    COND_HOOK(OnPlayDrawEnd, true, VrArchery_DrawAimDot);
}

static RegisterShipInitFunc initVrArchery(RegisterVrArchery);

} // namespace
