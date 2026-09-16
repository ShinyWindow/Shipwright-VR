/*
 * File: z_en_boom.c
 * Overlay: ovl_En_Boom
 * Description: Thrown Boomerang. Actor spawns when thrown and is killed when caught.
 */

#include "z_en_boom.h"
#include "objects/gameplay_keep/gameplay_keep.h"

#define FLAGS (ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED)

void EnBoom_Init(Actor* thisx, PlayState* play);
void EnBoom_Destroy(Actor* thisx, PlayState* play);
void EnBoom_Update(Actor* thisx, PlayState* play);
void EnBoom_Draw(Actor* thisx, PlayState* play);

void EnBoom_Fly(EnBoom* this, PlayState* play);

const ActorInit En_Boom_InitVars = {
    ACTOR_EN_BOOM,
    ACTORCAT_MISC,
    FLAGS,
    OBJECT_GAMEPLAY_KEEP,
    sizeof(EnBoom),
    (ActorFunc)EnBoom_Init,
    (ActorFunc)EnBoom_Destroy,
    (ActorFunc)EnBoom_Update,
    (ActorFunc)EnBoom_Draw,
    NULL,
};

static ColliderQuadInit sQuadInit = {
    {
        COLTYPE_NONE,
        AT_ON | AT_TYPE_PLAYER,
        AC_NONE,
        OC1_NONE,
        OC2_TYPE_PLAYER,
        COLSHAPE_QUAD,
    },
    {
        ELEMTYPE_UNK2,
        { 0x00000010, 0x00, 0x01 },
        { 0xFFCFFFFF, 0x00, 0x00 },
        TOUCH_ON | TOUCH_NEAREST | TOUCH_SFX_NORMAL,
        BUMP_NONE,
        OCELEM_NONE,
    },
    { { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } } },
};

static InitChainEntry sInitChain[] = {
    ICHAIN_S8(targetMode, 5, ICHAIN_CONTINUE),
    ICHAIN_VEC3S(shape.rot, 0, ICHAIN_STOP),
};

void EnBoom_SetupAction(EnBoom* this, EnBoomActionFunc actionFunc) {
    this->actionFunc = actionFunc;
}

void EnBoom_Init(Actor* thisx, PlayState* play) {
    EnBoom* this = (EnBoom*)thisx;
    EffectBlureInit1 blure;

    this->actor.room = -1;

    Actor_ProcessInitChain(&this->actor, sInitChain);

    blure.p1StartColor[0] = 255;
    blure.p1StartColor[1] = 255;
    blure.p1StartColor[2] = 100;
    blure.p1StartColor[3] = 255;

    blure.p2StartColor[0] = 255;
    blure.p2StartColor[1] = 255;
    blure.p2StartColor[2] = 100;
    blure.p2StartColor[3] = 64;

    blure.p1EndColor[0] = 255;
    blure.p1EndColor[1] = 255;
    blure.p1EndColor[2] = 100;
    blure.p1EndColor[3] = 0;

    blure.p2EndColor[0] = 255;
    blure.p2EndColor[1] = 255;
    blure.p2EndColor[2] = 100;
    blure.p2EndColor[3] = 0;

    blure.elemDuration = 8;
    blure.unkFlag = 0;
    blure.calcMode = 0;
    blure.trailType = TRAIL_TYPE_BOOMERANG;

    Effect_Add(play, &this->effectIndex, EFFECT_BLURE1, 0, 0, &blure);

    Collider_InitQuad(play, &this->collider);
    Collider_SetQuad(play, &this->collider, &this->actor, &sQuadInit);

    // SOH [VR] A vanilla throw until Player_VrThrowBoomerang says otherwise.
    this->vrPhysical = false;
    this->vrHomeValid = false;
    this->vrCatchArmed = false;
    this->vrCaught = false;
    this->vrCatchHand = -1;
    this->vrReturnTicks = 0;
    this->vrMinHomeDist = 100000.0f;
    this->vrArriveRadius = 0.0f;

    EnBoom_SetupAction(this, EnBoom_Fly);
}

