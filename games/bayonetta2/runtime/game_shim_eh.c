/* Shim of the game DLL generated WITH landing-pad resume entries (tools/mrbuild/src/lifter/eh.rs).
 * Everything of game_shim.c, plus what the exception unwinder needs from main: the resume table, the host's
 * nro_catch_call for functions with a catch handler, and Bayonetta 2's game-specific patch helpers called from
 * the lifted code (games/bayonetta2/mrbuild/post_lift.rs):
 *   - Findings 146 & 150: cutscenes release deleted characters and rebind safely when no package is unloading;
 *   - Finding 148: action prompt ring crescent rotates to match printed-letter button positions;
 *   - Findings 107, 113, 114, 129, 134, 136, 139, 144: >60 fps (up to 120 fps) clock, smart tick counters,
 *     float timer overshoot reloads, closed-form smoothings, cinematic shot holds, and time-channel 0 slowmo;
 *   - Ultrawide (21:9 / 32:9): Hor+ render width & centered 2D projection driven by RECOMPS_UW_WIDTH / RECOMPS_ASPECT.
 *
 * Uses only standard C / UCRT symbols and avoids TLS runtime dependencies so it links cleanly into both
 * Linux game.so and Windows -nostdlib game.dll (UCRT only). */
#include "game_shim.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { uint64_t lp; void (*fn)(Ctx *, uint64_t); } NroResume;
extern const NroResume resume_table_main[];
extern const int resume_count_main;

static void (*host_catch_call)(Ctx *, void (*)(Ctx *, uint64_t));
void nro_catch_call(Ctx *c, void (*body)(Ctx *, uint64_t)) { host_catch_call(c, body); }

__declspec(dllexport) void game_eh_bind(void (*catch_call)(Ctx *, void (*)(Ctx *, uint64_t)), const NroResume **table, int *count) {
    host_catch_call = catch_call;
    *table = resume_table_main;
    *count = resume_count_main;
}

/* What the runtime's FSR pass (runtime/nvn_vk_fsr.c) tells the game while it runs. */
__declspec(dllexport) volatile int game_fsr_w, game_fsr_h, game_fsr_phases;

typedef struct { volatile uint64_t dst, prev; } GameMvPair;
__declspec(dllexport) GameMvPair game_mv_pairs[1 << 16];

/* ============================================================================
 * 1. Cutscenes keep no deleted character (Findings 146 & 150)
 * ============================================================================ */
static int evfix_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMPS_EVENT_FIX"); v = !e || atoi(e); }
    return v;
}
static uint64_t evfix_slot[16];
static unsigned evfix_tries[16], evfix_wait[16];

static int evfix_unloading(void) {
    uint64_t m = rd64(0x7101868130ULL);
    if (!m) return 0;
    static const int lists[2][2] = { {0xb0, 0xbc}, {0xd0, 0xdc} };
    int n = 0;
    for (int l = 0; l < 2; l++) {
        uint64_t arr = rd64(m + lists[l][0]);
        uint32_t cnt = rd32(m + lists[l][1]);
        for (uint32_t i = 0; arr && i < cnt && i < 4096; i++) {
            uint64_t e = rd64(arr + 8 * (uint64_t)i);
            uint32_t st = e ? rd32(e + 0x60) : 0;
            n += st == 8 || st == 9;
        }
    }
    return n;
}

static int evfix_events(uint64_t *ev, int max) {
    int n = 0;
    uint64_t t = rd64(0x7101871808ULL);
    for (int i = 0; t && i < 8 && n < max; i++) ev[n++] = t + 64 * (uint64_t)i;
    uint64_t lst = rd64(0x7101871810ULL);
    if (lst) {
        uint64_t a = rd64(lst + 8), k = rd64(lst + 16);
        for (uint64_t j = 0; a && j < k && n < max; j++) ev[n++] = rd64(a + 8 * j);
    }
    return n;
}

