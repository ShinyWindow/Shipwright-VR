#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Enhancements/vr-combat/VrCombat.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "overlays/actors/ovl_En_Arrow/z_en_arrow.h"

s32 func_808351D4(Player* thisx, PlayState* play); // Arrow nocked
void EnArrow_Init(Actor* thisx, PlayState* play);

extern PlayState* gPlayState;
}

#define CVAR_ARROW_CYCLE_NAME CVAR_ENHANCEMENT("BowArrowCycle")
#define CVAR_ARROW_CYCLE_DEFAULT 0
#define CVAR_ARROW_CYCLE_VALUE CVarGetInteger(CVAR_ARROW_CYCLE_NAME, CVAR_ARROW_CYCLE_DEFAULT)
#define CVAR_VR_ARROW_CYCLE_VALUE CVarGetInteger("gVrBowTriggerArrowCycle", 0)

static const s16 sMagicArrowCosts[] = { 4, 4, 8 };

static const PlayerItemAction sArrowCycleOrder[] = {
    PLAYER_IA_BOW,
    PLAYER_IA_BOW_FIRE,
    PLAYER_IA_BOW_ICE,
    PLAYER_IA_BOW_LIGHT,
};

static int32_t sPendingMountedIconItem = ITEM_NONE;

static bool IsHoldingBow(Player* player) {
    return player->heldItemAction >= PLAYER_IA_BOW && player->heldItemAction <= PLAYER_IA_BOW_LIGHT;
}

static bool IsHoldingMagicBow(Player* player) {
    return player->heldItemAction >= PLAYER_IA_BOW_FIRE && player->heldItemAction <= PLAYER_IA_BOW_LIGHT;
}

static bool IsAimingBow(Player* player) {
    return IsHoldingBow(player) && ((player->unk_6AD == 2) || (player->upperActionFunc == func_808351D4));
}

static bool HasArrowType(PlayerItemAction itemAction) {
    switch (itemAction) {
        case PLAYER_IA_BOW:
            return true;
        case PLAYER_IA_BOW_FIRE:
            return INV_CONTENT(ITEM_ARROW_FIRE) == ITEM_ARROW_FIRE && gSaveContext.magic >= sMagicArrowCosts[0];
        case PLAYER_IA_BOW_ICE:
            return INV_CONTENT(ITEM_ARROW_ICE) == ITEM_ARROW_ICE && gSaveContext.magic >= sMagicArrowCosts[1];
        case PLAYER_IA_BOW_LIGHT:
            return INV_CONTENT(ITEM_ARROW_LIGHT) == ITEM_ARROW_LIGHT && gSaveContext.magic >= sMagicArrowCosts[2];
        default:
            return false;
    }
}

static s32 GetBowItemForArrow(PlayerItemAction itemAction) {
    switch (itemAction) {
        case PLAYER_IA_BOW_FIRE:
            return ITEM_BOW_ARROW_FIRE;
        case PLAYER_IA_BOW_ICE:
            return ITEM_BOW_ARROW_ICE;
        case PLAYER_IA_BOW_LIGHT:
            return ITEM_BOW_ARROW_LIGHT;
        default:
            return ITEM_BOW;
    }
}

static ArrowType GetArrowTypeForArrow(s8 itemAction) {
    switch (itemAction) {
        case PLAYER_IA_BOW_FIRE:
            return ARROW_FIRE;
        case PLAYER_IA_BOW_ICE:
            return ARROW_ICE;
        case PLAYER_IA_BOW_LIGHT:
            return ARROW_LIGHT;
        default:
            return ARROW_NORMAL;
    }
}

static PlayerItemAction GetItemActionForArrow(const EnArrow* arrow, const Player* player) {
    switch (arrow->actor.params) {
        case ARROW_FIRE:
            return PLAYER_IA_BOW_FIRE;
        case ARROW_ICE:
            return PLAYER_IA_BOW_ICE;
        case ARROW_LIGHT:
            return PLAYER_IA_BOW_LIGHT;
        default:
            return (PlayerItemAction)player->heldItemAction;
    }
}

