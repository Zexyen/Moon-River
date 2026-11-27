/* [moonriver] Recomp Achievements bridge (github.com/elripalda/RecompAchievements).
 * Exposes Moon River's achievement set (story chapter clears, difficulty completion, Platinum & Infinite Climax
 * Pure Platinum mastery, Rodin secret boss, Balder no-damage, weapons, Umbran Techniques, Muspelheim, resting
 * places, and Witch Trials I-V; see docs/ACHIEVEMENTS.md) to RecompLauncher over RL_SESSION. Started without
 * RL_SESSION, every call is a no-op. */
#ifndef MR_ACHIEVEMENTS_H
#define MR_ACHIEVEMENTS_H

#include <stddef.h>
#include <stdint.h>

#define MR_ACH_STORY_CHAPTERS 17u
#define MR_ACH_WITCH_TRIALS   5u
#define MR_ACH_BASE           1u
#define MR_ACH_COUNT          50u

/* Chapter award enum in Bayonetta 2 save records (save_obj + 500 + diff * 0x804 + ch * 0x4c + 16):
 * 0..2 = unplayed/none, 3 = Stone, 4 = Bronze, 5 = Silver, 6 = Gold, 7 = Platinum, 8 = Pure Platinum.
 * In MrAchSnapshot.story, -1 = unplayed, 0 = cleared (state 2/3 without medal), 3..8 = Stone..Pure Platinum. */
#define MR_ACH_MEDAL_STONE         3
#define MR_ACH_MEDAL_BRONZE        4
#define MR_ACH_MEDAL_SILVER        5
#define MR_ACH_MEDAL_GOLD          6
#define MR_ACH_MEDAL_PLATINUM      7
#define MR_ACH_MEDAL_PURE_PLATINUM 8

/* Bayonetta 2 chapter IDs for the 17 story chapters (1..17) and 5 Witch Trials (22..26).
 * Chapter 0 is Records of Time (intro tutorial, excluded by Play History f_7100d9736c). */
static const uint32_t mr_ach_story_chapters[MR_ACH_STORY_CHAPTERS] = {
    1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u,
};
static const uint32_t mr_ach_witch_trial_chapters[MR_ACH_WITCH_TRIALS] = {
    22u, 23u, 24u, 25u, 26u,
};

/* Story Bewitchment bitmasks in save_obj + 432 (MSB-first: 0x80000000u >> id, setter f_7100d9045c):
 *   16: Liberator of Time (open all Umbra Witches' final resting places)
 *   18: Umbran Successor (acquire all skills / Umbran Techniques)
 *   23: End Ruler (complete all 22 Muspelheim challenges)
 *   25: Darkness Envelops Light (defeat Balder in Chapter XII without taking damage)
 *   29: Beautiful! (defeat Rodin in the Platinum Ticket battle) */
#define MR_ACH_BEWITCH_RESTING_PLACES (0x80000000u >> 16)
#define MR_ACH_BEWITCH_ALL_SKILLS     (0x80000000u >> 18)
#define MR_ACH_BEWITCH_MUSPELHEIM     (0x80000000u >> 23)
#define MR_ACH_BEWITCH_BALDER_NODMG   (0x80000000u >> 25)
#define MR_ACH_BEWITCH_RODIN          (0x80000000u >> 29)

/* All 13 permanent Bayonetta 2 weapons in save_obj + 0xa46c (MSB-first: 0x80000000u >> id, setters f_7100d8fa50 /
 * f_7100d96c68): IDs 0..8 (Love Is Blue, Rakshasa, Takemikazuchi, Alruna, Kafka, Chernobog, Chain Chomp, Undine,
 * Salamandra) and IDs 12..15 (Handguns, Scarborough Fair, Shuraba, Rodin). Excludes unused IDs 9..10 and the
 * temporary Star Fox costume weapon ID 11 (Arwing). */
#define MR_ACH_ALL_WEAPONS_MASK 0xff8f0000u

#define MR_ACH_ID_COMPLETE_GAME 18u

typedef struct {
    uint32_t id;
    const char *title;
    const char *desc;
    uint32_t score;
    const char *event;
} MrAchDef;