void EnBoom_Destroy(Actor* thisx, PlayState* play) {
    EnBoom* this = (EnBoom*)thisx;
    Player* player = GET_PLAYER(play);

    Effect_Delete(play, this->effectIndex);
    Collider_DestroyQuad(play, &this->collider);

    // SOH [VR] Vanilla never clears player->boomerangActor (written at the throw, only read
    // while PLAYER_STATE1_BOOMERANG_THROWN), leaving it dangling after every return.
    if (player != NULL && player->boomerangActor == &this->actor) {
        player->boomerangActor = NULL;
    }
}

// SOH [VR] Vanilla's return-arrival tail, verbatim (see EnBoom_Fly), shared with the VR path.
void EnBoom_FinishReturn(EnBoom* this, PlayState* play) {
    Player* player = GET_PLAYER(play);
    Actor* target = this->grabbed;

    if (target != NULL) {
        Math_Vec3f_Copy(&target->world.pos, &player->actor.world.pos);

        // If the grabbed actor is EnItem00 (HP/Key etc) set gravity and flags so it falls in front of Link.
        // Otherwise if it's a Skulltula Token, just set flags so he collides with it to collect it.
        if (target->id == ACTOR_EN_ITEM00) {
            target->gravity = -0.9f;
            target->bgCheckFlags &= ~0x03;
        } else {
            target->flags &= ~ACTOR_FLAG_HOOKSHOT_ATTACHED;
        }
    }
    // Set player flags and kill the boomerang beacause Link caught it.
    player->stateFlags1 &= ~PLAYER_STATE1_BOOMERANG_THROWN;
    player->boomerangQuickRecall = false;
    Actor_Kill(&this->actor);
}

// SOH [VR] Did this tick's travel (prevPos -> world.pos) pass within radius of point? The
// boomerang covers ~12 units a tick, more than a hand-sized catch radius, so a point test
// could step straight over the hand.
static s32 EnBoom_VrSweepHits(EnBoom* this, Vec3f* point, f32 radius) {
    Vec3f a = this->actor.prevPos;
    Vec3f b = this->actor.world.pos;
    f32 dx = b.x - a.x;
    f32 dy = b.y - a.y;
    f32 dz = b.z - a.z;
    f32 lenSq = SQ(dx) + SQ(dy) + SQ(dz);
    f32 t = 0.0f;
    Vec3f closest;

    if (lenSq > 0.0001f) {
        t = ((point->x - a.x) * dx + (point->y - a.y) * dy + (point->z - a.z) * dz) / lenSq;
        t = CLAMP(t, 0.0f, 1.0f);
    }
    closest.x = a.x + dx * t;
    closest.y = a.y + dy * t;
    closest.z = a.z + dz * t;
    return Math_Vec3f_DistXYZ(&closest, point) <= radius;
}

