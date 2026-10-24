extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "overlays/actors/ovl_En_Box/z_en_box.h"
extern PlayState* gPlayState;
void func_8083AE40(Player* player, s16 objectId);
}

#include "VrCombat.h"
#include "soh/Enhancements/enhancementTypes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Enhancements/randomizer/draw.h"
#include "soh/OTRGlobals.h"
#include "soh/ShipInit.hpp"
#include <libultraship/bridge/consolevariablebridge.h>
#include <vr_interface.h>
#include <algorithm>
#include <cmath>
#include <cstring>

// Two-handed physical treasure-chest lids. En_Box remains the authority for chest visibility,
// rewards and save flags; this module only owns the unopened lid and asks the normal player chest
// path to run after the physical motion reaches its end.
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr uint16_t kClosedJoint = 0x8000;
constexpr uint16_t kOpenJoint = 0x34A5;
constexpr float kPhysicalOpenRadians = 106.0f * (kPi / 180.0f);
// Once the player has clearly lifted the lid, releasing it completes the remaining travel. A
// lower balance point keeps the full gesture inside one comfortable shoulder/wrist motion.
constexpr float kCommitAngleRadians = 25.0f * (kPi / 180.0f);
constexpr float kFinishStep = 0.065f;

struct ChestProfile {
    float halfWidth;
    float frontZ;
    float hingeZ;
    float lidY;
    float lidTopY;
    float hoverY;
    float itemScale;
    float grabReach;
    float surfaceClearance;
};

struct ChestHandState {
    EnBox* chest = nullptr;
    uint32_t frame = 0;
    uint32_t contactFrame = 0;
    bool active = false;
    bool latched = false;
    uint8_t trackingMissFrames = 0;
    float score = 1e30f;
    float localX = 0.0f;
    float localY = 0.0f;
    float localZ = 0.0f;
    float tangentY = 0.0f;
    float tangentZ = 1.0f;
    float contactAlong = 0.0f;
};

EnBox* sChest = nullptr;
EnBox* sOpenRequest = nullptr;
ChestHandState sHands[2];
EnBox* sPrevHoverChest[2] = {};
bool sGripNow[2] = {};
uint32_t sInputFrame = UINT32_MAX;
float sStartHandAngle[2] = {};
float sStartOpenFraction = 0.0f;
float sOpenFraction = 0.0f;
bool sOpening = false;
bool sOpenArmed = false;
bool sFinishing = false;
bool sRequestConsumed = false;
bool sSawAwardDraw = false;
bool sPreviewDone = false;
bool sAwardActive = false;
GetItemEntry sAwardEntry = GET_ITEM_NONE;
uint32_t sRevealFrame = 0;
float sFrameHandPos[2][3] = {};
bool sFrameHandValid[2] = {};

struct LidMatrixRecord {
    EnBox* chest = nullptr;
    uint32_t frame = 0;
    uint16_t joint = 0;
    float m[16] = {};
};

constexpr size_t kLidMatrixSlots = 32;
LidMatrixRecord sLidMatrices[kLidMatrixSlots];

bool IsSmall(const EnBox* chest) {
    return chest->type == ENBOX_TYPE_SMALL || chest->type == ENBOX_TYPE_6 ||
           chest->type == ENBOX_TYPE_ROOM_CLEAR_SMALL || chest->type == ENBOX_TYPE_SWITCH_FLAG_FALL_SMALL;
}

ChestProfile Profile(const EnBox* chest) {
    // Small chests use the same model at half scale, so one calibrated large profile can be
    // reduced exactly instead of maintaining age-dependent or scene-dependent dimensions.
    if (IsSmall(chest)) {
        return { 12.625f, -9.615f, 9.91f, 13.665f, 22.375f, 31.0f, 0.20f, 5.0f, 0.375f };
    }
    // Exact object_box collision/model bounds in world units at the actor's 0.01 scale. The lid
    // limb is a half-cylinder. Its real skeletal pivot is Y=27.33, Z=19.82 and its mesh
    // extends 39.05 units toward local -Z, rather than rotating about the collision-box corner.
    return { 25.25f, -19.23f, 19.82f, 27.33f, 44.75f, 58.0f, 0.20f, 10.0f, 0.75f };
}

s16 LidJoint(float fraction) {
    return static_cast<s16>(static_cast<int32_t>(kClosedJoint) +
                            static_cast<int32_t>((static_cast<int32_t>(kOpenJoint) -
                                                  static_cast<int32_t>(kClosedJoint)) * fraction));
}

