#include "global.h"
#include "vt.h"
#include <string.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/controls/Mouse.h"
#include "soh/Enhancements/vr-combat/VrCombat.h"
#include "soh/OTRGlobals.h"
#include "soh/ResourceManagerHelpers.h"

#include <vr_interface.h>

s32 D_8012D280 = 1;

void OTRControllerCallback(uint8_t rumble);

OSMesgQueue* PadMgr_LockSerialMesgQueue(PadMgr* padMgr) {
    OSMesgQueue* ctrlrQ = NULL;

    if (D_8012D280 > 2) {
        // "serialMsgQ Waiting for lock"
        osSyncPrintf("%2d %d serialMsgQロック待ち         %08x %08x          %08x\n", osGetThreadId(NULL),
                     padMgr->serialMsgQ.validCount, padMgr, &padMgr->serialMsgQ, &ctrlrQ);
    }

    osRecvMesg(&padMgr->serialMsgQ, (OSMesg*)&ctrlrQ, OS_MESG_BLOCK);

    if (D_8012D280 > 2) {
        // "serialMsgQ Locked"
        osSyncPrintf("%2d %d serialMsgQをロックしました                     %08x\n", osGetThreadId(NULL),
                     padMgr->serialMsgQ.validCount, ctrlrQ);
    }

    return ctrlrQ;
}

void PadMgr_UnlockSerialMesgQueue(PadMgr* padMgr, OSMesgQueue* ctrlrQ) {
    if (D_8012D280 > 2) {
        // "serialMsgQ Unlock"
        osSyncPrintf("%2d %d serialMsgQロック解除します   %08x %08x %08x\n", osGetThreadId(NULL),
                     padMgr->serialMsgQ.validCount, padMgr, &padMgr->serialMsgQ, ctrlrQ);
    }

    osSendMesgPtr(&padMgr->serialMsgQ, ctrlrQ, OS_MESG_BLOCK);

    if (D_8012D280 > 2) {
        // "serialMsgQ Unlocked"
        osSyncPrintf("%2d %d serialMsgQロック解除しました %08x %08x %08x\n", osGetThreadId(NULL),
                     padMgr->serialMsgQ.validCount, padMgr, &padMgr->serialMsgQ, ctrlrQ);
    }
}

void PadMgr_LockPadData(PadMgr* padMgr) {
    osRecvMesg(&padMgr->lockMsgQ, NULL, OS_MESG_BLOCK);
}

void PadMgr_UnlockPadData(PadMgr* padMgr) {
    osSendMesgPtr(&padMgr->lockMsgQ, NULL, OS_MESG_BLOCK);
}

