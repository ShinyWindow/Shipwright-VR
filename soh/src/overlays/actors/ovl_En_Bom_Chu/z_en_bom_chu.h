#ifndef Z_EN_BOM_CHU_H
#define Z_EN_BOM_CHU_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnBomChu;

typedef void (*EnBomChuActionFunc)(struct EnBomChu*, PlayState*);

typedef struct EnBomChu {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ EnBomChuActionFunc actionFunc;
    /* 0x0150 */ s16 timer;
    /* 0x0154 */ Vec3f axisForwards;
    /* 0x0160 */ Vec3f axisUp;
    /* 0x016C */ Vec3f axisLeft;
    /* 0x0178 */ f32 visualJitter;
    /* 0x017C */ s32 blure1Index;
    /* 0x0180 */ s32 blure2Index;
    /* 0x0184 */ ColliderJntSph collider;
    /* 0x01A4 */ ColliderJntSphElement colliderElements[1];
    // SOH [VR] Physical drop: released from the VR grip, the chu falls straight down from the
    // release point and only starts crawling once it lands. Travels with the actor (savestates).
    u8 vrPhysicalDrop;  // released by the VR grip: fall from here, then crawl
    u8 vrHasCrawlYaw;   // vrCrawlYaw is valid (else Link's facing at landing)
    s16 vrCrawlYaw;     // binang, horizontal direction to crawl once landed
    s16 vrFallTicks;    // safety: explode if no floor after N ticks
    f32 vrFallSpeed;    // own integrator: Actor_MoveXYZ has no gravity
} EnBomChu; // size = 0x01E4 (vanilla portion)

#endif
