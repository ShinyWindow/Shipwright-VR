extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
extern PlayState* gPlayState;
}
#include "VrCombat.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <vr_interface.h>
#include <cmath>
#include <cstdio>

// Small body collider (VR first person, roomscale on). User spec (October 4, 2026): to the player
// it should feel like a small collision radius — walk into a wall and you get right up to it — while
// climbing, ledges, tunnels, grabs and everything else keep working off the ORIGINAL radius. "The
// little circle is bound by the same constraint as the big one, it just lets us get closer to the
// boundary itself."
//
// Link's body (world.pos, the real collider) is untouched and still collides at the vanilla radius.
// The VIEW (and the hands, which hang off the same camera anchor) is a small circle that lives INSIDE
// that big circle: it may sit up to (big radius - small radius) from Link's centre and never further.
// Link's body is always held a full big radius out of walls, so the small circle can never pass a
// wall or a gap the body can't — no camera collision is involved; it is the containment itself.
//
// What moves the view off-centre is motion the body could not make:
//   - physical head movement the body couldn't follow (the roomscale residual — previously thrown
//     away at 0.1 units), and
//   - the part of the stick's movement a wall pushed back out (fed from Actor_UpdateBgCheckInfo).
// What brings it back is the existing roomscale catch-up: whenever the body is free it moves under
// the view (swept at the big radius), so the view itself never glides anywhere on its own.
//
// The camera offset from Link's centre is (head offset + roomscale residual); the roomscale residual
// is (tracked head - roomscale origin). Clamping that sum to the disk is done by shifting the origin,
// exactly as the old lean clamp did, just to the disk instead of to zero.

namespace {

constexpr float kMinSmallUnits = 5.0f;   // the near clip is 4 units: never closer than this to a wall
constexpr float kTeleportUnits = 100.0f; // a body jump this large in one tick is a warp, not movement

float sPushout[2] = { 0.0f, 0.0f };
// Diagnostic (gVrBodyLog): one CSV line per tick of everything that moves the view off Link's centre.
FILE* sLog = nullptr;
float sLogPre[2] = { 0.0f, 0.0f };
// The push Link's body took from light objects (pots, rocks, bushes, crates) in the last object-collision
// pass (end of the previous frame), applied by Actor_UpdatePos at the start of this tick.
float sObjectPush[2] = { 0.0f, 0.0f };
u32 sObjectPushFrame = 0;

// Static objects that have pushed Link's body recently: where their collider centre sits relative to the
// actor, and the centre distance at which they touch him. The roomscale body move keeps that distance.
struct Obstacle {
    Actor* actor;
    float offX, offZ; // collider centre - actor world.pos (xz)
    float dist;       // contact distance between the two collider centres
    u32 frame;        // last push
};
constexpr int kMaxObstacles = 8;
constexpr u32 kObstacleTicks = 60;
Obstacle sObstacles[kMaxObstacles];
int sObstacleCount = 0;

// Leaned over like a wall: anything that stays put. Characters (NPCs, enemies, bosses, other players),
// explosives, and anything held or moving keep shoving Link as vanilla.
bool StaticPusher(const Actor* a) {
    if (a == nullptr || a->parent != nullptr || a->update == nullptr) {
        return false;
    }
    switch (a->category) {
        case ACTORCAT_NPC:
        case ACTORCAT_ENEMY:
        case ACTORCAT_BOSS:
        case ACTORCAT_PLAYER:
        case ACTORCAT_EXPLOSIVE:
            return false;
        default:
            break;
    }
    return fabsf(a->speedXZ) < 0.01f && (fabsf(a->velocity.x) + fabsf(a->velocity.z)) < 0.01f;
}
float sLastBody[3] = { 0.0f, 0.0f, 0.0f };
bool sHaveLast = false;
VrBodyDebug sDebug = { 0, 0.0f, 0.0f, 0.0f, 0.0f, 0 };

float WorldScale() {
    const float ws = VR_GetWorldScale();
    return ws < 1.0f ? 35.0f : ws;
}

bool IsRealPlayer(Player* player) {
    return player != nullptr && gPlayState != nullptr && player == GET_PLAYER(gPlayState);
}

float SmallRadius() {
    return fmaxf(CVarGetFloat("gVrSmallBodyRadiusCm", 15.0f) * 0.01f * WorldScale(), kMinSmallUnits);
}

// The radius the game is actually colliding Link's body with right now (Player_ProcessSceneCollision).
float BigRadius(Player* player) {
    return (player->stateFlags2 & PLAYER_STATE2_CRAWLING) ? 10.0f : player->ageProperties->wallCheckRadius;
}

// How far the view may sit from Link's centre: the room between the two circles. On horseback the
// saddle is not a collider at all — the view stays centred.
float Slack(Player* player) {
    if (player->stateFlags1 & PLAYER_STATE1_ON_HORSE) {
        return 0.0f;
    }
    return fmaxf(BigRadius(player) - SmallRadius(), 0.0f);
}

// Clamp (head offset + roomscale residual) to the disk of radius slack by shifting the roomscale
// origin. out = the resulting view offset from Link's centre.
void ClampDisk(float slack, const float headOff[2], float out[2]) {
    float res[2];
    VR_GetRoomscaleDesired(res);
    float tx = headOff[0] + res[0];
    float tz = headOff[1] + res[1];
    const float len = sqrtf(tx * tx + tz * tz);
    if (len > slack) {
        const float k = (len > 1e-4f) ? (slack / len) : 0.0f;
        const float nx = tx * k, nz = tz * k;
        VR_AddRoomscaleDisplacement(tx - nx, tz - nz);
        tx = nx;
        tz = nz;
    }
    out[0] = tx;
    out[1] = tz;
}

} // namespace

