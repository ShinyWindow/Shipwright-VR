extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h" // gSaveContext
#include "objects/object_gi_ki_tan_mask/object_gi_ki_tan_mask.h"
#include "objects/object_gi_skj_mask/object_gi_skj_mask.h"
#include "objects/object_gi_redead_mask/object_gi_redead_mask.h"
#include "objects/object_gi_rabit_mask/object_gi_rabit_mask.h"
#include "objects/object_gi_golonmask/object_gi_golonmask.h"
#include "objects/object_gi_zoramask/object_gi_zoramask.h"
#include "objects/object_gi_gerudomask/object_gi_gerudomask.h"
#include "objects/object_gi_truth_mask/object_gi_truth_mask.h"
extern PlayState* gPlayState;
}
#include "VrCombat.h"
#include "VrPocket.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/frame_interpolation.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <cmath>

// Physical masks (VR first person, selector mode). User spec (October 1, 2026): "make essentially
// the exact same mechanic [as the Lens of Truth] for masks. all the masks in the game should work
// this way", and (asked) a worn mask stays on through item switches like vanilla, comes off by
// gripping it at your face any time, and putting on a different mask swaps it.
//
// Vanilla: Player_UseItem toggles player->currentMask (any mask in use comes OFF, else the used one
// goes on), remembers it in gSaveContext.ship.maskMemory, plays NA_SE_PL_CHANGE_ARMS. Everything
// that reacts to masks (NPCs, Bunny Hood speed, gossip stones) reads currentMask; Player_Process-
// ItemButtons takes it off if the mask leaves the C buttons. In VR first person the head (and the
// worn mask, drawn only by the default limb override) is not drawn. This module only moves WHERE
// currentMask changes: on at the face, off at the face.
//
// POCKET  A mask is selected and not already on your face: its get-item model sits at the pocket
//         point facing you, scaled by gVrMaskPreviewScale. The trigger stands down.
// HELD    A FRESH grip near it takes it (either hand) at real size (gVrMaskSize = the height of a
//         typical mask), welded to the live controller. Release: back in the pocket if it is the
//         selected mask, else put away. Brought within gVrMaskWearDistance of the face spot (in
//         front of the eyes): it goes on (currentMask = it, replacing any other mask), the
//         vanilla sound, a buzz. Nothing is drawn while worn: it's on your face.
// FACE    Any time a mask is worn (whatever is selected), a fresh grip with a hand at your face
//         takes it off into that hand (unless another item owns that grip right now: a carried
//         bomb, the boomerang catch, the worn lens). It must leave the face before it can go back
//         on. Fallback: gVrPhysMasks=0 restores the trigger toggle.