static const MrAchDef mr_ach_defs[MR_ACH_COUNT] = {
    /* 1..17: Story Chapter Clears (any difficulty) */
    { 1,  "Prologue Clear",              "Complete Prologue: World of Chaos on any difficulty.",                                10, "prologue_clear"},
    { 2,  "Chapter I Clear",             "Complete Chapter I: Noatun, The City of Genesis on any difficulty.",                  10, "chapter_1_clear"},
    { 3,  "Chapter II Clear",            "Complete Chapter II: A Remembrance of Time on any difficulty.",                       10, "chapter_2_clear"},
    { 4,  "Chapter III Clear",           "Complete Chapter III: Paradiso - The Gates of Paradise on any difficulty.",           10, "chapter_3_clear"},
    { 5,  "Chapter IV Clear",            "Complete Chapter IV: The Two Meet on any difficulty.",                                10, "chapter_4_clear"},
    { 6,  "Chapter V Clear",             "Complete Chapter V: The Cathedral of Cascades on any difficulty.",                    10, "chapter_5_clear"},
    { 7,  "Chapter VI Clear",            "Complete Chapter VI: The Bridge to the Heavens on any difficulty.",                   10, "chapter_6_clear"},
    { 8,  "Chapter VII Clear",           "Complete Chapter VII: The Ark on any difficulty.",                                    10, "chapter_7_clear"},
    { 9,  "Chapter VIII Clear",          "Complete Chapter VIII: An Ancient Civilization on any difficulty.",                   10, "chapter_8_clear"},
    {10,  "Chapter IX Clear",            "Complete Chapter IX: The Gates of Hell on any difficulty.",                           10, "chapter_9_clear"},
    {11,  "Chapter X Clear",             "Complete Chapter X: The Depths on any difficulty.",                                   10, "chapter_10_clear"},
    {12,  "Chapter XI Clear",            "Complete Chapter XI: Inferno and Its Ruler on any difficulty.",                       10, "chapter_11_clear"},
    {13,  "Chapter XII Clear",           "Complete Chapter XII: The Lumen Sage on any difficulty.",                             10, "chapter_12_clear"},
    {14,  "Chapter XIII Clear",          "Complete Chapter XIII: Vigrid, City of D\xc3\xa9j\xc3\xa0 Vu on any difficulty.",     10, "chapter_13_clear"},
    {15,  "Chapter XIV Clear",           "Complete Chapter XIV: The Witch Hunts on any difficulty.",                            10, "chapter_14_clear"},
    {16,  "Chapter XV Clear",            "Complete Chapter XV: Truth on any difficulty.",                                       10, "chapter_15_clear"},
    {17,  "Chapter XVI Clear",           "Complete Chapter XVI: Sovereign Power on any difficulty.",                            10, "chapter_16_clear"},
    /* 18..21: Game Completion, Difficulty & Platinum Mastery (higher difficulties retroactively unlock lower ones) */
    {18,  "Sovereign Power",             "Complete all 17 story chapters on any difficulty.",                                   50, "sovereign_power"},
    {19,  "3rd Climax Witch",            "Complete all 17 story chapters on 3rd Climax or higher.",                             50, "3rd_climax_witch"},
    {20,  "Infinite Climax",             "Complete all 17 story chapters on \xe2\x88\x9e Climax.",                              50, "infinite_climax"},
    {21,  "Platinum Witch",              "Earn a Platinum or Pure Platinum award on all 17 story chapters (any difficulty).",   50, "platinum_witch"},
    /* 22..38: Infinite Climax Pure Platinum Chapter Mastery */
    {22,  "Prologue: Pure Climax",       "Earn a Pure Platinum award in Prologue: World of Chaos on \xe2\x88\x9e Climax.",                                25, "prologue_pure_climax"},
    {23,  "Chapter I: Pure Climax",      "Earn a Pure Platinum award in Chapter I: Noatun, The City of Genesis on \xe2\x88\x9e Climax.",                  25, "chapter_1_pure_climax"},
    {24,  "Chapter II: Pure Climax",     "Earn a Pure Platinum award in Chapter II: A Remembrance of Time on \xe2\x88\x9e Climax.",                       25, "chapter_2_pure_climax"},
    {25,  "Chapter III: Pure Climax",    "Earn a Pure Platinum award in Chapter III: Paradiso - The Gates of Paradise on \xe2\x88\x9e Climax.",           25, "chapter_3_pure_climax"},
    {26,  "Chapter IV: Pure Climax",     "Earn a Pure Platinum award in Chapter IV: The Two Meet on \xe2\x88\x9e Climax.",                                25, "chapter_4_pure_climax"},
    {27,  "Chapter V: Pure Climax",      "Earn a Pure Platinum award in Chapter V: The Cathedral of Cascades on \xe2\x88\x9e Climax.",                    25, "chapter_5_pure_climax"},
    {28,  "Chapter VI: Pure Climax",     "Earn a Pure Platinum award in Chapter VI: The Bridge to the Heavens on \xe2\x88\x9e Climax.",                   25, "chapter_6_pure_climax"},
    {29,  "Chapter VII: Pure Climax",    "Earn a Pure Platinum award in Chapter VII: The Ark on \xe2\x88\x9e Climax.",                                    25, "chapter_7_pure_climax"},
    {30,  "Chapter VIII: Pure Climax",   "Earn a Pure Platinum award in Chapter VIII: An Ancient Civilization on \xe2\x88\x9e Climax.",                   25, "chapter_8_pure_climax"},
    {31,  "Chapter IX: Pure Climax",     "Earn a Pure Platinum award in Chapter IX: The Gates of Hell on \xe2\x88\x9e Climax.",                           25, "chapter_9_pure_climax"},
    {32,  "Chapter X: Pure Climax",      "Earn a Pure Platinum award in Chapter X: The Depths on \xe2\x88\x9e Climax.",                                   25, "chapter_10_pure_climax"},
    {33,  "Chapter XI: Pure Climax",     "Earn a Pure Platinum award in Chapter XI: Inferno and Its Ruler on \xe2\x88\x9e Climax.",                       25, "chapter_11_pure_climax"},
    {34,  "Chapter XII: Pure Climax",    "Earn a Pure Platinum award in Chapter XII: The Lumen Sage on \xe2\x88\x9e Climax.",                             25, "chapter_12_pure_climax"},
    {35,  "Chapter XIII: Pure Climax",   "Earn a Pure Platinum award in Chapter XIII: Vigrid, City of D\xc3\xa9j\xc3\xa0 Vu on \xe2\x88\x9e Climax.",     25, "chapter_13_pure_climax"},
    {36,  "Chapter XIV: Pure Climax",    "Earn a Pure Platinum award in Chapter XIV: The Witch Hunts on \xe2\x88\x9e Climax.",                            25, "chapter_14_pure_climax"},
    {37,  "Chapter XV: Pure Climax",     "Earn a Pure Platinum award in Chapter XV: Truth on \xe2\x88\x9e Climax.",                                       25, "chapter_15_pure_climax"},
    {38,  "Chapter XVI: Pure Climax",    "Earn a Pure Platinum award in Chapter XVI: Sovereign Power on \xe2\x88\x9e Climax.",                            25, "chapter_16_pure_climax"},
    /* 39..40: Secret Boss & Boss Mastery */
    {39,  "Devil's Duel",                "Defeat Rodin in the Platinum Ticket battle.",                                         50, "devils_duel"},
    {40,  "Darkness Envelops Light",     "Defeat Balder in Chapter XII without taking damage.",                                 50, "darkness_envelops_light"},
    /* 41..44: Arsenal, Techniques & Exploration */
    {41,  "Arsenal of Darkness",         "Unlock all 13 Bayonetta 2 weapons.",                                                  50, "arsenal_of_darkness"},
    {42,  "Umbran Successor",            "Acquire all Umbran Techniques.",                                                      50, "umbran_successor"},
    {43,  "End Ruler",                   "Complete all 22 Muspelheim challenges.",                                              50, "end_ruler"},
    {44,  "Liberator of Time",           "Open all of the Umbra Witches' final resting places.",                                50, "liberator_of_time"},
    /* 45..49: Witch Trials I-V */
    {45,  "Witch Trial I Clear",         "Complete Witch Trial I.",                                                             10, "witch_trial_1_clear"},
    {46,  "Witch Trial II Clear",        "Complete Witch Trial II.",                                                            25, "witch_trial_2_clear"},
    {47,  "Witch Trial III Clear",       "Complete Witch Trial III.",                                                           25, "witch_trial_3_clear"},
    {48,  "Witch Trial IV Clear",        "Complete Witch Trial IV.",                                                            50, "witch_trial_4_clear"},
    {49,  "Witch Trial V Clear",         "Complete Witch Trial V.",                                                             50, "witch_trial_5_clear"},
    /* 50: Ultimate Mastery */
    {50,  "Pure Platinum Witch",         "Earn a Pure Platinum award on all 17 story chapters on \xe2\x88\x9e Climax.",         50, "pure_platinum_witch"},
};