float LidOpenAngle(const EnBox* chest) {
    if (chest == nullptr) return 0.0f;
    // Derive the relative rotation from the exact skeletal joint that draws the lid. Working in
    // wrapping binary-angle space preserves the -106-degree path across the signed-s16 boundary.
    const uint16_t current = static_cast<uint16_t>(chest->skelanime.jointTable[3].z);
    const int16_t delta = static_cast<int16_t>(current - kClosedJoint);
    return -static_cast<float>(delta) * (2.0f * kPi / 65536.0f);
}

LidMatrixRecord* FindLidMatrix(const EnBox* chest) {
    if (chest == nullptr) return nullptr;
    for (LidMatrixRecord& record : sLidMatrices) {
        if (record.chest == chest) return &record;
    }
    return nullptr;
}

// Return the exact current lid transform. The captured draw matrix may be one update old, so rotate
// its local X/Y columns by the joint delta while retaining the skeleton's real pivot and parent axes.
bool LidTransform(const EnBox* chest, float x[3], float y[3], float z[3], float origin[3]) {
    LidMatrixRecord* record = FindLidMatrix(chest);
    if (record == nullptr || gPlayState == nullptr ||
        gPlayState->state.frames - record->frame > 3) {
        return false;
    }
    const uint16_t current = static_cast<uint16_t>(chest->skelanime.jointTable[3].z);
    const int16_t deltaJoint = static_cast<int16_t>(current - record->joint);
    const float delta = static_cast<float>(deltaJoint) * (2.0f * kPi / 65536.0f);
    const float co = cosf(delta);
    const float si = sinf(delta);
    for (int component = 0; component < 3; ++component) {
        const float oldX = record->m[component];
        const float oldY = record->m[4 + component];
        x[component] = oldX * co + oldY * si;
        y[component] = oldY * co - oldX * si;
        z[component] = record->m[8 + component];
        origin[component] = record->m[12 + component];
    }
    return true;
}

void Clear(bool keepPresentation = false) {
    sHands[0] = {};
    sHands[1] = {};
    sPrevHoverChest[0] = sPrevHoverChest[1] = nullptr;
    sOpening = false;
    sOpenArmed = false;
    sFinishing = false;
    if (!keepPresentation) {
        sChest = nullptr;
        sOpenRequest = nullptr;
        sStartHandAngle[0] = sStartHandAngle[1] = 0.0f;
        sStartOpenFraction = 0.0f;
        sOpenFraction = 0.0f;
        sRequestConsumed = false;
        sSawAwardDraw = false;
        sPreviewDone = false;
        sAwardActive = false;
        sAwardEntry = GET_ITEM_NONE;
        sRevealFrame = 0;
    }
}

void BeginInputFrame(PlayState* play) {
    if (sInputFrame == play->state.frames) return;
    sInputFrame = play->state.frames;
    for (int hand = 0; hand < 2; ++hand) {
        sPrevHoverChest[hand] = sHands[hand].active ? sHands[hand].chest : nullptr;
        if (!sHands[hand].latched) {
            // Retain the previous contact transform briefly; Tick may reuse it through a short
            // distance/tracking miss instead of visibly dropping and reacquiring the hand.
            sHands[hand].active = false;
            sHands[hand].score = 1e30f;
        }
        sGripNow[hand] = (VR_GetControllerButton(hand) & VR_BTN_GRIP) != 0;
        float quat[4];
        sFrameHandValid[hand] = VR_GetHandPose(hand, sFrameHandPos[hand], quat);
    }
}

bool Eligible(EnBox* chest, PlayState* play) {
    if (chest == nullptr || play == nullptr || !CVarGetInteger("gVrPhysicalChests", 0) ||
        !VR_IsInitialized() || !VR_GetFirstPerson() || VR_IsFlatScreen() ||
        !CVarGetInteger("gVrMotionHands", 1) || play->csCtx.state != CS_STATE_IDLE ||
        play->transitionTrigger != TRANS_TRIGGER_OFF || chest->alpha != 255 ||
        Flags_GetTreasure(play, chest->dyna.actor.params & 0x1F)) {
        return false;
    }
    if ((chest->type == ENBOX_TYPE_4 || chest->type == ENBOX_TYPE_6) &&
        (chest->dyna.actor.flags & ACTOR_FLAG_REACT_TO_LENS) && !play->actorCtx.lensActive) {
        return false;
    }
    Player* player = GET_PLAYER(play);
    return player != nullptr && !(player->stateFlags1 &
        (PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_DEAD | PLAYER_STATE1_CARRYING_ACTOR |
         PLAYER_STATE1_HANGING_OFF_LEDGE | PLAYER_STATE1_CLIMBING_LEDGE |
         PLAYER_STATE1_CLIMBING_LADDER | PLAYER_STATE1_IN_CUTSCENE));
}

