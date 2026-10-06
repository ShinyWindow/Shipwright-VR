extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "objects/gameplay_keep/gameplay_keep.h"
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}
#include "VrCombat.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/frame_interpolation.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <vr_interface.h>
#include <cmath>

// Physical bottle (VR first person, selector mode). Two interactions so far, both built on one
// notion: the bottle MOUTH — the rim centre of the held bottle model through the live hand draw
// matrix (MouthAnchorWorld), with a marker drawn there.
//
// SCOOPING (empty bottle). Vanilla catches work in two halves: every catchable actor (fairy,
// fish, blue fire, bugs) offers itself each frame through Actor_OfferGetItem(GI_MAX) when
// Link's BODY is within a few dozen units, and the bottle swing action reads that offer during a
// 3-frame animation window. Here the animation window is gone: the mouth entering the actor's
// catch volume IS the reach test — Actor_OfferGetItem measures GI_MAX offers against the mouth
// while this module covers, and the vanilla use-item action handler commits the catch through
// the vanilla resolution (Player_VrTryBottleCatch in z_player.c: same table, same parent
// handshake, same textbox and fanfare). No swing, no button. The mouth path is sampled once per
// game tick and volume tests use the segment from the previous sample, so a quick scoop cannot
// step over a small fairy at 20 Hz.
//
// POURING (fish, bug, blue fire, fairy). Hold the bottle upside down and shake it three times;
// a shake is a downward stroke of the mouth of at least gVrBottleShakeAmplitude (distance from
// the stroke's peak, so slow dips count). "Upside down" is the bottle model's own axis: the
// adult bottle mesh runs along hand-limb +X with the mouth at +X (vertex data: cross-sections
// in YZ planes at X = -938 base ... +440 rim), the same axis the sword blade extends along, so
// the hand draw matrix's X column is the base -> mouth direction exactly as rendered.
// Each valid shake ticks the hand, the third empties the bottle ON THE SPOT: the contents spawn
// at the mouth immediately (Player_VrBottlePourOut — the same actors the vanilla drop / release
// actions spawn at their key frames, dropped blue fire melts red ice by contact as in vanilla,
// a released fairy heals), no body animation, no cutscene state. Potions, milk and poes are
// DRUNK in vanilla and belong to the drink gesture; Big Poe and Ruto's Letter are shown to
// NPCs — none of those pour.
//
// DRINKING (red / blue / green potion, full / half milk, poe). Bring the mouth to the face: the
// face point is the headset eye position dropped gVrBottleDrinkFaceDown, and the mouth counts as
// at the face within gVrBottleDrinkDistance of it (a small radius on purpose). Arriving is sip
// one (haptic); every gVrBottleSipInterval it stays, another sip; the third sip is the swallow:
// Player_VrBottleDrink applies the vanilla effect (heal / magic / five hearts / the poe gamble)
// and empties the bottle (full milk becomes half) on the spot, no body animation, no freeze.
// Pulling the bottle away before the third sip (for more than a short grace) spends nothing.
//
// Fallbacks: gVrPhysBottleScoop=0 restores the trigger swing (the offer geometry and the trigger
// mask both return to vanilla); gVrPhysBottlePour=0 / gVrPhysBottleDrink=0 leave the trigger as
// the only way to use those contents. The trigger always keeps working for filled bottles
// (vanilla drink/pour/show).

