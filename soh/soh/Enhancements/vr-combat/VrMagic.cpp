extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
extern PlayState* gPlayState;
}
#include "VrCombat.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <vr_interface.h>
#include <cmath>

// Two-trigger spell casting (VR first person, selector mode): Din's Fire, Farore's Wind and
// Nayru's Love. Selecting a spell equips it passively like every other item; holding BOTH
// triggers casts it. One trigger alone does nothing.
//
// Vanilla already has the shape we want: pressing a spell's button enters Player_Action_808507F4,
// whose first phase is a wind-up (gPlayerAnim_link_magic_tame, Link rooted with
// Player_DecelerateToZero, the "hup" at frame 20) that flows straight into the cast (spell actor
// spawned, cast voice, magic spent). This module changes only who owns the wind-up:
//  - The trigger mirror emits the spell's button only while both triggers are down
//    (VrItemSelect_TriggerItemMask), so the chord IS the button press and every vanilla gate
//    (magic, Farore's warp point, on the ground) runs untouched.
//  - During the wind-up both hands buzz, building with the animation's progress. Letting go of
//    either trigger before it finishes cancels (Player_Action_808507F4, // SOH [VR]): the magic
//    preview resets, nothing is spent, Link returns to idle and can move again.
//  - The wind-up's end is the cast, exactly as vanilla: a full-strength burst on both hands.
// Farore's Wind with a warp point already set goes to the vanilla return/dispel choice on the
// chord (no wind-up there, so nothing to charge). Horseback, swimming and minigames keep the
// single-trigger path; gVrPhysMagicChord=0 restores it everywhere.

namespace {

bool sCharging = false; // this wind-up began from the chord and is still ours to cancel

bool IsSpell(int32_t itemAction) {
    return itemAction == PLAYER_IA_DINS_FIRE || itemAction == PLAYER_IA_FARORES_WIND ||
           itemAction == PLAYER_IA_NAYRUS_LOVE;
}

void BothHands(float amplitude, float durationMs) {
    VR_TriggerHaptic(VR_HAND_LEFT, amplitude, 0.0f, durationMs);
    VR_TriggerHaptic(VR_HAND_RIGHT, amplitude, 0.0f, durationMs);
}

} // namespace

extern "C" bool VrMagic_Covers(Player* player) {
    if (!CVarGetInteger("gVrPhysMagicChord", 1) || !VrItemSelect_ModeActive() || player == NULL ||
        player->actor.category != ACTORCAT_PLAYER || gPlayState == NULL) {
        return false;
    }
    if (gPlayState->shootingGalleryStatus != 0 || gPlayState->bombchuBowlingStatus != 0 ||
        (player->stateFlags1 & (PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_IN_WATER))) {
        return false;
    }
    return IsSpell(player->heldItemAction) && player->heldItemAction == player->itemAction;
}

extern "C" bool VrMagic_ChordHeld(void) {
    return (VR_GetControllerButton(VR_HAND_LEFT) & VR_BTN_TRIGGER) != 0 &&
           (VR_GetControllerButton(VR_HAND_RIGHT) & VR_BTN_TRIGGER) != 0;
}

extern "C" void VrMagic_BeginCharge(PlayState* play, Player* player) {
    (void)play;
    sCharging = VrMagic_Covers(player) && VrMagic_ChordHeld();
    if (sCharging) {
        BothHands(0.25f, 40.0f); // the catch of the chord
    }
}

extern "C" int32_t VrMagic_ChargeTick(PlayState* play, Player* player, float progress) {
    (void)play;
    (void)player;
    if (!sCharging) {
        return 0;
    }
    if (!VR_IsInitialized() || !VrItemSelect_ModeActive()) {
        sCharging = false; // left VR first person mid wind-up: vanilla finishes the cast
        return 0;
    }
    if (!VrMagic_ChordHeld()) {
        sCharging = false;
        BothHands(0.15f, 30.0f); // fizzle
        return 1;
    }
    const float p = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
    // A continuous rumble (each pulse outlasts the 50 ms tick) that swells toward the release.
    BothHands(0.08f + 0.6f * p * p, 70.0f);
    return 0;
}

extern "C" void VrMagic_OnCast(void) {
    if (sCharging) {
        BothHands(1.0f, 300.0f);
    }
    sCharging = false;
}
