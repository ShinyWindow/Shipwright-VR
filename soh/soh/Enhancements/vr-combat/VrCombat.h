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

// Real-life hand scale (gVrRealHandScale, default on): the multiplier on Link's model scale for
// the motion hands and everything drawn on them (z_player_lib.c VR_SetHandScale). The open hand,
// wrist to fingertip, is drawn at a real size per age at ANY world scale, auto or manual: adult
// gVrHandSizeCm (gLinkAdultLeftHandNearDL, 862 model units), child gVrHandSizeCmChild
// (gLinkChildLeftHandNearDL, 548). 1 when off or out of VR (the hands then follow world scale, as
// before). Items welded to the hand (nocked arrow, idle hookshot hook) take the same factor.
// ChildSized: which age's size is in use (Link is a child).
float VrHand_ScaleFactor(void);
bool VrHand_ChildSized(void);

// --- Physical melee (VrSwing.cpp) ---

// The held melee weapon is handled by physical combat: swords (Master/Kokiri/Biggoron, incl.
// the broken Giant's Knife), the Deku stick and the Megaton Hammer (gVrPhysHammer; off = the
// hammer keeps vanilla button combat). Also false for the co-op partner (only the real player
// swings).
bool VrCombat_MeleeCovered(struct Player* player);

// Physical Megaton Hammer (VrHammer.cpp + the hammer branch of VrSwing.cpp's melee feed). A
// heavy two-handed weapon: the head is a simulated mass that trails the hands (the sim's
// torque-limited orientation spring, soft one-handed, firm two-handed) and droops under
// gravity; it never cuts through anything — it stops dead on walls, floors and bodies. The
// off hand takes the handle with a fresh grip within gVrHammerGripReach of it (lets go only on
// grip release); while held, Link's off hand is drawn pinned
// to the handle. Damage uses the vanilla hammer row (DMG_HAMMER_SWING, DMG_HAMMER_JUMP past the
// heavy speed), so rusted switches, boulders and breakable walls answer as in the base game.
// GripConsumed: the off hand's grip loses its binding (R) while it holds the handle or is within
// reach of it, and never counts toward the quick-swap chord there.
bool VrHammer_GripConsumed(int32_t hand, uint16_t mask);
typedef struct VrHammerDebug {
    int32_t gate;           // 0 armed; 1 Physical Hammer off; 2 hammer not in hand (or combat inactive)
    int32_t twoHand;        // the off hand holds the handle
    int32_t offInReach;     // the off hand is within reach of the handle (a grip would take it)
    float offHandCm;        // off hand -> nearest point on the handle
    float reachCm;          // must be within this to take hold
    float headMps;          // simulated head speed this tick
    int32_t tier;           // 0 idle, 1 armed (windup), 2 hot (a hit would land)
    float lastImpactMps;    // approach speed of the head at its last contact
    int32_t lastImpact;     // -1 none yet, 0 soft touch, 1 GROUND POUND, 2 wall strike, 3 body / object
    int32_t lastStrike;     // the last contact queued damage (0 no, 1 swing class, 2 heavy class)
    float poundSpeed;       // a floor contact at or above this approach speed pounds
    float hitSpeed;         // a contact at or above this approach speed deals damage
} VrHammerDebug;
void VrHammer_GetDebug(VrHammerDebug* out);
// Implemented in z_player.c (// SOH [VR]): the head physically met the world hard enough to count.
// ground (a floor-facing surface) = the vanilla ground pound: func_80842A28 (quake, rumble,
// NA_SE_IT_HAMMER_HIT and actorCtx.unk_02 = 4 — the "hammer hit the ground" flag nearby actors
// read: Tektites and torch slugs flip, scrubs stun, Ganon's collapsing floor cracks) plus the
// white shockwave at pos. Otherwise the vanilla wall strike: the same composite plus the hit-stop
// freeze — without the recoil shove (VR comfort) and without ending anything.
void Player_VrHammerImpact(struct PlayState* play, struct Player* player, const float* pos, int32_t ground);

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
// The live head of Link's swing trail (VrSwing.cpp FeedTrail): when the trail `blure` took an edge
// during this frame's draw, the hand whose matrix drew the blade and that edge (world units, tip =
// the trail's p1, base = p2). EffectBlure_Draw bridges from it to the blade's live edge, welded to
// the controller (gVrTrailLiveHead).
bool VrCombat_TrailLiveEdge(const void* blure, int32_t* hand, float tip[3], float base[3]);

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
// z_player_lib: weld a matrix computed this frame (curMf16, MtxF layout) to the LIVE pose of a
// VR hand, like the bowstring — for held geometry drawn outside the player (the nocked arrow).
int32_t Player_VrWeldMtxToHand(struct PlayState* play, const void* mtx, int32_t vrHand, const float* curMf16);
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
uint16_t VrArchery_ItemButtonMask(void);
bool VrArchery_PinchConsumed(int32_t vrHand, uint16_t vrBtnMask);
bool VrArchery_AimSegment(float* outPosDir6);
void VrArchery_Reset(void);
// Fairy Bow profile (geometry read off the bow model, as the slingshot's pouch is).
// BowStringNock: world point the drawn bowstring's apex belongs at (string hand, capped at max
// draw); false when no bow nock is drawn. TakeBowShotPower: draw strength (0.3..1) of the bow
// shot now leaving, consumed once by EnArrow_Shoot; 0 = not a physical bow shot (stay vanilla).
bool VrArchery_BowStringNock(float* out3);
// SlingshotPouch: world point the drawn slingshot band's pouch belongs at (string hand, capped
// at 1.25 x full draw from the braced pouch); false when no slingshot nock is drawn.
bool VrArchery_SlingshotPouch(float* out3);
float VrArchery_TakeBowShotPower(void);
// BowAlignedMatrix: while a bow nock is drawn, rewrites the bow hand's limb matrix (MtxF layout,
// in place) so the bow points along the string hand -> arrow rest line; false = leave it raw.
bool VrArchery_BowAlignedMatrix(int32_t vrHand, float* mf16);
// NockedArrowHand: the VR hand the nocked arrow is welded to (the bow hand, like the string)
// while a bow nock is drawn; -1 otherwise (the arrow draws exactly as vanilla).
int32_t VrArchery_NockedArrowHand(void);

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