void Axes(const EnBox* chest, float right[3], float back[3]) {
    const float sn = Math_SinS(chest->dyna.actor.shape.rot.y);
    const float cs = Math_CosS(chest->dyna.actor.shape.rot.y);
    right[0] = cs; right[1] = 0.0f; right[2] = -sn;
    back[0] = sn; back[1] = 0.0f; back[2] = cs;
}

float HandLidPolar(const EnBox* chest, int hand) {
    if (chest == nullptr || hand < 0 || hand > 1 || !sFrameHandValid[hand]) return 0.0f;
    const ChestProfile profile = Profile(chest);
    float right[3], back[3];
    Axes(chest, right, back);
    const float dx = sFrameHandPos[hand][0] - chest->dyna.actor.world.pos.x;
    const float dz = sFrameHandPos[hand][2] - chest->dyna.actor.world.pos.z;
    const float localZ = dx * back[0] + dz * back[2];
    const float localY = sFrameHandPos[hand][1] - chest->dyna.actor.world.pos.y;
    return atan2f(localY - profile.lidY, profile.hingeZ - localZ);
}

float PositiveAngleChange(float current, float start) {
    float change = current - start;
    while (change > kPi) change -= 2.0f * kPi;
    while (change < -kPi) change += 2.0f * kPi;
    return fmaxf(0.0f, change);
}

