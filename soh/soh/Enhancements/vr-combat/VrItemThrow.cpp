extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "objects/gameplay_keep/gameplay_keep.h"
#include "src/overlays/actors/ovl_En_Bom_Chu/z_en_bom_chu.h"
extern PlayState* gPlayState;
}
#include "VrCombat.h"
#include "VrPocket.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/frame_interpolation.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <cmath>

namespace {
bool sGripPrev[2] = { true, true }; // require a fresh press on entry/reset
int sCarryHand = -1;                // hand that grabbed the current throwable, -1 = none
int sObservedItem = -1;
bool sReleaseHasGrip = false;

// Bombchu grab transform (VrPocket::Grab). The presented chu is a real model with an
// orientation; grabbing it records where it sat relative to the hand (position offset and its
// three model axes, all in the controller's own frame). The carry replays that transform every
// tick, so the chu is parented to the hand exactly as it was taken, and the angle you let go at
// is the angle it leaves at. Invalid = a hold restored without an observed grab (save state):
// the chu then sits at the hand in the default pose.
VrPocket::Grab sChuGrab;
int sPreviewInterpKey; // address only: frame-interpolation identity for the preview draw

float WorldScale() {
    return VrPocket::WorldScale();
}

int SwordHandIdx() {
    return CVarGetInteger("gVrLeftHanded", 0) ? VR_HAND_LEFT : VR_HAND_RIGHT;
}

// The bombchu rides the bomb's present-and-grab path; only its release differs (a drop that
// falls and crawls, never a throw). Its own switch so the chu alone can fall back to vanilla.
bool ChuCovered(Player* player) {
    return player->heldItemAction == PLAYER_IA_BOMBCHU && CVarGetInteger("gVrPhysBombchuDrop", 1);
}

// Record how the presented chu sits relative to the grabbing hand, in the hand's frame.
void CaptureChuGrab(int hand) {
    sChuGrab.Clear();
    float preview[3], left[3], up[3], forward[3];
    if (!VrItemThrow_PreviewPosition(preview) || !VrPocket::Axes(left, up, forward)) {
        return;
    }
    sChuGrab.Capture(hand, preview, left, up, forward);
}

// The carried chu's world pose this tick: the grab transform replayed on the current hand pose.
bool CarriedChuPose(int hand, float position[3], float left[3], float up[3], float forward[3]) {
    return sChuGrab.Replay(hand, position, left, up, forward);
}

// Bombchu release intent: the chu crawls where its nose was pointing at the moment of release
// (the carried model's forward, flattened to horizontal). Untracked controller or a nose
// pointing straight up/down = no intent; the actor uses Link's facing when it lands.
void SetChuCrawlYaw(EnBomChu* chu, int hand) {
    float position[3], left[3], up[3], forward[3];
    chu->vrHasCrawlYaw = false;
    if (!CarriedChuPose(hand, position, left, up, forward)) return;
    if (forward[0] * forward[0] + forward[2] * forward[2] < 1e-4f) return;
    // Same convention as Player_VrReleaseItem: rot.y = 0 is +Z, sin(rot.y) is the X component.
    chu->vrCrawlYaw = Math_Atan2S(forward[2], forward[0]);
    chu->vrHasCrawlYaw = true;
}

// The hand that owns the current carry. A hold restored without an observed grab (save-state
// load) defaults to the sword hand; sReleaseHasGrip stays false there, so it drops, not throws.
int CarryHand() {
    return sCarryHand >= 0 ? sCarryHand : SwordHandIdx();
}

bool HasThrowable(Player* player) {
    return player && player->heldActor && (player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR) &&
           player->heldActor->parent == &player->actor &&
           (player->heldActor->id == ACTOR_EN_BOM || player->heldActor->id == ACTOR_EN_ARROW ||
            player->heldActor->id == ACTOR_EN_BOM_CHU);
}

bool SwapChord() {
    const auto mask = (uint16_t)CVarGetInteger("gVrItemSelSwapInput", VR_BTN_GRIP);
    return mask && (VR_GetControllerButton(0) & mask) && (VR_GetControllerButton(1) & mask);
}

// Any-hand grabs (behavior plan): a hand is "about to grab" when it is within reach of the
// presented preview. Shared by the grab tick and the padmgr grip reservation, so a grip press
// only loses its normal binding when it would actually grab.
bool HandNearPreview(int hand) {
    float preview[3];
    return VrItemThrow_PreviewPosition(preview) && VrPocket::HandNear(hand, preview);
}
}

