#pragma once

// VR Physical Combat (gVrPhysCombat): motion-driven melee, shield, carrying and archery,
// replacing the animation-driven systems while active. This header is the module's surface for
// both the C decomp code (master predicate + per-frame shims, added milestone by milestone) and
// the module's own C++ files (soh/soh/Enhancements/vr-combat/).
//
// Game types appear only as pointers to tagged structs (struct Player / struct PlayState), so
// the header stays includable from any TU; consumers that pass real objects naturally already
// include the game headers.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Master predicate: true only when every physical-combat patch should take the VR path THIS
// frame — VR first person with the toggle on, a save loaded, and no state where the game owns
// Link's hands (cutscene, forced action, screen transition). Decomp patches call this and fall
// back to vanilla when false, so scripted sequences behave stock frame-by-frame.
bool VrCombat_Active(void);

// --- Physical melee (VrSwing.cpp) ---

// The held melee weapon is handled by physical combat: swords (Master/Kokiri/Biggoron, incl.
// the broken Giant's Knife) and the Deku stick. The hammer keeps vanilla button combat until
// its two-hand milestone. Also false for the co-op partner (only the real player swings).
bool VrCombat_MeleeCovered(struct Player* player);

// Implemented in z_player.c (// SOH [VR]): forwards to the vanilla landed-hit durability
// composite func_80842CF0 — the Deku stick snaps (half-stick effect, ammo, put away) and the
// unbroken Giant's Knife wears toward breaking. No-op for every other weapon, so the physical
// path calls it on real impacts without weapon-specific dispatch.
void VrCombat_MeleeImpactConsume(struct PlayState* play, struct Player* player);

// Draw-time feed, called from the Player L_HAND PostLimbDraw seam with the live (controller)
// matrix on the stack and the blade length already configured (D_80126080.x). Tracks swing
// speed, mirrors meleeWeaponState for enemy AI, feeds the sword trail, and registers velocity-
// gated swept sub-quads as AT colliders.
void VrCombat_FeedMelee(struct PlayState* play, struct Player* player);

// True while any physical-melee quad has an AT hit recorded this tick. OR'd into the vanilla
// "landing an attack cancels incoming damage this frame" check, which only knows about the
// player's own two quads.
bool VrCombat_MeleeQuadsHit(void);

// Implemented in z_player.c (// SOH [VR]): forwards to the file-internal melee-weapon-state
// setter func_80833A20, which plays the swing SFX / voice and counts the swing stat on the
// 0 -> nonzero edge. The vr-combat module drives the state from real hand motion.
void VrCombat_SetMeleeWeaponState(struct Player* player, int32_t newState);

// Visual-mesh harvest exclusion: brackets a display-list section the physical blade must NOT
// collide with (the player's own arms/weapon, the sword trail) by emitting mask marker
// commands into the OPA and XLU buckets. No-ops while physical combat is inactive.
void VrCombat_MeshMaskPush(struct GraphicsContext* gfxCtx);
void VrCombat_MeshMaskPop(struct GraphicsContext* gfxCtx);
// Marks a display-list section as FLESH for the visual-mesh harvest (enemy/NPC bodies):
// contacts stay silent (no stone clank/sparks) and read as bodies to the impact pipeline.
void VrCombat_MeshFleshPush(struct GraphicsContext* gfxCtx);
void VrCombat_MeshFleshPop(struct GraphicsContext* gfxCtx);

// Limb puppetry: the blade manipulates individual limbs of nearby NPC/enemy skeletons —
// rest the blade under an arm and the arm rides it; push a head and it tilts away; hits kick
// the limbs near the impact. The Warp pair brackets each limb's matrix conversion in
// z_skelanime.c: Begin records this limb's world position for the contact solve (the clean
// animated pose) and then offsets the matrix translation by the limb's current puppet offset;
// End restores it so child limbs compute cleanly. Purely visual: animation, hitboxes and AI
// never see the offsets. `actorArg` is compared before any dereference against actors the
// contact gather validated this tick.
void VrCombat_FlinchWarpBegin(struct PlayState* play, void* actorArg, int32_t limbIndex);
void VrCombat_FlinchWarpEnd(void);

