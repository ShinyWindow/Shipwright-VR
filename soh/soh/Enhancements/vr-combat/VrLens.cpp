extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "objects/object_gi_glasses/object_gi_glasses.h"
extern PlayState* gPlayState;
#include "variables.h" // gSaveContext
}
#include "VrCombat.h"
#include "VrPocket.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/frame_interpolation.h"
#include "soh/cvar_prefixes.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <cmath>

// Physical Lens of Truth (VR first person, selector mode). User spec (October 1, 2026): "select
// it, and then hold it up to your face, and it'll be enabled that way. i want the lense that you
// look through to be in actual world space right in front of your face ... when it's selected,
// it appears in front of you, then you hold it up to your face and it attaches. and then it'll
// stay attached to your face until you take it off or until you deselect the item".
//
// Vanilla has no held lens: Player_UseItem toggles play->actorCtx.lensActive behind
// Magic_RequestChange(MAGIC_CONSUME_LENS), the interface drains magic while it is on, and the
// reveal is Actor_DrawLensActors masking the lens actors with a SCREEN-space circle
// (Actor_DrawLensOverlay). This module replaces only the toggle and the circle's geometry:
//
// POCKET  The lens is selected in normal play: the get-item model (gGiLensDL frame + handle,
//         gGiLensGlassDL glass) sits at the bombchu/boomerang pocket point, shrunk by
//         gVrLensPreviewScale. The trigger stands down (no vanilla toggle).
// HELD    A FRESH grip press by either hand within reach takes it, real size (glass radius
//         gVrLensRadius), riding the hand rigidly (VrPocket::Grab), welded to the live controller
//         pose at render time. Grip release puts it back in the pocket.
// WORN    Bringing the glass within gVrLensWearDistance of the worn spot (gVrLensDistance in front
//         of the eyes, gVrLensSide / gVrLensHeight off center) puts it on — whether the grip is
//         still closed or not. It is then glued to the HEAD (libultraship head-child matrices: the
//         rendered center eye x a head-local transform), and the lens turns on through the vanilla
//         gate (Magic_RequestChange). The vanilla drain runs unchanged. Like vanilla's toggle (Link's
//         hands stay free), a worn lens STAYS ON through item switches, the sword, anything else in
//         hand (user, October 5). It comes off only when: a fresh grip press with a hand near it
//         takes it off (into that hand if the lens is the selected item, otherwise just off); the
//         magic runs out; or the lens is no longer on any C / D-pad button (vanilla's own "!hasLens"
//         rule). It must leave the face zone before it can go back on.
//
// The aperture: while worn, Actor_DrawLensOverlay draws the vanilla mask texture, render modes and
// combiners untouched, on a quad in the glass's own plane instead of a screen rect, with the mask
// circle's alpha-threshold edge (54.4 texels from the mirror center — measured from
// gLensOfTruthMaskTex) mapped onto the glass's edge. Prim depth still stamps 0, so the vanilla
// masking works as before, but the circle is where the glass is: each eye looks through it from
// its own side, exactly like a real lens in front of the face.
//
// Worn and the reveal goes off underneath it (magic ran out, a door, a cutscene, a textbox), the
// lens stays on the face and comes back on by itself once vanilla would allow it again (the same
// conditions the drain checks). Fallback: gVrPhysLens=0 restores the trigger toggle.