static void ChangeMountedArrowType(EnArrow* arrow, PlayerItemAction itemAction) {
    // Mounted Link's bow state only accepts PLAYER_IA_BOW. Changing itemAction or re-running
    // EnArrow_Init makes the horse action release the parented arrow and can leave Link stuck
    // aiming. Change only the live projectile's elemental payload; EnArrow_Update will create
    // the corresponding elemental child on its next update.
    if (arrow->actor.child != NULL) {
        Actor_Kill(arrow->actor.child);
        arrow->actor.child = NULL;
    }

    static const u32 damageFlags[] = { 0x00000020, 0x00000800, 0x00001000, 0x00002000 };
    const int index = itemAction - PLAYER_IA_BOW;
    arrow->actor.params = itemAction == PLAYER_IA_BOW ? ARROW_NORMAL_HORSE : GetArrowTypeForArrow(itemAction);
    arrow->collider.info.toucher.dmgFlags = damageFlags[index];

    // Epona must keep ITEM_BOW in the temporary B slot, but the HUD texture can still show the
    // selected elemental arrow. Interface_LoadItemIcon1 resolves the texture synchronously from
    // buttonItems[0], so expose the display item only for that load.
    sPendingMountedIconItem = GetBowItemForArrow(itemAction);
}

static void FlushMountedBowIcon() {
    if (sPendingMountedIconItem == ITEM_NONE || gPlayState == nullptr) {
        return;
    }

    // Run after the ordinary interface update, which restores Epona's temporary B icon during
    // the frame. Load only the alternate texture, then immediately restore the required B item.
    const uint8_t horseBowItem = gSaveContext.equips.buttonItems[0];
    gSaveContext.equips.buttonItems[0] = sPendingMountedIconItem;
    Interface_LoadItemIcon1(gPlayState, 0);
    gSaveContext.equips.buttonItems[0] = horseBowItem;
    sPendingMountedIconItem = ITEM_NONE;
}

static bool CanCycleArrows() {
    Player* player = GET_PLAYER(gPlayState);
    const bool vrPhysicalCycle = CVAR_VR_ARROW_CYCLE_VALUE && VR_IsInitialized() && VrArchery_Covers(player);

    return (LINK_IS_ADULT || CVarGetInteger(CVAR_CHEAT("TimelessEquipment"), 0)) && !gSaveContext.minigameState &&
           gPlayState->sceneNum != SCENE_SHOOTING_GALLERY &&
           (!(player->stateFlags1 & PLAYER_STATE1_ON_HORSE) || vrPhysicalCycle) &&
           (player->rideActor == NULL || vrPhysicalCycle) && INV_CONTENT(SLOT_BOW) == ITEM_BOW &&
           (INV_CONTENT(ITEM_ARROW_FIRE) == ITEM_ARROW_FIRE || INV_CONTENT(ITEM_ARROW_ICE) == ITEM_ARROW_ICE ||
            INV_CONTENT(ITEM_ARROW_LIGHT) == ITEM_ARROW_LIGHT);
}

static s8 GetNextArrowType(s8 currentArrowType) {
    int currentIndex = 0;
    for (int i = 0; i < (int)ARRAY_COUNT(sArrowCycleOrder); i++) {
        if (sArrowCycleOrder[i] == currentArrowType) {
            currentIndex = i;
            break;
        }
    }

    for (int offset = 1; offset <= (int)ARRAY_COUNT(sArrowCycleOrder); offset++) {
        int nextIndex = (currentIndex + offset) % ARRAY_COUNT(sArrowCycleOrder);
        if (HasArrowType(sArrowCycleOrder[nextIndex])) {
            return sArrowCycleOrder[nextIndex];
        }
    }

    return PLAYER_IA_BOW;
}

static void UpdateButtonAlpha(s16 flashAlpha, bool isButtonBow, u16* buttonAlpha) {
    if (isButtonBow) {
        *buttonAlpha = flashAlpha;
    }
}

static void UpdateEquippedBow(PlayState* play, s8 arrowType) {
    s32 bowItem = GetBowItemForArrow((PlayerItemAction)arrowType);
    bool dpadEnabled = CVarGetInteger(CVAR_ENHANCEMENT("DpadEquips"), 0);
    s32 maxButton = dpadEnabled ? 7 : 3;

    for (s32 i = 1; i <= maxButton; i++) {
        if ((gSaveContext.equips.buttonItems[i] == ITEM_BOW) ||
            (gSaveContext.equips.buttonItems[i] >= ITEM_BOW_ARROW_FIRE &&
             gSaveContext.equips.buttonItems[i] <= ITEM_BOW_ARROW_LIGHT)) {
            gSaveContext.equips.buttonItems[i] = bowItem;
            gSaveContext.equips.cButtonSlots[i - 1] = SLOT_BOW;

            if (i <= 3) {
                Interface_LoadItemIcon1(play, i);
            }

            gSaveContext.buttonStatus[i] = BTN_ENABLED;
        }
    }
}