void recomps_evfix_deleted(Ctx *c, uint64_t entry) {
    (void)c;
    if (!entry || !evfix_on()) return;
    uint64_t ev[72];
    int n = evfix_events(ev, 72);
    for (int e = 0; e < n; e++) {
        if (!ev[e] || !(rd32(ev[e]) & 1)) continue;
        uint64_t scene = rd64(ev[e] + 40);
        if (!scene) continue;
        static const int lists[2][2] = { {0xa8, 0xb4}, {0xd8, 0xe4} };
        for (int l = 0; l < 2; l++) {
            uint64_t arr = rd64(scene + lists[l][0]);
            int cnt = (int)rd32(scene + lists[l][1]);
            for (int j = 0; arr && j < cnt && j < 256; j++) {
                uint64_t s = arr + 192 * (uint64_t)j;
                if (rd64(s + 0x38) != entry) continue;
                wr64(s + 0x38, 0);
                wr64(s + 0x40, 0);
                int k = 0;
                while (k < 16 && evfix_slot[k] && evfix_slot[k] != s) k++;
                if (k < 16) { evfix_slot[k] = s; evfix_tries[k] = 0; evfix_wait[k] = 0; }
                printf("events: character %x deleted while event %x/%x/%x held it (scene slot %d): let go%s\n",
                       rd32(s + 0x2c), rd32(ev[e] + 4), rd32(ev[e] + 8), rd32(ev[e] + 12), j,
                       k < 16 ? "; bound again at its next update" : "");
                fflush(stdout);
            }
        }
    }
}

void recomps_evfix_slot(Ctx *c, uint64_t slot) {
    for (int k = 0; k < 16; k++) {
        if (evfix_slot[k] != slot) continue;
        if (rd64(slot + 0x38)) { evfix_slot[k] = 0; return; }
        if (++evfix_tries[k] > 120) {
            if (evfix_wait[k])
                printf("events: character %x not bound again (120 updates, %u of them waiting for character data on its way out)\n",
                       rd32(slot + 0x2c), evfix_wait[k]);
            else
                printf("events: character %x not bound again (nothing found in 120 updates)\n", rd32(slot + 0x2c));
            fflush(stdout);
            evfix_slot[k] = 0;
            return;
        }
        int out = evfix_unloading();
        if (out) {
            if (!evfix_wait[k]++) {
                printf("events: character %x: bound again later: %d character data on its way out (a chapter select or the title?)\n",
                       rd32(slot + 0x2c), out);
                fflush(stdout);
            }
            return;
        }
        wr32(slot + 0x94, rd32(slot + 0x94) & ~0x12u);
        Ctx save = *c;
        c->x[0] = slot; c->x[1] = 0; c->x[30] = 0;
        call_indirect(c, 0x7100e27b20ULL);
        *c = save;
        if (rd64(slot + 0x38)) {
            printf("events: character %x bound again (%u update%s later)\n",
                   rd32(slot + 0x2c), evfix_tries[k], evfix_tries[k] == 1 ? "" : "s");
            fflush(stdout);
            evfix_slot[k] = 0;
        }
        return;
    }
}

void recomps_evfix_test(Ctx *c) {
    static int ev = -2, at, slot, after;
    static unsigned long long upd, started;
    static uint64_t scene;
    static int ulog = -1, ulast, ulines;
    static unsigned long long uupd;
    if (ulog < 0) { const char *e = getenv("RECOMPS_EVENT_UNLOADLOG"); ulog = e && atoi(e); }
    if (ulog && ulines < 60) {
        int n = evfix_unloading();
        uupd++;
        if (n != ulast) {
            printf("events: TEST: character data on its way out: %d (logic update %llu)\n", n, uupd);
            fflush(stdout);
            ulast = n; ulines++;
        }
    }
    if (ev == -2) {
        const char *e = getenv("RECOMPS_EVENT_TEST");
        ev = -1;
        if (e && sscanf(e, "%d,%d,%d,%d", &ev, &at, &slot, &after) != 4) ev = -1;
    }
    if (ev < 0 || ev > 7) return;
    upd++;
    uint64_t t = rd64(0x7101871808ULL), s = t ? t + 64 * (uint64_t)ev : 0;
    if (!s) return;
    if (!scene) {
        if (upd >= (unsigned long long)at && (rd32(s) & 0x80000003u) == 3 && rd32(s + 20) == 2 && !(rd32(s) & 4)) {
            wr32(s, rd32(s) | 4);
            printf("events: TEST: event slot %d (%x) asked to start at update %llu\n", ev, rd32(s + 8), upd);
            fflush(stdout);
        }
        if (rd32(s + 20) == 3 && rd64(s + 40)) { scene = rd64(s + 40); started = upd; }
        return;
    }
    if (upd != started + (unsigned)after || rd64(s + 40) != scene) return;
    uint64_t arr = rd64(scene + 0xa8);
    if (!arr || slot < 0 || slot >= (int)rd32(scene + 0xb4)) return;
    uint64_t entry = rd64(arr + 192 * (uint64_t)slot + 0x38);
    if (!entry) return;
    printf("events: TEST: deletion of character %x (scene slot %d) requested %d updates into the scene\n",
           rd32(arr + 192 * (uint64_t)slot + 0x2c), slot, after);
    fflush(stdout);
    Ctx save = *c;
    c->x[0] = entry; c->x[30] = 0;
    call_indirect(c, 0x71009a6e44ULL);
    *c = save;
    ev = -1;
}