// Physical Lens of Truth (VrLens.cpp — selector mode): selected, the lens model sits in the pocket;
// a fresh grip takes it; held to the face it attaches (glued to the head) and turns the lens on
// through the vanilla magic gate; a grip at the face takes it off; deselecting turns it off.
// Covers: the lens selected in normal selector play (horse/water/minigames excluded; gVrPhysLens
// toggles) — the trigger stands down while it covers. PreviewIsModel: the module draws the lens
// (pocket / hand / face), the selector icon stands down. DrawAperture (Actor_DrawLensOverlay):
// worn, the mask is drawn on a head-glued quad in the glass's plane instead of the screen rects;
// false = not worn, draw the vanilla rects.
bool VrLens_Covers(struct Player* player);
void VrLens_Tick(struct PlayState* play, struct Player* player);
void VrLens_Reset(void);
bool VrLens_PreviewIsModel(void);
bool VrLens_GripConsumed(int32_t hand, uint16_t mask);
bool VrLens_DrawAperture(struct GraphicsContext* gfxCtx);
void VrLens_Draw(void);
typedef struct VrLensDebug {
    int32_t state;         // 0 none, 1 pocket, 2 in hand, 3 worn
    int32_t gate;          // 0 armed; 1 Physical Lens off; 2 lens not selected / not in normal play;
                           // 3 cutscene / pause / Link busy (state kept); 4 no pocket point (headset untracked)
    int32_t carryHand;     // -1 none, 0 left, 1 right
    int32_t armed;         // in hand: has been away from the face, may be put on
    float glassToFaceCm;   // in hand: glass center -> worn spot (-1 = not measured)
    float wearDistanceCm;  // puts on within this
    int32_t lensActive;    // play->actorCtx.lensActive
    int32_t lastActivate;  // -1 none yet, 0 refused (no magic / magic busy), 1 on
    int32_t ours;          // this module turned the lens on
} VrLensDebug;
void VrLens_GetDebug(VrLensDebug* out);

// Physical masks (VrMask.cpp — selector mode): the lens mechanic for every mask, but the worn
// state is vanilla's currentMask, so a worn mask stays on through item switches. Selected (and not
// already worn), the mask's model sits in the pocket; a fresh grip takes it; at the face it goes on
// (replacing any other mask). A fresh grip at the face takes the worn mask off into that hand, any
// time, unless another item owns that grip. Covers: a mask selected in normal selector play
// (gVrPhysMasks toggles) — the trigger stands down. PreviewIsModel: the selector icon stands down.
bool VrMask_Covers(struct Player* player);
void VrMask_Tick(struct PlayState* play, struct Player* player);
void VrMask_Reset(void);
bool VrMask_PreviewIsModel(void);
bool VrMask_GripConsumed(int32_t hand, uint16_t mask);
void VrMask_Draw(void);
typedef struct VrMaskDebug {
    int32_t state;        // 0 none, 1 pocket, 2 in hand
    int32_t gate;         // 0 armed; 1 Physical Masks off; 2 not in selector play (or horse / water /
                          // minigame); 3 cutscene / pause / Link busy (state kept)
    int32_t carryHand;    // -1 none, 0 left, 1 right
    int32_t selected;     // PLAYER_MASK_* of the selected mask (0 none)
    int32_t held;         // PLAYER_MASK_* in the hand (0 none)
    float maskToFaceCm;   // in hand: mask middle -> face spot (-1 = not measured)
    float wearDistanceCm; // goes on within this
    int32_t worn;         // player->currentMask
    int32_t armed;        // in hand: has been away from the face
    int32_t lastWorn;     // last mask this module put on (-1 none yet)
} VrMaskDebug;
void VrMask_GetDebug(VrMaskDebug* out);