void EnBoom_Fly(EnBoom* this, PlayState* play) {
    Actor* target;
    Player* player;
    s32 collided;
    s16 yawTarget;
    s16 yawDiff;
    s16 pitchTarget;
    s16 pitchDiff;
    s32 pad1;
    f32 distXYZScale;
    f32 distFromLink;
    DynaPolyActor* hitActor;
    s32 hitDynaID;
    Vec3f hitPoint;
    s32 pad2;

    player = GET_PLAYER(play);
    target = this->moveTo;

    // If the boomerang is moving toward a targeted actor, handle setting the proper x and y angle to fly toward it.
    if (target != NULL) {
        // SOH [VR] On the return leg (moveTo = Link) a physically thrown boomerang homes on the
        // catch HAND the module fed this tick, not Link's head. Same gain, same stepping.
        Vec3f* homePos = &target->focus.pos;
        if (this->vrPhysical && this->vrHomeValid && target == &player->actor) {
            homePos = &this->vrHomePos;
        }

        yawTarget = Actor_WorldYawTowardPoint(&this->actor, homePos);
        yawDiff = this->actor.world.rot.y - yawTarget;

        pitchTarget = Actor_WorldPitchTowardPoint(&this->actor, homePos);
        pitchDiff = this->actor.world.rot.x - pitchTarget;

        distXYZScale = (200.0f - Math_Vec3f_DistXYZ(&this->actor.world.pos, homePos)) * 0.005f;
        if (distXYZScale < 0.12f) {
            distXYZScale = 0.12f;
        }

        if ((target != &player->actor) && ((target->update == NULL) || (ABS(yawDiff) > 0x4000))) {
            //! @bug  This condition is why the boomerang will randomly fly off in a the down left direction sometimes.
            //      If the actor targetted is not Link and the difference between the 2 y angles is greater than 0x4000,
            //      the moveTo pointer is nulled and it flies off in a seemingly random direction.
            this->moveTo = NULL;
        } else {
            Math_ScaledStepToS(&this->actor.world.rot.y, yawTarget, (s16)(ABS(yawDiff) * distXYZScale));
            Math_ScaledStepToS(&this->actor.world.rot.x, pitchTarget, (s16)(ABS(pitchDiff) * distXYZScale));
        }
    }

    // Set xyz speed, move forward, and play the boomerang sound
    Actor_SetProjectileSpeed(&this->actor, 12.0f);
    Actor_MoveXZGravity(&this->actor);
    Actor_PlaySfx_Flagged(&this->actor, NA_SE_IT_BOOMERANG_FLY - SFX_FLAG);

    // If the boomerang collides with EnItem00 or a Skulltula token, set grabbed pointer to pick it up
    collided = this->collider.base.atFlags & AT_HIT;
    collided = !!(collided);
    if (collided) {
        if (((this->collider.base.at->id == ACTOR_EN_ITEM00) || (this->collider.base.at->id == ACTOR_EN_SI))) {
            this->grabbed = this->collider.base.at;
            if (this->collider.base.at->id == ACTOR_EN_SI) {
                this->collider.base.at->flags |= ACTOR_FLAG_HOOKSHOT_ATTACHED;
            }
        }
    }

    // Decrement the return timer and check if it's 0. If it is, check if Link can catch it and handle accordingly.
    // Otherwise handle grabbing and colliding.
    if (DECR(this->returnTimer) == 0 || player->boomerangQuickRecall) {
        this->moveTo = &player->actor;

        if (this->vrPhysical && this->vrHomeValid) {
            // SOH [VR] Return to the catch hand. Arrival = this tick's travel swept through the
            // catch sphere around the hand: with the grip closed that is a catch (the module
            // puts the boomerang in that hand), with it open a miss (vanilla end; the pocket
            // re-presents it). Quick recall (FastBoomerang) ends it on the spot as vanilla.
            // Safety nets so a hand that keeps dodging can't leave it circling: once it has
            // come near and is now clearly receding it counts as missed, and so does a return
            // leg longer than 8 s.
            s32 swept = EnBoom_VrSweepHits(this, &this->vrHomePos, this->vrArriveRadius);
            f32 distFromHand = Math_Vec3f_DistXYZ(&this->actor.world.pos, &this->vrHomePos);
            s32 passedBy;

            if (distFromHand < this->vrMinHomeDist) {
                this->vrMinHomeDist = distFromHand;
            }
            passedBy = (this->vrMinHomeDist < 60.0f) && (distFromHand > this->vrMinHomeDist + 40.0f);
            if (this->vrReturnTicks < 0xFFFF) {
                this->vrReturnTicks++;
            }

            if (swept || player->boomerangQuickRecall || passedBy || this->vrReturnTicks > 160) {
                this->vrCaught = swept && this->vrCatchArmed;
                EnBoom_FinishReturn(this, play);
            }
        } else {
            distFromLink = Math_Vec3f_DistXYZ(&this->actor.world.pos, &player->actor.focus.pos);

            // If the boomerang is less than 40 units away from Link, he can catch it.
            if (distFromLink < 40.0f || player->boomerangQuickRecall) {
                EnBoom_FinishReturn(this, play);
            }
        }
    } else {
        collided = (this->collider.base.atFlags & AT_HIT);
        collided = (!!(collided));
        if (collided) {
            // Copy the position from the prevous frame to the boomerang to start the bounce back.
            Math_Vec3f_Copy(&this->actor.world.pos, &this->actor.prevPos);
        } else {
            collided = BgCheck_EntityLineTest1(&play->colCtx, &this->actor.prevPos, &this->actor.world.pos, &hitPoint,
                                               &this->actor.wallPoly, true, true, true, true, &hitDynaID);

            if (collided) {
                // If the boomerang collides with something and it's is a Jabu Object actor with params equal to 0, then
                // set collided to 0 so that the boomerang will go through the wall.
                // Otherwise play a clank sound and keep collided set to bounce back.
                if (func_8002F9EC(play, &this->actor, this->actor.wallPoly, hitDynaID, &hitPoint) != 0 ||
                    (hitDynaID != BGCHECK_SCENE && ((hitActor = DynaPoly_GetActor(&play->colCtx, hitDynaID)) != NULL) &&
                     hitActor->actor.id == ACTOR_BG_BDAN_OBJECTS && hitActor->actor.params == 0)) {
                    collided = false;
                } else {
                    CollisionCheck_SpawnShieldParticlesMetal(play, &hitPoint);
                }
            }
        }

        // If the boomerang needs to bounce back, set x and y angle accordingly.
        // Set timer to 0 and set return actor to player so it goes back to Link.
        if (collided) {
            this->actor.world.rot.x = -this->actor.world.rot.x;
            this->actor.world.rot.y += 0x8000;
            this->moveTo = &player->actor;
            this->returnTimer = 0;
        }
    }

    // If the actor the boomerang is holding has a null update function, set grabbed to null.
    // Otherwise, copy the position from the boomerang to the actor to move it.
    target = this->grabbed;
    if (target != NULL) {
        if (target->update == NULL) {
            this->grabbed = NULL;
        } else {
            Math_Vec3f_Copy(&target->world.pos, &this->actor.world.pos);
        }
    }

    // SOH [VR] The hand position is good for one tick; the module re-feeds it before the next.
    this->vrHomeValid = false;
}

