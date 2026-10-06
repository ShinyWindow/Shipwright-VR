extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "src/overlays/actors/ovl_En_Diving_Game/z_en_diving_game.h"
#include "src/overlays/actors/ovl_En_Bom_Bowl_Pit/z_en_bom_bowl_pit.h"
#include "src/overlays/actors/ovl_En_In/z_en_in.h"
#include "src/overlays/actors/ovl_En_Kz/z_en_kz.h"
#include "src/overlays/actors/ovl_En_Daiku/z_en_daiku.h"
#include "src/overlays/actors/ovl_En_Heishi2/z_en_heishi2.h"
#include "src/overlays/actors/ovl_En_Go2/z_en_go2.h"
#include "src/overlays/actors/ovl_En_Ta/z_en_ta.h"
#include "src/overlays/actors/ovl_En_Zl1/z_en_zl1.h"
}
#include "VrCutsceneView.h"
#include <libultraship/bridge/consolevariablebridge.h>

// Every camera take-over in the game is one of three things, and each is told apart differently:
//   scripted cutscene  play->csCtx running. Carries no "which cutscene" id, so these rows are
//                      groups: warp songs (Demo_Kankyo warp effect alive), the fishing pond, boss
//                      scenes (a boss actor in the room), and everything else = story.
//   one-point shot     a sub-camera from OnePointCutscene_Init; camera->csId is the shot's id.
//   custom sub-camera  an actor's own Play_CreateSubCamera (csId stays Camera_Init's 0x7FFF); the
//                      owner is the NPC whose stored camera id is the active one.
// Ids and owners come from a sweep of every OnePointCutscene_Init / Play_CreateSubCamera call.