namespace {

enum class State { None, Pocket, Held };

struct MaskModel {
    const char* opa;
    const char* xlu;   // nullptr: no translucent part
    bool setup26;      // GetItem_DrawMaskOrBombchu uses Gfx_SetupDL_26Opa, the others 25
};

// Indexed by PLAYER_MASK_* - 1 (z_draw.c sDrawItemTable entries for the masks).
const MaskModel kModels[8] = {
    { gGiKeatonMaskDL, gGiKeatonMaskEyesDL, false },
    { gGiSkullMaskDL, nullptr, false },
    { gGiSpookyMaskDL, nullptr, false },
    { gGiBunnyHoodDL, gGiBunnyHoodEyesDL, false },
    { gGiGoronMaskDL, nullptr, true },
    { gGiZoraMaskDL, nullptr, true },
    { gGiGerudoMaskDL, nullptr, true },
    { gGiMaskOfTruthDL, gGiMaskOfTruthAccentsDL, false },
};

// Get-item mask models (measured from the vertex data): face toward model +Z, top +Y, origin at
// the middle of the face; a typical mask is ~66 model units tall.
constexpr float kTypicalHeightModel = 66.0f;

State sState = State::None;
int sCarryHand = -1;
int sHeldMask = PLAYER_MASK_NONE; // HELD: which mask is in the hand
bool sGripPrev[2] = { true, true };
int sObservedItem = -1;
VrPocket::Grab sGrab;
bool sArmed = false;
bool sCovers = false; // last tick: a mask is selected and this module presents it
int sPocketInterpKey;
int sHeldInterpKey;
VrMaskDebug sDebug = { 0, 0, -1, 0, 0, -1.0f, 0.0f, 0, 0, -1 };

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

float MaskScale() {
    return CmToUnits(CVarGetFloat("gVrMaskSize", 22.0f)) / kTypicalHeightModel;
}

int SelectedMask(Player* player) {
    return VrMask_Covers(player) ? player->heldItemAction - PLAYER_IA_MASK_KEATON + PLAYER_MASK_KEATON
                                 : PLAYER_MASK_NONE;
}

// The spot a worn mask's middle sits at: in front of the eyes, a little low. Head axes too.
bool FaceSpot(float out[3], float left[3], float up[3], float fwd[3]) {
    if (!VR_IsInitialized()) return false;
    float eye[3];
    VR_GetCameraPose(eye, fwd, up);
    // left = up x fwd (head +X is right; the mask's model X points to the wearer's left).
    left[0] = up[1] * fwd[2] - up[2] * fwd[1];
    left[1] = up[2] * fwd[0] - up[0] * fwd[2];
    left[2] = up[0] * fwd[1] - up[1] * fwd[0];
    const float ahead = CmToUnits(6.0f);
    const float down = CmToUnits(3.0f);
    for (int i = 0; i < 3; ++i) out[i] = eye[i] + fwd[i] * ahead - up[i] * down;
    return true;
}

float Distance(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool HandNearFace(int hand) {
    float face[3], left[3], up[3], fwd[3];
    return FaceSpot(face, left, up, fwd) && VrPocket::HandNear(hand, face);
}

// Pocket pose: the pocket point, the mask's face turned toward the player.
bool PocketPose(float position[3], float left[3], float up[3], float forward[3]) {
    float pocketLeft[3], pocketFwd[3];
    if (!VrPocket::Point(position) || !VrPocket::Axes(pocketLeft, up, pocketFwd)) return false;
    for (int i = 0; i < 3; ++i) {
        left[i] = -pocketLeft[i];
        forward[i] = -pocketFwd[i];
    }
    return true;
}

bool HeldPose(float position[3], float left[3], float up[3], float forward[3]) {
    return sCarryHand >= 0 && sGrab.Replay(sCarryHand, position, left, up, forward);
}

// Another item owns this hand's grip right now (a bomb in it, the boomerang catch, the hammer's
// off hand, an archery pinch, the worn lens at the face): never take a mask off with it.
bool GripOwnedElsewhere(int hand) {
    return VrItemThrow_GripConsumed(hand, VR_BTN_GRIP) || VrBoomerang_GripConsumed(hand, VR_BTN_GRIP) ||
           VrHammer_GripConsumed(hand, VR_BTN_GRIP) || VrArchery_PinchConsumed(hand, VR_BTN_GRIP) ||
           VrLens_GripConsumed(hand, VR_BTN_GRIP);
}

// The worn-mask take-off may run: selector mode in normal play, no horse/water/minigame.
bool FaceActive(PlayState* play, Player* player) {
    return VrItemSelect_ModeActive() && CVarGetInteger("gVrPhysMasks", 1) && player->currentMask != PLAYER_MASK_NONE &&
           play->bombchuBowlingStatus == 0 && play->shootingGalleryStatus == 0 &&
           !(player->stateFlags1 & (PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_IN_WATER));
}

void ClearCarry() {
    sCarryHand = -1;
    sHeldMask = PLAYER_MASK_NONE;
    sGrab.Clear();
}

void SetMask(Player* player, int mask) {
    player->currentMask = (u8)mask;
    gSaveContext.ship.maskMemory = player->currentMask;
    Player_PlaySfx(&player->actor, NA_SE_PL_CHANGE_ARMS);
}

void PutOn(Player* player, int hand) {
    SetMask(player, sHeldMask);
    sDebug.lastWorn = sHeldMask;
    sState = State::None; // re-evaluated next tick (pocket only shows a mask that isn't worn)
    ClearCarry();
    VR_TriggerHaptic(hand, 0.7f, 0.0f, 60.0f);
}

void TakeOff(Player* player, int hand) {
    float face[3], left[3], up[3], fwd[3];
    if (FaceSpot(face, left, up, fwd)) {
        sGrab.Capture(hand, face, left, up, fwd); // comes off exactly where it was: facing out
    } else {
        sGrab.Clear();
    }
    sHeldMask = player->currentMask;
    sCarryHand = hand;
    sState = State::Held;
    sArmed = false;
    SetMask(player, PLAYER_MASK_NONE);
    VR_TriggerHaptic(hand, 0.5f, 0.0f, 35.0f);
}

// Let go of a mask in the hand: back to the pocket if it is the selected one, else put away.
void Release(int selected) {
    const int mask = sHeldMask;
    const int hand = sCarryHand;
    ClearCarry();
    if (mask == selected) {
        sState = State::Pocket;
    } else {
        sState = State::None;
        if (hand >= 0) VR_TriggerHaptic(hand, 0.2f, 0.0f, 20.0f);
    }
}

int StateIndex() {
    switch (sState) {
        case State::Pocket: return 1;
        case State::Held: return 2;
        default: return 0;
    }
}

} // namespace

// extern "C" for the block-scope FrameInterpolation declarations inside OPEN_DISPS.
extern "C" void VrMask_DrawModel(GraphicsContext* gfxCtx, Mtx* mtx, int mask) {
    if (mask < PLAYER_MASK_KEATON || mask > PLAYER_MASK_TRUTH) return;
    const MaskModel& model = kModels[mask - 1];
    OPEN_DISPS(gfxCtx);
    POLY_OPA_DISP = Play_SetFog(gPlayState, POLY_OPA_DISP);
    if (model.setup26) {
        Gfx_SetupDL_26Opa(gfxCtx);
    } else {
        Gfx_SetupDL_25Opa(gfxCtx);
    }
    gSPMatrix(POLY_OPA_DISP++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_OPA_DISP++, (Gfx*)model.opa);
    if (model.xlu != nullptr) {
        Gfx_SetupDL_25Xlu(gfxCtx);
        gSPMatrix(POLY_XLU_DISP++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList(POLY_XLU_DISP++, (Gfx*)model.xlu);
    }
    CLOSE_DISPS(gfxCtx);
}

extern "C" bool VrMask_Covers(Player* player) {
    return VrItemSelect_ModeActive() && CVarGetInteger("gVrPhysMasks", 1) && player != nullptr &&
           player->actor.category == ACTORCAT_PLAYER && player->heldItemAction >= PLAYER_IA_MASK_KEATON &&
           player->heldItemAction <= PLAYER_IA_MASK_TRUTH && player->heldItemAction == player->itemAction &&
           gPlayState != nullptr && gPlayState->bombchuBowlingStatus == 0 && gPlayState->shootingGalleryStatus == 0 &&
           !(player->stateFlags1 & (PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_IN_WATER));
}

extern "C" void VrMask_Reset(void) {
    sState = State::None;
    ClearCarry();
    sGripPrev[0] = sGripPrev[1] = true;
    sObservedItem = -1;
    sArmed = false;
    sCovers = false;
}

extern "C" bool VrMask_PreviewIsModel(void) {
    // A selected mask is this module's to show: in the pocket, in the hand, or on the face.
    return sCovers || sState != State::None;
}

extern "C" bool VrMask_GripConsumed(int32_t hand, uint16_t mask) {
    if (!(mask & VR_BTN_GRIP) || gPlayState == nullptr || VrOcarina_InPlay() || hand < 0 || hand > 1) {
        return false;
    }
    switch (sState) {
        case State::Pocket: {
            float point[3];
            if (VrPocket::Point(point) && VrPocket::HandNear(hand, point)) return true;
            break;
        }
        case State::Held:
            if (hand == sCarryHand) return true;
            break;
        default:
            break;
    }
    // A hand at the face while a mask is worn reaches to take it off.
    Player* player = GET_PLAYER(gPlayState);
    return player != nullptr && FaceActive(gPlayState, player) && HandNearFace(hand) && !GripOwnedElsewhere(hand);
}

extern "C" void VrMask_GetDebug(VrMaskDebug* out) {
    if (out != nullptr) *out = sDebug;
}

extern "C" void VrMask_Tick(PlayState* play, Player* player) {
    if (play == nullptr || player == nullptr || player->actor.category != ACTORCAT_PLAYER) return;
    sCovers = VrMask_Covers(player);
    const int selected = SelectedMask(player);
    sDebug.selected = selected;
    sDebug.worn = player->currentMask;
    sDebug.wearDistanceCm = CVarGetFloat("gVrMaskWearDistance", 12.0f);

    int gate = 0;
    if (!CVarGetInteger("gVrPhysMasks", 1)) {
        gate = 1;
    } else if (!VrItemSelect_ModeActive() || play->bombchuBowlingStatus != 0 || play->shootingGalleryStatus != 0 ||
               (player->stateFlags1 & (PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_IN_WATER))) {
        gate = 2;
    } else if (!VrItemSelect_SelectionAllowed() || play->pauseCtx.state != 0 || player->unk_6AD != 0) {
        gate = 3;
    }
    sDebug.gate = gate;
    if (gate == 1 || gate == 2) {
        // A mask in the hand is put away; a worn mask stays worn (vanilla state, untouched).
        sState = State::None;
        ClearCarry();
        sGripPrev[0] = sGripPrev[1] = true;
        sDebug.state = 0;
        sDebug.carryHand = -1;
        return;
    }
    if (gate == 3) {
        sGripPrev[0] = sGripPrev[1] = true;
        sDebug.state = StateIndex();
        return;
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
        // Switching: a mask in the hand goes back to the pocket / is put away; a worn one stays on.
        if (sState == State::Held) Release(selected);
        sDebug.state = StateIndex();
        sDebug.carryHand = sCarryHand;
        return;
    }

    sDebug.maskToFaceCm = -1.0f;
    if (sState == State::Held) {
        const int hand = sCarryHand;
        float position[3], left[3], up[3], forward[3], face[3], faceLeft[3], faceUp[3], faceFwd[3];
        if (released[hand]) {
            Release(selected);
        } else if (HeldPose(position, left, up, forward) && FaceSpot(face, faceLeft, faceUp, faceFwd)) {
            const float wear = CmToUnits(sDebug.wearDistanceCm);
            const float dist = Distance(position, face);
            sDebug.maskToFaceCm = dist / (0.01f * VrPocket::WorldScale());
            if (!sArmed && dist > wear * 1.5f) sArmed = true;
            if (sArmed && dist <= wear) PutOn(player, hand);
        }
    } else {
        // The pocket shows the selected mask unless it is the one on your face.
        sState = (selected != PLAYER_MASK_NONE && selected != player->currentMask) ? State::Pocket : State::None;

        // Take a worn mask off at the face (any selection), sword hand first on a same-tick tie.
        bool tookOff = false;
        if (FaceActive(play, player)) {
            const int first = SwordHandIdx();
            for (int i = 0; i < 2 && !tookOff; ++i) {
                const int hand = i == 0 ? first : 1 - first;
                if (pressed[hand] && HandNearFace(hand) && !GripOwnedElsewhere(hand)) {
                    TakeOff(player, hand);
                    tookOff = true;
                }
            }
        }
        float point[3];
        if (!tookOff && sState == State::Pocket && VrPocket::Point(point)) {
            const int first = SwordHandIdx();
            for (int i = 0; i < 2; ++i) {
                const int hand = i == 0 ? first : 1 - first;
                if (pressed[hand] && VrPocket::HandNear(hand, point)) {
                    float pos[3], left[3], up[3], forward[3];
                    if (PocketPose(pos, left, up, forward)) {
                        sGrab.Capture(hand, pos, left, up, forward);
                    } else {
                        sGrab.Clear();
                    }
                    sHeldMask = selected;
                    sCarryHand = hand;
                    sState = State::Held;
                    sArmed = true; // the pocket is well away from the face
                    VR_TriggerHaptic(hand, 0.5f, 0.0f, 35.0f);
                    break;
                }
            }
        }
    }
    sDebug.state = StateIndex();
    sDebug.carryHand = sCarryHand;
    sDebug.held = sHeldMask;
    sDebug.armed = sArmed ? 1 : 0;
    sDebug.worn = player->currentMask;
}

// The pocket / carried mask. Own OnPlayDrawEnd hook. extern "C" linkage is load-bearing for the
// block-scope FrameInterpolation declarations inside OPEN_DISPS (see VrItemSelect_Draw).
extern "C" void VrMask_Draw(void) {
    if (gPlayState == nullptr || sState == State::None) return;
    if (!VrItemSelect_ModeActive() || !VrItemSelect_SelectionAllowed()) return;
    Player* player = GET_PLAYER(gPlayState);
    float position[3], left[3], up[3], forward[3];
    float scale = MaskScale();
    int mask = sHeldMask;
    const void* key = &sHeldInterpKey;
    if (sState == State::Pocket) {
        mask = SelectedMask(player);
        if (mask == PLAYER_MASK_NONE || !PocketPose(position, left, up, forward)) return;
        scale *= CVarGetFloat("gVrMaskPreviewScale", 60.0f) / 100.0f;
        key = &sPocketInterpKey;
    } else if (!HeldPose(position, left, up, forward)) {
        return;
    }
    Vec3s rot;
    VrPocket::SetRotFromAxes(&rot, left, up, forward);
    GraphicsContext* gfxCtx = gPlayState->state.gfxCtx;

    OPEN_DISPS(gfxCtx);
    FrameInterpolation_RecordOpenChild(key, 0);
    Matrix_SetTranslateRotateYXZ(position[0], position[1], position[2], &rot);
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    MtxF cur;
    Matrix_Get(&cur);
    Mtx* mtx = MATRIX_NEWMTX(gfxCtx);
    MtxF hand;
    if (sState == State::Held && VR_GetHandMatrix(sCarryHand, hand.mf)) {
        // Welded to the live controller (the lens / hookshot recipe).
        MtxF inv;
        MtxF local;
        SkinMatrix_Invert(&hand, &inv);
        SkinMatrix_MtxFMtxFMult(&inv, &cur, &local);
        VR_RegisterHandChildMatrix(mtx, sCarryHand, &local.mf[0][0]);
    }
    VrMask_DrawModel(gfxCtx, mtx, mask);
    FrameInterpolation_RecordCloseChild();
    CLOSE_DISPS(gfxCtx);
}

namespace {
void RegisterVrMask() {
    COND_HOOK(OnPlayDrawEnd, true, VrMask_Draw);
}
} // namespace

static RegisterShipInitFunc initVrMask(RegisterVrMask);
