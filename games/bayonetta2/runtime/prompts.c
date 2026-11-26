/* Built-in PlayStation / Xbox / Keyboard & Mouse button prompts for Bayonetta 2 (v1.2).
 *
 * Identifies the game's unmodified Switch button-prompt textures at upload time by (format, dimensions, data_size,
 * full hash) and patches their button cells in-place on the deswizzled CPU buffer before GPU upload:
 *   - core_sys_btn (256x256 RGBA8, all 5 regional packings covering JP/US/KO/SC/TC, DE, ES, FR, IT):
 *     all menu, HUD, tutorial, combo-list, QTE and action-prompt buttons; non-button icons (kick/punch/gun/jump,
 *     HOLD/AIR, etc.) remain untouched from the game's own texture.
 *   - hud1_main (512x1024 BC3, identical across all languages):
 *     the 4 large HUD action buttons (hud1_actbutton_L, R, ZL, ZR), patching only their 4x4 BC3 blocks.
 *   - tutorial_btn (128x64 RGBA8, identical across all languages):
 *     left-stick up/down tutorial icons (btn_029, btn_030) for keyboard bindings.
 *
 * Switching controller, keyboard/mouse (button_prompts = auto), PC Settings options (Button prompts / Face buttons),
 * or editing controllers.ini updates the prompt state key and re-uploads the atlases live on the next frame, with no
 * restart and no modification of guest memory. External RomFS prompt mods have different texture hashes and are left
 * untouched. Also keeps Bayonetta 2's action-prompt ring button table (0x7101464340, Finding 148) and menu
 * confirm/cancel swap state (0x71034fdf54, 0x71018e4810/14, 0x7101537d88) in sync live.
 *
 * Optional debug env vars:
 *   MR_PROMPTS=switch|playstation|xbox|keyboard   override controllers.ini button_prompts
 *   MR_PROMPTS_LOG=1                              log atlas detection and live prompt switches */
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef PROMPTS_NO_WIN
#include <windows.h>
extern __declspec(dllimport) volatile uint8_t game_rebind[16];
extern __declspec(dllimport) volatile int game_rebind_on;
extern __declspec(dllimport) volatile int game_control_type;
#endif
#include "prompts.h"
#include "prompt_art.h"
#include "nnpad.h"
#include "rebind.h"
#include "tex_decode.h"
#include "tex_pack.h"

/* ================================================================ Bayonetta 2 atlas tables */
enum {
    ATLAS_CORE_US = 1,          /* JP / US / KO / SC / TC */
    ATLAS_CORE_DE = 2,
    ATLAS_CORE_ES = 3,
    ATLAS_CORE_FR = 4,
    ATLAS_CORE_IT = 5,
    ATLAS_HUD1    = 6,          /* hud1_main (512x1024 BC3, all languages) */
    ATLAS_TUT1    = 7           /* tutorial_btn (128x64 RGBA8, all languages) */
};

/* Slot order in core_sys_btn (matches btn_001..btn_028 minus touch icons btn_000 / btn_026). */
enum {
    SL_B = 0, SL_A, SL_Y, SL_X,
    SL_R, SL_ZR, SL_L, SL_ZL,
    SL_DPAD, SL_DPAD_UD, SL_DPAD_LR, SL_DPAD_U, SL_DPAD_D, SL_DPAD_L, SL_DPAD_R,
    SL_RS, SL_R3, SL_LS, SL_L3,
    SL_PLUS, SL_MINUS,
    SL_RS_ROT, SL_LS_ROT, SL_LS_UD, SL_LS_LR,
    SL_COUNT
};

static const uint8_t k_core_wh[SL_COUNT][2] = {
    {32, 32}, {32, 32}, {32, 32}, {32, 32},     /* B, A, Y, X */
    {53, 27}, {37, 37}, {53, 27}, {37, 37},     /* R, ZR, L, ZL */
    {32, 32}, {32, 32}, {32, 32}, {32, 32}, {32, 32}, {32, 32}, {32, 32}, /* D-pad */
    {34, 32}, {32, 38}, {34, 32}, {32, 38},     /* RS, R3, LS, L3 */
    {28, 28}, {28, 28},                         /* PLUS, MINUS */
    {38, 38}, {38, 38}, {52, 38}, {56, 38},     /* RS_ROT, LS_ROT, LS_UD, LS_LR */
};

typedef struct {
    int id;
    const char *tag;
    uint64_t hash;
    uint16_t xy[SL_COUNT][2];
} CoreLayout;

static const CoreLayout k_core_layouts[5] = {
    { ATLAS_CORE_US, "core_sys_btn (US/JP/KO/SC/TC)", 0x61613b22195b641dULL, {
        { 96, 168}, { 96, 208}, {148,   0}, {144,  40},
        {  0,  84}, {  0, 192}, { 56,   0}, { 56, 208},
        {132,  84}, {160, 168}, {176,  40}, {180,   0}, {128, 208}, {128, 168}, {128, 128},
        {108,  40}, { 96, 128}, {112,   0}, {100,  84},
        {192, 208}, {228,  84},
        { 56, 168}, {  0, 152}, { 56,  40}, {  0,   0},
    }},
    { ATLAS_CORE_DE, "core_sys_btn (DE)", 0x1b6305dce9337e49ULL, {
        {108, 188}, {160,   0}, {124,  72}, {160,  40},
        { 68,  40}, {  0, 196}, {  0,  84}, { 68, 188},
        {140, 108}, {140, 148}, {192,  40}, {156,  72}, {192,   0}, {140, 188}, {108, 220},
        {124,   0}, {108, 108}, {124,  40}, {108, 148},
        { 40, 156}, { 40, 196},
        {  0, 156}, { 68, 148}, { 68,  68}, { 68,   0},
    }},
    { ATLAS_CORE_ES, "core_sys_btn (ES)", 0xb4aa11567a9e9bf8ULL, {
        { 96, 160}, { 96, 200}, {148,   0}, {148,  40},
        { 56,   0}, {  0, 208}, { 56,  40}, { 56, 200},
        {140,  80}, {128, 120}, {172,  80}, {180,  40}, {180,   0}, {128, 200}, {128, 160},
        {112,   0}, {108,  80}, {112,  40}, { 96, 120},
        {192, 200}, {224, 120},
        {  0, 168}, { 56, 160}, { 56,  80}, {  0,  40},
    }},
    { ATLAS_CORE_FR, "core_sys_btn (FR)", 0x8dcc4f50209d727aULL, {
        { 96, 168}, { 96, 208}, {148,   0}, {144,  40},
        {  0,  84}, {  0, 192}, { 56,   0}, { 56, 208},
        {164,  84}, {176,  40}, {180,   0}, {128, 208}, {128, 168}, {128, 128}, {132,  84},
        {108,  40}, {100,  84}, {112,   0}, { 96, 128},
        {192, 208}, {228,  84},
        { 56, 168}, {  0, 152}, { 56,  40}, {  0,   0},
    }},
    { ATLAS_CORE_IT, "core_sys_btn (IT)", 0x36a27ef923e7c1dfULL, {
        { 96, 168}, { 96, 208}, {148,   0}, {144,  40},
        {  0,  84}, {  0, 192}, { 56,   0}, { 56, 208},
        {132,  84}, {128, 128}, {164,  84}, {176,  40}, {180,   0}, {128, 208}, {128, 168},
        {112,   0}, {100,  84}, {108,  40}, { 96, 128},
        {192, 208}, {228,  84},
        {  0, 152}, { 56, 168}, { 56,  40}, {  0,   0},
    }},
};

#define HASH_HUD1_MAIN    0x0c3d124be27915f0ULL
#define HASH_TUTORIAL_BTN 0x5a3a921f4d3d7e80ULL

/* ================================================================ 4x4 supersampled SDF drawing */
typedef struct { uint8_t r, g, b; } Col;

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float sd_circle(float x, float y, float cx, float cy, float r) {
    return sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r;
}
static inline float sd_rbox(float x, float y, float x0, float y0, float x1, float y1, float r) {
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    float hx = (x1 - x0) * 0.5f - r, hy = (y1 - y0) * 0.5f - r;
    if (hx < 0) hx = 0;
    if (hy < 0) hy = 0;
    float dx = fabsf(x - cx) - hx, dy = fabsf(y - cy) - hy;
    float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
    float in = dx > dy ? dx : dy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}
/* Trigger silhouette: rounded box with a stronger curve on the outer shoulder (left_curve=1 for ZL/L2/LT, 0 for ZR/R2/RT). */
static inline float sd_trigger(float x, float y, float x0, float y0, float x1, float y1, int left_curve) {
    float w = x1 - x0, h = y1 - y0;
    float big_r = (w < h ? w : h) * 0.40f;
    float small_r = (w < h ? w : h) * 0.14f;
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    float px = x - cx, py = y - cy;
    float r = (py < 0 && ((left_curve && px < 0) || (!left_curve && px > 0))) ? big_r : small_r;
    float hx = w * 0.5f - r, hy = h * 0.5f - r;
    if (hx < 0) hx = 0;
    if (hy < 0) hy = 0;
    float dx = fabsf(px) - hx, dy = fabsf(py) - hy;
    float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
    float in = dx > dy ? dx : dy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}