bool HandContact(const EnBox* chest, int hand, float* outX, float* outY, float* outZ,
                 float* outTangentY, float* outTangentZ, float* outContactAlong,
                 float* outDistance) {
    float m[4][4];
    if (!VR_GetHandMatrix(hand, m)) return false;
    const ChestProfile profile = Profile(chest);
    float axisX[3], axisY[3], axisZ[3], origin[3];
    if (!LidTransform(chest, axisX, axisY, axisZ, origin)) return false;
    const float scaleSq = axisX[0] * axisX[0] + axisX[1] * axisX[1] + axisX[2] * axisX[2];
    if (scaleSq < 0.000001f) return false;
    const float modelScale = sqrtf(scaleSq);
    const float len = VrHand_ChildSized() ? 548.0f : 862.0f;
    float bestSq = 1e30f;
    float bestX = 0.0f, bestY = 0.0f, bestZ = 0.0f;
    float bestTangentX = -1.0f, bestTangentY = 0.0f;
    size_t bestSegment = 0;
    float bestFacetT = 0.0f;

    // The actual lid display list is authored in limb space: X runs from the hinge to the front,
    // Y traces the faceted arch, and Z spans the chest width. These coordinates are transformed by
    // the exact limb-3 draw matrix captured from EnBox_PostLimbDraw.
    static constexpr float roofX[] = { 0.0f, 400.0f, 1352.0f, 2552.0f, 3504.0f, 3905.0f };
    static constexpr float roofY[] = { 0.0f, -1039.0f, -1742.0f, -1742.0f, -1039.0f, 0.0f };
    constexpr float kHalfWidth = 2525.0f;

    auto worldToLid = [&](float wx, float wy, float wz, float out[3]) {
        const float dx = wx - origin[0];
        const float dy = wy - origin[1];
        const float dz = wz - origin[2];
        out[0] = (dx * axisX[0] + dy * axisX[1] + dz * axisX[2]) / scaleSq;
        out[1] = (dx * axisY[0] + dy * axisY[1] + dz * axisY[2]) / scaleSq;
        out[2] = (dx * axisZ[0] + dy * axisZ[1] + dz * axisZ[2]) / scaleSq;
    };

    float wrist[3];
    worldToLid(m[3][0], m[3][1], m[3][2], wrist);
    // Only exclude the narrow seam at the hinge. The complete arched upper shell remains valid.
    if (wrist[0] < 2.0f / modelScale) return false;

    auto distanceToFacet = [&](float along, size_t segment, float* closestXOut, float* closestYOut,
                               float* closestZOut, float* facetTOut) {
        const float px = m[3][0] + m[0][0] * along;
        const float py = m[3][1] + m[0][1] * along;
        const float pz = m[3][2] + m[0][2] * along;
        float local[3];
        worldToLid(px, py, pz, local);
        const float closestZ = std::clamp(local[2], -kHalfWidth, kHalfWidth);
        const float ez = local[2] - closestZ;
        const float vx = roofX[segment + 1] - roofX[segment];
        const float vy = roofY[segment + 1] - roofY[segment];
        const float lengthSq = vx * vx + vy * vy;
        const float t = std::clamp(((local[0] - roofX[segment]) * vx +
                                    (local[1] - roofY[segment]) * vy) / lengthSq,
                                   0.0f, 1.0f);
        const float closestX = roofX[segment] + vx * t;
        const float closestY = roofY[segment] + vy * t;
        const float ex = local[0] - closestX;
        const float ey = local[1] - closestY;
        *closestXOut = closestX;
        *closestYOut = closestY;
        *closestZOut = closestZ;
        *facetTOut = t;
        return ex * ex + ey * ey + ez * ez;
    };

    for (size_t segment = 0; segment < 5; ++segment) {
        float low = 0.0f;
        float high = len;
        for (int iteration = 0; iteration < 18; ++iteration) {
            const float third = (high - low) / 3.0f;
            float unusedX, unusedY, unusedZ, unusedT;
            const float lowDistance = distanceToFacet(low + third, segment, &unusedX, &unusedY,
                                                      &unusedZ, &unusedT);
            const float highDistance = distanceToFacet(high - third, segment, &unusedX, &unusedY,
                                                       &unusedZ, &unusedT);
            if (lowDistance <= highDistance) {
                high -= third;
            } else {
                low += third;
            }
        }
        const float along = (low + high) * 0.5f;
        float closestX, closestY, closestZ, t;
        const float distanceSq =
            distanceToFacet(along, segment, &closestX, &closestY, &closestZ, &t);
        if (distanceSq < bestSq) {
            bestSq = distanceSq;
            bestX = closestX;
            bestY = closestY;
            bestZ = closestZ;
            bestSegment = segment;
            bestFacetT = t;
        }
    }

    // Smooth the tangent across polygon joins while leaving contact on the real flat facets.
    const size_t vertex0Prev = bestSegment > 0 ? bestSegment - 1 : bestSegment;
    const size_t vertex1Next = bestSegment + 2 < 6 ? bestSegment + 2 : bestSegment + 1;
    float tangent0X = roofX[vertex0Prev] - roofX[bestSegment + 1];
    float tangent0Y = roofY[vertex0Prev] - roofY[bestSegment + 1];
    float tangent1X = roofX[bestSegment] - roofX[vertex1Next];
    float tangent1Y = roofY[bestSegment] - roofY[vertex1Next];
    const float tangent0Length = sqrtf(tangent0X * tangent0X + tangent0Y * tangent0Y);
    const float tangent1Length = sqrtf(tangent1X * tangent1X + tangent1Y * tangent1Y);
    tangent0X /= tangent0Length; tangent0Y /= tangent0Length;
    tangent1X /= tangent1Length; tangent1Y /= tangent1Length;
    bestTangentX = tangent0X + (tangent1X - tangent0X) * bestFacetT;
    bestTangentY = tangent0Y + (tangent1Y - tangent0Y) * bestFacetT;
    const float tangentLength = sqrtf(bestTangentX * bestTangentX + bestTangentY * bestTangentY);
    bestTangentX /= tangentLength;
    bestTangentY /= tangentLength;
    const float clearanceModel = profile.surfaceClearance / modelScale;
    // For a hinge-directed tangent, (-tangentY, tangentX) is the roof's outward normal.
    *outX = bestX - bestTangentY * clearanceModel;
    *outY = bestY + bestTangentX * clearanceModel;
    *outZ = bestZ;
    *outTangentY = bestTangentX;
    *outTangentZ = bestTangentY;
    *outContactAlong = len * 0.5f;
    *outDistance = sqrtf(bestSq) * modelScale;
    // Once a chest owned hover on the preceding frame, allow a small release hysteresis so the
    // hand does not flicker off at the joins between the half-cylinder's model facets.
    const bool continuingHover = sHands[hand].chest == chest || sPrevHoverChest[hand] == chest;
    return *outDistance <= profile.grabReach * (continuingHover ? 1.8f : 1.0f);
}

GetItemEntry PreviewEntry(const EnBox* chest) {
    if (sAwardActive && sAwardEntry.objectId != OBJECT_INVALID) {
        return sAwardEntry;
    }
    if (IS_RANDO && CVarGetInteger(CVAR_RANDOMIZER_ENHANCEMENT("MysteriousShuffle"), 0)) {
        return GET_ITEM_MYSTERY;
    }
    return chest->getItemEntry;
}

