#include "VrCombat.h"

#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"

#include <spdlog/spdlog.h>
#include <vr_interface.h>

#include "z64.h"
#include "macros.h"
#include "functions.h"

extern "C" {
extern PlayState* gPlayState;
}

namespace {

VrCombat::TickPath sTickPath[2];

// The soh <-> libultraship physical-combat contract is versioned; refuse to run against a stale
// submodule build rather than misbehave subtly (the check is one call, cached forever).
bool InterfaceOk() {
    static bool checked = false;
    static bool ok = false;
    if (!checked) {
        checked = true;
        ok = (VR_PhysGetInterfaceVersion() == VR_PHYS_INTERFACE_VERSION);
        if (!ok) {
            SPDLOG_ERROR("[VrCombat] libultraship physical-combat interface v{} != expected v{} - "
                         "stale submodule build? Physical combat disabled.",
                         VR_PhysGetInterfaceVersion(), VR_PHYS_INTERFACE_VERSION);
        }
    }
    return ok;
}

bool sWasActive = false;

// Aim-hand trigger edge, sampled once per tick (VR first person, independent of physical
// combat): while READY_TO_FIRE, this fires the bow/slingshot/hookshot — the natural VR
// release, since the walk-while-aiming path only knows the vanilla item-button-release fire.
bool sProjTriggerPressed = false;
bool sProjTriggerPrev = false;

int AimHand() {
    // The hand that aims held projectiles (see Player_VrAimHeldProjectile): the bow rides
    // Link's RIGHT hand model, driven by the player's LEFT controller when right-handed.
    return CVarGetInteger("gVrLeftHanded", 0) ? VR_HAND_RIGHT : VR_HAND_LEFT;
}

// Once per 20 Hz game tick, right after the player actor updates (and after the tick's
// CollisionCheck pass at the top of Play_Update, but before Play_Draw builds the display list):
//  1. read back last tick's melee quad results (hit haptics, one-hit-per-swing),
//  2. drain the headset-rate hand paths accumulated since the last tick, so the coming draw
//     feeds colliders from the freshest possible motion.
// The drain consumes the samples, so it happens exactly once here no matter how many systems
// read the snapshot.
void OnPlayerUpdateVrCombat() {
    if (gPlayState == NULL) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == NULL) {
        return;
    }

    // Trigger edge for projectile fire — sampled regardless of the physical-combat toggle
    // (walk-while-aiming is plain VR, so its fire input must be too).
    {
        const bool cur =
            VR_IsInitialized() && VR_GetFirstPerson() && (VR_GetControllerButton(AimHand()) & VR_BTN_TRIGGER);
        sProjTriggerPressed = cur && !sProjTriggerPrev;
        sProjTriggerPrev = cur;
    }

    const bool active = VrCombat_Active();
    if (active) {
        VrCombat::Swing_OnPlayerUpdate(gPlayState, player);
        VrCombat::Shield_OnPlayerUpdate(gPlayState, player);
    } else if (sWasActive) {
        VrCombat::Swing_Deactivate(gPlayState, player);
        VrCombat::Shield_Deactivate(gPlayState, player);
    }
    sWasActive = active;

    for (int hand = 0; hand < 2; hand++) {
        sTickPath[hand].count = VR_GetHandPath(hand, sTickPath[hand].samples, ARRAY_COUNT(sTickPath[hand].samples));
    }
}

} // namespace

extern "C" bool VrCombat_ProjectileFirePressed(Player* player) {
    (void)player;
    return sProjTriggerPressed;
}

extern "C" bool VrCombat_AimTriggerConsumed(int32_t vrHand, uint16_t vrBtnMask) {
    // While READY_TO_FIRE in VR first person, the aim hand's trigger IS the fire control:
    // padmgr must not also emit its normal button binding (Z-target flicker on every shot).
    if (!(vrBtnMask & VR_BTN_TRIGGER) || vrHand != AimHand()) {
        return false;
    }
    if (!VR_IsInitialized() || !VR_GetFirstPerson() || gPlayState == NULL) {
        return false;
    }
    Player* player = GET_PLAYER(gPlayState);
    return player != NULL && (player->stateFlags1 & PLAYER_STATE1_READY_TO_FIRE);
}

extern "C" bool VrCombat_Active(void) {
    if (!CVarGetInteger("gVrPhysCombat", 1) || !InterfaceOk()) {
        return false;
    }
    if (!VR_IsInitialized() || !VR_GetFirstPerson()) {
        return false;
    }
    if (!GameInteractor::IsSaveLoaded(true) || gPlayState == NULL) {
        return false;
    }
    // Any frame the game owns Link is a vanilla frame. This list grows as milestones need finer
    // exclusions (horse, swimming, crawlspaces, shooting galleries).
    Player* player = GET_PLAYER(gPlayState);
    if (player == NULL || Player_InBlockingCsMode(gPlayState, player)) {
        return false;
    }
    if (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF || gPlayState->csCtx.state != CS_STATE_IDLE) {
        return false;
    }
    return true;
}

// The mask must live IN the display lists: draws here only BUILD Gfx commands, while the
// harvest runs later when the interpreter EXECUTES them. The marker opcode toggles the mask
// at execution time, bracketing the excluded section in both the OPA and XLU buckets.
extern "C" void VrCombat_MeshMaskPush(GraphicsContext* gfxCtx) {
    if (!VrCombat_Active() || !CVarGetInteger("gVrPhysVisualMesh", 1)) {
        return;
    }
    gSPVrPhysMask(gfxCtx->polyOpa.p++, 1);
    gSPVrPhysMask(gfxCtx->polyXlu.p++, 1);
}

extern "C" void VrCombat_MeshMaskPop(GraphicsContext* gfxCtx) {
    if (!VrCombat_Active() || !CVarGetInteger("gVrPhysVisualMesh", 1)) {
        return;
    }
    gSPVrPhysMask(gfxCtx->polyOpa.p++, 0);
    gSPVrPhysMask(gfxCtx->polyXlu.p++, 0);
}

extern "C" void VrCombat_MeshFleshPush(GraphicsContext* gfxCtx) {
    if (!VrCombat_Active() || !CVarGetInteger("gVrPhysVisualMesh", 1)) {
        return;
    }
    gSPVrPhysMask(gfxCtx->polyOpa.p++, 2);
    gSPVrPhysMask(gfxCtx->polyXlu.p++, 2);
}

extern "C" void VrCombat_MeshFleshPop(GraphicsContext* gfxCtx) {
    if (!VrCombat_Active() || !CVarGetInteger("gVrPhysVisualMesh", 1)) {
        return;
    }
    gSPVrPhysMask(gfxCtx->polyOpa.p++, 3);
    gSPVrPhysMask(gfxCtx->polyXlu.p++, 3);
}

namespace VrCombat {

const TickPath& GetTickPath(int hand) {
    static const TickPath kEmpty = {};
    if (hand < 0 || hand > 1) {
        return kEmpty;
    }
    return sTickPath[hand];
}

} // namespace VrCombat

static void RegisterVrCombat() {
    // Always registered (not conditioned on gVrPhysCombat): flipping the toggle off mid-swing
    // must still run the falling-edge cleanup, or meleeWeaponState would stay stuck nonzero —
    // a permanently-hot vanilla sword collider. The body gates itself via VrCombat_Active().
    COND_HOOK(OnPlayerUpdate, true, OnPlayerUpdateVrCombat);
}

static RegisterShipInitFunc initFunc(RegisterVrCombat, {});