// Two-trigger spell casting (VrMagic.cpp — selector mode): Din's Fire, Farore's Wind, Nayru's
// Love cast only while BOTH triggers are down (VrItemSelect_TriggerItemMask withholds the spell's
// button otherwise). Covers: a spell selected in normal selector play (horse/water/minigames
// excluded; gVrPhysMagicChord toggles). BeginCharge (func_8083AF44, the vanilla cast setup): the
// wind-up is ours when it began from the chord. ChargeTick (each wind-up tick of
// Player_Action_808507F4, progress 0..1 through gPlayerAnim_link_magic_tame): builds the haptic;
// 1 = a trigger was released, cancel the cast (nothing spent). OnCast: the spell spawned.
bool VrMagic_Covers(struct Player* player);
bool VrMagic_ChordHeld(void);
void VrMagic_BeginCharge(struct PlayState* play, struct Player* player);
int32_t VrMagic_ChargeTick(struct PlayState* play, struct Player* player, float progress);
void VrMagic_OnCast(void);

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

// Hookshot / Longshot (VrHookshot.cpp — VR first person). InSwordHand: while the hookshot model
// is in Link's R_HAND limb (and gVrHookshotSwordHand, motion hands on), that limb rides the
// SWORD-hand (dominant) controller and L_HAND the off hand — the motion-hands limb override
// swaps them, unmirrored. RightLimbHand: the controller driving R_HAND right now (the off hand
// otherwise); the aim ray, the trigger mirror and classic fire all ask it. NoteAim (z_player_lib.c,
// right after Player_VrAimHeldProjectile aimed the idle hook): this frame's flight line — the
// hook's world.pos/world.rot, the controller, its 20 Hz hand-limb snapshot (MtxF, may be NULL)
// and the reach in world units — for the laser (gVrHookshotLaser), drawn at OnPlayDrawEnd as a
// child of the live hand matrix.
bool VrHookshot_InSwordHand(struct Player* player);
int32_t VrHookshot_RightLimbHand(struct Player* player);
void VrHookshot_NoteAim(struct Player* player, struct Actor* hook, int32_t vrHand, const float* handMtxF16,
                        float rangeUnits);
// Hookshot-only aim trim, applied by Player_VrAimHeldProjectile on top of the shared Weapon Aim
// Trim, to the aim ray (pos/dir, world units) of the controller holding the hookshot:
// gVrHookshotAimPitch/Yaw (degrees, up/right) turn the direction; the launch point is left alone
// (it is the barrel, read off the model). Yaw mirrors when the hookshot is in the left controller,
// so a tuned value means the same thing in either hand. The laser and the reticle trace the hook's
// resulting line, so they follow the trim.
void VrHookshot_TrimAimRay(int32_t vrHand, float* pos3, float* dir3);
// BarrelAim (gVrHookshotBarrelAim, default on): the hook aims from the hookshot MODEL — the
// vanilla in-hand hook transform, which the controller-driven R_HAND limb already carries — so it
// sits in the barrel, points along it and flies along it; off = the controller's aim ray. The
// hookshot trim applies on top either way. WeldIdleHook (ArmsHook_Draw, per matrix it draws while
// the hook is idle in the hand): registers that Mtx as a child of the live hand, so the hook and
// its stub of chain ride the controller at headset rate instead of trailing at the 20 Hz tick.
bool VrHookshot_BarrelAim(void);
void VrHookshot_WeldIdleHook(struct Actor* hook, const void* mtx);
// IdleWelded: true while this hook is drawn welded to the hand (same gate as WeldIdleHook), so
// ArmsHook_Draw can size it with the hand (VrHand_ScaleFactor).
bool VrHookshot_IdleWelded(struct Actor* hook);