static inline float sd_seg(float x, float y, float x0, float y0, float x1, float y1) {
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? clampf(((x - x0) * dx + (y - y0) * dy) / l2, 0.0f, 1.0f) : 0.0f;
    float px = x - (x0 + t * dx), py = y - (y0 + t * dy);
    return sqrtf(px * px + py * py);
}

/* Blend RGBA (straight alpha, 0..255) into a cell pixel buffer (cw x ch RGBA8). */
static void clear_cell(uint8_t *dst, int pitch, int x0, int y0, int cw, int ch) {
    for (int j = 0; j < ch; j++) memset(dst + (size_t)(y0 + j) * pitch + (size_t)x0 * 4, 0, (size_t)cw * 4);
}
static void over_px(uint8_t *p, float r, float g, float b, float a) {
    a = clampf(a, 0.0f, 1.0f);
    if (a <= 0.0f) return;
    float da = p[3] / 255.0f;
    float oa = a + da * (1.0f - a);
    if (oa <= 1e-5f) { p[0] = p[1] = p[2] = p[3] = 0; return; }
    float k = da * (1.0f - a);
    p[0] = (uint8_t)clampf((r * a + p[0] * k) / oa + 0.5f, 0.0f, 255.0f);
    p[1] = (uint8_t)clampf((g * a + p[1] * k) / oa + 0.5f, 0.0f, 255.0f);
    p[2] = (uint8_t)clampf((b * a + p[2] * k) / oa + 0.5f, 0.0f, 255.0f);
    p[3] = (uint8_t)clampf(oa * 255.0f + 0.5f, 0.0f, 255.0f);
}

/* Vector stroke distance for a string of uppercase ASCII letters/digits (reusing prompt_art.h's letter() / hex()). */
static float sd_text_box(float x, float y, float cx, float cy, const char *t, float k, float gap) {
    int n = (int)strlen(t);
    if (n <= 0) return 1e9f;
    float tw = n * 4.0f * k + (n - 1) * gap;
    float th = 6.0f * k;
    float ox0 = cx - tw * 0.5f;
    float oy0 = cy - th * 0.5f;
    float best = 1e9f;
    for (int i = 0; i < n; i++) {
        float ox = ox0 + i * (4.0f * k + gap);
        char c = t[i];
        if (c == '-') {
            float d = sd_seg(x, y, ox + 0.5f * k, oy0 + 3.0f * k, ox + 3.5f * k, oy0 + 3.0f * k);
            if (d < best) best = d;
            continue;
        }
        if (c == '+') {
            float d1 = sd_seg(x, y, ox + 0.5f * k, oy0 + 3.0f * k, ox + 3.5f * k, oy0 + 3.0f * k);
            float d2 = sd_seg(x, y, ox + 2.0f * k, oy0 + 1.2f * k, ox + 2.0f * k, oy0 + 4.8f * k);
            if (d1 < best) best = d1;
            if (d2 < best) best = d2;
            continue;
        }
        for (const char *p = letter(c); p[0] && p[2] && p[3]; p += 3) {
            if (p[2] == ' ') {
                float d = sd_seg(x, y, ox + hex(p[0]) * k * 0.5f, oy0 + hex(p[1]) * k * 0.5f,
                                       ox + hex(p[3]) * k * 0.5f, oy0 + hex(p[4]) * k * 0.5f);
                if (d < best) best = d;
            }
        }
    }
    return best;
}

/* Paint a string inside a cell with 4x4 supersampling and optional 1px subtle drop shadow for legibility. */
static void paint_text(uint8_t *dst, int pitch, int x0, int y0, int cw, int ch,
                       float cx, float cy, float max_w, float max_h, const char *t, Col col, int shadow) {
    int n = (int)strlen(t);
    if (n <= 0) return;
    float gap_ratio = n <= 1 ? 0.0f : n == 2 ? 0.38f : 0.28f;
    float k_w = max_w / (n * 4.0f + (n - 1) * 4.0f * gap_ratio);
    float k_h = max_h / 6.0f;
    float k = k_w < k_h ? k_w : k_h;
    float gap = k * 4.0f * gap_ratio;
    float half_w = clampf(k * 0.52f, 0.72f, 1.85f);
    for (int j = 0; j < ch; j++) {
        for (int i = 0; i < cw; i++) {
            float cov = 0.0f, scov = 0.0f;
            for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
                float px = i + (sx + 0.5f) * 0.25f;
                float py = j + (sy + 0.5f) * 0.25f;
                if (sd_text_box(px, py, cx, cy, t, k, gap) <= half_w) cov += 1.0f;
                if (shadow && sd_text_box(px, py - 0.85f, cx, cy, t, k, gap) <= half_w + 0.35f) scov += 1.0f;
            }
            uint8_t *p = dst + (size_t)(y0 + j) * pitch + (size_t)(x0 + i) * 4;
            if (shadow && scov > 0.0f) over_px(p, 12.0f, 14.0f, 18.0f, (scov / 16.0f) * 0.65f);
            if (cov > 0.0f) over_px(p, col.r, col.g, col.b, cov / 16.0f);
        }
    }
}

/* Shaded metallic silver body (matching Bayonetta 2's silver bevelled Switch buttons) or dark obsidian body. */
static void shade_body(uint8_t *dst, int pitch, int x0, int y0, int cw, int ch,
                       float (*sdf)(float, float, void *), void *ud, int dark, float top_y, float bot_y) {
    float span = bot_y > top_y ? bot_y - top_y : 1.0f;
    for (int j = 0; j < ch; j++) {
        for (int i = 0; i < cw; i++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
                float px = i + (sx + 0.5f) * 0.25f;
                float py = j + (sy + 0.5f) * 0.25f;
                float d = sdf(px, py, ud);
                if (d > 1.2f) continue;
                float t = clampf((py - top_y) / span, 0.0f, 1.0f);
                float sr, sg, sb, sa;
                if (d > 0.0f) {
                    /* crisp dark outer edge / shadow */
                    sa = clampf(1.0f - d / 1.15f, 0.0f, 1.0f) * 0.85f;
                    sr = 18.0f; sg = 20.0f; sb = 26.0f;
                } else if (d > -1.35f) {
                    /* outer metallic rim bevel */
                    sa = 1.0f;
                    if (dark) {
                        float rim = 168.0f - 52.0f * t;
                        sr = rim * 0.94f; sg = rim * 0.98f; sb = rim * 1.04f;
                    } else {
                        float rim = 98.0f - 36.0f * t;
                        sr = rim * 0.95f; sg = rim * 0.99f; sb = rim * 1.05f;
                    }
                } else if (d > -2.5f) {
                    /* inner specular bevel ring */
                    sa = 1.0f;
                    if (dark) {
                        float v = 64.0f - 24.0f * t;
                        sr = v * 0.92f; sg = v * 0.97f; sb = v * 1.08f;
                    } else {
                        float v = 248.0f - 76.0f * t;
                        sr = v * 0.97f; sg = v * 0.99f; sb = clampf(v * 1.02f, 0.0f, 255.0f);
                    }
                } else {
                    /* main dome / plate face */
                    sa = 1.0f;
                    if (dark) {
                        float v = 54.0f - 24.0f * t + (t < 0.35f ? (0.35f - t) * 34.0f : 0.0f);
                        sr = v * 0.92f; sg = v * 0.97f; sb = v * 1.12f;
                    } else {
                        float v = 250.0f - 58.0f * t + (t < 0.28f ? (0.28f - t) * 45.0f : 0.0f);
                        sr = clampf(v * 0.97f, 0.0f, 255.0f);
                        sg = clampf(v * 0.99f, 0.0f, 255.0f);
                        sb = clampf(v * 1.02f, 0.0f, 255.0f);
                    }
                }
                r += sr * sa; g += sg * sa; b += sb * sa; a += sa;
            }
            if (a > 0.0f) {
                uint8_t *p = dst + (size_t)(y0 + j) * pitch + (size_t)(x0 + i) * 4;
                over_px(p, r / a, g / a, b / a, a / 16.0f);
            }
        }
    }
}

typedef struct { float a, b, c, d, r; int flag; } SdfArgs;
static float fn_disc(float x, float y, void *u) { SdfArgs *p = u; return sd_circle(x, y, p->a, p->b, p->r); }
static float fn_rbox(float x, float y, void *u) { SdfArgs *p = u; return sd_rbox(x, y, p->a, p->b, p->c, p->d, p->r); }
static float fn_trig(float x, float y, void *u) { SdfArgs *p = u; return sd_trigger(x, y, p->a, p->b, p->c, p->d, p->flag); }

