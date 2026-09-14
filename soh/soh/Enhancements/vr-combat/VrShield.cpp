#include "z64.h"
#include "macros.h"
#include "functions.h"

extern "C" {
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}

#include "VrCombat.h"

#include "soh/cvar_prefixes.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <vr_interface.h>
#include <cmath>

// M4 physical shield: the shield lives in the off hand while a melee weapon is VISIBLY in the
// sword hand (leftHandType — the sword's visibility itself, so the shield leaves the arm the
// same frame the sword leaves the hand, exactly like the base game), and blocking is purely
// geometric — an enemy attack that touches the shield quad bounces (the quad's AC_HARD init
// makes CollisionCheck set AC_BOUNCED) and func_808382DC negates the damage exactly like a
// vanilla stance block, except there is no stance: no button, no crouch, no movement lock, no
// reaction animation, no backwards shove. Hold it up = blocked; hand at your side = hit. Both
// vanilla stance entries (crouch in Player_ActionHandler_11, Z-target upper-body in
// func_80834758) are suppressed at the root while this owns blocking, so their itemAction = -1
// diverged-models marker can never fire from a grip press.
//
// It rides three vanilla gates, all previously keyed on PLAYER_STATE1_SHIELDING (the R-button
// stance, force-cleared every frame — deliberately NOT set here):
//  - Player_SetModelsForHoldingShield (shield model in the off hand)  } widened with
//  - Player_UpdateShieldCollider (shield quad AC/AT registration)     } VrCombat_ShieldHeld()
//  - func_808382DC's block branch (stance reaction skipped via VrCombat_ShieldBlockPhysical()).
// The quad's vertices come from the R_HAND limb matrix, which first-person VR pins to the
// off-hand controller — the visible shield IS the collider, no new collision math needed.

namespace {

bool sWasHeld = false;
int sBlockHapticCooldown = 0; // game ticks; sustained contacts re-bounce every tick

inline int ShieldHand() {
    // Opposite of the sword hand (see VrSwing's SwordHand): Link's R_HAND limb, driven by the
    // player's LEFT controller in right-handed mode.
    return CVarGetInteger("gVrLeftHanded", 0) ? VR_HAND_RIGHT : VR_HAND_LEFT;
}

} // namespace

extern "C" bool VrCombat_ShieldHeld(Player* player) {
    if (!VrCombat_Active() || !CVarGetInteger("gVrPhysShield", 1)) {
        return false;
    }
    if (player->actor.category != ACTORCAT_PLAYER) {
        return false; // co-op partner keeps vanilla shielding
    }
    if (player->currentShield == PLAYER_SHIELD_NONE) {
        return false;
    }
    // Vanilla holdability rules: child Link carries the Hylian shield on his back (crouch
    // blocking stays vanilla there), and two-handed weapons occupy both hands.
    if (Player_IsChildWithHylianShield(player)) {
        return false;
    }
    if (Player_HoldsTwoHandedWeapon(player) &&
        !(CVarGetInteger(CVAR_CHEAT("ShieldTwoHanded"), 0) && (player->heldItemAction != PLAYER_IA_DEKU_STICK))) {
        return false;
    }
    // The shield rides the off hand exactly while a melee weapon is VISIBLY in the sword hand.
    // leftHandType is the output of Player_SetModels' central model derivation, so it IS the
    // sword's visibility: it goes empty in every scenario that stows the sword — real put-aways
    // AND the visual-only stows (the itemAction = -1 diverged-models family) where
    // heldItemAction still claims the sword. Keying on it restores the vanilla invariant the
    // old logical-item deny-list chased case by case (vanilla's stance could only exist in
    // model-coherent states, so its shield always left with the sword): the shield now leaves
    // the arm the same frame the sword leaves the hand, everywhere. Bare hands, bow/hookshot,
    // bottles, boomerang and the ocarina all fall out of the default case. The base-game rule
    // "the shield unequips with the sword" is deliberate policy here in BOTH control schemes —
    // classic mode loses vanilla's bare-hand shield raise.
    switch (player->leftHandType) {
        case PLAYER_MODELTYPE_LH_SWORD:
        case PLAYER_MODELTYPE_LH_SWORD_2:
        case PLAYER_MODELTYPE_LH_BGS:    // broken Giant's Knife / child Master Sword renders here
        case PLAYER_MODELTYPE_LH_HAMMER: // reachable only with the ShieldTwoHanded cheat
            break;
        case PLAYER_MODELTYPE_LH_CLOSED:
            // The Deku stick has no hand model of its own — it renders separately over a closed
            // fist (PLAYER_MODELGROUP_10), so the fist plus the stick logically held is its
            // "visibly in hand". A visual-only stick stow re-derives to an open hand and drops
            // the shield like every other weapon.
            if (player->heldItemAction != PLAYER_IA_DEKU_STICK) {
                return false;
            }
            break;
        default:
            return false;
    }
    // Hands-off states the model derivation doesn't encode (the sword can stay legitimately
    // drawn on Epona, but the off hand holds the reins): the shield goes back on Link's back
    // exactly like his other gear.
    if (player->stateFlags1 &
        (PLAYER_STATE1_CLIMBING_LADDER | PLAYER_STATE1_CLIMBING_LEDGE | PLAYER_STATE1_HANGING_OFF_LEDGE |
         PLAYER_STATE1_IN_WATER | PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_CARRYING_ACTOR | PLAYER_STATE1_DEAD)) {
        return false;
    }
    return true;
}

