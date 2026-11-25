/* runtime/achievements.c: presence formatting, version normalization, 50-achievement evaluation,
 * no-op without RL_SESSION, and full channel round-trip against a local fake RecompLauncher Unix socket.
 * See .github/workflows/mrbuild.yml for the compile command. */
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

volatile int game_presence[4] = {-1, 0, 0, 0};
#include "achievements.c"

static void has(const char *s, const char *want, int yes) {
    if (!strstr(s, want) != !yes) {
        fprintf(stderr, "line: %s\n  expected %s: %s\n", s, yes ? "to contain" : "NOT to contain", want);
        exit(1);
    }
}

static int read_line(int fd, char *buf, size_t cap) {
    size_t n = 0;
    struct pollfd p = {fd, POLLIN, 0};
    while (n + 1 < cap) {
        int r = poll(&p, 1, 2000);
        if (r <= 0) return -1;
        char ch;
        ssize_t k = read(fd, &ch, 1);
        if (k <= 0) return -1;
        if (ch == '\n') { buf[n] = 0; return (int)n; }
        buf[n++] = ch;
    }
    buf[n] = 0;
    return (int)n;
}

int main(void) {
    /* 1. Version string normalization */
    assert(!strcmp(ach_version_string("v0.1.0"), "0.1.0"));
    assert(!strcmp(ach_version_string("1.2.3"), "1.2.3"));
    assert(!strcmp(ach_version_string(""), ""));
    assert(!strcmp(ach_version_string("dev"), ""));
    assert(!strcmp(ach_version_string(NULL), ""));

    /* 1b. Achievement evaluation: chapters, difficulties, Platinum, Infinite Climax Pure Platinum,
     * Rodin, Balder no-damage, weapons, Umbran Techniques, Muspelheim, resting places, Witch Trials I-V */
    {
        MrAchSnapshot s;
        memset(&s, 0, sizeof s);
        memset(s.story, -1, sizeof s.story);
        assert(mr_ach_eval(&s) == 0ull);
        uint32_t total_score = 0;
        for (uint32_t i = 0; i < MR_ACH_COUNT; i++) {
            assert(mr_ach_defs[i].id == MR_ACH_BASE + i);
            total_score += mr_ach_defs[i].score;
        }
        assert(total_score == 1710u);

        /* Clearing a single chapter on 1st Climax unlocks only that chapter */
        s.story[0][0] = MR_ACH_MEDAL_STONE;
        assert(mr_ach_eval(&s) == (1ull << 0));

        /* Pure Platinum on 3rd Climax (diff 2) does NOT unlock the Infinite Climax Pure Platinum achievement */
        s.story[0][2] = MR_ACH_MEDAL_PURE_PLATINUM;
        assert(mr_ach_eval(&s) == (1ull << 0));

        /* Platinum (7) on Infinite Climax (diff 3) does NOT unlock Pure Platinum on Infinite Climax */
        s.story[0][3] = MR_ACH_MEDAL_PLATINUM;
        assert(mr_ach_eval(&s) == (1ull << 0));

        /* Pure Platinum (8) on Infinite Climax (diff 3) unlocks #22 (bit 21) */
        s.story[0][3] = MR_ACH_MEDAL_PURE_PLATINUM;
        assert(mr_ach_eval(&s) == ((1ull << 0) | (1ull << 21)));

        /* Clearing all 17 chapters on 1st Climax unlocks all 17 chapters + Sovereign Power (#18),
         * NOT 3rd Climax / Infinite Climax / Platinum Witch */
        memset(&s, 0, sizeof s);
        memset(s.story, -1, sizeof s.story);
        for (uint32_t i = 0; i < MR_ACH_STORY_CHAPTERS; i++) s.story[i][0] = 0;
        uint64_t all_ch = (1ull << MR_ACH_STORY_CHAPTERS) - 1ull;
        assert(mr_ach_eval(&s) == (all_ch | (1ull << 17)));

        /* Clearing all 17 chapters on Infinite Climax (diff 3) retroactively unlocks Sovereign Power (#18),
         * 3rd Climax Witch (#19), and Infinite Climax (#20); Gold (6) does NOT unlock Platinum Witch (#21) */
        memset(&s, 0, sizeof s);
        memset(s.story, -1, sizeof s.story);
        for (uint32_t i = 0; i < MR_ACH_STORY_CHAPTERS; i++) s.story[i][3] = MR_ACH_MEDAL_GOLD;
        assert(mr_ach_eval(&s) == (all_ch | (1ull << 17) | (1ull << 18) | (1ull << 19)));

        /* Mixed 3rd Climax (2) and Infinite Climax (3) unlocks Sovereign Power + 3rd Climax Witch, NOT Infinite Climax */
        s.story[5][3] = -1;
        s.story[5][2] = MR_ACH_MEDAL_PLATINUM;
        assert(mr_ach_eval(&s) == (all_ch | (1ull << 17) | (1ull << 18)));

        /* All Platinums (7) / Pure Platinums (8) on 2nd Climax unlocks Platinum Witch (#21), NOT any Pure Climax (#22..#38) */
        memset(&s, 0, sizeof s);
        memset(s.story, -1, sizeof s.story);
        for (uint32_t i = 0; i < MR_ACH_STORY_CHAPTERS; i++)
            s.story[i][1] = (i & 1) ? MR_ACH_MEDAL_PURE_PLATINUM : MR_ACH_MEDAL_PLATINUM;
        assert(mr_ach_eval(&s) == (all_ch | (1ull << 17) | (1ull << 20)));

        /* One chapter short of Platinum does NOT unlock Platinum Witch (#21) */
        s.story[16][1] = MR_ACH_MEDAL_GOLD;
        assert(mr_ach_eval(&s) == (all_ch | (1ull << 17)));

        /* All 17 chapters Pure Platinum on Infinite Climax unlocks chapters, all 3 difficulties, Platinum Witch,
         * all 17 Pure Climax (#22..#38), and Pure Platinum Witch (#50, bit 49) */
        memset(&s, 0, sizeof s);
        memset(s.story, -1, sizeof s.story);
        for (uint32_t i = 0; i < MR_ACH_STORY_CHAPTERS; i++) s.story[i][3] = MR_ACH_MEDAL_PURE_PLATINUM;
        uint64_t all_pure = ((1ull << MR_ACH_STORY_CHAPTERS) - 1ull) << 21;
        assert(mr_ach_eval(&s) == (all_ch | (1ull << 17) | (1ull << 18) | (1ull << 19) | (1ull << 20) | all_pure | (1ull << 49)));

        /* Bewitchments, weapons (13 permanent weapons, excluding Arwing ID 11), and Witch Trials I-V */
        memset(&s, 0, sizeof s);
        memset(s.story, -1, sizeof s.story);
        s.bewitchments_mask = MR_ACH_BEWITCH_RODIN;
        assert(mr_ach_eval(&s) == (1ull << 38));
        s.bewitchments_mask |= MR_ACH_BEWITCH_BALDER_NODMG;
        assert(mr_ach_eval(&s) == ((1ull << 38) | (1ull << 39)));
        /* 12 weapons + Arwing (bit 11 = 0x00100000) is NOT enough */
        s.weapons_mask = (MR_ACH_ALL_WEAPONS_MASK & ~(0x80000000u >> 15)) | (0x80000000u >> 11);
        assert(mr_ach_eval(&s) == ((1ull << 38) | (1ull << 39)));
        /* All 13 permanent weapons (without Arwing) unlocks #41 (bit 40) */
        s.weapons_mask = MR_ACH_ALL_WEAPONS_MASK;
        assert(mr_ach_eval(&s) == ((1ull << 38) | (1ull << 39) | (1ull << 40)));
        s.bewitchments_mask |= MR_ACH_BEWITCH_ALL_SKILLS | MR_ACH_BEWITCH_MUSPELHEIM | MR_ACH_BEWITCH_RESTING_PLACES;
        assert(mr_ach_eval(&s) == ((1ull << 38) | (1ull << 39) | (1ull << 40) | (1ull << 41) | (1ull << 42) | (1ull << 43)));
        for (uint32_t i = 0; i < MR_ACH_WITCH_TRIALS; i++) s.witch_trial_cleared[i] = 1;
        uint64_t all_wt = ((1ull << MR_ACH_WITCH_TRIALS) - 1ull) << 44;
        assert(mr_ach_eval(&s) == ((1ull << 38) | (1ull << 39) | (1ull << 40) | (1ull << 41) | (1ull << 42) | (1ull << 43) | all_wt));
    }

    /* 2. Presence formatting */
    char buf[256];
    achievements_format_presence(buf, sizeof buf, (int[]){-1, 0, 0, 0});
    assert(!strcmp(buf, "In the menus"));
    achievements_format_presence(buf, sizeof buf, (int[]){4, 3, 2, 0});
    assert(!strcmp(buf, "Chapter III: Paradiso - The Gates of Paradise - Verse 2 (\xe2\x88\x9e Climax)"));
    achievements_format_presence(buf, sizeof buf, (int[]){2, 1, 0, 0});
    assert(!strcmp(buf, "Chapter I: Noatun, The City of Genesis (2nd Climax)"));
    achievements_format_presence(buf, sizeof buf, (int[]){26, 3, 0, 0});
    assert(!strcmp(buf, "Witch Trial V (\xe2\x88\x9e Climax)"));
    achievements_format_presence(buf, sizeof buf, (int[]){22, 9, 5, 0});
    assert(!strcmp(buf, "Witch Trial I - Verse 5"));

    /* 3. No-op when RL_SESSION is unset */
    unsetenv("RL_SESSION");
    achievements_start();
    assert(!g_ach_enabled && !g_ach_inited);
    achievements_event(ACH_EV_DEFINE, 1, "Prologue Clear", "Test", 15);
    achievements_event(ACH_EV_READY, 0, NULL, NULL, 0);
    achievements_event(ACH_EV_UNLOCK, 1, NULL, NULL, 0);
    achievements_event(ACH_EV_FRAME, 0, NULL, NULL, 0);
    assert(!rl_ach_connected());

    /* 4. Full Unix-socket round-trip against a fake RecompLauncher */
    char sock_path[108];
    snprintf(sock_path, sizeof sock_path, "/tmp/mr_ach_test_%d.sock", (int)getpid());
    unlink(sock_path);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(srv >= 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock_path);
    assert(bind(srv, (struct sockaddr *)&sa, sizeof sa) == 0);
    assert(listen(srv, 1) == 0);

    setenv("RL_SESSION", sock_path, 1);

    achievements_start();
    assert(g_ach_enabled && !g_ach_inited);

    /* Queue definitions before ready so hello + one complete native_list go out together */
    achievements_event(ACH_EV_DEFINE, 1, "Prologue Clear", "Complete Prologue: World of Chaos on any difficulty.", 15);
    achievements_event(ACH_EV_DEFINE, 18, "Sovereign Power", "Complete all 17 story chapters on any difficulty.", 50);
    achievements_event(ACH_EV_READY, 0, NULL, NULL, 0);
    assert(g_ach_inited && rl_ach_connected());

    int cli = accept(srv, NULL, NULL);
    assert(cli >= 0);

    char line[4096];
    /* First two messages MUST be hello, then one native_list */
    assert(read_line(cli, line, sizeof line) > 0);
    has(line, "\"t\":\"hello\"", 1);
    has(line, "\"console\":\"switch\"", 1);
    has(line, "\"game\":\"moon-river\"", 1);
    has(line, "\"version\":\"0.1.0\"", 1);

    assert(read_line(cli, line, sizeof line) > 0);
    has(line, "\"t\":\"native_list\"", 1);
    has(line, "\"id\":1", 1);
    has(line, "\"title\":\"Prologue Clear\"", 1);
    has(line, "\"description\":\"Complete Prologue: World of Chaos on any difficulty.\"", 1);
    has(line, "\"id\":18", 1);
    has(line, "\"gamerscore\":50", 1);

    /* Save loaded -> save_loaded message + grace window classifies polled unlock as earlier */
    achievements_save_loaded();
    assert(read_line(cli, line, sizeof line) > 0);
    has(line, "\"t\":\"save_loaded\"", 1);

    achievements_event(ACH_EV_BITFIELD, 1, NULL, NULL, 1);
    assert(read_line(cli, line, sizeof line) > 0);
    has(line, "\"t\":\"native\"", 1);
    has(line, "\"id\":1", 1);
    has(line, "\"earlier\":true", 1);

    /* Live unlock after save-load grace window ends */
    __atomic_store_n(&g_ach_save_grace, 0, __ATOMIC_RELEASE);
    achievements_event(ACH_EV_UNLOCK, 18, NULL, NULL, 0);
    assert(read_line(cli, line, sizeof line) > 0);
    has(line, "\"t\":\"native\"", 1);
    has(line, "\"id\":18", 1);
    has(line, "\"earlier\":true", 0);

    /* Casual voiding */
    achievements_event(ACH_EV_CASUAL, 0, "debug", NULL, 0);
    assert(read_line(cli, line, sizeof line) > 0);
    has(line, "\"t\":\"casual\"", 1);
    has(line, "\"reason\":\"debug\"", 1);

    /* Presence update on frame tick */
    game_presence[0] = 4; game_presence[1] = 2; game_presence[2] = 0; game_presence[3] = 0;
    achievements_event(ACH_EV_FRAME, 0, NULL, NULL, 0);
    assert(read_line(cli, line, sizeof line) > 0);
    has(line, "\"t\":\"presence\"", 1);
    has(line, "Chapter III: Paradiso - The Gates of Paradise (3rd Climax)", 1);

    /* Launcher -> game message: quiet mode */
    assert(!rl_ach_quiet_toasts());
    const char *quiet_msg = "{\"t\":\"quiet\"}\n";
    assert(write(cli, quiet_msg, strlen(quiet_msg)) == (ssize_t)strlen(quiet_msg));
    for (int i = 0; i < 10 && !rl_ach_quiet_toasts(); i++) {
        usleep(5000);
        achievements_event(ACH_EV_FRAME, 0, NULL, NULL, 0);
    }
    assert(rl_ach_quiet_toasts());

    achievements_shutdown();
    close(cli);
    close(srv);
    unlink(sock_path);

    puts("achievements_test: ok");
    return 0;
}