void ItemPosition(const EnBox* chest, float out[3], float* scale) {
    const ChestProfile profile = Profile(chest);
    float right[3], back[3];
    Axes(chest, right, back);
    float rise = 0.0f;
    if (sRevealFrame != 0 && gPlayState != nullptr) {
        rise = std::clamp((gPlayState->state.frames - sRevealFrame) / 18.0f, 0.0f, 1.0f);
    }
    const float jump = sinf(rise * kPi) * (IsSmall(chest) ? 4.0f : 8.0f);
    const float insideY = IsSmall(chest) ? 11.0f : 22.0f;
    const float y = insideY + (profile.hoverY - insideY) * rise + jump;
    out[0] = chest->dyna.actor.world.pos.x - back[0] * (IsSmall(chest) ? 1.5f : 3.0f);
    out[1] = chest->dyna.actor.world.pos.y + y;
    out[2] = chest->dyna.actor.world.pos.z - back[2] * (IsSmall(chest) ? 1.5f : 3.0f);
    *scale = profile.itemScale + (0.2f - profile.itemScale) * rise;
}

void ForgetActor(Actor* actor) {
    if (actor != nullptr && (actor == reinterpret_cast<Actor*>(sChest) ||
                             actor == reinterpret_cast<Actor*>(sOpenRequest))) {
        Clear();
        return;
    }
    for (int hand = 0; actor != nullptr && hand < 2; ++hand) {
        if (actor == reinterpret_cast<Actor*>(sHands[hand].chest)) sHands[hand] = {};
        if (actor == reinterpret_cast<Actor*>(sPrevHoverChest[hand])) sPrevHoverChest[hand] = nullptr;
    }
    for (LidMatrixRecord& record : sLidMatrices) {
        if (actor == reinterpret_cast<Actor*>(record.chest)) record = {};
    }
}

int32_t CommitOpen(EnBox* chest, PlayState* play) {
    sOpenFraction = 1.0f;
    chest->skelanime.jointTable[3].z = static_cast<s16>(kOpenJoint);
    sHands[0] = {};
    sHands[1] = {};
    sOpening = false;
    sOpenArmed = false;
    sFinishing = false;
    sOpenRequest = chest;
    sRequestConsumed = false;
    sRevealFrame = play->state.frames;
    Player* player = GET_PLAYER(play);
    player->getItemId = -(chest->dyna.actor.params >> 5 & 0x7F);
    player->interactRangeActor = &chest->dyna.actor;
    player->getItemDirection = 0;
    Audio_PlayActorSound2(&chest->dyna.actor, NA_SE_EV_TBOX_OPEN);
    VR_TriggerHaptic(VR_HAND_LEFT, 0.75f, 0.0f, 45.0f);
    VR_TriggerHaptic(VR_HAND_RIGHT, 0.75f, 0.0f, 45.0f);
    return 2;
}

} // namespace

extern "C" void VrChest_RecordLidMatrix(EnBox* chest, const float* mf16) {
    if (chest == nullptr || mf16 == nullptr || gPlayState == nullptr) return;
    LidMatrixRecord* target = FindLidMatrix(chest);
    if (target == nullptr) {
        target = &sLidMatrices[0];
        for (LidMatrixRecord& candidate : sLidMatrices) {
            if (candidate.chest == nullptr) {
                target = &candidate;
                break;
            }
            if (candidate.frame < target->frame) target = &candidate;
        }
    }
    target->chest = chest;
    target->frame = gPlayState->state.frames;
    target->joint = static_cast<uint16_t>(chest->skelanime.jointTable[3].z);
    memcpy(target->m, mf16, sizeof(target->m));
}