extern "C" void VrCombat_ShieldQuadModelVerts(float* outXyz4) {
    // The block collider, built from scratch in R_HAND limb MODEL space (game units x100 —
    // the same convention as the blade sliders), replacing the vanilla stance quad wholesale:
    // that one is a 60x60 square hanging at (-15, 0, -6) game units, authored as stance-era
    // forgiveness, and lands nowhere near the visible shield under a controller-driven hand.
    // Shape is a symmetric trapezoid (top width, bottom width, height) so a Hylian-style
    // taper is expressible; strictness = set it smaller than the steel you see.
    const float halfWTop = CVarGetFloat("gVrPhysShieldWidthTop", 21.2f) * 50.0f;
    const float halfWBot = CVarGetFloat("gVrPhysShieldWidthBottom", 11.9f) * 50.0f;
    const float halfH = CVarGetFloat("gVrPhysShieldHeight", 18.0f) * 50.0f;
    const float cx = CVarGetFloat("gVrPhysShieldShiftX", -1.1f) * 100.0f;
    const float cy = CVarGetFloat("gVrPhysShieldShiftY", -0.8f) * 100.0f;
    const float cz = CVarGetFloat("gVrPhysShieldShiftZ", -2.5f) * 100.0f;
    const float d2r = 3.14159265f / 180.0f;
    const float pitch = CVarGetFloat("gVrPhysShieldPitch", -4.0f) * d2r;
    const float yaw = CVarGetFloat("gVrPhysShieldYaw", 0.0f) * d2r;
    const float roll = CVarGetFloat("gVrPhysShieldRoll", -90.0f) * d2r;

    // Width along +X, height along +Y (the vanilla quad's plane), tilted by pitch (about X)
    // and yaw (about Y), then rolled in the shield plane: R = Rz(roll) * Ry(yaw) * Rx(pitch).
    const float cp = cosf(pitch), sp = sinf(pitch);
    const float cyw = cosf(yaw), syw = sinf(yaw);
    const float cr = cosf(roll), sr = sinf(roll);
    const float axW[3] = { cr * cyw, sr * cyw, -syw };
    const float axH[3] = { cr * syw * sp - sr * cp, sr * syw * sp + cr * cp, cyw * sp };
    const float axN[3] = { cr * syw * cp + sr * sp, sr * syw * cp - cr * sp, cyw * cp };

    // The shifts live in the COLLIDER'S OWN frame — Across along its width axis, Up/Down
    // along its height axis, Out along its normal — so they keep meaning what their labels
    // say at any tilt (shifting in raw hand axes made "up/down" slide sideways, and no
    // rotation could fix it because the offsets didn't rotate with the quad).
    const float c3[3] = { axW[0] * cx + axH[0] * cy + axN[0] * cz,
                          axW[1] * cx + axH[1] * cy + axN[1] * cz,
                          axW[2] * cx + axH[2] * cy + axN[2] * cz };

    // Vanilla vertex order (the zigzag Collider_SetQuadVertices expects):
    // 0 = bottom-left, 1 = bottom-right, 2 = top-left, 3 = top-right.
    for (int axis = 0; axis < 3; axis++) {
        outXyz4[0 * 3 + axis] = c3[axis] - axW[axis] * halfWBot - axH[axis] * halfH;
        outXyz4[1 * 3 + axis] = c3[axis] + axW[axis] * halfWBot - axH[axis] * halfH;
        outXyz4[2 * 3 + axis] = c3[axis] - axW[axis] * halfWTop + axH[axis] * halfH;
        outXyz4[3 * 3 + axis] = c3[axis] + axW[axis] * halfWTop + axH[axis] * halfH;
    }
}