// World-space pause menu (VrPause.cpp, gVrPauseWorldSpace). While WorldSpace() is true the game is
// frozen exactly as vanilla pauses it, but the world keeps rendering in stereo (no backdrop
// capture, no flat panel), and the four kaleido pages are a box around the player's head:
// anchored where the menu opened (center = the head, front = the head's facing), spun by the
// vanilla page-change camera orbit (inverted, so the box turns rather than the camera), pages at
// gVrPauseRadius, sized by gVrPausePageScale. Input is untouched: InputMenuMode() reports "a menu
// owns the controls", exactly as the flat panel did.
// WorldSpace: VR first person, gate on, paused (not the debug editor, not game over).
// FrameSync: once per frame from graph.c (VR routing); clears the anchor when the pause ends,
//   suppresses artificial turning while the box is up, returns WorldSpace().
// LeanClamp: the roomscale lean allowance (units) for Play_Draw: generous while paused (nothing
//   moves the body, so the stock 0.1 would freeze head translation) and for a few frames after,
//   while the body catches up.
// RefreshCullView: Camera_Update is frozen with the game; rebuild play->view from the live head so
//   culling follows where the player looks.
// BeginDraw: start of KaleidoScope_Draw; anchors on the first draw of a pause, builds this
//   frame's spin, draws the world dim (into the current list, before the pages).
// PageTranslate: the page placement (replaces Matrix_Translate(x, y, z, MTXMODE_NEW) in
//   KaleidoScope_DrawPages): box base x vanilla translate x page scale.
// PanelMatrix: the name panel's base (replaces Matrix_Translate(0, 0, -144, MTXMODE_NEW)).
bool VrPause_WorldSpace(void);
bool VrPause_InputMenuMode(void);
bool VrPause_FrameSync(void);
float VrPause_LeanClamp(void);
void VrPause_RefreshCullView(struct PlayState* play);
void VrPause_BeginDraw(struct PlayState* play);
void VrPause_PageTranslate(float x, float y, float z);
void VrPause_PanelMatrix(void);
// GameOverRects: the world-space pause is on a game-over state (8..0x11), whose "GAME OVER" title
//   is screen-space texture rectangles; FrameSync has put the rect panel on the HUD frame's virtual
//   screen for them. graph.c reads it to know who owns the rect panel this frame.
bool VrPause_GameOverRects(void);

// World-space N64 logo screen (VrLogo.cpp, gVrLogoWorld; the boot logo, ovl_title). While
// WorldSpace() is true the logo screen renders in stereo instead of on the floating panel: the
// spinning N64 logo and the shimmering text strip hang gVrLogoDistance metres in front of where the
// player looked when it came up, framed exactly as vanilla's fixed 30-degree camera showed them.
// SetActive: Title_Init (true) / Title_Destroy (false).
// WorldSpace: VR on, gate on, the logo screen running.
// FrameSync: once per frame from graph.c: suppresses artificial turning while active, drops the
//   anchor when not; returns WorldSpace().
// BeginFrame: start of Title_Main: hand-matrix clear, first person, origin anchor, anchors on the
//   first frame, publishes the rect panel (the text strip's texture rectangles).
// ModelTranslate: replaces the logo's Matrix_Translate(x, y, z, MTXMODE_NEW): vanilla's view base
//   carried onto the player's head, then the vanilla translate.
void VrLogo_SetActive(bool active);
bool VrLogo_WorldSpace(void);
bool VrLogo_FrameSync(void);
void VrLogo_BeginFrame(void);
void VrLogo_ModelTranslate(float x, float y, float z);

// World-space file select (VrFileSelect.cpp, gVrFileSelectWorld). While WorldSpace() is true the
// file select renders in stereo instead of on the floating panel: its sky surrounds the player and
// the menu window hangs in front of them, anchored where they looked when the file select came up
// (gVrFileSelectDistance metres away, gVrFileSelectScale, gVrFileSelectHeightCm). Its screen-space
// rectangles land on a virtual TV screen in the window's plane (VR_SetRectWorldPanel). Input is
// untouched; the N64 logo and the title demo keep their own paths.
// SetActive: FileChoose_Init (true) / FileChoose_Destroy (false). The gate is this game state
//   explicitly, never "no PlayState" (the N64 logo has none either).
// WorldSpace: VR on, gate on, the file select running.
// FrameSync: once per frame from graph.c, AFTER VrPause_FrameSync: suppresses artificial turning
//   while active, drops the anchor when not (graph.c turns the rect panel off when no one publishes
//   it); returns WorldSpace().
// BeginFrame: start of FileChoose_Main: the VR camera duties Play_Draw would do (hand-matrix clear,
//   first person, origin anchor), anchors on the first frame, publishes the rect panel.
// WindowTranslate: replaces Matrix_Translate(0, 0, -93.6f, MTXMODE_NEW) at every window site.
// SkyPose: replaces the orbiting sky eye with the live head and turns the sky cube instead
//   (orbit = ZREG(11)); leaves everything untouched when not WorldSpace().
void VrFileSelect_SetActive(bool active);
bool VrFileSelect_WorldSpace(void);
bool VrFileSelect_FrameSync(void);
void VrFileSelect_BeginFrame(void);
void VrFileSelect_WindowTranslate(void);
void VrFileSelect_SkyPose(int16_t orbit, float* eyeX, float* eyeY, float* eyeZ, float* skyRotY);

