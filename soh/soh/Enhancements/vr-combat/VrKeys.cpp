extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "objects/object_gi_key/object_gi_key.h"
#include "objects/object_gi_bosskey/object_gi_bosskey.h"
#include "overlays/actors/ovl_En_Door/z_en_door.h"
#include "overlays/actors/ovl_Door_Shutter/z_door_shutter.h"
#include "overlays/actors/ovl_Door_Gerudo/z_door_gerudo.h"
extern PlayState* gPlayState;
}

#include "VrCombat.h"
#include "VrPocket.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/frame_interpolation.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <vr_interface.h>

// Phase one of physical locked doors. This exposes real inventory keys to the hands but never
// spends one; phase two will take ownership only after a compatible lock accepts the held key.
namespace {
enum class Kind { None, Small, Boss };
Kind sHeld = Kind::None;
int sHand = -1;
bool sGripPrev[2] = { true, true };
bool sNear[2] = { false, false };
Kind sNearKind = Kind::None;
int sHeldInterpKey;
Actor* sInsertLock = nullptr;
bool sInsertArmed = false;
enum class InsertState { None, WaitRelease, Ready, Turning, Complete };
InsertState sInsertState = InsertState::None;
float sInsertedPos[3] = {}, sInsertedSide[3] = {}, sInsertedUp[3] = {}, sInsertedForward[3] = {};
float sInsertedTip[3] = {}, sInsertedLength = 0.0f;
float sInsertedCalOffset[3] = {};
float sTurnStartQuat[4] = {};

float Cm(float cm) { return cm * 0.01f * VrPocket::WorldScale(); }

void LockOffsets(Kind kind, float* horizontal, float* vertical, float* depth) {
    *horizontal = CVarGetFloat(kind == Kind::Boss ? "gVrBossKeyLockOffsetX" : "gVrSmallKeyLockOffsetX", 0.0f);
    *vertical = CVarGetFloat(kind == Kind::Boss ? "gVrBossKeyLockOffsetY" : "gVrSmallKeyLockOffsetY", 0.0f);
    *depth = CVarGetFloat(kind == Kind::Boss ? "gVrBossKeyLockOffsetDepth" : "gVrSmallKeyLockOffsetDepth", 0.0f);
}

bool Gate(PlayState* play, Player* player) {
    return play && player && player == GET_PLAYER(play) && CVarGetInteger("gVrPhysicalKeys", 1) &&
           CVarGetInteger("gVrPhysicalDoors", 1) &&
           VR_IsInitialized() && VR_GetFirstPerson() && !VR_IsFlatScreen() &&
           CVarGetInteger("gVrMotionHands", 1) && play->pauseCtx.state == 0 && play->csCtx.state == CS_STATE_IDLE &&
           play->transitionTrigger == TRANS_TRIGGER_OFF && play->bombchuBowlingStatus == 0 &&
           play->shootingGalleryStatus == 0 && !(player->stateFlags1 &
               (PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_IN_WATER | PLAYER_STATE1_DEAD));
}

bool HasSmall() {
    const int map = gSaveContext.mapIndex;
    return map >= 0 && map < (int)ARRAY_COUNT(gSaveContext.inventory.dungeonKeys) &&
           // Negative is the engine's unlimited/debug-key sentinel (the HUD renders infinity).
           // Treat every nonzero value as available so the visible card and its interaction agree.
           gSaveContext.inventory.dungeonKeys[map] != 0;
}
bool OwnsBoss() {
    const int map = gSaveContext.mapIndex;
    return map >= 0 && map < (int)ARRAY_COUNT(gSaveContext.inventory.dungeonItems) &&
           CHECK_DUNGEON_ITEM(DUNGEON_KEY_BOSS, map);
}

bool IsBossKeyDungeon(const PlayState* play) {
    if (play == nullptr) return false;
    switch (play->sceneNum) {
        case SCENE_FOREST_TEMPLE:
        case SCENE_FIRE_TEMPLE:
        case SCENE_WATER_TEMPLE:
        case SCENE_SPIRIT_TEMPLE:
        case SCENE_SHADOW_TEMPLE:
        case SCENE_INSIDE_GANONS_CASTLE:
        case SCENE_GANONS_TOWER:
        case SCENE_FOREST_TEMPLE_BOSS:
        case SCENE_FIRE_TEMPLE_BOSS:
        case SCENE_WATER_TEMPLE_BOSS:
        case SCENE_SPIRIT_TEMPLE_BOSS:
        case SCENE_SHADOW_TEMPLE_BOSS:
            return true;
        default:
            return false;
    }
}

bool HasLockedBossDoorNearby(PlayState* play) {
    if (play == nullptr || !OwnsBoss()) return false;

    const Player* player = GET_PLAYER(play);
    constexpr float kAvailabilityRadius = 400.0f;
    constexpr float kAvailabilityRadiusSq = kAvailabilityRadius * kAvailabilityRadius;
    for (Actor* actor = play->actorCtx.actorLists[ACTORCAT_DOOR].head; actor != nullptr; actor = actor->next) {
        if (actor->id != ACTOR_DOOR_SHUTTER || actor->update == nullptr) continue;
        DoorShutter* door = reinterpret_cast<DoorShutter*>(actor);
        if (door->doorType != SHUTTER_BOSS || door->unlockTimer == 0) continue;

        const float dx = actor->world.pos.x - player->actor.world.pos.x;
        const float dy = actor->world.pos.y - player->actor.world.pos.y;
        const float dz = actor->world.pos.z - player->actor.world.pos.z;
        if (dx * dx + dy * dy + dz * dz <= kAvailabilityRadiusSq) return true;
    }
    return false;
}

bool HasUsedBossDoorLoaded(PlayState* play) {
    if (play == nullptr) return false;
    for (Actor* actor = play->actorCtx.actorLists[ACTORCAT_DOOR].head; actor != nullptr; actor = actor->next) {
        if (actor->id != ACTOR_DOOR_SHUTTER || actor->update == nullptr) continue;
        DoorShutter* door = reinterpret_cast<DoorShutter*>(actor);
        if (door->doorType == SHUTTER_BOSS && Flags_GetSwitch(play, actor->params & 0x3F)) return true;
    }
    return false;
}

bool HandNear(int hand, const float point[3]) {
    float p[3], q[4];
    if (!VR_GetHandPose(hand, p, q)) return false;
    const float dx = p[0] - point[0], dy = p[1] - point[1], dz = p[2] - point[2];
    const float reach = Cm(CVarGetFloat("gVrKeyGrabRadius", 12.0f));
    return dx * dx + dy * dy + dz * dz <= reach * reach;
}

bool Pose(Kind kind, float pos[3], float side[3], float up[3], float forward[3]) {
    if (sHeld != kind) return false;
    if (sInsertState != InsertState::None) {
        float horizontal, vertical, depth;
        LockOffsets(kind, &horizontal, &vertical, &depth);
        const float liveDelta[3] = { horizontal - sInsertedCalOffset[0], vertical - sInsertedCalOffset[1],
                                     depth - sInsertedCalOffset[2] };
        for (int i = 0; i < 3; ++i) {
            pos[i] = sInsertedPos[i] + sInsertedSide[i] * liveDelta[0] + sInsertedUp[i] * liveDelta[1] +
                     sInsertedForward[i] * liveDelta[2];
            side[i] = sInsertedSide[i];
            up[i] = sInsertedUp[i]; forward[i] = sInsertedForward[i];
        }
        if (sInsertState == InsertState::Turning || sInsertState == InsertState::Complete) {
            float handPos[3], q[4];
            if (VR_GetHandPose(VR_HAND_RIGHT, handPos, q)) {
                const float inv[4] = { -sTurnStartQuat[0], -sTurnStartQuat[1], -sTurnStartQuat[2], sTurnStartQuat[3] };
                const float delta[4] = {
                    q[3] * inv[0] + q[0] * inv[3] + q[1] * inv[2] - q[2] * inv[1],
                    q[3] * inv[1] - q[0] * inv[2] + q[1] * inv[3] + q[2] * inv[0],
                    q[3] * inv[2] + q[0] * inv[1] - q[1] * inv[0] + q[2] * inv[3],
                    q[3] * inv[3] - q[0] * inv[0] - q[1] * inv[1] - q[2] * inv[2],
                };
                // A key can only twist around the lock barrel. Applying the controller's complete
                // quaternion also pitched/yawed the seated key, making it hang crooked and pull its
                // blade out of the keyhole. Extract only twist about the fixed insertion axis.
                const float twistSin = delta[0] * sInsertedForward[0] + delta[1] * sInsertedForward[1] +
                                       delta[2] * sInsertedForward[2];
                const float twist = 2.0f * atan2f(twistSin, delta[3]);
                const float c = cosf(twist), s = sinf(twist);
                auto rotateAboutLock = [&](const float v[3], float out[3]) {
                    const float dot = v[0] * sInsertedForward[0] + v[1] * sInsertedForward[1] +
                                      v[2] * sInsertedForward[2];
                    const float cross[3] = {
                        sInsertedForward[1] * v[2] - sInsertedForward[2] * v[1],
                        sInsertedForward[2] * v[0] - sInsertedForward[0] * v[2],
                        sInsertedForward[0] * v[1] - sInsertedForward[1] * v[0],
                    };
                    for (int i = 0; i < 3; ++i) {
                        out[i] = v[i] * c + cross[i] * s + sInsertedForward[i] * dot * (1.0f - c);
                    }
                };
                rotateAboutLock(sInsertedSide, side);
                rotateAboutLock(sInsertedUp, up);
                for (int i = 0; i < 3; ++i) {
                    forward[i] = sInsertedForward[i];
                    pos[i] = sInsertedPos[i] + sInsertedSide[i] * liveDelta[0] + sInsertedUp[i] * liveDelta[1] +
                             sInsertedForward[i] * liveDelta[2];
                }
            }
        }
        return true;
    }
    // A held key has a deliberate grip-local pose rather than preserving the floating anchor's
    // world orientation. It is therefore rigidly part of the controller/closed fist and cannot
    // appear to rotate independently as the wrist turns.
    float handPos[3], q[4];
    if (!VR_GetHandPose(sHand, handPos, q)) return false;
    const float sideLocal[3] = { 0, 1, 0 };
    const float upLocal[3] = { 0, 0, -1 };
    const float forwardLocal[3] = { -1, 0, 0 };
    VrPocket::QuatRot(q, sideLocal, side);
    VrPocket::QuatRot(q, upLocal, up);
    VrPocket::QuatRot(q, forwardLocal, forward);
    // User calibration in controller-local space. This moves the complete grabbed model without
    // changing its orientation or the independently calibrated position used after lock insertion.
    const char* heldX = kind == Kind::Boss ? "gVrBossKeyHeldOffsetX" : "gVrSmallKeyHeldOffsetX";
    const char* heldY = kind == Kind::Boss ? "gVrBossKeyHeldOffsetY" : "gVrSmallKeyHeldOffsetY";
    const char* heldZ = kind == Kind::Boss ? "gVrBossKeyHeldOffsetZ" : "gVrSmallKeyHeldOffsetZ";
    const float localOffset[3] = { Cm(CVarGetFloat(heldX, 0.0f)), Cm(CVarGetFloat(heldY, 0.0f)),
                                   -Cm(CVarGetFloat(heldZ, 0.0f)) };
    float worldOffset[3];
    VrPocket::QuatRot(q, localOffset, worldOffset);
    for (int i = 0; i < 3; ++i) pos[i] = handPos[i] + worldOffset[i];
    return true;
}
void Clear() {
    sHeld = Kind::None; sHand = -1;
    sNear[0] = sNear[1] = false;
    sNearKind = Kind::None;
    sInsertLock = nullptr; sInsertArmed = false;
    sInsertState = InsertState::None;
}
void Take(Kind kind, int hand) {
    sHeld = kind; sHand = hand;
    // Match the confirmation tick used when the Alyx-style item selector changes selection.
    VR_TriggerHaptic(hand, 0.4f, 0.0f, 25.0f);
}
} // namespace

