#ifndef Z_EN_BOOM_H
#define Z_EN_BOOM_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnBoom;

typedef void (*EnBoomActionFunc)(struct EnBoom*, PlayState*);

typedef struct EnBoom {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ ColliderQuad collider;
    /* 0x01CC */ Actor* moveTo; // actor boomerang moves toward
    /* 0x01D0 */ Actor* grabbed; // actor grabbed by the boomerang
    /* 0x01D4 */ u8 returnTimer; // returns to Link when 0
    /* 0x01D5 */ u8 activeTimer; // increments once every update
    /* 0x01D8 */ s32 effectIndex;
    /* 0x01DC */ WeaponInfo boomerangInfo;
    /* 0x01F8 */ EnBoomActionFunc actionFunc;
    // SOH [VR] Physical boomerang (VrBoomerang.cpp). vrPhysical: thrown from the VR hand; the
    // return leg then homes on vrHomePos (the catch hand) instead of Link's head, and arrives by
    // a segment sweep against vrArriveRadius instead of the 40-unit head test. vrHomeValid is
    // written by the module every tick and consumed (cleared) here, so a module that stops
    // feeding (reset, F9, save state) drops the actor back to the vanilla return. vrCatchArmed:
    // the catch hand's grip is closed this tick, so an arrival is a catch (vrCaught = 1, read by
    // the module after the kill) rather than a miss. Never serialized as meaningful: a restored
    // flight simply falls back to vanilla until the module feeds it again.
    u8 vrPhysical;
    u8 vrHomeValid;
    u8 vrCatchArmed;
    u8 vrCaught;
    s8 vrCatchHand;
    u16 vrReturnTicks;   // ticks spent on the VR return leg (safety timeout)
    f32 vrMinHomeDist;   // closest approach to the hand so far on the return leg (pass-by detection)
    f32 vrArriveRadius;
    Vec3f vrHomePos;
} EnBoom;

// SOH [VR] The return-arrival tail of EnBoom_Fly (deposit the grabbed item/token, clear the
// player's thrown flag and quick recall, kill the actor), factored so the VR hand-arrival path
// ends the flight exactly as vanilla does.
void EnBoom_FinishReturn(EnBoom* boom, PlayState* play);

#endif