/* ============================================================================
 * 2. Action prompt ring angle (Finding 148)
 * ============================================================================ */
void recomps_prompt_ring(uint64_t at, uint32_t mask) {
    static int on = -1, said;
    if (on < 0) { const char *e = getenv("RECOMPS_PROMPT_RING"); on = !e || atoi(e); }
    static const int deg[4] = { 270, 180, 0, 90 };
    uint32_t t[4];
    int p[4], n = 0, a;
    for (int i = 0; i < 4; i++) t[i] = rd32(0x7101464340ULL + 4 * (uint64_t)i);
    if (!on || (t[0] == 0x40 && t[1] == 0x80 && t[2] == 0x10 && t[3] == 0x20)) return;
    if (mask != 0x10 && mask != 0x20 && mask != 0x40 && mask != 0x80 && mask != 0x50) return;
    for (int i = 0; i < 4; i++) if (t[i] & mask) p[n++] = i;
    if (n == 1) a = deg[p[0]];
    else if (n == 2 && (deg[p[0]] - deg[p[1]]) % 180) {
        int x = deg[p[0]], y = deg[p[1]];
        if (x - y > 180) y += 360; else if (y - x > 180) x += 360;
        a = (x + y) / 2 % 360;
    } else return;
    float f = (float)(a > 180 ? a - 360 : a), old;
    uint32_t b, ob = rd32(at);
    memcpy(&b, &f, 4); memcpy(&old, &ob, 4);
    wr32(at, b);
    if (said < 8 && b != ob) {
        said++;
        printf("prompts: the action prompt's ring for button %x turns to %g degrees (the game's: %g), its place in the table\n",
               mask, f, old);
        fflush(stdout);
    }
}

/* ============================================================================
 * 3. Frame rate above 60 fps (Findings 107, 113, 114, 129, 134, 136, 139, 144)
 * ============================================================================ */
int recomps_uncap_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMPS_UNCAP"); v = e && atoi(e); }
    return v;
}
float recomps_uncap_min(void) {
    static float m = -1.0f;
    if (m < 0.0f) { const char *e = getenv("RECOMPS_UNCAP_MIN"); m = e ? (float)atof(e) : 0.0625f; }
    return m;
}
float recomps_uncap_f(uint64_t a) { uint32_t b = rd32(a); float f; memcpy(&f, &b, 4); return f; }

static long long uncap_now_ns(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}
void recomps_uncap_limit(void) {
    static long long next;
    static int fps = -1;
    if (fps < 0) { const char *e = getenv("RECOMPS_UNCAP_FPS"); fps = e ? atoi(e) : 0; }
    if (fps <= 0) return;
    long long per = 1000000000LL / fps, now = uncap_now_ns();
    if (!next || now - next > 4 * per) next = now;
    next += per;
    while (now < next) {
#if defined(__x86_64__)
        __builtin_ia32_pause();
#endif
        now = uncap_now_ns();
    }
}

int recomps_fps_tick = 1;
static uint64_t recomps_fps_n, recomps_fps_g, fps_gtab[256];
static float fps_stab[256];
static uint64_t g_fps_clock_tpidr;

typedef struct { uint64_t key, field, S, U; int64_t now, carry; float f; } FpsRec;
#define FPS_RECS 4096
static FpsRec fps_rec[FPS_RECS];
static uint64_t fps_st[8];
static uint64_t fps_sm[2];
static uint64_t fps_rl[2];

static int fps_smart_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMPS_FPS_SMART"); v = !e || atoi(e); }
    return v;
}