extern "C" bool ArrowCycleMain(void) {
    if (gPlayState == nullptr || !CanCycleArrows()) {
        return false;
    }

    Player* player = GET_PLAYER(gPlayState);
    if (player->heldActor != NULL && player->heldActor->id == ACTOR_EN_ARROW) {
        if (IsHoldingMagicBow(player) && gSaveContext.magicState != MAGIC_STATE_IDLE && player->heldActor == NULL) {
            Audio_PlaySoundGeneral(NA_SE_SY_ERROR, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale,
                                   &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
            return true;
        }

        // reset magic state to IDLE before cycling to prevent error sound
        gSaveContext.magicState = MAGIC_STATE_IDLE;

        EnArrow* enArrow = (EnArrow*)player->heldActor;
        const bool mounted = (player->stateFlags1 & PLAYER_STATE1_ON_HORSE) != 0;
        s8 nextArrow = GetNextArrowType(mounted ? GetItemActionForArrow(enArrow, player) : player->heldItemAction);

        if (mounted) {
            ChangeMountedArrowType(enArrow, (PlayerItemAction)nextArrow);
            return true;
        }

        player->heldItemAction = nextArrow;
        player->itemAction = nextArrow;
        Actor* arrow = player->heldActor;

        if (arrow->child != NULL) {
            Actor_Kill(arrow->child);
            arrow->child = NULL;
        }
        arrow->params = GetArrowTypeForArrow(nextArrow);
        EnArrow_Init(arrow, gPlayState);
        UpdateEquippedBow(gPlayState, nextArrow);
        return true;
    }
    return false;
}

void RegisterArrowCycle() {
    COND_HOOK(OnPlayDrawEnd, CVAR_VR_ARROW_CYCLE_VALUE, FlushMountedBowIcon);
    // suppress shield input when R is held while aiming to allow arrow cycling
    COND_VB_SHOULD(VB_EXECUTE_PLAYER_ACTION_FUNC, CVAR_ARROW_CYCLE_VALUE || CVAR_VR_ARROW_CYCLE_VALUE, {
        Player* player = (Player*)va_arg(args, void*);
        Input* input = (Input*)va_arg(args, void*);
        if (CVAR_ARROW_CYCLE_VALUE && IsAimingBow(player) && CHECK_BTN_ANY(input->press.button, BTN_R)) {
            if (ArrowCycleMain()) {
                input->cur.button &= ~BTN_R;
                input->press.button &= ~BTN_R;
            }
        }
    });

    // don't consume magic on draw, but check if we have enough to fire
    COND_VB_SHOULD(VB_PLAYER_ARROW_MAGIC_CONSUMPTION, CVAR_ARROW_CYCLE_VALUE || CVAR_VR_ARROW_CYCLE_VALUE, {
        Player* player = va_arg(args, Player*);
        int32_t magicArrowType = va_arg(args, int32_t);
        int32_t* arrowType = va_arg(args, int32_t*);

        if (gSaveContext.magic < sMagicArrowCosts[magicArrowType]) {
            *arrowType = ARROW_NORMAL;
        } else {
            *should = false;
        }
    });

    COND_VB_SHOULD(VB_EN_ARROW_MAGIC_CONSUMPTION, CVAR_ARROW_CYCLE_VALUE || CVAR_VR_ARROW_CYCLE_VALUE, {
        EnArrow* arrow = va_arg(args, EnArrow*);

        if (arrow->actor.params < ARROW_FIRE || arrow->actor.params > ARROW_LIGHT) {
            return;
        }

        int32_t magicArrowType = arrow->actor.params - ARROW_FIRE;
        Magic_RequestChange(gPlayState, sMagicArrowCosts[magicArrowType], MAGIC_CONSUME_NOW);
    });
}

static RegisterShipInitFunc initFunc(RegisterArrowCycle, { CVAR_ARROW_CYCLE_NAME, "gVrBowTriggerArrowCycle" });