extern "C" bool VrItemThrow_Active(Player* player) {
    return VrItemSelect_ModeActive() && CVarGetInteger("gVrPhysicalItemThrows", 1) && player &&
           player->actor.category == ACTORCAT_PLAYER &&
           (player->heldItemAction == PLAYER_IA_BOMB || player->heldItemAction == PLAYER_IA_DEKU_NUT ||
            ChuCovered(player)) &&
           gPlayState && gPlayState->bombchuBowlingStatus == 0 && gPlayState->shootingGalleryStatus == 0 &&
           !(player->stateFlags1 & (PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_IN_WATER));
}

extern "C" void VrItemThrow_Reset(void) {
    sGripPrev[0] = sGripPrev[1] = true;
    sCarryHand = -1;
    sObservedItem = -1;
    sReleaseHasGrip = false;
    sChuGrab.Clear();
}

// The presented bombchu is a real (shrunken) chu model, not an item icon: the selector's
// passive icon stands down for it, and VrItemThrow_DrawPreview draws it.
extern "C" bool VrItemThrow_PreviewIsModel(void) {
    if (!gPlayState) return false;
    Player* player = GET_PLAYER(gPlayState);
    float position[3];
    return player && ChuCovered(player) && VrItemThrow_PreviewPosition(position);
}

// Preview draw: the chu as it would appear when used, shrunk by gVrChuPreviewScale, sitting at
// the preview point, level, nose away from the player. Own OnPlayDrawEnd hook with minimal
// gates (same pattern as the bottle mouth marker). extern "C" linkage is load-bearing for the
// block-scope FrameInterpolation declarations inside OPEN_DISPS (see VrItemSelect_Draw).
extern "C" void VrItemThrow_DrawPreview(void) {
    if (!VrItemThrow_PreviewIsModel()) return;
    float position[3], left[3], up[3], forward[3];
    if (!VrItemThrow_PreviewPosition(position) || !VrPocket::Axes(left, up, forward)) return;
    Vec3s rot;
    VrPocket::SetRotFromAxes(&rot, left, up, forward);
    const float scale = 0.01f * (CVarGetFloat("gVrChuPreviewScale", 50.0f) / 100.0f); // BOMBCHU_SCALE * shrink

    OPEN_DISPS(gPlayState->state.gfxCtx);
    FrameInterpolation_RecordOpenChild((const void*)&sPreviewInterpKey, 0);
    Matrix_SetTranslateRotateYXZ(position[0], position[1], position[2], &rot);
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    POLY_OPA_DISP = Play_SetFog(gPlayState, POLY_OPA_DISP);
    Gfx_SetupDL_25Opa(gPlayState->state.gfxCtx);
    // EnBomChu_Draw's body colour at full blink intensity (the cosmetic override honoured).
    if (CVarGetInteger(CVAR_COSMETIC("Equipment.ChuBody.Changed"), 0)) {
        const Color_RGB8 fallback = { 209, 34, 0 };
        const Color_RGB8 color = CVarGetColor24(CVAR_COSMETIC("Equipment.ChuBody.Value"), fallback);
        gDPSetEnvColor(POLY_OPA_DISP++, color.r, color.g, color.b, 255);
    } else {
        gDPSetEnvColor(POLY_OPA_DISP++, 218, 43, 0, 255);
    }
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(gPlayState->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gBombchuDL);
    FrameInterpolation_RecordCloseChild();
    CLOSE_DISPS(gPlayState->state.gfxCtx);
}

extern "C" bool VrItemThrow_PreviewPosition(float* position) {
    if (!gPlayState || !VrItemThrow_Active(GET_PLAYER(gPlayState)) ||
        !VrItemSelect_SelectionAllowed() || GET_PLAYER(gPlayState)->heldActor) {
        return false;
    }
    return VrPocket::Point(position);
}