// Physical block pushing (VrBlock.cpp, gVrPhysBlockPush — VR first person with motion hands). The
// hands are INTENT for the vanilla grab, nothing else: both hands on a pushable's face (WALL_FLAG
// 0x40 dynapoly or scene wall, never Bg_Heavy_Block) + both grips freshly squeezed = the A button
// of the grab; either grip opening (or pulling a hand gVrBlockPullOff away) = letting go of A.
// While attached, pressing the hands in (or drawing them back) past gVrBlockPushCm, measured in
// the player's own frame (hand relative to the head, along Link's facing), is the stick held
// forward (back): the block moves at the vanilla cadence however hard you shove. The stick still
// wins whenever it is pushed. The block, the actions, speed, step and rest are untouched.
// Tick: once per Player_UpdateCommon, before the action function (grips, reach, pressure, haptics).
// GrabHeld: the A substitute at the grab handler and the hold gate (func_8083F9D0).
// Intent: replaces func_8083FFB8's result at the three intent sites (+1 push, -1 pull, 0 hold).
// GripConsumed: a grip on the block (or at a pushable face) loses its binding and the sword chord.
// PinnedHandMatrix: while attached by hand, the hand limb is drawn riding the block face (MtxF
// layout, in: the live hand matrix, out: the pinned one); false = draw the controller as usual.
void VrBlock_Tick(struct PlayState* play, struct Player* player);
bool VrBlock_GrabHeld(struct Player* player);
int32_t VrBlock_Intent(struct Player* player, int32_t stickIntent);
bool VrBlock_GripConsumed(int32_t hand, uint16_t mask);
bool VrBlock_PinnedHandMatrix(struct Player* player, int32_t vrHand, float* mf16);
typedef struct VrBlockDebug {
    int32_t gate;        // 0 armed; 1 Physical Block Pushing off; 2 not VR first person / motion hands off;
                         // 3 cutscene / horse / water / transition
    int32_t pushable;    // Link is touching a pushable face
    int32_t atWall[2];   // each hand is on the face (within reach of its plane, in front of Link)
    float planeCm[2];    // each hand's signed distance to the face (+ in front, - inside); -999 unmeasured
    int32_t latched[2];  // each grip was squeezed fresh at the face and is still held
    int32_t action;      // 0 not grabbing, 1 putting the item away, 2 holding, 3 pushing, 4 pulling
    int32_t handGrab;    // attached by the hands (0 while attached = an A-button grab)
    float pressureCm;    // hands along Link's facing since the grab (+ in, - back)
    float pushCm;        // threshold for push / pull
    float reachCm;       // grab reach
    int32_t intent;      // +1 push, -1 pull, 0 hold
    float driftCm;       // the hand furthest from where it took hold
    float pullOffCm;     // drift past this lets go
    int32_t blockMoving; // the block moved last tick
    int32_t steps;       // steps landed during this grab
    int32_t lastRelease; // -1 none yet, 0 grip opened, 1 pulled off, 2 Link left the grab
} VrBlockDebug;
void VrBlock_GetDebug(VrBlockDebug* out);
// Small body collider (VrBody.cpp, gVrSmallBody — VR first person with roomscale). Link's body keeps
// the vanilla wall radius for everything; the view (and the hands on the same anchor) is a small
// circle (gVrSmallBodyRadiusCm) living inside that big circle, so it can get closer to walls than the
// body but can never leave the body's circle — never past anything the body is held back by.
// Active: the feature applies this frame.
// NoteWallPushout (Actor_UpdateBgCheckInfo): the wall push-out the player's body just took.
// BeginCollision / EndCollision (around Player_ProcessSceneCollision in the movement branch): the
//   push-out of the stick's movement stays in the view; returns true while the view still has room
//   toward the wall Link touches — the caller then lifts the vanilla facing-a-wall speed cap so the
//   view arrives at walking speed instead of creeping.
// ClampView (Play_Draw, first-person anchor): clamps the view to the disk; head[3] (game units,
//   Link's head with the head offset unswept) is rewritten; false = not active (old path: swept
//   head offset + lean clamp).
bool VrBody_Active(void);
void VrBody_NoteWallPushout(struct Actor* actor, float dx, float dz);
void VrBody_BeginCollision(struct Player* player);
bool VrBody_EndCollision(struct PlayState* play, struct Player* player);
bool VrBody_ClampView(struct PlayState* play, struct Player* player, float* head);
// NoteObjectPush (CollisionCheck_SetOCvsOC): the object-collision push the player's body takes from a
//   STATIC object (anything but NPCs, enemies, bosses, explosives, held or moving actors: pots, rocks,
//   torches, boulders, signs...); fed to the view like a wall push-out. pusherPos (x,y,z) = the pusher's
//   collider position, contactDist = how far apart the two collider centres are at contact.
// ClampBodyMove (the roomscale body move): the body moving toward the head (swept against walls only)
//   also stops at those objects' edges (the light liftables by their known collider, everything else by
//   the contact distance remembered from its last push), so a view leaning over a rock never drags the
//   body into it (or into a grotto hole under it).
void VrBody_NoteObjectPush(struct Actor* pusher, struct Actor* pushed, float dx, float dz, const float* pusherPos,
                           float contactDist);