extern "C" int32_t VrChest_Tick(EnBox* chest, PlayState* play) {
    BeginInputFrame(play);
    // Player_Update runs before this actor. If A committed the ordinary chest interaction earlier
    // in this same frame, unk_1F4 is already nonzero: discard any hover/grip ownership and let
    // EnBox_WaitOpen continue through its untouched native animation and reward path below.
    if (chest != nullptr && chest->unk_1F4 != 0) {
        if (sChest == chest && sOpenRequest == nullptr) Clear();
        return 0;
    }
    if (!Eligible(chest, play)) {
        if (sChest == chest && sOpenRequest == nullptr) {
            chest->skelanime.jointTable[3].z = static_cast<s16>(kClosedJoint);
            Clear();
        }
        return 0;
    }
    if (sChest != nullptr && sChest != chest) return 0;

    const ChestProfile profile = Profile(chest);
    if (sChest == chest && sFinishing) {
        sOpenFraction = fminf(1.0f, sOpenFraction + kFinishStep);
        chest->skelanime.jointTable[3].z = LidJoint(sOpenFraction);
        return sOpenFraction >= 0.999f ? CommitOpen(chest, play) : 1;
    }

    for (int hand = 0; hand < 2; ++hand) {
        float localX = 0.0f, localY = profile.lidTopY, localZ = profile.frontZ;
        float tangentY = 0.0f, tangentZ = 1.0f, contactAlong = 0.0f, distance = 0.0f;
        const bool near = HandContact(chest, hand, &localX, &localY, &localZ, &tangentY, &tangentZ,
                                      &contactAlong, &distance);
        if (sHands[hand].latched) {
            sHands[hand].chest = chest;
            sHands[hand].frame = play->state.frames;
            sHands[hand].active = true;
            if (!sGripNow[hand]) {
                sHands[hand].latched = false;
            } else if (sFrameHandValid[hand]) {
                sHands[hand].trackingMissFrames = 0;
            } else if (++sHands[hand].trackingMissFrames > 2) {
                sHands[hand].latched = false;
            }
        } else if (near && (sChest == nullptr || sChest == chest) && distance < sHands[hand].score) {
            ChestHandState& state = sHands[hand];
            state.chest = chest;
            state.frame = play->state.frames;
            state.contactFrame = play->state.frames;
            state.active = true;
            state.trackingMissFrames = 0;
            state.score = distance;
            state.localX = localX;
            // Pin the selected point along the hand just above the curved roof in material
            // coordinates. It stays outside the chest and follows the rotating lid.
            state.localY = localY;
            state.localZ = localZ;
            state.tangentY = tangentY;
            state.tangentZ = tangentZ;
            state.contactAlong = contactAlong;
            // As with shutters, grip only the surface owner resolved by the preceding completed
            // frame. This avoids actor-order flicker when several chests are close together, but
            // an already-held grip attaches as soon as this hover has remained stable for a frame.
            if (sGripNow[hand] && sPrevHoverChest[hand] == chest &&
                GameInteractor_Should(VB_OPEN_CHEST, true, chest)) {
                // Settle almost onto the lid when gripping, without changing the chosen contact.
                const float grabClearance = profile.surfaceClearance * 0.2f;
                const float clearanceChangeModel =
                    (profile.surfaceClearance - grabClearance) / fabsf(chest->dyna.actor.scale.x);
                // Limb-space outward normal is (-tangentY, tangentX). Move back toward the facet.
                state.localX += state.tangentZ * clearanceChangeModel;
                state.localY -= state.tangentY * clearanceChangeModel;
                sChest = chest;
                state.latched = true;
                VR_TriggerHaptic(hand, 0.3f, 0.0f, 25.0f);
            }
        } else if (sPrevHoverChest[hand] == chest && sHands[hand].chest == chest &&
                   play->state.frames - sHands[hand].contactFrame <= 2) {
            // Two-frame grace absorbs facet-edge precision and a single missing controller pose.
            // Keep drawing the last exact surface transform, but do not initiate a new grab from it.
            sHands[hand].frame = play->state.frames;
            sHands[hand].active = true;
        } else if (sHands[hand].chest == chest &&
                   play->state.frames - sHands[hand].contactFrame > 2) {
            sHands[hand] = {};
        }
    }

    const bool bothHeld = sChest == chest && sHands[0].latched && sHands[1].latched &&
                          sGripNow[0] && sGripNow[1];
    if (!bothHeld) {
        if (sOpening) {
            sOpening = false;
        }
        if (sChest == chest && sOpenArmed) {
            // Past the balance/commit angle, letting go hands the remaining travel to the chest,
            // just as a released physical shutter hands its remaining lift to the native action.
            sHands[0] = {};
            sHands[1] = {};
            sFinishing = true;
            return 1;
        }
        // Like an ordinary physical hinged door, a released lid stays at the furthest angle the
        // player reached. Its moving front edge remains hoverable, so either hand can re-grab it
        // and the second hand can join without the lid snapping shut.
        if (sChest == chest && sOpenFraction > 0.0f) {
            chest->skelanime.jointTable[3].z = LidJoint(sOpenFraction);
            return 1;
        }
        if (sChest == chest && !sHands[0].latched && !sHands[1].latched && sOpenRequest == nullptr) {
            Clear();
        }
        const bool hoveringThis =
            (sHands[0].active && sHands[0].chest == chest) ||
            (sHands[1].active && sHands[1].chest == chest);
        return (hoveringThis || sChest == chest) ? 1 : 0;
    }

    if (!sFrameHandValid[0] || !sFrameHandValid[1]) return 1;
    const float handAngle[2] = { HandLidPolar(chest, 0), HandLidPolar(chest, 1) };
    if (!sOpening) {
        sOpening = true;
        sStartHandAngle[0] = handAngle[0];
        sStartHandAngle[1] = handAngle[1];
        sStartOpenFraction = sOpenFraction;
        GetItemEntry entry = PreviewEntry(chest);
        if (entry.objectId != OBJECT_INVALID) func_8083AE40(GET_PLAYER(play), entry.objectId);
        if (sOpenFraction == 0.0f) Audio_PlayActorSound2(&chest->dyna.actor, NA_SE_EV_TBOX_UNLOCK);
    }
    // Match ordinary hinged-door control: drive the lid from the hands' real angular travel around
    // its hinge. The previous fixed vertical-distance mapping rotated the lid much faster than the
    // grabbed point's physical arc and made the pinned hands appear to trail or slide.
    const float angularLift =
        (PositiveAngleChange(handAngle[0], sStartHandAngle[0]) +
         PositiveAngleChange(handAngle[1], sStartHandAngle[1])) * 0.5f;
    const float targetFraction = std::clamp(
        sStartOpenFraction + angularLift / kPhysicalOpenRadians, 0.0f, 1.0f);
    // Never close because the hands moved down or tracking produced a short transient. Opening
    // progress is monotonic, just as the ordinary VR door preserves the angle already achieved.
    sOpenFraction = fmaxf(sOpenFraction, targetFraction);
    sOpenArmed = sOpenArmed || sOpenFraction * kPhysicalOpenRadians >= kCommitAngleRadians;
    chest->skelanime.jointTable[3].z = LidJoint(sOpenFraction);

    if (sOpenFraction >= 0.98f) {
        return CommitOpen(chest, play);
    }
    return 1;
}