static bool TryInsert(Actor* lock, float holeY, Kind required) {
    if (lock == nullptr || sHeld != required) return false;
    if (sInsertState == InsertState::Complete && sInsertLock == lock) {
        Clear();
        return true;
    }
    if (sInsertState != InsertState::None || sHand < 0) return false;

    float keyPos[3], side[3], up[3], forward[3];
    if (!Pose(sHeld, keyPos, side, up, forward)) return false;
    const float legacyScale = CVarGetFloat("gVrKeyHeldScale", 180.0f);
    const float heldScale = CVarGetFloat(required == Kind::Boss ? "gVrBossKeyHeldScale" : "gVrSmallKeyHeldScale",
                                         legacyScale) /
                            100.0f;
    const float modelScale = (required == Kind::Boss ? 0.018f : 0.022f) * heldScale;
    // Measured from the actual OTR vertices: the controller origin is placed at the decorative
    // handle and the blade tip lies roughly this far away along the corrected length axis.
    const float bladeModelLength = required == Kind::Boss ? 57.0f : 59.0f;
    float inward[3] = { -forward[0], -forward[1], -forward[2] };

    const float yawSin = Math_SinS(lock->shape.rot.y), yawCos = Math_CosS(lock->shape.rot.y);
    float outward[3] = { yawSin, 0.0f, yawCos };
    const float handSide = (keyPos[0] - lock->world.pos.x) * outward[0] +
                           (keyPos[2] - lock->world.pos.z) * outward[2];
    if (handSide < 0.0f) {
        outward[0] = -outward[0]; outward[2] = -outward[2];
    }
    float horizontal, vertical, depthOffset;
    LockOffsets(required, &horizontal, &vertical, &depthOffset);
    const float lockSide[3] = { outward[2], 0.0f, -outward[0] };
    const float hole[3] = { lock->world.pos.x + outward[0] * (5.0f + depthOffset) + lockSide[0] * horizontal,
                            lock->world.pos.y + holeY + vertical,
                            lock->world.pos.z + outward[2] * (5.0f + depthOffset) + lockSide[2] * horizontal };
    // Asset variants do not all author the blade on the same signed length axis. Select the end
    // physically nearer this keyhole, then use that same end for alignment and the seated pose.
    float tip[3] = { keyPos[0] + inward[0] * bladeModelLength * modelScale,
                     keyPos[1] + inward[1] * bladeModelLength * modelScale,
                     keyPos[2] + inward[2] * bladeModelLength * modelScale };
    const float otherTip[3] = { keyPos[0] - inward[0] * bladeModelLength * modelScale,
                                keyPos[1] - inward[1] * bladeModelLength * modelScale,
                                keyPos[2] - inward[2] * bladeModelLength * modelScale };
    const float tipSq = (tip[0] - hole[0]) * (tip[0] - hole[0]) + (tip[1] - hole[1]) * (tip[1] - hole[1]) +
                        (tip[2] - hole[2]) * (tip[2] - hole[2]);
    const float otherSq = (otherTip[0] - hole[0]) * (otherTip[0] - hole[0]) +
                          (otherTip[1] - hole[1]) * (otherTip[1] - hole[1]) +
                          (otherTip[2] - hole[2]) * (otherTip[2] - hole[2]);
    if (otherSq < tipSq) {
        for (int i = 0; i < 3; ++i) { tip[i] = otherTip[i]; inward[i] = -inward[i]; }
    }
    const float delta[3] = { tip[0] - hole[0], tip[1] - hole[1], tip[2] - hole[2] };
    const float depth = delta[0] * outward[0] + delta[2] * outward[2];
    const float radialSq = delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2] - depth * depth;
    const float alignment = inward[0] * -outward[0] + inward[2] * -outward[2];

    // The lock model and the door actor do not share pixel-perfect origins across every dungeon
    // skin. Capture generously around the visible keyhole, but still require the blade to point
    // into the door. Once captured, the exact seated transform below is authoritative.
    if (radialSq > 36.0f * 36.0f || alignment < 0.15f || depth < -24.0f || depth > 48.0f) {
        if (sInsertLock == lock) { sInsertLock = nullptr; sInsertArmed = false; }
        return false;
    }
    if (sInsertLock != lock) {
        sInsertLock = lock;
        VR_TriggerHaptic(sHand, 0.22f, 0.0f, 22.0f);
    }
    // Entering the visible keyhole is the precision step. Once the rendered tip is within its
    // capture volume, seat it in the canonical lock pose immediately; requiring a second hidden
    // depth-plane crossing left the key crooked beside the padlock even when it visibly touched.
    if (depth <= 28.0f) {
        // Snap to the lock, not to the wrist pose: blade perpendicular to the door, handle plane
        // vertical to the floor. This is the neutral pose from which the player releases and turns.
        const float seatedForward[3] = { outward[0], 0.0f, outward[2] };
        const float seatedUp[3] = { 0.0f, 1.0f, 0.0f };
        const float seatedSide[3] = { seatedForward[2], 0.0f, -seatedForward[0] };
        for (int i = 0; i < 3; ++i) {
            sInsertedPos[i] = hole[i] + seatedForward[i] * bladeModelLength * modelScale;
            sInsertedTip[i] = hole[i];
            sInsertedSide[i] = seatedSide[i];
            sInsertedUp[i] = seatedUp[i];
            sInsertedForward[i] = seatedForward[i];
        }
        sInsertedLength = bladeModelLength * modelScale;
        sInsertedCalOffset[0] = horizontal;
        sInsertedCalOffset[1] = vertical;
        sInsertedCalOffset[2] = depthOffset;
        sInsertState = InsertState::WaitRelease;
        sInsertArmed = false;
        VR_TriggerHaptic(sHand, 0.55f, 0.0f, 38.0f);
    }
    return false;
}