namespace {

// Mouth path this tick (game units, world). Prev is invalid on the first sampled tick.
float sMouthPrev[3] = {};
float sMouthCur[3] = {};
bool sMouthCurValid = false;
bool sMouthPrevValid = false;
bool sCovers = false; // scoop coverage, cached once per tick for the per-actor offer callbacks

// Reach feel: any volume test that lands within the "near" band this tick flags it; the tick
// turns the rising edge into a soft haptic on the bottle hand (pairs with the marker growing).
bool sNearThisTick = false;
bool sWasNear = false;

// Pour gesture state. A shake is a DOWNWARD STROKE of the mouth: from the height where the hand
// last turned to descend (the stroke's peak) the mouth must travel at least the amplitude
// setting. Peaks and valleys are the sampled extremes, with a small hysteresis deciding when a
// reversal is real, so this is a pure distance test — three slow dips count exactly like three
// sharp flicks, and a 20 Hz sample landing a tick after the true peak never shortens a stroke.
// (The previous detector measured between the first ticks AFTER each reversal, which cut two
// ticks of travel off every stroke: a quick 8 cm flick at 20 Hz read as ~2 cm and never counted.)
int sShakeCount = 0;      // valid shakes so far while inverted
int sTicksSinceShake = 0; // resets the count when the rhythm stops
int sUprightTicks = 0;    // consecutive ticks not inverted; strokes still count during the grace
bool sStrokeValid = false;    // stroke tracking primed
int sStrokeDir = 0;           // -1 descending, +1 ascending, 0 waiting for the first descent
float sStrokeStartY = 0.0f;   // height where the current stroke began (the last reversal's extreme)
float sStrokeExtremeY = 0.0f; // furthest height reached in the current direction
bool sStrokeCounted = false;  // the current down stroke has already been counted as a shake
constexpr int kShakesToPour = 3;
constexpr int kUprightGraceTicks = 6;   // 300 ms tilted out of the cone mid-flick is not "righted"
constexpr int kRhythmTimeoutTicks = 40; // 2 s without a shake starts the count over

// Drink gesture state. A sip is a moment the mouth is at the face: the arrival, then every sip
// interval it stays there. Leaving for longer than the grace resets the count.
int sSipCount = 0;        // sips so far while at the face
int sSipTicks = 0;        // ticks since the last sip (counts up while at the face)
int sAwayTicks = 0;       // consecutive ticks not at the face; sips survive a short grace
bool sAtFaceNow = false;  // last tick's verdict, for the marker
constexpr int kSipsToDrink = 3;
constexpr int kAwayGraceTicks = 6; // 300 ms of hand wobble out of the radius is not "put down"

// Live diagnostics for the settings menu readout (VrBottle_GetDebug). lastPourResult and
// lastDrinkResult start at -1 ("none yet") and are only ever written by an attempt.
VrBottleDebug sDebug = { 0, 0, 0, 0.0f, 0.0f, 0, 0.0f, 0.0f, -1, 0, 0.0f, 0.0f, 0, -1 };

// After a pour-out the mouth is, by construction, sitting right on what just came out: scooping
// stays disarmed for gVrBottleRecatchDelay seconds so the fish/bug/fairy can get away.
int sRecatchTicks = 0;

int BottleHand() {
    // The bottle is a left-hand model, so it rides the sword-hand controller.
    return CVarGetInteger("gVrLeftHanded", 0) ? VR_HAND_LEFT : VR_HAND_RIGHT;
}

float WorldScale() {
    const float ws = VR_GetWorldScale();
    return ws < 1.0f ? 35.0f : ws;
}

// The bottle opening in the held model's own coordinates: the centre of the rim ring, the
// cross-section at X = +440 of gLinkAdultBottleDL / gLinkChildBottleDL (vertices (440, 220..1033,
// -510..303) adult, (440, 162..975, -492..321) child; the child mesh is the adult one shifted
// -58 on Y and +18 on Z). Both are drawn in L_HAND space (z_player_lib.c, sBottleDLists).
constexpr float kBottleRimX = 440.0f;
constexpr float kBottleRimYZ[2][2] = { { 626.0f, -104.0f }, { 568.0f, -86.0f } }; // [linkAge]

// The bottle mouth in world: the rim centre through the live hand draw matrix, so it sits on
// the opening at every world scale, either age and with the hand calibration, exactly as the
// model is rendered. (Replaces the controller-centimetre sliders tuned adult-only on September
// 14, 2026, which landed within about 1 cm of this point at 35 units/m and drifted off it at
// any other scale.)
bool MouthAnchorWorld(float* out3) {
    float m[4][4];
    if (!VR_GetHandMatrix(BottleHand(), m)) {
        return false;
    }
    const int age = gSaveContext.linkAge != 0 ? 1 : 0;
    const float x = kBottleRimX;
    const float y = kBottleRimYZ[age][0];
    const float z = kBottleRimYZ[age][1];
    for (int i = 0; i < 3; i++) {
        out3[i] = m[0][i] * x + m[1][i] * y + m[2][i] * z + m[3][i];
    }
    return true;
}

// The bottle's UP axis in world: held items are authored along the hand limb's local +X (the
// sword blade sits at +5000 on X in func_80090A28), and the bottle display list lives in that
// same hand space, so the limb matrix's first column is the bottle's base -> mouth direction.
bool BottleUpWorld(float* out3) {
    float m[4][4];
    if (!VR_GetHandMatrix(BottleHand(), m)) {
        return false;
    }
    const float len = std::sqrt(m[0][0] * m[0][0] + m[0][1] * m[0][1] + m[0][2] * m[0][2]);
    if (len < 0.0001f) {
        return false;
    }
    out3[0] = m[0][0] / len;
    out3[1] = m[0][1] / len;
    out3[2] = m[0][2] / len;
    return true;
}

bool sInvertedNow = false; // last tick's inversion verdict, for the marker

float CatchRadiusUnits() {
    return CVarGetFloat("gVrBottleCatchRadius", 20.0f) * 0.01f * WorldScale();
}

// Point on the sampled mouth segment at parameter t (0 = previous sample, 1 = current).
void MouthAt(float t, float* out3) {
    if (!sMouthPrevValid) {
        t = 1.0f;
    }
    for (int i = 0; i < 3; i++) {
        out3[i] = sMouthPrev[i] + (sMouthCur[i] - sMouthPrev[i]) * t;
    }
}

// Catch volume test for one actor against one mouth point. Fairies, fish and bugs are small
// bodies: a sphere of the catch radius around their position. Blue fire is a flame column
// (vanilla capturable-flame collider: radius 25, height 80): a cylinder padded by the radius.
bool PointInCatchVolume(const Actor* actor, const float* p, float radius) {
    const float dx = p[0] - actor->world.pos.x;
    const float dy = p[1] - actor->world.pos.y;
    const float dz = p[2] - actor->world.pos.z;
    if (actor->id == ACTOR_EN_ICE_HONO) {
        const float r = 20.0f + radius;
        return (dx * dx + dz * dz) <= r * r && dy >= -radius && dy <= 70.0f + radius;
    }
    return (dx * dx + dy * dy + dz * dz) <= radius * radius;
}

bool AnyMouthSampleInVolume(const Actor* actor, float radius) {
    if (!sMouthCurValid) {
        return false;
    }
    const int steps = sMouthPrevValid ? 5 : 1;
    for (int s = 0; s < steps; s++) {
        float p[3];
        MouthAt(steps == 1 ? 1.0f : (float)s / (float)(steps - 1), p);
        if (PointInCatchVolume(actor, p, radius)) {
            return true;
        }
    }
    return false;
}

// Normal selector play with a bottle of any contents passively in hand (no half-finished item
// change). Everything bottle-shaped starts from here.
bool BottleInNormalPlay(Player* player) {
    if (!VrItemSelect_ModeActive() || player == NULL || player->actor.category != ACTORCAT_PLAYER ||
        gPlayState == NULL) {
        return false;
    }
    if (!VrItemSelect_SelectionAllowed()) {
        return false; // scripted control, transitions, ocarina
    }
    if (gPlayState->shootingGalleryStatus != 0 || gPlayState->bombchuBowlingStatus != 0 ||
        (player->stateFlags1 & (PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_IN_WATER))) {
        return false; // vanilla refuses bottle use while swimming; minigames keep their schemes
    }
    return player->heldItemAction >= PLAYER_IA_BOTTLE && player->heldItemAction <= PLAYER_IA_BOTTLE_FAIRY &&
           player->heldItemAction == player->itemAction;
}

// Contents that physically leave the bottle on use: the vanilla drop action (fish, blue fire,
// bugs) and the fairy release. Everything else is drunk or shown.
bool PourableContents(Player* player) {
    switch (player->heldItemAction) {
        case PLAYER_IA_BOTTLE_FISH:
        case PLAYER_IA_BOTTLE_FIRE:
        case PLAYER_IA_BOTTLE_BUG:
        case PLAYER_IA_BOTTLE_FAIRY:
            return true;
        default:
            return false;
    }
}

// Contents that are swallowed on use: the vanilla drink action. Big Poe and Ruto's Letter are
// shown to NPCs and belong to neither gesture.
bool DrinkableContents(Player* player) {
    switch (player->heldItemAction) {
        case PLAYER_IA_BOTTLE_POE:
        case PLAYER_IA_BOTTLE_POTION_RED:
        case PLAYER_IA_BOTTLE_POTION_BLUE:
        case PLAYER_IA_BOTTLE_POTION_GREEN:
        case PLAYER_IA_BOTTLE_MILK_FULL:
        case PLAYER_IA_BOTTLE_MILK_HALF:
            return true;
        default:
            return false;
    }
}

// The face point: the headset eye position dropped by the face-down setting (the player's
// mouth sits below the eyes). Only meaningful in VR first person, where the camera pose IS the
// headset. Returns false if there is no VR camera to measure against.
bool FacePointWorld(float* out3) {
    if (!VR_IsInitialized()) {
        return false;
    }
    float eye[3], fwd[3], up[3];
    VR_GetCameraPose(eye, fwd, up);
    const float down = CVarGetFloat("gVrBottleDrinkFaceDown", 8.0f) * 0.01f * WorldScale();
    for (int i = 0; i < 3; i++) {
        out3[i] = eye[i] - up[i] * down;
    }
    return true;
}

void ResetSip() {
    sSipCount = 0;
    sSipTicks = 0;
    sAwayTicks = 0;
}

void ResetStroke() {
    sStrokeValid = false;
    sStrokeDir = 0;
    sStrokeCounted = false;
}

void ResetShake() {
    sShakeCount = 0;
    sTicksSinceShake = 0;
    sUprightTicks = 0;
    ResetStroke();
}

// Stroke detector on the mouth's height. Returns true exactly once per downward stroke, the
// moment the mouth has descended `amplitude` (game units) from the stroke's peak — so the
// haptic (and the pour) land mid-flick, not after the hand has already turned back. A stroke
// ends when the mouth rises `hysteresis` above its lowest point; the next one can only begin
// after that reversal, so one long descent is one shake, never several. Before the first
// descent the peak reference simply follows the highest point seen. `travelOut` reports the
// current stroke's descent so far (0 while ascending) for the diagnostics readout.
bool DownStrokeCompleted(float y, float amplitude, float hysteresis, float* travelOut) {
    *travelOut = 0.0f;
    if (!sStrokeValid) {
        sStrokeValid = true;
        sStrokeDir = 0;
        sStrokeCounted = false;
        sStrokeStartY = sStrokeExtremeY = y;
        return false;
    }
    if (sStrokeDir == 0) {
        if (y > sStrokeStartY) {
            sStrokeStartY = y;
        }
        if (y < sStrokeStartY - hysteresis) {
            sStrokeDir = -1;
            sStrokeExtremeY = y;
            sStrokeCounted = false;
        }
    } else if (sStrokeDir == +1) {
        if (y > sStrokeExtremeY) {
            sStrokeExtremeY = y;
        }
        if (y < sStrokeExtremeY - hysteresis) {
            sStrokeDir = -1;
            sStrokeStartY = sStrokeExtremeY;
            sStrokeExtremeY = y;
            sStrokeCounted = false;
        }
    }
    if (sStrokeDir != -1) {
        return false;
    }
    if (y < sStrokeExtremeY) {
        sStrokeExtremeY = y;
    }
    const float travel = sStrokeStartY - sStrokeExtremeY;
    *travelOut = travel;
    if (!sStrokeCounted && travel >= amplitude) {
        sStrokeCounted = true;
        return true;
    }
    if (y > sStrokeExtremeY + hysteresis) {
        // Turned back up: the down stroke is over, an up stroke begins from its valley.
        sStrokeDir = +1;
        sStrokeStartY = sStrokeExtremeY;
        sStrokeExtremeY = y;
    }
    return false;
}

} // namespace