/* Snapshot of save-file chapter records, Bewitchments, and weapon unlocks used by mr_ach_eval().
 * story[i][d]: -1 = unplayed, 0 = cleared, 3..8 = Stone..Pure Platinum (d: 0=1st Climax, 1=2nd Climax, 2=3rd Climax, 3=Infinite Climax). */
typedef struct {
    int8_t story[MR_ACH_STORY_CHAPTERS][4];
    uint8_t witch_trial_cleared[MR_ACH_WITCH_TRIALS];
    uint32_t bewitchments_mask;
    uint32_t weapons_mask;
} MrAchSnapshot;

static inline uint64_t mr_ach_eval(const MrAchSnapshot *s) {
    uint64_t mask = 0;
    int all_any = 1, all_hard = 1, all_climax = 1, all_plat = 1, all_pure_climax = 1;
    for (uint32_t i = 0; i < MR_ACH_STORY_CHAPTERS; i++) {
        int cleared_any = 0, cleared_hard = 0, cleared_climax = 0, plat_any = 0, pure_climax = 0;
        for (int d = 0; d < 4; d++) {
            int8_t m = s->story[i][d];
            if (m < 0) continue;
            cleared_any = 1;
            if (d >= 2) cleared_hard = 1;
            if (d == 3) cleared_climax = 1;
            if (m >= MR_ACH_MEDAL_PLATINUM) plat_any = 1;
            if (d == 3 && m == MR_ACH_MEDAL_PURE_PLATINUM) pure_climax = 1;
        }
        if (cleared_any) mask |= 1ull << i;                    /* #1..#17 */
        else all_any = 0;
        if (!cleared_hard) all_hard = 0;
        if (!cleared_climax) all_climax = 0;
        if (!plat_any) all_plat = 0;
        if (pure_climax) mask |= 1ull << (21u + i);            /* #22..#38 */
        else all_pure_climax = 0;
    }
    if (all_any)         mask |= 1ull << 17;                   /* #18: Sovereign Power */
    if (all_hard)        mask |= 1ull << 18;                   /* #19: 3rd Climax Witch */
    if (all_climax)      mask |= 1ull << 19;                   /* #20: Infinite Climax */
    if (all_plat)        mask |= 1ull << 20;                   /* #21: Platinum Witch */
    if (s->bewitchments_mask & MR_ACH_BEWITCH_RODIN)          mask |= 1ull << 38; /* #39: Devil's Duel */
    if (s->bewitchments_mask & MR_ACH_BEWITCH_BALDER_NODMG)   mask |= 1ull << 39; /* #40: Darkness Envelops Light */
    if ((s->weapons_mask & MR_ACH_ALL_WEAPONS_MASK) == MR_ACH_ALL_WEAPONS_MASK)
        mask |= 1ull << 40;                                                       /* #41: Arsenal of Darkness */
    if (s->bewitchments_mask & MR_ACH_BEWITCH_ALL_SKILLS)     mask |= 1ull << 41; /* #42: Umbran Successor */
    if (s->bewitchments_mask & MR_ACH_BEWITCH_MUSPELHEIM)     mask |= 1ull << 42; /* #43: End Ruler */
    if (s->bewitchments_mask & MR_ACH_BEWITCH_RESTING_PLACES) mask |= 1ull << 43; /* #44: Liberator of Time */
    for (uint32_t i = 0; i < MR_ACH_WITCH_TRIALS; i++)
        if (s->witch_trial_cleared[i]) mask |= 1ull << (44u + i);                 /* #45..#49: Witch Trials I-V */
    if (all_pure_climax) mask |= 1ull << 49;                   /* #50: Pure Platinum Witch */
    return mask;
}

enum {
    ACH_EV_FRAME = 0,       /* game frame tick: drain incoming launcher messages and update presence */
    ACH_EV_DEFINE,          /* (id, title, desc, score): queue a native achievement definition before READY */
    ACH_EV_READY,           /* definitions queued: initialize rl_ach_init() so hello + native_list go out together */
    ACH_EV_UNLOCK,          /* (id): live achievement unlock during gameplay */
    ACH_EV_EARLIER,         /* (id): pre-existing achievement reconciled from a loaded save (earlier: true) */
    ACH_EV_CASUAL,          /* (title=reason): void the session for Hardcore (e.g. debug command) */
    ACH_EV_SAVE_LOADED,     /* a save file was opened for reading */
    ACH_EV_BITFIELD,        /* (id, score=new_count): achievement appeared during periodic save-state polling */
};

void achievements_start(void);
void achievements_shutdown(void);
void achievements_save_loaded(void);
void achievements_event(int kind, uint32_t id, const char *title, const char *desc, uint32_t score);
int achievements_format_presence(char *out, size_t n, const int g[4]);

#endif
