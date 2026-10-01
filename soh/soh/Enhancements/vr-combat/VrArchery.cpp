extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "objects/gameplay_keep/gameplay_keep.h"
extern PlayState* gPlayState;
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
// length sets arrow power. Everything slingshot-specific (anchor sliders, apex calibration)
// stays the slingshot's alone.
//
// Deliberate fallbacks: shooting galleries, bombchu bowling and horseback keep their own
// schemes (Covers is false there), and gVrPhysArchery=0 restores the previous behavior where
// the weapon hand's trigger mirrors the item button.

namespace {

bool sNocked = false;
bool sPinchPrev = true; // require a fresh pinch after entry/reset
float sDrawM = 0.0f;    // current string-hand draw distance, meters

// The firing release is processed by the vanilla pad path a frame AFTER the tick clears the
// nock — and draws in between would fall back to the one-hand ray and overwrite the shot
// direction (the "shots ignore the pull" bug). So the last live pull line is latched at
// release and served for a few ticks, exactly long enough for the vanilla release to fire.
float sAimLatch[6] = {};
bool sAimLatchLive = false; // a nock has produced a valid latch this draw cycle
int sAimLatchTicks = 0;     // >0: keep serving the latch after the nock ended

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

// Rotate a vector by a unit quaternion (x, y, z, w) — same layout the runtime hands out.
void QuatRot(const float q[4], const float v[3], float out[3]) {
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

// The nock anchor: where the string physically lives on the weapon — the weapon-hand
// controller plus the user-tuned grip-local offset (cm sliders; right/up/forward in the
// controller's own frame, forward = -Z as OpenXR defines it). The marker shows this point,
// reach and draw distance measure against it, and the shot flies string-hand -> anchor.
bool NockAnchorWorld(float* out3) {
    float pos[3], rot[4];
    if (!VR_GetHandPose(BowHand(), pos, rot)) {
        return false;
    }
    const float u = 0.01f * WorldScale(); // cm -> game units
    float mirror = CVarGetInteger("gVrLeftHanded", 0) ? -1.0f : 1.0f;
    // Defaults are the headset-tuned slingshot values (September 14, 2026).
    const float local[3] = { CVarGetFloat("gVrArcheryAnchorRight", 4.0f) * u * mirror,
                             CVarGetFloat("gVrArcheryAnchorUp", -5.0f) * u,
                             -CVarGetFloat("gVrArcheryAnchorFwd", 17.0f) * u };
    float off[3];
    QuatRot(rot, local, off);
    for (int i = 0; i < 3; i++) {
        out3[i] = pos[i] + off[i];
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
// The slingshot's pouch-and-fork geometry is a tuned controller offset; the bow instead reads its
// geometry straight off the model it renders (object_link_boy, measured September 30, 2026): in
// the bow hand's limb frame the riser runs along model X (bow tips at X = +-2010, string ends at
// +-1483), the arrow flies along +Y, and the string rests 360.4 behind the grip (sBowStringData).
// The string DL's apex vertex — the nocking point — sits at X = 360 (the top of the fist) and
// 1160 further along -Y at the vanilla pull. So, like every VR bow: the arrow rest is the grip at
// nock height, the arrow runs from the string hand through that rest, the string pulls to the
// hand, and how far you draw decides the shot's power.
constexpr float kBowNockX = 360.0f;     // model units, riser height of the nock / arrow rest
constexpr float kBowStringY = -360.4f;  // model units, braced string behind the grip
constexpr float kArrowNockBack = 3.96f; // world units: EnArrow's model nock end sits 396 model
                                        // units (scale 0.01) behind the actor origin

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
    return gPlayState != NULL && HoldsBow(GET_PLAYER(gPlayState));
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

// Where the string hand nocks: the braced string for the bow, the tuned anchor for the slingshot.
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
    if (!VrItemSelect_ModeActive() || !CVarGetInteger("gVrPhysArchery", 1) || player == NULL ||
        player->actor.category != ACTORCAT_PLAYER || gPlayState == NULL) {
        return false;
    }
    if (!VrItemSelect_SelectionAllowed()) {
        return false; // scripted control, transitions, ocarina: no nock, no button mask
    }
    if (gPlayState->shootingGalleryStatus != 0 || gPlayState->bombchuBowlingStatus != 0 ||
        (player->stateFlags1 & PLAYER_STATE1_ON_HORSE)) {
        return false; // galleries, bowling and horseback keep their dedicated schemes
    }
    return (player->heldItemAction >= PLAYER_IA_BOW && player->heldItemAction <= PLAYER_IA_BOW_0E) ||
           player->heldItemAction == PLAYER_IA_SLINGSHOT;
}

extern "C" void VrArchery_Reset(void) {
    sNocked = false;
    sPinchPrev = true;
    sDrawM = 0.0f;
    sAimLatchLive = false;
    sAimLatchTicks = 0;
    sBowShotPower = 0.0f;
    sBowClick = 0;
    sBowFullFelt = false;
}

// Nock-point icon: a miniature Deku Nut (the classic drop model, gameplay_keep so it is
// always loaded) rendered in-world at the nock anchor while the bow/slingshot is out and no
// nock is drawn. It grows when the string hand is in pinch reach. DELIBERATELY minimal gates
// (mode + cvar + weapon out, none of the input-side availability checks): the icon is a
// tuning target for the anchor sliders and a liveness diagnostic — it must show even when
// the input gates are the thing that is broken. extern "C" linkage is load-bearing for the
// block-scope FrameInterpolation declarations inside OPEN_DISPS (see VrItemSelect_Draw).
extern "C" void VrArchery_DrawNockIcon(void) {
    if (gPlayState == NULL || !CVarGetInteger("gVrPhysArchery", 1) || !VrItemSelect_ModeActive() || sNocked) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == NULL ||
        !((player->heldItemAction >= PLAYER_IA_BOW && player->heldItemAction <= PLAYER_IA_BOW_0E) ||
          player->heldItemAction == PLAYER_IA_SLINGSHOT)) {
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

// True while a nock is drawn — the string presentation renders pulled to the string hand.
extern "C" bool VrArchery_StringNocked(void) {
    return sNocked && gPlayState != NULL && VrArchery_Covers(GET_PLAYER(gPlayState));
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
    static const uint16_t kItemButtons[] = { BTN_B,   BTN_CLEFT, BTN_CDOWN, BTN_CRIGHT,
                                             BTN_DUP, BTN_DDOWN, BTN_DLEFT, BTN_DRIGHT };
    const int slot = player->heldItemButton;
    return (slot >= 0 && slot < (int)(sizeof(kItemButtons) / sizeof(kItemButtons[0]))) ? kItemButtons[slot] : 0;
}

// The string hand's pinch input loses its normal binding only when it is about to nock (near
// the weapon) or currently drawn — a pinch elsewhere keeps whatever it is bound to.
extern "C" bool VrArchery_PinchConsumed(int32_t vrHand, uint16_t vrBtnMask) {
    if (gPlayState == NULL || vrHand != StringHand() || !(vrBtnMask & PinchMask()) || VrOcarina_InPlay()) {
        return false;
    }
    if (!VrArchery_Covers(GET_PLAYER(gPlayState))) {
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
        // gVrBowArrowOffset (cm, + = toward the bow) slides the arrow model along its own line so
        // its nock can be matched to the drawn string by eye; the shot line is unchanged.
        const float along = kArrowNockBack + CVarGetFloat("gVrBowArrowOffset", 0.0f) * 0.01f * WorldScale();
        for (int i = 0; i < 3; i++) {
            sAimLatch[i] = nock[i] + dir[i] * along;
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
    return BowHand();
}

// Draw strength -> arrow power, the VR-bow convention: a full draw is the vanilla shot exactly;
// a partial draw leaves slower and drops. Consumed once by EnArrow_Shoot at the vanilla release
// that follows a firing bow release; 0 = not a physical bow shot, leave the shot vanilla.
extern "C" float VrArchery_TakeBowShotPower(void) {
    if (sBowShotPower <= 0.0f || sAimLatchTicks <= 0) {
        sBowShotPower = 0.0f;
        return 0.0f;
    }
    const float p = sBowShotPower;
    sBowShotPower = 0.0f;
    return p;
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
        if (sAimLatchTicks == 0) {
            sAimLatchLive = false;
        }
    }

    const bool pinch = (VR_GetControllerButton(StringHand()) & PinchMask()) != 0;
    const bool pressed = pinch && !sPinchPrev;
    sPinchPrev = pinch;

    const bool bow = HoldsBow(player);
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
        sAimLatchTicks = 4;
        if (bow) {
            // Power from draw: 30% at no draw up to the full vanilla shot at full draw.
            float norm = sDrawM / BowFullDrawM();
            norm = norm > 1.0f ? 1.0f : norm;
            sBowShotPower = CVarGetInteger("gVrBowDrawPower", 1) ? 0.3f + 0.7f * norm : 1.0f;
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
}

static RegisterShipInitFunc initVrArchery(RegisterVrArchery);

} // namespace