/* ---- PlayStation colored face symbols (0=Cross, 1=Circle, 2=Square, 3=Triangle) */
static void draw_ps_face(uint8_t *dst, int pitch, int x0, int y0, int cw, int ch, int sym) {
    static const Col cols[4] = {
        { 32,  92, 220},    /* Cross: rich PlayStation blue */
        {218,  42,  48},    /* Circle: rich PlayStation red */
        {196,  48, 152},    /* Square: rich PlayStation magenta-pink */
        { 18, 148, 108},    /* Triangle: rich PlayStation emerald */
    };
    clear_cell(dst, pitch, x0, y0, cw, ch);
    float cx = cw * 0.5f, cy = ch * 0.5f;
    float rad = (cw < ch ? cw : ch) * 0.5f - 1.8f;
    SdfArgs a = { cx, cy, 0, 0, rad, 0 };
    shade_body(dst, pitch, x0, y0, cw, ch, fn_disc, &a, 0, cy - rad, cy + rad);

    Col col = cols[sym & 3];
    float s = rad * 0.50f;
    float hw = clampf(rad * 0.12f, 1.05f, 2.2f);
    for (int j = 0; j < ch; j++) {
        for (int i = 0; i < cw; i++) {
            float cov = 0.0f;
            for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
                float px = i + (sx + 0.5f) * 0.25f;
                float py = j + (sy + 0.5f) * 0.25f;
                float d = 1e9f;
                if (sym == 0) { /* Cross */
                    float d1 = sd_seg(px, py, cx - s * 0.92f, cy - s * 0.92f, cx + s * 0.92f, cy + s * 0.92f);
                    float d2 = sd_seg(px, py, cx + s * 0.92f, cy - s * 0.92f, cx - s * 0.92f, cy + s * 0.92f);
                    d = d1 < d2 ? d1 : d2;
                } else if (sym == 1) { /* Circle */
                    d = fabsf(sd_circle(px, py, cx, cy, s * 1.02f));
                } else if (sym == 2) { /* Square */
                    float e = s * 0.90f;
                    float d1 = sd_seg(px, py, cx - e, cy - e, cx + e, cy - e);
                    float d2 = sd_seg(px, py, cx + e, cy - e, cx + e, cy + e);
                    float d3 = sd_seg(px, py, cx + e, cy + e, cx - e, cy + e);
                    float d4 = sd_seg(px, py, cx - e, cy + e, cx - e, cy - e);
                    d = d1 < d2 ? d1 : d2;
                    if (d3 < d) d = d3;
                    if (d4 < d) d = d4;
                } else { /* Triangle */
                    float ty = cy - s * 1.05f, by = cy + s * 0.85f, bx = s * 1.05f;
                    float d1 = sd_seg(px, py, cx, ty, cx + bx, by);
                    float d2 = sd_seg(px, py, cx + bx, by, cx - bx, by);
                    float d3 = sd_seg(px, py, cx - bx, by, cx, ty);
                    d = d1 < d2 ? d1 : d2;
                    if (d3 < d) d = d3;
                }
                if (d <= hw) cov += 1.0f;
            }
            if (cov > 0.0f) {
                uint8_t *p = dst + (size_t)(y0 + j) * pitch + (size_t)(x0 + i) * 4;
                over_px(p, col.r, col.g, col.b, cov / 16.0f);
            }
        }
    }
}

/* ---- Xbox colored face letters (0=A green, 1=B red, 2=X blue, 3=Y gold) */
static void draw_xbox_face(uint8_t *dst, int pitch, int x0, int y0, int cw, int ch, int idx) {
    static const char *const lbl[4] = { "A", "B", "X", "Y" };
    static const Col cols[4] = {
        { 28, 148,  36},    /* A: rich Xbox green */
        {212,  38,  40},    /* B: rich Xbox red */
        { 28, 104, 224},    /* X: rich Xbox blue */
        {198, 134,  10},    /* Y: rich Xbox amber-gold */
    };
    clear_cell(dst, pitch, x0, y0, cw, ch);
    float cx = cw * 0.5f, cy = ch * 0.5f;
    float rad = (cw < ch ? cw : ch) * 0.5f - 1.8f;
    SdfArgs a = { cx, cy, 0, 0, rad, 0 };
    shade_body(dst, pitch, x0, y0, cw, ch, fn_disc, &a, 0, cy - rad, cy + rad);
    paint_text(dst, pitch, x0, y0, cw, ch, cx, cy, rad * 1.05f, rad * 1.15f, lbl[idx & 3], cols[idx & 3], 0);
}

/* ---- System disc icons: kind 0 = Options/Menu (3 horizontal bars), 1 = Create/Share (3 fanned rays), 2 = View (2 windows) */
static void draw_sys_disc(uint8_t *dst, int pitch, int x0, int y0, int cw, int ch, int kind) {
    clear_cell(dst, pitch, x0, y0, cw, ch);
    float cx = cw * 0.5f, cy = ch * 0.5f;
    float rad = (cw < ch ? cw : ch) * 0.5f - 1.6f;
    SdfArgs a = { cx, cy, 0, 0, rad, 0 };
    shade_body(dst, pitch, x0, y0, cw, ch, fn_disc, &a, 0, cy - rad, cy + rad);

    float hw = clampf(rad * 0.095f, 0.85f, 1.5f);
    for (int j = 0; j < ch; j++) {
        for (int i = 0; i < cw; i++) {
            float cov = 0.0f;
            for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
                float px = i + (sx + 0.5f) * 0.25f;
                float py = j + (sy + 0.5f) * 0.25f;
                int hit = 0;
                if (kind == 0) { /* 3 horizontal bars */
                    float sp = rad * 0.34f, len = rad * 0.44f;
                    for (int k = -1; k <= 1; k++)
                        if (sd_seg(px, py, cx - len, cy + k * sp, cx + len, cy + k * sp) <= hw) hit = 1;
                } else if (kind == 1) { /* PlayStation Create / Share: 3 fanned rays */
                    float len = rad * 0.44f, by = cy + len * 0.85f, ty = cy - len * 0.90f;
                    if (sd_seg(px, py, cx, by, cx, ty) <= hw) hit = 1;
                    if (sd_seg(px, py, cx - rad * 0.22f, by, cx - rad * 0.46f, ty + rad * 0.14f) <= hw) hit = 1;
                    if (sd_seg(px, py, cx + rad * 0.22f, by, cx + rad * 0.46f, ty + rad * 0.14f) <= hw) hit = 1;
                } else { /* Xbox View: two overlapping rectangular frames */
                    float d_back = fabsf(sd_rbox(px, py, cx - rad * 0.48f, cy - rad * 0.44f, cx + rad * 0.16f, cy + rad * 0.14f, 0.8f));
                    float d_front_in = sd_rbox(px, py, cx - rad * 0.14f, cy - rad * 0.12f, cx + rad * 0.48f, cy + rad * 0.44f, 0.8f);
                    if (fabsf(d_front_in) <= hw) hit = 1;
                    else if (d_back <= hw && d_front_in > 0.4f) hit = 1;
                }
                if (hit) cov += 1.0f;
            }
            if (cov > 0.0f) {
                uint8_t *p = dst + (size_t)(y0 + j) * pitch + (size_t)(x0 + i) * 4;
                over_px(p, 34.0f, 38.0f, 46.0f, cov / 16.0f);
            }
        }
    }
}

/* ---- Metallic silver bumper (L1 / R1 / LB / RB) */
static void draw_bumper(uint8_t *dst, int pitch, int x0, int y0, int cw, int ch, const char *label) {
    clear_cell(dst, pitch, x0, y0, cw, ch);
    float px0 = 1.6f, py0 = 2.0f, px1 = cw - 1.6f, py1 = ch - 2.0f;
    float r = clampf((py1 - py0) * 0.24f, 3.0f, 7.0f);
    SdfArgs a = { px0, py0, px1, py1, r, 0 };
    shade_body(dst, pitch, x0, y0, cw, ch, fn_rbox, &a, 0, py0, py1);
    Col ink = { 34, 38, 46 };
    paint_text(dst, pitch, x0, y0, cw, ch, cw * 0.5f, ch * 0.5f, (px1 - px0) * 0.58f, (py1 - py0) * 0.56f, label, ink, 0);
}

/* ---- Metallic silver trigger (L2 / R2 / LT / RT) */
static void draw_trigger(uint8_t *dst, int pitch, int x0, int y0, int cw, int ch, const char *label, int left_curve) {
    clear_cell(dst, pitch, x0, y0, cw, ch);
    float px0 = 1.6f, py0 = ch * 0.10f, px1 = cw - 1.6f, py1 = ch * 0.90f;
    SdfArgs a = { px0, py0, px1, py1, 0, left_curve };
    shade_body(dst, pitch, x0, y0, cw, ch, fn_trig, &a, 0, py0, py1);
    Col ink = { 34, 38, 46 };
    paint_text(dst, pitch, x0, y0, cw, ch, cw * 0.5f, (py0 + py1) * 0.52f, (px1 - px0) * 0.64f, (py1 - py0) * 0.48f, label, ink, 0);
}