extern "C" bool VrBody_Active(void) {
    return CVarGetInteger("gVrSmallBody", 1) && CVarGetInteger("gVrRoomscale", 1) && VR_IsInitialized() &&
           VR_GetFirstPerson() && !VR_IsFlatScreen();
}

extern "C" void VrBody_NoteWallPushout(Actor* actor, float dx, float dz) {
    if (gPlayState == nullptr || actor != &GET_PLAYER(gPlayState)->actor) {
        return;
    }
    sPushout[0] += dx;
    sPushout[1] += dz;
}

extern "C" void VrBody_BeginCollision(Player* player) {
    if (IsRealPlayer(player)) {
        sPushout[0] = sPushout[1] = 0.0f;
    }
}

extern "C" bool VrBody_EndCollision(PlayState* play, Player* player) {
    if (!IsRealPlayer(player)) {
        return false;
    }
    const float px = player->actor.world.pos.x, py = player->actor.world.pos.y, pz = player->actor.world.pos.z;
    const float jx = px - sLastBody[0], jy = py - sLastBody[1], jz = pz - sLastBody[2];
    const bool warped = sHaveLast && (jx * jx + jy * jy + jz * jz) > kTeleportUnits * kTeleportUnits;
    sLastBody[0] = px;
    sLastBody[1] = py;
    sLastBody[2] = pz;
    sHaveLast = true;
    float pushX = sPushout[0], pushZ = sPushout[1];
    sPushout[0] = sPushout[1] = 0.0f;
    // Light objects push the body like walls do (object collision, applied by Actor_UpdatePos this tick):
    // the view keeps that too, so you can lean over a rock or a pot instead of being shoved off it.
    if (play != nullptr && (play->state.frames - sObjectPushFrame) <= 1) {
        pushX += sObjectPush[0];
        pushZ += sObjectPush[1];
    }
    sObjectPush[0] = sObjectPush[1] = 0.0f;

    const float slack = Slack(player);
    sDebug.active = VrBody_Active() ? 1 : 0;
    sDebug.slack = slack;
    sDebug.smallRadius = SmallRadius();
    sDebug.bigRadius = BigRadius(player);
    if (!sDebug.active) {
        return false;
    }
    if (warped) {
        // A warp (respawn, scripted placement): start centred, so the body can't "catch up" across
        // the old spot.
        VR_ResetRoomscale();
        return false;
    }

    // The stick's movement the wall took back stays in the view: shifting the origin by the push-out
    // leaves the view where the body would have been.
    float preRes[2];
    VR_GetRoomscaleDesired(preRes);
    if (pushX != 0.0f || pushZ != 0.0f) {
        VR_AddRoomscaleDisplacement(pushX, pushZ);
    }
    const s16 yaw = player->actor.shape.rot.y;
    const float fwd = CVarGetFloat("gVrHeadOffsetForward", 0.0f), side = CVarGetFloat("gVrHeadOffsetSide", 0.0f);
    const float headOff[2] = { Math_SinS(yaw) * fwd - Math_CosS(yaw) * side,
                               Math_CosS(yaw) * fwd + Math_SinS(yaw) * side };
    float view[2];
    ClampDisk(slack, headOff, view);
    sDebug.offset = sqrtf(view[0] * view[0] + view[1] * view[1]);
    if (CVarGetInteger("gVrBodyLog", 0)) {
        if (sLog == nullptr) {
            sLog = fopen("vrbody_log.csv", "w");
            if (sLog != nullptr) {
                fprintf(sLog, "frame,action,bodyX,bodyZ,linVel,wall,resPreX,resPreZ,pushX,pushZ,viewX,viewZ,slack\n");
            }
        }
        if (sLog != nullptr) {
            fprintf(sLog, "%u,%p,%.2f,%.2f,%.2f,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
                    (unsigned)(play != nullptr ? play->state.frames : 0), (void*)player->actionFunc, px, pz,
                    player->linearVelocity, (player->actor.bgCheckFlags & BGCHECKFLAG_WALL) ? 1 : 0, preRes[0],
                    preRes[1], pushX, pushZ, view[0], view[1], slack);
            fflush(sLog);
        }
    } else if (sLog != nullptr) {
        fclose(sLog);
        sLog = nullptr;
    }

    // Room left toward the wall Link is touching: while the small circle can still go further, the
    // vanilla "facing a wall" speed cap (down to 0.1 units/tick head-on) must not apply, or the view
    // would creep the last stretch instead of arriving at walking speed.
    sDebug.room = 0;
    if ((player->actor.bgCheckFlags & BGCHECKFLAG_WALL) && player->actor.wallPoly != nullptr) {
        const CollisionPoly* poly = player->actor.wallPoly;
        float nx = -COLPOLY_GET_NORMAL(poly->normal.x), nz = -COLPOLY_GET_NORMAL(poly->normal.z);
        const float nl = sqrtf(nx * nx + nz * nz);
        if (nl > 0.5f) {
            nx /= nl;
            nz /= nl;
            const float into = view[0] * nx + view[1] * nz;
            const float lateral = -view[0] * nz + view[1] * nx;
            const float reach = sqrtf(fmaxf(slack * slack - lateral * lateral, 0.0f));
            sDebug.room = (reach - into) > 0.25f ? 1 : 0;
        }
    }
    return sDebug.room != 0;
}