// An EMPTY bottle is passively selected in normal selector play: the physical scoop owns its
// use (the trigger mirror stands down) and GI_MAX offers measure against the mouth. Never
// covers a filled bottle — contents keep their vanilla trigger path (plus the pour gesture).
extern "C" bool VrBottle_Covers(Player* player) {
    return CVarGetInteger("gVrPhysBottleScoop", 1) && BottleInNormalPlay(player) &&
           player->heldItemAction == PLAYER_IA_BOTTLE;
}

extern "C" void VrBottle_Reset(void) {
    sMouthCurValid = sMouthPrevValid = false;
    sCovers = false;
    sNearThisTick = false;
    sWasNear = false;
    sRecatchTicks = 0;
    ResetShake();
    ResetSip();
    sInvertedNow = false;
    sAtFaceNow = false;
}

// Coarse pre-check for the actors' own "is Link close enough to bother offering" gates
// (fish/bugs: 32 units from the body, blue fire: 40): true when the mouth is anywhere near.
// Cheap — uses this tick's cached mouth, no runtime calls, so per-actor per-frame is fine.
extern "C" bool VrBottle_InReach(Actor* actor) {
    if (!sCovers || actor == NULL || !sMouthCurValid || sRecatchTicks > 0) {
        return false;
    }
    const float reach = CatchRadiusUnits() + 30.0f;
    const float dx = sMouthCur[0] - actor->world.pos.x;
    const float dy = sMouthCur[1] - actor->world.pos.y;
    const float dz = sMouthCur[2] - actor->world.pos.z;
    return (dx * dx + dy * dy + dz * dz) <= reach * reach;
}