void EnBoom_Update(Actor* thisx, PlayState* play) {
    EnBoom* this = (EnBoom*)thisx;
    Player* player = GET_PLAYER(play);

    if (!(player->stateFlags1 & PLAYER_STATE1_IN_CUTSCENE)) {
        this->actionFunc(this, play);
        Actor_SetFocus(&this->actor, 0.0f);
        this->activeTimer = this->activeTimer + 1;
    }
}

void EnBoom_Draw(Actor* thisx, PlayState* play) {
    static Vec3f sMultVec1 = { -960.0f, 0.0f, 0.0f };
    static Vec3f sMultVec2 = { 960.0f, 0.0f, 0.0f };
    EnBoom* this = (EnBoom*)thisx;
    Vec3f vec1;
    Vec3f vec2;

    OPEN_DISPS(play->state.gfxCtx);

    Matrix_RotateY(this->actor.world.rot.y * (M_PI / 0x8000), MTXMODE_APPLY);
    Matrix_RotateZ(0x1F40 * (M_PI / 0x8000), MTXMODE_APPLY);
    Matrix_RotateX(this->actor.world.rot.x * (M_PI / 0x8000), MTXMODE_APPLY);
    Matrix_MultVec3f(&sMultVec1, &vec1);
    Matrix_MultVec3f(&sMultVec2, &vec2);

    if (func_80090480(play, &this->collider, &this->boomerangInfo, &vec1, &vec2) != 0) {
        EffectBlure_AddVertex(Effect_GetByIndex(this->effectIndex), &vec1, &vec2);
    }

    Gfx_SetupDL_25Opa(play->state.gfxCtx);
    Matrix_RotateY((this->activeTimer * 12000) * (M_PI / 0x8000), MTXMODE_APPLY);

    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_OPA_DISP++, gBoomerangRefDL);

    CLOSE_DISPS(play->state.gfxCtx);
}
