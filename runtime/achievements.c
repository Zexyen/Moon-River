/* [moonriver] Recomp Achievements integration (github.com/elripalda/RecompAchievements).
 * Connects to RecompLauncher over RL_SESSION when present; otherwise every entry point is a no-op.
 * Sends Moon River's 50-achievement set (story chapter clears, difficulty completion, Platinum & Infinite Climax
 * Pure Platinum mastery, Rodin secret boss, Balder no-damage, weapons, Umbran Techniques, Muspelheim, resting
 * places, and Witch Trials I-V; see docs/ACHIEVEMENTS.md), plus Rich Presence from game_presence[4]. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "achievements.h"
#include "rl_achievements.h"

#ifndef MR_VERSION
#define MR_VERSION ""
#endif

extern __declspec(dllimport) volatile int game_presence[4];

static const struct { int id; const char *name; } mr_chapters[] = {
    { 0, "Records of Time"},
    { 1, "Prologue: World of Chaos"},
    { 2, "Chapter I: Noatun, The City of Genesis"},
    { 3, "Chapter II: A Remembrance of Time"},
    { 4, "Chapter III: Paradiso - The Gates of Paradise"},
    { 5, "Chapter IV: The Two Meet"},
    { 6, "Chapter V: The Cathedral of Cascades"},
    { 7, "Chapter VI: The Bridge to the Heavens"},
    { 8, "Chapter VII: The Ark"},
    { 9, "Chapter VIII: An Ancient Civilization"},
    {10, "Chapter IX: The Gates of Hell"},
    {11, "Chapter X: The Depths"},
    {12, "Chapter XI: Inferno and Its Ruler"},
    {13, "Chapter XII: The Lumen Sage"},
    {14, "Chapter XIII: Vigrid, City of D\xc3\xa9j\xc3\xa0 Vu"},
    {15, "Chapter XIV: The Witch Hunts"},
    {16, "Chapter XV: Truth"},
    {17, "Chapter XVI: Sovereign Power"},
    {22, "Witch Trial I"},
    {23, "Witch Trial II"},
    {24, "Witch Trial III"},
    {25, "Witch Trial IV"},
    {26, "Witch Trial V"},
};
static const char *const mr_difficulties[4] = { "1st Climax", "2nd Climax", "3rd Climax", "\xe2\x88\x9e Climax" };

static int g_ach_enabled;
static int g_ach_inited;
static int g_ach_pending_save;
static int g_ach_save_grace;
static unsigned g_ach_frame;

static const char *ach_version_string(const char *raw) {
    if (!raw || !raw[0]) return "";
    if (raw[0] == 'v' && raw[1] >= '0' && raw[1] <= '9') return raw + 1;
    if (raw[0] >= '0' && raw[0] <= '9') return raw;
    return "";
}

int achievements_format_presence(char *out, size_t n, const int g[4]) {
    const char *chapter = NULL;
    for (size_t i = 0; i < sizeof mr_chapters / sizeof *mr_chapters; i++)
        if (mr_chapters[i].id == g[0]) { chapter = mr_chapters[i].name; break; }
    if (!chapter) return snprintf(out, n, "In the menus");
    char verse[20] = "";
    if (g[2] > 0) snprintf(verse, sizeof verse, " - Verse %d", g[2]);
    const char *difficulty = g[1] >= 0 && g[1] < 4 ? mr_difficulties[g[1]] : "";
    if (difficulty[0])
        return snprintf(out, n, "%s%s (%s)", chapter, verse, difficulty);
    return snprintf(out, n, "%s%s", chapter, verse);
}

static void ach_ensure_init(void) {
    if (!g_ach_enabled || g_ach_inited) return;
    g_ach_inited = 1;
    rl_ach_set_info("moon-river", ach_version_string(MR_VERSION));
    int connected = rl_ach_init("switch", NULL, NULL);
    if (connected) {
        printf("achievements: connected to RecompLauncher\n");
        if (__atomic_exchange_n(&g_ach_pending_save, 0, __ATOMIC_ACQ_REL)) rl_ach_save_loaded();
    }
}

static void ach_update_presence(void) {
    int now[4];
    for (int i = 0; i < 4; i++) now[i] = game_presence[i];
    char text[256];
    achievements_format_presence(text, sizeof text, now);
    rl_ach_presence(text);
}

void achievements_start(void) {
    const char *session = getenv("RL_SESSION");
    if (!session || !session[0]) return;
    g_ach_enabled = 1;
    atexit(achievements_shutdown);
}

void achievements_shutdown(void) {
    g_ach_enabled = 0;
    if (!g_ach_inited) return;
    g_ach_inited = 0;
    rl_ach_shutdown();
}

void achievements_save_loaded(void) {
    if (!g_ach_enabled) return;
    /* Hold a 180-frame grace window so progress loaded from save*.sav on the save worker thread
     * is classified as pre-existing (earlier) on the next periodic milestone poll. */
    __atomic_store_n(&g_ach_save_grace, 180, __ATOMIC_RELEASE);
    if (g_ach_inited) rl_ach_save_loaded();
    else __atomic_store_n(&g_ach_pending_save, 1, __ATOMIC_RELEASE);
}

void achievements_event(int kind, uint32_t id, const char *title, const char *desc, uint32_t score) {
    if (!g_ach_enabled) return;
    switch (kind) {
    case ACH_EV_FRAME: {
        int grace = __atomic_load_n(&g_ach_save_grace, __ATOMIC_ACQUIRE);
        if (grace > 0) __atomic_compare_exchange_n(&g_ach_save_grace, &grace, grace - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
        if (!g_ach_inited) {
            /* Fallback: connect even if ACH_EV_READY was delayed. */
            if (++g_ach_frame >= 300) ach_ensure_init();
            return;
        }
        rl_ach_frame();
        if (rl_ach_connected() && (g_ach_frame++ & 63) == 0) ach_update_presence();
        break;
    }
    case ACH_EV_DEFINE:
        rl_ach_native_define(id, title, desc, score);
        break;
    case ACH_EV_READY:
        ach_ensure_init();
        break;
    case ACH_EV_UNLOCK: {
        int grace = __atomic_load_n(&g_ach_save_grace, __ATOMIC_ACQUIRE);
        ach_ensure_init();
        if (grace > 0) rl_ach_native_earlier(id);
        else rl_ach_native(id);
        break;
    }
    case ACH_EV_EARLIER:
        ach_ensure_init();
        rl_ach_native_earlier(id);
        break;
    case ACH_EV_CASUAL:
        ach_ensure_init();
        rl_ach_casual(title ? title : "debug");
        break;
    case ACH_EV_SAVE_LOADED:
        achievements_save_loaded();
        break;
    case ACH_EV_BITFIELD: {
        /* A milestone appeared during periodic save-state polling without going through a live hook.
         * During a save load, in the menus (no chapter active), or when multiple bits appear at once,
         * it came from a save file (report as earlier/Casual). Otherwise it is a live unlock on an
         * older lifted build whose save hooks were not yet re-lifted. */
        int grace = __atomic_load_n(&g_ach_save_grace, __ATOMIC_ACQUIRE);
        ach_ensure_init();
        if (grace > 0 || game_presence[0] < 0 || score > 1) rl_ach_native_earlier(id);
        else rl_ach_native(id);
        break;
    }
    default:
        break;
    }
}