// The offer geometry (called from Actor_OfferGetItem for GI_MAX offers while covering): did the
// mouth pass through this actor's catch volume since the last tick? Also feeds the reach feel.
extern "C" bool VrBottle_MouthInVolume(Actor* actor) {
    if (!sCovers || actor == NULL || !sMouthCurValid || sRecatchTicks > 0) {
        return false;
    }
    const float radius = CatchRadiusUnits();
    if (AnyMouthSampleInVolume(actor, radius * 2.5f)) {
        sNearThisTick = true;
    }
    return AnyMouthSampleInVolume(actor, radius);
}

// Catch committed (Player_VrTryBottleCatch): a solid thunk in the bottle hand.
extern "C" void VrBottle_OnCatch(void) {
    VR_TriggerHaptic(BottleHand(), 0.7f, 0.0f, 45.0f);
    sWasNear = false;
}

// Pour and drink gestures, at the native item boundary (Player_UpdateItems, the same seam the
// bomb grab commits from). Pour: while a pourable bottle is inverted past gVrBottleInvertDeg,
// each completed downward stroke of the mouth of at least gVrBottleShakeAmplitude (distance,
// not speed — slow dips count) is one shake and ticks the hand; the third empties the bottle
// immediately at the mouth (Player_VrBottlePourOut). Righting the bottle or pausing the rhythm
// for 2 s resets the count. Drink: while a drinkable bottle's mouth is at the face, sips tick
// the hand (arrival, then every sip interval); the third swallows (Player_VrBottleDrink).
// Nothing is spent until the third shake / sip.
extern "C" void VrBottle_Tick(PlayState* play, Player* player) {
    // Gates, in order, each named for the readout so a silent gesture can be diagnosed in-headset.
    // The contents decide which gesture applies: pourables shake out, drinkables sip.
    const bool pourOn = CVarGetInteger("gVrPhysBottlePour", 1) != 0;
    const bool drinkOn = CVarGetInteger("gVrPhysBottleDrink", 1) != 0;
    int gate = 0;
    int kind = 0;
    if (!pourOn && !drinkOn) {
        gate = 1;
    } else if (!BottleInNormalPlay(player)) {
        gate = 2;
    } else if (PourableContents(player)) {
        kind = 0;
        if (!pourOn) {
            gate = 6;
        }
    } else if (DrinkableContents(player)) {
        kind = 1;
        if (!drinkOn) {
            gate = 6;
        }
    } else {
        gate = 3;
    }
    if (gate == 0 && (play->pauseCtx.state != 0 || player->unk_6AD != 0 || VrItemSelect_PendingSlot() != -2)) {
        gate = 4;
    }
    float mouth[3], axis[3], face[3];
    if (gate == 0 && (!MouthAnchorWorld(mouth) || !BottleUpWorld(axis) || (kind == 1 && !FacePointWorld(face)))) {
        gate = 5;
    }
    sDebug.kind = kind;
    if (gate != 0) {
        ResetShake();
        ResetSip();
        sInvertedNow = false;
        sAtFaceNow = false;
        sDebug.gate = gate;
        sDebug.inverted = 0;
        sDebug.shakeCount = 0;
        sDebug.strokeCm = 0.0f;
        sDebug.atFace = 0;
        sDebug.sipCount = 0;
        return;
    }
    sDebug.gate = 0;

    if (kind == 1) {
        // Drink: the mouth within the drink radius of the face point. Arrival is sip one; each
        // sip interval it stays there is another; the third swallows. Wobbling out of the radius
        // for a few ticks keeps the count (grace); longer than that and the count starts over.
        ResetShake();
        sInvertedNow = false;
        const int hand = BottleHand();
        const float u = 0.01f * WorldScale();
        const float radius = CVarGetFloat("gVrBottleDrinkDistance", 10.0f) * u;
        const float dx = mouth[0] - face[0];
        const float dy = mouth[1] - face[1];
        const float dz = mouth[2] - face[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const bool atFace = dist <= radius;
        sAtFaceNow = atFace;
        sDebug.atFace = atFace ? 1 : 0;
        sDebug.faceCm = dist / u;
        sDebug.drinkDistanceCm = radius / u;
        if (!atFace) {
            if (sSipCount > 0 && ++sAwayTicks > kAwayGraceTicks) {
                ResetSip();
            }
            sDebug.sipCount = sSipCount;
            return;
        }
        sAwayTicks = 0;
        int intervalTicks = (int)(CVarGetFloat("gVrBottleSipInterval", 0.45f) * 20.0f + 0.5f);
        if (intervalTicks < 2) {
            intervalTicks = 2;
        }
        bool sip = false;
        if (sSipCount == 0) {
            sip = true; // arrival
        } else if (++sSipTicks >= intervalTicks) {
            sip = true;
        }
        if (!sip) {
            sDebug.sipCount = sSipCount;
            return;
        }
        sSipCount++;
        sSipTicks = 0;
        sDebug.sipCount = sSipCount;
        if (sSipCount < kSipsToDrink) {
            VR_TriggerHaptic(hand, 0.4f, 0.0f, 30.0f);
            return;
        }
        // Third sip: the swallow. Effect and bottle state settle right now (Player_VrBottleDrink),
        // no body animation, no cutscene state.
        ResetSip();
        sDebug.sipCount = 0;
        const int32_t drunk = Player_VrBottleDrink(play, player);
        sDebug.lastDrinkResult = drunk;
        if (drunk) {
            VR_TriggerHaptic(hand, 0.8f, 0.0f, 60.0f);
        }
        return;
    }

    ResetSip();
    sAtFaceNow = false;
    sDebug.atFace = 0;
    sDebug.sipCount = 0;
    const float invertCos = std::cos(CVarGetFloat("gVrBottleInvertDeg", 50.0f) * 3.14159265f / 180.0f);
    const bool inverted = axis[1] < -invertCos; // the bottle's up axis pointing down
    sInvertedNow = inverted;
    sDebug.axisY = axis[1];
    sDebug.invertThreshold = -invertCos;
    sDebug.inverted = inverted ? 1 : 0;
    if (inverted) {
        sUprightTicks = 0;
    } else {
        // A flick can tilt the bottle out of the cone for a tick or two at the ends of a
        // stroke; keep measuring through a short grace, and only a real righting resets.
        if (++sUprightTicks > kUprightGraceTicks || !sStrokeValid) {
            ResetShake();
            sDebug.shakeCount = 0;
            sDebug.strokeCm = 0.0f;
            return;
        }
    }

    const int hand = BottleHand();
    const float u = 0.01f * WorldScale(); // cm -> game units
    const float amplitude = CVarGetFloat("gVrBottleShakeAmplitude", 9.0f) * u;
    // Reversal hysteresis: a fifth of a shake, never below 0.5 cm (tracking jitter) nor above
    // 2 cm (so a small return bounce still separates two strokes).
    float hysteresis = amplitude * 0.2f;
    if (hysteresis < 0.5f * u) {
        hysteresis = 0.5f * u;
    } else if (hysteresis > 2.0f * u) {
        hysteresis = 2.0f * u;
    }
    float travel = 0.0f;
    const bool stroke = DownStrokeCompleted(mouth[1], amplitude, hysteresis, &travel);
    sDebug.strokeCm = travel / u;
    if (sShakeCount > 0 && ++sTicksSinceShake > kRhythmTimeoutTicks) {
        // The rhythm stopped: start the count over. The stroke tracker keeps its place so the
        // next shake is still measured from a true peak.
        sShakeCount = 0;
        sTicksSinceShake = 0;
    }
    sDebug.shakeCount = sShakeCount;
    if (!stroke) {
        return;
    }
    sShakeCount++;
    sTicksSinceShake = 0;
    sDebug.shakeCount = sShakeCount;
    sDebug.lastStrokeCm = travel / u;
    if (sShakeCount < kShakesToPour) {
        VR_TriggerHaptic(hand, 0.4f, 0.0f, 30.0f);
        return;
    }
    // Third shake: the commitment point. Contents leave the bottle at the mouth right now —
    // no body animation, no cutscene state (Player_VrBottlePourOut spawns what the vanilla
    // drop / fairy-release actions spawn at their key frames and settles the held state).
    ResetShake();
    sDebug.shakeCount = 0;
    const int32_t poured = Player_VrBottlePourOut(play, player, mouth);
    sDebug.lastPourResult = poured;
    if (poured) {
        VR_TriggerHaptic(hand, 0.8f, 0.0f, 60.0f);
        sRecatchTicks = (int)(CVarGetFloat("gVrBottleRecatchDelay", 1.5f) * 20.0f + 0.5f);
    }
}

extern "C" void VrBottle_GetDebug(VrBottleDebug* out) {
    if (out != nullptr) {
        *out = sDebug;
    }
}

// Mouth marker: a miniature Deku Nut at the bottle mouth while a bottle is out, growing when a
// catchable is in the near band. Deliberately minimal gates (mode + cvar + bottle in hand): it
// is the visual check that the mouth sits on the model's opening and the liveness diagnostic when the
// offer/commit gates misbehave. extern "C" linkage is load-bearing for the block-scope
// FrameInterpolation declarations inside OPEN_DISPS (see VrItemSelect_Draw).
extern "C" void VrBottle_DrawMouthMarker(void) {
    if (gPlayState == NULL || !CVarGetInteger("gVrBottleShowMouth", 1) || !VrItemSelect_ModeActive()) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == NULL || player->heldItemAction < PLAYER_IA_BOTTLE || player->heldItemAction > PLAYER_IA_BOTTLE_FAIRY) {
        return;
    }
    float anchor[3];
    if (!MouthAnchorWorld(anchor)) {
        return;
    }

    OPEN_DISPS(gPlayState->state.gfxCtx);
    FrameInterpolation_RecordOpenChild((const void*)&sMouthCur, 0);
    Matrix_Translate(anchor[0], anchor[1], anchor[2], MTXMODE_NEW);
    Matrix_ReplaceRotation(&gPlayState->billboardMtxF);
    float iconScale = 0.0003f * CVarGetFloat("gVrBottleIconScale", 12.0f);
    if (sWasNear) {
        iconScale *= 1.5f;
    }
    if (sInvertedNow || sAtFaceNow) {
        iconScale *= 1.6f; // gesture diagnostic: big marker = the game agrees (upside down / at the face)
    }
    Matrix_Scale(iconScale, iconScale, iconScale, MTXMODE_APPLY);
    POLY_OPA_DISP = Play_SetFog(gPlayState, POLY_OPA_DISP);
    POLY_OPA_DISP = Gfx_SetupDL_66(POLY_OPA_DISP);
    gSPSegment(POLY_OPA_DISP++, 0x08, (uintptr_t)SEGMENTED_TO_VIRTUAL(gDropDekuNutTex));
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(gPlayState->state.gfxCtx), G_MTX_MODELVIEW | G_MTX_LOAD);
    gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gItemDropDL);
    FrameInterpolation_RecordCloseChild();
    CLOSE_DISPS(gPlayState->state.gfxCtx);
}