extern "C" bool VrKeys_TryEnDoor(EnDoor* door, PlayState* play) {
    return play != nullptr && door != nullptr && door->lockTimer != 0 && HasSmall() &&
           TryInsert(&door->actor, 50.0f, Kind::Small);
}

extern "C" bool VrKeys_TryShutter(DoorShutter* door, PlayState* play) {
    if (play == nullptr || door == nullptr || door->unlockTimer == 0) return false;
    if (door->doorType == SHUTTER_BOSS) {
        return OwnsBoss() && TryInsert(&door->dyna.actor, 80.0f, Kind::Boss);
    }
    return door->doorType == SHUTTER_KEY_LOCKED && HasSmall() &&
           TryInsert(&door->dyna.actor, door->gfxType == 6 ? 80.0f : 50.0f, Kind::Small);
}

extern "C" bool VrKeys_TryGerudoDoor(DoorGerudo* door, PlayState* play) {
    return play != nullptr && door != nullptr && door->unk_166 != 0 && HasSmall() &&
           TryInsert(&door->dyna.actor, 50.0f, Kind::Small);
}

extern "C" void VrKeys_Reset(void) {
    Clear(); sGripPrev[0] = sGripPrev[1] = true;
}

extern "C" bool VrKeys_GripConsumed(int32_t hand, uint16_t mask) {
    return (mask & VR_BTN_GRIP) && hand == VR_HAND_RIGHT &&
           ((sHeld != Kind::None && (sHand == hand || sInsertState != InsertState::None)) || sNear[hand]);
}