// M4 physical shield. ShieldHeld: the shield is physically in the off hand — this widens the
// vanilla PLAYER_STATE1_SHIELDING gates on the shield hand model and the shield quad, so the
// quad registers from the off-hand controller pose every frame with no stance, no button.
// ShieldFacingVeto: the facing gate, called from CollisionCheck_SetATvsAC for every confirmed
// AT-vs-AC hit (one pointer compare for everything that isn't the physical shield quad). True
// when the attack comes from outside the shield's facing cone (gVrPhysShieldFacingDeg): the
// hit is then dropped at the source — no bounce, no enemy recoil, the attack passes the
// shield as if it weren't there. ShieldBlockJudge: called from func_808382DC when the quad
// DID bounce an attack — 0 = not physical (vanilla handling), 1 = physical block (haptic
// fired; caller keeps damage negation + Deku burn, skips the stance reaction anim and shove).
bool VrCombat_ShieldHeld(struct Player* player);
bool VrCombat_ShieldFacingVeto(void* acCollider, void* atCollider);
int32_t VrCombat_ShieldBlockJudge(struct Player* player);

// Alyx-style item selector (VrItemSelect.cpp — VR first person, independent of physical
// combat): hold the configured VR input, flick the hand toward an item, release to take it.
// True when the given VR input on the given hand is DEDICATED to the selector — padmgr skips
// that input's normal button binding so the opening click never leaks its bound action.
bool VrItemSelect_ConsumesInput(int32_t vrHand, uint16_t vrBtnMask);

// SELECTOR MODE (gVrItemSelect on, VR first person): the selector equips and the TRIGGER of the
// hand the item ended up in uses it, replacing "press the C button you assigned it to".
// ModeActive: the mode owns item activation this frame (third person and flat screen stay stock).
// TriggerItemMask: the N64 button the item held in this hand answers to, or 0 — padmgr ORs it into
// the pad while that hand's trigger is DOWN, as button state rather than a one-frame press, so
// press/hold/release all come out of the vanilla item path (that is what makes draw-and-hold and
// release-to-fire work with nothing re-implemented). Returns 0 for weapons physical combat covers,
// where the swing is the attack. TriggerConsumed: both triggers are reserved in selector mode, so
// padmgr skips their normal bindings — the selector-mode binding profile rehouses Z-target and
// friends on the grips and face buttons.
bool VrItemSelect_ModeActive(void);
// Explicit selection lifecycle. Requests identify an equipped button slot (0..3), or -1
// for empty hands. The player update owns execution; a newer request replaces the old one.
void VrItemSelect_Request(int32_t slot);
int32_t VrItemSelect_PendingSlot(void);
int32_t VrItemSelect_PendingItem(void);
void VrItemSelect_FinishRequest(void);
void VrItemSelect_CancelRequest(void);
void VrItemSelect_Reset(void);
bool VrItemSelect_SelectionAllowed(void);
// Native player bridge: passive equip and safe cancellation, never a simulated item press.
// 0 = wait, 1 = equipment changed, 2 = rejected/idempotent (preserve held input).
int32_t Player_VrSelectItem(struct PlayState* play, struct Player* player, int32_t slot);
void Player_VrCancelPreparedItem(struct PlayState* play, struct Player* player);
// Falling edge of selector mode (third person, flat screen, F9): clears Link's hands.
// Gently releases a carried throwable (zero impulse), cancels prepared aiming without
// firing or spending, then vanilla put-away where safe. Never touches hookshot flight.
void Player_VrModeExitClearHands(struct PlayState* play, struct Player* player);
// Preview/grip adapter for bombs, nuts and bombchus (the chu drops and crawls instead of
// throwing; gVrPhysBombchuDrop gates it alone). Tick runs at the native item boundary.
bool VrItemThrow_Active(struct Player* player);
void VrItemThrow_Tick(struct PlayState* play, struct Player* player);
void VrItemThrow_Reset(void);
bool VrItemThrow_PreviewPosition(float* position);
// True while the presented item is a real model (the bombchu) rather than an item icon; the
// selector's passive icon stands down and VrItemThrow_DrawPreview draws it.
bool VrItemThrow_PreviewIsModel(void);
void VrItemThrow_DrawPreview(void);
bool VrItemThrow_GripConsumed(int32_t hand, uint16_t mask);
void VrItemThrow_UpdateCarryPose(struct Player* player);
bool Player_VrGrabItem(struct PlayState* play, struct Player* player);
void Player_VrReleaseItem(struct PlayState* play, struct Player* player, const float* velocity);
uint16_t VrItemSelect_TriggerItemMask(int32_t vrHand);
bool VrItemSelect_TriggerConsumed(int32_t vrHand, uint16_t vrBtnMask);