namespace {

enum class State { None, Pocket, Held, Worn };

// Get-item model facts (object_gi_glasses, measured from the vertex data): the glass is a disc in
// the model's XY plane, radius 15, centered 7 above the origin; the handle runs down -Y.
constexpr float kGlassRadiusModel = 15.0f;
constexpr float kGlassCenterYModel = 7.0f;
// The mask texture's circle edge (alpha >= 8/256, the shader's threshold), in texels from the
// mirror center, and the center itself in the tile coordinates Actor_DrawLensOverlay sets up.
constexpr float kMaskRadiusTexels = 54.4f;
constexpr float kMaskCenterS = 160.0f;
constexpr float kMaskCenterT = 120.0f;
// The aperture quad extends this many glass radii from the center (stays inside the s10.5 range:
// (160 + 15 * 54.4) * 32 < 32768) — ~85 degrees off-axis at the default distance.
constexpr float kApertureExtentRadii = 15.0f;
constexpr int kRetryCooldownTicks = 20;

State sState = State::None;
int sCarryHand = -1;
bool sGripPrev[2] = { true, true };
int sObservedItem = -1;
VrPocket::Grab sGrab;
bool sArmed = false;      // HELD: the glass has been away from the face since it was last taken off
bool sOurs = false;       // the lens is on because this module turned it on
bool sLensPrev = false;   // WORN: lensActive last tick (vanilla turning it off starts the cooldown)
int sRetryCooldown = 0;
int sPocketInterpKey; // address only: frame-interpolation identities for the draws
int sHeldInterpKey;
int sWornInterpKey;
VrLensDebug sDebug = { 0, 0, -1, 0, -1.0f, 0.0f, 0, -1, 0 };

int SwordHandIdx() {
    return CVarGetInteger("gVrLeftHanded", 0) ? VR_HAND_LEFT : VR_HAND_RIGHT;
}

bool SwapChord() {
    const auto mask = (uint16_t)CVarGetInteger("gVrItemSelSwapInput", VR_BTN_GRIP);
    return mask && (VR_GetControllerButton(0) & mask) && (VR_GetControllerButton(1) & mask);
}

bool GripDown(int hand) {
    return hand >= 0 && (VR_GetControllerButton(hand) & VR_BTN_GRIP) != 0;
}

float CmToUnits(float cm) {
    return cm * 0.01f * VrPocket::WorldScale();
}

float GlassRadiusUnits() {
    return CmToUnits(CVarGetFloat("gVrLensRadius", 5.0f));
}

// Model scale for the real-size lens (in the hand).
float LensScale() {
    return GlassRadiusUnits() / kGlassRadiusModel;
}

// On the face the lens has its own size (gVrLensWornRadius), so it can be worn bigger than it is
// in the hand; it changes size as it goes on and comes off.
float WornRadiusUnits() {
    return CmToUnits(CVarGetFloat("gVrLensWornRadius", 6.2f));
}

float WornScale() {
    return WornRadiusUnits() / kGlassRadiusModel;
}

// Putting it on is "hold it up to your face": measured against a spot no further than this in
// front of the eyes, however far out the worn lens sits (gVrLensDistance).
constexpr float kPutOnSpotMaxCm = 8.0f;

// The glass center in the head frame (+X right, +Y up, -Z forward), game units. putOnSpot = the
// spot the held glass is brought to (the worn spot, pulled in to kPutOnSpotMaxCm).
void WornCenterLocal(float out[3], bool putOnSpot = false) {
    float distanceCm = CVarGetFloat("gVrLensDistance", 8.6f);
    if (putOnSpot && distanceCm > kPutOnSpotMaxCm) distanceCm = kPutOnSpotMaxCm;
    out[0] = CmToUnits(CVarGetFloat("gVrLensSide", 0.0f));
    out[1] = CmToUnits(CVarGetFloat("gVrLensHeight", 0.0f));
    out[2] = -CmToUnits(distanceCm);
}

// Where the worn glass center is in the world this tick, plus the head's axes (right, up, fwd).
bool WornCenterWorld(float out[3], float right[3], float up[3], float fwd[3], bool putOnSpot = false) {
    if (!VR_IsInitialized()) return false;
    float eye[3];
    VR_GetCameraPose(eye, fwd, up);
    right[0] = fwd[1] * up[2] - fwd[2] * up[1];
    right[1] = fwd[2] * up[0] - fwd[0] * up[2];
    right[2] = fwd[0] * up[1] - fwd[1] * up[0];
    float local[3];
    WornCenterLocal(local, putOnSpot);
    for (int i = 0; i < 3; ++i) out[i] = eye[i] + right[i] * local[0] + up[i] * local[1] - fwd[i] * local[2];
    return true;
}

float Distance(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool HeldPose(float position[3], float left[3], float up[3], float forward[3]) {
    return sCarryHand >= 0 && sGrab.Replay(sCarryHand, position, left, up, forward);
}

bool HeldGlassCenter(float out[3]) {
    float position[3], left[3], up[3], forward[3];
    if (!HeldPose(position, left, up, forward)) return false;
    const float lift = kGlassCenterYModel * LensScale();
    for (int i = 0; i < 3; ++i) out[i] = position[i] + up[i] * lift;
    return true;
}

// The worn model in the head frame: model axes = head axes, glass center on the worn spot.
void WornModelLocal(MtxF* local) {
    float center[3];
    WornCenterLocal(center);
    const float scale = WornScale();
    Matrix_Push();
    Matrix_Translate(center[0], center[1] - kGlassCenterYModel * scale, center[2], MTXMODE_NEW);
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    Matrix_Get(local);
    Matrix_Pop();
}

// A head-local transform as a drawable Mtx: substituted per eye with the rendered head; the
// stored value (the 20 Hz head) only shows if the head isn't located yet.
Mtx* HeadWeldedMtx(GraphicsContext* gfxCtx, MtxF* local) {
    MtxF head;
    MtxF world;
    VR_GetHeadMatrix(head.mf);
    SkinMatrix_MtxFMtxFMult(&head, local, &world);
    Mtx* mtx = (Mtx*)Graph_Alloc(gfxCtx, sizeof(Mtx));
    Matrix_MtxFToMtx(&world, mtx);
    VR_RegisterHeadChildMatrix(mtx, &local->mf[0][0]);
    return mtx;
}

void ClearCarry() {
    sCarryHand = -1;
    sGrab.Clear();
}

void LensOn(PlayState* play, bool loud) {
    if (play->actorCtx.lensActive) {
        sOurs = true;
        sDebug.lastActivate = 1;
        return;
    }
    if (Magic_RequestChange(play, 0, MAGIC_CONSUME_LENS)) {
        play->actorCtx.lensActive = true;
        sOurs = true;
        Sfx_PlaySfxCentered(NA_SE_SY_GLASSMODE_ON);
        sDebug.lastActivate = 1;
    } else {
        if (loud) Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
        sDebug.lastActivate = 0;
    }
}

void LensOff(PlayState* play) {
    if (play != nullptr && sOurs && play->actorCtx.lensActive) {
        Actor_DisableLens(play);
        Sfx_PlaySfxCentered(NA_SE_SY_GLASSMODE_OFF);
    }
    sOurs = false;
}

void EnterNone(PlayState* play) {
    LensOff(play);
    sState = State::None;
    ClearCarry();
    sGripPrev[0] = sGripPrev[1] = true;
}

void PutOn(PlayState* play, int hand) {
    sState = State::Worn;
    ClearCarry();
    sLensPrev = play->actorCtx.lensActive;
    sRetryCooldown = 0;
    VR_TriggerHaptic(hand, 0.6f, 0.0f, 50.0f);
    LensOn(play, true);
    sLensPrev = play->actorCtx.lensActive;
}

void TakeOff(PlayState* play, int hand) {
    float center[3], right[3], up[3], fwd[3];
    if (WornCenterWorld(center, right, up, fwd)) {
        // Capture the worn pose (model axes = head axes; model origin below the glass center) so
        // it comes off the face exactly where it was.
        const float drop = kGlassCenterYModel * LensScale();
        const float origin[3] = { center[0] - up[0] * drop, center[1] - up[1] * drop, center[2] - up[2] * drop };
        const float back[3] = { -fwd[0], -fwd[1], -fwd[2] };
        sGrab.Capture(hand, origin, right, up, back);
    } else {
        sGrab.Clear();
    }
    sCarryHand = hand;
    sState = State::Held;
    sArmed = false;
    LensOff(play);
    VR_TriggerHaptic(hand, 0.5f, 0.0f, 35.0f);
}

// WORN with the reveal off: back on once vanilla's own drain would keep it on (the conditions
// z_parameter.c checks in MAGIC_STATE_CONSUME_LENS), never faster than the cooldown after vanilla
// switched it off, so nothing can flap ON/OFF.
void RetryLens(PlayState* play) {
    if (sRetryCooldown > 0) {
        --sRetryCooldown;
        return;
    }
    const s32 hazard = Player_GetEnvironmentalHazard(play);
    if (gSaveContext.magic == 0 || play->msgCtx.msgMode != MSGMODE_NONE || play->pauseCtx.state != 0 ||
        play->gameOverCtx.state != GAMEOVER_INACTIVE || play->transitionTrigger != TRANS_TRIGGER_OFF ||
        play->transitionMode != TRANS_MODE_OFF || Play_InCsMode(play) || (hazard >= 2 && hazard < 5)) {
        return;
    }
    LensOn(play, false);
    if (!play->actorCtx.lensActive) sRetryCooldown = kRetryCooldownTicks;
}

// The lens is still in the loadout: on a C button (or the D-pad with DpadEquips), the same check
// vanilla's drain makes (z_parameter.c MAGIC_STATE_CONSUME_LENS).
bool LensOnButtons() {
    const int count = CVarGetInteger(CVAR_ENHANCEMENT("DpadEquips"), 0) != 0
                          ? (int)ARRAY_COUNT(gSaveContext.equips.buttonItems)
                          : 4;
    for (int i = 1; i < count; i++) {
        if (gSaveContext.equips.buttonItems[i] == ITEM_LENS) {
            return true;
        }
    }
    return false;
}

// A lens on the face may stay there whatever is in hand: physical lens mode is on, the player is
// the real player, and the lens is still equipped.
bool WornAllowed(Player* player) {
    return VrItemSelect_ModeActive() && CVarGetInteger("gVrPhysLens", 1) && player != nullptr &&
           player->actor.category == ACTORCAT_PLAYER && LensOnButtons();
}

bool WornOnFace() {
    return sState == State::Worn && gPlayState != nullptr && WornAllowed(GET_PLAYER(gPlayState));
}

// Another item owns this hand's grip right now (a bomb in it, the boomerang catch, the hammer's off
// hand, an archery pinch): a worn lens never comes off with it (VrMask's rule).
bool GripOwnedElsewhere(int hand) {
    return VrItemThrow_GripConsumed(hand, VR_BTN_GRIP) || VrBoomerang_GripConsumed(hand, VR_BTN_GRIP) ||
           VrHammer_GripConsumed(hand, VR_BTN_GRIP) || VrArchery_PinchConsumed(hand, VR_BTN_GRIP);
}

// Off the face without going into a hand (another item is selected): the lens just goes away.
void RemoveFromFace(PlayState* play, int hand) {
    LensOff(play);
    sState = State::None;
    ClearCarry();
    sArmed = false;
    if (hand >= 0) {
        VR_TriggerHaptic(hand, 0.5f, 0.0f, 35.0f);
    }
}

int StateIndex() {
    switch (sState) {
        case State::Pocket: return 1;
        case State::Held: return 2;
        case State::Worn: return 3;
        default: return 0;
    }
}

} // namespace

// extern "C" for the block-scope FrameInterpolation declarations inside OPEN_DISPS.
extern "C" void VrLens_DrawModel(GraphicsContext* gfxCtx, Mtx* mtx, bool glass) {
    OPEN_DISPS(gfxCtx);
    POLY_OPA_DISP = Play_SetFog(gPlayState, POLY_OPA_DISP);
    Gfx_SetupDL_25Opa(gfxCtx);
    gSPMatrix(POLY_OPA_DISP++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gGiLensDL);
    if (glass) {
        Gfx_SetupDL_25Xlu(gfxCtx);
        gSPMatrix(POLY_XLU_DISP++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList(POLY_XLU_DISP++, (Gfx*)gGiLensGlassDL);
    }
    CLOSE_DISPS(gfxCtx);
}

extern "C" bool VrLens_Covers(Player* player) {
    return VrItemSelect_ModeActive() && CVarGetInteger("gVrPhysLens", 1) && player != nullptr &&
           player->actor.category == ACTORCAT_PLAYER && player->heldItemAction == PLAYER_IA_LENS_OF_TRUTH &&
           player->heldItemAction == player->itemAction && gPlayState != nullptr &&
           gPlayState->bombchuBowlingStatus == 0 && gPlayState->shootingGalleryStatus == 0 &&
           !(player->stateFlags1 & (PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_IN_WATER));
}

extern "C" void VrLens_Reset(void) {
    // sOurs survives on purpose: the next tick turns a lens this module left on back off.
    sState = State::None;
    ClearCarry();
    sGripPrev[0] = sGripPrev[1] = true;
    sObservedItem = -1;
    sArmed = false;
    sRetryCooldown = 0;
}

extern "C" bool VrLens_PreviewIsModel(void) {
    // Worn while something else is in hand: that item's own preview / icon stands.
    return sState != State::None && (sState != State::Worn || (gPlayState != nullptr &&
                                                                VrLens_Covers(GET_PLAYER(gPlayState))));
}

extern "C" bool VrLens_GripConsumed(int32_t hand, uint16_t mask) {
    if (!(mask & VR_BTN_GRIP) || gPlayState == nullptr || VrOcarina_InPlay() || hand < 0 || hand > 1) {
        return false;
    }
    switch (sState) {
        case State::Pocket: {
            float point[3];
            return VrPocket::Point(point) && VrPocket::HandNear(hand, point);
        }
        case State::Held:
            return hand == sCarryHand;
        case State::Worn: {
            // Only a hand at the face (reaching to take it off) loses its binding.
            float center[3], right[3], up[3], fwd[3];
            return WornCenterWorld(center, right, up, fwd) && VrPocket::HandNear(hand, center);
        }
        default:
            return false;
    }
}

extern "C" void VrLens_GetDebug(VrLensDebug* out) {
    if (out != nullptr) *out = sDebug;
}

extern "C" void VrLens_Tick(PlayState* play, Player* player) {
    if (play == nullptr || player == nullptr || player->actor.category != ACTORCAT_PLAYER) return;
    const bool covers = VrLens_Covers(player);
    sDebug.wearDistanceCm = CVarGetFloat("gVrLensWearDistance", 10.0f);
    sDebug.lensActive = play->actorCtx.lensActive ? 1 : 0;

    // A lens on the face outlives the selection (WornAllowed); everything else needs it selected.
    const bool worn = (sState == State::Worn) && WornAllowed(player);
    int gate = 0;
    if (!CVarGetInteger("gVrPhysLens", 1)) {
        gate = 1;
    } else if (!covers && !worn) {
        gate = 2;
    } else if (!VrItemSelect_SelectionAllowed() || play->pauseCtx.state != 0 || player->unk_6AD != 0) {
        gate = 3;
    }
    sDebug.gate = gate;
    if (gate == 1 || gate == 2) {
        EnterNone(play);
        sDebug.state = 0;
        sDebug.carryHand = -1;
        return;
    }
    if (sState != State::Worn && sOurs) {
        LensOff(play); // left on across a reset (save state, mode toggle): nothing is on the face
    }
    if (gate == 3) {
        // Cutscene, textbox-driven cs mode, ocarina, pause, Link busy: hold position (a worn lens
        // stays on the face; vanilla may switch the reveal off meanwhile) and re-arm the grips.
        sGripPrev[0] = sGripPrev[1] = true;
        if (sState == State::Worn) sLensPrev = play->actorCtx.lensActive;
        sDebug.state = StateIndex();
        return;
    }
    if (sState == State::Worn && gSaveContext.magic == 0) {
        // Out of magic: the lens comes off the face (vanilla's drain already ended the reveal).
        RemoveFromFace(play, -1);
        Sfx_PlaySfxCentered(NA_SE_SY_GLASSMODE_OFF);
    }
    if (sState == State::None) {
        if (!covers) {
            sDebug.state = StateIndex();
            sDebug.carryHand = sCarryHand;
            return;
        }
        sState = State::Pocket;
        ClearCarry();
    }
    if (sObservedItem != player->heldItemId) {
        sObservedItem = player->heldItemId;
        sGripPrev[0] = sGripPrev[1] = true;
    }
    bool pressed[2], released[2];
    for (int hand = 0; hand < 2; ++hand) {
        const bool grip = GripDown(hand);
        pressed[hand] = grip && !sGripPrev[hand];
        released[hand] = !grip && sGripPrev[hand];
        sGripPrev[hand] = grip;
    }
    if (SwapChord() || VrItemSelect_PendingSlot() != -2) {
        // Switching: a lens in the hand goes back to the pocket; a worn one stays on the face
        // (WornAllowed), whatever the switch lands on.
        if (sState == State::Held) {
            sState = State::Pocket;
            ClearCarry();
        }
        sDebug.state = StateIndex();
        sDebug.carryHand = sCarryHand;
        return;
    }

    sDebug.glassToFaceCm = -1.0f;
    if (sState == State::Pocket) {
        float point[3];
        if (!VrPocket::Point(point)) {
            sDebug.gate = 4;
        } else {
            const int first = SwordHandIdx();
            for (int i = 0; i < 2; ++i) {
                const int hand = i == 0 ? first : 1 - first;
                if (pressed[hand] && VrPocket::HandNear(hand, point)) {
                    float left[3], up[3], forward[3];
                    if (VrPocket::Axes(left, up, forward)) {
                        sGrab.Capture(hand, point, left, up, forward);
                    } else {
                        sGrab.Clear();
                    }
                    sCarryHand = hand;
                    sState = State::Held;
                    sArmed = false; // set below from the real distance, this same tick
                    VR_TriggerHaptic(hand, 0.5f, 0.0f, 35.0f);
                    break;
                }
            }
        }
    }
    if (sState == State::Held) {
        const int hand = sCarryHand;
        float glass[3], face[3], right[3], up[3], fwd[3];
        if (released[hand]) {
            sState = State::Pocket;
            ClearCarry();
        } else if (HeldGlassCenter(glass) && WornCenterWorld(face, right, up, fwd, true)) {
            const float wear = CmToUnits(sDebug.wearDistanceCm);
            const float dist = Distance(glass, face);
            sDebug.glassToFaceCm = dist / (0.01f * VrPocket::WorldScale());
            if (!sArmed && dist > wear * 1.5f) sArmed = true;
            if (sArmed && dist <= wear) PutOn(play, hand);
        }
    } else if (sState == State::Worn) {
        const bool now = play->actorCtx.lensActive;
        if (sLensPrev && !now) {
            // Vanilla switched the reveal off under the worn lens (magic out, door, cutscene,
            // spell): the lens stays on the face and retries once it may.
            sOurs = false;
            sRetryCooldown = kRetryCooldownTicks;
        }
        if (!now) RetryLens(play);
        sLensPrev = play->actorCtx.lensActive;
        float center[3], right[3], up[3], fwd[3];
        if (WornCenterWorld(center, right, up, fwd)) {
            const int first = SwordHandIdx();
            for (int i = 0; i < 2; ++i) {
                const int hand = i == 0 ? first : 1 - first;
                if (pressed[hand] && VrPocket::HandNear(hand, center) && (covers || !GripOwnedElsewhere(hand))) {
                    if (covers) {
                        TakeOff(play, hand);
                    } else {
                        RemoveFromFace(play, hand);
                    }
                    break;
                }
            }
        }
    }
    sDebug.state = StateIndex();
    sDebug.carryHand = sCarryHand;
    sDebug.armed = sArmed ? 1 : 0;
    sDebug.ours = sOurs ? 1 : 0;
    sDebug.lensActive = play->actorCtx.lensActive ? 1 : 0;
}

// Actor_DrawLensOverlay (z_actor.c), after it loads the mask texture and sets the tile: while the
// lens is worn, the mask goes on a quad in the glass's plane, glued to the head, instead of the
// screen rects. Everything else (render mode, combiner, prim depth) is the caller's, untouched.
extern "C" bool VrLens_DrawAperture(GraphicsContext* gfxCtx) {
    if (gfxCtx == nullptr || !WornOnFace()) return false;

    const float radius = WornRadiusUnits();
    const float extent = kApertureExtentRadii * radius;
    float center[3];
    WornCenterLocal(center);
    MtxF local;
    Matrix_Push();
    Matrix_Translate(center[0], center[1], center[2], MTXMODE_NEW);
    Matrix_Scale(extent / 1024.0f, extent / 1024.0f, extent / 1024.0f, MTXMODE_APPLY);
    Matrix_Get(&local);
    Matrix_Pop();
    Mtx* mtx = HeadWeldedMtx(gfxCtx, &local);

    // Corners at +-1024 model units = +-extent; texture offsets in texels, s10.5. Screen t runs
    // down, head +Y runs up.
    const float texels = kApertureExtentRadii * kMaskRadiusTexels;
    Vtx* vtx = (Vtx*)Graph_Alloc(gfxCtx, 4 * sizeof(Vtx));
    static const s8 kCorner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int i = 0; i < 4; ++i) {
        Vtx_t& v = vtx[i].v;
        v.ob[0] = (s16)(kCorner[i][0] * 1024);
        v.ob[1] = (s16)(kCorner[i][1] * 1024);
        v.ob[2] = 0;
        v.flag = 0;
        v.tc[0] = (s16)((kMaskCenterS + kCorner[i][0] * texels) * 32.0f);
        v.tc[1] = (s16)((kMaskCenterT - kCorner[i][1] * texels) * 32.0f);
        v.cn[0] = v.cn[1] = v.cn[2] = v.cn[3] = 255;
    }

    OPEN_DISPS(gfxCtx);
    gSPTexture(POLY_XLU_DISP++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    gSPClearGeometryMode(POLY_XLU_DISP++, G_FOG | G_LIGHTING | G_CULL_BOTH | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR);
    gSPMatrix(POLY_XLU_DISP++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPVertex(POLY_XLU_DISP++, (uintptr_t)vtx, 4, 0);
    gSP2Triangles(POLY_XLU_DISP++, 0, 1, 2, 0, 0, 2, 3, 0);
    CLOSE_DISPS(gfxCtx);
    return true;
}

// The pocket / carried / worn model. Own OnPlayDrawEnd hook. extern "C" linkage is load-bearing
// for the block-scope FrameInterpolation declarations inside OPEN_DISPS (see VrItemSelect_Draw).
extern "C" void VrLens_Draw(void) {
    if (gPlayState == nullptr || sState == State::None) return;
    Player* player = GET_PLAYER(gPlayState);
    if (!VrLens_Covers(player) && !WornOnFace()) return;
    GraphicsContext* gfxCtx = gPlayState->state.gfxCtx;

    if (sState == State::Worn) {
        if (!CVarGetInteger("gVrLensShowFrame", 1)) return;
        MtxF local;
        WornModelLocal(&local);
        OPEN_DISPS(gfxCtx);
        FrameInterpolation_RecordOpenChild(&sWornInterpKey, 0);
        Mtx* mtx = HeadWeldedMtx(gfxCtx, &local);
        // Frame only: the glass film would tint the whole view through it.
        VrLens_DrawModel(gfxCtx, mtx, false);
        FrameInterpolation_RecordCloseChild();
        CLOSE_DISPS(gfxCtx);
        return;
    }

    // Pocket / hand: hidden while the selector stands down (cutscene, ocarina).
    if (!VrItemSelect_SelectionAllowed()) return;
    float position[3], left[3], up[3], forward[3];
    float scale = LensScale();
    const void* key = &sHeldInterpKey;
    if (sState == State::Pocket) {
        if (!VrPocket::Point(position) || !VrPocket::Axes(left, up, forward)) return;
        scale *= CVarGetFloat("gVrLensPreviewScale", 60.0f) / 100.0f;
        key = &sPocketInterpKey;
    } else if (!HeldPose(position, left, up, forward)) {
        return;
    }
    Vec3s rot;
    VrPocket::SetRotFromAxes(&rot, left, up, forward);

    OPEN_DISPS(gfxCtx);
    FrameInterpolation_RecordOpenChild(key, 0);
    Matrix_SetTranslateRotateYXZ(position[0], position[1], position[2], &rot);
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    MtxF cur;
    Matrix_Get(&cur);
    Mtx* mtx = MATRIX_NEWMTX(gfxCtx);
    MtxF hand;
    if (sState == State::Held && VR_GetHandMatrix(sCarryHand, hand.mf)) {
        // Welded to the live controller (the hookshot's idle-hook recipe): the hand-LOCAL part
        // against this tick's hand snapshot, re-composed per eye with the rendered hand.
        MtxF inv;
        MtxF local;
        SkinMatrix_Invert(&hand, &inv);
        SkinMatrix_MtxFMtxFMult(&inv, &cur, &local);
        VR_RegisterHandChildMatrix(mtx, sCarryHand, &local.mf[0][0]);
    }
    VrLens_DrawModel(gfxCtx, mtx, true);
    FrameInterpolation_RecordCloseChild();
    CLOSE_DISPS(gfxCtx);
}

namespace {
void RegisterVrLens() {
    COND_HOOK(OnPlayDrawEnd, true, VrLens_Draw);
}
} // namespace

static RegisterShipInitFunc initVrLens(RegisterVrLens);