extern "C" bool VrKeys_HoldsHand(int32_t hand) {
    return sHeld != Kind::None && sHand == hand;
}

extern "C" bool VrKeys_HudHighlighted(int32_t bossKey) {
    return sNearKind == (bossKey ? Kind::Boss : Kind::Small);
}

extern "C" bool VrKeys_BossAvailable(PlayState* play) {
    // OoT retains the Boss Key inventory bit after use. For the physical wrist card, the unlocked
    // boss-door switch is the consumed state: once that lock has accepted the key, hide the card.
    return IsBossKeyDungeon(play) && OwnsBoss() && !HasUsedBossDoorLoaded(play);
}

extern "C" void VrKeys_Tick(PlayState* play, Player* player) {
    if (!Gate(play, player)) {
        VrKeys_Reset();
        return;
    }
    if ((sHeld == Kind::Small && !HasSmall()) || (sHeld == Kind::Boss && !VrKeys_BossAvailable(play))) Clear();
    bool pressed[2], released[2];
    for (int h = 0; h < 2; ++h) {
        const bool grip = (VR_GetControllerButton(h) & VR_BTN_GRIP) != 0;
        pressed[h] = grip && !sGripPrev[h]; released[h] = !grip && sGripPrev[h];
        sGripPrev[h] = grip; sNear[h] = false;
    }
    sNearKind = Kind::None;
    if (sInsertState != InsertState::None) {
        if (sInsertState == InsertState::WaitRelease && sHand >= 0 && released[sHand]) {
            sHand = -1;
            sInsertState = InsertState::Ready;
        } else if (sInsertState == InsertState::Ready && pressed[VR_HAND_RIGHT] &&
                   HandNear(VR_HAND_RIGHT, sInsertedPos)) {
            float p[3];
            if (VR_GetHandPose(VR_HAND_RIGHT, p, sTurnStartQuat)) {
                sHand = VR_HAND_RIGHT;
                sInsertState = InsertState::Turning;
                VR_TriggerHaptic(sHand, 0.35f, 0.0f, 28.0f);
            }
        } else if (sInsertState == InsertState::Turning) {
            if (released[VR_HAND_RIGHT]) {
                sHand = -1;
                sInsertState = InsertState::Ready;
            } else {
                float p[3], q[4];
                if (VR_GetHandPose(VR_HAND_RIGHT, p, q)) {
                    float dot = fabsf(q[0] * sTurnStartQuat[0] + q[1] * sTurnStartQuat[1] +
                                      q[2] * sTurnStartQuat[2] + q[3] * sTurnStartQuat[3]);
                    dot = fmaxf(0.0f, fminf(1.0f, dot));
                    if (2.0f * acosf(dot) * (180.0f / M_PI) >= 90.0f) {
                        sInsertState = InsertState::Complete;
                    }
                }
            }
        }
        return;
    }
    if (sHeld != Kind::None) {
        if (released[sHand]) {
            VR_TriggerHaptic(sHand, 0.18f, 0.0f, 18.0f);
            Clear();
        }
        return;
    }
    // Visibility and interaction must use the same ownership rules. Door proximity belongs to
    // insertion, not to taking an already-visible inventory key from the wrist.
    const Kind available[2] = { HasSmall() ? Kind::Small : Kind::None,
                                VrKeys_BossAvailable(play) ? Kind::Boss : Kind::None };
    Kind nearest = Kind::None;
    float nearestDistance = 1.0e30f;
    for (Kind kind : available) {
        if (kind == Kind::None) continue;
        // The key lives on the left wrist HUD, so the right hand takes it. Letting the wrist's own
        // grip qualify would make ordinary left-grip input grab a key without reaching for it.
        for (int h = VR_HAND_RIGHT; h <= VR_HAND_RIGHT; ++h) {
            float distance = 0.0f;
            const int element = kind == Kind::Boss ? VR_HUD_EL_PHYSICAL_BOSS_KEY : VR_HUD_EL_PHYSICAL_KEYS;
            if (!VR_GetHudElementHandDistance(h, element, &distance) ||
                distance > CVarGetFloat("gVrKeyGrabRadius", 12.0f) * 0.01f) {
                continue;
            }
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearest = kind;
            }
        }
    }
    if (nearest != Kind::None) {
        sNear[VR_HAND_RIGHT] = true;
        sNearKind = nearest;
        if (pressed[VR_HAND_RIGHT]) Take(nearest, VR_HAND_RIGHT);
    }
}