// Quick swap to sword & shield (selector mode): squeezing the configured input
// (gVrItemSelSwapInput, default grip) on BOTH controllers at once stows the held item and draws
// the sword. True while the full chord is down for a matching input — padmgr skips its normal
// binding on both hands, so the swap doesn't also emit Z-target / R. Single-hand squeezes keep
// their bindings untouched.
bool VrItemSelect_SwapConsumed(int32_t vrHand, uint16_t vrBtnMask);

// True while the ocarina interface is up (free play, song playback, scarecrow recording, the
// frog and Skull Kid minigames — every msgMode from OCARINA_STARTING through FROGS_WAITING).
// While true, padmgr swaps to the dedicated OCARINA binding set (sVrBindOca*) and every
// selector-mode input reservation stands down, so notes can live on any input — including the
// triggers and the selector's own click.
bool VrOcarina_InPlay(void);

// Physical archery (VrArchery.cpp — selector mode): the string hand pinches near the bow hand
// to nock; while nocked the weapon's item button reads held-down (padmgr ORs ItemButtonMask as
// raw state, the same mirror trick the trigger uses), so the vanilla draw/hold/release pipeline
// runs untouched. Covers: bow/slingshot in normal selector play (galleries/bowling/horseback
// excluded, gVrPhysArchery toggles). PinchConsumed: the string hand's pinch input loses its
// binding only near the weapon or while drawn. AimSegment: origin + direction of the
// string->bow line while nocked (consumed by Player_VrAimHeldProjectile; false = fall back to
// the one-hand aim ray). Reset clears transient nock state.
bool VrArchery_Covers(struct Player* player);
bool VrArchery_StringNocked(void);
uint16_t VrArchery_ItemButtonMask(void);
bool VrArchery_PinchConsumed(int32_t vrHand, uint16_t vrBtnMask);
bool VrArchery_AimSegment(float* outPosDir6);
void VrArchery_Reset(void);

// Physical bottle scooping (VrBottle.cpp — selector mode): an EMPTY bottle catches by
// moving its MOUTH (bottle-hand controller + grip-local offset) into a catchable's volume — no
// swing, no button. Covers: empty bottle passively selected in normal play (gVrPhysBottleScoop
// toggles; swimming/horse/minigames excluded). InReach: coarse mouth proximity for the actors'
// own "bother offering?" gates. MouthInVolume: the offer geometry Actor_OfferGetItem uses for
// GI_MAX offers while covering (segment since last tick vs. the actor's catch volume). OnCatch:
// commit haptic. Player_VrTryBottleCatch (z_player.c): resolves a mouth-measured offer through
// the vanilla catch action from the use-item action handler; 1 = an action was set up.
// Tick (from Player_UpdateItems): the pour gesture — bottle inverted past gVrBottleInvertDeg,
// three downward strokes of the mouth of at least gVrBottleShakeAmplitude (haptic per shake),
// the third empties the bottle on the spot through Player_VrBottlePourOut (z_player.c):
// fish/bug/blue fire/fairy spawn at the mouth immediately, held state settled, no animation.
// 1 = contents left the bottle. Also the drink gesture — the mouth held within
// gVrBottleDrinkDistance of the face (headset eye point, gVrBottleDrinkFaceDown below it):
// a sip haptic on arrival and every gVrBottleSipInterval seconds after, the third sip swallows
// through Player_VrBottleDrink (z_player.c): potion / milk / poe effect applied on the spot, the
// bottle emptied (full milk -> half), no animation. 1 = contents were drunk. GetDebug: live
// gesture state for the settings menu readout.
bool VrBottle_Covers(struct Player* player);
void VrBottle_Tick(struct PlayState* play, struct Player* player);
int32_t Player_VrBottlePourOut(struct PlayState* play, struct Player* player, const float* mouth);
int32_t Player_VrBottleDrink(struct PlayState* play, struct Player* player);
typedef struct VrBottleDebug {
    int32_t gate; // 0 = a gesture is armed; 1 both gestures switched off; 2 no bottle in normal
                  // play; 3 contents neither pour nor drink; 4 paused / Link busy / item change
                  // pending; 5 bottle hand untracked; 6 the contents' gesture is switched off
    int32_t kind;           // which gesture the contents belong to: 0 pour, 1 drink
    int32_t inverted;       // the game currently agrees the bottle is upside down
    float axisY;            // bottle base->mouth axis, world Y component (+1 mouth up, -1 mouth down)
    float invertThreshold;  // axisY must be below this to count as upside down
    int32_t shakeCount;     // valid shakes so far (0..2; the third pours and resets)
    float strokeCm;         // the current downward stroke's descent so far
    float lastStrokeCm;     // descent of the last stroke that counted as a shake
    int32_t lastPourResult; // -1 none yet, 0 Player_VrBottlePourOut refused, 1 poured
    int32_t atFace;         // the mouth is currently within the drink distance of the face
    float faceCm;           // mouth -> face point distance
    float drinkDistanceCm;  // must be below this to count as at the face
    int32_t sipCount;       // sips so far (0..2; the third swallows and resets)
    int32_t lastDrinkResult; // -1 none yet, 0 Player_VrBottleDrink refused, 1 drunk
} VrBottleDebug;
void VrBottle_GetDebug(VrBottleDebug* out);
bool VrBottle_InReach(struct Actor* actor);
bool VrBottle_MouthInVolume(struct Actor* actor);
void VrBottle_OnCatch(void);
void VrBottle_Reset(void);
int32_t Player_VrTryBottleCatch(struct PlayState* play, struct Player* player);