void PadMgr_RumbleControl(PadMgr* padMgr) {
    static u32 errcnt = 0;
    static u32 frame;
    s32 temp = 1;
    s32 triedRumbleComm;
    OSMesgQueue* ctrlrQ = PadMgr_LockSerialMesgQueue(padMgr);
    s32 var4;
    s32 i;

    triedRumbleComm = 0;

    for (i = 0; i < 4; i++) {
        if (padMgr->ctrlrIsConnected[i]) {
            if (padMgr->padStatus[i].status & 1) {
                if (padMgr->pakType[i] == temp) {
                    if (padMgr->rumbleEnable[i] != 0) {
                        if (padMgr->rumbleCounter[i] < 3) {
                            // clang-format off
                            osSyncPrintf(VT_FGCOL(YELLOW));
                            // clang-format on

                            // "Vibration pack jumble jumble"?
                            osSyncPrintf("padmgr: %dコン: %s\n", i + 1, "振動パック ぶるぶるぶるぶる");
                            osSyncPrintf(VT_RST);

                            if (__osMotorAccess(&padMgr->pfs[i], temp) != 0) {
                                padMgr->pakType[i] = 0;
                                osSyncPrintf(VT_FGCOL(YELLOW));
                                // "A communication error has occurred with the vibration pack"
                                osSyncPrintf("padmgr: %dコン: %s\n", i + 1, "振動パックで通信エラーが発生しました");
                                osSyncPrintf(VT_RST);
                            } else {
                                padMgr->rumbleCounter[i] = 3;
                            }

                            triedRumbleComm = 1;
                        }
                    } else {
                        if (padMgr->rumbleCounter[i] != 0) {
                            // clang-format off
                            osSyncPrintf(VT_FGCOL(YELLOW));
                            // clang-format on

                            // "Stop vibration pack"
                            osSyncPrintf("padmgr: %dコン: %s\n", i + 1, "振動パック 停止");
                            osSyncPrintf(VT_RST);

                            if (osMotorStop(&padMgr->pfs[i]) != 0) {
                                padMgr->pakType[i] = 0;
                                osSyncPrintf(VT_FGCOL(YELLOW));
                                // "A communication error has occurred with the vibration pack"
                                osSyncPrintf("padmgr: %dコン: %s\n", i + 1, "振動パックで通信エラーが発生しました");
                                osSyncPrintf(VT_RST);
                            } else {
                                padMgr->rumbleCounter[i]--;
                            }

                            triedRumbleComm = 1;
                        }
                    }
                }
            } else {
                if (padMgr->pakType[i] != 0) {
                    if (padMgr->pakType[i] == 1) {
                        osSyncPrintf(VT_FGCOL(YELLOW));
                        // "It seems that a vibration pack was pulled out"
                        osSyncPrintf("padmgr: %dコン: %s\n", i + 1, "振動パックが抜かれたようです");
                        osSyncPrintf(VT_RST);
                        padMgr->pakType[i] = 0;
                    } else {
                        osSyncPrintf(VT_FGCOL(YELLOW));
                        // "It seems that a controller pack that is not a vibration pack was pulled out"
                        osSyncPrintf("padmgr: %dコン: %s\n", i + 1,
                                     "振動パックではないコントローラパックが抜かれたようです");
                        osSyncPrintf(VT_RST);
                        padMgr->pakType[i] = 0;
                    }
                }
            }
        }
    }

    if (!triedRumbleComm) {
        i = frame % 4;

        if (padMgr->ctrlrIsConnected[i] && (padMgr->padStatus[i].status & 1) && (padMgr->pakType[i] != 1)) {
            var4 = osMotorInit(ctrlrQ, &padMgr->pfs[i], i);

            if (var4 == 0) {
                padMgr->pakType[i] = 1;
                osMotorStart(&padMgr->pfs[i]);
                osMotorStop(&padMgr->pfs[i]);
                osSyncPrintf(VT_FGCOL(YELLOW));
                // "Recognized vibration pack"
                osSyncPrintf("padmgr: %dコン: %s\n", i + 1, "振動パックを認識しました");
                osSyncPrintf(VT_RST);
            } else if (var4 == 11) {
                padMgr->pakType[i] = 2;
            } else if (var4 == 4) {
                LOG_NUM("++errcnt", ++errcnt);
                osSyncPrintf(VT_FGCOL(YELLOW));
                // "Controller pack communication error"
                osSyncPrintf("padmgr: %dコン: %s\n", i + 1, "コントローラパックの通信エラー");
                osSyncPrintf(VT_RST);
            }
        }
    }

    frame++;
    PadMgr_UnlockSerialMesgQueue(padMgr, ctrlrQ);
}

void PadMgr_RumbleStop(PadMgr* padMgr) {
    s32 i;
    OSMesgQueue* ctrlrQ = PadMgr_LockSerialMesgQueue(padMgr);

    for (i = 0; i < 4; i++) {
        if (osMotorInit(ctrlrQ, &padMgr->pfs[i], i) == 0) {
#if 0
            if ((gFaultStruct.msgId == 0) && (padMgr->rumbleOnFrames != 0))
            {
                osSyncPrintf(VT_FGCOL(YELLOW));
                // "Stop vibration pack"
                osSyncPrintf("padmgr: %dコン: %s\n", i + 1, "振動パック 停止");
                osSyncPrintf(VT_RST);
            }
#endif

            osMotorStop(&padMgr->pfs[i]);
        }
    }

    PadMgr_UnlockSerialMesgQueue(padMgr, ctrlrQ);
}

void PadMgr_RumbleReset(PadMgr* padMgr) {
    padMgr->rumbleOffFrames = 3;
}

void PadMgr_RumbleSetSingle(PadMgr* padMgr, u32 ctrlr, u32 rumble) {
    padMgr->rumbleEnable[ctrlr] = rumble;
    padMgr->rumbleOnFrames = 240;
}