extern "C" bool VrCombat_ShieldFacingVeto(void* acCollider, void* atCollider) {
    // Called from CollisionCheck_SetATvsAC for EVERY confirmed AT-vs-AC hit: everything that
    // isn't the physical shield quad exits on the first pointer compare.
    if (gPlayState == NULL) {
        return false;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == NULL || acCollider != (void*)&player->shieldQuad.base) {
        return false;
    }
    if (!VrCombat_ShieldHeld(player)) {
        return false; // vanilla stance blocking is not judged
    }
    // gVrPhysShieldFacingDeg = the generosity cone: max angle between the shield's outward
    // normal and the direction to the attacker. 180 disables the gate.
    const float maxDeg = CVarGetFloat("gVrPhysShieldFacingDeg", 65.0f);
    if (maxDeg >= 179.0f) {
        return false;
    }
    Actor* attacker = ((Collider*)atCollider)->actor;
    if (attacker == NULL) {
        return false;
    }
    const Vec3f* q = player->shieldQuad.dim.quad;
    const Vec3f center = { (q[0].x + q[1].x + q[2].x + q[3].x) * 0.25f,
                           (q[0].y + q[1].y + q[2].y + q[3].y) * 0.25f,
                           (q[0].z + q[1].z + q[2].z + q[3].z) * 0.25f };
    // Quad normal from the two edges; sign oriented AWAY from the player's chest, so "front"
    // is always the outward face regardless of winding or hand pose.
    const Vec3f e1 = { q[1].x - q[0].x, q[1].y - q[0].y, q[1].z - q[0].z };
    const Vec3f e2 = { q[2].x - q[0].x, q[2].y - q[0].y, q[2].z - q[0].z };
    Vec3f n = { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
    const float nLen = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
    if (nLen < 1e-3f) {
        return false;
    }
    n.x /= nLen;
    n.y /= nLen;
    n.z /= nLen;
    const Vec3f chest = { player->actor.world.pos.x, player->actor.world.pos.y + 40.0f,
                          player->actor.world.pos.z };
    const Vec3f out = { center.x - chest.x, center.y - chest.y, center.z - chest.z };
    if (n.x * out.x + n.y * out.y + n.z * out.z < 0.0f) {
        n.x = -n.x;
        n.y = -n.y;
        n.z = -n.z;
    }
    // Attack direction: toward the attacker's focus point (chest/head for tall enemies, the
    // projectile itself for rocks and arrows).
    const Vec3f toAtk = { attacker->focus.pos.x - center.x, attacker->focus.pos.y - center.y,
                          attacker->focus.pos.z - center.z };
    const float dLen = sqrtf(toAtk.x * toAtk.x + toAtk.y * toAtk.y + toAtk.z * toAtk.z);
    if (dLen < 1e-3f) {
        return false;
    }
    const float cosang = (n.x * toAtk.x + n.y * toAtk.y + n.z * toAtk.z) / dLen;
    return cosang < cosf(maxDeg * (3.14159265f / 180.0f));
}

extern "C" int32_t VrCombat_ShieldBlockJudge(Player* player) {
    if (!VrCombat_ShieldHeld(player)) {
        return 0; // vanilla handling
    }
    // Any bounce that reached this point already passed the facing gate inside CollisionCheck
    // (bad-angle hits never bounce at all). The thump re-arms on a short cooldown instead of
    // buzzing while an attack grinds against the shield.
    if (sBlockHapticCooldown <= 0) {
        VR_TriggerHaptic(ShieldHand(), 1.0f, 0.0f, 90.0f);
        sBlockHapticCooldown = 3; // ~150 ms at 20 Hz
    }
    return 1;
}

namespace VrCombat {

void Shield_OnPlayerUpdate(PlayState* play, Player* player) {
    if (sBlockHapticCooldown > 0) {
        sBlockHapticCooldown--;
    }
    const bool held = VrCombat_ShieldHeld(player);
    if (held) {
        // Keep the shield in the off hand. Vanilla re-asserts this every frame while the
        // stance flag is up; we re-assert every tick while physically held (the patched gate
        // inside accepts VrCombat_ShieldHeld). Also converts the sheath model so the shield
        // leaves Link's back while it's in his hand.
        Player_SetModelsForHoldingShield(player);
    } else if (sWasHeld && player->rightHandType == PLAYER_MODELTYPE_RH_SHIELD) {
        // Falling edge (item change, two-hander drawn, mode off): re-derive hand and sheath
        // models from the current model group so the shield returns to Link's back — but ONLY
        // if the shield is actually still mounted. When the falling edge was CAUSED by the game
        // re-deriving models itself (a real put-away, or a visual-only stow that leaves
        // modelGroup pointing at the sword group), the hands are already correct and slamming
        // modelGroup back would re-equip the sword the game just stowed.
        Player_SetModels(player, player->modelGroup);
    }
    sWasHeld = held;
}

void Shield_Deactivate(PlayState* play, Player* player) {
    if (sWasHeld) {
        if (player->rightHandType == PLAYER_MODELTYPE_RH_SHIELD) {
            Player_SetModels(player, player->modelGroup); // see the falling-edge note above
        }
        sWasHeld = false;
    }
    sBlockHapticCooldown = 0;
}

} // namespace VrCombat