// Physical boomerang (VrBoomerang.cpp — selector mode). The boomerang has no held actor in
// vanilla (selection only sets a flag; EnBoom exists from the throw to the return), so it gets
// its own VR-only state machine instead of the VrItemThrow carry path: POCKET — selecting it
// presents a shrunken gBoomerangRefDL at the bombchu's pocket point (Link's fist model hidden);
// HELD — a fresh grip within reach takes it as a virtual carry, drawn rigidly parented to the
// hand (VrPocket::Grab); the carry hand's grip release with hand speed >= gVrBoomerangThrowMinSpeed
// throws (Player_VrThrowBoomerang: the vanilla frame-6 spawn with direction from the hand's
// velocity, vanilla lock-on homing, speed, stun, fetch, bounce), a still release puts it back
// in the pocket, and so do switching, the sword chord, F9 and a save-state load; THROWN — the
// module feeds the actor the catch hand every tick (EnBoom vr* fields) so the return leg flies
// to the HAND; grip closed as it arrives = caught into that hand (ready to throw again), grip
// open = vanilla end and the pocket re-presents it. Returning while another item is selected
// keeps vanilla's silent recovery plus a catch sound and a light haptic. Covers: boomerang
// selected in normal selector play (galleries/bowling/horse/water excluded; gVrPhysBoomerang
// toggles). Tick runs at the native item boundary next to VrItemThrow_Tick. PreviewIsModel:
// the pocket is showing (selector icon stands down). HidesHandModel: the module owns the
// boomerang model this frame (pocket / hand / flight), so Link's fist draws open.
// TriggerStandsDown: the trigger mirror must not start the vanilla aim while the boomerang is
// pocketed or carried (it stays on in flight for FastBoomerang's recall, after the trigger has
// been seen up once). GripConsumed: the grip loses its binding where it grabs, carries or
// catches. Draw: the pocket / carried model on its own OnPlayDrawEnd hook.
bool VrBoomerang_Covers(struct Player* player);
void VrBoomerang_Tick(struct PlayState* play, struct Player* player);
void VrBoomerang_Reset(void);
bool VrBoomerang_PreviewIsModel(void);
bool VrBoomerang_HidesHandModel(void);
bool VrBoomerang_TriggerStandsDown(void);
bool VrBoomerang_GripConsumed(int32_t hand, uint16_t mask);
void VrBoomerang_Draw(void);
typedef struct VrBoomerangDebug {
    int32_t state;         // 0 none, 1 pocket, 2 held, 3 thrown
    int32_t gate;          // 0 armed; 1 Physical Boomerang off; 2 boomerang not selected in normal play;
                           // 3 paused / Link busy (state kept); 4 a boomerang not thrown by the hand is
                           // in flight; 5 no pocket point (headset untracked)
    int32_t carryHand;     // -1 none, 0 left, 1 right
    int32_t throwHand;     // hand of the current / last throw, -1 none
    float lastReleaseSpeed; // hand speed at the last carry release, m/s
    float throwMinSpeed;    // must reach this to throw (else back to the pocket)
    int32_t lastRelease;   // -1 none yet, 0 back to the pocket (too slow), 1 thrown, 2 throw refused
    int32_t returnLeg;     // the thrown boomerang is on its way back
    float handDistanceCm;  // returning boomerang -> catch hand
    float catchRadiusCm;   // must sweep within this of the hand to arrive
    int32_t catchArmed;    // the catch hand's grip is closed
    int32_t lastReturn;    // -1 none yet, 0 missed (pocket), 1 caught (hand), 2 returned while another item was selected
} VrBoomerangDebug;
void VrBoomerang_GetDebug(VrBoomerangDebug* out);
// z_player.c: the vanilla throw commit (func_808359FC frame 6) from a hand position and unit
// direction. Returns the spawned EnBoom or NULL (guards refused / spawn failed).
struct EnBoom* Player_VrThrowBoomerang(struct PlayState* play, struct Player* player, const float* pos,
                                       const float* dir);