void PadMgr_RumbleSet(PadMgr* padMgr, u8* ctrlrRumbles) {
    s32 i;

    for (i = 0; i < 4; i++) {
        padMgr->rumbleEnable[i] = ctrlrRumbles[i];
    }

    padMgr->rumbleOnFrames = 240;
}

#define PAUSE_BUFFER_INPUT_BLOCK_ID 0
void PadMgr_ProcessInputs(PadMgr* padMgr) {
    s32 i;
    Input* input;
    OSContPad* padnow1; // original name
    s32 buttonDiff;

    PadMgr_LockPadData(padMgr);

    input = &padMgr->inputs[0];
    padnow1 = &padMgr->pads[0];

    for (i = 0; i < padMgr->nControllers; i++, input++, padnow1++) {
        input->prev = input->cur;

        switch (padnow1->err_no) {
            case 0:
                input->cur = *padnow1;

                if (GameInteractor_DisableZTargetingActive()) {
                    input->cur.button &= ~(BTN_Z);
                }

                uint32_t emulatedButtons = GameInteractor_GetEmulatedButtons();
                if (emulatedButtons) {
                    input->cur.button |= emulatedButtons;
                    GameInteractor_SetEmulatedButtons(0);
                }

                if (GameInteractor_ReverseControlsActive()) {
                    if (input->cur.stick_x == -128) {
                        input->cur.stick_x = 127;
                    } else {
                        input->cur.stick_x *= -1;
                    }

                    if (input->cur.stick_y == -128) {
                        input->cur.stick_y = 127;
                    } else {
                        input->cur.stick_y *= -1;
                    }
                }

                if (!padMgr->ctrlrIsConnected[i]) {
                    padMgr->ctrlrIsConnected[i] = true;
                    osSyncPrintf(VT_FGCOL(YELLOW));
                    osSyncPrintf("padmgr: %dコン: %s\n", i + 1, "認識しました"); // "Recognized"
                    osSyncPrintf(VT_RST);
                }
                break;
            case 4:
                input->cur = input->prev;
                LOG_NUM("this->Key_switch[i]", padMgr->ctrlrIsConnected[i]);
                osSyncPrintf(VT_FGCOL(YELLOW));
                // "Overrun error occurred"
                osSyncPrintf("padmgr: %dコン: %s\n", i + 1, "オーバーランエラーが発生");
                osSyncPrintf(VT_RST);
                break;
            case 8:
                input->cur.button = 0;
                input->cur.stick_x = 0;
                input->cur.stick_y = 0;
                input->cur.err_no = padnow1->err_no;
                if (padMgr->ctrlrIsConnected[i]) {
                    padMgr->ctrlrIsConnected[i] = false;
                    padMgr->pakType[i] = 0;
                    padMgr->rumbleCounter[i] = 0xFF;
                    osSyncPrintf(VT_FGCOL(YELLOW));
                    // "Do not respond"?
                    osSyncPrintf("padmgr: %dコン: %s\n", i + 1, "応答しません");
                    osSyncPrintf(VT_RST);
                }
                break;
            default:
                LOG_HEX("padnow1->errno", padnow1->err_no);
                Fault_AddHungupAndCrash(__FILE__, __LINE__);
        }

        buttonDiff = input->prev.button ^ input->cur.button;
        input->press.button |= (u16)(buttonDiff & input->cur.button);
        input->rel.button |= (u16)(buttonDiff & input->prev.button);
        PadUtils_UpdateRelXY(input);
        input->press.stick_x += (s8)(input->cur.stick_x - input->prev.stick_x);
        input->press.stick_y += (s8)(input->cur.stick_y - input->prev.stick_y);
        // #region SOH [Enhancement]
        PadUtils_UpdateRelRXY(input);
        input->press.right_stick_x += (s8)(input->cur.right_stick_x - input->prev.right_stick_x);
        input->press.right_stick_y += (s8)(input->cur.right_stick_y - input->prev.right_stick_y);
        // #endregion
    }

    uint8_t rumble = (padMgr->rumbleEnable[0] > 0);
    OTRControllerCallback(rumble);

    PadMgr_UnlockPadData(padMgr);
}