extern "C" bool VrItemThrow_GripConsumed(int32_t hand, uint16_t mask) {
    if (!gPlayState || !(mask & VR_BTN_GRIP) || VrOcarina_InPlay()) {
        return false;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (!VrItemThrow_Active(player)) {
        return false;
    }
    // Carrying: the owning hand's grip is the release input. Preview up: only a hand close
    // enough to grab loses its binding — a grip press elsewhere keeps Z-target and friends.
    return HasThrowable(player) ? hand == CarryHand() : HandNearPreview(hand);
}

// A bomb / bombchu held through this module draws welded to the live hand (every Mtx it makes,
// the VrCarry draw weld): its world pose is written at 20 Hz from the hand, so it trailed the fist.
// The Deku Nut carried here has no visible model while held.
extern "C" bool VrItemThrow_BeginDrawWeld(Actor* actor) {
    if (actor == nullptr || gPlayState == nullptr || !CVarGetInteger("gVrCarryLiveDraw", 1)) {
        return false;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (!VrItemThrow_Active(player) || !HasThrowable(player) || actor != player->heldActor ||
        actor->id == ACTOR_EN_ARROW) {
        return false;
    }
    gVrMtxWeldHand = CarryHand();
    return true;
}

extern "C" void VrItemThrow_UpdateCarryPose(Player* player) {
    if (!VrItemThrow_Active(player) || !HasThrowable(player)) return;
    float position[3], rotation[4];
    if (player->heldActor->id == ACTOR_EN_BOM_CHU) {
        // Parented to the hand with the transform it was grabbed at: position and orientation.
        float left[3], up[3], forward[3];
        if (CarriedChuPose(CarryHand(), position, left, up, forward)) {
            player->heldActor->world.pos = { position[0], position[1], position[2] };
            VrPocket::SetRotFromAxes(&player->heldActor->shape.rot, left, up, forward);
        }
    } else if (VR_GetHandPose(CarryHand(), position, rotation)) {
        player->heldActor->world.pos = { position[0], position[1], position[2] };
    }
}

extern "C" void VrItemThrow_Tick(PlayState* play, Player* player) {
    if (!VrItemThrow_Active(player) || !VrItemSelect_SelectionAllowed() ||
        play->pauseCtx.state != 0 || player->unk_6AD != 0 || player->heldItemAction != player->itemAction) {
        VrItemThrow_Reset();
        return;
    }
    if (sObservedItem != player->heldItemId) {
        sObservedItem = player->heldItemId;
        sGripPrev[0] = sGripPrev[1] = true;
    }
    bool pressed[2], released[2];
    for (int hand = 0; hand < 2; ++hand) {
        const bool grip = (VR_GetControllerButton(hand) & VR_BTN_GRIP) != 0;
        pressed[hand] = grip && !sGripPrev[hand];
        released[hand] = !grip && sGripPrev[hand];
        sGripPrev[hand] = grip;
    }
    if (HasThrowable(player) && (VR_GetControllerButton(CarryHand()) & VR_BTN_GRIP)) {
        sReleaseHasGrip = true;
    }
    if (SwapChord() || VrItemSelect_PendingSlot() != -2) return;

    if (HasThrowable(player)) {
        VrItemThrow_UpdateCarryPose(player);
        const int hand = CarryHand();
        if (released[hand]) {
            float velocity[3] = {}, angular[3];
            // Runtime velocities exclude locomotion/snap-turn displacement. Use the
            // shared path snapshot if available, never drain the XR buffer again.
            const auto& path = VrCombat::GetTickPath(hand);
            int samples = 0;
            if (path.count) {
                const auto latest = path.samples[path.count - 1].timeNs;
                for (int i = 0; i < path.count; ++i) {
                    if (latest - path.samples[i].timeNs > 100000000ULL) continue;
                    for (int axis = 0; axis < 3; ++axis) velocity[axis] += path.samples[i].linVelMps[axis];
                    ++samples;
                }
            }
            if (samples) {
                for (float& component : velocity) component /= samples;
            } else {
                VR_GetHandVelocity(hand, velocity, angular);
            }
            // A restored/interrupted hold without an observed grip is a drop, never
            // a throw using motion samples from the previous timeline.
            if (!sReleaseHasGrip) velocity[0] = velocity[1] = velocity[2] = 0.0f;
            sReleaseHasGrip = false;
            const float scale = WorldScale();
            for (float& component : velocity) component *= scale * 1.4f / 20.0f;
            if (player->heldActor->id == ACTOR_EN_BOM_CHU) {
                // Never a throw: it falls from the hand and crawls where the controller points.
                SetChuCrawlYaw((EnBomChu*)player->heldActor, hand);
                velocity[0] = velocity[1] = velocity[2] = 0.0f;
            }
            Player_VrReleaseItem(play, player, velocity);
            VR_TriggerHaptic(hand, 0.35f, 0.0f, 25.0f);
            sCarryHand = -1;
        }
    } else {
        sCarryHand = -1;
        // Any hand may claim the preview; sword hand checked first on a same-tick tie.
        const int first = SwordHandIdx();
        for (int i = 0; i < 2; ++i) {
            const int hand = i == 0 ? first : 1 - first;
            if (pressed[hand] && HandNearPreview(hand)) {
                // Capture the presented pose before the grab replaces the preview with the actor.
                CaptureChuGrab(hand);
                if (!Player_VrGrabItem(play, player)) {
                    sChuGrab.Clear();
                    continue;
                }
                sCarryHand = hand;
                VrItemThrow_UpdateCarryPose(player);
                VR_TriggerHaptic(hand, 0.5f, 0.0f, 35.0f);
                break;
            }
        }
    }
}

namespace {
void RegisterVrItemThrow() {
    COND_HOOK(OnPlayDrawEnd, true, VrItemThrow_DrawPreview);
}
} // namespace

static RegisterShipInitFunc initVrItemThrow(RegisterVrItemThrow);