void VrBody_ClampBodyMove(struct PlayState* play, struct Player* player, const float* from, float* to);
// VrCarry.cpp: the light static liftables (not cuccos / bombs, not held) and their collider cylinder.
bool VrCarry_LightObjectCylinder(struct Actor* actor, float* radius, float* height, float* yShift);

// --- Get-item hold-up (VrGetItem.cpp) ---
// In first person the item Link holds up after opening a chest / receiving an item would sit on
// top of your head (vanilla puts it over his raised hands, 3.3 units ahead). While the player is
// in the get-item state, this pushes it gVrGetItemDistance cm further out from the hands
// (horizontally, away from the eyes), so it still follows the hands. hands = vanilla's reference
// point (the raised-hands midpoint); out = that point pushed out, world game units (the caller
// keeps vanilla's height above it). Returns false (vanilla placement) outside first-person VR,
// for other actors running the player draw, and for exchange items shown to NPCs.
bool VrGetItem_HoldUpPos(struct PlayState* play, struct Player* player, const float* hands, float* out);
typedef struct VrBodyDebug {
    int32_t active;    // the small body applies
    float slack;       // how far the view may sit from Link's centre (big - small), units
    float smallRadius; // units
    float bigRadius;   // the radius the body collides with now (crawling = 10), units
    float offset;      // the view's offset from Link's centre this tick, units
    int32_t room;      // touching a wall with room left toward it (speed cap lifted)
} VrBodyDebug;
void VrBody_GetDebug(VrBodyDebug* out);