void PadMgr_HandleRetraceMsg(PadMgr* padMgr) {
    s32 i;
    OSMesgQueue* queue = PadMgr_LockSerialMesgQueue(padMgr);
    u32 mask;

    osContStartReadData(queue);
    if (padMgr->retraceCallback) {
        padMgr->retraceCallback(padMgr, padMgr->retraceCallbackValue);
    }
    osRecvMesg(queue, NULL, OS_MESG_BLOCK);
    osContGetReadData(padMgr->pads);

    // #region SOH [VR] Map the OpenXR controllers onto the N64 pad (port 0) so the headset controllers
    // are the default game controller everywhere — gameplay AND menus (this runs before
    // PadMgr_ProcessInputs, which derives press/rel). OR'd in, so keyboard/gamepad still work alongside.
    if (VR_IsInitialized() && CVarGetInteger("gVrControllerInput", 1)) {
        OSContPad* vrPad = &padMgr->pads[0];
        uint16_t vrL = VR_GetControllerButton(VR_HAND_LEFT);
        uint16_t vrR = VR_GetControllerButton(VR_HAND_RIGHT);
        // Set per hand while the ocarina stick-direction bindings claim that thumbstick (see the
        // ocarina block below): the stick's stock job stands down for the duration so a bound
        // direction never double-fires alongside it.
        s32 vrOcaStickClaimed[2] = { 0, 0 };

        // Assignable VR input -> N64 button mapping, configured in VR Settings -> VR Inputs. Each
        // CVar holds an N64 BUTTON BITMASK (same encoding as the base game's button-combination
        // selector), so one controller input may press a single button or a whole combination.
        {
            // TWO PROFILES, picked by gVrItemSelect, so a player can swap the whole control
            // scheme back and forth from the menu without re-binding anything.
            //
            // CLASSIC (selector off): items are activated by the C buttons you assigned them to.
            // Defaults: left = Z-target, R-shield, C-left, C-right, Start (stick click), none;
            //           right = B-sword, none, A, C-down, none, none.
            // C-Up ships deliberately UNBOUND — ocarina notes come from the dedicated OCARINA
            // set below, so no gameplay binding needs to carry a note.
            static const char* sVrBindCvars[2][6] = {
                { "gVrBindLTrigger", "gVrBindLGrip", "gVrBindLPrimary", "gVrBindLSecondary", "gVrBindLStickClick",
                  "gVrBindLMenu" },
                { "gVrBindRTrigger", "gVrBindRGrip", "gVrBindRPrimary", "gVrBindRSecondary", "gVrBindRStickClick",
                  "gVrBindRMenu" },
            };
            static const s32 sVrBindDefaults[2][6] = {
                { BTN_Z, BTN_R, BTN_CLEFT, BTN_CRIGHT, BTN_START, 0 },
                { BTN_B, 0, BTN_A, BTN_CDOWN, 0, 0 },
            };
            // SELECTOR (selector on): the Alyx-style selector equips and the holding hand's
            // TRIGGER uses the item, so both triggers are reserved (unbound, and skipped below)
            // and the C buttons no longer pull anything out — they are left unbound here
            // (ocarina notes live in the dedicated set below). Everything the triggers used to
            // carry moves:
            // Z-target to the sword-hand grip, the shield stays on the off-hand grip, A and B
            // on the face buttons. Start is bound on BOTH the left stick click and the left menu
            // button, because whichever stick click the selector is set to is eaten by it.
            // Keep in sync with sVrInputDefsSelector in SohMenuVRSettings.cpp.
            static const char* sVrBindSelCvars[2][6] = {
                { "gVrBindSelLTrigger", "gVrBindSelLGrip", "gVrBindSelLPrimary", "gVrBindSelLSecondary",
                  "gVrBindSelLStickClick", "gVrBindSelLMenu" },
                { "gVrBindSelRTrigger", "gVrBindSelRGrip", "gVrBindSelRPrimary", "gVrBindSelRSecondary",
                  "gVrBindSelRStickClick", "gVrBindSelRMenu" },
            };
            static const s32 sVrBindSelDefaults[2][6] = {
                { 0, BTN_R, 0, 0, BTN_START, BTN_START },
                { 0, BTN_Z, BTN_A, BTN_B, 0, 0 },
            };
            // OCARINA (the ocarina interface is up, in EITHER profile): the five notes get their
            // own bindable set, because neither gameplay profile has all five note buttons — in
            // selector mode the C notes had NOTHING to live on, which made the ocarina unplayable.
            // While VrOcarina_InPlay() this set replaces the gameplay bindings outright and every
            // selector reservation stands down (both triggers, the selector click, the held-item
            // trigger mirror — see VrItemSelect.cpp), so notes may sit anywhere. Defaults, notes
            // low to high: D4 (A) = R trigger, F4 (C-down) = L trigger, A4 (C-right) = A,
            // B4 (C-left) = X, D5 (C-up) = Y; sharpen (R) = right grip, flatten (Z) = left grip;
            // B (put the ocarina away) = B button. Pitch bend rides the left thumbstick, which is
            // already the N64 analog stick below. Keep in sync with sVrInputDefsOcarina in
            // SohMenuVRSettings.cpp.
            static const char* sVrBindOcaCvars[2][6] = {
                { "gVrBindOcaLTrigger", "gVrBindOcaLGrip", "gVrBindOcaLPrimary", "gVrBindOcaLSecondary",
                  "gVrBindOcaLStickClick", "gVrBindOcaLMenu" },
                { "gVrBindOcaRTrigger", "gVrBindOcaRGrip", "gVrBindOcaRPrimary", "gVrBindOcaRSecondary",
                  "gVrBindOcaRStickClick", "gVrBindOcaRMenu" },
            };
            static const s32 sVrBindOcaDefaults[2][6] = {
                { BTN_CDOWN, BTN_Z, BTN_CLEFT, BTN_CUP, 0, 0 },
                { BTN_A, BTN_R, BTN_CRIGHT, BTN_B, 0, 0 },
            };
            // OCARINA stick directions: in this set ONLY, the four cardinal deflections of each
            // thumbstick are bindable inputs too (gameplay sticks keep their stock jobs: move and
            // turn). Dominant-axis with the same 0.5 threshold as the stock C-stick mapping, so a
            // diagonal flick plays one note, not two. All unbound by default. A hand with any
            // direction bound claims that stick outright — vrOcaStickClaimed above — standing
            // down its stock ocarina job (left: pitch bend / analog stick; right: third-person
            // C-stick). Order: up, down, left, right. Keep in sync with sVrInputDefsOcarina in
            // SohMenuVRSettings.cpp.
            static const char* sVrBindOcaStickCvars[2][4] = {
                { "gVrBindOcaLStickUp", "gVrBindOcaLStickDown", "gVrBindOcaLStickLeft", "gVrBindOcaLStickRight" },
                { "gVrBindOcaRStickUp", "gVrBindOcaRStickDown", "gVrBindOcaRStickLeft", "gVrBindOcaRStickRight" },
            };
            static const u16 sVrBtnMasks[6] = { VR_BTN_TRIGGER,   VR_BTN_GRIP,       VR_BTN_PRIMARY,
                                                VR_BTN_SECONDARY, VR_BTN_THUMBCLICK, VR_BTN_MENU };
            s32 vrSelProfile = CVarGetInteger("gVrItemSelect", 1);
            s32 vrOcarina = VrOcarina_InPlay();
            s32 vrHandIdx, vrBtnIdx;
            for (vrHandIdx = 0; vrHandIdx < 2; vrHandIdx++) {
                uint16_t vrState = (vrHandIdx == 0) ? vrL : vrR;
                for (vrBtnIdx = 0; vrBtnIdx < 6; vrBtnIdx++) {
                    // SOH [VR] The Alyx-style item selector owns its configured input outright:
                    // its normal binding never fires (the opening click must not leak). In
                    // selector mode both triggers belong to item use, and the two-hand quick-swap
                    // chord suspends its inputs' bindings while fully held. While aiming a
                    // projectile (classic mode), the aim hand's trigger is the FIRE control and
                    // its binding is likewise suspended.
                    if (VrItemSelect_ConsumesInput(vrHandIdx, sVrBtnMasks[vrBtnIdx]) ||
                        VrItemSelect_TriggerConsumed(vrHandIdx, sVrBtnMasks[vrBtnIdx]) ||
                        VrItemSelect_SwapConsumed(vrHandIdx, sVrBtnMasks[vrBtnIdx]) ||
                        VrItemThrow_GripConsumed(vrHandIdx, sVrBtnMasks[vrBtnIdx]) ||
                        VrBoomerang_GripConsumed(vrHandIdx, sVrBtnMasks[vrBtnIdx]) ||
                        VrLens_GripConsumed(vrHandIdx, sVrBtnMasks[vrBtnIdx]) ||
                        VrMask_GripConsumed(vrHandIdx, sVrBtnMasks[vrBtnIdx]) ||
                        VrHammer_GripConsumed(vrHandIdx, sVrBtnMasks[vrBtnIdx]) ||
                        VrArchery_PinchConsumed(vrHandIdx, sVrBtnMasks[vrBtnIdx]) ||
                        VrCombat_AimTriggerConsumed(vrHandIdx, sVrBtnMasks[vrBtnIdx])) {
                        continue;
                    }
                    if (vrState & sVrBtnMasks[vrBtnIdx]) {
                        s32 mapped = vrOcarina      ? CVarGetInteger(sVrBindOcaCvars[vrHandIdx][vrBtnIdx],
                                                                     sVrBindOcaDefaults[vrHandIdx][vrBtnIdx])
                                     : vrSelProfile ? CVarGetInteger(sVrBindSelCvars[vrHandIdx][vrBtnIdx],
                                                                     sVrBindSelDefaults[vrHandIdx][vrBtnIdx])
                                                    : CVarGetInteger(sVrBindCvars[vrHandIdx][vrBtnIdx],
                                                                     sVrBindDefaults[vrHandIdx][vrBtnIdx]);
                        // Mask to the real pad bits (drops the PC-only modifier bits if selected).
                        vrPad->button |= (u16)(mapped & 0xFFFF);
                    }
                }
            }

            // SOH [VR] Selector mode: the trigger of the hand HOLDING the item is that item's
            // button. Mirrored as raw button STATE, not a one-frame emulated press, so the
            // vanilla pad path derives press, held and release from it by itself — press nocks
            // the bow, holding keeps it drawn, releasing looses the arrow, and every other item
            // (bombs, bottles, boomerang aim-and-throw, magic) obeys its own vanilla rules with
            // nothing re-implemented. Returns 0 when nothing is held in that hand.
            for (vrHandIdx = 0; vrHandIdx < 2; vrHandIdx++) {
                if (((vrHandIdx == 0) ? vrL : vrR) & VR_BTN_TRIGGER) {
                    vrPad->button |= VrItemSelect_TriggerItemMask(vrHandIdx);
                }
            }

            // SOH [VR] Physical archery: a pinched nock IS the weapon's item button, held
            // down for as long as the string hand keeps the pinch — the same raw-state mirror
            // as the trigger above, so nock/draw/release all come out of the vanilla pad path.
            vrPad->button |= VrArchery_ItemButtonMask();

            // SOH [VR] Ocarina stick-direction bindings (sVrBindOcaStickCvars above): fire the
            // bound mask while the dominant axis holds past the threshold — raw button STATE,
            // like everything else here, so press/rel derive naturally.
            if (vrOcarina) {
                for (vrHandIdx = 0; vrHandIdx < 2; vrHandIdx++) {
                    s32 vrDirIdx;
                    s32 vrHeldDir = -1;
                    float vrSx = 0.0f, vrSy = 0.0f;
                    for (vrDirIdx = 0; vrDirIdx < 4; vrDirIdx++) {
                        if (CVarGetInteger(sVrBindOcaStickCvars[vrHandIdx][vrDirIdx], 0) != 0) {
                            vrOcaStickClaimed[vrHandIdx] = 1;
                        }
                    }
                    if (!vrOcaStickClaimed[vrHandIdx]) {
                        continue;
                    }
                    VR_GetThumbstick(vrHandIdx, &vrSx, &vrSy);
                    if ((vrSy * vrSy) >= (vrSx * vrSx)) {
                        vrHeldDir = (vrSy > 0.5f) ? 0 : (vrSy < -0.5f) ? 1 : -1;
                    } else {
                        vrHeldDir = (vrSx < -0.5f) ? 2 : (vrSx > 0.5f) ? 3 : -1;
                    }
                    if (vrHeldDir >= 0) {
                        s32 mapped = CVarGetInteger(sVrBindOcaStickCvars[vrHandIdx][vrHeldDir], 0);
                        // Mask to the real pad bits (drops the PC-only modifier bits if selected).
                        vrPad->button |= (u16)(mapped & 0xFFFF);
                    }
                }
            }
        }

        // Left thumbstick -> movement (control stick). Overrides only when actually pushed (deadzone),
        // so it doesn't zero out a keyboard/gamepad stick when idle.
        float lx = 0.0f, ly = 0.0f;
        VR_GetThumbstick(VR_HAND_LEFT, &lx, &ly);
        if (!vrOcaStickClaimed[VR_HAND_LEFT] && ((lx * lx) + (ly * ly)) > (0.15f * 0.15f)) {
            vrPad->stick_x = (s8)CLAMP(lx * 127.0f, -128.0f, 127.0f);
            vrPad->stick_y = (s8)CLAMP(ly * 127.0f, -128.0f, 127.0f);
        }

        // Right thumbstick -> C-buttons ONLY where the game genuinely owns the stick as the
        // stock C-stick: flat-screen menus (the pause inventory ASSIGNS items with C presses)
        // and THIRD PERSON. In first-person play the stick NEVER fires items or C-buttons —
        // a stick position is far too easy to graze mid-play; items live on the bindable VR
        // Inputs and the Alyx-style selector, and the X axis belongs to artificial turning.
        float rx = 0.0f, ry = 0.0f;
        VR_GetThumbstick(VR_HAND_RIGHT, &rx, &ry);
        if (!vrOcaStickClaimed[VR_HAND_RIGHT] && (VR_IsFlatScreen() || !VR_GetFirstPerson())) {
            if (ry > 0.5f) vrPad->button |= BTN_CUP;
            if (ry < -0.5f) vrPad->button |= BTN_CDOWN;
            if (rx > 0.5f) vrPad->button |= BTN_CRIGHT;
            if (rx < -0.5f) vrPad->button |= BTN_CLEFT;
        }
    }
    // #endregion

    Mouse_UpdateAll();

    for (i = 0; i < __osMaxControllers; i++) {
        padMgr->padStatus[i].status = Controller_ShouldRumble(i);
    }

    if (padMgr->preNMIShutdown) {
        memset(padMgr->pads, 0, sizeof(padMgr->pads));
    }
    PadMgr_ProcessInputs(padMgr);
    osContStartQuery(queue);
    osRecvMesg(queue, NULL, OS_MESG_BLOCK);
    osContGetQuery(padMgr->padStatus);
    PadMgr_UnlockSerialMesgQueue(padMgr, queue);

    mask = 0;
    for (i = 0; i < 4; i++) {
        if (padMgr->padStatus[i].err_no == 0) {
            if (padMgr->padStatus[i].type == CONT_TYPE_NORMAL) {
                mask |= 1 << i;
            } else {
                // LOG_HEX("this->pad_status[i].type", padMgr->padStatus[i].type);
                //  "An unknown type of controller is connected"
                // osSyncPrintf("知らない種類のコントローラが接続されています\n");
            }
        }
    }
    padMgr->validCtrlrsMask = mask;

    /* if (gFaultStruct.msgId) {
        PadMgr_RumbleStop(padMgr);
    } else */
    if (padMgr->rumbleOffFrames > 0) {
        --padMgr->rumbleOffFrames;
        PadMgr_RumbleStop(padMgr);
    } else if (padMgr->rumbleOnFrames == 0) {
        PadMgr_RumbleStop(padMgr);
    } else if (!padMgr->preNMIShutdown) {
        PadMgr_RumbleControl(padMgr);
        --padMgr->rumbleOnFrames;
    }
}