extern "C" void VrBody_NoteObjectPush(Actor* pusher, Actor* pushed, float dx, float dz, const float* pusherPos,
                                      float contactDist) {
    if (gPlayState == nullptr || pushed != &GET_PLAYER(gPlayState)->actor || !StaticPusher(pusher)) {
        return;
    }
    const u32 now = gPlayState->state.frames;
    // Remember where it touches (refreshed every push; the oldest entry makes room).
    if (pusherPos != nullptr && contactDist > 0.0f) {
        int slot = -1;
        for (int i = 0; i < sObstacleCount; i++) {
            if (sObstacles[i].actor == pusher) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            if (sObstacleCount < kMaxObstacles) {
                slot = sObstacleCount++;
            } else {
                slot = 0;
                for (int i = 1; i < sObstacleCount; i++) {
                    if (sObstacles[i].frame < sObstacles[slot].frame) {
                        slot = i;
                    }
                }
            }
        }
        sObstacles[slot] = { pusher, pusherPos[0] - pusher->world.pos.x, pusherPos[2] - pusher->world.pos.z,
                             contactDist, now };
    }
    if (dx == 0.0f && dz == 0.0f) {
        return;
    }
    if (now != sObjectPushFrame) {
        sObjectPush[0] = sObjectPush[1] = 0.0f;
        sObjectPushFrame = now;
    }
    sObjectPush[0] += dx;
    sObjectPush[1] += dz;
}