static void DrawKey(Kind kind, const void* interpKey) {
    float pos[3], side[3], up[3], forward[3];
    if (!Pose(kind, pos, side, up, forward)) return;
    Vec3s rot;
    VrPocket::SetRotFromAxes(&rot, side, up, forward);
    GraphicsContext* gfxCtx = gPlayState->state.gfxCtx;
    const float anchorScale = kind == Kind::Boss ? 0.018f : 0.022f;
    // The two get-item models have very different authored proportions, so each key needs its own
    // real-world calibration. Fall back to the former shared setting to preserve existing configs.
    const float legacyHeldScale = CVarGetFloat("gVrKeyHeldScale", 180.0f);
    const float heldScale = CVarGetFloat(kind == Kind::Boss ? "gVrBossKeyHeldScale" : "gVrSmallKeyHeldScale",
                                         legacyHeldScale) /
                            100.0f;
    const float scale = anchorScale * (sHeld == kind ? heldScale : 1.0f);
    OPEN_DISPS(gfxCtx);
    FrameInterpolation_RecordOpenChild(interpKey, 0);
    Matrix_SetTranslateRotateYXZ(pos[0], pos[1], pos[2], &rot);
    Matrix_RotateX(1.57079632679f, MTXMODE_APPLY);
    if (sHeld == kind) {
        // Roll around the authored length axis. Different controller grips need fine calibration,
        // so expose the value while the key is held. Once inserted, use the canonical quarter-turn
        // instead: controller calibration must not leave the Boss Key tilted in the door's lock.
        // The get-item key is authored upside-down relative to the grip basis. Preserve the
        // exposed calibration while applying the required half-turn so its teeth point down.
        const char* heldRoll = kind == Kind::Boss ? "gVrBossKeyRollDeg" : "gVrSmallKeyRollDeg";
        const char* insertRoll = kind == Kind::Boss ? "gVrBossKeyInsertRollDeg" : "gVrSmallKeyInsertRollDeg";
        const char* insertRotX = kind == Kind::Boss ? "gVrBossKeyInsertRotXDeg" : "gVrSmallKeyInsertRotXDeg";
        const char* insertRotZ = kind == Kind::Boss ? "gVrBossKeyInsertRotZDeg" : "gVrSmallKeyInsertRotZDeg";
        if (sInsertState != InsertState::None) {
            Matrix_RotateX(CVarGetFloat(insertRotX, 0.0f) * (M_PI / 180.0f), MTXMODE_APPLY);
        }
        const float rollDeg = sInsertState == InsertState::None
                                  ? CVarGetFloat(heldRoll, CVarGetFloat("gVrKeyRollDeg", -90.0f)) + 180.0f
                                  : CVarGetFloat(insertRoll, 90.0f);
        Matrix_RotateY(rollDeg * (M_PI / 180.0f), MTXMODE_APPLY);
        if (sInsertState != InsertState::None) {
            Matrix_RotateZ(CVarGetFloat(insertRotZ, 0.0f) * (M_PI / 180.0f), MTXMODE_APPLY);
        }
        // Both get-item meshes are authored diagonally in their XY plane rather than along +Y.
        // Straighten their measured principal axis before applying the hand-space pose.
        const float straightenDeg = kind == Kind::Boss ? 36.1f : 37.3f;
        Matrix_RotateZ(straightenDeg * (M_PI / 180.0f), MTXMODE_APPLY);
    }
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    if (sHeld == kind) {
        // Place the measured centre of the non-blade handle at the controller origin. Translation
        // is model-space (and therefore scales with Held Key Size), leaving the decorative head
        // outside the fist while the shaft and teeth extend away from it.
        const float gripCenter = kind == Kind::Boss ? 15.0f : 18.0f;
        Matrix_Translate(-gripCenter, -gripCenter, 0.0f, MTXMODE_APPLY);
    }
    MtxF cur; Matrix_Get(&cur);
    Mtx* mtx = MATRIX_NEWMTX(gfxCtx);
    const int weldHand = sHeld == kind ? sHand : VR_HAND_LEFT;
    MtxF hand;
    if (weldHand >= 0 && VR_GetHandMatrix(weldHand, hand.mf)) {
        MtxF inv, local;
        SkinMatrix_Invert(&hand, &inv);
        SkinMatrix_MtxFMtxFMult(&inv, &cur, &local);
        VR_RegisterHandChildMatrix(mtx, weldHand, &local.mf[0][0]);
    }
    POLY_OPA_DISP = Play_SetFog(gPlayState, POLY_OPA_DISP);
    Gfx_SetupDL_25Opa(gfxCtx);
    gSPMatrix(POLY_OPA_DISP++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_OPA_DISP++, (Gfx*)(kind == Kind::Boss ? gGiBossKeyDL : gGiSmallKeyDL));
    if (kind == Kind::Boss) {
        Gfx_SetupDL_25Xlu(gfxCtx);
        gSPMatrix(POLY_XLU_DISP++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList(POLY_XLU_DISP++, (Gfx*)gGiBossKeyGemDL);
    }
    FrameInterpolation_RecordCloseChild();
    CLOSE_DISPS(gfxCtx);
}

extern "C" void VrKeys_Draw(void) {
    if (!gPlayState || !Gate(gPlayState, GET_PLAYER(gPlayState))) return;
    if (sHeld == Kind::Small) DrawKey(Kind::Small, &sHeldInterpKey);
    if (sHeld == Kind::Boss) DrawKey(Kind::Boss, &sHeldInterpKey);
    // Availability is represented by two-dimensional wrist-HUD icons. This hook draws only the
    // physical model created after the player actually takes one of those icons into their hand.
}

namespace {
void RegisterVrKeys() { COND_HOOK(OnPlayDrawEnd, true, VrKeys_Draw); }
static RegisterShipInitFunc initVrKeys(RegisterVrKeys);
}