static uint64_t fps_seq_key;
static int fps_sitelog_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMPS_FPS_SITELOG");
        v = e && strtoull(e, 0, 16);
        if (v) fps_seq_key = strtoull(e, 0, 16);
    }
    return v;
}
static struct { uint64_t key, visits, held, de0, again, off, fields, last; int chains; } fps_sl[64];

static void fps_chain(Ctx *c, char *out, int n) {
    int k = snprintf(out, (size_t)n, "%llx", (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 8 && fp && !(fp & 7) && k < n - 20; i++) {
        k += snprintf(out + k, (size_t)(n - k), " %llx", (unsigned long long)rd64(fp + 8));
        fp = rd64(fp);
    }
}

static FpsRec *fps_find(uint64_t key, uint64_t field) {
    uint64_t h = (key * 0x9E3779B97F4A7C15ull) ^ (field * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 29;
    FpsRec *victim = 0;
    for (int i = 0; i < 8; i++) {
        FpsRec *r = &fps_rec[(h + (uint64_t)i) % FPS_RECS];
        uint64_t k = __atomic_load_n(&r->key, __ATOMIC_ACQUIRE);
        if (k == key && r->field == field) return r;
        if (!k) {
            if (__atomic_compare_exchange_n(&r->key, &k, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                r->field = field; r->U = ~0ull; r->now = r->carry = 0;
                return r;
            }
            if (k == key && r->field == field) return r;
            continue;
        }
        if (!victim || r->U < victim->U) victim = r;
    }
    if (!victim) victim = &fps_rec[h % FPS_RECS];
    fps_st[6]++;
    victim->key = key; victim->field = field; victim->U = ~0ull; victim->now = victim->carry = 0;
    return victim;
}

static int64_t fps_visit(Ctx *c, uint64_t key, uint64_t field, FpsRec **rp) {
    uint64_t n = __atomic_load_n(&recomps_fps_n, __ATOMIC_ACQUIRE), g = fps_gtab[n & 255];
    FpsRec *r = fps_find(key, field);
    int held = r->U == n - 1, again = r->U == n;
    int on_clock = c->tpidr == __atomic_load_n(&g_fps_clock_tpidr, __ATOMIC_RELAXED);
    int64_t de;
    if (again) { de = r->now; fps_st[4]++; }
    else {
        if (held) { de = (int64_t)(g - r->S); if (de < 0) de = 0; if (de > 4) de = 4; de += r->carry; }
        else { de = 1; fps_st[3]++; }
        r->carry = 0;
        if (de > 8) { r->carry = de - 8; de = 8; }
        r->now = de;
    }
    r->S = g; r->U = n;
    if (!on_clock) fps_st[5]++;
    if (fps_sitelog_on()) {
        unsigned s = (unsigned)((key >> 2) * 2654435761u) & 63u;
        for (int i = 0; i < 64; i++, s = (s + 1) & 63u)
            if (fps_sl[s].key == key || !fps_sl[s].key) {
                fps_sl[s].key = key; fps_sl[s].visits++; fps_sl[s].held += (uint64_t)held;
                fps_sl[s].de0 += (uint64_t)(de == 0); fps_sl[s].again += (uint64_t)again;
                fps_sl[s].off += (uint64_t)(!on_clock);
                if (fps_sl[s].last != field) { fps_sl[s].fields++; fps_sl[s].last = field; }
                if (!on_clock && fps_sl[s].chains < 3) {
                    char ch[200]; fps_chain(c, ch, (int)sizeof ch); fps_sl[s].chains++;
                    printf("fps_site: %llx off the clock's thread (tpidr %llx, update %llu): return addresses %s\n",
                           (unsigned long long)key, (unsigned long long)c->tpidr, (unsigned long long)n, ch);
                }
                break;
            }
    }
    if (fps_seq_key == key) {
        static int seq;
        if (seq++ < 400)
            printf("fps_seq: %llx update %llu frames due %llu field %llx: %s, credit %lld, tpidr %llx%s\n",
                   (unsigned long long)key, (unsigned long long)n, (unsigned long long)g,
                   (unsigned long long)field, again ? "again in this update" : held ? "ran in the previous update" : "first visit",
                   (long long)de, (unsigned long long)c->tpidr, on_clock ? " (the clock's)" : "");
    }
    *rp = r;
    return de;
}

static void fps_limit(FpsRec *r, int64_t *de, int64_t keep) {
    if (*de > keep) { r->carry += *de - keep; if (r->carry > 8) r->carry = 8; *de = keep; fps_st[7]++; }
}

static void fps_sites_log(long n) {
    printf("uncap: counters: visits credited 0 / 1 / more frames %llu / %llu / %llu, first visits %llu, again in the same update %llu, "
           "off the clock's thread %llu, records given way %llu, held back by a limit %llu\n",
           (unsigned long long)fps_st[0], (unsigned long long)fps_st[1], (unsigned long long)fps_st[2],
           (unsigned long long)fps_st[3], (unsigned long long)fps_st[4], (unsigned long long)fps_st[5],
           (unsigned long long)fps_st[6], (unsigned long long)fps_st[7]);
    memset(fps_st, 0, sizeof fps_st);
    printf("uncap: reloads: %llu float timers kept their overshoot (%llu of them a period or more behind)\n",
           (unsigned long long)fps_rl[0], (unsigned long long)fps_rl[1]);
    memset(fps_rl, 0, sizeof fps_rl);
    printf("uncap: smoothing: %llu visits, %llu of them given the step\n",
           (unsigned long long)fps_sm[0], (unsigned long long)fps_sm[1]);
    memset(fps_sm, 0, sizeof fps_sm);
    if (fps_sitelog_on() && n % 6000 == 0) {
        for (int i = 0; i < 64; i++)
            if (fps_sl[i].visits)
                printf("fps_site: %llx visits %llu, ran in the previous update %llu, credited 0 %llu, again in the same update %llu, off the clock's thread %llu, field changes %llu\n",
                       (unsigned long long)fps_sl[i].key, (unsigned long long)fps_sl[i].visits, (unsigned long long)fps_sl[i].held,
                       (unsigned long long)fps_sl[i].de0, (unsigned long long)fps_sl[i].again, (unsigned long long)fps_sl[i].off,
                       (unsigned long long)fps_sl[i].fields);
        memset(fps_sl, 0, sizeof fps_sl);
    }
}

void recomps_uncap_tick(Ctx *c, float step) {
    static int on = -1;
    static double acc, acc_g, sum, mn = 99, mx;
    static long n, ticks, due;
    if (on < 0) { const char *e = getenv("RECOMPS_FPS_TICKS"); on = !e || atoi(e); }
    __atomic_store_n(&g_fps_clock_tpidr, c->tpidr, __ATOMIC_RELAXED);
    if (!on) { recomps_fps_tick = 1; return; }
    acc += step;
    if (acc >= 0.999) { recomps_fps_tick = 1; acc -= 1.0; if (acc >= 0.999) acc = 0; }
    else recomps_fps_tick = 0;
    acc_g += step;
    int t = (int)(acc_g + 1e-3);
    if (t > 4) { t = 4; acc_g = 4.0; }
    acc_g -= t;
    if (acc_g < 0) acc_g = 0;
    recomps_fps_g += (uint64_t)t;
    fps_gtab[(recomps_fps_n + 1) & 255] = recomps_fps_g;
    fps_stab[(recomps_fps_n + 1) & 255] = step;
    __atomic_store_n(&recomps_fps_n, recomps_fps_n + 1, __ATOMIC_RELEASE);
    {
        static int log = -1;
        if (log < 0) { const char *e = getenv("RECOMPS_UNCAP_LOG"); log = e && atoi(e); }
        if (log) {
            n++; ticks += recomps_fps_tick; due += t; sum += step;
            if (step < mn) mn = step;
            if (step > mx) mx = step;
            if (n % 600 == 0) {
                printf("uncap: 600 clock updates: step mean %.3f min %.3f max %.3f, %ld ticks, %ld frames due\n",
                       sum / 600, mn, mx, ticks, due);
                fps_sites_log(n);
                fflush(stdout);
                sum = 0; mn = 99; mx = 0; ticks = 0; due = 0;
            }
        }
    }
}

uint32_t recomps_fps_ci(Ctx *c, uint64_t key, uint64_t field, uint32_t wb, int k, int sign) {
    if (!recomps_uncap_on() || !recomps_fps_n) return (uint32_t)k;
    if (!fps_smart_on()) return (uint32_t)(k * recomps_fps_tick);
    FpsRec *r;
    int64_t de = fps_visit(c, key, field, &r), w = (int32_t)wb;
    if (sign < 0) {
        if (de == 0 && w == 0) de = 1;
        fps_limit(r, &de, w > k ? 1 + (w - k) / k : 1);
    } else fps_limit(r, &de, 1);
    fps_st[de == 0 ? 0 : de == 1 ? 1 : 2]++;
    return (uint32_t)(k * de);
}

int recomps_fps_cf(Ctx *c, uint64_t key, uint64_t field, float w, int sign) {
    if (!recomps_uncap_on() || !recomps_fps_n) return 1;
    if (!fps_smart_on()) return recomps_fps_tick;
    FpsRec *r;
    int64_t de = fps_visit(c, key, field, &r);
    fps_limit(r, &de, sign < 0 ? (w >= 2.0f ? 1 + (int64_t)(w - 1.0f) : 1) : 1);
    fps_st[de == 0 ? 0 : de == 1 ? 1 : 2]++;
    return (int)de;
}

float recomps_fps_fs(Ctx *c, uint64_t key, uint64_t field) {
    (void)c;
    if (!recomps_uncap_on() || !recomps_fps_n) return 1.0f;
    if (!fps_smart_on()) return (float)recomps_fps_tick;
    uint64_t n = __atomic_load_n(&recomps_fps_n, __ATOMIC_ACQUIRE);
    FpsRec *r = fps_find(key, field);
    float f = r->U == n ? r->f : r->U == n - 1 ? fps_stab[n & 255] : 1.0f;
    if (!(f > 0.0f)) f = 0.0f;
    else if (f > 4.0f) f = 4.0f;
    r->f = f; r->U = n; r->S = fps_gtab[n & 255];
    return f;
}

static struct { int64_t d; float a; } fps_lin[16][4];
static unsigned fps_lin_tid(Ctx *c) { return (unsigned)((c->tpidr >> 6) & 15u); }

float recomps_fps_lmul(Ctx *c, int slot, uint64_t key, uint64_t field, float a) {
    int64_t d = 1;
    float p = a;
    if (recomps_uncap_on() && recomps_fps_n) {
        if (!fps_smart_on()) d = recomps_fps_tick;
        else {
            FpsRec *r;
            d = fps_visit(c, key, field, &r);
            fps_st[d == 0 ? 0 : d == 1 ? 1 : 2]++;
            p = 1.0f;
            for (int64_t i = 0; i < d; i++) p *= a;
        }
    }
    unsigned tid = fps_lin_tid(c);
    fps_lin[tid][slot & 3].d = d;
    fps_lin[tid][slot & 3].a = a;
    return p;
}

float recomps_fps_lsum(Ctx *c, int slot) {
    unsigned tid = fps_lin_tid(c);
    int64_t d = fps_lin[tid][slot & 3].d;
    float a = fps_lin[tid][slot & 3].a, s = 0.0f, p = 1.0f;
    for (int64_t i = 0; i < d; i++) { s += p; p *= a; }
    return s;
}

float recomps_fps_smk(Ctx *c, float k, uint64_t key, uint64_t field) {
    (void)c;
    if (!recomps_uncap_on() || !fps_smart_on() || !recomps_fps_n) return k;
    uint64_t n = __atomic_load_n(&recomps_fps_n, __ATOMIC_ACQUIRE);
    FpsRec *r = fps_find(key, field);
    float f = r->U == n ? r->f : r->U == n - 1 ? fps_stab[n & 255] : 1.0f;
    r->f = f; r->U = n; r->S = fps_gtab[n & 255];
    fps_sm[0]++;
    if (f == 1.0f || !(f > 0.0f)) return k;
    if (k > 0.0f && k < 1.0f) { fps_sm[1]++; return 1.0f - powf(1.0f - k, f); }
    if (k < 0.0f && k > -1.0f) { fps_sm[1]++; return -(1.0f - powf(1.0f + k, f)); }
    return k;
}

void recomps_fps_reload(Ctx *c, int C, int X) {
    if (!recomps_uncap_on() || !fps_smart_on()) return;
    uint32_t b = (uint32_t)c->x[C];
    float p, r = c->v[X].f[0], v;
    memcpy(&p, &b, 4);
    if (!(p > 0.0f && r < 0.0f)) return;
    if (r > -p) v = p + r;
    else { v = p + fmodf(r, p); if (!(v > 0.0f && v <= p)) v = p; fps_rl[1]++; }
    memcpy(&b, &v, 4);
    c->x[C] = b;
    fps_rl[0]++;
}

/* Cinematic shot hold (Finding 129) */
int recomps_cine_fix_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMPS_CINE_FIX"); v = recomps_uncap_on() && (!e || atoi(e)); }
    return v;
}
static struct { uint64_t cur; int32_t shot; float excess, delta; } cine_st[8];
static int cine_slot(uint64_t cur, int add) {
    int s;
    for (s = 0; s < 8 && cine_st[s].cur != cur; s++) {}
    if (s < 8 || !add) return s < 8 ? s : -1;
    for (s = 0; s < 8 && cine_st[s].cur; s++) {}
    if (s == 8) s = 0;
    cine_st[s].cur = cur; cine_st[s].shot = -1; cine_st[s].excess = cine_st[s].delta = 0;
    return s;
}
static double cine_hold(int s, int32_t i, float fc, float fn, float lim) {
    double t = (fc >= lim && cine_st[s].shot == i ? (double)lim + cine_st[s].excess : (double)fc) + ((double)fn - (double)fc);
    cine_st[s].shot = i; cine_st[s].excess = (float)(t - lim); cine_st[s].delta = fn - fc;
    return t;
}
void recomps_cine_shot_end(Ctx *c) {
    uint64_t cur = c->x[20], nxt = c->x[19];
    float fc = recomps_uncap_f(cur + 4), lim = c->v[1].f[0], out;
    int s = cine_slot(cur, 1);
    double t = cine_hold(s, (int32_t)rd32(cur), fc, c->v[0].f[0], lim), k = (double)(int32_t)(uint32_t)c->x[21];
    if (t < (double)lim + k) out = lim;
    else { wr32(nxt, (uint32_t)c->x[23]); out = (float)(t - lim - k); cine_st[s].shot = (int32_t)c->x[23]; cine_st[s].excess = 0; }
    uint32_t b; memcpy(&b, &out, 4); wr32(nxt + 4, b);
}
void recomps_cine_last_hold(Ctx *c) {
    uint64_t cur = c->x[20];
    cine_hold(cine_slot(cur, 1), (int32_t)rd32(cur), recomps_uncap_f(cur + 4), c->v[1].f[0], c->v[0].f[0]);
}
int recomps_cine_end_early(Ctx *c) {
    uint64_t cur = c->x[20];
    int s = cine_slot(cur, 0);
    if (s < 0 || cine_st[s].shot != (int32_t)rd32(cur)) return 0;
    return (double)cine_st[s].excess + cine_st[s].delta < (double)((int32_t)(uint32_t)c->x[11] - (int32_t)(uint32_t)c->x[10]) - 1e-3;
}

/* Slow motion of time channel 0 (Finding 144) */
int recomps_slowmo_fix(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMPS_SLOWMO_FIX"); v = recomps_uncap_on() ? (e ? atoi(e) : 2) : 0; }
    return v;
}
void recomps_slowmo_timer(Ctx *c) {
    float k = (rd32(rd64(0x71018668e0ULL) + 4) >> 5 & 1) ? 2.0f : 1.0f;
    c->v[2].f[0] = c->v[2].f[0] * recomps_uncap_f(rd64(0x7101867708ULL) + 200) / k;
}
int recomps_slowmo_align(Ctx *c) {
    float rem = c->v[0].f[0], s = recomps_uncap_f(c->x[0] + 200);
    if (!(rem > 1e-3f) || rem > 2.0f - 1e-3f || !(s > 0.0f)) return 1;
    vzero(&c->v[0]);
    c->v[0].f[0] = rem / s < 1.0f ? rem / s : 1.0f;
    return 0;
}

/* ============================================================================
 * 4. Wide screens (21:9 / 32:9)
 * ============================================================================ */
unsigned recomps_uw_width(void) {
    static int w = -1;
    if (w < 0) {
        const char *e = getenv("RECOMPS_UW_WIDTH");
        w = e ? atoi(e) : 0;
        if (w <= 1280) {
            const char *ae = getenv("RECOMPS_ASPECT");
            double a = ae ? atof(ae) : 0.0;
            if (a > 16.0 / 9.0 + 0.01 && a <= 8.0) w = (int)(720.0 * a + 0.5) & ~1;
        }
        if (w <= 1280 || w > 8192) w = 0;
    }
    return (unsigned)w;
}