// Physical climbing (VrClimb.cpp, gVrPhysClimb — VR first person with motion hands). Every climbable
// polygon carries the SurfaceType wall flags 0x02 (ladder), 0x04 (ladder top) or 0x08 (vines,
// fences, climbable rock), steep (|normal.y| < 600); a hand on (or within gVrClimbGrabReach of) one
// + a FRESH grip there = hold, and the drawn hand snaps onto the surface. Ladders and vines behave
// the same. The vanilla climb action (Player_Action_8084BF1C, PLAYER_STATE1_CLIMBING_LADDER) stays
// the state: while any hand holds, Link's body moves opposite to the last-grabbed hand 1:1 in 3D:
// along the wall, and toward / away from it (the wall glue's distance follows the arms; the view
// never closer than ~10 cm), instead of by the stick's step animations. The vanilla wall glue, bg
// check and randomizer climb gate (VB_CLIMB) run on every move, so the climbable area's edges stop
// you. Letting go of everything at or above (or within gVrClimbTopWindowCm below) where vanilla would
// climb over plays vanilla's climb-over (or the ladder dismount right at its rung); at the floor it
// steps off; anywhere else Link drops keeping a capped share of the body's last motion
// (gVrClimbMomentum %, gVrClimbTossCm max rise). With no hand holding (or physical climbing off) the
// climb is vanilla: stick + A.
// Tick: once per Player_UpdateCommon, before the action function (grips, hand probes, latches).
// TakeMount: a hand took hold of a climbable surface this tick while Link isn't climbing; fills the
//   polygon, its bgId and the hand's contact point for Player_VrClimbMount (called from the grab
//   handler on the ground and from the jump/fall action in the air).
// Step: inside the climb action. 0 = no hand holds, vanilla runs; 1 = move the body by outMove
//   (units, 3D); 2 = the last hand let go this tick: apply outMove (its last motion), then resolve.
// Moved: the body's achieved move this tick (units, the wall's own motion excluded) and its distance
//   from the wall; true = a rung's worth of travel: play the vanilla climbing sound.
// HandSnapOffset: while a hand holds, it is drawn this far (world units) from its controller: on
//   the surface where it took hold (gVrClimbSnapHands).
// Launch: the release velocity (units/tick) for a drop, momentum share applied and capped.
// SuppressVanillaGrab: after a physical release Link is falling on purpose — vanilla's automatic
//   re-grab of the climbable wall he falls along must not catch him. Clears on landing.
// BlockWalkInMount: walking into a ladder/vine with the stick doesn't auto-climb (gVrClimbWalkIn off).
// GripConsumed: a grip holding the wall (or on a climbable surface, about to) loses its binding.
// FrameSync (graph.c, every frame): turns the view lock off whenever the player isn't ticking
//   (pause, transitions); true while climbing physically = artificial turning stays off.
typedef struct VrClimbHit {
    void* poly; // CollisionPoly* (an anonymous-struct typedef: no tag to forward-declare)
    int32_t bgId;
    float pos[3];
    int32_t hand;
} VrClimbHit;
void VrClimb_Tick(struct PlayState* play, struct Player* player);
bool VrClimb_TakeMount(struct PlayState* play, struct Player* player, VrClimbHit* out);
int32_t VrClimb_Step(struct PlayState* play, struct Player* player, float outMove[3]);
bool VrClimb_Moved(struct Player* player, const float achieved[3], float wallDistUnits);
bool VrClimb_HandSnapOffset(struct Player* player, int32_t hand, float out[3]);
void VrClimb_Launch(struct Player* player, float outVel[3]);
void VrClimb_NoteResolved(struct Player* player, int32_t how);
// Active: physical climbing applies to this player this tick (gate open).
bool VrClimb_Active(struct Player* player);
bool VrClimb_SuppressVanillaGrab(struct Player* player);
bool VrClimb_BlockWalkInMount(struct Player* player);
float VrClimb_TopWindowUnits(void);
bool VrClimb_GripConsumed(int32_t hand, uint16_t mask);
bool VrClimb_FrameSync(void);
typedef struct VrClimbDebug {
    int32_t gate;         // 0 armed; 1 Physical Climbing off; 2 not VR first person / motion hands off;
                          // 3 cutscene / horse / transition
    int32_t onSurface[2]; // each hand is on a climbable surface
    int32_t surfFlags[2]; // the wall flags under each hand (0x02 ladder, 0x04 ladder top, 0x08 climbable)
    float surfCm[2];      // the closest part of each hand from the surface (+ short of it, - into it)
    int32_t latched[2];   // each hand is holding
    int32_t anchor;       // the hand that drives the body (-1 none)
    int32_t climbing;     // Link is in the climb (PLAYER_STATE1_CLIMBING_LADDER)
    int32_t driving;      // the hands move the body (0 = vanilla stick climbing)
    float wallDistCm;     // Link's body from the wall (vanilla holds it at 15 units)
    float moveCm;         // body move requested this tick
    float achievedCm;     // body move achieved this tick
    int32_t lastRelease;  // -1 none yet, 0 dropped, 1 climbed over the top, 2 stepped off at the floor,
                          // 3 got onto a ladder from its top
    float lastTossCm;     // the rise the last drop's launch was good for
    int32_t mounts;       // hand mounts this session
} VrClimbDebug;
void VrClimb_GetDebug(VrClimbDebug* out);

