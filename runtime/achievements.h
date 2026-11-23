// Achievements: Metroid Prime's milestones - its bosses, upgrades, maps, rooms, scans, Hard mode and a few challenges
// (the milestones of RetroAchievements' Metroid Prime set for the GameCube, by TheMysticalOne and Ghal416; names and
// words our own) - earned as they happen in play (runtime/hle/achievements.c) and kept in achievements.txt in the data
// folder, apart from the game's saves. The F1 menu's Achievements tab and the popup when one is earned:
// runtime/ui/achievements_ui.mm. Started from kryptalivian's pull request #100.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { ACH_STORY, ACH_UPGRADES, ACH_CHALLENGES, ACH_HARD, ACH_EXPLORE, ACH_COMPLETE, ACH_GROUPS };
// (the picture on each: runtime/ui/achievements_ui.mm draws them)
enum { ACH_BADGE_BOSS, ACH_BADGE_BEAM, ACH_BADGE_GEAR, ACH_BADGE_SUIT, ACH_BADGE_MISSILE, ACH_BADGE_BOMB,
       ACH_BADGE_ARTIFACT, ACH_BADGE_ESCAPE, ACH_BADGE_MAP, ACH_BADGE_SCAN, ACH_BADGE_TIME };

typedef struct {
    const char *id;     // kept in achievements.txt: never changed once released
    const char *title, *desc;
    int group, badge;
} AchInfo;

int ach_count(void);
const AchInfo *ach_info(int i);
const char *ach_group_name(int group);
// when it was earned (Unix time), or 0
int64_t ach_earned(int i);
// how far along a counted one is (items, scans, artifacts, a world's rooms), from the latest play: false when it has no
// count or none is known yet (nothing played since the start). percent: have is a percentage (need: the one to reach)
bool ach_progress(int i, int *have, int *need, bool *percent);
// achievements.txt couldn't be read or written (the tab says so)
bool ach_file_error(void);
// the next one earned since the last call, for the popup (-1: none)
int ach_next_popup(void);

// From the game (runtime/hle/playlog.c, runtime/services/fs.c; the game's thread):
// each frame Samus is in play: the game state, her player state and the state manager
void achievements_frame(uint64_t game_state, uint64_t player_state, uint64_t state_manager);
// she entered another room (its name: "Phendrana Drifts / 19_Ice_Thardus", NULL until the names are read)
void achievements_room(const uint8_t id[16], const char *name);
// a save file is being read: what it holds isn't earned now (the next frame in play is where counting starts again)
void achievements_save_read(void);

#ifdef __cplusplus
}
#endif