/* ---- Stick click (L3 / R3 / LS / RS) with click-down chevron arrow at top */
static void draw_stick_click(uint8_t *dst, int pitch, int x0, int y0, int cw, int ch, const char *label) {
    clear_cell(dst, pitch, x0, y0, cw, ch);
    float cx = cw * 0.5f;
    float rad = (cw - 3.6f) * 0.5f;
    float cy = ch - rad - 2.2f;
    /* subtle stick stem beneath dome */
    SdfArgs stem = { cx - rad * 0.42f, cy + rad * 0.55f, cx + rad * 0.42f, ch - 1.2f, 2.0f, 0 };
    shade_body(dst, pitch, x0, y0, cw, ch, fn_rbox, &stem, 1, cy, ch - 1.0f);
    /* metallic dome */
    SdfArgs dome = { cx, cy, 0, 0, rad, 0 };
    shade_body(dst, pitch, x0, y0, cw, ch, fn_disc, &dome, 0, cy - rad, cy + rad);
    Col ink = { 34, 38, 46 };
    paint_text(dst, pitch, x0, y0, cw, ch, cx, cy, rad * 1.22f, rad * 0.92f, label, ink, 0);

    /* downward click triangle at top (matching Bayonetta 2's btn_019 / btn_017) */
    float ay0 = 1.8f, ay1 = cy - rad + 2.5f;
    if (ay1 < ay0 + 4.5f) ay1 = ay0 + 4.5f;
    float aw = (ay1 - ay0) * 1.05f;
    for (int j = 0; j < ch; j++) {
        for (int i = 0; i < cw; i++) {
            float fill = 0.0f, edge = 0.0f;
            for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
                float px = i + (sx + 0.5f) * 0.25f;
                float py = j + (sy + 0.5f) * 0.25f;
                float t = (py - ay0) / (ay1 - ay0);
                if (t >= 0.0f && t <= 1.0f && fabsf(px - cx) <= aw * (1.0f - t)) fill += 1.0f;
                float d1 = sd_seg(px, py, cx - aw, ay0, cx + aw, ay0);
                float d2 = sd_seg(px, py, cx + aw, ay0, cx, ay1);
                float d3 = sd_seg(px, py, cx, ay1, cx - aw, ay0);
                float d = d1 < d2 ? d1 : d2;
                if (d3 < d) d = d3;
                if (d <= 0.95f) edge += 1.0f;
            }
            uint8_t *p = dst + (size_t)(y0 + j) * pitch + (size_t)(x0 + i) * 4;
            if (fill > 0.0f) over_px(p, 240.0f, 244.0f, 250.0f, fill / 16.0f);
            if (edge > 0.0f) over_px(p, 28.0f, 32.0f, 38.0f, edge / 16.0f);
        }
    }
}

/* ---- Keyboard keycap (in any sub-rectangle of the cell) */
static void draw_keycap_rect(uint8_t *dst, int pitch, int x0, int y0, int cw, int ch,
                             float bx0, float by0, float bx1, float by1, const char *label) {
    float r = clampf(((bx1 - bx0) < (by1 - by0) ? (bx1 - bx0) : (by1 - by0)) * 0.18f, 2.2f, 5.5f);
    SdfArgs a = { bx0, by0, bx1, by1, r, 0 };
    shade_body(dst, pitch, x0, y0, cw, ch, fn_rbox, &a, 0, by0, by1);
    Col fg = { 34, 38, 46 };
    paint_text(dst, pitch, x0, y0, cw, ch, (bx0 + bx1) * 0.5f, (by0 + by1) * 0.5f,
               (bx1 - bx0) * 0.72f, (by1 - by0) * 0.58f, label, fg, 0);
}

/* ---- Directional arrow glyph inside a keycap (dir: 0=L, 1=U, 2=R, 3=D, 4=UD, 5=LR, 6=4-way) */
static void draw_arrow_key(uint8_t *dst, int pitch, int x0, int y0, int cw, int ch, int dir) {
    clear_cell(dst, pitch, x0, y0, cw, ch);
    float bx0 = 2.0f, by0 = 2.0f, bx1 = cw - 2.0f, by1 = ch - 2.0f;
    SdfArgs a = { bx0, by0, bx1, by1, 4.0f, 0 };
    shade_body(dst, pitch, x0, y0, cw, ch, fn_rbox, &a, 0, by0, by1);

    float cx = cw * 0.5f, cy = ch * 0.5f;
    float s = ((bx1 - bx0) < (by1 - by0) ? (bx1 - bx0) : (by1 - by0)) * 0.25f;
    float hw = clampf(s * 0.19f, 0.9f, 1.6f);
    for (int j = 0; j < ch; j++) {
        for (int i = 0; i < cw; i++) {
            float cov = 0.0f;
            for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
                float px = i + (sx + 0.5f) * 0.25f;
                float py = j + (sy + 0.5f) * 0.25f;
                float d = 1e9f;
                if (dir == 0) { /* Left */
                    float d1 = sd_seg(px, py, cx - s, cy, cx + s, cy);
                    float d2 = sd_seg(px, py, cx - s, cy, cx - s * 0.1f, cy - s * 0.8f);
                    float d3 = sd_seg(px, py, cx - s, cy, cx - s * 0.1f, cy + s * 0.8f);
                    d = d1 < d2 ? d1 : d2; if (d3 < d) d = d3;
                } else if (dir == 1) { /* Up */
                    float d1 = sd_seg(px, py, cx, cy - s, cx, cy + s);
                    float d2 = sd_seg(px, py, cx, cy - s, cx - s * 0.8f, cy - s * 0.1f);
                    float d3 = sd_seg(px, py, cx, cy - s, cx + s * 0.8f, cy - s * 0.1f);
                    d = d1 < d2 ? d1 : d2; if (d3 < d) d = d3;
                } else if (dir == 2) { /* Right */
                    float d1 = sd_seg(px, py, cx - s, cy, cx + s, cy);
                    float d2 = sd_seg(px, py, cx + s, cy, cx + s * 0.1f, cy - s * 0.8f);
                    float d3 = sd_seg(px, py, cx + s, cy, cx + s * 0.1f, cy + s * 0.8f);
                    d = d1 < d2 ? d1 : d2; if (d3 < d) d = d3;
                } else if (dir == 3) { /* Down */
                    float d1 = sd_seg(px, py, cx, cy - s, cx, cy + s);
                    float d2 = sd_seg(px, py, cx, cy + s, cx - s * 0.8f, cy + s * 0.1f);
                    float d3 = sd_seg(px, py, cx, cy + s, cx + s * 0.8f, cy + s * 0.1f);
                    d = d1 < d2 ? d1 : d2; if (d3 < d) d = d3;
                } else if (dir == 4) { /* Up/Down */
                    float d1 = sd_seg(px, py, cx, cy - s * 1.05f, cx - s * 0.7f, cy - s * 0.25f);
                    float d2 = sd_seg(px, py, cx, cy - s * 1.05f, cx + s * 0.7f, cy - s * 0.25f);
                    float d3 = sd_seg(px, py, cx, cy + s * 1.05f, cx - s * 0.7f, cy + s * 0.25f);
                    float d4 = sd_seg(px, py, cx, cy + s * 1.05f, cx + s * 0.7f, cy + s * 0.25f);
                    d = d1 < d2 ? d1 : d2; if (d3 < d) d = d3; if (d4 < d) d = d4;
                } else if (dir == 5) { /* Left/Right */
                    float d1 = sd_seg(px, py, cx - s * 1.05f, cy, cx - s * 0.25f, cy - s * 0.7f);
                    float d2 = sd_seg(px, py, cx - s * 1.05f, cy, cx - s * 0.25f, cy + s * 0.7f);
                    float d3 = sd_seg(px, py, cx + s * 1.05f, cy, cx + s * 0.25f, cy - s * 0.7f);
                    float d4 = sd_seg(px, py, cx + s * 1.05f, cy, cx + s * 0.25f, cy + s * 0.7f);
                    d = d1 < d2 ? d1 : d2; if (d3 < d) d = d3; if (d4 < d) d = d4;
                } else { /* 4-way arrows (cross stems + arrowheads) */
                    float o = s * 1.15f, h = s * 0.38f, m = s * 0.72f;
                    float segs[10][4] = {
                        {cx, cy - o, cx, cy + o}, {cx - o, cy, cx + o, cy},
                        {cx, cy - o, cx - h, cy - m}, {cx, cy - o, cx + h, cy - m},
                        {cx, cy + o, cx - h, cy + m}, {cx, cy + o, cx + h, cy + m},
                        {cx - o, cy, cx - m, cy - h}, {cx - o, cy, cx - m, cy + h},
                        {cx + o, cy, cx + m, cy - h}, {cx + o, cy, cx + m, cy + h},
                    };
                    for (int k = 0; k < 10; k++) {
                        float dk = sd_seg(px, py, segs[k][0], segs[k][1], segs[k][2], segs[k][3]);
                        if (dk < d) d = dk;
                    }
                }
                if (d <= hw) cov += 1.0f;
            }
            if (cov > 0.0f) {
                uint8_t *p = dst + (size_t)(y0 + j) * pitch + (size_t)(x0 + i) * 4;
                over_px(p, 34.0f, 38.0f, 46.0f, cov / 16.0f);
            }
        }
    }
}

