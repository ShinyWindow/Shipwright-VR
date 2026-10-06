#pragma once

// VR cutscene view table (gVrCutsceneThirdPerson): in first person, which camera take-overs play
// from the game's original camera (third person) and which stay in Link's eyes. One checkbox per
// row (gVrCs3P.<key>); z_play.c asks once per drawn frame.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PlayState;

// True while the camera shot running now belongs to a row the player has ticked third person.
bool VrCutsceneView_ThirdPerson(struct PlayState* play);

#ifdef __cplusplus
}

struct VrCutsceneRow {
    const char* group;   // section header; consecutive rows share one
    const char* label;
    const char* cvar;
    bool thirdPerson;    // default
    const char* tooltip;
};

// The table in menu order.
const VrCutsceneRow* VrCutsceneView_Rows(int* count);
#endif