void PadMgr_HandlePreNMI(PadMgr* padMgr) {
    osSyncPrintf("padmgr_HandlePreNMI()\n");
    padMgr->preNMIShutdown = true;
    PadMgr_RumbleReset(padMgr);
}

void PadMgr_RequestPadData(PadMgr* padMgr, Input* inputs, s32 mode) {
    s32 i;
    Input* ogInput;
    Input* newInput;
    s32 buttonDiff;

    PadMgr_LockPadData(padMgr);

    ogInput = &padMgr->inputs[0];
    newInput = &inputs[0];
    for (i = 0; i < 4; i++) {
        if (mode != 0) {
            *newInput = *ogInput;
            ogInput->press.button = 0;
            ogInput->press.stick_x = 0;
            ogInput->press.stick_y = 0;
            ogInput->rel.button = 0;
        } else {
            newInput->prev = newInput->cur;
            newInput->cur = ogInput->cur;
            buttonDiff = newInput->prev.button ^ newInput->cur.button;
            newInput->press.button = newInput->cur.button & buttonDiff;
            newInput->rel.button = newInput->prev.button & buttonDiff;
            PadUtils_UpdateRelXY(newInput);
            newInput->press.stick_x += (s8)(newInput->cur.stick_x - newInput->prev.stick_x);
            newInput->press.stick_y += (s8)(newInput->cur.stick_y - newInput->prev.stick_y);
            // #region SOH [Enhancement]
            PadUtils_UpdateRelRXY(newInput);
            newInput->press.right_stick_x += (s8)(newInput->cur.right_stick_x - newInput->prev.right_stick_x);
            newInput->press.right_stick_y += (s8)(newInput->cur.right_stick_y - newInput->prev.right_stick_y);
            // #endregion
        }
        ogInput++;
        newInput++;
    }

    PadMgr_UnlockPadData(padMgr);
}

