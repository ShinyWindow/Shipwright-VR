#include "SohMenu.h"
#include "SohGui.hpp"
#include <libultraship/bridge/consolevariablebridge.h>
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vr_interface.h>
#include <fast/vr_openxr.h>
#include <fast/vr_hud_settings.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <ship/Context.h>
#include <ship/window/gui/Gui.h>
#include "soh/Enhancements/vr-combat/VrCombat.h"
#include "soh/Enhancements/vr-combat/VrCutsceneView.h"

namespace SohGui {

extern std::shared_ptr<SohMenu> mSohMenu;
using namespace UIWidgets;

static const std::map<int32_t, const char*> vrMirrorAxisOptions = {
    { 0, "X (finger axis)" },
    { 1, "Y" },
    { 2, "Z (thumb axis)" },
};

static const std::map<int32_t, const char*> vrViewModeOptions = {
    { 0, "Third Person" },
    { 1, "First Person" },
};

static const std::map<int32_t, const char*> vrTurnStyleOptions = {
    { 0, "Snap" },
    { 1, "Smooth" },
};

static const std::map<int32_t, const char*> vrHudLayoutOptions = {
    { 0, "Wrist Panels" },
    { 1, "Classic (one panel)" },
};

// Open state of the collapsed "Calibration (Dev)" groups (closed every launch).
static bool sVrComfortCalOpen = false;
static bool sVrCombatCalOpen = false;

static bool VrHudWristLayout() {
    return CVarGetInteger("gVrHudLayout", 0) == 0;
}

static const std::map<int32_t, const char*> vrItemSelHandOptions = {
    { 0, "Sword Hand" },
    { 1, "Off Hand" },
};

static const std::map<int32_t, const char*> vrItemSelInputOptions = {
    { VR_BTN_TRIGGER, "Trigger" },     { VR_BTN_GRIP, "Grip" },
    { VR_BTN_PRIMARY, "A / X" },       { VR_BTN_SECONDARY, "B / Y" },
    { VR_BTN_THUMBCLICK, "Stick Click" }, { VR_BTN_MENU, "Menu Button" },
};

// Sword-swap chord choices: no Trigger entry (both triggers are reserved for using the held
// item in selector mode), and 0 turns the chord off.
static const std::map<int32_t, const char*> vrItemSelSwapOptions = {
    { 0, "Disabled" },
    { VR_BTN_GRIP, "Both Grips" },
    { VR_BTN_PRIMARY, "A + X" },
    { VR_BTN_SECONDARY, "B + Y" },
    { VR_BTN_THUMBCLICK, "Both Stick Clicks" },
    { VR_BTN_MENU, "Both Menu Buttons" },
};

// --- VR Inputs: N64-button-first binding editor (styled after the base game's bindings window:
// colored N64 chip per row, removable chips for each bound VR input, "+" to add). All state lives
// in the gVrBind* mask CVars that padmgr.c reads.
//
// There are THREE independent binding sets. The two GAMEPLAY sets are picked by gVrItemSelect,
// so swapping control schemes in the menu never costs you your bindings; selector mode reserves
// both TRIGGERS for using the held item, so their rows have no CVar there (nullptr; every access
// is behind VrInputReserved). The OCARINA set takes
// over the controllers whenever the ocarina interface is up (in either scheme) — it maps notes,
// sharps/flats and put-away, and because every selector reservation stands down while playing,
// notes may live on any input including the triggers. Defaults must match
// sVrBindDefaults / sVrBindSelDefaults / sVrBindOcaDefaults in padmgr.c.
struct VrInputDef {
    const char* label;
    const char* cvar;
    int32_t defaultMask;
};
static const VrInputDef sVrInputDefsClassic[] = {
    { "L Trigger", "gVrBindLTrigger", BTN_Z },      { "L Grip", "gVrBindLGrip", BTN_R },
    { "X", "gVrBindLPrimary", BTN_CLEFT },          { "Y", "gVrBindLSecondary", BTN_CRIGHT },
    { "L Stick", "gVrBindLStickClick", BTN_START }, { "L Menu", "gVrBindLMenu", 0 },
    { "R Trigger", "gVrBindRTrigger", BTN_B },      { "R Grip", "gVrBindRGrip", 0 },
    { "A", "gVrBindRPrimary", BTN_A },              { "B", "gVrBindRSecondary", BTN_CDOWN },
    { "R Stick", "gVrBindRStickClick", 0 },         { "R Menu", "gVrBindRMenu", 0 },
};
static const VrInputDef sVrInputDefsSelector[] = {
    { "L Trigger", nullptr, 0 },                           { "L Grip", "gVrBindSelLGrip", BTN_R },
    { "X", "gVrBindSelLPrimary", 0 },                      { "Y", "gVrBindSelLSecondary", 0 },
    { "L Stick", "gVrBindSelLStickClick", BTN_START },     { "L Menu", "gVrBindSelLMenu", BTN_START },
    { "R Trigger", nullptr, 0 },                           { "R Grip", "gVrBindSelRGrip", BTN_Z },
    { "A", "gVrBindSelRPrimary", BTN_A },                  { "B", "gVrBindSelRSecondary", BTN_B },
    { "R Stick", "gVrBindSelRStickClick", 0 },             { "R Menu", "gVrBindSelRMenu", 0 },
};
static const VrInputDef sVrInputDefsOcarina[] = {
    { "L Trigger", "gVrBindOcaLTrigger", 0 },         { "L Grip", "gVrBindOcaLGrip", BTN_Z },
    { "X", "gVrBindOcaLPrimary", 0 },                 { "Y", "gVrBindOcaLSecondary", 0 },
    { "L Stick", "gVrBindOcaLStickClick", 0 },        { "L Menu", "gVrBindOcaLMenu", 0 },
    { "R Trigger", "gVrBindOcaRTrigger", 0 },         { "R Grip", "gVrBindOcaRGrip", BTN_R },
    { "A", "gVrBindOcaRPrimary", BTN_A },             { "B", "gVrBindOcaRSecondary", BTN_B },
    { "R Stick", "gVrBindOcaRStickClick", 0 },        { "R Menu", "gVrBindOcaRMenu", 0 },
    // Stick DIRECTIONS, bindable in the ocarina set only (indices 12+, order up/down/left/right
    // per hand — the listener below computes 12 + hand * 4 + dir). A hand with any direction
    // bound claims that whole stick while playing: its stock job (left: pitch bend; right:
    // third-person C-stick) stands down. Matches sVrBindOcaStickCvars in padmgr.c.
    { "L Stick " ICON_FA_ARROW_UP, "gVrBindOcaLStickUp", 0 },
    { "L Stick " ICON_FA_ARROW_DOWN, "gVrBindOcaLStickDown", 0 },
    { "L Stick " ICON_FA_ARROW_LEFT, "gVrBindOcaLStickLeft", 0 },
    { "L Stick " ICON_FA_ARROW_RIGHT, "gVrBindOcaLStickRight", 0 },
    { "R Stick " ICON_FA_ARROW_UP, "gVrBindOcaRStickUp", BTN_CUP },
    { "R Stick " ICON_FA_ARROW_DOWN, "gVrBindOcaRStickDown", BTN_CDOWN },
    { "R Stick " ICON_FA_ARROW_LEFT, "gVrBindOcaRStickLeft", BTN_CLEFT },
    { "R Stick " ICON_FA_ARROW_RIGHT, "gVrBindOcaRStickRight", BTN_CRIGHT },
};
static const int kVrButtonInputCount = 12;
static const int kVrOcarinaInputCount = 20; // buttons + the 8 stick directions

// Which set the editor is showing: the active gameplay scheme, or the ocarina set.
static bool sVrEditOcarina = false;

static bool VrSelectorProfile() {
    return CVarGetInteger("gVrItemSelect", 1) != 0;
}
static const VrInputDef* VrInputDefs() {
    if (sVrEditOcarina) {
        return sVrInputDefsOcarina;
    }
    return VrSelectorProfile() ? sVrInputDefsSelector : sVrInputDefsClassic;
}
static int VrInputCount() {
    return sVrEditOcarina ? kVrOcarinaInputCount : kVrButtonInputCount;
}
// Dominant-axis stick direction: 0 up, 1 down, 2 left, 3 right, -1 centered. Must match the
// firing logic in padmgr.c so what binds here is what plays there.
static int VrStickDir(float x, float y) {
    if ((y * y) >= (x * x)) {
        return (y > 0.5f) ? 0 : (y < -0.5f) ? 1 : -1;
    }
    return (x < -0.5f) ? 2 : (x > 0.5f) ? 3 : -1;
}
// Indices 0 and 6 are the two triggers: reserved by selector mode, so they are not bindable —
// except in the ocarina set, where the reservations don't apply and triggers are prime note real
// estate.
static bool VrInputReserved(int idx) {
    return !sVrEditOcarina && VrSelectorProfile() && ((idx == 0) || (idx == 6));
}

struct VrN64RowDef {
    const char* label;
    uint16_t mask;
    ImVec4 color;
};

// Which N64 button row is currently listening for a physical VR press (0 = none), and the
// previous frame's controller state per hand for rising-edge detection (so a button already held
// when listening starts doesn't instantly bind).
static uint16_t sVrListenRowMask = 0;
static uint16_t sVrListenPrevBtn[2] = { 0, 0 };
// Previous frame's stick direction per hand (VrStickDir result), for the same rising-edge rule:
// a stick already deflected when listening starts must not instantly bind.
static int sVrListenPrevStickDir[2] = { -1, -1 };

static void VrInputBindingRow(const VrN64RowDef& row) {
    ImGui::PushID(row.label);

    // The N64 button chip (colored, inert — it's a label).
    ImGui::PushStyleColor(ImGuiCol_Button, row.color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, row.color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, row.color);
    ImGui::Button(row.label, ImVec2(96.0f, 0.0f)); // wide enough for the ocarina note labels
    ImGui::PopStyleColor(3);

    // One removable chip per VR input currently bound to this button.
    for (int i = 0; i < VrInputCount(); i++) {
        const VrInputDef& input = VrInputDefs()[i];
        int32_t cur = VrInputReserved(i) ? 0 : CVarGetInteger(input.cvar, input.defaultMask);
        if (cur & row.mask) {
            ImGui::SameLine();
            ImGui::PushID(input.cvar);
            char chip[48];
            snprintf(chip, sizeof(chip), "%s %s x", ICON_FA_GAMEPAD, input.label);
            if (ImGui::SmallButton(chip)) {
                CVarSetInteger(input.cvar, cur & ~row.mask);
                CVarSave();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove this binding");
            }
            ImGui::PopID();
        }
    }

    ImGui::SameLine();
    const bool listening = (sVrListenRowMask == row.mask);
    if (listening) {
        // Listening: press any input on either VR controller to bind it to this row.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.48f, 0.78f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.55f, 0.88f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.30f, 0.55f, 1.0f));
        if (ImGui::SmallButton("Press a VR input... (click or Esc to cancel)")) {
            sVrListenRowMask = 0;
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            sVrListenRowMask = 0;
        }
        static const uint16_t sVrBtnBits[6] = { VR_BTN_TRIGGER,   VR_BTN_GRIP,       VR_BTN_PRIMARY,
                                                VR_BTN_SECONDARY, VR_BTN_THUMBCLICK, VR_BTN_MENU };
        for (int hand = 0; hand < 2 && sVrListenRowMask != 0; hand++) {
            uint16_t curBtn = VR_GetControllerButton(hand);
            uint16_t pressed = curBtn & ~sVrListenPrevBtn[hand];
            sVrListenPrevBtn[hand] = curBtn;
            for (int b = 0; b < 6; b++) {
                if (pressed & sVrBtnBits[b]) {
                    const int idx = hand * 6 + b;
                    if (VrInputReserved(idx)) {
                        continue; // reserved for using the held item — keep listening
                    }
                    const VrInputDef& input = VrInputDefs()[idx];
                    CVarSetInteger(input.cvar, CVarGetInteger(input.cvar, input.defaultMask) | row.mask);
                    CVarSave();
                    sVrListenRowMask = 0;
                    break;
                }
            }
        }
        // Stick directions are bindable in the ocarina set only: a fresh deflection past the
        // threshold binds this row to that direction.
        for (int hand = 0; sVrEditOcarina && hand < 2 && sVrListenRowMask != 0; hand++) {
            float sx = 0.0f, sy = 0.0f;
            VR_GetThumbstick(hand, &sx, &sy);
            const int dir = VrStickDir(sx, sy);
            const bool fresh = (dir >= 0) && (dir != sVrListenPrevStickDir[hand]);
            sVrListenPrevStickDir[hand] = dir;
            if (fresh) {
                const VrInputDef& input = VrInputDefs()[12 + hand * 4 + dir];
                CVarSetInteger(input.cvar, CVarGetInteger(input.cvar, input.defaultMask) | row.mask);
                CVarSave();
                sVrListenRowMask = 0;
            }
        }
    } else {
        // Blue "+": in VR, listen for a physical press; outside VR, fall back to a picker list.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.38f, 0.65f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.48f, 0.78f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.30f, 0.55f, 1.0f));
        if (ImGui::SmallButton("+")) {
            if (VR_IsInitialized()) {
                sVrListenRowMask = row.mask;
                for (int hand = 0; hand < 2; hand++) {
                    sVrListenPrevBtn[hand] = VR_GetControllerButton(hand);
                    float sx = 0.0f, sy = 0.0f;
                    VR_GetThumbstick(hand, &sx, &sy);
                    sVrListenPrevStickDir[hand] = VrStickDir(sx, sy);
                }
            } else {
                ImGui::OpenPopup("VrAddBinding");
            }
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(VR_IsInitialized() ? "Then press the VR controller input to bind"
                                                 : "Pick a VR input to bind (VR not active)");
        }
        if (ImGui::BeginPopup("VrAddBinding")) {
            for (int i = 0; i < VrInputCount(); i++) {
                if (VrInputReserved(i)) {
                    continue;
                }
                const VrInputDef& input = VrInputDefs()[i];
                int32_t cur = CVarGetInteger(input.cvar, input.defaultMask);
                if (!(cur & row.mask)) {
                    if (ImGui::MenuItem(input.label)) {
                        CVarSetInteger(input.cvar, cur | row.mask);
                        CVarSave();
                    }
                }
            }
            ImGui::EndPopup();
        }
    }

    ImGui::PopID();
}

static void VrInputBindings(WidgetInfo& info) {
    static const VrN64RowDef sButtonRows[] = {
        { "A", BTN_A, ImVec4(0.22f, 0.24f, 0.50f, 1.0f) },
        { "B", BTN_B, ImVec4(0.12f, 0.35f, 0.14f, 1.0f) },
        { "Start", BTN_START, ImVec4(0.48f, 0.14f, 0.14f, 1.0f) },
        { "L", BTN_L, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
        { "R", BTN_R, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
        { "Z", BTN_Z, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
        { "C " ICON_FA_ARROW_UP, BTN_CUP, ImVec4(0.60f, 0.44f, 0.06f, 1.0f) },
        { "C " ICON_FA_ARROW_DOWN, BTN_CDOWN, ImVec4(0.60f, 0.44f, 0.06f, 1.0f) },
        { "C " ICON_FA_ARROW_LEFT, BTN_CLEFT, ImVec4(0.60f, 0.44f, 0.06f, 1.0f) },
        { "C " ICON_FA_ARROW_RIGHT, BTN_CRIGHT, ImVec4(0.60f, 0.44f, 0.06f, 1.0f) },
    };
    static const VrN64RowDef sDpadRows[] = {
        { "D " ICON_FA_ARROW_UP, BTN_DUP, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
        { "D " ICON_FA_ARROW_DOWN, BTN_DDOWN, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
        { "D " ICON_FA_ARROW_LEFT, BTN_DLEFT, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
        { "D " ICON_FA_ARROW_RIGHT, BTN_DRIGHT, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
    };
    // The ocarina set is edited in NOTES, not N64 buttons: each row is the note (with the N64
    // button it stands for), plus the sharp/flat modifiers, put-away, and the free-play guard
    // (hold L: songs are not recognized, so nothing interrupts noodling).
    static const VrN64RowDef sOcarinaRows[] = {
        { "D4 (A)", BTN_A, ImVec4(0.22f, 0.24f, 0.50f, 1.0f) },
        { "F4 (C" ICON_FA_ARROW_DOWN ")", BTN_CDOWN, ImVec4(0.60f, 0.44f, 0.06f, 1.0f) },
        { "A4 (C" ICON_FA_ARROW_RIGHT ")", BTN_CRIGHT, ImVec4(0.60f, 0.44f, 0.06f, 1.0f) },
        { "B4 (C" ICON_FA_ARROW_LEFT ")", BTN_CLEFT, ImVec4(0.60f, 0.44f, 0.06f, 1.0f) },
        { "D5 (C" ICON_FA_ARROW_UP ")", BTN_CUP, ImVec4(0.60f, 0.44f, 0.06f, 1.0f) },
        { "Sharp (R)", BTN_R, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
        { "Flat (Z)", BTN_Z, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
        { "Put Away", BTN_B, ImVec4(0.12f, 0.35f, 0.14f, 1.0f) },
        { "No Song (L)", BTN_L, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
    };

    // Which binding set is being edited. The gameplay tab follows the active scheme
    // (gVrItemSelect), so flipping the selector toggle swaps schemes with bindings intact; the
    // ocarina set is its own thing, in force whenever the ocarina interface is up.
    if (ImGui::RadioButton(VrSelectorProfile() ? "Gameplay (Item Selector)" : "Gameplay (Classic)", !sVrEditOcarina)) {
        sVrEditOcarina = false;
        sVrListenRowMask = 0;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Ocarina", sVrEditOcarina)) {
        sVrEditOcarina = true;
        sVrListenRowMask = 0;
    }
    ImGui::Separator();

    if (sVrEditOcarina) {
        ImGui::TextWrapped("Editing the OCARINA binding set, used whenever the ocarina is out — in "
                           "either control scheme. Notes run low to high: D4, F4, A4, B4, D5. The "
                           "left thumbstick bends pitch, as the analog stick always did. Triggers "
                           "are NOT reserved here: while playing, every input belongs to the "
                           "ocarina. Thumbstick DIRECTIONS are bindable too (flick the stick while "
                           "a row is listening); binding any direction on a stick gives that whole "
                           "stick to notes while playing — left stick loses pitch bend, right "
                           "stick loses its C-stick role.");
        ImGui::Separator();
        for (const VrN64RowDef& row : sOcarinaRows) {
            VrInputBindingRow(row);
        }
        return;
    }
    if (VrSelectorProfile()) {
        ImGui::TextWrapped("Editing the ITEM SELECTOR binding set. Both triggers are reserved: the "
                           "trigger of the hand holding an item uses that item. C buttons no "
                           "longer pull items out — the selector does that — so they are free.");
    } else {
        ImGui::TextWrapped("Editing the CLASSIC binding set: items are used by pressing the C "
                           "button you assigned them to in the inventory.");
    }
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Buttons##VrInputs", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const VrN64RowDef& row : sButtonRows) {
            VrInputBindingRow(row);
        }
    }
    if (ImGui::CollapsingHeader("D-Pad##VrInputs", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const VrN64RowDef& row : sDpadRows) {
            VrInputBindingRow(row);
        }
    }
}

// Live frame-cost breakdown. The interesting number is "XR wait": that is time spent blocked in
// xrWaitFrame, i.e. spare headroom. When it trends toward zero the frame no longer fits and the
// compositor starts reprojecting.
static void VrPerformanceReadout(WidgetInfo& info) {
    if (!VR_IsInitialized()) {
        ImGui::TextUnformatted("Not in VR.");
        return;
    }

    VrFrameStats s = {};
    vr_get_frame_stats(&s);

    ImGui::Text("XR frames submitted   %6.1f Hz", s.frame_hz);
    ImGui::Text("Stereo pairs rendered %6.1f Hz", s.eye_hz);
    ImGui::Separator();
    ImGui::Text("XR wait (headroom)  %6.2f ms", s.wait_ms);
    ImGui::Text("Both eye passes     %6.2f ms", s.eyes_ms);
    ImGui::Text("HUD quad pass       %6.2f ms", s.hud_ms);
    ImGui::Text("Companion window    %6.2f ms", s.desktop_ms);
    ImGui::Text("Whole frame         %6.2f ms", s.frame_ms);
    ImGui::Separator();
    ImGui::Text("Game logic tick     %6.2f ms", s.tick_ms);
    ImGui::TextUnformatted("(game logic runs once per 20 Hz tick, on this\n"
                           "same thread, so it comes out of the render budget)");
}

// Live hand-speed readout for tuning physical combat: current speed plus a slowly-bleeding peak
// per hand, in physical meters/second — the unit every swing threshold is tuned in, independent
// of world scale and Link's age. Runs at menu (render) rate, so it shows every XR frame's sample.
static void VrPhysCombatReadout(WidgetInfo& info) {
    if (!VR_IsInitialized()) {
        ImGui::TextUnformatted("Not in VR.");
        return;
    }
    static float sPeak[2] = { 0.0f, 0.0f };
    const float dt = ImGui::GetIO().DeltaTime;
    static const char* sNames[2] = { "Left ", "Right" };
    for (int hand = 0; hand < 2; hand++) {
        float lin[3];
        float ang[3];
        float speed = 0.0f;
        if (VR_GetHandVelocity(hand, lin, ang)) {
            speed = sqrtf(lin[0] * lin[0] + lin[1] * lin[1] + lin[2] * lin[2]);
        }
        sPeak[hand] = fmaxf(speed, sPeak[hand] - 2.0f * dt); // bleed 2 m/s per second
        ImGui::Text("%s hand  %5.2f m/s   peak %5.2f m/s", sNames[hand], speed, sPeak[hand]);
    }
    ImGui::TextUnformatted("Swing a controller and watch the numbers move.");
}

// ---------------------------------------------------------------- Wrist HUD editor
// Every wrist HUD setting (fast/vr_hud_settings.h: names + defaults shared with the renderer),
// per Adult / Child profile. Values save as they change; Ctrl+click a slider to type a value past
// its range. "Copy All Settings" puts every value of both profiles on the clipboard, ready to send
// as the new defaults.

struct VrHudPanelField {
    const char* field;
    const char* label;
    float min, max;
    const char* fmt;
    float VrHudPanelDefaults::*member;
    const char* tooltip;
};

static const VrHudPanelField kVrHudPanelFields[] = {
    { "X", "Sideways", -60.0f, 60.0f, "%.1f cm", &VrHudPanelDefaults::x, "Along the controller's right axis." },
    { "Y", "Up", -60.0f, 60.0f, "%.1f cm", &VrHudPanelDefaults::y, "Along the controller's up axis." },
    { "Z", "Forward / Back", -60.0f, 60.0f, "%.1f cm", &VrHudPanelDefaults::z,
      "Negative = toward the controller's front (the way it points)." },
    { "Pitch", "Tilt", -180.0f, 180.0f, "%.0f deg", &VrHudPanelDefaults::pitch,
      "Tips the panel toward / away from your eyes." },
    { "Yaw", "Turn", -180.0f, 180.0f, "%.0f deg", &VrHudPanelDefaults::yaw, "Turns the panel left / right." },
    { "Roll", "Roll", -180.0f, 180.0f, "%.0f deg", &VrHudPanelDefaults::roll, "Spins the panel about its face." },
    { "Scale", "Size", 10.0f, 1000.0f, "%.0f%%", &VrHudPanelDefaults::scale,
      "Real size of the whole panel. 100% = a heart about 8 mm across." },
    { "Width", "Card Width", 0.0f, 320.0f, "%.0f", &VrHudPanelDefaults::width,
      "Width of the dark card in HUD units (a heart is ~10). 0 = fit the elements." },
    { "Height", "Card Height", 0.0f, 480.0f, "%.0f", &VrHudPanelDefaults::height,
      "Height of the dark card in HUD units. 0 = fit the elements." },
    { "Padding", "Card Padding", 0.0f, 60.0f, "%.0f", &VrHudPanelDefaults::padding,
      "Space between the card edge and the elements (HUD units)." },
    { "Backing", "Card Opacity", 0.0f, 100.0f, "%.0f%%", &VrHudPanelDefaults::backing,
      "Opacity of the dark card behind the panel." },
};

struct VrHudElementField {
    const char* field;
    const char* label;
    float min, max;
    const char* fmt;
};

static const VrHudElementField kVrHudElementFields[] = {
    { "X", "Move Left / Right", -320.0f, 320.0f, "%.0f" },
    { "Y", "Move Up / Down", -480.0f, 480.0f, "%.0f" },
    { "Scale", "Size", 10.0f, 500.0f, "%.0f%%" },
};

static float VrHudElementDefault(const VrHudElementDesc& d, int child, const char* field) {
    if (strcmp(field, "X") == 0) {
        return d.x[child];
    }
    if (strcmp(field, "Y") == 0) {
        return d.y[child];
    }
    return d.scale[child];
}

static void VrHudSaveSoon() {
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

static void VrHudSliderRow(const char* label, const char* cvar, float def, float mn, float mx, const char* fmt,
                           const char* tooltip) {
    ImGui::PushID(cvar);
    float v = CVarGetFloat(cvar, def);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    if (ImGui::SliderFloat(label, &v, mn, mx, fmt)) {
        CVarSetFloat(cvar, v);
        VrHudSaveSoon();
    }
    if (tooltip != nullptr && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s\n(Ctrl+click to type any value.)", tooltip);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) {
        CVarClear(cvar);
        VrHudSaveSoon();
    }
    ImGui::PopID();
}

// Copy or reset a whole profile. from < 0 = reset `to` to the defaults.
static void VrHudProfileCopy(int from, int to) {
    char src[96], dst[96];
    for (int hand = 0; hand < 2; hand++) {
        for (const auto& f : kVrHudPanelFields) {
            VrHudPanelCVar(dst, sizeof(dst), to, hand, f.field);
            if (from < 0) {
                CVarClear(dst);
            } else {
                VrHudPanelCVar(src, sizeof(src), from, hand, f.field);
                CVarSetFloat(dst, CVarGetFloat(src, kVrHudPanelDefaults[from][hand].*(f.member)));
            }
        }
    }
    for (int i = 0; i < kVrHudElementCount; i++) {
        const VrHudElementDesc& d = kVrHudElements[i];
        for (const auto& f : kVrHudElementFields) {
            VrHudElementCVar(dst, sizeof(dst), to, d.key, f.field);
            if (from < 0) {
                CVarClear(dst);
            } else {
                VrHudElementCVar(src, sizeof(src), from, d.key, f.field);
                CVarSetFloat(dst, CVarGetFloat(src, VrHudElementDefault(d, from, f.field)));
            }
        }
        VrHudElementCVar(dst, sizeof(dst), to, d.key, "Show");
        if (from < 0) {
            CVarClear(dst);
        } else {
            VrHudElementCVar(src, sizeof(src), from, d.key, "Show");
            CVarSetInteger(dst, CVarGetInteger(src, d.show[from]));
        }
    }
    VrHudSaveSoon();
}

static std::string VrHudExportText() {
    std::string out = "Shipwright-VR wrist HUD settings\n";
    char name[96], line[160];
    for (int child = 0; child < 2; child++) {
        for (int hand = 0; hand < 2; hand++) {
            for (const auto& f : kVrHudPanelFields) {
                VrHudPanelCVar(name, sizeof(name), child, hand, f.field);
                snprintf(line, sizeof(line), "%s=%.2f\n", name,
                         CVarGetFloat(name, kVrHudPanelDefaults[child][hand].*(f.member)));
                out += line;
            }
        }
        for (int i = 0; i < kVrHudElementCount; i++) {
            const VrHudElementDesc& d = kVrHudElements[i];
            for (const auto& f : kVrHudElementFields) {
                VrHudElementCVar(name, sizeof(name), child, d.key, f.field);
                snprintf(line, sizeof(line), "%s=%.2f\n", name, CVarGetFloat(name, VrHudElementDefault(d, child, f.field)));
                out += line;
            }
            VrHudElementCVar(name, sizeof(name), child, d.key, "Show");
            snprintf(line, sizeof(line), "%s=%d\n", name, CVarGetInteger(name, d.show[child]));
            out += line;
        }
    }
    return out;
}

static void VrHudProfileEditor(int child) {
    char name[96];
    if (ImGui::Button(child ? "Copy Adult settings into Child" : "Copy Child settings into Adult")) {
        VrHudProfileCopy(child ? 0 : 1, child);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset this profile to defaults")) {
        VrHudProfileCopy(-1, child);
    }

    for (int hand = 0; hand < 2; hand++) {
        ImGui::PushID(hand);
        if (ImGui::CollapsingHeader(hand ? "Right Hand Panel (buttons + minimap)" : "Left Hand Panel (vitals)",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto& f : kVrHudPanelFields) {
                VrHudPanelCVar(name, sizeof(name), child, hand, f.field);
                VrHudSliderRow(f.label, name, kVrHudPanelDefaults[child][hand].*(f.member), f.min, f.max, f.fmt, f.tooltip);
            }
        }
        if (ImGui::CollapsingHeader(hand ? "Right Hand Elements" : "Left Hand Elements")) {
            ImGui::TextWrapped("Each element starts in its automatic spot on the panel; these move and size "
                               "it from there (HUD units: a heart is ~10).");
            for (int i = 0; i < kVrHudElementCount; i++) {
                const VrHudElementDesc& d = kVrHudElements[i];
                if (d.hand != hand) {
                    continue;
                }
                ImGui::PushID(d.key);
                if (ImGui::TreeNode(d.label)) {
                    VrHudElementCVar(name, sizeof(name), child, d.key, "Show");
                    bool show = CVarGetInteger(name, d.show[child]) != 0;
                    if (ImGui::Checkbox("Show", &show)) {
                        CVarSetInteger(name, show ? 1 : 0);
                        VrHudSaveSoon();
                    }
                    for (const auto& f : kVrHudElementFields) {
                        VrHudElementCVar(name, sizeof(name), child, d.key, f.field);
                        VrHudSliderRow(f.label, name, VrHudElementDefault(d, child, f.field), f.min, f.max, f.fmt, nullptr);
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }
        ImGui::PopID();
    }
}

static void VrWristHudEditor(WidgetInfo& info) {
    const bool child_now = vr_get_hud_child();
    ImGui::TextWrapped("Wrist HUD layout. Link's current age uses the %s profile. Edit either profile below; "
                       "use Preview to see the other one without changing age.",
                       child_now ? "Child" : "Adult");
    if (CVarGetInteger("gVrHudLayout", 0) != 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                           "HUD & Menus -> HUD Layout is set to Classic: these settings apply to Wrist Panels.");
    }

    static const char* kPreview[] = { "Auto (Link's age)", "Adult", "Child" };
    int preview = std::clamp(CVarGetInteger("gVrHud.Preview", 0), 0, 2);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    if (ImGui::Combo("Preview Profile", &preview, kPreview, 3)) {
        CVarSetInteger("gVrHud.Preview", preview);
        VrHudSaveSoon();
    }

    bool grabEdit = CVarGetInteger("gVrHud.GrabEdit", 1) != 0;
    if (ImGui::Checkbox("Edit by grabbing (in the headset)", &grabEdit)) {
        CVarSetInteger("gVrHud.GrabEdit", grabEdit ? 1 : 0);
        VrHudSaveSoon();
    }
    ImGui::TextWrapped("Bring one hand to the other wrist's panel (it tints blue when in reach), then:\n"
                       "  A / X + grip: grab the whole panel. It sticks to your hand; move and turn it, "
                       "let go of grip to drop it there.\n"
                       "  B / Y + grip: grab the HUD element nearest your hand and slide it across the panel.\n"
                       "Edits go into the profile in use (Link's age, or Preview). The sliders below follow "
                       "along, so Copy All Settings afterwards to send them.");

    if (ImGui::Button("Copy All Settings to Clipboard")) {
        ImGui::SetClipboardText(VrHudExportText().c_str());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Both profiles, every value. Paste it to Claude to make them the defaults.");
    }

    if (ImGui::BeginTabBar("VrWristHudProfiles")) {
        static bool sFirst = true;
        const ImGuiTabItemFlags adultFlags = (sFirst && !child_now) ? ImGuiTabItemFlags_SetSelected : 0;
        const ImGuiTabItemFlags childFlags = (sFirst && child_now) ? ImGuiTabItemFlags_SetSelected : 0;
        sFirst = false;
        if (ImGui::BeginTabItem("Adult Link", nullptr, adultFlags)) {
            VrHudProfileEditor(0);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Child Link", nullptr, childFlags)) {
            VrHudProfileEditor(1);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// Physics flight recorder. The checkbox arms a ring buffer holding the most recent ~20 s of sim
// steps; switching it off writes the capture to CSV next to the executable. Recording keeps the
// LAST window rather than the first, so the workflow is: enable, go reproduce the problem, then
// disable — whatever just happened is in the file.
static void VrPhysLogControl(WidgetInfo& info) {
    static bool sWasLogging = false;
    static char sStatus[512] = "";

    const bool wantLog = CVarGetInteger("gVrPhysLog", 0) != 0;
    if (wantLog != sWasLogging) {
        sWasLogging = wantLog;
        if (wantLog) {
            VR_PhysLogSetEnabled(true);
            snprintf(sStatus, sizeof(sStatus), "Recording...");
        } else {
            VR_PhysLogSetEnabled(false);
            const char* path = "vr_phys_log.csv";
            const int32_t n = VR_PhysLogWrite(path);
            if (n > 0) {
                char abs[MAX_PATH] = "";
                if (_fullpath(abs, path, sizeof(abs)) == nullptr) {
                    snprintf(abs, sizeof(abs), "%s", path);
                }
                snprintf(sStatus, sizeof(sStatus), "Wrote %d samples to:\n%s", n, abs);
            } else if (n == 0) {
                snprintf(sStatus, sizeof(sStatus), "Nothing captured (was the sword in hand, "
                                                   "with Physical Combat + blade inertia on?)");
            } else {
                snprintf(sStatus, sizeof(sStatus), "Could not open the log file for writing.");
            }
        }
    }

    if (wantLog) {
        ImGui::Text("Recording: %d samples buffered", VR_PhysLogCount());
        ImGui::TextUnformatted("Reproduce the problem, then turn this off to write the file.");
    } else if (sStatus[0] != '\0') {
        ImGui::TextUnformatted(sStatus);
    }
}

// Live state of the bottle pour / drink gestures: which gate (if any) is holding them, whether
// the game agrees the bottle is upside down or at the face, and how far the current shake has
// travelled. Every value is the 20 Hz tick's; the menu just re-reads it each frame. This is the
// in-headset answer to "I'm shaking / sipping and nothing happens": one of these lines will say why.
static void VrBottlePourReadout(WidgetInfo& info) {
    if (!VR_IsInitialized()) {
        ImGui::TextUnformatted("Not in VR.");
        return;
    }
    static const char* sGate[] = {
        "ARMED",
        "off (Physical Bottle Pouring and Drinking both unchecked)",
        "no bottle in hand / not in normal play",
        "contents neither pour (fish, bug, blue fire, fairy) nor drink (potions, milk, poe)",
        "paused, Link busy, or item change pending",
        "bottle hand not tracked",
        "off (this content's gesture is unchecked)",
    };
    VrBottleDebug d;
    VrBottle_GetDebug(&d);
    const int gate = (d.gate >= 0 && d.gate < 7) ? d.gate : 0;
    ImGui::Text("%s gesture: %s", d.kind ? "Drink" : "Pour", sGate[gate]);
    if (gate == 0 && !d.kind) {
        ImGui::Text("Upside down: %s   (axis %+.2f, must be below %+.2f)", d.inverted ? "YES" : "no", d.axisY,
                    d.invertThreshold);
        ImGui::Text("Shakes: %d / 3   this stroke %.1f cm   last shake %.1f cm", d.shakeCount, d.strokeCm,
                    d.lastStrokeCm);
    } else if (gate == 0) {
        ImGui::Text("At face: %s   (mouth is %.1f cm from the face, must be under %.1f)", d.atFace ? "YES" : "no",
                    d.faceCm, d.drinkDistanceCm);
        ImGui::Text("Sips: %d / 3", d.sipCount);
    }
    ImGui::Text("Last pour: %s", d.lastPourResult < 0 ? "none yet"
                                 : d.lastPourResult   ? "poured"
                                                      : "REFUSED (Link busy: cutscene, carrying, item cs)");
    ImGui::Text("Last drink: %s", d.lastDrinkResult < 0 ? "none yet"
                                  : d.lastDrinkResult   ? "drunk"
                                                        : "REFUSED (Link busy: cutscene, carrying, item cs)");
}

// Live state of the physical boomerang: pocket / hand / flight, which gate (if any) is holding
// it, the hand speed at the last release against the throw threshold, and on the way back how
// far it is from the catch hand and whether that hand's grip is closed. The in-headset answer
// to "I let go and nothing flew" or "it went past my hand".
static void VrBoomerangReadout(WidgetInfo& info) {
    if (!VR_IsInitialized()) {
        ImGui::TextUnformatted("Not in VR.");
        return;
    }
    static const char* sState[] = { "none", "POCKET", "IN HAND", "IN FLIGHT" };
    static const char* sGate[] = {
        "ARMED",
        "off (Physical Boomerang unchecked)",
        "boomerang not selected / not in normal play",
        "paused or Link busy (state kept)",
        "a boomerang not thrown by the hand is out (vanilla throw / restored flight)",
        "no pocket point (headset untracked)",
    };
    static const char* sHand[] = { "left", "right" };
    VrBoomerangDebug d;
    VrBoomerang_GetDebug(&d);
    const int state = (d.state >= 0 && d.state < 4) ? d.state : 0;
    const int gate = (d.gate >= 0 && d.gate < 6) ? d.gate : 0;
    ImGui::Text("Boomerang: %s   (%s)", sState[state], sGate[gate]);
    if (d.carryHand >= 0 && d.carryHand < 2) {
        ImGui::Text("In the %s hand — release still: back to the pocket; release moving: throw", sHand[d.carryHand]);
    }
    ImGui::Text("Last release: %.2f m/s (throws at %.2f)   %s", d.lastReleaseSpeed, d.throwMinSpeed,
                d.lastRelease < 0    ? "none yet"
                : d.lastRelease == 1 ? "THROWN"
                : d.lastRelease == 2 ? "throw REFUSED (Link busy / already out)"
                                     : "too slow, back to the pocket");
    if (state == 3) {
        ImGui::Text("%s   catch hand: %s   grip closed: %s", d.returnLeg ? "Coming back" : "Outbound",
                    (d.throwHand >= 0 && d.throwHand < 2) ? sHand[d.throwHand] : "?", d.catchArmed ? "YES" : "no");
        if (d.handDistanceCm >= 0.0f) {
            ImGui::Text("Boomerang -> catch hand: %.0f cm (arrives within %.0f)", d.handDistanceCm, d.catchRadiusCm);
        }
    }
    ImGui::Text("Last return: %s", d.lastReturn < 0    ? "none yet"
                                   : d.lastReturn == 1 ? "CAUGHT in the hand"
                                   : d.lastReturn == 2 ? "recovered while another item was selected"
                                                       : "missed, back to the pocket");
}

// Live state of the physical Lens of Truth: pocket / hand / face, which gate holds it, in the hand
// how far the glass is from the worn spot (and whether it may go on yet), and whether the reveal
// is on. The in-headset answer to "I held it to my face and nothing happened".
static void VrHandScaleReadout(WidgetInfo& info) {
    if (!VR_IsInitialized()) {
        ImGui::TextUnformatted("Not in VR.");
        return;
    }
    float ws = VR_GetWorldScale();
    if (ws < 1.0f) {
        ws = 35.0f;
    }
    // Open-hand lengths, wrist to fingertip, in game units at Link's 0.01 scale (adult / child
    // hand DLs: 862 / 548 model units).
    const float adultUnits = 8.62f;
    const float childUnits = 5.48f;
    const bool on = CVarGetInteger("gVrRealHandScale", 1) != 0;
    ImGui::Text("World scale now: %.1f units/m (%s), Link is %s", ws,
                CVarGetInteger("gVrAutoWorldScale", 1) ? "auto" : "manual",
                VrHand_ChildSized() ? "a child" : "an adult");
    if (on) {
        ImGui::Text("Hands drawn at: adult %.1f cm, child %.1f cm  (now x%.2f Link's size)",
                    CVarGetFloat("gVrHandSizeCm", 23.0f), CVarGetFloat("gVrHandSizeCmChild", 23.0f),
                    VrHand_ScaleFactor());
    }
    ImGui::Text("World-scale size would be: adult %.1f cm, child %.1f cm%s", adultUnits / ws * 100.0f,
                childUnits / ws * 100.0f, on ? "" : "  (in use: real-life scale is off)");
    ImGui::TextDisabled("Held items scale with the hand. Sword, shield and hammer hit what you see.");
}

static void VrLensReadout(WidgetInfo& info) {
    if (!VR_IsInitialized()) {
        ImGui::TextUnformatted("Not in VR.");
        return;
    }
    static const char* sState[] = { "none", "POCKET", "IN HAND", "WORN" };
    static const char* sGate[] = {
        "ARMED",
        "off (Physical Lens unchecked)",
        "lens not selected / not in normal play",
        "cutscene, pause or Link busy (state kept)",
        "no pocket point (headset untracked)",
    };
    static const char* sHand[] = { "left", "right" };
    VrLensDebug d;
    VrLens_GetDebug(&d);
    const int state = (d.state >= 0 && d.state < 4) ? d.state : 0;
    const int gate = (d.gate >= 0 && d.gate < 5) ? d.gate : 0;
    ImGui::Text("Lens: %s   (%s)", sState[state], sGate[gate]);
    if (state == 2 && d.carryHand >= 0 && d.carryHand < 2) {
        if (d.glassToFaceCm >= 0.0f) {
            ImGui::Text("In the %s hand — glass %.0f cm from your face (goes on within %.0f)%s", sHand[d.carryHand],
                        d.glassToFaceCm, d.wearDistanceCm, d.armed ? "" : "   move it away first");
        } else {
            ImGui::Text("In the %s hand", sHand[d.carryHand]);
        }
    }
    ImGui::Text("Reveal: %s%s   last put-on: %s", d.lensActive ? "ON" : "off", d.ours ? " (worn)" : "",
                d.lastActivate < 0    ? "none yet"
                : d.lastActivate == 1 ? "turned on"
                                      : "REFUSED (no magic / magic busy)");
}

// Live state of the physical masks: what's selected, in the hand and on the face, and in the hand
// how far the mask is from the face spot.
static void VrMaskReadout(WidgetInfo& info) {
    if (!VR_IsInitialized()) {
        ImGui::TextUnformatted("Not in VR.");
        return;
    }
    static const char* sState[] = { "none", "POCKET", "IN HAND" };
    static const char* sGate[] = {
        "ARMED",
        "off (Physical Masks unchecked)",
        "not in selector play (or horse / water / minigame)",
        "cutscene, pause or Link busy (state kept)",
    };
    static const char* sMask[] = { "none", "Keaton", "Skull", "Spooky", "Bunny Hood",
                                   "Goron", "Zora", "Gerudo", "Mask of Truth" };
    auto name = [](int m) { return (m >= 0 && m < 9) ? sMask[m] : "?"; };
    VrMaskDebug d;
    VrMask_GetDebug(&d);
    const int state = (d.state >= 0 && d.state < 3) ? d.state : 0;
    const int gate = (d.gate >= 0 && d.gate < 4) ? d.gate : 0;
    ImGui::Text("Masks: %s   (%s)", sState[state], sGate[gate]);
    ImGui::Text("Selected: %s   Worn: %s   In hand: %s", name(d.selected), name(d.worn), name(d.held));
    if (state == 2 && d.maskToFaceCm >= 0.0f) {
        ImGui::Text("Mask %.0f cm from your face (goes on within %.0f)%s", d.maskToFaceCm, d.wearDistanceCm,
                    d.armed ? "" : "   move it away first");
    }
}

// Live state of the physical hammer: which hands hold it, the off hand's distance to the handle
// against the reach, the simulated head's speed and swing tier, and what the head's last contact
// counted as. The in-headset answer to "my off hand won't take the handle" and "I slammed the
// floor and nothing happened".
static void VrHammerReadout(WidgetInfo& info) {
    if (!VR_IsInitialized()) {
        ImGui::TextUnformatted("Not in VR.");
        return;
    }
    static const char* sGate[] = {
        "ARMED",
        "off (Physical Hammer unchecked)",
        "hammer not in hand / physical combat inactive",
    };
    static const char* sTier[] = { "idle", "windup", "HOT (a hit lands)" };
    VrHammerDebug d;
    VrHammer_GetDebug(&d);
    const int gate = (d.gate >= 0 && d.gate < 3) ? d.gate : 2;
    ImGui::Text("Hammer: %s   (%s)", d.twoHand ? "TWO HANDS" : "one hand", sGate[gate]);
    if (gate != 0) {
        return;
    }
    if (d.offHandCm >= 0.0f) {
        ImGui::Text("Off hand -> handle: %.0f cm (takes hold within %.0f)%s", d.offHandCm, d.reachCm,
                    d.twoHand ? "" : (d.offInReach ? "   IN REACH: squeeze grip" : ""));
    }
    const int tier = (d.tier >= 0 && d.tier < 3) ? d.tier : 0;
    ImGui::Text("Head speed: %.1f m/s   %s   (hits from %.1f)", d.headMps, sTier[tier], d.hitSpeed);
    ImGui::Text("Last contact: %.1f m/s into the surface -> %s%s", d.lastImpactMps,
                d.lastImpact < 0    ? "none yet"
                : d.lastImpact == 1 ? "GROUND POUND"
                : d.lastImpact == 2 ? "WALL STRIKE"
                : d.lastImpact == 3 ? "body / object"
                                    : "soft touch (below the pound speed)",
                d.lastStrike == 2 ? ", heavy damage" : (d.lastStrike == 1 ? ", damage" : ""));
    ImGui::Text("Pounds from %.1f m/s into a floor", d.poundSpeed);
}

// Live state of physical block pushing: whether Link touches a pushable face, each hand's distance
// to it and its grip latch, then (attached) the push pressure against the threshold, the intent it
// gives, and the block's motion. The in-headset answer to "it won't grab" and "it won't move".
static void VrBlockReadout(WidgetInfo& info) {
    if (!VR_IsInitialized()) {
        ImGui::TextUnformatted("Not in VR.");
        return;
    }
    static const char* sGate[] = {
        "ARMED",
        "off (Physical Block Pushing unchecked)",
        "not VR first person / motion hands off",
        "cutscene / horse / water / transition",
    };
    static const char* sAction[] = { "not grabbing", "putting the item away", "HOLDING", "PUSHING", "PULLING" };
    VrBlockDebug d;
    VrBlock_GetDebug(&d);
    const int gate = (d.gate >= 0 && d.gate < 4) ? d.gate : 2;
    const int action = (d.action >= 0 && d.action < 5) ? d.action : 0;
    ImGui::Text("Block: %s%s   (%s)", sAction[action], (action != 0 && !d.handGrab) ? " (A button)" : "",
                sGate[gate]);
    if (gate != 0) {
        return;
    }
    if (action == 0) {
        ImGui::Text("Touching a pushable face: %s", d.pushable ? "YES" : "no");
        if (d.pushable) {
            for (int h = 0; h < 2; h++) {
                if (d.planeCm[h] <= -900.0f) {
                    ImGui::Text("%s hand: untracked", h == 0 ? "Left" : "Right");
                } else {
                    ImGui::Text("%s hand: %+.0f cm from the face (on it within %.0f)%s%s", h == 0 ? "Left" : "Right",
                                d.planeCm[h], d.reachCm, d.atWall[h] ? "   ON THE FACE" : "",
                                d.latched[h] ? "   GRIPPED" : "");
                }
            }
        }
    } else if (d.handGrab) {
        ImGui::Text("Pressure: %+.1f cm (push / pull past %.0f) -> %s", d.pressureCm, d.pushCm,
                    d.intent > 0 ? "PUSH" : (d.intent < 0 ? "PULL" : "hold"));
        ImGui::Text("Hands drifted %.0f cm (lets go past %.0f)", d.driftCm, d.pullOffCm);
        ImGui::Text("Block %s   steps this grab: %d", d.blockMoving ? "SLIDING" : "still", d.steps);
    }
    ImGui::Text("Last let go: %s", d.lastRelease < 0    ? "none yet"
                                   : d.lastRelease == 0 ? "grip opened"
                                   : d.lastRelease == 1 ? "hand pulled away"
                                                        : "Link left the grab (fall, hit, cutscene)");
}

// Physical climbing (VrClimb): which hand is on a climbable surface and holding, what drives the
// body, and how the last climb ended. The in-headset answer to "it won't grab" / "it won't let me up".
static void VrClimbReadout(WidgetInfo& info) {
    if (!VR_IsInitialized()) {
        ImGui::TextUnformatted("Not in VR.");
        return;
    }
    static const char* sGate[] = {
        "ARMED",
        "off (Physical Climbing unchecked)",
        "not VR first person / motion hands off",
        "cutscene / horse / transition",
    };
    VrClimbDebug d;
    VrClimb_GetDebug(&d);
    const int gate = (d.gate >= 0 && d.gate < 4) ? d.gate : 2;
    ImGui::Text("Climb: %s   (%s)",
                d.driving ? "HANDS" : (d.climbing ? "on the wall, stick climbing" : "not climbing"), sGate[gate]);
    if (gate != 0) {
        return;
    }
    for (int h = 0; h < 2; h++) {
        if (d.onSurface[h]) {
            ImGui::Text("%s hand: on %s (%+.0f cm off it)%s%s", h == 0 ? "Left" : "Right",
                        (d.surfFlags[h] & 0x08) ? "climbable wall" : ((d.surfFlags[h] & 0x02) ? "ladder" : "ladder top"),
                        d.surfCm[h], d.latched[h] ? "   HOLDING" : "", d.anchor == h ? "   DRIVES" : "");
        } else {
            ImGui::Text("%s hand: -%s%s", h == 0 ? "Left" : "Right", d.latched[h] ? "   HOLDING" : "",
                        d.anchor == h ? "   DRIVES" : "");
        }
    }
    if (d.driving) {
        ImGui::Text("Body move: %.1f cm asked, %.1f cm done   body %.0f cm from the wall", d.moveCm,
                    d.achievedCm, d.wallDistCm);
    }
    ImGui::Text("Last let go: %s   (toss good for %.0f cm)   hand mounts: %d",
                d.lastRelease < 0    ? "none yet"
                : d.lastRelease == 0 ? "dropped"
                : d.lastRelease == 1 ? "CLIMBED OVER"
                : d.lastRelease == 2 ? "stepped off at the floor"
                                     : "got on from the ladder's top",
                d.lastTossCm, d.mounts);
}

void SohMenu::AddMenuVRSettings() {
    // Collapsible groups. The menu has no collapsing-header widget, so vrBeginCollapsed adds a
    // custom header (ImGui::CollapsingHeader, closed by default) and vrEndCollapsed wraps the
    // PreFunc of every widget added to that page since, so each one also hides while its header
    // is closed (its own hide condition still applies when open).
    auto vrWidgets = [this](WidgetPath& path) -> std::vector<WidgetInfo>& {
        return menuEntries.at(path.sectionName).sidebars.at(path.sidebarName).columnWidgets.at(path.column);
    };
    auto vrBeginCollapsed = [this, &vrWidgets](WidgetPath& path, const char* label, bool* open) -> size_t {
        AddWidget(path, label, WIDGET_CUSTOM)
            .CustomFunction([label, open](WidgetInfo& info) { *open = ImGui::CollapsingHeader(label); })
            .HideInSearch(true);
        return vrWidgets(path).size();
    };
    auto vrEndCollapsed = [&vrWidgets](WidgetPath& path, size_t begin, bool* open) {
        std::vector<WidgetInfo>& widgets = vrWidgets(path);
        for (size_t i = begin; i < widgets.size(); i++) {
            WidgetFunc inner = widgets[i].preFunc;
            widgets[i].preFunc = [inner, open](WidgetInfo& info) {
                info.isHidden = false;
                if (inner != nullptr) {
                    inner(info);
                }
                if (!*open) {
                    info.isHidden = true;
                }
            };
        }
    };
    AddMenuEntry("VR Settings", CVAR_SETTING("Menu.VRSettingsSidebarSection"));

    // ------------------------------------------------------------------ General
    AddSidebarEntry("VR Settings", "General", 1);
    WidgetPath generalPath = { "VR Settings", "General", SECTION_COLUMN_1 };

    AddWidget(generalPath, "VR Mode (F9)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrEnabled")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Switch between VR and regular flat-screen play at any time - F9 does "
                              "the same thing. The switch takes effect on the next game tick (up to "
                              "50 ms). Turning VR off leaves the headset idle and ready to resume "
                              "instantly; if the game was started with VR off, turning it on "
                              "connects to the headset on the spot."));
    AddWidget(generalPath, "Stay In VR When Headset Is Removed", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrStayOnDoff")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrEnabled", 1); })
        .Options(CheckboxOptions().Tooltip(
            "By default, taking the headset off automatically drops the game to flat-screen play, "
            "and putting it back on resumes VR right where you left it. Enable this to keep "
            "rendering in VR while the headset is off (useful if removal is being detected when "
            "you don't want it to). Needs a runtime that reports headset presence; if yours "
            "doesn't, doffing never switches regardless."));
    AddWidget(generalPath, "VR View", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrFirstPerson")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrEnabled", 1); })
        .Options(ComboboxOptions()
                     .DefaultIndex(1)
                     .ComboMap(vrViewModeOptions)
                     .Tooltip("First Person: you are Link - the camera sits at his head, movement "
                              "follows your gaze, motion-control hands, snap turning. Third "
                              "Person: the stock game in stereo 3D - the view rides the game "
                              "camera exactly (position and facing, cutscenes included), you add "
                              "head-look and lean on top, and the right stick is normal C-buttons "
                              "(no snap turn; the game owns all camera movement). The camera's "
                              "up/down tilt is not applied - the horizon stays level with the real "
                              "world and you tilt your own head instead. Switch any time."));
    AddWidget(generalPath, "Auto Director Camera When Far From Link", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrFpAutoDirectorCam")
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger("gVrEnabled", 1) || !CVarGetInteger("gVrFirstPerson", 1);
        })
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Cutscenes stay first-person (you experience them from Link's eyes) - but when the "
            "game's camera goes far away from Link (a cutscene showing a distant place, or a "
            "scene still loading), first person would leave you staring at nothing, so the view "
            "automatically rides the game's camera until it comes back to Link. Disable to stay "
            "strictly in Link's head no matter what."));
    // ------------------------------------------------------- Cutscenes
    AddSidebarEntry("VR Settings", "Cutscenes", 1);
    WidgetPath cutscenePath = { "VR Settings", "Cutscenes", SECTION_COLUMN_1 };
    auto cutsceneTableHidden = []() {
        return !CVarGetInteger("gVrEnabled", 1) || !CVarGetInteger("gVrFirstPerson", 1) ||
               !CVarGetInteger("gVrCutsceneThirdPerson", 0);
    };

    AddWidget(cutscenePath, "Choose Third-Person Cutscenes", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrCutsceneThirdPerson")
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger("gVrEnabled", 1) || !CVarGetInteger("gVrFirstPerson", 1);
        })
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "In first person, the cutscenes ticked below play from their original camera, exactly "
            "like Third Person view: you move and turn with the game's camera and can still look "
            "around and lean with your head. Unticked ones stay in Link's eyes. First person "
            "resumes when the cutscene ends. Off = every cutscene stays first person (the far-"
            "camera fallback on the General page still applies)."));
    AddWidget(cutscenePath, "All Third Person", WIDGET_BUTTON)
        .PreFunc([cutsceneTableHidden](WidgetInfo& info) { info.isHidden = cutsceneTableHidden(); })
        .Options(ButtonOptions().Tooltip("Tick every row."))
        .Callback([](WidgetInfo& info) {
            int count;
            const VrCutsceneRow* rows = VrCutsceneView_Rows(&count);
            for (int i = 0; i < count; i++) {
                CVarSetInteger(rows[i].cvar, 1);
            }
            CVarSave();
        });
    AddWidget(cutscenePath, "All First Person", WIDGET_BUTTON)
        .PreFunc([cutsceneTableHidden](WidgetInfo& info) { info.isHidden = cutsceneTableHidden(); })
        .Options(ButtonOptions().Tooltip("Untick every row."))
        .Callback([](WidgetInfo& info) {
            int count;
            const VrCutsceneRow* rows = VrCutsceneView_Rows(&count);
            for (int i = 0; i < count; i++) {
                CVarSetInteger(rows[i].cvar, 0);
            }
            CVarSave();
        });
    AddWidget(cutscenePath, "Reset to Defaults", WIDGET_BUTTON)
        .PreFunc([cutsceneTableHidden](WidgetInfo& info) { info.isHidden = cutsceneTableHidden(); })
        .Options(ButtonOptions().Tooltip("Story cutscenes, bosses and the Zelda courtyard third "
                                         "person; everything else first person."))
        .Callback([](WidgetInfo& info) {
            int count;
            const VrCutsceneRow* rows = VrCutsceneView_Rows(&count);
            for (int i = 0; i < count; i++) {
                CVarClear(rows[i].cvar);
            }
            CVarSave();
        });
    {
        int count;
        const VrCutsceneRow* rows = VrCutsceneView_Rows(&count);
        const char* group = nullptr;
        for (int i = 0; i < count; i++) {
            const VrCutsceneRow& row = rows[i];
            if (group == nullptr || strcmp(group, row.group) != 0) {
                group = row.group;
                AddWidget(cutscenePath, group, WIDGET_SEPARATOR_TEXT)
                    .PreFunc([cutsceneTableHidden](WidgetInfo& info) { info.isHidden = cutsceneTableHidden(); });
            }
            CheckboxOptions options = CheckboxOptions().DefaultValue(row.thirdPerson);
            if (row.tooltip != nullptr) {
                options.Tooltip(row.tooltip);
            }
            AddWidget(cutscenePath, row.label, WIDGET_CVAR_CHECKBOX)
                .CVar(row.cvar)
                .PreFunc([cutsceneTableHidden](WidgetInfo& info) { info.isHidden = cutsceneTableHidden(); })
                .Options(options);
        }
    }

    // ------------------------------------------------------- Comfort & Movement
    AddSidebarEntry("VR Settings", "Comfort & Movement", 1);
    WidgetPath comfortPath = { "VR Settings", "Comfort & Movement", SECTION_COLUMN_1 };

    AddWidget(comfortPath, "Locomotion", WIDGET_SEPARATOR_TEXT);
    AddWidget(comfortPath, "Body Follows Head (VR Movement)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrBodyFollowsHead")
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger("gVrEnabled", 1) || !CVarGetInteger("gVrFirstPerson", 1);
        })
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Standard VR locomotion: Link's body always faces where you're looking, and on the "
            "ground your velocity IS the stick - exact direction, speed proportional to "
            "deflection, applied the same frame. Push a little to creep, release to stop dead; no "
            "acceleration ramp, no start-step or turn-around animations gating movement (motion-"
            "sickness comfort: what your hand does is exactly what your body feels). Rolls, "
            "jumps, attacks and knockbacks keep their normal motion, and the game still "
            "choreographs Link in cutscenes, on Epona and while climbing. Off = classic OoT "
            "movement."));
    AddWidget(comfortPath, "Dash in Any Direction", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrDashAnyDirection")
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger("gVrEnabled", 1) || !CVarGetInteger("gVrFirstPerson", 1) ||
                            !CVarGetInteger("gVrBodyFollowsHead", 1);
        })
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "A with the stick pushed is a dash, targeting or not: stick mostly forward = roll, "
            "mostly left / right = side hop, mostly back = backflip. Link travels in the exact "
            "direction the stick points (the quadrant only picks the move), and a roll steers with "
            "the stick instead of your gaze. Same speeds, distances and no-hop floors as the "
            "original. A with the stick centred is unchanged (put the sword away / Navi, or the "
            "targeted jump). Off = the original rules (hops only while targeting, roll only "
            "forward)."));
    AddWidget(comfortPath, "Legaiaflame's Lock On", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrLegaiaLockOn")
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger("gVrEnabled", 1) || !CVarGetInteger("gVrFirstPerson", 1);
        })
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Z-targeting the way the original game plays it. While you're locked onto something, "
            "the world rotates to keep the target in front of you: circle it and it stays dead "
            "ahead, with no turning and no walking yourself around to keep it in view. Link's body "
            "turns to face it too, and the control stick is read in that direction - forward closes "
            "in, back retreats, left and right circle the target.\n\n"
            "COMFORT WARNING: this rotates your view for you, which is the classic VR motion "
            "sickness trigger. The two sliders below exist to soften it - raise the framing cone to "
            "get free head movement back, lower the turn speed to make the rotation gentler. Off: "
            "your body keeps following your head, and the view is never moved for you."));
    AddWidget(comfortPath, "Lock-On Framing Cone: %.0f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrLockOnDeadzone")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrLegaiaLockOn", 0); })
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(80.0f)
                     .DefaultValue(0.0f)
                     .Step(5.0f)
                     .Format("%.0f")
                     .Tooltip("How far off-center the target is allowed to sit before the view "
                              "rotates to re-frame it. 0 (default) holds it dead ahead at all "
                              "times - the enemy is always in front of you and you cannot look "
                              "away for long. Raise it to carve out a cone where your head is "
                              "completely free and the world never moves: at 30 you can glance "
                              "around normally and the view only steps in when the target starts "
                              "leaving your vision."));
    AddWidget(comfortPath, "Lock-On Turn Speed: %.0f deg/s", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrLockOnTurnSpeed")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrLegaiaLockOn", 0); })
        .Options(FloatSliderOptions()
                     .Min(30.0f)
                     .Max(360.0f)
                     .DefaultValue(120.0f)
                     .Step(10.0f)
                     .Format("%.0f")
                     .Tooltip("Ceiling on how fast the view is allowed to swing around while "
                              "re-framing the target. Lower is gentler on motion sickness but "
                              "will lag behind a fast enemy you are circling closely."));
    AddWidget(comfortPath, "Artificial Turning (Right Stick)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrSnapTurnOn")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Turn the world with the right thumbstick, snap or smooth. In "
                              "first person the stick never presses C-buttons or items - those "
                              "live on the bindable VR Inputs and the item selector (menus and "
                              "third person keep the stock C-stick)."));
    AddWidget(comfortPath, "Turning Style", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrTurnStyle")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrSnapTurnOn", 1); })
        .Options(ComboboxOptions()
                     .DefaultIndex(0)
                     .ComboMap(vrTurnStyleOptions)
                     .Tooltip("Snap: flick the stick to rotate in discrete steps - the most "
                              "comfortable option. Smooth: hold the stick to rotate "
                              "continuously, like turning in a flat game."));
    AddWidget(comfortPath, "Snap Turn Angle: %.0f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrSnapTurnDegrees")
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger("gVrSnapTurnOn", 1) || CVarGetInteger("gVrTurnStyle", 0) != 0;
        })
        .Options(FloatSliderOptions()
                     .Min(10.0f)
                     .Max(180.0f)
                     .DefaultValue(45.0f)
                     .Step(5.0f)
                     .Format("%.0f")
                     .Tooltip("Degrees rotated per flick of the stick."));
    AddWidget(comfortPath, "Smooth Turn Speed: %.0f deg/s", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrSmoothTurnSpeed")
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger("gVrSnapTurnOn", 1) || CVarGetInteger("gVrTurnStyle", 0) != 1;
        })
        .Options(FloatSliderOptions()
                     .Min(30.0f)
                     .Max(360.0f)
                     .DefaultValue(120.0f)
                     .Step(5.0f)
                     .Format("%.0f")
                     .Tooltip("How fast the world rotates at full stick tilt."));
    AddWidget(comfortPath, "Smooth Turn Deadzone: %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrSmoothTurnDeadzone")
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger("gVrSnapTurnOn", 1) || CVarGetInteger("gVrTurnStyle", 0) != 1;
        })
        .Options(FloatSliderOptions()
                     .Min(0.05f)
                     .Max(0.80f)
                     .DefaultValue(0.25f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("How far the stick must tilt before turning starts. Raise it if "
                              "the view drifts when your thumb rests on the stick."));
    AddWidget(comfortPath, "Analog Turn Speed", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrSmoothTurnAnalog")
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger("gVrSnapTurnOn", 1) || CVarGetInteger("gVrTurnStyle", 0) != 1;
        })
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Stick tilt controls how fast you turn (gentle tilt = slow pan, "
                              "full tilt = full speed). Off: any tilt past the deadzone turns "
                              "at the full configured speed."));

    AddWidget(comfortPath, "World Scale", WIDGET_SEPARATOR_TEXT);
    AddWidget(comfortPath, "Match Scale To My Height (Be Link-Sized)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrAutoWorldScale")
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Derive world scale from your real standing eye height so you are exactly Link-sized: "
            "the ground meets your physical floor and age swaps rescale automatically. "
            "Re-measured when you recenter or re-enter first person - stand normally when you do. "
            "Disable to use the fixed World Scale slider instead (one true world size regardless "
            "of who is playing). Needs a runtime with floor calibration; without one the slider "
            "applies either way."));
    AddWidget(comfortPath, "World Scale: %.1f units/m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrWorldScale")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrAutoWorldScale", 1); })
        .Options(FloatSliderOptions()
                     .Min(10.0f)
                     .Max(100.0f)
                     .DefaultValue(35.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Game units per real-world meter. Higher makes the world feel smaller "
                              "(and physical movements cover more in-game distance); lower makes "
                              "everything tower over you. Applies live."));
    AddWidget(comfortPath, "Eye Height Offset: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHeadHeightOffset")
        .Options(FloatSliderOptions()
                     .Min(-100.0f)
                     .Max(100.0f)
                     .DefaultValue(-9.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Raise/lower the eye anchor relative to Link's eye height, in game "
                              "units. Lowering it brings the ground closer by exactly offset / world "
                              "scale meters."));

    AddWidget(comfortPath, "Hand & Item Size", WIDGET_SEPARATOR_TEXT);
    AddWidget(comfortPath, "Real-Life Hand Scale", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrRealHandScale")
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Draw your hands, and everything you hold, at one real-life size instead of letting "
            "the world scale resize them. Same size on auto or manual world scale, for any player "
            "height, with its own size for adult and child Link. Held items are "
            "drawn on the hand, so they keep matching it; the sword, shield and hammer hit "
            "exactly what you see. Off = hands follow the world scale (the old behavior)."));
    AddWidget(comfortPath, "Adult Hand Size: %.1f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandSizeCm")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrRealHandScale", 1); })
        .Options(FloatSliderOptions()
                     .Min(8.0f)
                     .Max(40.0f)
                     .DefaultValue(23.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Adult Link's open hand, wrist to fingertip, in real centimetres. "
                              "Calibrate as adult: hold your hand up, open, next to Link's and "
                              "match the fingertips. Applies live."));
    AddWidget(comfortPath, "Child Hand Size: %.1f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandSizeCmChild")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrRealHandScale", 1); })
        .Options(FloatSliderOptions()
                     .Min(5.0f)
                     .Max(40.0f)
                     .DefaultValue(23.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Child Link's open hand, wrist to fingertip, in real centimetres. "
                              "Calibrate as child the same way. Applies live."));
    AddWidget(comfortPath, "VrHandScaleReadout", WIDGET_CUSTOM).CustomFunction(VrHandScaleReadout).HideInSearch(true);

    const size_t comfortCalBegin =
        vrBeginCollapsed(comfortPath, "Calibration (Dev)##ComfortCal", &sVrComfortCalOpen);
    AddWidget(comfortPath, "Body", WIDGET_SEPARATOR_TEXT);
    AddWidget(comfortPath, "Small Body Collider", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrSmallBody")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Lets you get right up to walls, blocks and ledges, as if you had a small "
                              "collision radius. Link's real collision is unchanged for everything (climbing, "
                              "ledges, tunnels, gaps, grabbing): your view is a small circle inside his, so it "
                              "can get closer to a wall than his body but never past anything that stops "
                              "him. Needs roomscale. Off: your view stays at his body's distance."));
    AddWidget(comfortPath, "Small Body Radius: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrSmallBodyRadiusCm")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrSmallBody", 1); })
        .Options(FloatSliderOptions()
                     .Min(8.0f)
                     .Max(40.0f)
                     .DefaultValue(15.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How close your eyes can get to a wall. Never closer than the headset's near "
                              "clip allows, and never bigger than Link's own radius."));
    AddWidget(comfortPath, "Log Small Body (diagnostic)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrBodyLog")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrSmallBody", 1); })
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Writes vrbody_log.csv next to the game: one line per tick of what moves your view off "
            "Link's centre. For debugging only; leave off."));
    AddWidget(comfortPath, "VrBodyReadout", WIDGET_CUSTOM)
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrSmallBody", 1); })
        .CustomFunction([](WidgetInfo& info) {
            VrBodyDebug d;
            VrBody_GetDebug(&d);
            if (!d.active) {
                ImGui::TextUnformatted("Small body: inactive (VR first person with roomscale only)");
                return;
            }
            const float ws = VR_GetWorldScale() < 1.0f ? 35.0f : VR_GetWorldScale();
            ImGui::Text("Body radius %.0f cm, yours %.0f cm: view may sit %.0f cm off centre (now %.0f)%s",
                        d.bigRadius / ws * 100.0f, d.smallRadius / ws * 100.0f, d.slack / ws * 100.0f,
                        d.offset / ws * 100.0f, d.room ? "   closing in on a wall" : "");
        })
        .HideInSearch(true);

    AddWidget(comfortPath, "Hand Rotation (sword hand)", WIDGET_SEPARATOR_TEXT);
    AddWidget(comfortPath, "Tune while looking at the SWORD hand - the other hand mirrors automatically.", WIDGET_TEXT);
    AddWidget(comfortPath, "Pitch: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandCalPitch")
        .Options(FloatSliderOptions()
                     .Min(-180.0f)
                     .Max(180.0f)
                     .DefaultValue(88.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Rotation about the grip X axis (wrist tilt up/down)."));
    AddWidget(comfortPath, "Yaw: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandCalYaw")
        .Options(FloatSliderOptions()
                     .Min(-180.0f)
                     .Max(180.0f)
                     .DefaultValue(-100.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Rotation about the grip Y axis. If the sword points backward or sideways "
                              "out of your fist, adjust this first."));
    AddWidget(comfortPath, "Roll: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandCalRoll")
        .Options(FloatSliderOptions()
                     .Min(-180.0f)
                     .Max(180.0f)
                     .DefaultValue(80.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Rotation about the grip Z axis (twist around the handle - use to line up "
                              "the blade edge and palm)."));

    AddWidget(comfortPath, "Hand Position (sword hand)", WIDGET_SEPARATOR_TEXT);
    AddWidget(comfortPath, "Offset X: %.1f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandOffX")
        .Options(FloatSliderOptions()
                     .Min(-15.0f)
                     .Max(15.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Slide the hand along the grip X axis (real cm, so it holds at any world "
                              "scale; the other controller mirrors)."));
    AddWidget(comfortPath, "Offset Y: %.1f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandOffY")
        .Options(FloatSliderOptions()
                     .Min(-15.0f)
                     .Max(15.0f)
                     .DefaultValue(6.3f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Slide the hand along the grip Y axis."));
    AddWidget(comfortPath, "Offset Z: %.1f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandOffZ")
        .Options(FloatSliderOptions()
                     .Min(-15.0f)
                     .Max(15.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Slide the hand along the grip Z axis (roughly along the handle)."));

    AddWidget(comfortPath, "Left Hand Override", WIDGET_SEPARATOR_TEXT);
    AddWidget(comfortPath, "Tune Left Hand Separately", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrHandLOverride")
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "By default the left controller's hand is derived from the values above by mirror symmetry. "
            "If it doesn't look right, enable this and dial it in with its own values below."));
    AddWidget(comfortPath, "L Pitch: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLCalPitch")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-180.0f).Max(180.0f).DefaultValue(-149.0f).Step(1.0f).Format("%.1f"));
    AddWidget(comfortPath, "L Yaw: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLCalYaw")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-180.0f).Max(180.0f).DefaultValue(76.0f).Step(1.0f).Format("%.1f"));
    AddWidget(comfortPath, "L Roll: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLCalRoll")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-180.0f).Max(180.0f).DefaultValue(30.0f).Step(1.0f).Format("%.1f"));
    AddWidget(comfortPath, "L Offset X: %.1f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLOffX")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-15.0f).Max(15.0f).DefaultValue(0.0f).Step(0.5f).Format("%.1f"));
    AddWidget(comfortPath, "L Offset Y: %.1f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLOffY")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-15.0f).Max(15.0f).DefaultValue(6.3f).Step(0.5f).Format("%.1f"));
    AddWidget(comfortPath, "L Offset Z: %.1f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLOffZ")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-15.0f).Max(15.0f).DefaultValue(0.0f).Step(0.5f).Format("%.1f"));


    vrEndCollapsed(comfortPath, comfortCalBegin, &sVrComfortCalOpen);

    // ------------------------------------------------------------------ Gameplay
    AddSidebarEntry("VR Settings", "Gameplay", 1);
    WidgetPath gameplayPath = { "VR Settings", "Gameplay", SECTION_COLUMN_1 };

    AddWidget(gameplayPath, "Motion-Control Hands", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrMotionHands")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Detach Link's hands from his body and pin them to the VR controllers."));
    AddWidget(gameplayPath, "Motion Weapon Aim", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrWeaponAim")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrMotionHands", 1); })
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Slingshot seeds, arrows and the hookshot launch from your weapon hand and fly where "
            "that controller points (its aim ray - the same ray runtimes use for menu pointing). "
            "The weapon rides the hand holding the bow/slingshot model. Even while Z-targeted, "
            "your hand decides the shot; lock-on only steers the camera. Off = the stock "
            "stick-aiming behavior."));
    AddWidget(gameplayPath, "Left-Handed Mode", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrLeftHanded")
        .Options(CheckboxOptions().Tooltip(
            "Swap which controller drives the sword hand: the LEFT controller holds the sword (matching "
            "Link's own left-handedness) instead of the right."));
    AddWidget(gameplayPath, "Hide Link's Body", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrHideBody")
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "First person only: don't draw Link's body - just the floating hands and whatever "
            "they hold (the classic VR style). Some players prefer it because the body can block "
            "the view when looking down, and its animations don't always match what you're "
            "doing. Third person and cutscenes always show the full body."));
    AddWidget(gameplayPath, "Lock-On Reticle Size: %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrReticleScale")
        .Options(FloatSliderOptions()
                     .Min(0.3f)
                     .Max(3.0f)
                     .DefaultValue(1.0f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("Size of the in-world Z-target reticle (the converging triangles "
                              "that wrap whatever you lock onto). In VR the reticle is drawn in "
                              "the 3D scene at the target, not on the flat HUD."));
    AddWidget(gameplayPath, "Show Letterbox Bars", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrLetterbox")
        .Options(CheckboxOptions().Tooltip(
            "Show the cinematic black bars during Z-targeting and cutscenes, like the original "
            "game. Off by default in VR: the bars just float on the head-locked overlay and "
            "shrink your view. Flat-screen play is unaffected by this setting (see Enhancements "
            "> Graphics for the flat equivalent)."));
    AddWidget(gameplayPath, "Get-Item Hold-Up Distance: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrGetItemDistance")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrFirstPerson", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(100.0f)
                     .DefaultValue(20.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("When you get an item and Link holds it up, it sits this much further "
                              "out from your hands (away from you), so it isn't right on top of "
                              "your head. It still follows your hands."));

    // ----------------------------------------------------------- Physical Combat
    AddSidebarEntry("VR Settings", "Physical Combat", 1);
    WidgetPath physPath = { "VR Settings", "Physical Combat", SECTION_COLUMN_1 };

    AddWidget(physPath, "Physical Combat (Experimental)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysCombat")
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Physics-driven combat: swing the sword yourself (swing speed decides the hit), "
            "block by physically holding the shield up, grab pots with both hands, draw the bow "
            "for real. Replaces button combat only in VR first person; cutscenes, minigames and "
            "flat-screen play stay stock. Currently a foundations preview: this enables the "
            "underlying motion tracking - the combat changes themselves arrive milestone by "
            "milestone."));
    AddWidget(physPath, "Debug Overlay", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysCombatDebug")
        .Options(CheckboxOptions().Tooltip(
            "Draw hand velocity arrows and the per-tick motion path in the world, plus the live "
            "speed readout below. Arrow color previews the swing tiers: green = too slow to "
            "count, yellow = normal hit, red = strong hit."));

    const size_t combatCalBegin =
        vrBeginCollapsed(physPath, "Calibration (Dev)##CombatCal", &sVrCombatCalOpen);
    AddWidget(physPath, "Sword Swing Speeds", WIDGET_SEPARATOR_TEXT);
    AddWidget(physPath, "Arm Swing At: %.1f m/s", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysArmSpeed")
        .Options(FloatSliderOptions()
                     .Min(0.3f)
                     .Max(6.0f)
                     .DefaultValue(2.0f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("Blade tip speed (real meters/second) where a swing starts counting: "
                              "the trail appears, the swing sound plays, and enemies begin their "
                              "guard/dodge reactions. Below this the sword is inert."));
    AddWidget(physPath, "Hit At: %.1f m/s", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysHitSpeed")
        .Options(FloatSliderOptions()
                     .Min(0.5f)
                     .Max(10.0f)
                     .DefaultValue(5.0f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("Tip speed where the blade actually damages what it sweeps through, "
                              "at the weapon's normal slash strength."));
    AddWidget(physPath, "Strong Hit At: %.1f m/s", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysHeavySpeed")
        .Options(FloatSliderOptions()
                     .Min(1.0f)
                     .Max(16.0f)
                     .DefaultValue(8.0f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("Tip speed for a committed swing: damage steps up to the weapon's "
                              "jump-slash class (double against most enemies)."));
    AddWidget(physPath, "Re-Arm Below: %.1f m/s", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysReArmSpeed")
        .Options(FloatSliderOptions()
                     .Min(0.1f)
                     .Max(4.0f)
                     .DefaultValue(0.8f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("A swing ends (and the sword can strike again) once the tip slows "
                              "below this. One strike lands per swing; follow-through and wind-up "
                              "back up naturally re-arm you."));
    AddWidget(physPath, "Min Hand Speed To Damage: %.1f m/s", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysMinHandSpeed")
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(4.0f)
                     .DefaultValue(1.2f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("Anti-wiggle: the hand itself must move at least this fast for a "
                              "swing to deal damage. Pure wrist flicks spin the blade quickly but "
                              "shouldn't cut - real swings come from the arm. 0 disables."));

    AddWidget(physPath, "Blade Collider", WIDGET_SEPARATOR_TEXT);
    AddWidget(physPath, "Kokiri Sword Length: %.0f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysBladeLenKokiri")
        .Options(FloatSliderOptions()
                     .Min(10.0f)
                     .Max(60.0f)
                     .DefaultValue(18.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Blade collider length for the Kokiri Sword (game units, from the "
                              "hilt). Default matches the visible blade. Unlike the base game, the "
                              "collider is exactly one blade line - no invisible extra reach."));
    AddWidget(physPath, "Master Sword Length: %.0f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysBladeLenMaster")
        .Options(FloatSliderOptions()
                     .Min(10.0f)
                     .Max(70.0f)
                     .DefaultValue(35.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Blade collider length for the Master Sword (game units). Default "
                              "matches the visible blade."));
    AddWidget(physPath, "Biggoron Sword Length: %.0f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysBladeLenBiggoron")
        .Options(FloatSliderOptions()
                     .Min(20.0f)
                     .Max(90.0f)
                     .DefaultValue(55.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Blade collider length for the Biggoron Sword / Giant's Knife (game "
                              "units). Default matches the visible blade. Swings one-handed for "
                              "now; real two-handed weight comes in a later update."));
    AddWidget(physPath, "Deku Stick Length: %.0f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysBladeLenStick")
        .Options(FloatSliderOptions()
                     .Min(20.0f)
                     .Max(80.0f)
                     .DefaultValue(50.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Collider length for the Deku stick (game units). Default matches "
                              "the visible stick; the collider follows the shorter broken stub "
                              "automatically. Like the swords, it swings physically and snaps on "
                              "a real landed hit."));
    AddWidget(physPath, "Deku Stick World Collision", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysStickCollision")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("The stick collides with walls and objects like the swords do (and "
                              "snaps when whacked into them at attack speed). Disable to swing it "
                              "through the world like the base game - it still damages enemies "
                              "and still breaks on landed hits."));
    AddWidget(physPath, "Blade Width: %.0f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysBladeWidth")
        .Options(FloatSliderOptions()
                     .Min(1.0f)
                     .Max(12.0f)
                     .DefaultValue(4.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Width of the blade's stab cross-section (game units). Only matters "
                              "for straight thrusts - slashes get their hit area from the sweep "
                              "itself."));

    AddWidget(physPath, "Blade Physics", WIDGET_SEPARATOR_TEXT);
    AddWidget(physPath, "Blade Inertia & Collision", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysBladeInertia")
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "The sword becomes a simulated object: it follows your hand on a stiff spring, "
            "STOPS and bounces on walls, armor and enemy shields (with impact buzz and sparks) "
            "while your real hand keeps going, and springs back as you pull away. Swings below "
            "damage speed also bounce off enemies instead of passing through. Off = the blade "
            "is glued to your hand and passes through everything (damage rules unchanged)."));
    AddWidget(physPath, "Sword Snappiness: %.0f Hz", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysSword1HFreq")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(4.0f)
                     .Max(30.0f)
                     .DefaultValue(14.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How stiffly the virtual blade tracks your hand. High = near-1:1 "
                              "and responsive (light sword); low = floaty and heavy. Two-handed "
                              "weapons get their own weight in a later update."));
    AddWidget(physPath, "Sword Rotation Snappiness: %.0f Hz", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysSwordAngFreq")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(5.0f)
                     .Max(60.0f)
                     .DefaultValue(30.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How fast the blade's ANGLE follows your wrist. Raise this if the "
                              "sword lags behind during quick rotations; lower it for a heavier, "
                              "slower-turning weapon."));
    AddWidget(physPath, "Weight Wiggle: %.0f ms", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysWeightLagMs")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(80.0f)
                     .DefaultValue(0.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Cosmetic weight: the visible sword trails a fast swing by this "
                              "many milliseconds of rotation, then snaps back with a little "
                              "overshoot. Collision, damage and aim never lag - the sword still "
                              "moves exactly with your hand. 0 = off."));
    AddWidget(physPath, "Weight Wiggle Snap: %.1f Hz", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysWeightSnapHz")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(2.0f)
                     .Max(12.0f)
                     .DefaultValue(2.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("How quickly the trailing sword catches back up after a swing. "
                              "Lower = heavier and floppier, higher = a tight little flick."));
    AddWidget(physPath, "Blade Thickness: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysBladeThickness")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(4.0f)
                     .DefaultValue(0.4f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("Collision thickness of the blade (game units) - how far the steel "
                              "rests off a surface it is pressed against. Lower = the blade "
                              "visually touches walls more closely."));
    AddWidget(physPath, "Collide With Visual Meshes (Experimental)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysVisualMesh")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("EXPERIMENTAL: the blade collides with the rendered geometry you "
                              "actually see (harvested from the renderer, animated enemies "
                              "included) instead of the simplified collision mesh. Turn off to "
                              "fall back to collision-mesh physics."));
    AddWidget(physPath, "Blade Collider Roll: %.0f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysBladeRoll")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(-180.0f)
                     .Max(180.0f)
                     .DefaultValue(-90.0f)
                     .Step(5.0f)
                     .Format("%.0f")
                     .Tooltip("Rotates the flat blade collider about the blade axis. Turn on the "
                              "debug overlay and adjust until the cyan rectangle lies in the "
                              "same plane as the visible blade."));
    AddWidget(physPath, "Collider Shift Along Blade: %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysBladeShiftFwd")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(-10.0f)
                     .Max(10.0f)
                     .DefaultValue(0.0f)
                     .Step(0.01f)
                     .Format("%.2f")
                     .Tooltip("Slides the physical blade rectangle lengthwise (game units). "
                              "Align the cyan debug outline with the visible steel."));
    AddWidget(physPath, "Collider Shift Along Edge: %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysBladeShiftEdge")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(-10.0f)
                     .Max(10.0f)
                     .DefaultValue(1.1f)
                     .Step(0.01f)
                     .Format("%.2f")
                     .Tooltip("Slides the collider across the blade's width direction."));
    AddWidget(physPath, "Collider Shift Along Flat: %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysBladeShiftFlat")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(-10.0f)
                     .Max(10.0f)
                     .DefaultValue(0.0f)
                     .Step(0.01f)
                     .Format("%.2f")
                     .Tooltip("Slides the collider perpendicular to the blade's flat plane."));
    AddWidget(physPath, "Limb Resistance: %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysLimbResist")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(1.0f)
                     .DefaultValue(0.5f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("How much limbs fight back against the blade: 0 = ragdoll-loose, "
                              "1 = they barely budge. Limbs lag behind your push and spring "
                              "back firmly."));
    AddWidget(physPath, "Body Capsule Radius: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysBodyCapsuleRadius")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(1.0f)
                     .Max(12.0f)
                     .DefaultValue(4.5f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("Thickness of the invisible capsules fitted to each enemy/NPC "
                              "skeleton bone - the surfaces the sword actually rests on. Match "
                              "to limb thickness: too big and the sword floats off bodies, too "
                              "small and it sinks in before stopping."));
    AddWidget(physPath, "Blade Tip Taper: %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysBladeTipTaper")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(0.5f)
                     .DefaultValue(0.2f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("The blade collider is a flat rectangle as wide as Blade Width, "
                              "converging to a point over this trailing fraction of its length. "
                              "0 = square tip, 0.2 = pointed over the last 20%."));
    AddWidget(physPath, "Impact Tolerance: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysTouchTolerance")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.05f)
                     .Max(3.0f)
                     .DefaultValue(0.3f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("How close (game units) counts as a real hit for impact sounds, "
                              "sparks and rumble. Because the blade is stopped exactly AT "
                              "surfaces rather than inside them, a little tolerance is needed or "
                              "impacts rarely register. Raise if hits feel like they get missed."));
    AddWidget(physPath, "Swing-Through Speed: %.1f m/s", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysPassthroughSpeed")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(8.0f)
                     .DefaultValue(2.2f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("Swings faster than this cut THROUGH surfaces instead of stopping "
                              "on them; gentle contact still rests on the surface. 0 = the "
                              "blade never passes through anything."));
    AddWidget(physPath, "Hit Flinch Amount: %.0f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysFlinchAmount")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(60.0f)
                     .DefaultValue(0.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Punching-bag hit reaction: how far a struck body caves toward "
                              "the swing around the impact point before springing back. Purely "
                              "visual - hitboxes and enemy AI never move. 0 = off."));
    AddWidget(physPath, "Knockback Strength: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysKnockbackScale")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(3.0f)
                     .DefaultValue(0.0f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("How hard landed hits shove enemies, scaled by swing speed. "
                              "Bosses and rooted enemies never budge. 0 = off."));
    AddWidget(physPath, "Blade Push Strength: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysPressPush")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(6.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Enemies get nudged away when you press the blade against them "
                              "(no damage - just steel insisting). Bosses and rooted enemies "
                              "stay put. 0 = off."));
    AddWidget(physPath, "Cut Resistance (Flesh): %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysCutDragFlesh")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(0.97f)
                     .DefaultValue(0.73f)
                     .Step(0.01f)
                     .Format("%.2f")
                     .Tooltip("How much enemy bodies hold the blade back while a fast swing "
                              "cuts through them. The blade drags in the cut (with rumble) and "
                              "catches up to your hand on exit. 0 = clean effortless cuts."));
    AddWidget(physPath, "Cut Resistance (World): %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysCutDragWorld")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(0.97f)
                     .DefaultValue(0.73f)
                     .Step(0.01f)
                     .Format("%.2f")
                     .Tooltip("Drag while a fast swing passes through world geometry (walls, "
                              "fences). Light by default so committed swings stay fluid."));
    AddWidget(physPath, "Blade Friction: %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysBladeFriction")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysBladeInertia", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(1.0f)
                     .DefaultValue(0.5f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("How much the blade drags while sliding along a surface. 0 = "
                              "frictionless skating, higher = the blade angle sticks and trails "
                              "as you drag it across walls and floors."));

    AddWidget(physPath, "Shield", WIDGET_SEPARATOR_TEXT);
    AddWidget(physPath, "Physical Shield", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysShield")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("The shield rides your off hand whenever it can - no button, no "
                              "stance, no movement lock. Hold it up and whatever hits it is "
                              "blocked; whatever gets around it hits YOU. Blocks never stagger "
                              "you or shove you back. Child Link still carries the Hylian "
                              "shield on his back, and aiming items (bow, hookshot) suspends "
                              "the shield exactly like vanilla."));
    AddWidget(physPath, "Shield Facing Leniency: %.0f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysShieldFacingDeg")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysShield", 1); })
        .Options(FloatSliderOptions()
                     .Min(30.0f)
                     .Max(180.0f)
                     .DefaultValue(65.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How far the shield's face may be turned away from an attack "
                              "and still block it. Outside this cone the hit doesn't count - "
                              "no back-of-shield or corner deflections; if the attack also "
                              "reached your body, it hurts. Lower = you must square up to the "
                              "threat. 180 = block from any angle."));
    AddWidget(physPath, "Shield Width Top: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysShieldWidthTop")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysShield", 1); })
        .Options(FloatSliderOptions()
                     .Min(5.0f)
                     .Max(90.0f)
                     .DefaultValue(21.2f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Width of the block collider's TOP edge (game units). Turn on "
                              "the debug overlay - the cyan quad on the shield is exactly what "
                              "blocks. Anything outside it doesn't count: smaller = stricter."));
    AddWidget(physPath, "Shield Width Bottom: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysShieldWidthBottom")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysShield", 1); })
        .Options(FloatSliderOptions()
                     .Min(5.0f)
                     .Max(90.0f)
                     .DefaultValue(11.9f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Width of the block collider's BOTTOM edge. Set narrower than "
                              "the top for a Hylian-style tapered shape."));
    AddWidget(physPath, "Shield Height: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysShieldHeight")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysShield", 1); })
        .Options(FloatSliderOptions()
                     .Min(5.0f)
                     .Max(90.0f)
                     .DefaultValue(18.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Height of the block collider (game units)."));
    AddWidget(physPath, "Shield Shift Across: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysShieldShiftX")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysShield", 1); })
        .Options(FloatSliderOptions()
                     .Min(-40.0f)
                     .Max(40.0f)
                     .DefaultValue(-1.1f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Slides the collider along its own width axis (game units) - the "
                              "shifts follow the tilt sliders, so they always mean what they "
                              "say. Center the cyan quad on the visible steel."));
    AddWidget(physPath, "Shield Shift Up/Down: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysShieldShiftY")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysShield", 1); })
        .Options(FloatSliderOptions()
                     .Min(-40.0f)
                     .Max(40.0f)
                     .DefaultValue(-0.8f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Slides the collider along the shield face's vertical axis."));
    AddWidget(physPath, "Shield Shift Out: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysShieldShiftZ")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysShield", 1); })
        .Options(FloatSliderOptions()
                     .Min(-40.0f)
                     .Max(40.0f)
                     .DefaultValue(-2.5f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Slides the collider along the shield's facing direction, until "
                              "it lies in the same plane as the steel."));
    AddWidget(physPath, "Shield Pitch: %.0f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysShieldPitch")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysShield", 1); })
        .Options(FloatSliderOptions()
                     .Min(-180.0f)
                     .Max(180.0f)
                     .DefaultValue(-4.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Tilts the collider plane forward/back relative to the grip."));
    AddWidget(physPath, "Shield Yaw: %.0f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysShieldYaw")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysShield", 1); })
        .Options(FloatSliderOptions()
                     .Min(-180.0f)
                     .Max(180.0f)
                     .DefaultValue(0.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Turns the collider plane left/right relative to the grip."));
    AddWidget(physPath, "Shield Roll: %.0f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPhysShieldRoll")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysShield", 1); })
        .Options(FloatSliderOptions()
                     .Min(-180.0f)
                     .Max(180.0f)
                     .DefaultValue(-90.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Spins the collider within the shield plane."));

    AddWidget(physPath, "Export", WIDGET_SEPARATOR_TEXT);
    AddWidget(physPath, "Copy All Combat Tuning Values", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip("Copy every Physical Combat tuning value (speed tiers, "
                                         "blade collider, physics, hit feel — including the "
                                         "console-only knobs) so they can be handed to a "
                                         "developer to become the defaults."))
        .Callback([](WidgetInfo& info) {
            char buf[2048];
            snprintf(buf, sizeof(buf),
                     "gVrPhysArmSpeed=%.2f\n"
                     "gVrPhysHitSpeed=%.2f\n"
                     "gVrPhysHeavySpeed=%.2f\n"
                     "gVrPhysReArmSpeed=%.2f\n"
                     "gVrPhysMinHandSpeed=%.2f\n"
                     "gVrPhysBladeLenKokiri=%.2f\n"
                     "gVrPhysBladeLenMaster=%.2f\n"
                     "gVrPhysBladeLenBiggoron=%.2f\n"
                     "gVrPhysBladeWidth=%.2f\n"
                     "gVrPhysBladeThickness=%.2f\n"
                     "gVrPhysBladeRoll=%.1f\n"
                     "gVrPhysBladeShiftFwd=%.2f\n"
                     "gVrPhysBladeShiftEdge=%.2f\n"
                     "gVrPhysBladeShiftFlat=%.2f\n"
                     "gVrPhysBladeTipTaper=%.2f\n"
                     "gVrPhysBladeInertia=%d\n"
                     "gVrPhysSword1HFreq=%.2f\n"
                     "gVrPhysSword1HZeta=%.2f\n"
                     "gVrPhysSwordAngFreq=%.2f\n"
                     "gVrPhysWeightLagMs=%.1f\n"
                     "gVrPhysWeightSnapHz=%.1f\n"
                     "gVrPhysMaxAccel=%.1f\n"
                     "gVrPhysMaxAngAccel=%.1f\n"
                     "gVrPhysTouchTolerance=%.2f\n"
                     "gVrPhysBladeFriction=%.2f\n"
                     "gVrPhysPivotOnly=%d\n"
                     "gVrPhysVisualMesh=%d\n"
                     "gVrPhysMeshRadius=%.1f\n"
                     "gVrPhysPassthroughSpeed=%.2f\n"
                     "gVrPhysCutDragFlesh=%.2f\n"
                     "gVrPhysCutDragWorld=%.2f\n"
                     "gVrPhysKnockbackScale=%.2f\n"
                     "gVrPhysKnockbackCap=%.2f\n"
                     "gVrPhysPressPush=%.2f\n"
                     "gVrPhysFlinchAmount=%.1f\n"
                     "gVrPhysLimbResist=%.2f\n"
                     "gVrPhysLimbRadius=%.1f\n"
                     "gVrPhysLimbPushMax=%.1f\n"
                     "gVrPhysBodyCapsuleRadius=%.2f\n"
                     "gVrPhysSubQuads=%d\n"
                     "gVrPhysShield=%d\n"
                     "gVrPhysShieldWidthTop=%.1f\n"
                     "gVrPhysShieldWidthBottom=%.1f\n"
                     "gVrPhysShieldHeight=%.1f\n"
                     "gVrPhysShieldShiftX=%.1f\n"
                     "gVrPhysShieldShiftY=%.1f\n"
                     "gVrPhysShieldShiftZ=%.1f\n"
                     "gVrPhysShieldPitch=%.0f\n"
                     "gVrPhysShieldYaw=%.0f\n"
                     "gVrPhysShieldRoll=%.0f\n"
                     "gVrPhysShieldFacingDeg=%.0f\n",
                     CVarGetFloat("gVrPhysArmSpeed", 2.0f), CVarGetFloat("gVrPhysHitSpeed", 5.0f),
                     CVarGetFloat("gVrPhysHeavySpeed", 8.0f), CVarGetFloat("gVrPhysReArmSpeed", 0.8f),
                     CVarGetFloat("gVrPhysMinHandSpeed", 1.2f),
                     CVarGetFloat("gVrPhysBladeLenKokiri", 18.0f),
                     CVarGetFloat("gVrPhysBladeLenMaster", 35.0f),
                     CVarGetFloat("gVrPhysBladeLenBiggoron", 55.0f),
                     CVarGetFloat("gVrPhysBladeWidth", 4.0f), CVarGetFloat("gVrPhysBladeThickness", 0.4f),
                     CVarGetFloat("gVrPhysBladeRoll", -90.0f), CVarGetFloat("gVrPhysBladeShiftFwd", 0.0f),
                     CVarGetFloat("gVrPhysBladeShiftEdge", 1.1f),
                     CVarGetFloat("gVrPhysBladeShiftFlat", 0.0f),
                     CVarGetFloat("gVrPhysBladeTipTaper", 0.2f), CVarGetInteger("gVrPhysBladeInertia", 1),
                     CVarGetFloat("gVrPhysSword1HFreq", 14.0f), CVarGetFloat("gVrPhysSword1HZeta", 1.0f),
                     CVarGetFloat("gVrPhysSwordAngFreq", 30.0f), CVarGetFloat("gVrPhysWeightLagMs", 0.0f),
                     CVarGetFloat("gVrPhysWeightSnapHz", 2.0f), CVarGetFloat("gVrPhysMaxAccel", 400.0f),
                     CVarGetFloat("gVrPhysMaxAngAccel", 3000.0f),
                     CVarGetFloat("gVrPhysTouchTolerance", 0.3f),
                     CVarGetFloat("gVrPhysBladeFriction", 0.5f), CVarGetInteger("gVrPhysPivotOnly", 1),
                     CVarGetInteger("gVrPhysVisualMesh", 1), CVarGetFloat("gVrPhysMeshRadius", 150.0f),
                     CVarGetFloat("gVrPhysPassthroughSpeed", 2.2f),
                     CVarGetFloat("gVrPhysCutDragFlesh", 0.73f),
                     CVarGetFloat("gVrPhysCutDragWorld", 0.73f),
                     CVarGetFloat("gVrPhysKnockbackScale", 0.0f),
                     CVarGetFloat("gVrPhysKnockbackCap", 8.0f), CVarGetFloat("gVrPhysPressPush", 0.0f),
                     CVarGetFloat("gVrPhysFlinchAmount", 0.0f), CVarGetFloat("gVrPhysLimbResist", 0.5f),
                     CVarGetFloat("gVrPhysLimbRadius", 9.0f), CVarGetFloat("gVrPhysLimbPushMax", 22.0f),
                     CVarGetFloat("gVrPhysBodyCapsuleRadius", 4.5f), CVarGetInteger("gVrPhysSubQuads", 3),
                     CVarGetInteger("gVrPhysShield", 1), CVarGetFloat("gVrPhysShieldWidthTop", 21.2f),
                     CVarGetFloat("gVrPhysShieldWidthBottom", 11.9f), CVarGetFloat("gVrPhysShieldHeight", 18.0f),
                     CVarGetFloat("gVrPhysShieldShiftX", -1.1f), CVarGetFloat("gVrPhysShieldShiftY", -0.8f),
                     CVarGetFloat("gVrPhysShieldShiftZ", -2.5f), CVarGetFloat("gVrPhysShieldPitch", -4.0f),
                     CVarGetFloat("gVrPhysShieldYaw", 0.0f), CVarGetFloat("gVrPhysShieldRoll", -90.0f),
                     CVarGetFloat("gVrPhysShieldFacingDeg", 65.0f));
            ImGui::SetClipboardText(buf);
        });

    AddWidget(physPath, "Diagnostics", WIDGET_SEPARATOR_TEXT);
    AddWidget(physPath, "Pacify Enemies (Testing)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysPacifist")
        .Options(CheckboxOptions().Tooltip(
            "Freezes all enemies solid: no AI, no detection, no attacks, animation paused - "
            "living statues for testing blade physics and limb manipulation. Damage still "
            "lands. Uncheck to thaw."));
    AddWidget(physPath, "Record Physics Log", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysLog")
        .Options(CheckboxOptions().Tooltip(
            "Capture every physics step of the held weapon (hand target, the pose the spring "
            "produced, the pose after collision, every contact point/normal/depth, and a "
            "fingerprint of the collision geometry in play). Keeps the most recent ~20 seconds. "
            "Turn it ON, go reproduce the problem, then turn it OFF - the capture is written to "
            "vr_phys_log.csv next to the game executable."));
    AddWidget(physPath, "VrPhysLogControl", WIDGET_CUSTOM).CustomFunction(VrPhysLogControl).HideInSearch(true);

    AddWidget(physPath, "Haptics", WIDGET_SEPARATOR_TEXT);
    AddWidget(physPath, "Test Left Haptic", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip("Buzz the left controller for 0.1 s."))
        .Callback([](WidgetInfo& info) { VR_TriggerHaptic(0, 0.8f, 0.0f, 100.0f); });
    AddWidget(physPath, "Test Right Haptic", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip("Buzz the right controller for 0.1 s."))
        .Callback([](WidgetInfo& info) { VR_TriggerHaptic(1, 0.8f, 0.0f, 100.0f); });

    AddWidget(physPath, "Live Hand Speed", WIDGET_SEPARATOR_TEXT);
    AddWidget(physPath, "VrPhysCombatReadout", WIDGET_CUSTOM).CustomFunction(VrPhysCombatReadout).HideInSearch(true);

    vrEndCollapsed(physPath, combatCalBegin, &sVrCombatCalOpen);

    // --------------------------------------------------------------- HUD & Menus
    AddSidebarEntry("VR Settings", "HUD & Menus", 1);
    WidgetPath hudPath = { "VR Settings", "HUD & Menus", SECTION_COLUMN_1 };

    AddWidget(hudPath, "HUD", WIDGET_SEPARATOR_TEXT);
    AddWidget(hudPath, "HUD Layout", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrHudLayout")
        .Options(ComboboxOptions()
                     .DefaultIndex(0)
                     .ComboMap(vrHudLayoutOptions)
                     .Tooltip("Wrist Panels: hearts, magic, rupees, keys and timers on your LEFT "
                              "wrist; item buttons, the A button and the minimap on your RIGHT "
                              "wrist, each on a small dark panel (VR Settings -> Wrist HUD lays them "
                              "out). Glance at a hand to read it. "
                              "Classic: the whole HUD on one panel floating in front of you. "
                              "Text boxes always get their own panel in front of you either way."));
    AddWidget(hudPath, "HUD Distance: %.1f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudDistance")
        .PreFunc([](WidgetInfo& info) { info.isHidden = VrHudWristLayout(); })
        .Options(FloatSliderOptions().Min(0.5f).Max(5.0f).DefaultValue(2.0f).Step(0.1f).Format("%.1f"));
    AddWidget(hudPath, "HUD Size: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudSize")
        .PreFunc([](WidgetInfo& info) { info.isHidden = VrHudWristLayout(); })
        .Options(FloatSliderOptions().Min(0.2f).Max(3.0f).DefaultValue(1.5f).Step(0.05f).Format("%.2f"));
    AddWidget(hudPath, "HUD Horizontal: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudOffX")
        .PreFunc([](WidgetInfo& info) { info.isHidden = VrHudWristLayout(); })
        .Options(FloatSliderOptions().Min(-1.5f).Max(1.5f).DefaultValue(0.0f).Step(0.02f).Format("%.2f"));
    AddWidget(hudPath, "HUD Vertical: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudOffY")
        .PreFunc([](WidgetInfo& info) { info.isHidden = VrHudWristLayout(); })
        .Options(FloatSliderOptions().Min(-1.5f).Max(1.5f).DefaultValue(0.0f).Step(0.02f).Format("%.2f"));

    // Text boxes (dialogue, signs, chests, item text) and the ocarina staff always get their own
    // panel that soft-follows in front of the player; only its placement is configurable. All in
    // real metres: world scale and Link's age don't change it.
    AddWidget(hudPath, "Text Panel", WIDGET_SEPARATOR_TEXT);
    AddWidget(hudPath, "Text Distance: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrTextDistance")
        .Options(FloatSliderOptions()
                     .Min(0.6f)
                     .Max(3.0f)
                     .DefaultValue(1.4f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("How far in front of your eyes text boxes and the ocarina staff appear. "
                              "Text always floats on its own panel in front of you, whatever the HUD "
                              "attachment is."));
    AddWidget(hudPath, "Text Width: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrTextWidth")
        .Options(FloatSliderOptions().Min(0.3f).Max(2.0f).DefaultValue(0.9f).Step(0.05f).Format("%.2f"));
    AddWidget(hudPath, "Text Height: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrTextHeight")
        .Options(FloatSliderOptions()
                     .Min(-0.8f)
                     .Max(0.5f)
                     .DefaultValue(-0.15f)
                     .Step(0.01f)
                     .Format("%.2f")
                     .Tooltip("Panel centre relative to eye level (negative = below)."));
    AddWidget(hudPath, "Text Follow Deadzone: %.0f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrTextFollowDeg")
        .Options(FloatSliderOptions()
                     .Min(5.0f)
                     .Max(60.0f)
                     .DefaultValue(20.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How far you can look away before the panel glides back in front of "
                              "you. Inside this angle it stays perfectly still."));
    AddWidget(hudPath, "Text Follow Smoothing: %.2f s", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrTextFollowSpeed")
        .Options(FloatSliderOptions()
                     .Min(0.05f)
                     .Max(1.0f)
                     .DefaultValue(0.25f)
                     .Step(0.01f)
                     .Format("%.2f")
                     .Tooltip("How long the panel takes to catch up once it starts following. "
                              "Lower = snappier, higher = floatier."));

    AddWidget(hudPath, "Menu Screen", WIDGET_SEPARATOR_TEXT);
    AddWidget(hudPath, "Menu Screen Distance: %.1f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrScreenDistance")
        .Options(FloatSliderOptions()
                     .Min(0.5f)
                     .Max(5.0f)
                     .DefaultValue(2.2f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("How far in front of you the floating menu panel (file select, pause) "
                              "appears. Applies the next time a menu opens."));
    AddWidget(hudPath, "Menu Screen Size: %.1f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrScreenSize")
        .Options(FloatSliderOptions()
                     .Min(0.5f)
                     .Max(5.0f)
                     .DefaultValue(2.4f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("Width of the floating menu panel in meters (height follows 4:3). "
                              "Applies live."));

    AddWidget(hudPath, "File Select", WIDGET_SEPARATOR_TEXT);
    AddWidget(hudPath, "File Select in World Space", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrFileSelectWorld")
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Stand inside the file select's sky with the menu window floating in front of you, "
            "where you were looking when it came up. Controls are unchanged. "
            "Off: the floating menu panel."));
    AddWidget(hudPath, "File Select Distance: %.1f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrFileSelectDistance")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrFileSelectWorld", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.8f)
                     .Max(3.0f)
                     .DefaultValue(1.6f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("How far in front of you the file select window hangs. It keeps the "
                              "same apparent size at any distance (use File Select Size for that). "
                              "Applies live."));
    AddWidget(hudPath, "File Select Size: %.0f%%", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrFileSelectScale")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrFileSelectWorld", 1); })
        .Options(FloatSliderOptions()
                     .Min(50.0f)
                     .Max(150.0f)
                     .DefaultValue(100.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Size of the file select window. At 100% it spans about the same part "
                              "of your view as it did on a TV. Applies live."));
    AddWidget(hudPath, "File Select Height: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrFileSelectHeightCm")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrFileSelectWorld", 1); })
        .Options(FloatSliderOptions()
                     .Min(-60.0f)
                     .Max(60.0f)
                     .DefaultValue(0.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Raise or lower the window relative to your eye height when the file "
                              "select came up."));

    AddWidget(hudPath, "Boot Logo", WIDGET_SEPARATOR_TEXT);
    AddWidget(hudPath, "Boot Logo in World Space", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrLogoWorld")
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "The N64 logo screen at startup floats in front of you, where you were looking when it "
            "came up. Off: the floating menu panel."));
    AddWidget(hudPath, "Boot Logo Distance: %.1f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrLogoDistance")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrLogoWorld", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.8f)
                     .Max(6.0f)
                     .DefaultValue(2.5f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("How far in front of you the logo hangs. It keeps the same apparent "
                              "size at any distance."));
    AddWidget(hudPath, "Boot Logo Height: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrLogoHeightCm")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrLogoWorld", 1); })
        .Options(FloatSliderOptions()
                     .Min(-60.0f)
                     .Max(60.0f)
                     .DefaultValue(0.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Raise or lower the logo relative to your eye height when it came up."));

    AddWidget(hudPath, "Title Screen", WIDGET_SEPARATOR_TEXT);
    AddWidget(hudPath, "Title Logo on a Floating Panel", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrTitleLogoPanel")
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "The title screen's logo, PRESS START and copyright float in front of you and follow your "
            "head softly (like text boxes). Off: they go to the HUD, where the wrist layout hides them."));
    AddWidget(hudPath, "Title Logo Width: %.1f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrTitleLogoWidth")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrTitleLogoPanel", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.5f)
                     .Max(5.0f)
                     .DefaultValue(1.8f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("Width of the whole title frame (the logo spans about two thirds of it)."));
    AddWidget(hudPath, "Title Logo Distance: %.1f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrTitleLogoDistance")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrTitleLogoPanel", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.5f)
                     .Max(6.0f)
                     .DefaultValue(2.2f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("How far in front of you the title floats."));
    AddWidget(hudPath, "Title Logo Height: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrTitleLogoHeight")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrTitleLogoPanel", 1); })
        .Options(FloatSliderOptions()
                     .Min(-1.0f)
                     .Max(1.0f)
                     .DefaultValue(0.0f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("Raise or lower the title relative to your eyes."));

    AddWidget(hudPath, "Pause Menu", WIDGET_SEPARATOR_TEXT);
    AddWidget(hudPath, "Pause Menu in World Space", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPauseWorldSpace")
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "First person: pausing freezes the game but keeps it rendering, so you can still look "
            "around the frozen world. The four inventory pages surround you where you opened the "
            "menu, and the whole box spins around you as you change pages. Controls are unchanged. "
            "Off: the floating menu panel."));
    AddWidget(hudPath, "Pause Box Radius: %.1f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPauseRadius")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPauseWorldSpace", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.6f)
                     .Max(3.0f)
                     .DefaultValue(1.3f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("Size of the pause box: the distance from its center to each page. "
                              "Applies live."));
    AddWidget(hudPath, "Pause View Position: %.0f%%", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPauseViewBack")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPauseWorldSpace", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(100.0f)
                     .DefaultValue(100.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Where you stand inside the box. 100% is the original game's camera "
                              "spot, near the back wall: the front page is farther away and you see "
                              "all of it, and changing pages swings the box around you the way the "
                              "original camera moved. 0% puts you at the center, surrounded."));
    AddWidget(hudPath, "Pause Page Size: %.0f%%", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPausePageScale")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPauseWorldSpace", 1); })
        .Options(FloatSliderOptions()
                     .Min(40.0f)
                     .Max(160.0f)
                     .DefaultValue(100.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Page size. At 100% the four pages meet at the corners and close the "
                              "box around you (each page spans 90 degrees); smaller pages float "
                              "apart and are easier to take in at a glance."));
    AddWidget(hudPath, "Pause Link Size: %.0f%%", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPauseLinkScale")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPauseWorldSpace", 1); })
        .Options(FloatSliderOptions()
                     .Min(50.0f)
                     .Max(250.0f)
                     .DefaultValue(100.0f)
                     .Step(5.0f)
                     .Format("%.0f")
                     .Tooltip("Size of the 3D Link standing on the equipment page. 100% matches the "
                              "original picture; he grows from his feet."));
    AddWidget(hudPath, "Pause Box Height: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPauseHeightCm")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPauseWorldSpace", 1); })
        .Options(FloatSliderOptions()
                     .Min(-60.0f)
                     .Max(60.0f)
                     .DefaultValue(0.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Raise or lower the pages relative to your eye height when the menu "
                              "opened."));
    AddWidget(hudPath, "Pause Name Panel Depth: %.0f%%", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPausePanelDepth")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPauseWorldSpace", 1); })
        .Options(FloatSliderOptions()
                     .Min(30.0f)
                     .Max(95.0f)
                     .DefaultValue(60.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Distance of the item-name bar, as a percentage of the distance to the "
                              "front page. It stays the same size on screen; this only sets how close "
                              "it floats."));
    AddWidget(hudPath, "Pause World Dim: %.0f%%", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPauseDim")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPauseWorldSpace", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(90.0f)
                     .DefaultValue(40.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Darkens the frozen world behind the pages so they read. 0 = no dim."));
    AddWidget(hudPath, "Pause HUD Canvas Width: %.0f%%", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPauseHudWidth")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPauseWorldSpace", 1); })
        .Options(FloatSliderOptions()
                     .Min(50.0f)
                     .Max(400.0f)
                     .DefaultValue(130.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Width of the screen the HUD (hearts, magic, buttons, rupees) sits on "
                              "while paused. 100 = the original TV frame around the front page (default 130). "
                              "Elements stay pinned to their corners and keep their size."));
    AddWidget(hudPath, "Pause HUD Canvas Height: %.0f%%", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPauseHudHeight")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPauseWorldSpace", 1); })
        .Options(FloatSliderOptions()
                     .Min(50.0f)
                     .Max(400.0f)
                     .DefaultValue(100.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Height of the screen the HUD sits on while paused. 100 = the original "
                              "TV frame. Elements stay pinned to their corners and keep their size."));
    AddWidget(hudPath, "Pause HUD Element Size: %.0f%%", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrPauseHudScale")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPauseWorldSpace", 1); })
        .Options(FloatSliderOptions()
                     .Min(25.0f)
                     .Max(300.0f)
                     .DefaultValue(100.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Size of the HUD elements while paused (their gap from the corner "
                              "scales with them)."));

    // ---------------------------------------------------------------- Performance
    // --------------------------------------------------------------- Wrist HUD
    AddSidebarEntry("VR Settings", "Wrist HUD", 1);
    WidgetPath wristPath = { "VR Settings", "Wrist HUD", SECTION_COLUMN_1 };
    AddWidget(wristPath, "VrWristHudEditor", WIDGET_CUSTOM).CustomFunction(VrWristHudEditor).HideInSearch(true);

    AddSidebarEntry("VR Settings", "Performance", 2);
    WidgetPath perfPath = { "VR Settings", "Performance", SECTION_COLUMN_1 };

    AddWidget(perfPath, "Render Cost", WIDGET_SEPARATOR_TEXT);
    AddWidget(perfPath, "The game's display list is walked once per eye, every frame. At 120 Hz "
                       "that is 240 full traversals a second on one thread, plus the HUD. These "
                       "settings trade world-update rate for headroom; head tracking always stays "
                       "at the headset's full rate because skipped frames are reprojected by the "
                       "compositor.",
              WIDGET_TEXT);
    AddWidget(perfPath, "Stereo Render Divisor: %d", WIDGET_CVAR_SLIDER_INT)
        .CVar("gVrStereoDivisor")
        .Options(IntSliderOptions()
                     .Min(1)
                     .Max(4)
                     .DefaultValue(1)
                     .Format("%d")
                     .Tooltip("Redraw the stereo pair every Nth frame; in between, the previous "
                              "images are resubmitted with the pose they were drawn from and the "
                              "compositor reprojects them onto your live head pose. 2 roughly "
                              "halves render cost. The source animation is 20 fps, so the drop "
                              "from 120 to 60 world updates is hard to see; head tracking is "
                              "unaffected."));
    AddWidget(perfPath, "Draw HUD Once Per Game Tick", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrHudPerTick")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("The overlay display list is rebuilt once per 20 Hz game tick, so "
                              "redrawing it on every interpolated sub-frame renders identical "
                              "content up to six times. Off = redraw every frame (only useful if "
                              "something in the HUD looks like it is updating too slowly)."));
    AddWidget(perfPath, "Companion Window Divisor: %d", WIDGET_CVAR_SLIDER_INT)
        .CVar("gVrDesktopViewDivisor")
        .Options(IntSliderOptions()
                     .Min(1)
                     .Max(16)
                     .DefaultValue(4)
                     .Format("%d")
                     .Tooltip("How often the desktop window is updated, in frames. Each update "
                              "costs an ImGui frame, a full-eye-resolution mirror copy and a "
                              "Present, all on the critical path. 4 gives roughly 30 fps on the "
                              "monitor at a 120 Hz headset. This menu updates at that rate too."));
    AddWidget(perfPath, "Render Resolution Scale: %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrResolutionScale")
        .Options(FloatSliderOptions()
                     .Min(0.5f)
                     .Max(1.5f)
                     .DefaultValue(1.0f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("Multiplier on the runtime's recommended per-eye resolution. This is "
                              "the GPU-side knob. Applied when the OpenXR session is created, so "
                              "it takes effect on the next restart."));

    perfPath.column = SECTION_COLUMN_2;
    AddWidget(perfPath, "Live Frame Cost", WIDGET_SEPARATOR_TEXT);
    AddWidget(perfPath, "VRPerformanceReadout", WIDGET_CUSTOM).CustomFunction(VrPerformanceReadout).HideInSearch(true);

    AddWidget(perfPath, "Settings That Do Nothing In VR", WIDGET_SEPARATOR_TEXT);
    AddWidget(perfPath, "MSAA and Internal Resolution (under Settings > Graphics) have no effect "
                       "while VR is active: the eyes render into OpenXR swapchains, which are "
                       "created single-sampled and at the size set by Render Resolution Scale "
                       "above. Turning them up costs memory and changes nothing you can see.",
              WIDGET_TEXT);
    AddWidget(perfPath, "V-Sync is bypassed in VR: the headset's compositor paces frames, and a "
                       "second pacer running off the monitor's refresh rate would fight it.",
              WIDGET_TEXT);

    // ------------------------------------------------------------------ VR Inputs
    AddSidebarEntry("VR Settings", "VR Inputs", 1);
    WidgetPath buttonsPath = { "VR Settings", "VR Inputs", SECTION_COLUMN_1 };

    AddWidget(buttonsPath, "VrInputBindings", WIDGET_CUSTOM).CustomFunction(VrInputBindings).HideInSearch(true);

    AddWidget(buttonsPath, "Item Select (Half-Life: Alyx Style)", WIDGET_SEPARATOR_TEXT);
    AddWidget(buttonsPath, "Alyx-Style Item Select", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrItemSelect")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Hold the selector input and a compass of your equipped items "
                              "appears at your hand: flick toward one and release to take it. "
                              "Up = sword & shield, left/right/down = your three C items, "
                              "release without moving = empty hands. Haptic tick marks each "
                              "highlight.\n\n"
                              "This also changes how items are USED. The selector equips; the "
                              "TRIGGER of the hand the item ended up in fires it — squeeze to "
                              "draw the bow, let go to loose. Buttons can no longer pull items "
                              "out or draw the sword (that is the selector's job), and both "
                              "triggers are reserved, so this mode has its own binding set with "
                              "Z-target and the rest moved onto the grips and face buttons. "
                              "Turning it off restores the classic scheme and its bindings "
                              "exactly as you left them."));
    // Developer: the sandbox for in-development physical-item work (bombs/nuts, archery, and
    // whatever item lands next), followed by the one-time calibration. Kept apart from the
    // stable settings so the mess stays contained while items are being tuned.
    AddSidebarEntry("VR Settings", "Developer", 1);
    WidgetPath devPath = { "VR Settings", "Developer", SECTION_COLUMN_1 };
    AddWidget(devPath, "Renderer", WIDGET_SEPARATOR_TEXT);
    AddWidget(devPath, "Graphics Test Card", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrGfxTestCard")
        .Options(CheckboxOptions()
                     .DefaultValue(false)
                     .Tooltip("Draws a test card over every VR image (both eyes, HUD, text panel, menu panel) "
                              "and the desktop mirror: red top-left, green top-right, blue bottom-left, white "
                              "bottom-right, a 16-step grey ramp along the top and an arrow pointing up. "
                              "Shows at a glance whether each image is upright and whether the greys match "
                              "between renderers."));
    AddWidget(devPath, "OpenGL: Flip Wrist/Text Crop", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrGlSubImageYUp")
        .Options(CheckboxOptions()
                     .DefaultValue(false)
                     .Tooltip("OpenGL VR only. If the wrist panels or the text panel show the wrong part of "
                              "their image (with the test card on: blue corner where red should be), toggle "
                              "this. Takes effect immediately."));
    AddWidget(devPath, "Bombs, Nuts & Bombchus", WIDGET_SEPARATOR_TEXT);
    AddWidget(devPath, "Physical Bomb and Nut Throws", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysicalItemThrows")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Selecting a bomb or nut shows a preview in front of you. Reach toward it "
                              "with your sword hand and squeeze grip to grab; release grip to throw. "
                              "Switching equipment drops the held item without a throw impulse. "
                              "Disable for button-operated use after selection."));
    AddWidget(devPath, "Physical Bombchu Drop", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysBombchuDrop")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Selecting a bombchu shows a preview in front of you. Grab it with grip "
                              "(the fuse starts then, as in the base game) and let go anywhere: it "
                              "drops straight down from your hand, lands, and crawls off in the "
                              "direction the controller was pointing. No throw, however fast your "
                              "hand moved. Needs Physical Bomb and Nut Throws on. Disable for the "
                              "base game's put-down after selection."));
    AddWidget(devPath, "Bombchu Preview Size: %.0f%%", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrChuPreviewScale")
        .Options(FloatSliderOptions()
                     .Min(10.0f)
                     .Max(100.0f)
                     .DefaultValue(50.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Size of the presented bombchu as a percent of a real one. You grab it "
                              "with the orientation it shows; it then rides your hand exactly as taken."));
    AddWidget(devPath, "Boomerang", WIDGET_SEPARATOR_TEXT);
    AddWidget(devPath, "Physical Boomerang", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysBoomerang")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Selecting the boomerang shows a small boomerang in the pocket in front of "
                              "you. Grab it with grip (either hand), flick and let go to throw it the "
                              "way your hand moved; it flies, stuns and fetches as in the base game and "
                              "comes back to your HAND. Close your grip as it arrives to catch it "
                              "(ready to throw again); leave the hand open and it returns to the "
                              "pocket. Letting go without moving, switching items or the sword chord "
                              "never throw it. Disable for the base game's trigger aim-and-throw."));
    AddWidget(devPath, "Boomerang Pocket Size: %.0f%%", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBoomerangPreviewScale")
        .Options(FloatSliderOptions()
                     .Min(10.0f)
                     .Max(100.0f)
                     .DefaultValue(50.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Size of the presented boomerang as a percent of the real one. You grab it "
                              "with the orientation it shows; it then rides your hand exactly as taken."));
    AddWidget(devPath, "Boomerang Throw Speed: %.1f m/s", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBoomerangThrowMinSpeed")
        .Options(FloatSliderOptions()
                     .Min(0.2f)
                     .Max(3.0f)
                     .DefaultValue(1.0f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("How fast the hand must be moving when you let go for it to count as a "
                              "throw. Slower releases put the boomerang back in the pocket."));
    AddWidget(devPath, "Boomerang Catch Radius: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBoomerangCatchRadius")
        .Options(FloatSliderOptions()
                     .Min(5.0f)
                     .Max(60.0f)
                     .DefaultValue(20.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How close to the catch hand the returning boomerang must pass to arrive. "
                              "Grip closed at that moment = caught into the hand; grip open = it "
                              "vanishes and the pocket shows it again."));
    AddWidget(devPath, "VrBoomerangReadout", WIDGET_CUSTOM).CustomFunction(VrBoomerangReadout).HideInSearch(true);
    AddWidget(devPath, "Megaton Hammer", WIDGET_SEPARATOR_TEXT);
    AddWidget(devPath, "Physical Hammer", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysHammer")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("The hammer swings with your arms and has real weight: the head trails your "
                              "hands and carries through, and it stops dead on walls, floors and bodies. "
                              "One-handed it is slow and droops; squeeze grip with your other hand on the "
                              "handle to hold it with both for control and power. Slam the head into the "
                              "floor hard enough for the ground pound. Disable for the base game's button "
                              "swings."));
    AddWidget(devPath, "Hammer One-Hand Torque: %.0f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHammer1HAccel")
        .Options(FloatSliderOptions()
                     .Min(20.0f)
                     .Max(600.0f)
                     .DefaultValue(80.0f)
                     .Step(5.0f)
                     .Format("%.0f")
                     .Tooltip("How hard one hand can spin the head up and stop it (rad/s^2). This is the "
                              "weight: lower = the head trails further behind your hand and carries further "
                              "past where you stop."));
    AddWidget(devPath, "Hammer One-Hand Grip Stiffness: %.1f Hz", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHammer1HFreq")
        .Options(FloatSliderOptions()
                     .Min(2.0f)
                     .Max(20.0f)
                     .DefaultValue(6.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("How firmly one hand pulls the head back in line with the wrist. Lower = a "
                              "looser, wobblier grip."));
    AddWidget(devPath, "Hammer One-Hand Follow-Through: %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHammer1HZeta")
        .Options(FloatSliderOptions()
                     .Min(0.3f)
                     .Max(1.2f)
                     .DefaultValue(0.7f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("Damping of the one-hand grip. Lower = the head swings past and rocks back "
                              "when you stop; 1.0 and above = it settles without overshoot."));
    AddWidget(devPath, "Hammer One-Hand Droop: %.1fx", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHammer1HDroop")
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(8.0f)
                     .DefaultValue(2.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Gravity on the head held in one hand, as a multiple of real gravity. Higher "
                              "= the head sags lower when you hold the handle level."));
    AddWidget(devPath, "Hammer Two-Hand Torque: %.0f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHammer2HAccel")
        .Options(FloatSliderOptions()
                     .Min(40.0f)
                     .Max(1500.0f)
                     .DefaultValue(200.0f)
                     .Step(10.0f)
                     .Format("%.0f")
                     .Tooltip("How hard two hands can spin the head up and stop it (rad/s^2). Higher = "
                              "quicker, more controlled swings."));
    AddWidget(devPath, "Hammer Two-Hand Grip Stiffness: %.1f Hz", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHammer2HFreq")
        .Options(FloatSliderOptions()
                     .Min(3.0f)
                     .Max(30.0f)
                     .DefaultValue(9.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("How firmly two hands hold the head in line with the handle."));
    AddWidget(devPath, "Hammer Two-Hand Follow-Through: %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHammer2HZeta")
        .Options(FloatSliderOptions()
                     .Min(0.3f)
                     .Max(1.2f)
                     .DefaultValue(0.8f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("Damping of the two-hand grip. Lower = more swing-past when you stop."));
    AddWidget(devPath, "Hammer Two-Hand Droop: %.1fx", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHammer2HDroop")
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(8.0f)
                     .DefaultValue(1.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Gravity on the head held in both hands, as a multiple of real gravity."));
    AddWidget(devPath, "Hammer Off-Hand Reach: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHammerGripReach")
        .Options(FloatSliderOptions()
                     .Min(4.0f)
                     .Max(25.0f)
                     .DefaultValue(10.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How close to the handle the off hand must be for a grip squeeze to take "
                              "hold (a light tick marks entering reach). Inside this range the off hand's "
                              "grip does nothing else."));
    AddWidget(devPath, "Hammer Hit Speed: %.1f m/s", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHammerHitSpeed")
        .Options(FloatSliderOptions()
                     .Min(1.0f)
                     .Max(8.0f)
                     .DefaultValue(3.0f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("How fast the HEAD must be moving for a blow to deal damage (enemies, "
                              "rusted switches, boulders). Measured on the simulated head, so its weight "
                              "counts."));
    AddWidget(devPath, "Hammer Heavy Speed: %.1f m/s", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHammerHeavySpeed")
        .Options(FloatSliderOptions()
                     .Min(2.0f)
                     .Max(12.0f)
                     .DefaultValue(5.5f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("Head speed for the heavy blow (the base game's jump-attack hammer damage)."));
    AddWidget(devPath, "Hammer Ground Pound Speed: %.1f m/s", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHammerPoundSpeed")
        .Options(FloatSliderOptions()
                     .Min(0.5f)
                     .Max(8.0f)
                     .DefaultValue(2.5f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("How fast the head must be moving INTO a floor for the ground pound "
                              "(shockwave, quake, and the thump that flips Tektites and stuns scrubs). "
                              "The same speed into a wall gives the base game's wall strike."));
    AddWidget(devPath, "VrHammerReadout", WIDGET_CUSTOM).CustomFunction(VrHammerReadout).HideInSearch(true);
    AddWidget(devPath, "Carrying", WIDGET_SEPARATOR_TEXT);
    AddWidget(devPath, "Physical Carrying", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysCarry")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Pots, rocks, bushes, crates, bomb flowers, bombs on the ground and cuccos: put "
                              "your hand on one and squeeze grip to pick it up on the spot. Hold it in one "
                              "hand or both (squeeze with the other hand on it), open your hand to throw it "
                              "with your arm. Everything about the object itself (what it reveals, breaking, "
                              "the cucco glide) is the base game. The A button always still lifts everything the "
                              "original way; uncheck this to lift ONLY with A (like the original)."));
    AddWidget(devPath, "Physical Heavy Lifting", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysCarryHeavy")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Silver-gauntlet boulders and golden-gauntlet pillars by hand. Boulder: grip it "
                              "with one hand (it latches), then the other - it lifts, trails your hands like "
                              "something heavy, and Link can't move until you open a grip to throw it. "
                              "Pillar: both hands on its face and both grips, Link hoists it, it stays "
                              "overhead until you let go, then flies exactly as in the base game. "
                              "Off: lift them with the A button, like the original."));
    AddWidget(devPath, "Heavy Throw Strength: %.2fx", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrCarryHeavyThrowScale")
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger("gVrPhysCarry", 1) || !CVarGetInteger("gVrPhysCarryHeavy", 1);
        })
        .Options(FloatSliderOptions()
                     .Min(0.5f)
                     .Max(3.0f)
                     .DefaultValue(1.0f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("1.00 = real physics for the boulder (it never leaves faster than the base "
                              "game's own heave)."));
    AddWidget(devPath, "Heavy Follow: %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrCarryHeavyFollow")
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger("gVrPhysCarry", 1) || !CVarGetInteger("gVrPhysCarryHeavy", 1);
        })
        .Options(FloatSliderOptions()
                     .Min(0.05f)
                     .Max(1.0f)
                     .DefaultValue(0.3f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("How fast a held boulder catches up with your hands each tick. Lower = heavier "
                              "(more lag); 1 = rigid."));
    AddWidget(devPath, "Carry Grab Reach: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrCarryReachCm")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysCarry", 1); })
        .Options(FloatSliderOptions()
                     .Min(2.0f)
                     .Max(30.0f)
                     .DefaultValue(10.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How close your hand must be to the object for a grip to pick it up."));
    AddWidget(devPath, "Carry Throw Strength: %.2fx", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrCarryThrowScale")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysCarry", 1); })
        .Options(FloatSliderOptions()
                     .Min(0.5f)
                     .Max(3.0f)
                     .DefaultValue(1.0f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("1.00 = real physics: a throw lands where the same throw would in real life "
                              "(matched to each object's gravity). Raise for an arcade arm."));
    AddWidget(devPath, "Carry Throw Cap: %.0f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrCarryThrowMax")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysCarry", 1); })
        .Options(FloatSliderOptions()
                     .Min(10.0f)
                     .Max(60.0f)
                     .DefaultValue(25.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Fastest a thrown object can leave your hand (game units per tick; the base "
                              "game's throw is about 14)."));
    AddWidget(devPath, "Held Objects at Headset Rate", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrCarryLiveDraw")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysCarry", 1); })
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Draw what you hold welded to your live hand, so it moves with it at full headset rate "
            "instead of trailing at the game's 20 updates a second."));
    AddWidget(devPath, "VrCarryReadout", WIDGET_CUSTOM)
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrPhysCarry", 1); })
        .CustomFunction([](WidgetInfo& info) {
            VrCarryDebug d;
            VrCarry_GetDebug(&d);
            if (d.gate != 0) {
                ImGui::TextUnformatted(d.gate == 1 ? "Carry: off" : "Carry: inactive (selector play only, no horse/water/minigame)");
                return;
            }
            if (d.holding) {
                ImGui::Text("Carry: HOLDING with %s", d.hands == 3 ? "both hands" : (d.hands == 1 ? "left hand" : "right hand"));
            } else if (d.nearestCm >= 0.0f) {
                ImGui::Text("Carry: liftable %.0f cm from your hand: squeeze grip", d.nearestCm);
            } else {
                ImGui::TextUnformatted("Carry: nothing offered in reach");
            }
            if (d.lastReleaseMps >= 0.0f) {
                ImGui::Text("Last release: %s at %.1f m/s", d.lastRelease ? "THROWN" : "dropped", d.lastReleaseMps);
            }
            if (d.heavy > 0) {
                ImGui::Text("Boulder: %s", d.heavy == 3   ? "LIFTED (open a grip to throw)"
                                           : d.heavy == 2 ? "both hands latched (needs Silver Gauntlets)"
                                                          : "one hand latched: grip it with the other");
            }
        })
        .HideInSearch(true);
    AddWidget(devPath, "Blocks", WIDGET_SEPARATOR_TEXT);
    AddWidget(devPath, "Physical Block Pushing", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysBlockPush")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Put both hands on a push block and squeeze both grips to grab it. Press your "
                              "hands in to push, draw them back to pull, open either grip to let go. The "
                              "block moves at the original speed and rhythm BY DESIGN — however hard you "
                              "shove — so it keeps its weight. The stick and the A button still work as in "
                              "the base game. Disable for the A-button grab only."));
    AddWidget(devPath, "Block Grab Reach: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBlockGrabReach")
        .Options(FloatSliderOptions()
                     .Min(5.0f)
                     .Max(30.0f)
                     .DefaultValue(15.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How close to the block's face a hand must be to count as on it (a light tick "
                              "marks arriving). Both hands on, then squeeze both grips."));
    AddWidget(devPath, "Block Push Pressure: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBlockPushCm")
        .Options(FloatSliderOptions()
                     .Min(3.0f)
                     .Max(20.0f)
                     .DefaultValue(8.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How far you press your hands in (or draw them back) from where you grabbed "
                              "before the block moves. Keep them there to keep it moving; it eases off at "
                              "half this distance."));
    AddWidget(devPath, "Block Pull-Off Distance: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBlockPullOff")
        .Options(FloatSliderOptions()
                     .Min(15.0f)
                     .Max(80.0f)
                     .DefaultValue(35.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Dragging either hand this far from where it took hold lets go of the block, "
                              "even with the grips still closed."));
    AddWidget(devPath, "Pin Hands to the Block", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrBlockPinHands")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("While you hold a block your hands are drawn on its face and ride it as it "
                              "moves, instead of following the controllers."));
    AddWidget(devPath, "Block Haptics: %d%%", WIDGET_CVAR_SLIDER_INT)
        .CVar("gVrBlockHaptics")
        .Options(IntSliderOptions()
                     .Min(0)
                     .Max(200)
                     .DefaultValue(100)
                     .Step(10)
                     .Format("%d")
                     .Tooltip("Strength of the grab thunk, the slide rumble and the thunk of each step."));
    AddWidget(devPath, "VrBlockReadout", WIDGET_CUSTOM).CustomFunction(VrBlockReadout).HideInSearch(true);
    AddWidget(devPath, "Climbing", WIDGET_SEPARATOR_TEXT);
    AddWidget(devPath, "Physical Climbing", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysClimb")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Ladders, vines, climbable fences and rock: put a hand on one and squeeze its "
                              "grip to take hold, then pull yourself along hand over hand: up, down, sideways, "
                              "and closer to or away from the wall. Let go of everything near the top and Link "
                              "climbs over like in the base game; let go anywhere else and you drop, keeping "
                              "some of your swing. With no hand holding, the stick and A climb as in the base "
                              "game. Disable for the base game's climbing only."));
    AddWidget(devPath, "Climb by Walking Into Ladders/Vines", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrClimbWalkIn")
        .Options(CheckboxOptions()
                     .DefaultValue(false)
                     .Tooltip("Walking into a ladder or vines with the stick starts climbing by itself, as in "
                              "the base game. Off: you take hold with your hands (walking off a ledge onto "
                              "vines still catches you)."));
    AddWidget(devPath, "Climb Grab Reach: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrClimbGrabReach")
        .Options(FloatSliderOptions()
                     .Min(2.0f)
                     .Max(60.0f)
                     .DefaultValue(25.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How close the nearest part of your hand (fingertips, palm or wrist) must be to a "
                              "climbable surface to take hold of it (a light tick marks being in reach). Your "
                              "hand then snaps onto the surface."));
    AddWidget(devPath, "Snap Hands onto the Wall", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrClimbSnapHands")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("While a hand holds, it is drawn right on the surface where it took hold instead "
                              "of exactly at the controller (which may be a little short of it or into it)."));
    AddWidget(devPath, "Climb-Over Window: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrClimbTopWindowCm")
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(100.0f)
                     .DefaultValue(30.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Letting go anywhere above the height the base game climbs over from (about eye "
                              "level with the top), or within this distance below it, climbs you over the "
                              "top. Lower than that you drop. You can pull yourself up past it to the end of "
                              "the climbable surface."));
    AddWidget(devPath, "Release Momentum: %d%%", WIDGET_CVAR_SLIDER_INT)
        .CVar("gVrClimbMomentum")
        .Options(IntSliderOptions()
                     .Min(0)
                     .Max(150)
                     .DefaultValue(70)
                     .Step(5)
                     .Format("%d")
                     .Tooltip("How much of your body's motion you keep when you let go mid-climb: pull hard "
                              "and let go to toss yourself up a little. 0 = just drop."));
    AddWidget(devPath, "Max Toss Height: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrClimbTossCm")
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(150.0f)
                     .DefaultValue(35.0f)
                     .Step(5.0f)
                     .Format("%.0f")
                     .Tooltip("The highest a let-go toss can lift you, however hard you pull (real "
                              "centimetres, the same as child and adult). Sideways tosses are capped at "
                              "the same speed."));
    AddWidget(devPath, "Climb Haptics: %d%%", WIDGET_CVAR_SLIDER_INT)
        .CVar("gVrClimbHaptics")
        .Options(IntSliderOptions()
                     .Min(0)
                     .Max(200)
                     .DefaultValue(100)
                     .Step(10)
                     .Format("%d")
                     .Tooltip("Strength of the touch tick, the grab, the rung ticks while you climb, the bump "
                              "at the end of the climb and the let-go."));
    AddWidget(devPath, "VrClimbReadout", WIDGET_CUSTOM).CustomFunction(VrClimbReadout).HideInSearch(true);
    AddWidget(devPath, "Hookshot", WIDGET_SEPARATOR_TEXT);
    AddWidget(devPath, "Hookshot in Dominant Hand", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrHookshotSwordHand")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Hold the hookshot / longshot in your sword hand (the right controller, or "
                              "the left in left-handed mode): it aims where that controller points and "
                              "its trigger fires it. Disable to keep it in the off hand with the bow."));
    AddWidget(devPath, "Hookshot Laser Pointer", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrHookshotLaser")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("While the hookshot / longshot is in your hand, a thin beam shows exactly "
                              "where the hook will fly, out to its reach. Green: the surface it ends on "
                              "takes the hook. Red: the hook would bounce off. A beam that fades out "
                              "means nothing is in reach."));
    AddWidget(devPath, "Aim Along Hookshot Barrel", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrHookshotBarrelAim")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("The hook sits in the hookshot's barrel, turns with it, and flies exactly "
                              "where the hookshot model points (the laser follows). Disable to aim along "
                              "the controller's pointing ray instead. The sliders below fine-tune "
                              "whichever is active - set them back to 0 when switching."));
    AddWidget(devPath, "Hookshot Aim Pitch: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHookshotAimPitch")
        .Options(FloatSliderOptions()
                     .Min(-180.0f)
                     .Max(180.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Tilt the hookshot's shot up (+) or down (-) relative to the controller. "
                              "Hookshot only, added on top of Calibration > Weapon Aim Trim. The laser "
                              "and reticle follow it."));
    AddWidget(devPath, "Hookshot Aim Yaw: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHookshotAimYaw")
        .Options(FloatSliderOptions()
                     .Min(-180.0f)
                     .Max(180.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Turn the hookshot's shot right (+) or left (-). Mirrored when the hookshot "
                              "is in the left controller."));
    AddWidget(devPath, "Slingshot & Bow", WIDGET_SEPARATOR_TEXT);
    AddWidget(devPath, "Physical Archery", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysArchery")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Real two-hand archery for the slingshot and bow: bring your free "
                              "hand to the weapon and squeeze its trigger to nock, pull back to "
                              "draw, release to fire along the line between your hands. Releasing "
                              "with almost no draw cancels the shot and keeps the ammo. Disable "
                              "for the classic scheme (weapon-hand trigger draws and fires). "
                              "Shooting galleries and horseback keep their own controls."));
    AddWidget(devPath, "Slingshot Nock Reach: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrArcheryNockRadius")
        .Options(FloatSliderOptions()
                     .Min(5.0f)
                     .Max(40.0f)
                     .DefaultValue(20.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How close the string hand must be to the slingshot's nock point to nock."));
    AddWidget(devPath, "Slingshot Minimum Draw: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrArcheryMinDraw")
        .Options(FloatSliderOptions()
                     .Min(2.0f)
                     .Max(30.0f)
                     .DefaultValue(10.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Releasing the string with less draw than this cancels instead of "
                              "firing - no ammo or magic is spent."));
    AddWidget(devPath, "Show Nock Icon", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrArcheryShowIcon")
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Shows a small Deku Nut where the string is nocked on the bow and slingshot, so you "
            "can see where to pinch. Off by default: the string itself is the target."));
    AddWidget(devPath, "Nock Icon Size: %.0f%%", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrArcheryIconScale")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrArcheryShowIcon", 0); })
        .Options(FloatSliderOptions()
                     .Min(3.0f)
                     .Max(100.0f)
                     .DefaultValue(25.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Size of the Deku Nut nock-point icon, as a percent of a normal "
                              "nut drop. Make it as tiny as you like; it still grows slightly "
                              "when your string hand is in pinch reach."));
    AddWidget(devPath, "Fairy Bow", WIDGET_SEPARATOR_TEXT);
    AddWidget(devPath, "Bow Nock Reach: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBowNockRadius")
        .Options(FloatSliderOptions()
                     .Min(5.0f)
                     .Max(40.0f)
                     .DefaultValue(20.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How close your string hand must be to the bowstring (the nut icon "
                              "just behind your bow fist) to nock an arrow."));
    AddWidget(devPath, "Bow Full Draw: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBowFullDraw")
        .Options(FloatSliderOptions()
                     .Min(20.0f)
                     .Max(80.0f)
                     .DefaultValue(45.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How far you pull the string back (past its resting spot) for a full "
                              "draw: full power and the firm pulse in both hands. The string stops "
                              "stretching a little past this. Shorter arms: lower it."));
    AddWidget(devPath, "Bow Minimum Draw: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBowMinDraw")
        .Options(FloatSliderOptions()
                     .Min(2.0f)
                     .Max(30.0f)
                     .DefaultValue(8.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Letting go of the string with less draw than this puts the arrow "
                              "away instead of firing - no arrow or magic is spent."));
    AddWidget(devPath, "Bow Draw Sets Arrow Power", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrBowDrawPower")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("A full draw fires the normal arrow; a partial draw fires a slower "
                              "arrow that drops sooner. Disable to make every shot full power."));
    AddWidget(devPath, "Bow Aims Along Draw", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrBowAlign")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("While the string is drawn, the bow turns to point along the line "
                              "from your string hand through the bow grip, so the arrow always "
                              "points straight out of the bow wherever you pull. Your wrist's "
                              "tilt is kept. Disable to keep the bow fixed to your controller."));
    AddWidget(devPath, "Bottle", WIDGET_SEPARATOR_TEXT);
    AddWidget(devPath, "Physical Bottle Scooping", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysBottleScoop")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("With an empty bottle in hand, move its mouth into a fairy, fish, bug "
                              "or blue fire to catch it - no swing, no button. The bottle-hand "
                              "trigger no longer swings. Disable for the classic scheme (trigger "
                              "swings, catch during the animation). Drinking and pouring are "
                              "unchanged either way."));
    AddWidget(devPath, "Show Bottle Mouth Marker", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrBottleShowMouth")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Draws a tiny Deku Nut at the point the game treats as the bottle "
                              "mouth: the centre of the bottle model's rim, so it rides the "
                              "opening at any world scale and either age. It grows when a "
                              "catchable is within reach."));
    AddWidget(devPath, "Bottle Catch Radius: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBottleCatchRadius")
        .Options(FloatSliderOptions()
                     .Min(5.0f)
                     .Max(40.0f)
                     .DefaultValue(20.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How close the bottle mouth must pass to a fairy, fish or bug to "
                              "catch it (blue fire uses its flame column plus this padding). "
                              "Larger is more forgiving."));
    AddWidget(devPath, "Bottle Marker Size: %.0f%%", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBottleIconScale")
        .Options(FloatSliderOptions()
                     .Min(3.0f)
                     .Max(100.0f)
                     .DefaultValue(12.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Size of the bottle mouth marker as a percent of a normal nut drop."));
    AddWidget(devPath, "Physical Bottle Pouring", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysBottlePour")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Hold a bottle upside down and shake it three times to let out a "
                              "fish, bug, blue fire or fairy. Each valid shake buzzes the hand; "
                              "the third empties the bottle through the normal use action. "
                              "Potions and milk are drunk, not poured. The trigger still works "
                              "for contents either way."));
    AddWidget(devPath, "Bottle Upside-Down Angle: %.0f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBottleInvertDeg")
        .Options(FloatSliderOptions()
                     .Min(20.0f)
                     .Max(80.0f)
                     .DefaultValue(50.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How far from straight down the bottle mouth may point and still "
                              "count as upside down for pouring. Larger is more lenient."));
    AddWidget(devPath, "Bottle Shake Size: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBottleShakeAmplitude")
        .Options(FloatSliderOptions()
                     .Min(2.0f)
                     .Max(25.0f)
                     .DefaultValue(9.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How far the bottle mouth must travel downward in one stroke for "
                              "it to count as a shake. Distance, not speed: slow shakes count. "
                              "Lower if shakes are missed, raise if the bottle empties from "
                              "ordinary movement."));
    AddWidget(devPath, "Bottle Re-catch Delay: %.1f s", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBottleRecatchDelay")
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(5.0f)
                     .DefaultValue(1.5f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("After pouring something out, scooping stays off for this long so "
                              "the mouth (still right on top of it) can't catch it straight back."));
    AddWidget(devPath, "Physical Bottle Drinking", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysBottleDrink")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Bring the bottle mouth (the marker) up to your face to drink a "
                              "potion, milk or poe. The hand buzzes once on arrival and again a "
                              "moment later; the third buzz is the swallow and the effect lands "
                              "right then, with no Link animation. Move it away before the third "
                              "and nothing is spent. The trigger still drinks either way."));
    AddWidget(devPath, "Bottle Drink Distance: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBottleDrinkDistance")
        .Options(FloatSliderOptions()
                     .Min(3.0f)
                     .Max(30.0f)
                     .DefaultValue(10.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How close the bottle mouth must come to your face to count as "
                              "drinking. Small on purpose: raise it if sips are missed, lower it "
                              "if the bottle drinks itself when you look at it."));
    AddWidget(devPath, "Bottle Face Point Below Eyes: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBottleDrinkFaceDown")
        .Options(FloatSliderOptions()
                     .Min(0.0f)
                     .Max(20.0f)
                     .DefaultValue(8.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Where your mouth is, measured straight down from the headset's "
                              "eye point. The drink distance is measured from here."));
    AddWidget(devPath, "Bottle Sip Interval: %.2f s", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrBottleSipInterval")
        .Options(FloatSliderOptions()
                     .Min(0.15f)
                     .Max(1.5f)
                     .DefaultValue(0.45f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("Time between sip buzzes while the bottle stays at your face. Two "
                              "of these after the first buzz is the swallow."));
    AddWidget(devPath, "VrBottlePourReadout", WIDGET_CUSTOM).CustomFunction(VrBottlePourReadout).HideInSearch(true);
    AddWidget(devPath, "Magic", WIDGET_SEPARATOR_TEXT);
    AddWidget(devPath, "Two-Trigger Spell Casting", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysMagicChord")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Din's Fire, Farore's Wind and Nayru's Love cast only while you hold "
                              "BOTH triggers. Link plants his feet and both controllers buzz, "
                              "building until the spell goes off with a strong jolt. Let go of "
                              "either trigger before then and the spell fizzles: no magic spent, "
                              "you can move again. Disable to cast with the one trigger of the "
                              "hand holding the spell."));
    AddWidget(devPath, "Lens of Truth", WIDGET_SEPARATOR_TEXT);
    AddWidget(devPath, "Physical Lens of Truth", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysLens")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Select the Lens of Truth and it appears in front of you. Grab it "
                              "with either grip and hold it up to your face: it attaches and the "
                              "lens turns on (uses magic as usual). Grip it at your face to take "
                              "it off. Switching items takes it off too. Disable to toggle the "
                              "lens with the trigger instead."));
    AddWidget(devPath, "Lens Size In Hand (glass radius, cm)", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrLensRadius")
        .Options(FloatSliderOptions()
                     .Min(2.0f)
                     .Max(12.0f)
                     .DefaultValue(5.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Real size of the lens glass while you hold it (and in the pocket)."));
    AddWidget(devPath, "Lens Size Worn (glass radius, cm)", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrLensWornRadius")
        .Options(FloatSliderOptions()
                     .Min(2.0f)
                     .Max(40.0f)
                     .DefaultValue(6.2f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Size of the lens once it's on your face. Bigger (or closer) = a "
                              "wider view through it."));
    AddWidget(devPath, "Worn Distance (cm)", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrLensDistance")
        .Options(FloatSliderOptions()
                     .Min(2.0f)
                     .Max(60.0f)
                     .DefaultValue(8.6f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("How far in front of your eyes the lens sits once it's on. You still "
                              "put it on by holding it up to your face (within 8 cm)."));
    AddWidget(devPath, "Worn Side Offset (cm)", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrLensSide")
        .Options(FloatSliderOptions()
                     .Min(-6.0f)
                     .Max(6.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("0 = centered between your eyes. About +-3 puts it over one eye "
                              "(positive = right), like a monocle."));
    AddWidget(devPath, "Worn Height Offset (cm)", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrLensHeight")
        .Options(FloatSliderOptions()
                     .Min(-8.0f)
                     .Max(8.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Raise or lower the worn lens relative to your eye line."));
    AddWidget(devPath, "Put-On Distance (cm)", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrLensWearDistance")
        .Options(FloatSliderOptions()
                     .Min(4.0f)
                     .Max(25.0f)
                     .DefaultValue(10.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("The lens attaches once its glass comes this close to your face "
                              "(the worn spot, or 8 cm in front of your eyes if that's further)."));
    AddWidget(devPath, "Lens Pocket Size (%)", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrLensPreviewScale")
        .Options(FloatSliderOptions()
                     .Min(20.0f)
                     .Max(100.0f)
                     .DefaultValue(60.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Size of the lens waiting in front of you, relative to its real "
                              "size in your hand."));
    AddWidget(devPath, "Show Lens Frame While Worn", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrLensShowFrame")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Draw the lens's frame and handle in front of your face while it's "
                              "on. The see-through circle works either way."));
    AddWidget(devPath, "VrLensReadout", WIDGET_CUSTOM).CustomFunction(VrLensReadout).HideInSearch(true);
    AddWidget(devPath, "Masks", WIDGET_SEPARATOR_TEXT);
    AddWidget(devPath, "Physical Masks", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrPhysMasks")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Select a mask and it appears in front of you. Grab it and hold it "
                              "to your face to put it on (it replaces any mask you're wearing). "
                              "It stays on when you switch items. Grip at your face any time to "
                              "take it off. Disable to put masks on and off with the trigger."));
    AddWidget(devPath, "Mask Size (cm)", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrMaskSize")
        .Options(FloatSliderOptions()
                     .Min(10.0f)
                     .Max(40.0f)
                     .DefaultValue(22.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Real height of a typical mask in your hand (each mask keeps its "
                              "own proportions)."));
    AddWidget(devPath, "Mask Put-On Distance (cm)", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrMaskWearDistance")
        .Options(FloatSliderOptions()
                     .Min(5.0f)
                     .Max(25.0f)
                     .DefaultValue(12.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("The mask goes on once its middle comes this close to the front "
                              "of your face."));
    AddWidget(devPath, "Mask Pocket Size (%)", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrMaskPreviewScale")
        .Options(FloatSliderOptions()
                     .Min(20.0f)
                     .Max(100.0f)
                     .DefaultValue(60.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Size of the mask waiting in front of you, relative to its real "
                              "size in your hand."));
    AddWidget(devPath, "VrMaskReadout", WIDGET_CUSTOM).CustomFunction(VrMaskReadout).HideInSearch(true);
    AddWidget(devPath, "Diagnostics", WIDGET_SEPARATOR_TEXT);
    AddWidget(devPath, "Log ReDead Grabs (diagnostic)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrRedeadLog")
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Writes vrredead_log.csv next to the game: one line per tick per nearby ReDead/Gibdo with "
            "everything its notice, freeze and grab checks read. For debugging issue #47; leave off."));
    AddWidget(buttonsPath, "Selector Hand", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrItemSelHand")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrItemSelect", 1); })
        .Options(ComboboxOptions()
                     .DefaultIndex(0)
                     .ComboMap(vrItemSelHandOptions)
                     .Tooltip("Which hand opens the selector and does the flicking. Alyx uses "
                              "the weapon hand."));
    AddWidget(buttonsPath, "Selector Input (Hold)", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrItemSelInput")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrItemSelect", 1); })
        .Options(ComboboxOptions()
                     .DefaultIndex(VR_BTN_THUMBCLICK)
                     .ComboMap(vrItemSelInputOptions)
                     .Tooltip("The input you hold to open the selector. It becomes DEDICATED: "
                              "its normal button binding stops firing, and while held, that "
                              "hand's thumbstick is ignored (no turning or C-buttons from a "
                              "resting thumb)."));
    AddWidget(buttonsPath, "Sword Swap (Both Hands)", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrItemSelSwapInput")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrItemSelect", 1); })
        .Options(ComboboxOptions()
                     .DefaultIndex(VR_BTN_GRIP)
                     .ComboMap(vrItemSelSwapOptions)
                     .Tooltip("Quick swap back to sword & shield: squeeze this input on BOTH "
                              "controllers at once to stow whatever you're holding and draw the "
                              "sword (the shield already rides your off hand). While both are "
                              "down, the inputs' normal bindings pause so the swap doesn't also "
                              "Z-target or raise R; squeezed alone they work as bound."));
    AddWidget(buttonsPath, "Flick Distance: %.0f cm", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrItemSelDistance")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrItemSelect", 1); })
        .Options(FloatSliderOptions()
                     .Min(3.0f)
                     .Max(30.0f)
                     .DefaultValue(5.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("How far your hand must move from where you pressed the input "
                              "for a direction to count. Shorter = snappier, longer = harder "
                              "to pick by accident."));
    AddWidget(buttonsPath, "Click a chip to remove a binding, + to add one. A VR input may press "
                           "several buttons at once. Sticks are fixed: left = movement, right = "
                           "C-buttons/snap turn.",
              WIDGET_TEXT);

    // ---------------------------------------------------------------- Calibration (Developer page)
    WidgetPath calPath = devPath;
    AddWidget(calPath, "Calibration", WIDGET_SEPARATOR_TEXT);

    AddWidget(calPath, "One-time tuning. The defaults were calibrated in-headset; you should not "
                       "need anything here unless the hands, weapon aim or camera look off on "
                       "your setup. Tune, then use the copy button to share your values.",
              WIDGET_TEXT);

    AddWidget(calPath, "Hand Mirroring", WIDGET_SEPARATOR_TEXT);
    AddWidget(calPath, "Mirror Sword Hand", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrHandMirrorSword")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Reflect the sword hand's mesh so it reads as a right hand on the right "
                              "controller. Only applies in right-handed mode."));
    AddWidget(calPath, "Mirror Shield Hand", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrHandMirrorShield")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Reflect the shield hand's mesh so it reads as a left hand on the left "
                              "controller. Note the reflection also mirrors the shield's face design; "
                              "pair with the Left Hand Override values (Comfort & Movement) to orient it correctly."));
    AddWidget(calPath, "Mirror Axis", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrHandMirrorAxis")
        .Options(ComboboxOptions()
                     .DefaultIndex(2)
                     .ComboMap(vrMirrorAxisOptions)
                     .Tooltip("Which model-local axis the mirror reflection negates. Should be the thumb "
                              "axis: it must keep the finger direction and flip the thumb so the mesh reads "
                              "as the opposite hand. Try each if the hands look inside-out."));

    calPath.column = SECTION_COLUMN_2;
    AddWidget(calPath, "Weapon Aim Trim", WIDGET_SEPARATOR_TEXT);
    AddWidget(calPath, "Aim Pitch: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrAimCalPitch")
        .Options(FloatSliderOptions()
                     .Min(-45.0f)
                     .Max(45.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Tilt the aim ray up/down relative to the controller. If shots "
                              "consistently land high or low of where you point, trim it here."));
    AddWidget(calPath, "Aim Yaw: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrAimCalYaw")
        .Options(FloatSliderOptions()
                     .Min(-45.0f)
                     .Max(45.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Skew the aim ray left/right relative to the controller."));
    AddWidget(calPath, "Aim Origin Right: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrAimOffX")
        .Options(FloatSliderOptions()
                     .Min(-0.3f)
                     .Max(0.3f)
                     .DefaultValue(0.0f)
                     .Step(0.01f)
                     .Format("%.2f")
                     .Tooltip("Slide the projectile's launch point sideways along the aim frame "
                              "(meters), e.g. to sit in the slingshot pouch."));
    AddWidget(calPath, "Aim Origin Up: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrAimOffY")
        .Options(FloatSliderOptions().Min(-0.3f).Max(0.3f).DefaultValue(0.0f).Step(0.01f).Format("%.2f"));
    AddWidget(calPath, "Aim Origin Forward: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrAimOffZ")
        .Options(FloatSliderOptions()
                     .Min(-0.3f)
                     .Max(0.3f)
                     .DefaultValue(0.0f)
                     .Step(0.01f)
                     .Format("%.2f")
                     .Tooltip("Push the launch point forward along the ray (negative = toward "
                              "you). Note OpenXR aim forward is -Z, so forward here is negative Z "
                              "in the raw frame - this slider already accounts for that."));

    AddWidget(calPath, "Head Position (relative to Link's body)", WIDGET_SEPARATOR_TEXT);
    AddWidget(calPath, "Forward Offset: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHeadOffsetForward")
        .Options(FloatSliderOptions()
                     .Min(-60.0f)
                     .Max(60.0f)
                     .DefaultValue(0.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Move the eye anchor along Link's facing (game units). Positive pushes "
                              "the camera forward out of his head; negative pulls it back. 0 = centred, "
                              "so turning rotates exactly about your eye; any offset swings the view "
                              "around Link's centre as he turns."));
    AddWidget(calPath, "Side Offset: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHeadOffsetSide")
        .Options(FloatSliderOptions()
                     .Min(-60.0f)
                     .Max(60.0f)
                     .DefaultValue(0.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Move the eye anchor sideways relative to Link's facing (game units)."));

    AddWidget(calPath, "Export", WIDGET_SEPARATOR_TEXT);
    AddWidget(calPath, "Copy All Calibration Values", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip("Copy every VR tuning value (hands, weapon aim, camera, "
                                         "world scale, HUD placement) so they can be handed to a "
                                         "developer to become the defaults."))
        .Callback([](WidgetInfo& info) {
            char buf[2048];
            snprintf(buf, sizeof(buf),
                     "gVrMotionHands=%d\n"
                     "gVrLeftHanded=%d\n"
                     "gVrHandMirrorSword=%d\n"
                     "gVrHandMirrorShield=%d\n"
                     "gVrHandMirrorAxis=%d\n"
                     "gVrHandCalPitch=%.1f\n"
                     "gVrHandCalYaw=%.1f\n"
                     "gVrHandCalRoll=%.1f\n"
                     "gVrHandOffX=%.1f\n"
                     "gVrHandOffY=%.1f\n"
                     "gVrHandOffZ=%.1f\n"
                     "gVrHandLOverride=%d\n"
                     "gVrHandLCalPitch=%.1f\n"
                     "gVrHandLCalYaw=%.1f\n"
                     "gVrHandLCalRoll=%.1f\n"
                     "gVrHandLOffX=%.1f\n"
                     "gVrHandLOffY=%.1f\n"
                     "gVrHandLOffZ=%.1f\n"
                     "gVrAimCalPitch=%.1f\n"
                     "gVrAimCalYaw=%.1f\n"
                     "gVrAimOffX=%.2f\n"
                     "gVrAimOffY=%.2f\n"
                     "gVrAimOffZ=%.2f\n"
                     "gVrHeadHeightOffset=%.1f\n"
                     "gVrHeadOffsetForward=%.1f\n"
                     "gVrHeadOffsetSide=%.1f\n"
                     "gVrWorldScale=%.1f\n"
                     "gVrScreenDistance=%.1f\n"
                     "gVrScreenSize=%.1f\n"
                     "gVrHudDistance=%.1f\n"
                     "gVrHudSize=%.2f\n"
                     "gVrHudOffX=%.2f\n"
                     "gVrHudOffY=%.2f\n",
                     CVarGetInteger("gVrMotionHands", 1), CVarGetInteger("gVrLeftHanded", 0),
                     CVarGetInteger("gVrHandMirrorSword", 1), CVarGetInteger("gVrHandMirrorShield", 1),
                     CVarGetInteger("gVrHandMirrorAxis", 2),
                     CVarGetFloat("gVrHandCalPitch", 88.0f), CVarGetFloat("gVrHandCalYaw", -100.0f),
                     CVarGetFloat("gVrHandCalRoll", 80.0f), CVarGetFloat("gVrHandOffX", 0.0f),
                     CVarGetFloat("gVrHandOffY", 6.3f), CVarGetFloat("gVrHandOffZ", 0.0f),
                     CVarGetInteger("gVrHandLOverride", 1), CVarGetFloat("gVrHandLCalPitch", -149.0f),
                     CVarGetFloat("gVrHandLCalYaw", 76.0f), CVarGetFloat("gVrHandLCalRoll", 30.0f),
                     CVarGetFloat("gVrHandLOffX", 0.0f), CVarGetFloat("gVrHandLOffY", 6.3f),
                     CVarGetFloat("gVrHandLOffZ", 0.0f),
                     CVarGetFloat("gVrAimCalPitch", 0.0f), CVarGetFloat("gVrAimCalYaw", 0.0f),
                     CVarGetFloat("gVrAimOffX", 0.0f), CVarGetFloat("gVrAimOffY", 0.0f),
                     CVarGetFloat("gVrAimOffZ", 0.0f),
                     CVarGetFloat("gVrHeadHeightOffset", -9.0f), CVarGetFloat("gVrHeadOffsetForward", 0.0f),
                     CVarGetFloat("gVrHeadOffsetSide", 0.0f), CVarGetFloat("gVrWorldScale", 35.0f),
                     CVarGetFloat("gVrScreenDistance", 2.2f), CVarGetFloat("gVrScreenSize", 2.4f),
                     CVarGetFloat("gVrHudDistance", 2.0f), CVarGetFloat("gVrHudSize", 1.5f),
                     CVarGetFloat("gVrHudOffX", 0.0f), CVarGetFloat("gVrHudOffY", 0.0f));
            ImGui::SetClipboardText(buf);
        });
}

} // namespace SohGui
