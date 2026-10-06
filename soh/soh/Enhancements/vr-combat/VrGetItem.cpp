extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
}
#include "VrCombat.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <vr_interface.h>
#include <cmath>

// Get-item hold-up placement. Vanilla (Player_DrawGetItemImpl) draws the item over the midpoint
// of Link's raised hands (which follow the controllers in VR), 3.3 units ahead of them along his
// facing: from Link's own eyes that is right on top of you. This keeps the item on the hands and
// only pushes it further OUT, gVrGetItemDistance real centimetres (world scale changes with age
// and player height) horizontally away from the eyes through the hands, so holding the hands
// further out or to the side still moves it, and the height above the hands stays vanilla.

namespace {

float CmToUnits(float cm) {
    float ws = VR_GetWorldScale();
    if (ws < 1.0f) {
        ws = 35.0f;
    }
    return cm * 0.01f * ws;
}

} // namespace

extern "C" bool VrGetItem_HoldUpPos(PlayState* play, Player* player, const float* hands, float* out) {
    if (play == NULL || player == NULL || &player->actor != &GET_PLAYER(play)->actor ||
        player->actor.category != ACTORCAT_PLAYER || !VR_IsInitialized() || !VR_GetFirstPerson() ||
        VR_IsFlatScreen() || !(player->stateFlags1 & PLAYER_STATE1_GETTING_ITEM) ||
        player->exchangeItemId != EXCH_ITEM_NONE) {
        return false;
    }

    // "Out" = horizontally from the eyes through the hands. With the hands (nearly) straight
    // overhead that direction is meaningless, so it eases into Link's facing there.
    float eye[3], fwd[3], up[3];
    VR_GetCameraPose(eye, fwd, up);
    const float blend = CmToUnits(10.0f);
    float dx = hands[0] - eye[0] + Math_SinS(player->actor.shape.rot.y) * blend;
    float dz = hands[2] - eye[2] + Math_CosS(player->actor.shape.rot.y) * blend;
    const float len = sqrtf(dx * dx + dz * dz);
    if (len < 0.001f) {
        return false;
    }
    const float dist = CmToUnits(CVarGetFloat("gVrGetItemDistance", 20.0f));
    out[0] = hands[0] + dx / len * dist;
    out[1] = hands[1];
    out[2] = hands[2] + dz / len * dist;
    return true;
}