namespace {

enum Row {
    // Scripted cutscenes
    R_STORY,
    R_BOSS,
    R_WARP_SONG,
    R_FISHING,
    // Characters
    R_BLUE_WARP,
    R_ZELDA_COURTYARD,
    R_ZELDA_ESCAPE,
    R_KING_ZORA,
    R_GUARD,
    R_CARPENTERS,
    R_INGO,
    R_TALON,
    R_GORONS,
    R_DARUNIA,
    R_RUTO,
    R_GREAT_FAIRY,
    R_OWL,
    R_FROGS,
    R_SCARECROWS,
    R_SCRUBS,
    R_DIVING,
    R_BOWLING,
    R_MINIGAME_START,
    // World events
    R_WATERFALL,
    R_OASIS,
    R_LAKE_LOCK,
    R_GERUDO_GATES,
    R_DMC_BOULDER,
    R_GORON_POT,
    R_GORON_DOOR,
    R_BEAN,
    // Puzzle reveals
    R_ATTENTION,
    R_CHEST_FALL,
    R_RED_ICE,
    R_RETURN_TO_LINK,
    R_DEKU_TREE,
    R_DODONGO,
    R_JABU,
    R_FOREST,
    R_POE_SISTERS,
    R_FIRE,
    R_WATER,
    R_SHADOW,
    R_SPIRIT,
    // Gameplay
    R_CRAWLSPACE,
    R_GROTTO,
    R_FARORE,
    R_DEATH,
    R_FAIRY_REVIVE,
    R_WALLMASTER,
    R_CUCCO,
    R_GOLD_SKULLTULA,
    R_PILLAR,
    // Catch-alls
    R_OTHER_SHOT,
    R_OTHER_CAMERA,
    R_COUNT
};

const VrCutsceneRow kRows[R_COUNT] = {
    { "Scripted Cutscenes", "Story cutscenes", "gVrCs3P.Story", true,
      "Scripted story scenes: the Great Deku Tree opening, song lessons, Zelda, Sheik, the sages, "
      "area introductions, Epona's escape, the Door of Time." },
    { "Scripted Cutscenes", "Boss intros and deaths", "gVrCs3P.Boss", true,
      "Every boss's entrance and defeat, including Koume and Kotake and Phantom Ganon's ride in." },
    { "Scripted Cutscenes", "Warp songs", "gVrCs3P.WarpSong", false,
      "Leaving and arriving by Minuet, Bolero, Serenade, Requiem, Nocturne or Prelude." },
    { "Scripted Cutscenes", "Fishing pond", "gVrCs3P.Fishing", false,
      "The fishing pond owner's camera shots." },

    { "Characters", "Blue warps", "gVrCs3P.BlueWarp", false,
      "Stepping into a blue warp after a boss, and Ruto's warp out of Jabu-Jabu. The sage scene "
      "that follows is a story cutscene." },
    { "Characters", "Zelda in the castle courtyard", "gVrCs3P.ZeldaCourtyard", true,
      "Meeting Zelda at the window. Her story is a story cutscene; tick this too so the meeting "
      "doesn't switch views halfway through." },
    { "Characters", "Zelda during the Ganon's Tower escape", "gVrCs3P.ZeldaEscape", false,
      "Shots of Zelda opening the barred doors while you escape." },
    { "Characters", "King Zora", "gVrCs3P.KingZora", false, "King Zora moving aside." },
    { "Characters", "Gate guards", "gVrCs3P.Guard", false, "The Kakariko and castle gate guards." },
    { "Characters", "Freeing the carpenters", "gVrCs3P.Carpenters", false,
      "A freed carpenter running out of the Gerudo Fortress cell." },
    { "Characters", "Ingo", "gVrCs3P.Ingo", false, "Ingo's horse races at Lon Lon Ranch." },
    { "Characters", "Talon", "gVrCs3P.Talon", false, "Waking Talon, and the Cucco game." },
    { "Characters", "Gorons and Biggoron", "gVrCs3P.Gorons", false,
      "Biggoron, the rolling Goron, and the eye drops." },
    { "Characters", "Darunia", "gVrCs3P.Darunia", false, "Darunia's shots outside cutscenes." },
    { "Characters", "Ruto", "gVrCs3P.Ruto", false, "Ruto in the Water Temple." },
    { "Characters", "Great Fairies", "gVrCs3P.GreatFairy", false, "A Great Fairy rising from her fountain." },
    { "Characters", "Kaepora Gaebora (owl)", "gVrCs3P.Owl", false, "The owl's landing and take-off." },
    { "Characters", "Frogs", "gVrCs3P.Frogs", false, "The Zora's River frogs." },
    { "Characters", "Scarecrows", "gVrCs3P.Scarecrows", false, "Teaching and calling Pierre." },
    { "Characters", "Lost Woods Deku Scrubs", "gVrCs3P.Scrubs", false,
      "The mask-judging stage, the scrub judge, and the target-shooting prize." },
    { "Characters", "Zora diving game", "gVrCs3P.Diving", false, "Rupees thrown into the water." },
    { "Characters", "Bombchu Bowling prizes", "gVrCs3P.Bowling", false, "The prize shot after a strike." },
    { "Characters", "Minigame starts", "gVrCs3P.MinigameStart", false,
      "The opening shots of the shooting gallery and Bombchu Bowling." },

    { "World Events", "Zora's waterfall opening", "gVrCs3P.Waterfall", false, nullptr },
    { "World Events", "Desert Colossus oasis", "gVrCs3P.Oasis", false, nullptr },
    { "World Events", "Lake Hylia water lock", "gVrCs3P.LakeLock", false, nullptr },
    { "World Events", "Gerudo gates", "gVrCs3P.GerudoGates", false, nullptr },
    { "World Events", "Dodongo's Cavern boulder", "gVrCs3P.DmcBoulder", false,
      "Blowing up the boulder on Death Mountain Trail." },
    { "World Events", "Goron City spinning pot", "gVrCs3P.GoronPot", false, nullptr },
    { "World Events", "Goron City door", "gVrCs3P.GoronDoor", false, nullptr },
    { "World Events", "Magic bean growing", "gVrCs3P.Bean", false, nullptr },

    { "Puzzle Reveals", "Switches, torches and doors", "gVrCs3P.Attention", false,
      "Floor and eye switches, lighting every torch, sun switches, Song of Time blocks, a door "
      "unbarring after a room is cleared, water jets." },
    { "Puzzle Reveals", "Chests appearing", "gVrCs3P.ChestFall", false, nullptr },
    { "Puzzle Reveals", "Red ice melting", "gVrCs3P.RedIce", false, nullptr },
    { "Puzzle Reveals", "Camera gliding back to Link", "gVrCs3P.ReturnToLink", false,
      "The glide that ends many shots. Usually left matching the shots it follows." },
    { "Puzzle Reveals", "Deku Tree", "gVrCs3P.DekuTree", false, nullptr },
    { "Puzzle Reveals", "Dodongo's Cavern", "gVrCs3P.Dodongo", false, nullptr },
    { "Puzzle Reveals", "Jabu-Jabu's Belly", "gVrCs3P.Jabu", false, nullptr },
    { "Puzzle Reveals", "Forest Temple", "gVrCs3P.Forest", false,
      "Elevators, the twisting hallway, the paintings, the pillars." },
    { "Puzzle Reveals", "Poe Sisters appearing", "gVrCs3P.PoeSisters", false, nullptr },
    { "Puzzle Reveals", "Fire Temple", "gVrCs3P.Fire", false, nullptr },
    { "Puzzle Reveals", "Water Temple", "gVrCs3P.Water", false, "Water levels and shutters." },
    { "Puzzle Reveals", "Shadow Temple and graveyard", "gVrCs3P.Shadow", false, nullptr },
    { "Puzzle Reveals", "Spirit Temple", "gVrCs3P.Spirit", false, nullptr },

    { "Gameplay", "Leaving a crawlspace", "gVrCs3P.Crawlspace", false, nullptr },
    { "Gameplay", "Jumping out of a grotto", "gVrCs3P.Grotto", false, nullptr },
    { "Gameplay", "Farore's Wind", "gVrCs3P.Farore", false, nullptr },
    { "Gameplay", "Dying", "gVrCs3P.Death", false, nullptr },
    { "Gameplay", "Fairy revive", "gVrCs3P.FairyRevive", false, nullptr },
    { "Gameplay", "Wallmaster grab", "gVrCs3P.Wallmaster", false, nullptr },
    { "Gameplay", "Cucco revenge", "gVrCs3P.Cucco", false, nullptr },
    { "Gameplay", "Gold Skulltula kill", "gVrCs3P.GoldSkulltula", false,
      "Only with SoH's Gold Skulltula cutscene enhancement on." },
    { "Gameplay", "Lifting the giant pillar", "gVrCs3P.Pillar", false, "Golden Gauntlets." },

    { "Anything Else", "Other camera shots", "gVrCs3P.OtherShot", false,
      "A one-point camera shot not listed above." },
    { "Anything Else", "Other character cameras", "gVrCs3P.OtherCamera", false,
      "A character's own camera not listed above." },
};

struct ShotId {
    s16 csId;
    Row row;
};

// One-point shot id -> row (OnePointCutscene_Init callers).
const ShotId kShots[] = {
    { 0, R_BLUE_WARP },          // Door_Warp1
    { 1000, R_ZELDA_ESCAPE },    // En_Zl3
    { 4000, R_ZELDA_ESCAPE },    { 4010, R_ZELDA_ESCAPE }, { 4011, R_ZELDA_ESCAPE },
    { 1020, R_RETURN_TO_LINK },  // z_camera/z_play end-of-shot glide, Bg_Mori_Bigst/Elevator
    { 1100, R_FARORE },          // player
    { 2200, R_GOLD_SKULLTULA },  // En_Sw
    { 2210, R_BEAN },            // Obj_Bean
    { 2220, R_SCRUBS },          // En_Dnt_Demo
    { 2230, R_SCRUBS },          // En_Dnt_Jiji
    { 2340, R_SCRUBS },          { 2350, R_SCRUBS },
    { 4140, R_SCRUBS },          // En_Dnt_Nomal target prize
    { 2260, R_SCARECROWS },      { 2270, R_SCARECROWS }, { 2280, R_SCARECROWS }, // En_Kakasi(3)
    { 2290, R_CUCCO },           // En_Niw
    { 3010, R_DEKU_TREE },       { 3020, R_DEKU_TREE }, { 3040, R_DEKU_TREE },   // Bg_Ydan_*
    { 3050, R_DODONGO },         { 3060, R_DODONGO }, { 3065, R_DODONGO }, { 3380, R_DODONGO }, // Bg_Ddan/Dodoago
    { 3070, R_JABU },            { 3080, R_JABU }, { 3090, R_JABU }, { 3100, R_JABU }, // Bg_Bdan_Objects
    { 3120, R_WATER },           // Bg_Mizu_Water
    { 4510, R_WATER },           // Bg_Mizu_Shutter
    { 3130, R_RUTO },            // En_Ru2
    { 3140, R_POE_SISTERS },     { 3180, R_POE_SISTERS }, { 3190, R_POE_SISTERS }, // En_Po_Sisters
    { 3150, R_FOREST },          { 3160, R_FOREST }, { 3170, R_FOREST },        // Bg_Po_Event
    { 3220, R_FOREST },          { 3230, R_FOREST }, { 3240, R_FOREST },        // Bg_Mori_*
    { 3260, R_FOREST },          { 3261, R_FOREST }, { 6010, R_FOREST },
    { 3290, R_FIRE },            { 3310, R_FIRE }, { 3340, R_FIRE },            // Bg_Hidan_*
    { 3350, R_FIRE },            { 3360, R_FIRE },
    { 3330, R_DARUNIA },         // En_Du
    { 3390, R_SHADOW },          { 3400, R_SHADOW }, { 6001, R_SHADOW },        // Bg_Haka_*
    { 3410, R_SPIRIT },          { 3430, R_SPIRIT }, { 3440, R_SPIRIT }, { 3450, R_SPIRIT }, // Bg_Jya_*
    { 4020, R_PILLAR },          { 4021, R_PILLAR }, { 4022, R_PILLAR },        // Bg_Heavy_Block
    { 4100, R_WATERFALL },       // Bg_Spot03_Taki
    { 4110, R_FROGS },           // En_Fr
    { 4120, R_LAKE_LOCK },       // Bg_Spot06_Objects
    { 4150, R_OASIS },           // Bg_Spot11_Oasis
    { 4160, R_GERUDO_GATES },    { 4170, R_GERUDO_GATES }, // Bg_Spot12_Gate/Saku
    { 4175, R_TALON },           // En_Ta
    { 4180, R_DMC_BOULDER },     // Bg_Spot16_Bombstone
    { 4190, R_GORONS },          { 4200, R_GORONS },   // En_Go/En_Go2
    { 4210, R_GORON_POT },       { 4220, R_GORON_POT }, // Bg_Spot18_Basket
    { 4221, R_GORON_DOOR },      // Bg_Spot18_Shutter
    { 4500, R_CHEST_FALL },      // En_Box
    { 5010, R_ATTENTION },       // OnePointCutscene_Attention callers, En_Siofuki
    { 5110, R_GROTTO },          // player
    { 5120, R_RED_ICE },         // Obj_Ice_Poly
    { 8002, R_MINIGAME_START },  // En_Syateki_Man
    { 8010, R_MINIGAME_START },  // En_Bom_Bowl_Man
    { 8603, R_GREAT_FAIRY },     { 8604, R_GREAT_FAIRY }, // Bg_Dy_Yoseizo
    { 8700, R_OWL },             // En_Owl
    { 9500, R_WALLMASTER },      // En_Wallmas
    { 9601, R_CRAWLSPACE },      { 9602, R_CRAWLSPACE }, // player
    { 9806, R_DEATH },           // player
    { 9908, R_FAIRY_REVIVE },    // player
};

constexpr s16 kCustomCameraCsId = 0x7FFF; // Camera_Init's csId: not a one-point shot

Actor* FindActor(PlayState* play, s16 id) {
    for (s32 cat = 0; cat < ACTORCAT_MAX; cat++) {
        for (Actor* a = play->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            if (a->id == id) {
                return a;
            }
        }
    }
    return nullptr;
}

bool WarpSongPlaying(PlayState* play) {
    // Demo_Kankyo params 0x0F/0x10 (DEMOKANKYO_WARP_OUT/IN) run the warp-song light and its
    // scripted cutscene; the actor changes category while warping, so search them all.
    for (s32 cat = 0; cat < ACTORCAT_MAX; cat++) {
        for (Actor* a = play->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            if (a->id == ACTOR_DEMO_KANKYO && (a->params == 0x0F || a->params == 0x10)) {
                return true;
            }
        }
    }
    return false;
}

bool BossScene(PlayState* play) {
    // Phantom Ganon's ride in belongs to his horse (En_fHG), before the boss actor exists.
    return play->actorCtx.actorLists[ACTORCAT_BOSS].head != nullptr || FindActor(play, ACTOR_EN_FHG) != nullptr;
}

// The NPC whose stored camera id is the active one.
int CustomCameraOwner(PlayState* play, s16 camId) {
    Actor* a;
    if ((a = FindActor(play, ACTOR_EN_DIVING_GAME)) && ((EnDivingGame*)a)->subCamId == camId) {
        return R_DIVING;
    }
    if ((a = FindActor(play, ACTOR_EN_BOM_BOWL_PIT)) && ((EnBomBowlPit*)a)->camId == camId) {
        return R_BOWLING;
    }
    if ((a = FindActor(play, ACTOR_EN_IN)) && ((EnIn*)a)->camId == camId) {
        return R_INGO;
    }
    if ((a = FindActor(play, ACTOR_EN_KZ)) && ((EnKz*)a)->cutsceneCamera == camId) {
        return R_KING_ZORA;
    }
    if ((a = FindActor(play, ACTOR_EN_HEISHI2)) && ((EnHeishi2*)a)->cameraId == camId) {
        return R_GUARD;
    }
    if ((a = FindActor(play, ACTOR_EN_GO2)) && ((EnGo2*)a)->camId == camId) {
        return R_GORONS;
    }
    if ((a = FindActor(play, ACTOR_EN_TA)) && ((EnTa*)a)->subCamId == camId) {
        return R_TALON;
    }
    if ((a = FindActor(play, ACTOR_EN_ZL1)) && ((EnZl1*)a)->subCamId == camId) {
        return R_ZELDA_COURTYARD;
    }
    // Several carpenters can be loaded; any of them may own it.
    for (a = play->actorCtx.actorLists[ACTORCAT_NPC].head; a != nullptr; a = a->next) {
        if (a->id == ACTOR_EN_DAIKU && ((EnDaiku*)a)->subCamId == camId) {
            return R_CARPENTERS;
        }
    }
    // Ruto's warp keeps its camera id in a file static; the blue warp actor is the owner.
    if (FindActor(play, ACTOR_DOOR_WARP1) != nullptr) {
        return R_BLUE_WARP;
    }
    return -1;
}

// Which row the running camera take-over belongs to, or -1 when the game camera is ordinary.
int CurrentRow(PlayState* play) {
    const bool scripted = play->csCtx.state != CS_STATE_IDLE;
    Camera* cam = (play->activeCamera != CAM_ID_MAIN) ? play->cameraPtrs[play->activeCamera] : nullptr;

    if (!scripted && cam == nullptr) {
        return -1;
    }
    if (play->sceneNum == SCENE_FISHING_POND) {
        return R_FISHING;
    }
    if (scripted && WarpSongPlaying(play)) {
        return R_WARP_SONG;
    }
    if (cam != nullptr && cam->csId != kCustomCameraCsId) {
        for (const ShotId& shot : kShots) {
            if (shot.csId == cam->csId) {
                return shot.row;
            }
        }
        return R_OTHER_SHOT;
    }
    if (scripted) {
        return BossScene(play) ? R_BOSS : R_STORY;
    }
    // A custom sub-camera outside any script.
    int owner = CustomCameraOwner(play, play->activeCamera);
    if (owner >= 0) {
        return owner;
    }
    return BossScene(play) ? R_BOSS : R_OTHER_CAMERA;
}

} // namespace

const VrCutsceneRow* VrCutsceneView_Rows(int* count) {
    *count = R_COUNT;
    return kRows;
}

extern "C" bool VrCutsceneView_ThirdPerson(PlayState* play) {
    if (play == nullptr || !CVarGetInteger("gVrCutsceneThirdPerson", 0)) {
        return false;
    }
    int row = CurrentRow(play);
    return row >= 0 && CVarGetInteger(kRows[row].cvar, kRows[row].thirdPerson ? 1 : 0) != 0;
}