// Projectile fire (VR first person, independent of physical combat): the walk-while-aiming
// path only fires on the vanilla item-button RELEASE, so the aim hand's trigger is wired in
// as the natural VR fire. FirePressed: rising edge of that trigger this tick (checked in the
// bow/slingshot/hookshot aim action). AimTriggerConsumed: true while READY_TO_FIRE for the
// aim hand's trigger — padmgr skips its normal binding so a shot can't also toggle Z-target.
bool VrCombat_ProjectileFirePressed(struct Player* player);
bool VrCombat_AimTriggerConsumed(int32_t vrHand, uint16_t vrBtnMask);
// The physical shield's block collider, built parametrically from the Shield sliders in
// R_HAND limb model space (outXyz4 = 4 vertices x xyz, vanilla zigzag order) — replaces the
// vanilla stance quad, whose size/offset never matched a controller-held shield. Strictness =
// dimensions smaller than the visible shield: rim grazes miss the collider entirely.
void VrCombat_ShieldQuadModelVerts(float* outXyz4);

#ifdef __cplusplus
}

// C++-side module internals (swing tracking, debug overlay, menu readouts).
#include <vr_interface.h>

// Global-scope forward declarations, NOT `struct X` inside the namespace: an elaborated type
// specifier in a namespace-scope parameter would silently declare a fresh VrCombat::X in any TU
// that includes this header before the game headers.
struct PlayState;
struct Player;

namespace VrCombat {

// Hand path drained this game tick: world-space samples at headset rate, oldest first. Drained
// exactly once per tick into this snapshot so every consumer sees the same data. 24 covers a
// 20 Hz tick of 120 Hz XR frames (~6) with generous slack for hitches.
struct TickPath {
    VrHandSample samples[24];
    int count;
};
const TickPath& GetTickPath(int hand);

// Per-tick swing bookkeeping (VrSwing.cpp), driven from VrCombat's OnPlayerUpdate hook:
// reads back last tick's quad results (hit haptics, one-hit-per-swing demotion), then resets
// the quads' AT flags for the coming draw.
void Swing_OnPlayerUpdate(PlayState* play, Player* player);
// Falling edge of VrCombat_Active(): hand melee state back to vanilla (state 0, trail off).
void Swing_Deactivate(PlayState* play, Player* player);

// Per-tick shield bookkeeping (VrShield.cpp): keeps the shield model in the off hand while
// physically held, restores the vanilla models on the falling edge.
void Shield_OnPlayerUpdate(PlayState* play, Player* player);
void Shield_Deactivate(PlayState* play, Player* player);

// Debug-overlay snapshots (this tick): the blade's contact-primitive set, and this draw's
// registered damage quads as 12 floats each (4 verts x xyz). Plain float arrays so this header
// never needs the game's Vec3f type.
int Swing_GetDebugContactPrims(VrContactPrim* out, int maxPrims);
int Swing_GetDebugQuads(float* outVerts12PerQuad, int maxQuads);
// Physical blade rectangle outline (5 world-unit points: rootA, cornerA, tip, cornerB, rootB);
// returns 0 when the sim blade is inactive.
int Swing_GetDebugBladeOutline(float* outPts5x3);
// Puppet-tracked limbs (world positions + current offset magnitude per limb). Overlay proof
// of which bodies the blade can manipulate.
int Swing_GetDebugPuppetLimbs(float* outPos3PerLimb, float* outOffsetMag, int maxLimbs);

} // namespace VrCombat
#endif