/* ---- Mouse icon: button 0 = move (camera), 1 = LMB, 2 = RMB, 3 = MMB/wheel, 4 = M4, 5 = M5 */
static void draw_mouse(uint8_t *dst, int pitch, int x0, int y0, int cw, int ch, int button) {
    clear_cell(dst, pitch, x0, y0, cw, ch);
    float cx = cw * 0.5f, cy = ch * 0.5f;
    float mh = (ch - 3.6f);
    float mw = mh * 0.66f;
    if (mw > cw - 4.0f) { mw = cw - 4.0f; mh = mw / 0.66f; }
    float mx0 = cx - mw * 0.5f, mx1 = cx + mw * 0.5f;
    float my0 = cy - mh * 0.5f, my1 = cy + mh * 0.5f;
    float mr = mw * 0.48f;
    SdfArgs body = { mx0, my0, mx1, my1, mr, 0 };
    shade_body(dst, pitch, x0, y0, cw, ch, fn_rbox, &body, 0, my0, my1);

    float split_y = my0 + mh * 0.44f;
    Col hi = { 28, 128, 228 };      /* crisp blue highlight for active mouse button on silver body */
    for (int j = 0; j < ch; j++) {
        for (int i = 0; i < cw; i++) {
            float cov_hi = 0.0f, cov_line = 0.0f, cov_whl = 0.0f;
            for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
                float px = i + (sx + 0.5f) * 0.25f;
                float py = j + (sy + 0.5f) * 0.25f;
                float d_in = sd_rbox(px, py, mx0 + 1.9f, my0 + 1.9f, mx1 - 1.9f, my1 - 1.9f, mr - 1.9f > 1.0f ? mr - 1.9f : 1.0f);
                if (d_in <= 0.0f) {
                    if (button == 1 && py < split_y - 0.6f && px < cx - 0.6f) cov_hi += 1.0f;
                    else if (button == 2 && py < split_y - 0.6f && px > cx + 0.6f) cov_hi += 1.0f;
                    else if (button == 4 && py >= split_y && py < my0 + mh * 0.72f && px < mx0 + 4.2f) cov_hi += 1.0f;
                    else if (button == 5 && py >= my0 + mh * 0.62f && px < mx0 + 4.2f) cov_hi += 1.0f;
                    if (fabsf(py - split_y) <= 0.75f) cov_line += 1.0f;
                    if (py < split_y && fabsf(px - cx) <= 0.75f) cov_line += 1.0f;
                    float dw = sd_rbox(px, py, cx - 1.7f, my0 + mh * 0.14f, cx + 1.7f, my0 + mh * 0.36f, 1.3f);
                    if (dw <= 0.0f) cov_whl += 1.0f;
                }
                if (button == 0) {
                    /* small left/right move chevrons beside mouse */
                    float ax = mw * 0.5f + 2.2f;
                    float d1 = sd_seg(px, py, cx - ax - 2.6f, cy, cx - ax, cy - 2.4f);
                    float d2 = sd_seg(px, py, cx - ax - 2.6f, cy, cx - ax, cy + 2.4f);
                    float d3 = sd_seg(px, py, cx + ax + 2.6f, cy, cx + ax, cy - 2.4f);
                    float d4 = sd_seg(px, py, cx + ax + 2.6f, cy, cx + ax, cy + 2.4f);
                    float dm = d1 < d2 ? d1 : d2; if (d3 < dm) dm = d3; if (d4 < dm) dm = d4;
                    if (dm <= 0.9f) cov_line += 1.0f;
                }
            }
            uint8_t *p = dst + (size_t)(y0 + j) * pitch + (size_t)(x0 + i) * 4;
            if (cov_hi > 0.0f) over_px(p, hi.r, hi.g, hi.b, (cov_hi / 16.0f) * 0.92f);
            if (cov_line > 0.0f) over_px(p, 40.0f, 46.0f, 56.0f, (cov_line / 16.0f) * 0.88f);
            if (cov_whl > 0.0f) {
                if (button == 3) over_px(p, hi.r, hi.g, hi.b, cov_whl / 16.0f);
                else over_px(p, 48.0f, 54.0f, 64.0f, (cov_whl / 16.0f) * 0.90f);
            }
        }
    }
}