extern "C" void VrBody_ClampBodyMove(PlayState* play, Player* player, const float* from, float* to) {
    if (play == nullptr || player == nullptr || !IsRealPlayer(player) || !VrBody_Active()) {
        return;
    }
    // Everything that pushed the body recently, at the distance it touched him.
    const u32 now = play->state.frames;
    int w = 0;
    for (int i = 0; i < sObstacleCount; i++) {
        Obstacle& o = sObstacles[i];
        if (o.actor == nullptr || o.actor->update == nullptr || (now - o.frame) > kObstacleTicks ||
            !StaticPusher(o.actor)) {
            continue; // forgotten
        }
        sObstacles[w++] = o;
        const float cx = o.actor->world.pos.x + o.offX, cz = o.actor->world.pos.z + o.offZ;
        const float tx = to[0] - cx, tz = to[2] - cz;
        const float d = sqrtf(tx * tx + tz * tz);
        if (d >= o.dist || d < 1e-3f) {
            continue;
        }
        const float fx = from[0] - cx, fz = from[2] - cz;
        const float keep = fminf(sqrtf(fx * fx + fz * fz), o.dist);
        if (d < keep) {
            to[0] = cx + tx / d * keep;
            to[2] = cz + tz / d * keep;
        }
    }
    sObstacleCount = w;
    // The body's own object-collision radius, as the game pushes it with.
    const float bodyR = player->cylinder.dim.radius;
    for (Actor* a = play->actorCtx.actorLists[ACTORCAT_PROP].head; a != nullptr; a = a->next) {
        float r, h, y;
        if (!VrCarry_LightObjectCylinder(a, &r, &h, &y)) {
            continue;
        }
        // Only objects at Link's height (a pot on a ledge above or below is not in the way).
        const float base = a->world.pos.y + y;
        if (player->actor.world.pos.y + player->cylinder.dim.height < base || player->actor.world.pos.y > base + h) {
            continue;
        }
        const float minD = bodyR + r;
        const float tx = to[0] - a->world.pos.x, tz = to[2] - a->world.pos.z;
        const float d = sqrtf(tx * tx + tz * tz);
        if (d >= minD || d < 1e-3f) {
            continue;
        }
        // Never deeper into it than the body already was (the stick can shove it in a little, as vanilla;
        // the object pushes it back out next tick), never past its edge otherwise.
        const float fx = from[0] - a->world.pos.x, fz = from[2] - a->world.pos.z;
        const float keep = fminf(sqrtf(fx * fx + fz * fz), minD);
        if (d < keep) {
            to[0] = a->world.pos.x + tx / d * keep;
            to[2] = a->world.pos.z + tz / d * keep;
        }
    }
}

extern "C" bool VrBody_ClampView(PlayState* play, Player* player, float* head) {
    if (!VrBody_Active() || !IsRealPlayer(player) || head == nullptr || VrPause_LeanClamp() > 0.1f) {
        return false;
    }
    const float bx = player->actor.world.pos.x, bz = player->actor.world.pos.z;
    const float headOff[2] = { head[0] - bx, head[2] - bz };
    float view[2];
    ClampDisk(Slack(player), headOff, view);
    // Backstop for geometry the body's wall check can't see (it tests at waist height; a wall that
    // leans in or overhangs at eye height): sweep a slightly smaller circle from the body's centre to
    // the view at eye height and stop short of anything. Against an upright wall the view sits at
    // exactly the small radius from it, so this never engages there.
    const float len = sqrtf(view[0] * view[0] + view[1] * view[1]);
    float keep = 1.0f;
    if (len > 0.01f) {
        Vec3f from = { bx, player->actor.world.pos.y, bz };
        Vec3f to = { bx + view[0], from.y, bz + view[1] };
        Vec3f res;
        CollisionPoly* poly;
        s32 bgId;
        if (BgCheck_EntitySphVsWall3(&play->colCtx, &res, &to, &from, SmallRadius() * 0.75f, &poly, &bgId,
                                     &player->actor, head[1] - from.y)) {
            keep = ((res.x - from.x) * view[0] + (res.z - from.z) * view[1]) / (len * len);
            keep = fminf(fmaxf(keep, 0.0f), 1.0f);
        }
    }
    // head + residual = the view; place the head so that comes out at keep x the clamped offset.
    float res2[2];
    VR_GetRoomscaleDesired(res2);
    head[0] = bx + view[0] * keep - res2[0];
    head[2] = bz + view[1] * keep - res2[1];
    return true;
}

extern "C" void VrBody_GetDebug(VrBodyDebug* out) {
    if (out != nullptr) {
        *out = sDebug;
    }
}