// Physical carrying (VrCarry.cpp, gVrPhysCarry — selector mode, motion hands). The light liftables
// (pots, small rocks, bushes, crates, bomb flowers, grounded bombs, cuccos) are picked up by a fresh
// grip with the hand on one that offered itself to be carried — instantly, no lift animation — held
// rigidly in one hand or both, and thrown with the object's real velocity at release. The offer, the
// carrying state, the object's reactions and its flight are all vanilla.
// NoteOffer (Actor_OfferGetItem): every GI_NONE carry offer in range is a candidate for a hand, not
//   only the one Link faces most (the vanilla pick).
// Tick: once per Player_UpdateCommon before the action (grips, pick up, join, hand-over, throw).
// UpdateCarryPose (player draw, after vanilla places the held actor): the hold pose.
// GripConsumed: the holding hand(s), a free hand at the held object, and a hand on a liftable lose
//   their grip binding (and the sword chord).
// BeginDrawWeld / EndDrawWeld (Actor_Draw around actor->draw): while the held object draws, every
//   Mtx it makes is welded to the live hand (gVrMtxWeldHand -> Matrix_ToMtx), so it moves at headset
//   rate instead of the 20 Hz tick.
void VrCarry_NoteOffer(struct Actor* actor);
void VrCarry_Tick(struct PlayState* play, struct Player* player);
void VrCarry_UpdateCarryPose(struct Player* player);
bool VrCarry_GripConsumed(int32_t hand, uint16_t mask);
bool VrCarry_BeginDrawWeld(struct Actor* actor);
// The same draw weld for a bomb / bombchu held through VrItemThrow (its carry hand); ended by
// VrCarry_EndDrawWeld like the carry weld.
bool VrItemThrow_BeginDrawWeld(struct Actor* actor);
void VrCarry_EndDrawWeld(void);
typedef struct VrCarryDebug {
    int32_t gate;         // 0 armed; 1 Physical Carrying off; 2 not selector play / horse / water / minigame
    int32_t holding;      // something is held by hand
    int32_t primary;      // the hand the draw welds to (-1 none)
    int32_t hands;        // bit 0 left, bit 1 right
    float nearestCm;      // not holding: the nearest offered liftable to a hand (-1 none in reach)
    int32_t lastPickUp;   // actor id of the last pick-up (-1 none yet)
    float lastReleaseMps; // the object's speed at the last release, m/s (-1 none yet)
    int32_t lastRelease;  // 0 dropped, 1 thrown
    int32_t heavy;        // the boulder: 0 none, 1 one hand latched, 2 both latched (not lifting), 3 lifted
} VrCarryDebug;
// PinnedHandMatrix (hand-limb draw): a hand latched on a silver boulder is drawn on it (MtxF layout, in:
//   the live hand matrix, out: pinned); false = draw the controller as usual.
bool VrCarry_PinnedHandMatrix(struct Player* player, int32_t vrHand, float* mf16);
// z_player.c: the vanilla boulder lift without its animation: put away what is held, the boulder action
//   (Player_Action_80846260, Link rooted) in its holding loop, the boulder attached. 1 lifted, 0 refused,
//   -1 too heavy (no Silver Gauntlets). The release is Player_VrCarryRelease (which also stands Link up).
int32_t Player_VrCarryPickUpHeavy(struct PlayState* play, struct Player* player, struct Actor* actor);
// z_player.c: Link is lifting a gauntlet pillar (its put-away or the lift action) — the hands then follow
//   his animation instead of the controllers, so the pillar's pose and throw are the base game's.
int32_t Player_VrPillarLift(struct Player* player);
// VrBlock.cpp: a pillar lift begun by the hands is held overhead (the frame before the vanilla throw)
//   while both grips stay closed; letting go throws it.
bool VrBlock_PillarHold(struct Player* player);
void VrCarry_GetDebug(VrCarryDebug* out);
// Implemented in z_player.c (// SOH [VR]). CarryPickUp: the vanilla lift without its animation —
// put away what is held, then exactly the lift's attach frame (heldActor / parent / carrying state /
// carry upper action). 1 lifted, 0 refused (Link busy), -1 too heavy (no bracelet). CarryRelease:
// the held object leaves with `velocity` (units/tick), then the vanilla post-throw detach.
int32_t Player_VrCarryPickUp(struct PlayState* play, struct Player* player, struct Actor* actor);
void Player_VrCarryRelease(struct PlayState* play, struct Player* player, const float* velocity);
// z_player_lib.c: the hand every Mtx made by Matrix_ToMtx is welded to (-1 = none), and the weld itself
// (Player_VrWeldMtxToHand on gPlayState; a no-op without this frame's hand snapshot).
extern int32_t gVrMtxWeldHand;
void Player_VrWeldCurrentMtx(const void* mtx, const float* curMf16);

// Implemented in z_player.c (// SOH [VR]). BlockAction: which part of the vanilla grab Link is in
// (0 none, 1 putting the held item away first, 2 hold, 3 push, 4 pull). TouchingPushable: the wall
// half of the vanilla "Grab" prompt — touching a WALL_FLAG 0x40 face (sTouchedWallFlags).
int32_t Player_VrBlockAction(struct Player* player);
int32_t Player_VrTouchingPushable(struct Player* player);

// VrCombatDebug.cpp: ReDead/Gibdo grab diagnostic (gVrRedeadLog -> vrredead_log.csv), called at the end
// of EnRd_Update. grab: -1 no attempt this tick, 0 grabPlayer refused, 1 grabbed.
void VrRedead_LogTick(struct Actor* rd, int32_t action, int32_t stunWait, int32_t grabWait, int32_t grab);

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

// Megaton Hammer grip state (VrHammer.cpp). The melee feed caches the handle's grip-local
// geometry each draw (world units, in the frame of the SERVED lead-hand pose); the tick uses it
// to take / release the handle with the off hand. HammerGrip is what the next feed pushes to the
// held-object sim (secondary hand + its grip point on the handle).
struct HammerGrip {
    bool twoHand;
    int offHand;
    float secondaryLocal[3]; // grip-local world units: the off hand's point on the handle
};
const HammerGrip& Hammer_GetGrip();
void Hammer_SetHandleLocal(const float buttLocal[3], const float neckLocal[3]);
void Hammer_Tick(PlayState* play, Player* player);
void Hammer_Reset();
void Hammer_NoteSwing(float headMps, int tier);
void Hammer_NoteImpact(float approachMps, int kind, int strike);

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