/* ---- Virtual key -> label or special icon (arrow / mouse) */
static int vk_short_name(int vk, char *out, size_t cap, int wide) {
    vk &= 0xff;
    if (!vk) { snprintf(out, cap, "-"); return 0; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { snprintf(out, cap, "%c", vk); return 0; }
    switch (vk) {
    case 0x01: return -1;       /* VK_LBUTTON -> mouse LMB */
    case 0x02: return -2;       /* VK_RBUTTON -> mouse RMB */
    case 0x04: return -3;       /* VK_MBUTTON -> mouse MMB */
    case 0x05: snprintf(out, cap, "M4"); return 0;
    case 0x06: snprintf(out, cap, "M5"); return 0;
    case 0x08: snprintf(out, cap, wide ? "BKSP" : "BS"); return 0;
    case 0x09: snprintf(out, cap, "TAB"); return 0;
    case 0x0D: snprintf(out, cap, wide ? "ENTER" : "ENT"); return 0;
    case 0x10: case 0xA0: case 0xA1: snprintf(out, cap, wide ? "SHIFT" : "SHF"); return 0;
    case 0x11: case 0xA2: case 0xA3: snprintf(out, cap, wide ? "CTRL" : "CTL"); return 0;
    case 0x12: case 0xA4: case 0xA5: snprintf(out, cap, "ALT"); return 0;
    case 0x1B: snprintf(out, cap, "ESC"); return 0;
    case 0x20: snprintf(out, cap, wide ? "SPACE" : "SPC"); return 0;
    case 0x25: return -10;      /* VK_LEFT */
    case 0x26: return -11;      /* VK_UP */
    case 0x27: return -12;      /* VK_RIGHT */
    case 0x28: return -13;      /* VK_DOWN */
    default:
        if (vk >= 0x60 && vk <= 0x69) { snprintf(out, cap, "N%d", vk - 0x60); return 0; }
        if (vk >= 0x70 && vk <= 0x7B) { snprintf(out, cap, "F%d", vk - 0x70 + 1); return 0; }
        snprintf(out, cap, "K");
        return 0;
    }
}

static void draw_kb_binding(uint8_t *dst, int pitch, int x0, int y0, int cw, int ch, int vk) {
    char buf[12];
    int wide = cw >= 48;
    int kind = vk_short_name(vk, buf, sizeof buf, wide);
    if (kind == -1 || kind == -2 || kind == -3) { draw_mouse(dst, pitch, x0, y0, cw, ch, -kind); return; }
    if (kind <= -10 && kind >= -13) { draw_arrow_key(dst, pitch, x0, y0, cw, ch, -(kind + 10)); return; }
    clear_cell(dst, pitch, x0, y0, cw, ch);
    float pad_x = cw > 40 ? 2.5f : 1.8f;
    float pad_y = ch > 32 ? 3.5f : 2.0f;
    /* keep single-char keys near-square in non-square cells (bumpers / stick-click slots) */
    if (strlen(buf) == 1 && cw > ch + 8) {
        float w = (float)ch - 2.0f;
        pad_x = (cw - w) * 0.5f;
    } else if (ch > cw + 2) {
        pad_y = (ch - (cw - 2.0f * pad_x)) * 0.5f;
    }
    draw_keycap_rect(dst, pitch, x0, y0, cw, ch, pad_x, pad_y, cw - pad_x, ch - pad_y, buf);
}

static void draw_kb_pair(uint8_t *dst, int pitch, int x0, int y0, int cw, int ch, int vk1, int vk2) {
    char a[12], b[12];
    int k1 = vk_short_name(vk1, a, sizeof a, 0);
    int k2 = vk_short_name(vk2, b, sizeof b, 0);
    if (k1 < 0) snprintf(a, sizeof a, "L");
    if (k2 < 0) snprintf(b, sizeof b, "R");
    clear_cell(dst, pitch, x0, y0, cw, ch);
    float gap = 2.0f;
    float kw = (cw - 3.0f - gap) * 0.5f;
    float kh = ch > 28 ? 24.0f : (ch - 4.0f);
    if (kh > kw + 2.0f) kh = kw + 2.0f;
    float by0 = (ch - kh) * 0.5f, by1 = by0 + kh;
    draw_keycap_rect(dst, pitch, x0, y0, cw, ch, 1.5f, by0, 1.5f + kw, by1, a);
    draw_keycap_rect(dst, pitch, x0, y0, cw, ch, 1.5f + kw + gap, by0, cw - 1.5f, by1, b);
}

static void draw_kb_wasd(uint8_t *dst, int pitch, int x0, int y0, int cw, int ch, const int vk4[4]) {
    /* vk4 = {left, up, right, down} -> compact WASD key label */
    char lbl[5] = {0};
    int order[4] = { 1, 0, 3, 2 }; /* W, A, S, D */
    int n = 0;
    for (int i = 0; i < 4; i++) {
        char tmp[12];
        int k = vk_short_name(vk4[order[i]], tmp, sizeof tmp, 0);
        lbl[n++] = (k == 0 && tmp[0]) ? tmp[0] : '?';
    }
    lbl[n] = 0;
    clear_cell(dst, pitch, x0, y0, cw, ch);
    float pad_x = 1.6f, pad_y = ch > 32 ? 5.0f : 3.5f;
    draw_keycap_rect(dst, pitch, x0, y0, cw, ch, pad_x, pad_y, cw - pad_x, ch - pad_y, lbl);
}

/* ================================================================ Action-prompt ring sync (Finding 148) */
static void prompts_sync_ring(int want_letter) {
#ifndef PROMPTS_NO_WIN
    static int last = -1;
    if (last == want_letter) return;
    uint32_t *t = (uint32_t *)(uintptr_t)0x7101464340ull;
    static const uint32_t sw[4] = {0x40, 0x80, 0x10, 0x20};
    static const uint32_t lt[4] = {0x80, 0x40, 0x20, 0x10};
    const uint32_t *want = want_letter ? lt : sw;
    if (IsBadReadPtr(t, sizeof sw)) return;
    if (memcmp(t, sw, sizeof sw) != 0 && memcmp(t, lt, sizeof lt) != 0) return;
    if (memcmp(t, want, sizeof sw) == 0) { last = want_letter; return; }
    DWORD old = 0;
    if (!VirtualProtect(t, sizeof sw, PAGE_READWRITE, &old)) return;
    memcpy(t, want, sizeof sw);
    VirtualProtect(t, sizeof sw, old, &old);
    last = want_letter;
    if (getenv("MR_PROMPTS_LOG"))
        printf("prompts: action prompt ring table -> %s\n", want_letter ? "printed-letter (Y/X/B/A)" : "positional (X/Y/A/B)");
#else
    (void)want_letter;
#endif
}

/* ================================================================ Menu confirm/cancel swap (swap_confirm)
 * Bayonetta 2 v1.2 stores two global bitmasks read by in-game menus (and not gameplay):
 *   0x71018e4810  confirm mask (default 0x10 = Pro A)
 *   0x71018e4814  cancel mask  (default 0x20 = Pro B)
 * Every frame, f_710098fc64 refreshes both masks from the system confirm-swap flag at 0x71034fdf54 (via
 * f_7100dab818 -> f_7100dab598; initialized to 0 by f_7100dab2c4 on Switch). Writing 0x71034fdf54 makes
 * the game keep confirm/cancel in sync every frame, and updating 0x71018e4810/14 immediately avoids a
 * one-frame lag on the poll that changes the setting or switches between pad and keyboard.
 *
 * Inline menu button prompts resolve through a 46-entry table at 0x7101537d88 ({u32 id, u32 crc31, f32, f32},
 * searched by f_7100bd1b0c):
 *   [0] id=0  Confirm (default 0x1760995e = crc32("btn_002") & 0x7fffffff = A)
 *   [1] id=1  Cancel  (default 0x0e69c8e4 = crc32("btn_001") & 0x7fffffff = B)
 *   [2] id=2  B       (0x0e69c8e4 = btn_001, gameplay/explicit B - left untouched)
 *   [3] id=3  A       (0x1760995e = btn_002, gameplay/explicit A - left untouched)
 *
 * A few in-game Yes/No & notice dialogs (f_71007c649c for Muspelheim / Gates of Hell teleporters,
 * f_7100b7a29c & f_7100b7a624 for the Chapter 1 telephone) hardcoded Pro A/B bits instead of reading
 * 0x71018e4810/14; games/bayonetta2/mrbuild/post_lift.rs patches those checks to use both masks. */
#define B2_PAD_A        0x10u
#define B2_PAD_B        0x20u
#define B2_PIC_A        0x1760995eu   /* crc32("btn_002") & 0x7fffffff */
#define B2_PIC_B        0x0e69c8e4u   /* crc32("btn_001") & 0x7fffffff */
#define B2_CONFIRM_BITS 0x71018e4810ULL
#define B2_CANCEL_BITS  0x71018e4814ULL
#define B2_SWAP_FLAG    0x71034fdf54ULL
#define B2_GUIDE_TABLE  0x7101537d88ULL

/* Exposed for unit tests (tests/prompts_test.c) as well as live prompts_menu_swap_tick(). */
int prompts_apply_menu_swap_at(int want_swap, uint32_t *swap_flag, uint32_t *confirm_bits,
                               uint32_t *cancel_bits, uint32_t *guide_raw,
                               int (*make_rw)(void *, size_t)) {
    if (!swap_flag || !confirm_bits || !cancel_bits) return 0;
    uint32_t cur_c = *confirm_bits, cur_x = *cancel_bits;
    if (!((cur_c == B2_PAD_A && cur_x == B2_PAD_B) || (cur_c == B2_PAD_B && cur_x == B2_PAD_A)))
        return 0;

    uint32_t want_flag    = want_swap ? 1u : 0u;
    uint32_t want_confirm = want_swap ? B2_PAD_B : B2_PAD_A;
    uint32_t want_cancel  = want_swap ? B2_PAD_A : B2_PAD_B;
    uint32_t want_pic0    = want_swap ? B2_PIC_B : B2_PIC_A;
    uint32_t want_pic1    = want_swap ? B2_PIC_A : B2_PIC_B;
    int changed = 0;

    if (*swap_flag != want_flag) { *swap_flag = want_flag; changed = 1; }
    if (cur_c != want_confirm)   { *confirm_bits = want_confirm; changed = 1; }
    if (cur_x != want_cancel)    { *cancel_bits  = want_cancel;  changed = 1; }

    if (guide_raw && guide_raw[0] == 0u && guide_raw[4] == 1u) {
        uint32_t h0 = guide_raw[1], h1 = guide_raw[5];
        if ((h0 == B2_PIC_A && h1 == B2_PIC_B) || (h0 == B2_PIC_B && h1 == B2_PIC_A)) {
            if (h0 != want_pic0 || h1 != want_pic1) {
                if (!make_rw || make_rw(guide_raw, 8 * sizeof(uint32_t))) {
                    guide_raw[1] = want_pic0;
                    guide_raw[5] = want_pic1;
                    changed = 1;
                }
            }
        }
    }
    return changed;
}

#ifndef PROMPTS_NO_WIN
static int make_guide_rw(void *p, size_t n) {
    static int rw_done = 0;
    if (rw_done) return 1;
    DWORD old = 0;
    /* Keep this .rodata page writable once touched so concurrent pad polls cannot race VirtualProtect. */
    if (!VirtualProtect(p, n, PAGE_READWRITE, &old)) return 0;
    rw_done = 1;
    return 1;
}
#endif

/* ================================================================ Family & state key */
static int env_prompts_override(void) {
    const char *e = getenv("MR_PROMPTS");
    if (!e || !e[0]) return 0;
    if (!_stricmp(e, "switch") || !strcmp(e, "1")) return 1;
    if (!_stricmp(e, "playstation") || !_stricmp(e, "ps") || !strcmp(e, "2")) return 2;
    if (!_stricmp(e, "xbox") || !strcmp(e, "3")) return 3;
    if (!_stricmp(e, "keyboard") || !_stricmp(e, "kb") || !strcmp(e, "4")) return 4;
    return 0;
}

static int current_family(void) {
    int ov = env_prompts_override();
    return ov ? ov : nnpad_button_prompts();
}

void prompts_menu_swap_tick(void) {
#ifndef PROMPTS_NO_WIN
    static int mapped = 0;
    uint32_t *swap_flag    = (uint32_t *)(uintptr_t)B2_SWAP_FLAG;
    uint32_t *confirm_bits = (uint32_t *)(uintptr_t)B2_CONFIRM_BITS;
    uint32_t *cancel_bits  = (uint32_t *)(uintptr_t)B2_CANCEL_BITS;
    uint32_t *guide        = (uint32_t *)(uintptr_t)B2_GUIDE_TABLE;
    if (!mapped) {
        if (IsBadReadPtr(confirm_bits, 8) || IsBadReadPtr(swap_flag, 4) || IsBadReadPtr(guide, 32))
            return;
        mapped = 1;
    }
    int want = nnpad_menu_swapped() && current_family() != 4;
    if (prompts_apply_menu_swap_at(want, swap_flag, confirm_bits, cancel_bits, guide, make_guide_rw) &&
        getenv("MR_PROMPTS_LOG")) {
        printf("prompts: swap_confirm -> %s (confirm=0x%x cancel=0x%x)\n",
               want ? "on (B confirm, A back)" : "off (A confirm, B back)",
               *confirm_bits, *cancel_bits);
    }
    /* Sync gameplay controller rebinding state into game.dll / game.so (disabled on keyboard). */
    {
        unsigned char r[16];
        int on = nnpad_bind_live() && current_family() != 4 && nnpad_bind_get(r) && rb_valid(r);
        if (on) {
            for (int i = 0; i < 16; i++) game_rebind[i] = r[i];
        }
        game_rebind_on = on;
    }
#endif
}

int prompts_control_type(void) {
#ifndef PROMPTS_NO_WIN
    int t = game_control_type;
    return t >= 0 && t < 4 ? t : 0;
#else
    return 0;
#endif
}

uint64_t prompts_state_key(void) {
    int fam = current_family();
    int letter = (fam == 2 || fam == 3) ? (nnpad_prompts_by_letter() != 0) : 0;
    uint64_t h = 0x517cc1b727220a95ULL ^ ((uint64_t)fam << 8) ^ (uint64_t)letter;
    if (fam == 4) {
        h = (h ^ (uint64_t)(nnpad_mouse_cfg(NULL, NULL) != 0)) * 0x100000001B3ULL;
        for (int i = 0; i < 24; i++) {
            h = (h ^ (uint64_t)(nnpad_kb_vk(i) & 0xff)) * 0x100000001B3ULL;
        }
    }
    h ^= h >> 29;
    return h ? h : 1;
}

/* ================================================================ Atlas identification */
int prompts_identify(int nvn_format, int w, int h, int levels, int layers, size_t data_size, const uint8_t *swizzled) {
    if (!swizzled || levels != 1 || layers != 1) return 0;
    int is_rgba8 = (nvn_format == TEX_FMT_RGBA8 || nvn_format == TEX_FMT_RGBA8_SRGB);
    int is_bc3   = (nvn_format == TEX_FMT_RGBA_DXT5 || nvn_format == TEX_FMT_RGBA_DXT5_SRGB);

    if (is_rgba8 && w == 256 && h == 256 && data_size == 262144) {
        uint64_t hash = tex_pack_hash(swizzled, data_size);
        for (int i = 0; i < 5; i++) {
            if (k_core_layouts[i].hash == hash) {
                if (getenv("MR_PROMPTS_LOG"))
                    printf("prompts: identified %s (hash %016llx)\n", k_core_layouts[i].tag, (unsigned long long)hash);
                return k_core_layouts[i].id;
            }
        }
    } else if (is_bc3 && w == 512 && h == 1024 && data_size == 524288) {
        uint64_t hash = tex_pack_hash(swizzled, data_size);
        if (hash == HASH_HUD1_MAIN) {
            if (getenv("MR_PROMPTS_LOG"))
                printf("prompts: identified hud1_main (hash %016llx)\n", (unsigned long long)hash);
            return ATLAS_HUD1;
        }
    } else if (is_rgba8 && w == 128 && h == 64 && data_size == 32768) {
        uint64_t hash = tex_pack_hash(swizzled, data_size);
        if (hash == HASH_TUTORIAL_BTN) {
            if (getenv("MR_PROMPTS_LOG"))
                printf("prompts: identified tutorial_btn (hash %016llx)\n", (unsigned long long)hash);
            return ATLAS_TUT1;
        }
    }
    return 0;
}

/* ================================================================ Atlas patching */
static void patch_core(uint8_t *rgba, int w, int h, const CoreLayout *lay,
                       int fam, int by_letter, int mouse_cam, const int kb[24]) {
    int pitch = w * 4;
    if (fam <= 1 || w < 256 || h < 256) return;

    if (fam == 2) {
        /* PlayStation:
         * positional: A(right)=Circle(1), B(bottom)=Cross(0), X(top)=Triangle(3), Y(left)=Square(2)
         * by_letter:  A=Cross(0), B=Circle(1), X=Square(2), Y=Triangle(3) */
        draw_ps_face(rgba, pitch, lay->xy[SL_A][0], lay->xy[SL_A][1], 32, 32, by_letter ? 0 : 1);
        draw_ps_face(rgba, pitch, lay->xy[SL_B][0], lay->xy[SL_B][1], 32, 32, by_letter ? 1 : 0);
        draw_ps_face(rgba, pitch, lay->xy[SL_X][0], lay->xy[SL_X][1], 32, 32, by_letter ? 2 : 3);
        draw_ps_face(rgba, pitch, lay->xy[SL_Y][0], lay->xy[SL_Y][1], 32, 32, by_letter ? 3 : 2);
        draw_bumper(rgba, pitch, lay->xy[SL_L][0], lay->xy[SL_L][1], 53, 27, "L1");
        draw_bumper(rgba, pitch, lay->xy[SL_R][0], lay->xy[SL_R][1], 53, 27, "R1");
        draw_trigger(rgba, pitch, lay->xy[SL_ZL][0], lay->xy[SL_ZL][1], 37, 37, "L2", 1);
        draw_trigger(rgba, pitch, lay->xy[SL_ZR][0], lay->xy[SL_ZR][1], 37, 37, "R2", 0);
        draw_sys_disc(rgba, pitch, lay->xy[SL_PLUS][0], lay->xy[SL_PLUS][1], 28, 28, 0);
        draw_sys_disc(rgba, pitch, lay->xy[SL_MINUS][0], lay->xy[SL_MINUS][1], 28, 28, 1);
        draw_stick_click(rgba, pitch, lay->xy[SL_L3][0], lay->xy[SL_L3][1], 32, 38, "L3");
        draw_stick_click(rgba, pitch, lay->xy[SL_R3][0], lay->xy[SL_R3][1], 32, 38, "R3");
    } else if (fam == 3) {
        /* Xbox:
         * positional: A(right)=B(1), B(bottom)=A(0), X(top)=Y(3), Y(left)=X(2)
         * by_letter:  A=A(0), B=B(1), X=X(2), Y=Y(3) */
        draw_xbox_face(rgba, pitch, lay->xy[SL_A][0], lay->xy[SL_A][1], 32, 32, by_letter ? 0 : 1);
        draw_xbox_face(rgba, pitch, lay->xy[SL_B][0], lay->xy[SL_B][1], 32, 32, by_letter ? 1 : 0);
        draw_xbox_face(rgba, pitch, lay->xy[SL_X][0], lay->xy[SL_X][1], 32, 32, by_letter ? 2 : 3);
        draw_xbox_face(rgba, pitch, lay->xy[SL_Y][0], lay->xy[SL_Y][1], 32, 32, by_letter ? 3 : 2);
        draw_bumper(rgba, pitch, lay->xy[SL_L][0], lay->xy[SL_L][1], 53, 27, "LB");
        draw_bumper(rgba, pitch, lay->xy[SL_R][0], lay->xy[SL_R][1], 53, 27, "RB");
        draw_trigger(rgba, pitch, lay->xy[SL_ZL][0], lay->xy[SL_ZL][1], 37, 37, "LT", 1);
        draw_trigger(rgba, pitch, lay->xy[SL_ZR][0], lay->xy[SL_ZR][1], 37, 37, "RT", 0);
        draw_sys_disc(rgba, pitch, lay->xy[SL_PLUS][0], lay->xy[SL_PLUS][1], 28, 28, 0);
        draw_sys_disc(rgba, pitch, lay->xy[SL_MINUS][0], lay->xy[SL_MINUS][1], 28, 28, 2);
        draw_stick_click(rgba, pitch, lay->xy[SL_L3][0], lay->xy[SL_L3][1], 32, 38, "LS");
        draw_stick_click(rgba, pitch, lay->xy[SL_R3][0], lay->xy[SL_R3][1], 32, 38, "RS");
    } else if (fam == 4) {
        /* Keyboard & Mouse: reflects actual bindings in controllers.ini [keyboard] */
        static const struct { int sl, kb_idx; } direct[] = {
            {SL_A, 0}, {SL_B, 1}, {SL_X, 2}, {SL_Y, 3},
            {SL_L3, 4}, {SL_R3, 5}, {SL_L, 6}, {SL_R, 7},
            {SL_ZL, 8}, {SL_ZR, 9}, {SL_PLUS, 10}, {SL_MINUS, 11},
            {SL_DPAD_L, 12}, {SL_DPAD_U, 13}, {SL_DPAD_R, 14}, {SL_DPAD_D, 15},
        };
        for (size_t i = 0; i < sizeof direct / sizeof direct[0]; i++) {
            int sl = direct[i].sl;
            draw_kb_binding(rgba, pitch, lay->xy[sl][0], lay->xy[sl][1], k_core_wh[sl][0], k_core_wh[sl][1], kb[direct[i].kb_idx]);
        }
        draw_arrow_key(rgba, pitch, lay->xy[SL_DPAD][0],    lay->xy[SL_DPAD][1],    32, 32, 6);
        draw_arrow_key(rgba, pitch, lay->xy[SL_DPAD_UD][0], lay->xy[SL_DPAD_UD][1], 32, 32, 4);
        draw_arrow_key(rgba, pitch, lay->xy[SL_DPAD_LR][0], lay->xy[SL_DPAD_LR][1], 32, 32, 5);
        draw_kb_wasd(rgba, pitch, lay->xy[SL_LS][0],     lay->xy[SL_LS][1],     34, 32, &kb[16]);
        draw_kb_wasd(rgba, pitch, lay->xy[SL_LS_ROT][0], lay->xy[SL_LS_ROT][1], 38, 38, &kb[16]);
        draw_kb_pair(rgba, pitch, lay->xy[SL_LS_UD][0],  lay->xy[SL_LS_UD][1],  52, 38, kb[17], kb[19]);
        draw_kb_pair(rgba, pitch, lay->xy[SL_LS_LR][0],  lay->xy[SL_LS_LR][1],  56, 38, kb[16], kb[18]);
        if (mouse_cam) {
            draw_mouse(rgba, pitch, lay->xy[SL_RS][0],     lay->xy[SL_RS][1],     34, 32, 0);
            draw_mouse(rgba, pitch, lay->xy[SL_RS_ROT][0], lay->xy[SL_RS_ROT][1], 38, 38, 0);
        } else {
            draw_kb_wasd(rgba, pitch, lay->xy[SL_RS][0],     lay->xy[SL_RS][1],     34, 32, &kb[20]);
            draw_kb_wasd(rgba, pitch, lay->xy[SL_RS_ROT][0], lay->xy[SL_RS_ROT][1], 38, 38, &kb[20]);
        }
    }
}

/* Patch one 4x4-aligned sub-region of a BC3 (DXT5) texture in place, preserving every outside block bit-exact. */
static void patch_bc3_rect(uint8_t *bc3, int tex_w, int tex_h, int rx, int ry, int rw, int rh,
                           void (*paint)(uint8_t *rgba, int pitch, int ox, int oy, int rw, int rh, void *ud), void *ud) {
    int bx0 = (rx & ~3), by0 = (ry & ~3);
    int bx1 = (rx + rw + 3) & ~3, by1 = (ry + rh + 3) & ~3;
    if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0;
    if (bx1 > tex_w) bx1 = tex_w; if (by1 > tex_h) by1 = tex_h;
    int bw = bx1 - bx0, bh = by1 - by0;
    if (bw <= 0 || bh <= 0) return;

    uint8_t *sub_bc3 = malloc((size_t)(bw / 4) * (size_t)(bh / 4) * 16);
    uint8_t *sub_rgba = malloc((size_t)bw * (size_t)bh * 4);
    if (!sub_bc3 || !sub_rgba) { free(sub_bc3); free(sub_rgba); return; }

    int tex_bpr = tex_w / 4, sub_bpr = bw / 4;
    for (int by = 0; by < bh / 4; by++)
        memcpy(sub_bc3 + (size_t)by * sub_bpr * 16,
               bc3 + ((size_t)(by0 / 4 + by) * tex_bpr + (size_t)(bx0 / 4)) * 16,
               (size_t)sub_bpr * 16);

    tex_decode_rgba8(TEX_FMT_RGBA_DXT5, sub_bc3, bw, bh, sub_rgba);
    paint(sub_rgba, bw * 4, rx - bx0, ry - by0, rw, rh, ud);
    tex_encode_bc3(sub_rgba, bw, bh, sub_bc3);

    for (int by = 0; by < bh / 4; by++)
        memcpy(bc3 + ((size_t)(by0 / 4 + by) * tex_bpr + (size_t)(bx0 / 4)) * 16,
               sub_bc3 + (size_t)by * sub_bpr * 16,
               (size_t)sub_bpr * 16);

    free(sub_bc3);
    free(sub_rgba);
}

typedef struct { int fam, kind, kb_vk; } HudPaintCtx;
static void hud_paint_cb(uint8_t *rgba, int pitch, int ox, int oy, int rw, int rh, void *ud) {
    HudPaintCtx *c = ud;
    if (c->fam == 4) { draw_kb_binding(rgba, pitch, ox, oy, rw, rh, c->kb_vk); return; }
    if (c->kind == 0) draw_bumper(rgba, pitch, ox, oy, rw, rh, c->fam == 2 ? "L1" : "LB");
    else if (c->kind == 1) draw_bumper(rgba, pitch, ox, oy, rw, rh, c->fam == 2 ? "R1" : "RB");
    else if (c->kind == 2) draw_trigger(rgba, pitch, ox, oy, rw, rh, c->fam == 2 ? "L2" : "LT", 1);
    else draw_trigger(rgba, pitch, ox, oy, rw, rh, c->fam == 2 ? "R2" : "RT", 0);
}

static void patch_hud1(uint8_t *bc3, int w, int h, int fam, const int kb[24]) {
    if (fam <= 1 || w != 512 || h != 1024) return;
    /* hud1_actbutton_L, R, ZL, ZR in hud1.uvd */
    static const struct { int x, y, w, h, kind, kb_idx; } slots[4] = {
        {260, 960, 78, 35, 0, 6},   /* L */
        {180, 732, 78, 35, 1, 7},   /* R */
        {400,  16, 50, 50, 2, 8},   /* ZL */
        {460, 920, 50, 50, 3, 9},   /* ZR */
    };
    for (int i = 0; i < 4; i++) {
        HudPaintCtx ctx = { fam, slots[i].kind, kb[slots[i].kb_idx] };
        patch_bc3_rect(bc3, w, h, slots[i].x, slots[i].y, slots[i].w, slots[i].h, hud_paint_cb, &ctx);
    }
}

static void patch_tut1(uint8_t *rgba, int w, int h, int fam, const int kb[24]) {
    if (fam != 4 || w < 128 || h < 64) return;
    /* btn_029 (0, 0, 38x38) = L stick up, btn_030 (40, 0, 38x38) = L stick down */
    draw_kb_binding(rgba, w * 4,  0, 0, 38, 38, kb[17]);
    draw_kb_binding(rgba, w * 4, 40, 0, 38, 38, kb[19]);
}

/* Exposed for unit tests (tests/prompts_test.c) as well as live prompts_patch(). */
void prompts_patch_ex(int atlas_id, uint8_t *linear, int w, int h, int nvn_format,
                      int fam, int by_letter, int mouse_cam, const int kb[24]) {
    (void)nvn_format;
    if (!linear || atlas_id <= 0 || fam <= 1) return;
    if (atlas_id >= ATLAS_CORE_US && atlas_id <= ATLAS_CORE_IT) {
        patch_core(linear, w, h, &k_core_layouts[atlas_id - ATLAS_CORE_US], fam, by_letter, mouse_cam, kb);
    } else if (atlas_id == ATLAS_HUD1) {
        patch_hud1(linear, w, h, fam, kb);
    } else if (atlas_id == ATLAS_TUT1) {
        patch_tut1(linear, w, h, fam, kb);
    }
}

void prompts_patch(int atlas_id, uint8_t *linear, int w, int h, int nvn_format) {
    int fam = current_family();
    int by_letter = (fam == 2 || fam == 3) ? (nnpad_prompts_by_letter() != 0) : 0;
    int mouse_cam = nnpad_mouse_cfg(NULL, NULL) != 0;
    int kb[24];
    for (int i = 0; i < 24; i++) kb[i] = nnpad_kb_vk(i);
    if (atlas_id >= ATLAS_CORE_US && atlas_id <= ATLAS_CORE_IT)
        prompts_sync_ring(by_letter);
    if (getenv("MR_PROMPTS_LOG")) {
        static const char *const fn[5] = {"auto", "switch", "playstation", "xbox", "keyboard"};
        printf("prompts: patching atlas %d (%dx%d fmt 0x%x) -> %s%s\n", atlas_id, w, h, nvn_format,
               fn[fam >= 1 && fam <= 4 ? fam : 0], (fam == 2 || fam == 3) ? (by_letter ? " (printed-letter)" : " (positional)") : "");
    }
    prompts_patch_ex(atlas_id, linear, w, h, nvn_format, fam, by_letter, mouse_cam, kb);
}