extern "C" bool VrChest_GripConsumed(int32_t hand, uint16_t mask) {
    const bool hovering = hand >= 0 && hand < 2 && gPlayState != nullptr &&
                          sHands[hand].frame == gPlayState->state.frames && sHands[hand].active;
    return (mask & VR_BTN_GRIP) && hand >= 0 && hand < 2 &&
           (hovering || sHands[hand].latched || sOpening || sFinishing);
}

extern "C" bool VrChest_HoldsHand(int32_t hand) {
    return hand >= 0 && hand < 2 && gPlayState != nullptr && sOpenRequest == nullptr &&
           sHands[hand].frame == gPlayState->state.frames && sHands[hand].active &&
           sHands[hand].chest != nullptr;
}

extern "C" bool VrChest_PinnedHandMatrix(Player* player, int32_t hand, float* mf16) {
    if (player == nullptr || mf16 == nullptr || hand < 0 || hand > 1 || gPlayState == nullptr ||
        player != GET_PLAYER(gPlayState) || !sHands[hand].active || sHands[hand].chest == nullptr ||
        sHands[hand].frame != gPlayState->state.frames || sOpenRequest != nullptr) {
        return false;
    }
    const ChestHandState& state = sHands[hand];
    EnBox* handChest = state.chest;
    float axisX[3], axisY[3], axisZ[3], origin[3];
    if (!LidTransform(handChest, axisX, axisY, axisZ, origin)) return false;
    // The native anatomical open-hand meshes have the opposite palm-normal convention from the
    // controller-side basis used here. Flip X and Z together: palm faces down without reversing
    // the wrist-to-fingertip tangent or producing a negative-determinant matrix.
    // Limb-space Z points opposite the actor-space width basis used by the previous placement.
    // Reverse the paired width/normal signs so each anatomical mesh presents its palm to the lid
    // while retaining a positive-determinant matrix.
    const float palmTurn = hand == VR_HAND_LEFT ? 1.0f : -1.0f;
    const float sx = sqrtf(mf16[0] * mf16[0] + mf16[1] * mf16[1] + mf16[2] * mf16[2]);
    const float sy = sqrtf(mf16[4] * mf16[4] + mf16[5] * mf16[5] + mf16[6] * mf16[6]);
    const float sz = sqrtf(mf16[8] * mf16[8] + mf16[9] * mf16[9] + mf16[10] * mf16[10]);

    float width[3] = { axisZ[0], axisZ[1], axisZ[2] };
    float tangent[3] = {
        axisX[0] * state.tangentY + axisY[0] * state.tangentZ,
        axisX[1] * state.tangentY + axisY[1] * state.tangentZ,
        axisX[2] * state.tangentY + axisY[2] * state.tangentZ,
    };
    const float widthLength = sqrtf(width[0] * width[0] + width[1] * width[1] + width[2] * width[2]);
    const float tangentLength =
        sqrtf(tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);
    for (int component = 0; component < 3; ++component) {
        width[component] /= widthLength;
        tangent[component] /= tangentLength;
    }
    const float normal[3] = {
        width[1] * tangent[2] - width[2] * tangent[1],
        width[2] * tangent[0] - width[0] * tangent[2],
        width[0] * tangent[1] - width[1] * tangent[0],
    };
    for (int component = 0; component < 3; ++component) {
        mf16[component] = palmTurn * width[component] * sx;
        mf16[4 + component] = tangent[component] * sy;
        mf16[8 + component] = palmTurn * normal[component] * sz;
    }

    const float px = origin[0] + axisX[0] * state.localX + axisY[0] * state.localY + axisZ[0] * state.localZ;
    const float py = origin[1] + axisX[1] * state.localX + axisY[1] * state.localY + axisZ[1] * state.localZ;
    const float pz = origin[2] + axisX[2] * state.localX + axisY[2] * state.localY + axisZ[2] * state.localZ;
    // The contact clearance describes the gap below the palm, not the hand's centre plane. Lift
    // the centre by the scaled half-thickness of the native open-hand mesh so its visible underside
    // stays outside the lid. The small extra allowance prevents the smoothly blended palm plane
    // from cutting a flat polygon facet near a join; gripping reduces it for weight-bearing contact.
    constexpr float kPalmHalfThicknessModel = 220.0f;
    const float curveSafety = state.latched ? 0.25f : 0.75f;
    const float palmLift = sz * kPalmHalfThicknessModel + curveSafety;
    mf16[12] = px + normal[0] * palmLift - mf16[4] * state.contactAlong;
    mf16[13] = py + normal[1] * palmLift - mf16[5] * state.contactAlong;
    mf16[14] = pz + normal[2] * palmLift - mf16[6] * state.contactAlong;
    return true;
}