void PadMgr_ThreadEntry(PadMgr* padMgr) {
    s16* mesg = NULL;
    s32 exit;

    // osSyncPrintf("コントローラスレッド実行開始\n"); // "Controller thread execution start"

    exit = false;
    while (!exit) {
        if ((D_8012D280 > 2) && (padMgr->interruptMsgQ.validCount == 0)) {
            // "Waiting for controller thread event"
            osSyncPrintf("コントローラスレッドイベント待ち %lld\n", OS_CYCLES_TO_USEC(osGetTime()));
        }

        osRecvMesg(&padMgr->interruptMsgQ, (OSMesg*)&mesg, OS_MESG_BLOCK);
        // LOG_CHECK_NULL_POINTER("msg", mesg);

        PadMgr_HandleRetraceMsg(padMgr);
        break;

#if 0
        switch (*mesg) {
            case OS_SC_RETRACE_MSG:
                if (D_8012D280 > 2) {
                    osSyncPrintf("padmgr_HandleRetraceMsg START %lld\n", OS_CYCLES_TO_USEC(osGetTime()));
                }

                PadMgr_HandleRetraceMsg(padMgr);

                if (D_8012D280 > 2) {
                    osSyncPrintf("padmgr_HandleRetraceMsg END   %lld\n", OS_CYCLES_TO_USEC(osGetTime()));
                }

                break;
            case OS_SC_PRE_NMI_MSG:
                PadMgr_HandlePreNMI(padMgr);
                break;
            case OS_SC_NMI_MSG:
                exit = true;
                break;
        }
#endif
    }

    // OTRTODO: Removed due to crash
    // IrqMgr_RemoveClient(padMgr->irqMgr, &padMgr->irqClient);

    // osSyncPrintf("コントローラスレッド実行終了\n"); // "Controller thread execution end"
}