namespace {

// Once per game tick, right after the player actor updates and BEFORE the catchable actors
// update (they are later categories), so their offers this frame test the freshest segment.
void BottleTick() {
    if (gPlayState == NULL || !GameInteractor::IsSaveLoaded(true)) {
        VrBottle_Reset();
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (!BottleInNormalPlay(player)) {
        VrBottle_Reset();
        return;
    }
    sCovers = VrBottle_Covers(player);
    if (gPlayState->pauseCtx.state != 0) {
        return; // nothing offers while paused; keep the path where it was
    }
    if (sRecatchTicks > 0) {
        sRecatchTicks--;
    }

    float anchor[3];
    if (MouthAnchorWorld(anchor)) {
        if (sMouthCurValid) {
            for (int i = 0; i < 3; i++) {
                sMouthPrev[i] = sMouthCur[i];
            }
            sMouthPrevValid = true;
        }
        for (int i = 0; i < 3; i++) {
            sMouthCur[i] = anchor[i];
        }
        sMouthCurValid = true;
    } else {
        sMouthCurValid = sMouthPrevValid = false;
    }

    // Reach feel: soft tick the moment the mouth comes near a catchable (flag set by last
    // frame's offer tests), pairing with the marker growing.
    if (sNearThisTick && !sWasNear) {
        VR_TriggerHaptic(BottleHand(), 0.2f, 0.0f, 15.0f);
    }
    sWasNear = sNearThisTick;
    sNearThisTick = false;
}

void RegisterVrBottle() {
    COND_HOOK(OnPlayerUpdate, true, BottleTick);
    COND_HOOK(OnPlayDrawEnd, true, VrBottle_DrawMouthMarker);
}

static RegisterShipInitFunc initVrBottle(RegisterVrBottle);

} // namespace