extern "C" bool VrChest_OpenRequested(EnBox* chest) {
    return chest != nullptr && chest == sOpenRequest && !sRequestConsumed;
}

extern "C" void VrChest_ConsumeOpenRequest(EnBox* chest) {
    if (chest == sOpenRequest) sRequestConsumed = true;
}

extern "C" void VrChest_AwardStarted(EnBox* chest, const GetItemEntry* entry) {
    if (chest != nullptr && chest == sChest) {
        sAwardActive = true;
        sSawAwardDraw = true;
        if (entry != nullptr) sAwardEntry = *entry;
    }
}

extern "C" void VrChest_AwardFinished(EnBox* chest) {
    if (chest != nullptr && chest == sChest) {
        sAwardActive = false;
        sPreviewDone = true;
        Clear();
    }
}

extern "C" bool VrChest_AwardActive() {
    return sAwardActive;
}

extern "C" bool VrChest_GetItemHoldUpPos(PlayState* play, Player* player, float* out) {
    if (play == nullptr || player == nullptr || out == nullptr || sChest == nullptr || sPreviewDone ||
        !(player->stateFlags1 & PLAYER_STATE1_GETTING_ITEM)) {
        return false;
    }
    float scale;
    ItemPosition(sChest, out, &scale);
    out[1] -= 14.0f;
    sSawAwardDraw = true;
    return true;
}

extern "C" void VrChest_DrawItem(EnBox* chest, PlayState* play) {
    if (chest == nullptr || play == nullptr || chest != sChest || sOpenFraction < 0.08f || sPreviewDone) return;
    Player* player = GET_PLAYER(play);
    if (sRequestConsumed && sRevealFrame != 0 && play->state.frames - sRevealFrame > 300) {
        Clear();
        return;
    }

    GetItemEntry entry = PreviewEntry(chest);
    float pos[3], scale;
    ItemPosition(chest, pos, &scale);
    OPEN_DISPS(play->state.gfxCtx);
    Matrix_Push();
    gSegments[6] = VIRTUAL_TO_PHYSICAL(player->giObjectSegment);
    gSPSegment(POLY_OPA_DISP++, 0x06, (uintptr_t)player->giObjectSegment);
    gSPSegment(POLY_XLU_DISP++, 0x06, (uintptr_t)player->giObjectSegment);
    Matrix_Translate(pos[0], pos[1], pos[2], MTXMODE_NEW);
    Matrix_RotateZYX(0, play->gameplayFrames * 1000, 0, MTXMODE_APPLY);
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    GetItemEntry_Draw(play, entry);
    Matrix_Pop();
    CLOSE_DISPS(play->state.gfxCtx);
}

namespace {
void ReleaseFinishedPresentation() {
    if (sChest == nullptr || gPlayState == nullptr || !sRequestConsumed) return;
    const bool timedOut = sRevealFrame != 0 && gPlayState->state.frames - sRevealFrame > 300;
    if (!sAwardActive && timedOut) Clear();
}

void RegisterVrChest() {
    COND_HOOK(OnSceneInit, true, [](int16_t) { Clear(); });
    COND_HOOK(OnActorDestroy, true, [](void* actor) { ForgetActor(static_cast<Actor*>(actor)); });
    COND_HOOK(OnGameFrameUpdate, true, ReleaseFinishedPresentation);
}
static RegisterShipInitFunc initVrChest(RegisterVrChest);
} // namespace