void PadMgr_Init(PadMgr* padMgr, OSMesgQueue* siIntMsgQ, IrqMgr* irqMgr, OSId id, OSPri priority, void* stack) {
    osSyncPrintf("パッドマネージャ作成 padmgr_Create()\n"); // "Pad Manager creation"

    memset(padMgr, 0, sizeof(PadMgr));
    padMgr->irqMgr = irqMgr;

    osCreateMesgQueue(&padMgr->interruptMsgQ, padMgr->interruptMsgBuf, 4);
    // OTRTODO: Removed due to crash
    // IrqMgr_AddClient(padMgr->irqMgr, &padMgr->irqClient, &padMgr->interruptMsgQ);
    osCreateMesgQueue(&padMgr->serialMsgQ, padMgr->serialMsgBuf, 1);
    PadMgr_UnlockSerialMesgQueue(padMgr, siIntMsgQ);
    osCreateMesgQueue(&padMgr->lockMsgQ, padMgr->lockMsgBuf, 1);
    PadMgr_UnlockPadData(padMgr);
    PadSetup_Init(siIntMsgQ, (u8*)&padMgr->validCtrlrsMask, padMgr->padStatus);

    padMgr->nControllers = 4;
    osContSetCh(padMgr->nControllers);

    osCreateThread(&padMgr->thread, id, (void (*)(void*))PadMgr_ThreadEntry, padMgr, stack, priority);
    osStartThread(&padMgr->thread);
}
