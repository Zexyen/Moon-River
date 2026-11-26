/* [moonriver] PC settings menu: a panel drawn by the runtime over the game, for the keys of graphics.ini.
 *
 * Open: F1 on the keyboard, or Start + Select (Plus + Minus) on player 1's controller; seen at the game's own pad reads
 * (pcmenu_gate, nnhid.c), so nothing runs while it is closed but one key and one mask test per read. While it is open:
 *   - the game is held: every guest thread stops at its next SDK call (hostrt.c g_guest_frozen, the console's suspension);
 *     its sound runs dry; the game's pad reads (none while held) get an idle pad, and after the menu
 *     closes too, until the buttons that closed it are let go;
 *   - a thread of its own reads player 1 (nnhid_menu_read: test scripts, the keyboard player's keys, the first controller),
 *     the arrow keys / Enter / Escape / F1 and the mouse, and posts a job to the render thread about 60 times a second that
 *     presents the game's last picture again with the panel over it (nvn_vk_pcmenu.c).
 * Controls: up / down choose a row, left / right (or A / Enter, B / Escape closes) change it; the mouse: point at a row,
 * click on the value (on '‹' for the one before). A change is written to graphics.ini at once (gfx_option_set: comments and
 * other keys kept) and applied while the game runs where the runtime can: cutscene_fps (the game library's switch), vsync
 * (present mode and pacing), the window (Linux), model detail, film grain, aspect,
 * resolution_scale (when the panel closes, so that going past 3.0 does not make its images; from the game's Options rows at
 * once: nvn_vk_draw.c rs_apply). The rest (frame_rate) is marked "applies at the next start". A key set by an environment
 * variable is shown and locked.
 * Rows with `optional` appear only when graphics.ini has their key (for keys of later builds; a frame-rate cap: one more row,
 * and its live field in gfx_option_set). Anisotropic filtering and the mip bias are fixed (16, -1; gfx_options.c) and have no rows since 2026-11-10.
 * The panel: drawn on the CPU (BGRA) when something changes, text with stb_truetype (third_party/, public domain / MIT) in
 * subsets of Marcellus and Source Sans 3 (SIL OFL 1.1, third_party/OFL-*.txt), embedded with #embed, with the Moon River
 * icon from runtime/art/icon.bmp and procedural Moon River night-sky / moon-phase / river-shimmer / glowing-glyph art
 * matching the launcher (launcher/ui/launcher.rcss, launcher/src/ui_render.cpp). The same drawing makes the first-start
 * shader preparation screen (draw_prep) for nvn_vk_draw.c.
 * Some rows are not graphics.ini's: button_prompts, swap_confirm and layout, of controllers.ini.
 * Test aids: MR_PCMENU_SHOT=<folder> (nvn_vk_pcmenu.c), MR_PCMENU_OPEN=<seconds> opens it that long after the first pad
 * read (for scripted runs without keys), MR_PCMENU_LOG=1 logs each input step. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include "gfx_options.h"
#include "rebind.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

static const unsigned char k_font_head[] = {               /* Marcellus (SIL OFL 1.1, third_party/OFL-Marcellus.txt): titles */
#embed "third_party/Marcellus-pcmenu.ttf"
};
static const unsigned char k_font_ui[] = {                 /* Source Sans 3 Regular and SemiBold (SIL OFL 1.1, third_party/OFL-SourceSans3.txt) */
#embed "third_party/SourceSans3-pcmenu.ttf"
};
static const unsigned char k_font_uib[] = {
#embed "third_party/SourceSans3-SemiBold-pcmenu.ttf"
};

extern volatile LONG g_guest_frozen;                       /* hostrt.c */
extern volatile LONG g_display_close;                      /* display.c: the window is being closed (the game must run to exit) */
void nnhid_menu_read(uint64_t *buttons, int *lx, int *ly, int swap_ab);       /* nnhid.c */
int nnhid_menu_key(int vk);
uint32_t nnhid_menu_pad(void);
int nvn_front_post_pcmenu(void);                           /* nvn_gl.c: 0 = not the Vulkan renderer */
int nvn_vk_hdr_on(void);                                   /* nvn_vk.c: the picture goes out as HDR now */
void display_vsync_apply(void);                            /* display.c */
int display_window_apply(void);
void display_quit(void);
void display_mouse_want(int on);
int nnpad_mouse_cfg(float *sens, int *invert);             /* nnpad_sdl.c */
const char *nnpad_prompts_setting(void);
int nnpad_prompts_set(const char *val);
const char *nnpad_menu_swap_setting(void);
int nnpad_menu_swap_set(const char *val);
int nnpad_menu_swapped(void);
const char *nnpad_layout_setting(void);
int nnpad_layout_set(const char *val);
int nnpad_button_prompts(void);
int nnpad_prompts_by_letter(void);
int nnpad_bind_get(unsigned char *out);
int nnpad_bind_set(const unsigned char *b);
int prompts_control_type(void);

/* A_PROMPTS, A_MENU_SWAP, A_LAYOUT: keys of controllers.ini. A_PAGE: opens Controls page.
 * A_QUIT: ends the game. A_PRESET / A_BIND: page 1 (Controls) rows. */
enum { A_LIVE, A_VSYNC, A_WINDOW, A_SIZE, A_SCALE, A_RESTART, A_PROMPTS, A_MENU_SWAP, A_LAYOUT, A_PAGE, A_QUIT, A_PRESET, A_BIND };
#define ROW_VALUES 12
typedef struct {
    const char *key, *label, *help, *env, *dflt;           /* dflt: the reader's value when the key is not in the file */
    int apply, optional;
    const char *val[ROW_VALUES], *name[ROW_VALUES];
    int slot;                                              /* A_BIND: Type A logical Pro button index 0..15 */
} Row;
#define BIND(key, label, slot) {key, label, "Press A / Enter, then press the controller button to assign (swaps if already used). Left / right also cycles buttons.", NULL, NULL, A_BIND, 0, {0}, {0}, slot}
static const Row k_rows[] = {
    {"window", "Display mode", "Windowed: a window of the size below. Borderless and fullscreen cover the monitor the window is on.", NULL, "windowed", A_WINDOW, 0,
     {"windowed", "borderless", "fullscreen"}, {"Windowed", "Borderless", "Fullscreen"}, 0},
    {"size", "Window size", "The window's size in windowed mode. The game is drawn at the internal resolution and shown at this size.", "MR_WINDOW", NULL, A_SIZE, 0,
     {"1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"}, {"1280 \xC3\x97 720", "1600 \xC3\x97 900", "1920 \xC3\x97 1080", "2560 \xC3\x97 1440", "3840 \xC3\x97 2160"}, 0},
    {"frame_rate", "Frame rate", "60: as on the console. Above 60 (experimental): real-time clock, smart timers and smoothings. Applies at the next start.", "RECOMPS_UNCAP", "60", A_RESTART, 0,
     {"60", "90", "120"}, {"60 fps", "90 fps", "120 fps"}, 0},
    {"vsync", "Vertical sync", "Auto: locked to a 60 / 120 / 240 Hz display, else the program's own clock. Off: no waiting, tearing allowed. On: the swapchain's FIFO.", "MR_VK_PRESENT", "auto", A_VSYNC, 0,
     {"auto", "display", "clock", "off", "on", "mailbox"}, {"Auto", "Display", "Clock", "Off", "On", "Mailbox"}, 0},
    {"resolution_scale", "Internal resolution", "The size the game renders at (720p is the console's docked size). Applies when this panel closes.", "MR_VK_SCALE", "1.0", A_SCALE, 0,
     {"0.5", "0.75", "1.0", "1.25", "1.5", "2.0", "2.5", "3.0"},
     {"360p", "540p", "720p", "900p", "1080p", "1440p", "1800p", "4K"}, 0},
    {"hdr", "HDR", "Auto: HDR output while the desktop's HDR is on for this monitor. The scene's lights go above white, up to HDR peak; menus and the HUD stay at white.", NULL, "auto", A_LIVE, 0,
     {"auto", "off", "on"}, {"Auto", "Off", "On"}, 0},
    {"hdr_paper_white", "HDR white", "How bright the game's white is with HDR on. Auto: the desktop's brightness of SDR programs.", NULL, "auto", A_LIVE, 0,
     {"auto", "100", "150", "200", "250", "300", "400"}, {"Auto", "100 nits", "150 nits", "200 nits", "250 nits", "300 nits", "400 nits"}, 0},
    {"hdr_peak", "HDR peak", "How bright the scene's brightest parts get with HDR on: your monitor's peak brightness. Auto: what the desktop says (Linux), 1000 on Windows.", NULL, "auto", A_LIVE, 0,
     {"auto", "400", "600", "800", "1000", "1500", "2000", "4000"}, {"Auto", "400 nits", "600 nits", "800 nits", "1000 nits", "1500 nits", "2000 nits", "4000 nits"}, 0},
    {"aspect", "Picture fit", "Keep shape: 16:9 with bars. Stretch: fills window. Wide: native 21:9 / 32:9 ultrawide (applies at next start).", NULL, "keep", A_LIVE, 0,
     {"keep", "stretch", "wide"}, {"Keep shape", "Stretch", "Wide"}, 0},
    {"hud_area", "HUD area", "Ultrawide only. Centered (16:9): keeps HUD in the middle 16:9 area. Full screen: moves corner HUD elements to the ultrawide edges.", NULL, "screen", A_LIVE, 0,
     {"16:9", "screen"}, {"Centered (16:9)", "Full screen"}, 0},
    {"button_prompts", "Button prompts", "Which buttons the game shows. Auto follows what you use, pad, keyboard or mouse. The others always show that set.", NULL, "auto", A_PROMPTS, 0,
     {"auto", "playstation", "xbox", "switch", "keyboard"}, {"Auto", "PlayStation", "Xbox", "Switch", "Keyboard"}, 0},
    {"swap_confirm", "Swap Confirm", "Menus confirm with B and go back with A, and show it. Gameplay and the keyboard stay the same. This panel follows when it next opens.", NULL, "off", A_MENU_SWAP, 0,
     {"off", "on"}, {"Off", "On"}, 0},
    {"layout", "Face buttons", "Position: the right button acts as A, as on a Switch. Printed letter: the button marked A, Cross as A on PlayStation pads. Prompts follow.", NULL, "auto", A_LAYOUT, 0,
     {"auto", "positional", "labels"}, {"Auto", "Position", "Printed letter"}, 0},
    {"controls", "Controls", "Rebind every button of gameplay for controllers. Menus keep their own buttons, and the game's prompts follow yours.", NULL, "open", A_PAGE, 0,
     {"open"}, {"Open"}, 0},
    {"quit", "Quit to desktop", "Ends the game, as closing its window does. Progress since the last save is lost. Press again to confirm.", NULL, "quit", A_QUIT, 0,
     {"quit"}, {"Quit"}, 0},
    /* ---- Page 1: Controls ---- */
    {"bind_preset", "Preset", "Bayonetta 2's four built-in control types as a starting point. Game's setting uses the save's own Control Type (A-D).", NULL, "game", A_PRESET, 0,
     {"game", "a", "b", "c", "d"}, {"Game's setting", "Type A", "Type B", "Type C", "Type D"}, 0},
    BIND("bind_punch",  "Punch",            RB_X),
    BIND("bind_kick",   "Kick",             RB_A),
    BIND("bind_jump",   "Jump",             RB_B),
    BIND("bind_shoot",  "Shoot",            RB_Y),
    BIND("bind_dodge",  "Dodge",            RB_ZR),
    BIND("bind_lock",   "Lock-On",          RB_R),
    BIND("bind_weapon", "Weapon Set",       RB_ZL),
    BIND("bind_climax", "Umbran Climax",    RB_L),
    BIND("bind_cam",    "Reset Camera",     RB_STICK_R),
    BIND("bind_l3",     "Left Stick Press", RB_STICK_L),
    BIND("bind_dleft",  "Item (Left)",      RB_LEFT),
    BIND("bind_dup",    "Item (Up)",        RB_UP),
    BIND("bind_dright", "Item (Right)",     RB_RIGHT),
    BIND("bind_ddown",  "Item (Down)",      RB_DOWN),
    BIND("bind_sub",    "Subscreen",        RB_MINUS),
    BIND("bind_pause",  "Pause",            RB_PLUS),
};
#define NROW ((int)(sizeof k_rows / sizeof k_rows[0]))

static SRWLOCK g_lock = SRWLOCK_INIT;
static volatile LONG g_open, g_work, g_job;
static ULONGLONG g_open_at, g_close_at;                    /* the opening's and the closing's start (the GPU path animates them: pcmenu_gpu_scene) */
#define PCM_OPEN_MS 320                                        /* the opening: a quick start, a gradual finish (ease_out) */
#define PCM_CLOSE_MS 220                                       /* the closing: the same way back, quicker */
static volatile LONG g_release;                            /* a bit per npad id: that player still holds what was pressed in the menu */
static int g_sel, g_cur[NROW], g_start[NROW], g_shown[NROW], g_lock_why[NROW], g_inited, g_said_restart[NROW], g_nowin;
static char g_raw[NROW][32];                               /* a value of the file that is none of the row's: shown as it is */
static unsigned g_ver;
static int g_env_cut, g_env_set[NROW];
static int g_swap, g_quit_ask;                                         /* swap_confirm as this opening of the panel began; Quit confirm */
static int g_fam, g_letter;                                /* the button names of the hints */
static int g_page, g_cap = -1, g_cap_armed;                /* 1 = Controls page; g_cap = row waiting for a pad button */
static ULONGLONG g_cap_t;
static uint8_t g_b[16], g_b_start[16];                     /* Controls layout shown & snapshot when opened */
static int g_b_on, g_b_start_on;
/* the environment as the program started (gfx_options sets MR_CUTSCENE_FPS itself, later) */
__attribute__((constructor)) static void pcmenu_env(void) {
    for (int i = 0; i < NROW; i++) g_env_set[i] = k_rows[i].env && getenv(k_rows[i].env) != NULL;
}
static int row_on(int i) { return g_shown[i] && g_page == (k_rows[i].apply >= A_PRESET) && (strncmp(k_rows[i].key, "hdr_", 4) || nvn_vk_hdr_on()); }      /* HDR's two brightness rows: while the picture goes out as HDR */
static int row_count(int i) { if (k_rows[i].apply == A_BIND) return 16; int n = 0; while (n < ROW_VALUES && k_rows[i].val[n]) n++; return n; }
static int find_val(int i, const char *v) {
    for (int k = 0; k < row_count(i); k++) {
        if (!_stricmp(k_rows[i].val[k], v)) return k;
        char *e; double a = strtod(v, &e), b = atof(k_rows[i].val[k]);       /* numbers: 1 = 1.0, 0.50 = 0.5 */
        if (e != v && !*e && strspn(k_rows[i].val[k], "+-.0123456789") == strlen(k_rows[i].val[k]) && fabs(a - b) < 1e-6) return k;
    }
    return -1;
}
static void binds_sync(void) {
    for (int i = 0; i < NROW; i++) {
        if (k_rows[i].apply == A_BIND) {
            g_cur[i] = g_b[k_rows[i].slot];
            g_start[i] = g_b_start[k_rows[i].slot];
        } else if (k_rows[i].apply == A_PRESET) {
            if (!g_b_on) g_cur[i] = 0;
            else {
                int p = rb_which_preset(g_b);
                g_cur[i] = p >= 0 ? p + 1 : -1;
                if (g_cur[i] < 0) snprintf(g_raw[i], sizeof g_raw[i], "Custom");
            }
            if (!g_b_start_on) g_start[i] = 0;
            else {
                int p = rb_which_preset(g_b_start);
                g_start[i] = p >= 0 ? p + 1 : -1;
            }
        }
    }
}
static void binds_load(void) {
    g_b_on = nnpad_bind_get(g_b) && rb_valid(g_b);
    if (!g_b_on) rb_preset(prompts_control_type(), g_b);
    memcpy(g_b_start, g_b, 16);
    g_b_start_on = g_b_on;
    binds_sync();
}
static void page_go(int page, int focus_controls) {
    g_page = page;
    g_cap = -1;
    g_quit_ask = 0;
    if (page == 0 && focus_controls) {
        for (int i = 0; i < NROW; i++) if (k_rows[i].apply == A_PAGE && row_on(i)) { g_sel = i; return; }
    }
    g_sel = 0;
    while (g_sel < NROW - 1 && !row_on(g_sel)) g_sel++;
}
static void menu_init(void) {                              /* the first opening: the file's values = what runs */
    const GfxOptions *o = gfx_options();
    char v[64];
    for (int i = 0; i < NROW; i++) {
        const Row *r = &k_rows[i];
        if (r->apply == A_BIND) { g_shown[i] = 1; g_lock_why[i] = 0; continue; }
        int have = r->apply == A_SIZE || r->apply == A_PROMPTS || r->apply == A_MENU_SWAP || r->apply == A_LAYOUT || r->apply == A_PAGE || r->apply == A_QUIT || r->apply == A_PRESET || gfx_ini_get(r->key, v, sizeof v);
        if (r->apply == A_WINDOW) { static const char *const w[3] = {"windowed", "borderless", "fullscreen"}; snprintf(v, sizeof v, "%s", w[o->window_mode]); have = 1; }      /* [moonriver] what runs: Big Picture's borderless included (gfx_options.c) */
        else if (r->apply == A_SIZE) snprintf(v, sizeof v, "%dx%d", o->width, o->height);      /* MR_WINDOW included */
        else if (r->apply == A_PROMPTS) snprintf(v, sizeof v, "%s", nnpad_prompts_setting());
        else if (r->apply == A_MENU_SWAP) snprintf(v, sizeof v, "%s", nnpad_menu_swap_setting());
        else if (r->apply == A_LAYOUT) snprintf(v, sizeof v, "%s", nnpad_layout_setting());
        else if (r->apply == A_PAGE || r->apply == A_QUIT || r->apply == A_PRESET) snprintf(v, sizeof v, "%s", r->dflt);
        else if (!have) snprintf(v, sizeof v, "%s", r->dflt);
        g_shown[i] = !r->optional || have;
        if ((g_cur[i] = find_val(i, v)) < 0) snprintf(g_raw[i], sizeof g_raw[i], "%s", v);
        g_start[i] = g_cur[i];
        g_lock_why[i] = g_env_set[i] ? 1 : o->profile == 1 && (!strcmp(r->key, "vsync") || !strcmp(r->key, "resolution_scale")) ? 2 : 0;
    }
    binds_load();
    if (!row_on(g_sel)) page_go(0, 0);
    g_inited = 1;
}

/* ---------------------------------------------------------------- drawing (BGRA, on the CPU) */
/* Moon River look (matching launcher/ui/launcher.rcss and launcher/src/ui_render.cpp):
 *   - Night sky gradient (#030817 -> #071433 -> #0c2554 -> #081a3d) with bottom river band (#0a2149 -> #050f28 -> #030a1d)
 *   - Dimmed live astronomical moon phase + subtle horizontal river shimmer + slowly rotating celestial lunar ring
 *   - Right-side drawer sliding over the paused game with a soft dark scrim and glowing cyan (#6fe6ff) edge accent
 *   - Breadcrumb with the Moon River icon, Marcellus row titles with cyan focus glow, warm gold (#ffcf7a) changed/pending
 *     indicators, fading cyan description band, dark footer with outlined glowing button prompts
 *   - First-start shader preparation screen matching the launcher's Build screen (moon disc, rising fill, glowing cyan ring).
 * Laid out for 1280 x 720 and scaled by s = min(W / 1280, H / 720). */
static stbtt_fontinfo g_f_head, g_f_ui, g_f_uib;          /* Marcellus (titles & row names); Source Sans 3 Regular and SemiBold */
static uint8_t *g_px;                                      /* the panel (ground on GPU, full frame on CPU) */
static int g_pw, g_ph, g_rect[4], g_for_w, g_for_h;
/* A layer: the same size as the panel, premultiplied BGRA, with the box that was drawn in. While g_to is set, put() and
 * put_pre() draw into it instead of the panel; layer_over() composes it onto the panel. */
typedef struct { uint8_t *px; int n, x0, y0, x1, y1; } Layer;
static Layer *g_to;
static void layer_clear(Layer *l) {
    if (!l->px || l->n != g_pw * g_ph) { free(l->px); l->px = calloc((size_t)g_pw * g_ph, 4); l->n = g_pw * g_ph; }
    else memset(l->px, 0, (size_t)g_pw * g_ph * 4);
    l->x0 = g_pw; l->y0 = g_ph; l->x1 = l->y1 = 0;
}
static void layer_free(Layer *l) { free(l->px); l->px = NULL; l->n = 0; }
static void layer_over(const Layer *l) {
    if (!l->px) return;
    for (int j = l->y0; j < l->y1; j++) for (int i = l->x0; i < l->x1; i++) {
        const uint8_t *q = l->px + ((size_t)j * g_pw + i) * 4;
        if (!q[3]) continue;
        uint8_t *p = g_px + ((size_t)j * g_pw + i) * 4;
        float k = 1 - q[3] / 255.0f;
        int b = (int)(p[0] * k + q[0] + 0.5f), g = (int)(p[1] * k + q[1] + 0.5f), r = (int)(p[2] * k + q[2] + 0.5f);
        p[0] = (uint8_t)(b > 255 ? 255 : b); p[1] = (uint8_t)(g > 255 ? 255 : g); p[2] = (uint8_t)(r > 255 ? 255 : r);
    }
}
static void layer_blend(int x, int y, float b, float g, float r, float a) {   /* premultiplied source over the layer */
    Layer *l = g_to;
    uint8_t *p = l->px + ((size_t)y * g_pw + x) * 4;
    float k = 1 - a;
    int nb = (int)(p[0] * k + b + 0.5f), ng = (int)(p[1] * k + g + 0.5f), nr = (int)(p[2] * k + r + 0.5f), na = (int)(p[3] * k + a * 255 + 0.5f);
    p[0] = (uint8_t)(nb > 255 ? 255 : nb); p[1] = (uint8_t)(ng > 255 ? 255 : ng); p[2] = (uint8_t)(nr > 255 ? 255 : nr); p[3] = (uint8_t)(na > 255 ? 255 : na);
    if (x < l->x0) l->x0 = x; if (y < l->y0) l->y0 = y; if (x + 1 > l->x1) l->x1 = x + 1; if (y + 1 > l->y1) l->y1 = y + 1;
}
static unsigned g_drawn_ver = ~0u, g_gen;
static int g_row_y[NROW], g_row_h, g_row_x0[NROW], g_row_x1[NROW], g_val_x0[NROW], g_val_x1[NROW], g_close_r[4];
typedef struct { uint8_t b, g, r; } Rgb;                   /* BGRA byte order */
static const Rgb C_SKY0      = { 23,   8,   3},            /* #030817 */
                 C_SKY1      = { 51,  20,   7},            /* #071433 */
                 C_SKY2      = { 84,  37,  12},            /* #0c2554 */
                 C_SKY3      = { 61,  26,   8},            /* #081a3d */
                 C_RIV0      = { 73,  33,  10},            /* #0a2149 */
                 C_RIV1      = { 40,  15,   5},            /* #050f28 */
                 C_RIV2      = { 29,  10,   3},            /* #030a1d */
                 C_RIV_EDGE  = {255, 220, 160},            /* #a0dcff */
                 C_DISC      = { 66,  28,  11},            /* #0b1c42 */
                 C_WHITE     = {255, 255, 255},            /* #ffffff */
                 C_ICE       = {255, 252, 239},            /* #effcff */
                 C_INK       = {255, 247, 223},            /* #dff7ff */
                 C_BODY      = {246, 233, 207},            /* #cfe9f6 */
                 C_BAND_TXT  = {249, 238, 214},            /* #d6eef9 */
                 C_CRUMB     = {234, 211, 159},            /* #9fd3ea */
                 C_SUB       = {220, 201, 169},            /* #a9c9dc */
                 C_MUTE      = {204, 179, 143},            /* #8fb3cc */
                 C_DIM       = {153, 127,  95},            /* #5f7f99 */
                 C_CYAN      = {255, 230, 111},            /* #6fe6ff */
                 C_CYAN_MID  = {224, 182,  63},            /* #3fb6e0 */
                 C_BAND_GLOW = {180, 120,  40},            /* #2878b4 */
                 C_GOLD      = {122, 207, 255},            /* #ffcf7a */
                 C_ERR       = {189, 179, 255},            /* #ffb3bd */
                 C_ERR_DOT   = {122, 107, 255},            /* #ff6b7a */
                 C_FOOT      = { 16,   5,   1},            /* #010510 */
                 C_VIG       = { 12,   3,   0};            /* #00030c */

static int fonts_init(void) {
    return stbtt_InitFont(&g_f_head, k_font_head, stbtt_GetFontOffsetForIndex(k_font_head, 0)) &&
           stbtt_InitFont(&g_f_ui, k_font_ui, stbtt_GetFontOffsetForIndex(k_font_ui, 0)) &&
           stbtt_InitFont(&g_f_uib, k_font_uib, stbtt_GetFontOffsetForIndex(k_font_uib, 0));
}
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static float smoothstepf(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }
static float covf(float d) { return clampf(0.5f - d, 0, 1); }
static void put(int x, int y, Rgb c, float a) {
    if (x < 0 || y < 0 || x >= g_pw || y >= g_ph || a <= 0) return;
    if (a > 1) a = 1;
    if (g_to) { layer_blend(x, y, c.b * a, c.g * a, c.r * a, a); return; }
    uint8_t *p = g_px + ((size_t)y * g_pw + x) * 4;
    if (a >= 1) { p[0] = c.b; p[1] = c.g; p[2] = c.r; p[3] = 255; return; }
    p[0] = (uint8_t)(p[0] + (c.b - p[0]) * a + 0.5f);
    p[1] = (uint8_t)(p[1] + (c.g - p[1]) * a + 0.5f);
    p[2] = (uint8_t)(p[2] + (c.r - p[2]) * a + 0.5f);
}
static void put_pre(int x, int y, const float c[4], float a) {
    if (x < 0 || y < 0 || x >= g_pw || y >= g_ph || a <= 0) return;
    if (g_to) { layer_blend(x, y, c[0] * a, c[1] * a, c[2] * a, clampf(c[3] * a / 255.0f, 0, 1)); return; }
    uint8_t *p = g_px + ((size_t)y * g_pw + x) * 4;
    float k = 1 - clampf(c[3] * a / 255.0f, 0, 1);
    int b = (int)(p[0] * k + c[0] * a + 0.5f), g = (int)(p[1] * k + c[1] * a + 0.5f), r = (int)(p[2] * k + c[2] * a + 0.5f);
    p[0] = (uint8_t)(b > 255 ? 255 : b); p[1] = (uint8_t)(g > 255 ? 255 : g); p[2] = (uint8_t)(r > 255 ? 255 : r);
}
static Rgb mix(Rgb a, Rgb b, float t) {
    t = clampf(t, 0, 1);
    Rgb c = {(uint8_t)(a.b + (b.b - a.b) * t + 0.5f), (uint8_t)(a.g + (b.g - a.g) * t + 0.5f), (uint8_t)(a.r + (b.r - a.r) * t + 0.5f)};
    return c;
}
static void rect(int x, int y, int w, int h, Rgb c, float a) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > g_pw ? g_pw : x + w, y1 = y + h > g_ph ? g_ph : y + h;
    if (x1 <= x0 || y1 <= y0 || a <= 0) return;
    if (!g_to && a >= 1) {
        uint32_t v = (uint32_t)c.b | ((uint32_t)c.g << 8) | ((uint32_t)c.r << 16) | (255u << 24);
        for (int j = y0; j < y1; j++) {
            uint32_t *row = (uint32_t *)(g_px + (size_t)j * g_pw * 4);
            for (int i = x0; i < x1; i++) row[i] = v;
        }
        return;
    }
    for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) put(i, j, c, a);
}
static void hgrad_a(int x, int y, int w, int h, Rgb a, Rgb b, float a0, float a1) {
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < w; i++) {
        float t = (float)i / (w > 1 ? w - 1 : 1);
        rect(x + i, y, 1, h, mix(a, b, t), a0 + (a1 - a0) * t);
    }
}
static void hfade_center(int x, int y, int w, int h, Rgb c, float peak_a) {      /* gen:fade?d=c: 0 -> peak_a -> 0 */
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < w; i++) {
        float u = (i + 0.5f) / w;
        float a = (1.0f - fabsf(u * 2.0f - 1.0f)) * peak_a;
        if (a > 0.002f) rect(x + i, y, 1, h, c, a);
    }
}
static void vgrad_a(int x, int y, int w, int h, Rgb c0, Rgb c1, float a0, float a1) {
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        float t = (float)j / (h > 1 ? h - 1 : 1);
        rect(x, y + j, w, 1, mix(c0, c1, t), a0 + (a1 - a0) * t);
    }
}
static void diamond(float cx, float cy, float r, Rgb c, float glow_a) {      /* rotated 45-deg diamond marker (launcher .ck .d) */
    int pad = (int)ceilf(r * 2.4f) + 2;
    for (int y = (int)floorf(cy) - pad; y <= (int)ceilf(cy) + pad; y++)
        for (int x = (int)floorf(cx) - pad; x <= (int)ceilf(cx) + pad; x++) {
            float d = fabsf(x + 0.5f - cx) + fabsf(y + 0.5f - cy) - r;
            if (glow_a > 0 && d > 0 && d < r * 1.8f) {
                float ga = expf(-(d * d) / (r * r * 0.9f)) * glow_a;
                if (ga > 0.005f) put(x, y, c, ga);
            }
            float a = covf(d * 0.70710678f);
            if (a > 0) put(x, y, c, a);
        }
}

/* ---- art: embedded 32-bit premultiplied BGRA icon (runtime/art/icon.bmp) + procedural GPU textures ---- */
static const unsigned char k_art_icon[] = {
#embed "art/icon.bmp"
};
typedef struct { int w, h; const uint8_t *px; int bottom_up; } Art32;      /* premultiplied BGRA */
static Art32 g_icon;
static uint8_t *g_ring_px;                                  /* 512x512 premultiplied BGRA celestial lunar ring */
static Art32 g_ring;

static Art32 art32_bmp(const unsigned char *b, size_t n) {
    Art32 a = {0};
    if (n < 54 || b[0] != 'B' || b[1] != 'M') return a;
    uint32_t off = b[10] | (b[11] << 8) | (b[12] << 16) | ((uint32_t)b[13] << 24);
    int w = (int)(b[18] | (b[19] << 8) | (b[20] << 16) | ((uint32_t)b[21] << 24));
    int h = (int)(b[22] | (b[23] << 8) | (b[24] << 16) | ((uint32_t)b[25] << 24));
    uint16_t bpp = (uint16_t)(b[28] | (b[29] << 8));
    if (bpp != 32 || w <= 0 || h == 0) return a;
    a.bottom_up = h > 0; if (h < 0) h = -h;
    if (off + (size_t)w * (size_t)h * 4 > n) return a;
    a.w = w; a.h = h; a.px = b + off;
    return a;
}
static const uint8_t k_clear[4];
static inline const uint8_t *art32_entry(const Art32 *a, int x, int y) {
    if (x < 0 || y < 0 || x >= a->w || y >= a->h) return k_clear;
    int ry = a->bottom_up ? (a->h - 1 - y) : y;
    return a->px + ((size_t)ry * a->w + x) * 4;
}
static inline void art32_sample(const Art32 *a, float u, float v, float out[4]) {
    float fu = u - 0.5f, fv = v - 0.5f;
    int x = (int)floorf(fu), y = (int)floorf(fv);
    float fx = fu - x, fy = fv - y;
    const uint8_t *e00 = art32_entry(a, x, y), *e10 = art32_entry(a, x + 1, y), *e01 = art32_entry(a, x, y + 1), *e11 = art32_entry(a, x + 1, y + 1);
    if (!(e00[3] | e10[3] | e01[3] | e11[3])) { out[3] = 0; return; }
    float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
    for (int k = 0; k < 4; k++) out[k] = e00[k] * w00 + e10[k] * w10 + e01[k] * w01 + e11[k] * w11;
}

/* Procedural 512x512 celestial lunar dial ring (inspired by icon.png's outer frame: double thin rings, 4 cardinal phase marks, star ticks) */
static void ring_over(uint8_t *px, int N, int x, int y, Rgb c, float a) {
    if (x < 0 || y < 0 || x >= N || y >= N || a <= 0) return;
    if (a > 1) a = 1;
    uint8_t *p = px + ((size_t)y * N + x) * 4;
    float k = 1 - a;
    int b = (int)(p[0] * k + c.b * a + 0.5f), g = (int)(p[1] * k + c.g * a + 0.5f), r = (int)(p[2] * k + c.r * a + 0.5f), al = (int)(p[3] * k + 255 * a + 0.5f);
    p[0] = (uint8_t)(b > 255 ? 255 : b); p[1] = (uint8_t)(g > 255 ? 255 : g); p[2] = (uint8_t)(r > 255 ? 255 : r); p[3] = (uint8_t)(al > 255 ? 255 : al);
}
static void art_ring_build(void) {
    if (g_ring_px) return;
    const int N = 512;
    g_ring_px = calloc((size_t)N * N, 4);
    if (!g_ring_px) return;
    const float c = N * 0.5f;
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        float dx = x + 0.5f - c, dy = y + 0.5f - c, r = sqrtf(dx * dx + dy * dy);
        if (r < 200 || r > 254) continue;
        float d1 = fabsf(r - 242.0f), d2 = fabsf(r - 226.0f), d3 = fabsf(r - 214.0f);
        if (d1 < 8.0f) ring_over(g_ring_px, N, x, y, C_CYAN, expf(-(d1 * d1) / 14.0f) * 0.30f);
        float a1 = covf(d1 - 1.3f), a2 = covf(d2 - 1.0f), a3 = covf(d3 - 0.65f);
        if (a1 > 0) ring_over(g_ring_px, N, x, y, C_ICE, a1 * 0.90f);
        if (a2 > 0) ring_over(g_ring_px, N, x, y, C_CYAN, a2 * 0.72f);
        if (a3 > 0) ring_over(g_ring_px, N, x, y, C_CYAN_MID, a3 * 0.45f);
    }
    /* 12 dial marks between r=226 and r=242 (4 cardinal lunar phase markers + 8 small star dots) */
    for (int k = 0; k < 12; k++) {
        float ang = k * (3.14159265f / 6.0f) - 1.57079633f;
        float mx = c + cosf(ang) * 234.0f, my = c + sinf(ang) * 234.0f;
        int cardinal = (k % 3) == 0;
        float rad = cardinal ? 5.6f : 2.3f;
        int x0 = (int)floorf(mx - rad - 4), y0 = (int)floorf(my - rad - 4), x1 = (int)ceilf(mx + rad + 4), y1 = (int)ceilf(my + rad + 4);
        for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
            float ux = x + 0.5f - mx, uy = y + 0.5f - my, d = sqrtf(ux * ux + uy * uy);
            if (d < rad + 4) ring_over(g_ring_px, N, x, y, C_CYAN, expf(-(d * d) / (rad * rad * 0.8f)) * (cardinal ? 0.35f : 0.25f));
            if (cardinal) {
                float rim = covf(fabsf(d - rad) - 0.85f);
                if (rim > 0) ring_over(g_ring_px, N, x, y, C_ICE, rim * 0.95f);
                float inside = covf(d - (rad - 0.7f));
                if (inside > 0) {
                    int fill = k == 0 ? 1 : k == 6 ? 0 : (k == 3 ? ux >= 0 : ux <= 0);
                    if (fill) ring_over(g_ring_px, N, x, y, C_ICE, inside * 0.92f);
                }
            } else {
                float dot = covf(d - rad);
                if (dot > 0) ring_over(g_ring_px, N, x, y, C_ICE, dot * 0.88f);
            }
        }
    }
    g_ring.w = N; g_ring.h = N; g_ring.px = g_ring_px; g_ring.bottom_up = 0;
}
static void art_init(void) {
    if (!g_icon.w) g_icon = art32_bmp(k_art_icon, sizeof k_art_icon);
    if (!g_ring.w) art_ring_build();
}

#define NOCLIP (-1e9f)
#define CLIPPED(c) ((c) > -1e8f)
/* GPU sprite struct (shared layout with nvn_vk_pcmenu.c) */
typedef struct {
    int art;                                               /* 0 scrim, 1 outer shadow+glow, 2 celestial ring; -1 hubs, -2 words, -3 ground */
    float x, y, w, h, deg, alpha, ann0, ann1;
    float clip_top; int clip_right;
    float clip2_top; int clip2_right;
    float y0, y1;
} PcmSprite;
static PcmSprite *g_emit; static int g_emit_n, g_emit_max, g_gpu;

static void art32_blit(const Art32 *a, float x, float y, float w, float h, float alpha, float deg, float clip_x, int clip_right) {
    if (!a->w || w <= 0 || h <= 0 || alpha <= 0) return;
    float cx = x + w * 0.5f, cy = y + h * 0.5f, cs = cosf(deg * 3.14159265f / 180.0f), sn = sinf(deg * 3.14159265f / 180.0f);
    float sx = a->w / w, sy = a->h / h;
    float hw = w * 0.5f, hh = h * 0.5f, bx = fabsf(hw * cs) + fabsf(hh * sn), by = fabsf(hw * sn) + fabsf(hh * cs);
    int x0 = (int)floorf(cx - bx) - 1, y0 = (int)floorf(cy - by) - 1, x1 = (int)ceilf(cx + bx) + 1, y1 = (int)ceilf(cy + by) + 1;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > g_pw) x1 = g_pw; if (y1 > g_ph) y1 = g_ph;
    if (CLIPPED(clip_x)) { int e = (int)floorf(clip_x); if (clip_right) { if (e > x0) x0 = e; } else if (e < x1) x1 = e; }
    for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) {
        float dx = i + 0.5f - cx, dy = j + 0.5f - cy;
        float u = (dx * cs + dy * sn + hw) * sx, v = (-dx * sn + dy * cs + hh) * sy, c[4];
        if (u < 0 || v < 0 || u >= a->w || v >= a->h) continue;
        art32_sample(a, u, v, c);
        if (c[3] > 0) put_pre(i, j, c, alpha);
    }
}
/* Procedural GPU textures for nvn_vk_pcmenu.c (premultiplied BGRA, top row first, malloc'd) */
void *pcmenu_art_pixels(int id, int *w, int *h) {
    art_init();
    if (id == 0) {                                         /* 1x1 dark night scrim (#020612) */
        uint8_t *px = malloc(4);
        if (px) { px[0] = 18; px[1] = 6; px[2] = 2; px[3] = 255; *w = *h = 1; }
        return px;
    }
    if (id == 1) {                                         /* 64x1 outer drop shadow + soft cyan glow right up to the drawer edge */
        const int W = 64;
        uint8_t *px = calloc((size_t)W, 4);
        if (!px) return NULL;
        for (int i = 0; i < W; i++) {
            float u = (i + 0.5f) / W;                      /* 0 at far left .. 1 at drawer border */
            float sh = u * u * 0.56f;                      /* smooth dark drop shadow */
            float d = (1.0f - u) * W;                      /* pixels from border */
            float gl = d < 18.0f ? expf(-(d * d) / 48.0f) * 0.34f : 0.0f;
            float b = 12.0f * sh * (1.0f - gl) + C_CYAN.b * gl;
            float g =  4.0f * sh * (1.0f - gl) + C_CYAN.g * gl;
            float r =  1.0f * sh * (1.0f - gl) + C_CYAN.r * gl;
            float a = sh + gl * (1.0f - sh);
            px[i * 4 + 0] = (uint8_t)clampf(b + 0.5f, 0, 255);
            px[i * 4 + 1] = (uint8_t)clampf(g + 0.5f, 0, 255);
            px[i * 4 + 2] = (uint8_t)clampf(r + 0.5f, 0, 255);
            px[i * 4 + 3] = (uint8_t)clampf(a * 255.0f + 0.5f, 0, 255);
        }
        *w = W; *h = 1;
        return px;
    }
    if (id == 2 && g_ring.w && g_ring_px) {                /* 512x512 rotating celestial lunar ring */
        size_t sz = (size_t)g_ring.w * g_ring.h * 4;
        uint8_t *px = malloc(sz);
        if (px) { memcpy(px, g_ring_px, sz); *w = g_ring.w; *h = g_ring.h; }
        return px;
    }
    return NULL;
}
static double anim_t(void) { return (double)(GetTickCount64() % 3600000ull) / 1000.0; }

/* ---- Moon River procedural backdrop: sky, river, live moon phase, river shimmer, vignette ---- */
static double moon_phase_now(void) {
    const char *env = getenv("MR_MOON_PHASE");
    if (env && env[0]) { double p = fmod(atof(env), 1.0); return p < 0 ? p + 1.0 : p; }
    const double SYNODIC = 29.530588853, REF_NEW = 947182440.0;
    double p = fmod(((double)time(NULL) - REF_NEW) / 86400.0 / SYNODIC, 1.0);
    return p < 0.0 ? p + 1.0 : p;
}
static void draw_sky_river(int x0, int x1, int H, float s, int river_h) {
    if (x0 < 0) x0 = 0; if (x1 > g_pw) x1 = g_pw;
    if (x1 <= x0) return;
    int ry = H - river_h;
    for (int y = 0; y < H; y++) {
        Rgb c;
        if (y < ry) {
            float t = (y + 0.5f) / H;
            c = t < 0.45f ? mix(C_SKY0, C_SKY1, t / 0.45f)
              : t < 0.72f ? mix(C_SKY1, C_SKY2, (t - 0.45f) / 0.27f)
              : mix(C_SKY2, C_SKY3, (t - 0.72f) / 0.28f);
        } else {
            float rt = (y - ry + 0.5f) / (river_h > 0 ? river_h : 1);
            c = rt < 0.70f ? mix(C_RIV0, C_RIV1, rt / 0.70f)
              : mix(C_RIV1, C_RIV2, (rt - 0.70f) / 0.30f);
        }
        rect(x0, y, x1 - x0, 1, c, 1);
    }
    int eh = (int)(1 * s + 0.5f); if (eh < 1) eh = 1;
    rect(x0, ry, x1 - x0, eh, C_RIV_EDGE, 0.16f);
}
static void draw_moon_phase(float cx, float cy, float R, float R_halo, float overall, int clip_x0, float *lit_out) {
    float phase = (float)moon_phase_now();
    float cs = cosf(2.0f * 3.14159265f * phase);
    float lit = (1.0f - cs) * 0.5f;
    if (lit_out) *lit_out = lit;
    int waxing = phase < 0.5f;
    float halo_mul = 0.25f + 0.75f * lit;
    static const float craters[4][4] = {
        { 23.0f / 95.0f,  15.0f / 95.0f, 17.0f / 95.0f, 0.35f},
        {-30.0f / 95.0f,  30.0f / 95.0f, 11.0f / 95.0f, 0.25f},
        { 10.0f / 95.0f, -40.0f / 95.0f,  9.0f / 95.0f, 0.22f},
        {-45.0f / 95.0f, -15.0f / 95.0f,  7.0f / 95.0f, 0.18f},
    };
    int x0 = (int)floorf(cx - R_halo), y0 = (int)floorf(cy - R_halo);
    int x1 = (int)ceilf(cx + R_halo), y1 = (int)ceilf(cy + R_halo);
    if (x0 < clip_x0) x0 = clip_x0; if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > g_pw) x1 = g_pw; if (y1 > g_ph) y1 = g_ph;
    for (int y = y0; y < y1; y++) {
        float dy = y + 0.5f - cy;
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, r = sqrtf(dx * dx + dy * dy);
            if (r >= R_halo) continue;
            float mb = 0, mg = 0, mr = 0, ma = 0;           /* premultiplied local composite */
            /* 1. soft blue-white halo */
            float rn = r / R_halo, ht = rn <= 0.45f ? 0.0f : (rn - 0.45f) / 0.55f;
            float hr = 170.0f + (90.0f - 170.0f) * ht, hg = 215.0f + (160.0f - 215.0f) * ht, hb = 255.0f;
            float ha = (1.0f - ht) * 0.28f * halo_mul;
            mb = hb * ha; mg = hg * ha; mr = hr * ha; ma = ha;
            /* 2. unlit disc + subtle rim + lit crescent/gibbous face */
            if (r < R + 1.5f) {
                float dcov = covf(r - R);
                if (dcov > 0) {
                    float a = 0.55f * dcov, k = 1 - a;
                    mb = mb * k + 74 * a; mg = mg * k + 37 * a; mr = mr * k + 19 * a; ma = ma * k + a;
                }
                float rcov = covf(fabsf(r - R) - 0.55f);
                if (rcov > 0) {
                    float a = 0.16f * rcov, k = 1 - a;
                    mb = mb * k + 255 * a; mg = mg * k + 215 * a; mr = mr * k + 170 * a; ma = ma * k + a;
                }
                if (dcov > 0) {
                    float w = sqrtf(R * R > dy * dy ? R * R - dy * dy : 0);
                    float xt = (waxing ? cs : -cs) * w;
                    float sx = waxing ? (dx - xt) : (xt - dx);
                    float slope = w > 1.0f ? (fabsf(cs) * fabsf(dy) / w) : 4.0f;
                    float lcov = dcov * clampf(0.5f + sx / sqrtf(1.0f + slope * slope), 0, 1);
                    if (lcov > 0) {
                        float gt = clampf(sqrtf((dx + 0.24f * R) * (dx + 0.24f * R) + (dy + 0.28f * R) * (dy + 0.28f * R)) / (1.4f * R), 0, 1);
                        float fr, fg, fb;
                        if (gt < 0.55f) { float u = gt / 0.55f; fr = 251 + (224 - 251) * u; fg = 253 + (236 - 253) * u; fb = 255 + (251 - 255) * u; }
                        else { float u = (gt - 0.55f) / 0.45f; fr = 224 + (169 - 224) * u; fg = 236 + (192 - 236) * u; fb = 251 + (224 - 251) * u; }
                        float nx = dx / R, ny = dy / R;
                        for (int c = 0; c < 4; c++) {
                            float cd = (sqrtf((nx - craters[c][0]) * (nx - craters[c][0]) + (ny - craters[c][1]) * (ny - craters[c][1])) - craters[c][2]) * R;
                            float ca = covf(cd) * craters[c][3];
                            if (ca > 0) { fr = fr * (1 - ca) + 120 * ca; fg = fg * (1 - ca) + 150 * ca; fb = fb * (1 - ca) + 190 * ca; }
                        }
                        float k = 1 - lcov;
                        mb = mb * k + fb * lcov; mg = mg * k + fg * lcov; mr = mr * k + fr * lcov; ma = ma * k + lcov;
                    }
                }
            }
            if (ma > 0.001f) {
                float col[4] = {mb, mg, mr, ma * 255.0f};
                put_pre(x, y, col, overall);
            }
        }
    }
}
static void draw_shimmer(float cx, int river_y, float rw, float rh, float s, float lit, float overall, int clip_x0) {
    int x0 = (int)floorf(cx - rw * 0.5f), x1 = (int)ceilf(cx + rw * 0.5f);
    int y0 = river_y + 2, y1 = (int)ceilf(river_y + rh);
    if (x0 < clip_x0) x0 = clip_x0; if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > g_pw) x1 = g_pw; if (y1 > g_ph) y1 = g_ph;
    float mul = overall * (0.35f + 0.65f * lit);
    Rgb c = {255, 240, 220};
    for (int y = y0; y < y1; y++) {
        float y_dp = (y - river_y + 0.5f) / s;
        float mod = fmodf(y_dp, 9.0f);
        float stripe = mod < 6.5f ? 0 : mod < 7.5f ? (mod - 6.5f) : mod <= 8.5f ? 1.0f : clampf(9.5f - mod, 0, 1);
        if (stripe <= 0) continue;
        float v = (y - river_y + 0.5f) / rh;
        for (int x = x0; x < x1; x++) {
            float u = (x + 0.5f - cx) / rw;
            float d = sqrtf((u / 0.5f) * (u / 0.5f) + (v / 0.5f) * (v / 0.5f));
            float mask = 1.0f - smoothstepf(0.20f, 0.78f, d);
            float a = stripe * 0.55f * mask * mul;
            if (a > 0.004f) put(x, y, c, a);
        }
    }
}
static void draw_vignette(int x0, int x1, int H, float strength) {
    if (x0 < 0) x0 = 0; if (x1 > g_pw) x1 = g_pw;
    float w = (float)(x1 - x0);
    if (w <= 1 || H <= 1) return;
    for (int y = 0; y < H; y++) {
        float dy = ((y + 0.5f) / H - 0.45f) / 0.70f, dy2 = dy * dy;
        for (int x = x0; x < x1; x++) {
            float dx = ((x - x0 + 0.5f) / w - 0.50f) / 0.75f;
            float d = sqrtf(dx * dx + dy2);
            if (d > 0.55f) put(x, y, C_VIG, clampf((d - 0.55f) / 0.45f, 0, 1) * strength);
        }
    }
}

/* ---- text (with fast separable cyan glow helper matching launcher font-effect: blur) ---- */
static int utf8(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p++;
    if (c >= 0xF0) { c = (c & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63); p += 3; }
    else if (c >= 0xE0) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
    else if (c >= 0xC0) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
    *s = (const char *)p;
    return c;
}
static float text_w(const stbtt_fontinfo *f, const char *s, float px, float track) {
    float sc = stbtt_ScaleForPixelHeight(f, px), w = 0;
    int prev = 0, adv, lsb;
    while (*s) { int c = utf8(&s); if (prev) w += sc * stbtt_GetCodepointKernAdvance(f, prev, c); stbtt_GetCodepointHMetrics(f, c, &adv, &lsb); w += adv * sc + track; prev = c; }
    return w - (prev ? track : 0);
}
static float text_a(const stbtt_fontinfo *f, float x, int base, const char *s, float px, Rgb col, float track, float alpha) {
    static uint8_t buf[384 * 384];
    float sc = stbtt_ScaleForPixelHeight(f, px);
    int prev = 0, adv, lsb;
    while (*s) {
        int c = utf8(&s), x0, y0, x1, y1;
        if (prev) x += sc * stbtt_GetCodepointKernAdvance(f, prev, c);
        float fx = x - floorf(x);
        stbtt_GetCodepointBitmapBoxSubpixel(f, c, sc, sc, fx, 0, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        if (w > 0 && h > 0 && w <= 384 && h <= 384) {
            stbtt_MakeCodepointBitmapSubpixel(f, buf, w, h, w, sc, sc, fx, 0, c);
            for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) if (buf[j * w + i]) put((int)floorf(x) + x0 + i, base + y0 + j, col, buf[j * w + i] / 255.0f * alpha);
        }
        stbtt_GetCodepointHMetrics(f, c, &adv, &lsb);
        x += adv * sc + track;
        prev = c;
    }
    return x;
}
static float text(const stbtt_fontinfo *f, float x, int base, const char *s, float px, Rgb col, float track) { return text_a(f, x, base, s, px, col, track, 1); }

/* Fast O(W*H) separable running-sum box blur on an 8-bit alpha mask (two passes approximate a smooth Gaussian glow) */
static void blur_alpha(uint8_t *src, uint8_t *tmp, int w, int h, int rad) {
    if (rad <= 0 || w <= 0 || h <= 0) return;
    int diam = rad * 2 + 1;
    for (int y = 0; y < h; y++) {
        const uint8_t *r = src + (size_t)y * w;
        uint8_t *d = tmp + (size_t)y * w;
        int sum = r[0] * (rad + 1);
        for (int k = 1; k <= rad; k++) sum += r[k < w ? k : w - 1];
        for (int x = 0; x < w; x++) {
            d[x] = (uint8_t)((sum + rad) / diam);
            int xr = x + rad + 1, xl = x - rad;
            sum += r[xr < w ? xr : w - 1] - r[xl >= 0 ? xl : 0];
        }
    }
    for (int x = 0; x < w; x++) {
        int sum = tmp[x] * (rad + 1);
        for (int k = 1; k <= rad; k++) sum += tmp[(size_t)(k < h ? k : h - 1) * w + x];
        for (int y = 0; y < h; y++) {
            src[(size_t)y * w + x] = (uint8_t)((sum + rad) / diam);
            int yd = y + rad + 1, yu = y - rad;
            sum += tmp[(size_t)(yd < h ? yd : h - 1) * w + x] - tmp[(size_t)(yu >= 0 ? yu : 0) * w + x];
        }
    }
}
static float text_glow(const stbtt_fontinfo *f, float x, int base, const char *s, float px, Rgb col, Rgb glow_c, float glow_rad, float glow_a, float track) {
    int rad = (int)(glow_rad + 0.5f);
    if (rad < 1) rad = 1;
    if (rad > 24) rad = 24;
    int pad = rad * 2 + 2;
    float tw = text_w(f, s, px, track);
    int bw = (int)ceilf(tw) + pad * 2 + 8, bh = (int)ceilf(px * 1.45f) + pad * 2 + 8;
    int ox = (int)floorf(x) - pad - 2, oy = base - (int)ceilf(px * 1.05f) - pad - 2;
    if (bw > 0 && bh > 0 && bw <= 2400 && bh <= 600) {
        size_t sz = (size_t)bw * bh;
        uint8_t *m = calloc(sz, 1), *tmp = malloc(sz);
        if (m && tmp) {
            static uint8_t gbuf[384 * 384];
            float sc = stbtt_ScaleForPixelHeight(f, px), pen = x;
            int prev = 0, adv, lsb;
            const char *p = s;
            while (*p) {
                int c = utf8(&p), x0, y0, x1, y1;
                if (prev) pen += sc * stbtt_GetCodepointKernAdvance(f, prev, c);
                float fx = pen - floorf(pen);
                stbtt_GetCodepointBitmapBoxSubpixel(f, c, sc, sc, fx, 0, &x0, &y0, &x1, &y1);
                int gw = x1 - x0, gh = y1 - y0;
                if (gw > 0 && gh > 0 && gw <= 384 && gh <= 384) {
                    stbtt_MakeCodepointBitmapSubpixel(f, gbuf, gw, gh, gw, sc, sc, fx, 0, c);
                    int gx = (int)floorf(pen) + x0 - ox, gy = base + y0 - oy;
                    for (int j = 0; j < gh; j++) {
                        int dy = gy + j; if (dy < 0 || dy >= bh) continue;
                        for (int i = 0; i < gw; i++) {
                            int dx = gx + i; if (dx < 0 || dx >= bw) continue;
                            int v = m[(size_t)dy * bw + dx] + gbuf[j * gw + i];
                            m[(size_t)dy * bw + dx] = (uint8_t)(v > 255 ? 255 : v);
                        }
                    }
                }
                stbtt_GetCodepointHMetrics(f, c, &adv, &lsb);
                pen += adv * sc + track;
                prev = c;
            }
            blur_alpha(m, tmp, bw, bh, rad);
            blur_alpha(m, tmp, bw, bh, rad);
            for (int j = 0; j < bh; j++) for (int i = 0; i < bw; i++) {
                uint8_t v = m[(size_t)j * bw + i];
                if (v) put(ox + i, oy + j, glow_c, (v / 255.0f) * glow_a);
            }
        }
        free(m); free(tmp);
    }
    return text(f, x, base, s, px, col, track);
}
static int wrap(const stbtt_fontinfo *f, char lines[][160], int maxl, const char *s, float px, float track, float width) {
    int n = 0;
    while (*s && n < maxl) {
        char line[160] = "";
        const char *best = s;
        for (const char *p = s; ; p++) {
            if (*p == ' ' || !*p) {
                char t[160]; size_t len = (size_t)(p - s) < sizeof t - 1 ? (size_t)(p - s) : sizeof t - 1;
                memcpy(t, s, len); t[len] = 0;
                if (text_w(f, t, px, track) > width && best != s) break;
                memcpy(line, t, len + 1); best = p;
                if (!*p) break;
            }
        }
        snprintf(lines[n++], 160, "%s", line);
        s = *best ? best + 1 : best;
    }
    return n;
}
static const char *pad_name(int b) {
    static const char *const sw[16] = {"A", "B", "X", "Y", "Left stick press", "Right stick press", "L", "R", "ZL", "ZR", "Plus", "Minus", "D-pad left", "D-pad up", "D-pad right", "D-pad down"};
    static const char *const face[2][2][4] = {{{"Circle", "Cross", "Triangle", "Square"}, {"Cross", "Circle", "Square", "Triangle"}}, {{"B", "A", "Y", "X"}, {"A", "B", "X", "Y"}}};
    static const char *const ps[6] = {"L1", "R1", "L2", "R2", "Options", "Create"}, *const xb[6] = {"LB", "RB", "LT", "RT", "Menu", "View"};
    if (g_fam != 2 && g_fam != 3) return sw[b];
    if (b < 4) return face[g_fam == 3][g_letter][b];
    if (b >= 6 && b <= 11) return (g_fam == 3 ? xb : ps)[b - 6];
    return sw[b];
}
static const char *val_name(int i) {
    if (i == g_cap) return "Press a button\xE2\x80\xA6";
    if (k_rows[i].apply == A_BIND) return pad_name(g_cur[i] >= 0 && g_cur[i] < 16 ? g_cur[i] : 0);
    if (k_rows[i].apply == A_PRESET && g_cur[i] == 0) {
        static const char *const gn[4] = {"Game (Type A)", "Game (Type B)", "Game (Type C)", "Game (Type D)"};
        int ct = prompts_control_type();
        return gn[ct >= 0 && ct < 4 ? ct : 0];
    }
    if (k_rows[i].apply == A_PAGE) return g_b_on ? "Custom  \xE2\x80\xBA" : "Open  \xE2\x80\xBA";
    if (k_rows[i].apply == A_QUIT && g_quit_ask) return "Press again to quit";
    return g_cur[i] >= 0 ? k_rows[i].name[g_cur[i]] : g_raw[i];
}
static int row_restart(int i) {
    if (k_rows[i].apply == A_PAGE || k_rows[i].apply == A_QUIT || k_rows[i].apply == A_PRESET || k_rows[i].apply == A_BIND) return 0;
    if (g_cur[i] == g_start[i]) return 0;
    if (!strcmp(k_rows[i].key, "aspect")) return (g_cur[i] >= 0 && !strcmp(k_rows[i].val[g_cur[i]], "wide")) || (g_start[i] >= 0 && !strcmp(k_rows[i].val[g_start[i]], "wide"));
    return k_rows[i].apply == A_RESTART || (k_rows[i].apply == A_SCALE && g_open) || ((k_rows[i].apply == A_WINDOW || k_rows[i].apply == A_SIZE) && g_nowin);
}

/* Outlined glowing button prompt matching launcher/src/ui_render.cpp gen:glyph: bright #eafaff outline + inner symbol
 * with soft #6fe6ff cyan halo behind it, followed by the label in #e3f5ff. Returns total width; draw=0 measures only. */
static float hint_btn(float x, int base, float px, int b, const char *what, int draw) {
    float r = px * 0.62f, cy = base - px * 0.34f, stroke = px * 0.095f;
    if (stroke < 1.25f) stroke = 1.25f;
    float gw = 2 * r, gh = 2 * r;
    const char *key = b ? "ESC" : "ENTER";
    float kpx = px * 0.50f;
    if (g_fam == 4) {
        gw = text_w(&g_f_uib, key, kpx, 0.8f * px / 16.0f) + px * 0.72f;
        gh = 2 * r * 0.88f;
    }
    if (draw) {
        int rad = (int)(px * 0.22f + 0.5f); if (rad < 2) rad = 2; if (rad > 10) rad = 10;
        int pad = rad * 2 + 4;
        int bw = (int)ceilf(gw) + pad * 2, bh = (int)ceilf(gh) + pad * 2;
        int ox = (int)floorf(x) - pad, oy = (int)floorf(cy - gh * 0.5f) - pad;
        float lx = x - ox, ly = cy - oy, lcx = lx + gw * 0.5f;
        size_t sz = (size_t)bw * bh;
        uint8_t *fg = calloc(sz, 1), *halo = calloc(sz, 1), *tmp = malloc(sz);
        if (fg && halo && tmp) {
            if (g_fam == 4) {                              /* outlined rounded keycap (ENTER / ESC) */
                float rr = px * 0.22f, hx = gw * 0.5f - rr, hy = gh * 0.5f - rr;
                for (int j = 0; j < bh; j++) for (int i = 0; i < bw; i++) {
                    float dx = fabsf(i + 0.5f - lcx) - hx, dy = fabsf(j + 0.5f - ly) - hy;
                    float ox0 = dx > 0 ? dx : 0, oy0 = dy > 0 ? dy : 0, in = dx > dy ? dx : dy;
                    float d = sqrtf(ox0 * ox0 + oy0 * oy0) + (in < 0 ? in : 0) - rr;
                    float sh = covf(d), ring = covf(fabsf(d + stroke * 0.5f) - stroke * 0.5f);
                    if (sh > 0) halo[(size_t)j * bw + i] = (uint8_t)(sh * 115.0f + 0.5f);
                    if (ring > 0) fg[(size_t)j * bw + i] = (uint8_t)(ring * 255.0f + 0.5f);
                }
            } else {                                       /* outlined face-button ring (Switch / Xbox / PlayStation) */
                for (int j = 0; j < bh; j++) for (int i = 0; i < bw; i++) {
                    float dx = i + 0.5f - lcx, dy = j + 0.5f - ly, d = sqrtf(dx * dx + dy * dy) - r;
                    float sh = covf(d), ring = covf(fabsf(d + stroke * 0.5f) - stroke * 0.5f);
                    if (sh > 0) halo[(size_t)j * bw + i] = (uint8_t)(sh * 115.0f + 0.5f);
                    if (ring > 0) fg[(size_t)j * bw + i] = (uint8_t)(ring * 255.0f + 0.5f);
                }
                if (g_fam == 2) {                          /* PlayStation Cross / Circle inside the ring */
                    int cross = (b == 0) == (g_letter != 0);
                    if (cross) {
                        float L = r * 0.42f, sw = stroke * 0.55f;
                        for (int j = 0; j < bh; j++) for (int i = 0; i < bw; i++) {
                            float dx = i + 0.5f - lcx, dy = j + 0.5f - ly;
                            float u = fabsf(dx + dy) * 0.70710678f, v = fabsf(dx - dy) * 0.70710678f;
                            float d1 = u > sw ? u - sw : 0, e1 = v > L ? v - L : 0, d2 = v > sw ? v - sw : 0, e2 = u > L ? u - L : 0;
                            float d = sqrtf(d1 * d1 + e1 * e1), f = sqrtf(d2 * d2 + e2 * e2);
                            if (f < d) d = f;
                            float a = covf(d - 0.35f);
                            if (a > 0) {
                                int cur = fg[(size_t)j * bw + i], nv = (int)(a * 255.0f + 0.5f);
                                if (nv > cur) fg[(size_t)j * bw + i] = (uint8_t)nv;
                            }
                        }
                    } else {
                        float cr = r * 0.46f, sw = stroke * 0.55f;
                        for (int j = 0; j < bh; j++) for (int i = 0; i < bw; i++) {
                            float dx = i + 0.5f - lcx, dy = j + 0.5f - ly;
                            float a = covf(fabsf(sqrtf(dx * dx + dy * dy) - cr) - sw);
                            if (a > 0) {
                                int cur = fg[(size_t)j * bw + i], nv = (int)(a * 255.0f + 0.5f);
                                if (nv > cur) fg[(size_t)j * bw + i] = (uint8_t)nv;
                            }
                        }
                    }
                }
            }
            blur_alpha(halo, tmp, bw, bh, rad);
            blur_alpha(halo, tmp, bw, bh, rad);
            for (int j = 0; j < bh; j++) for (int i = 0; i < bw; i++) {
                if (halo[(size_t)j * bw + i]) put(ox + i, oy + j, C_CYAN, halo[(size_t)j * bw + i] / 255.0f);
                if (fg[(size_t)j * bw + i]) put(ox + i, oy + j, C_ICE, fg[(size_t)j * bw + i] / 255.0f);
            }
        }
        free(fg); free(halo); free(tmp);
        if (g_fam == 4) {
            float lw = text_w(&g_f_uib, key, kpx, 0.8f * px / 16.0f);
            text(&g_f_uib, x + (gw - lw) * 0.5f, (int)(cy + kpx * 0.35f), key, kpx, C_ICE, 0.8f * px / 16.0f);
        } else if (g_fam != 2) {
            const char *letter = pad_name(b);
            float lpx = px * 0.72f, lw = text_w(&g_f_uib, letter, lpx, 0);
            text(&g_f_uib, x + r - lw * 0.5f, (int)(cy + lpx * 0.35f), letter, lpx, C_ICE, 0);
        }
        text(&g_f_ui, x + gw + px * 0.45f, base, what, px, C_INK, 0);
    }
    return gw + px * 0.45f + text_w(&g_f_ui, what, px, 0);
}

static void alloc_panel(int pw, int ph) {
    if (pw * ph != g_pw * g_ph) { free(g_px); g_px = malloc((size_t)pw * ph * 4); }
    g_pw = pw; g_ph = ph;
}
static float scale_of(int W, int H) { float s = (float)W / 1280.0f < (float)H / 720.0f ? (float)W / 1280.0f : (float)H / 720.0f; return s < 0.45f ? 0.45f : s; }
/* Right-side Moon River drawer geometry: 740dp wide anchored to the right edge (leaving the paused game visible on the left). */
static void panel_geom(int W, int H, float s, float *edge, float *panel_w, float moon[3]) {
    float pw = 740.0f * s;
    if (pw > (float)W) pw = (float)W;
    *panel_w = pw;
    *edge = floorf((float)W - pw);
    moon[0] = (float)W - 142.0f * s;                       /* dimmed background moon center X */
    moon[1] = 215.0f * s;                                  /* center Y */
    moon[2] = 74.0f * s;                                   /* moon disc radius */
    (void)H;
}

static void draw_panel_words(int W, int H, float s);
static uint8_t *g_pbg; static int g_pbg_w, g_pbg_h; static unsigned g_pbg_ver = ~0u;
static Layer g_hubs, g_words;

static void draw_panel(int W, int H) {
    float s = scale_of(W, H), edge, pw, moon[3];
    art_init();
    alloc_panel(W, H);
    if (!g_px) return;
    g_rect[0] = 0; g_rect[1] = 0; g_rect[2] = W; g_rect[3] = H;
    panel_geom(W, H, s, &edge, &pw, moon);
    int ex = (int)edge;
    if (g_pbg && (g_pbg_w != W || g_pbg_h != H)) { free(g_pbg); g_pbg = NULL; }
    if (g_pbg && g_pbg_ver == g_ver) {
        memcpy(g_px, g_pbg, (size_t)W * H * 4);
    } else {
        if (!g_pbg) {
            int river_h = (int)(196 * s);
            /* Full-window night sky + river (left of edge is only shown by CPU fallback; GPU clips ground at edge) */
            draw_sky_river(0, W, H, s, river_h);
            /* Dimmed live astronomical moon phase + river shimmer inside the drawer (matching launcher dim=1 opacity) */
            float lit = 0.5f;
            draw_moon_phase(moon[0], moon[1], moon[2], moon[2] * 2.4f, 0.13f, ex, &lit);
            draw_shimmer(moon[0], H - river_h, 230 * s, 170 * s, s, lit, 0.18f, ex);
            draw_vignette(ex, W, H, 0.42f);
            /* Soft outer shadow + cyan edge halo on left of edge (CPU fallback parity) */
            if (ex > 0) {
                int shw = (int)(52 * s);
                for (int i = 0; i < shw && ex - 1 - i >= 0; i++) {
                    float d = (float)i, u = 1.0f - d / (shw > 1 ? shw : 1);
                    rect(ex - 1 - i, 0, 1, H, C_VIG, u * u * 0.56f);
                    if (d < 16 * s) rect(ex - 1 - i, 0, 1, H, C_CYAN, expf(-(d * d) / (36 * s * s)) * 0.28f);
                }
            }
            /* Luminous cyan drawer left border + subtle inner bloom (x >= ex) */
            int bw = (int)(2 * s + 0.5f); if (bw < 1) bw = 1;
            int bloom = (int)(14 * s);
            for (int i = 0; i < bloom && ex + i < W; i++) {
                float d = (float)i;
                rect(ex + i, 0, 1, H, C_CYAN, expf(-(d * d) / (28 * s * s)) * 0.22f);
            }
            for (int y = 0; y < H; y++) {
                float v = (y + 0.5f) / H;
                float a = 0.42f + 0.40f * sinf(v * 3.14159265f);
                rect(ex, y, bw, 1, C_CYAN, a);
            }
            g_pbg = malloc((size_t)W * H * 4); g_pbg_w = W; g_pbg_h = H;
            if (g_pbg) memcpy(g_pbg, g_px, (size_t)W * H * 4);
        } else memcpy(g_px, g_pbg, (size_t)W * H * 4);
        layer_clear(&g_hubs);
        layer_clear(&g_words);
        g_to = &g_words;
        draw_panel_words(W, H, s);
        g_to = NULL;
        g_pbg_ver = g_ver;
    }
    if (g_gpu) return;
    double t = anim_t();
    float ring_r = moon[2] * 1.48f;
    art32_blit(&g_ring, moon[0] - ring_r, moon[1] - ring_r, 2 * ring_r, 2 * ring_r, 0.14f, (float)fmod(t * 2.0, 360.0), edge, 1);
    layer_over(&g_words);
}

static void draw_panel_words(int W, int H, float s) {
    float edge, pw, moon[3];
    panel_geom(W, H, s, &edge, &pw, moon);
    int nshow = 0;
    for (int i = 0; i < NROW; i++) nshow += row_on(i);
    float pad = 42 * s, cx0 = edge + pad, cx1 = W - pad, cw = cx1 - cx0;

    /* ---- Header breadcrumb (Moon River — PC Settings, or PC Settings — Controls) ---- */
    float hy = g_page ? 20 * s : 28 * s, ic = (g_page ? 26 : 30) * s, cpx = (g_page ? 19 : 21) * s;
    int cbase = (int)(hy + ic * 0.5f + cpx * 0.34f);
    art32_blit(&g_icon, cx0, hy, ic, ic, 1.0f, 0, edge, 1);
    float hx = text(&g_f_head, cx0 + ic + 10 * s, cbase, g_page ? "PC Settings" : "Moon River", cpx, C_CRUMB, 0.4f * s) + 14 * s;
    rect((int)hx, (int)(hy + ic * 0.5f), (int)(22 * s), (int)(1.2f * s + 0.5f) ? (int)(1.2f * s + 0.5f) : 1, C_CYAN_MID, 0.9f);
    hx += 22 * s + 14 * s;
    text_glow(&g_f_head, hx, cbase, g_page ? "Controls" : "PC Settings", cpx, C_ICE, C_CYAN, 4.5f * s, 0.55f, 0.4f * s);
    const char *sub = !g_page ? "The game waits while this is open."
                    : g_fam == 4 ? "Controller layout (keyboard keys are in [keyboard] of controllers.ini)."
                                 : "Gameplay buttons for controllers. Menus keep their own.";
    text(&g_f_ui, cx0 + 2 * s, (int)(hy + ic + (g_page ? 17 : 22) * s), sub, (g_page ? 13.5f : 14.5f) * s, C_MUTE, 0.3f * s);

    /* ---- Settings rows (aligned vertical Moon River list, matching launcher .rows / .rw) ---- */
    float band_y = H - 116 * s, foot_y = H - 58 * s;
    float top = g_page ? 76 * s : 96 * s;
    float avail = (band_y - (g_page ? 22 : 34) * s) - top;
    float rowh = nshow > 0 ? avail / nshow : 44 * s;
    float maxh = g_page ? 34 * s : 44 * s, minh = g_page ? 24 * s : 28 * s;
    if (rowh > maxh) rowh = maxh;
    if (rowh < minh) rowh = minh;
    float rpx = rowh * (g_page ? 0.55f : 0.48f), vpx = rowh * (g_page ? 0.48f : 0.41f);
    if (rpx > (g_page ? 18.0f : 21.0f) * s) rpx = (g_page ? 18.0f : 21.0f) * s;
    if (vpx > (g_page ? 15.5f : 17.5f) * s) vpx = (g_page ? 15.5f : 17.5f) * s;
    /* Top border hairline of the rows list (launcher .rows border-top: #6fe6ff38) */
    rect((int)cx0, (int)top, (int)cw, 1, C_CYAN, 0.24f);
    g_row_h = (int)rowh;
    int y = (int)top + 1;
    for (int i = 0; i < NROW; i++) {
        g_row_y[i] = -1;
        if (!row_on(i)) continue;
        g_row_y[i] = y;
        g_row_x0[i] = (int)cx0; g_row_x1[i] = (int)cx1;
        float vx1 = cx1 - 12 * s, vx0 = vx1 - (g_page ? 248 : 224) * s;
        int is_pill = k_rows[i].apply == A_QUIT || k_rows[i].apply == A_PAGE;
        g_val_x0[i] = (int)(is_pill ? cx0 : vx0);
        g_val_x1[i] = (int)vx1;
        int sel = i == g_sel, base = y + (int)(rowh * 0.67f);
        int is_quit_ask = k_rows[i].apply == A_QUIT && g_quit_ask;
        int is_cap = i == g_cap;
        int changed = !is_pill && (g_cur[i] != g_start[i] || (k_rows[i].apply == A_PRESET && g_b_on != g_b_start_on));
        if (sel) {
            /* Cyan wash gradient across the selected row (gold while waiting for a button press) */
            Rgb accent = is_quit_ask ? C_ERR_DOT : is_cap ? C_GOLD : C_CYAN;
            hgrad_a((int)cx0, y, (int)cw, (int)rowh, accent, accent, 0.16f, 0.02f);
            /* Soft rightward bloom from the left accent bar */
            int bl = (int)(12 * s);
            hgrad_a((int)cx0, y, bl, (int)rowh, accent, accent, 0.20f, 0.0f);
            /* Crisp 2.5dp glowing left accent bar */
            int barw = (int)(2.5f * s + 0.5f); if (barw < 2) barw = 2;
            rect((int)cx0, y, barw, (int)rowh, accent, 0.95f);
            /* Subtle top & bottom luminous edge highlights */
            hgrad_a((int)cx0, y, (int)cw, 1, accent, accent, 0.30f, 0.04f);
            hgrad_a((int)cx0, y + (int)rowh - 1, (int)cw, 1, accent, accent, 0.28f, 0.04f);
        } else {
            rect((int)cx0, y + (int)rowh - 1, (int)cw, 1, C_CYAN, 0.11f);
        }
        /* Row label in Marcellus (white with cyan glow when selected) */
        Rgb tc = g_lock_why[i] ? C_DIM : is_quit_ask ? C_ERR : sel ? C_WHITE : C_INK;
        if (sel && !g_lock_why[i]) text_glow(&g_f_head, cx0 + 16 * s, base, k_rows[i].label, rpx, tc, is_quit_ask ? C_ERR_DOT : is_cap ? C_GOLD : C_CYAN, 4.0f * s, 0.50f, 0.3f * s);
        else text(&g_f_head, cx0 + 16 * s, base, k_rows[i].label, rpx, tc, 0.3f * s);
        /* Value text & chevrons */
        const char *v = val_name(i);
        float vw = text_w(&g_f_uib, v, vpx, 0);
        Rgb vc = g_lock_why[i] ? C_DIM : is_quit_ask ? C_ERR : is_cap ? C_GOLD : changed ? C_GOLD : (k_rows[i].apply == A_PAGE && g_b_on) ? C_GOLD : sel ? C_WHITE : C_ICE;
        if (is_pill) {
            /* Action / Subpage row: right-aligned subtle pill look without < > chevrons */
            float bx = vx1 - vw - 22 * s, bw = vw + 22 * s, bh = rowh * 0.64f, by = y + (rowh - bh) * 0.5f;
            Rgb bc = is_quit_ask ? C_ERR_DOT : sel ? C_CYAN : C_CYAN_MID;
            rect((int)bx, (int)by, (int)bw, (int)bh, is_quit_ask ? C_ERR_DOT : C_SKY2, sel ? 0.32f : 0.20f);
            rect((int)bx, (int)by, (int)bw, 1, bc, sel ? 0.85f : 0.45f);
            rect((int)bx, (int)(by + bh - 1), (int)bw, 1, bc, sel ? 0.85f : 0.45f);
            rect((int)bx, (int)by, 1, (int)bh, bc, sel ? 0.85f : 0.45f);
            rect((int)(bx + bw - 1), (int)by, 1, (int)bh, bc, sel ? 0.85f : 0.45f);
            text(&g_f_uib, bx + (bw - vw) * 0.5f, base - (int)(rowh * 0.02f), v, vpx, vc, 0);
        } else {
            text(&g_f_uib, (vx0 + vx1) * 0.5f - vw * 0.5f, base, v, vpx, vc, 0);
            if (!g_lock_why[i] && !is_cap) {
                float cva = sel ? 1.0f : 0.65f, cpx = vpx * 1.48f;
                int cbase = base + (int)(vpx * 0.05f);
                Rgb cvc = sel ? C_CYAN : C_CYAN_MID;
                text_a(&g_f_ui, vx0, cbase, "\xE2\x80\xB9", cpx, cvc, 0, cva);
                text_a(&g_f_ui, vx1 - text_w(&g_f_ui, "\xE2\x80\xBA", cpx, 0), cbase, "\xE2\x80\xBA", cpx, cvc, 0, cva);
            }
            if (row_restart(i)) diamond(vx0 - 14 * s, base - vpx * 0.34f, 4.8f * s, C_GOLD, 0.38f);
        }
        y += (int)rowh;
    }

    /* ---- State note under the rows (right-aligned) ---- */
    char st_txt[160] = "";
    Rgb sc = C_MUTE; int mark = 0;                         /* 1 gold pending, 2 cyan applied */
    if (g_cap >= 0) { snprintf(st_txt, sizeof st_txt, "Waiting for controller button"); sc = C_GOLD; mark = 1; }
    else if (g_lock_why[g_sel] == 1) { snprintf(st_txt, sizeof st_txt, "Set by environment variable %s", k_rows[g_sel].env); sc = C_MUTE; }
    else if (g_lock_why[g_sel] == 2) { snprintf(st_txt, sizeof st_txt, "Set by profile = low in graphics.ini"); sc = C_MUTE; }
    else if (row_restart(g_sel)) { snprintf(st_txt, sizeof st_txt, "Saved; applies %s", k_rows[g_sel].apply == A_SCALE ? "when this panel closes" : "at the next start"); sc = C_GOLD; mark = 1; }
    else if ((g_cur[g_sel] != g_start[g_sel] || (k_rows[g_sel].apply == A_PRESET && g_b_on != g_b_start_on)) && k_rows[g_sel].apply != A_QUIT && k_rows[g_sel].apply != A_PAGE) { snprintf(st_txt, sizeof st_txt, "Applied and saved"); sc = C_CYAN; mark = 2; }
    if (st_txt[0]) {
        float px = (g_page ? 13.5f : 14.5f) * s, w = text_w(&g_f_ui, st_txt, px, 0.3f * s);
        int sb = y + (int)((g_page ? 16 : 22) * s);
        if (sb > (int)(band_y - 6 * s)) sb = (int)(band_y - 6 * s);
        if (mark) diamond(cx1 - w - 13 * s, sb - px * 0.34f, 4.2f * s, sc, 0.35f);
        text(&g_f_ui, cx1 - w, sb, st_txt, px, sc, 0.3f * s);
    }

    /* ---- Description band (launcher .band2 glow strip + .desc-wrap fading cyan hairlines) ---- */
    int bh = (int)(54 * s);
    hfade_center((int)edge, (int)band_y, (int)pw, bh, C_BAND_GLOW, 0.24f);
    hfade_center((int)cx0, (int)band_y, (int)cw, 1, C_CYAN, 0.42f);
    hfade_center((int)cx0, (int)band_y + bh - 1, (int)cw, 1, C_CYAN, 0.42f);
    const char *help = g_cap >= 0 ? "Press the controller button to assign to this action. Press Esc to cancel." : k_rows[g_sel].help;
    float hpx = 16.5f * s, hw = text_w(&g_f_ui, help, hpx, 0.3f * s);
    if (hw <= cw * 0.96f) {
        text(&g_f_ui, cx0 + (cw - hw) * 0.5f, (int)(band_y + bh * 0.5f + hpx * 0.35f), help, hpx, C_BAND_TXT, 0.3f * s);
    } else {
        char ln[3][160];
        int n;
        for (hpx = 15.0f * s; (n = wrap(&g_f_ui, ln, 3, help, hpx, 0.2f * s, cw * 0.95f)) > 2 && hpx > 12.0f * s; hpx -= 0.5f * s) {}
        float lh = hpx * 1.28f, total_h = n * lh;
        float sy = band_y + (bh - total_h) * 0.5f + hpx * 0.85f;
        for (int l = 0; l < n; l++) {
            float w = text_w(&g_f_ui, ln[l], hpx, 0.2f * s);
            text(&g_f_ui, cx0 + (cw - w) * 0.5f, (int)(sy + l * lh), ln[l], hpx, C_BAND_TXT, 0.2f * s);
        }
    }

    /* ---- Footer bar (launcher .foot gradient + glowing outlined prompts) ---- */
    int fh = H - (int)foot_y;
    vgrad_a((int)edge, (int)foot_y, (int)pw, fh, C_FOOT, C_FOOT, 0.12f, 0.86f);
    int fb = (int)(foot_y + fh * 0.56f + 4 * s);
    text(&g_f_ui, cx0, fb, "F1  \xC2\xB7  Start + Select", 14.5f * s, C_MUTE, 0.4f * s);
    float fpx = 16.5f * s;
    const char *act = k_rows[g_sel].apply == A_QUIT ? "Confirm"
                    : k_rows[g_sel].apply == A_PAGE ? "Open"
                    : k_rows[g_sel].apply == A_BIND ? "Rebind" : "Change";
    const char *close_lbl = g_page ? "Back" : "Close";
    float wc = hint_btn(0, fb, fpx, g_swap ? 1 : 0, act, 0);
    float wb = hint_btn(0, fb, fpx, g_swap ? 0 : 1, close_lbl, 0);
    float x = cx1 - wc - 28 * s - wb;
    hint_btn(x, fb, fpx, g_swap ? 1 : 0, act, 1);
    x += wc + 28 * s;
    g_close_r[0] = (int)(x - 10 * s); g_close_r[1] = (int)(foot_y + 6 * s); g_close_r[2] = (int)(wb + 20 * s); g_close_r[3] = (int)(fh - 12 * s);
    hint_btn(x, fb, fpx, g_swap ? 0 : 1, close_lbl, 1);
}

/* ---- First-start shader preparation screen (nvn_vk_draw.c prep_first_hold) ----
 * Matches the Moon River launcher's Building screen: full night sky + river shimmer, Moon River breadcrumb,
 * centered-right dark moon disc with rising lit moonlight fill, glowing cyan clockwise progress ring, and rotating
 * celestial outer ring. */
static struct { int on, pct, eta, reading; long long done, total; char info[96]; } g_prep;
static uint8_t *g_bg; static int g_bg_w, g_bg_h;
static void prep_disc_geom(int W, int H, float s, float *cx, float *cy, float *r_disc, float *r_ring) {
    *cx = W * 0.66f;
    *cy = H * 0.46f;
    *r_disc = 136 * s;
    *r_ring = 156 * s;
}
static void draw_prep(int W, int H) {
    float s = scale_of(W, H), cx, cy, rd, rr;
    art_init();
    alloc_panel(W, H);
    if (!g_px) return;
    g_rect[0] = 0; g_rect[1] = 0; g_rect[2] = W; g_rect[3] = H;
    prep_disc_geom(W, H, s, &cx, &cy, &rd, &rr);
    if (g_bg && (g_bg_w != W || g_bg_h != H)) { free(g_bg); g_bg = NULL; }
    if (!g_bg) {
        int river_h = (int)(206 * s);
        draw_sky_river(0, W, H, s, river_h);
        draw_shimmer(cx, H - river_h, 280 * s, 180 * s, s, 0.75f, 0.40f, 0);
        draw_vignette(0, W, H, 0.65f);
        /* Static moon disc base + rim + outer faint track ring (matching launcher gen:builddisc) */
        int x0 = (int)floorf(cx - rr - 6 * s), y0 = (int)floorf(cy - rr - 6 * s);
        int x1 = (int)ceilf(cx + rr + 6 * s), y1 = (int)ceilf(cy + rr + 6 * s);
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W) x1 = W; if (y1 > H) y1 = H;
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
            float r = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            float fc = covf(r - rd);
            if (fc > 0) put(x, y, C_DISC, fc);
            float ec = covf(fabsf(r - rd) - 0.7f * s);
            if (ec > 0) put(x, y, C_RIV_EDGE, 0.22f * ec);
            float tc = covf(fabsf(r - rr) - 1.5f * s);
            if (tc > 0) put(x, y, C_CYAN, 0.14f * tc);
        }
        /* Top-left breadcrumb (Moon River — First start) */
        float hy = 38 * s, ic = 32 * s, cpx = 21 * s;
        int cbase = (int)(hy + ic * 0.5f + cpx * 0.34f);
        art32_blit(&g_icon, 56 * s, hy, ic, ic, 1.0f, 0, NOCLIP, 0);
        float hx = text(&g_f_head, 56 * s + ic + 10 * s, cbase, "Moon River", cpx, C_CRUMB, 0.4f * s) + 14 * s;
        rect((int)hx, (int)(hy + ic * 0.5f), (int)(24 * s), (int)(1.2f * s + 0.5f) ? (int)(1.2f * s + 0.5f) : 1, C_CYAN_MID, 0.9f);
        hx += 24 * s + 14 * s;
        text_glow(&g_f_head, hx, cbase, "First start", cpx, C_ICE, C_CYAN, 4.5f * s, 0.55f, 0.4f * s);
        /* Left lead column */
        float lx = 68 * s, ly = 218 * s, ttl = 44 * s;
        text_glow(&g_f_head, lx, (int)(ly + ttl * 0.78f), "Preparing shaders", ttl, C_ICE, C_CYAN, 5.5f * s, 0.48f, 0);
        hgrad_a((int)lx, (int)(ly + ttl * 1.12f), (int)(240 * s), 1, C_CYAN, C_CYAN, 0.75f, 0.0f);
        text(&g_f_ui, lx, (int)(ly + ttl * 1.12f + 36 * s), "Compiling shader pipelines for your GPU.", 18 * s, C_BODY, 0.2f * s);
        text(&g_f_ui, lx, (int)(ly + ttl * 1.12f + 68 * s), "This happens once per GPU, driver, or update.", 15.5f * s, C_MUTE, 0.2f * s);
        text(&g_f_ui, lx, (int)(ly + ttl * 1.12f + 94 * s), "Closing the window keeps everything built so far.", 15.5f * s, C_MUTE, 0.2f * s);
        /* Bottom status band + footer */
        int band_y = (int)(H - 102 * s), bh = (int)(40 * s), foot_y = (int)(H - 58 * s), fh = H - foot_y;
        hfade_center(0, band_y, W, bh, C_BAND_GLOW, 0.22f);
        const char *bn = "First start only  \xC2\xB7  Closing the window keeps what is done.";
        float bw = text_w(&g_f_ui, bn, 16 * s, 0.3f * s);
        text(&g_f_ui, (W - bw) * 0.5f, band_y + (int)(bh * 0.5f + 5.5f * s), bn, 16 * s, C_BAND_TXT, 0.3f * s);
        vgrad_a(0, foot_y, W, fh, C_FOOT, C_FOOT, 0.12f, 0.86f);
        float fic = 26 * s;
        art32_blit(&g_icon, 44 * s, foot_y + (fh - fic) * 0.5f, fic, fic, 1.0f, 0, NOCLIP, 0);
        text(&g_f_ui, 44 * s + fic + 12 * s, foot_y + (int)(fh * 0.56f + 4 * s), g_prep.info, 15 * s, C_SUB, 0.3f * s);
        g_bg = malloc((size_t)W * H * 4); g_bg_w = W; g_bg_h = H;
        if (g_bg) memcpy(g_bg, g_px, (size_t)W * H * 4);
        layer_clear(&g_hubs); layer_clear(&g_words);
    } else memcpy(g_px, g_bg, (size_t)W * H * 4);

    /* Rising lit moon fill inside the disc (matching launcher gen:moonfill) */
    float frac = clampf(g_prep.pct / 100.0f, 0, 1);
    if (frac > 0.002f) {
        float cut_y = (cy + rd) - frac * (2.0f * rd);
        int x0 = (int)floorf(cx - rd - 1), x1 = (int)ceilf(cx + rd + 1);
        int y0 = (int)floorf(cut_y - 1), y1 = (int)ceilf(cy + rd + 1);
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W) x1 = W; if (y1 > H) y1 = H;
        for (int y = y0; y < y1; y++) {
            float rcov = clampf((y + 0.5f) - cut_y + 0.5f, 0, 1);
            if (rcov <= 0) continue;
            float dy = y + 0.5f - cy;
            for (int x = x0; x < x1; x++) {
                float dx = x + 0.5f - cx;
                float c = covf(sqrtf(dx * dx + dy * dy) - (rd - 1.0f)) * rcov;
                if (c <= 0) continue;
                float gx = (dx / (2 * rd) + 0.5f) - 0.40f, gy = (dy / (2 * rd) + 0.5f) - 0.38f;
                float gt = clampf(sqrtf(gx * gx + gy * gy) / 0.72f, 0, 1);
                float r, g, b;
                if (gt < 0.60f) { float u = gt / 0.60f; r = 251 + (207 - 251) * u; g = 253 + (224 - 253) * u; b = 255 + (245 - 255) * u; }
                else { float u = (gt - 0.60f) / 0.40f; r = 207 + (143 - 207) * u; g = 224 + (169 - 224) * u; b = 245 + (208 - 245) * u; }
                Rgb mc = {(uint8_t)b, (uint8_t)g, (uint8_t)r};
                put(x, y, mc, 0.28f * c);
            }
        }
    }
    /* Glowing cyan clockwise progress arc from 12 o'clock with smooth rounded glowing end-caps (matching launcher gen:glowring) */
    if (frac > 0.002f) {
        const float TAU = 6.28318530f;
        float max_ang = frac * TAU;
        float glow_span = 18 * s;
        float ex0 = cx, ey0 = cy - rr;
        float ex1 = cx + sinf(max_ang) * rr, ey1 = cy - cosf(max_ang) * rr;
        int x0 = (int)floorf(cx - rr - glow_span), x1 = (int)ceilf(cx + rr + glow_span);
        int y0 = (int)floorf(cy - rr - glow_span), y1 = (int)ceilf(cy + rr + glow_span);
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W) x1 = W; if (y1 > H) y1 = H;
        for (int y = y0; y < y1; y++) {
            float dy = y + 0.5f - cy;
            for (int x = x0; x < x1; x++) {
                float dx = x + 0.5f - cx, rad_d = sqrtf(dx * dx + dy * dy), dist = fabsf(rad_d - rr);
                if (dist > glow_span) continue;
                float ang = atan2f(dx, -dy);               /* 0 at top, clockwise 0 .. 2pi */
                if (ang < 0) ang += TAU;
                if (frac < 0.999f && ang > max_ang) {
                    float d0 = sqrtf((x + 0.5f - ex0) * (x + 0.5f - ex0) + (y + 0.5f - ey0) * (y + 0.5f - ey0));
                    float d1 = sqrtf((x + 0.5f - ex1) * (x + 0.5f - ex1) + (y + 0.5f - ey1) * (y + 0.5f - ey1));
                    dist = d0 < d1 ? d0 : d1;
                    if (dist > glow_span) continue;
                }
                float dn = dist / s;
                float halo = expf(-(dn * dn) / 36.0f) * 0.55f;
                float core = covf(dist - 1.6f * s);
                float a = clampf(halo + core * (1.0f - halo), 0, 1);
                if (a > 0.004f) {
                    Rgb gc = {
                        (uint8_t)(C_CYAN.b + (255 - C_CYAN.b) * core * 0.4f),
                        (uint8_t)(C_CYAN.g + (252 - C_CYAN.g) * core * 0.4f),
                        (uint8_t)(C_CYAN.r + (235 - C_CYAN.r) * core * 0.4f)
                    };
                    put(x, y, gc, a);
                }
            }
        }
    }
    /* Centered Marcellus percentage + stage & ETA notes inside the moon disc */
    char num[32], t1[64], t2[64];
    snprintf(num, sizeof num, "%d", g_prep.pct);
    float big = 78 * s, sym = 32 * s;
    float nw = text_w(&g_f_head, num, big, 0), sw = text_w(&g_f_head, "%", sym, 0);
    int pb = (int)(cy + big * 0.24f);
    float nx = cx - (nw + 3 * s + sw) * 0.5f;
    text_glow(&g_f_head, nx, pb, num, big, C_ICE, C_CYAN, 6.0f * s, 0.55f, 0);
    text_glow(&g_f_head, nx + nw + 3 * s, pb, "%", sym, C_ICE, C_CYAN, 4.0f * s, 0.45f, 0);
    if (g_prep.reading) snprintf(t1, sizeof t1, "reading the game data");
    else snprintf(t1, sizeof t1, "%lld of about %lld stages", g_prep.done, g_prep.total);
    float w1 = text_w(&g_f_ui, t1, 15 * s, 0.2f * s);
    text(&g_f_ui, cx - w1 * 0.5f, pb + (int)(30 * s), t1, 15 * s, C_SUB, 0.2f * s);
    if (g_prep.eta >= 0) snprintf(t2, sizeof t2, "about %d:%02d left", g_prep.eta / 60, g_prep.eta % 60);
    else snprintf(t2, sizeof t2, "working out the time left");
    float w2 = text_w(&g_f_ui, t2, 14 * s, 0.2f * s);
    text(&g_f_ui, cx - w2 * 0.5f, pb + (int)(52 * s), t2, 14 * s, C_MUTE, 0.2f * s);
    if (!g_gpu) {
        float cr = rr + 24 * s;
        art32_blit(&g_ring, cx - cr, cy - cr, 2 * cr, 2 * cr, 0.24f, (float)fmod(anim_t() * 2.0, 360.0), NOCLIP, 0);
    }
}
/* the render thread (nvn_vk_draw.c): the first-start screen's numbers; reading: the game data is still being read */
void pcmenu_prep_show(int pct, long long done, long long total, int eta, int reading, const char *info) {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_inited && fonts_init()) menu_init();
    if (!g_prep.on || g_prep.pct != pct || g_prep.done != done || g_prep.total != total || g_prep.eta != eta || g_prep.reading != reading) g_ver++;
    g_prep.on = 1; g_prep.pct = pct; g_prep.done = done; g_prep.total = total; g_prep.eta = eta; g_prep.reading = reading;
    snprintf(g_prep.info, sizeof g_prep.info, "%s", info ? info : "");
    ReleaseSRWLockExclusive(&g_lock);
}
void pcmenu_prep_hide(void) {
    AcquireSRWLockExclusive(&g_lock);
    g_prep.on = 0; g_ver++;
    free(g_bg); g_bg = NULL; layer_free(&g_hubs); layer_free(&g_words);
    ReleaseSRWLockExclusive(&g_lock);
}

/* render thread (nvn_vk_pcmenu.c): the panel for this window size, drawn again when something changed */
int pcmenu_is_open(void) { return g_open != 0; }
int pcmenu_kind(void) { return g_open ? 1 : g_prep.on ? 3 : 0; }
int pcmenu_settled(void) { return pcmenu_kind() != 1 || (!g_close_at && GetTickCount64() - g_open_at >= 1000); }
int pcmenu_visible(void) { return pcmenu_kind() != 0; }
int pcmenu_panel_size(int W, int H, int r[4], unsigned *gen) {
    static int drawn_kind = -1;
    static ULONGLONG drawn_at;
    int kind = pcmenu_kind();
    if (!kind) return 0;
    AcquireSRWLockExclusive(&g_lock);
    static ULONGLONG took = 33;
    ULONGLONG now = GetTickCount64();
    int turn = !g_gpu && (kind == 1 || kind == 3) && now - drawn_at >= (took > 33 ? took : 33);
    if (g_drawn_ver != g_ver || g_for_w != W || g_for_h != H || drawn_kind != kind || turn) {
        if (kind == 1) draw_panel(W, H); else draw_prep(W, H);
        g_drawn_ver = g_ver; g_for_w = W; g_for_h = H; drawn_kind = kind; g_gen++; drawn_at = now;
        took = GetTickCount64() - now;
    }
    memcpy(r, g_rect, sizeof g_rect); *gen = g_gen;
    ReleaseSRWLockExclusive(&g_lock);
    return g_px != NULL;
}
void pcmenu_panel_read(void *dst, int w, int h) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_px && w == g_pw && h == g_ph) memcpy(dst, g_px, (size_t)w * h * 4);
    ReleaseSRWLockExclusive(&g_lock);
}
int pcmenu_take_work(void) { InterlockedExchange(&g_job, 0); return (int)InterlockedExchange(&g_work, 0); }
void pcmenu_gpu(int on) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_gpu != (on != 0)) { g_gpu = on != 0; g_drawn_ver = ~0u; free(g_pbg); g_pbg = NULL; }
    ReleaseSRWLockExclusive(&g_lock);
}
static float ease_out(float t) { if (t <= 0) return 0; if (t >= 1) return 1; float u = 1 - t; return 1 - u * u * u; }
static void emit(int art, float x, float y, float w, float h, float deg, float alpha, float clip_top, int clip_right, float y0, float y1) {
    if (g_emit_n >= g_emit_max || alpha <= 0) return;
    PcmSprite e = {art, x, y, w, h, deg, alpha, 0, 0, clip_top, clip_right, NOCLIP, 0, y0, y1};
    g_emit[g_emit_n++] = e;
}
/* GPU sprite list for the window (W, H):
 *   - First-start screen (kind == 3): slowly rotating celestial lunar ring over the moon progress disc.
 *   - PC Settings panel (kind == 1): over the live game frame (*over_game = 1), fades in a dark scrim + soft shadow on the
 *     left and slides the Moon River drawer in from the right with staggered rows and rotating celestial ring. */
int pcmenu_gpu_scene(int W, int H, PcmSprite *out, int max, int *over_game) {
    int kind = pcmenu_kind(), can = *over_game;
    *over_game = 0;
    if (kind != 1 && kind != 3) return 0;
    AcquireSRWLockExclusive(&g_lock);
    float s = scale_of(W, H), fw = (float)W, fh = (float)H;
    double t = anim_t();
    float deg = (float)fmod(t * 2.0, 360.0);
    ULONGLONG now = GetTickCount64();
    g_emit = out; g_emit_n = 0; g_emit_max = max;
    if (kind == 3) {
        float cx, cy, rd, rr;
        prep_disc_geom(W, H, s, &cx, &cy, &rd, &rr);
        float cr = rr + 24 * s;
        emit(2, cx - cr, cy - cr, 2 * cr, 2 * cr, deg, 0.24f, NOCLIP, 0, 0, 0);
    } else {
        float edge, pw, moon[3];
        panel_geom(W, H, s, &edge, &pw, moon);
        float ring_r = moon[2] * 1.48f;
        if (!can) {
            emit(2, moon[0] - ring_r, moon[1] - ring_r, 2 * ring_r, 2 * ring_r, deg, 0.14f, edge, 1, 0, 0);
            emit(-2, 0, 0, fw, fh, 0, 1.0f, NOCLIP, 0, 0, 0);
        } else {
            *over_game = 1;
            float open = g_close_at ? 1 - ease_out((float)(now - g_close_at) / PCM_CLOSE_MS) : ease_out((float)(now - g_open_at) / PCM_OPEN_MS);
            float dx = (1 - open) * pw, ex = edge + dx;
            /* Dimmed game scrim + soft drop shadow & outer cyan edge halo left of the drawer */
            emit(0, 0, 0, fw, fh, 0, 0.42f * open, ex, 0, 0, 0);
            emit(1, ex - 52 * s, 0, 52 * s, fh, 0, open, ex, 0, 0, 0);
            /* Sliding Moon River drawer ground + subtle rotating celestial ring */
            emit(-3, dx, 0, fw, fh, 0, 1.0f, ex, 1, 0, 0);
            emit(2, moon[0] + dx - ring_r, moon[1] - ring_r, 2 * ring_r, 2 * ring_r, deg, 0.14f, ex, 1, 0, 0);
            /* Words layer: single pass when settled, staggered row slide during open/close */
            if (open >= 1 && !g_close_at) {
                emit(-2, 0, 0, fw, fh, 0, 1.0f, edge, 1, 0, 0);
            } else {
                float rows_top = fh, rows_bot = 0;
                int k = 0, nshow = 0;
                for (int i = 0; i < NROW; i++) nshow += g_row_y[i] >= 0;
                float stag = nshow > 1 ? (0.18f / (nshow - 1) < 0.035f ? 0.18f / (nshow - 1) : 0.035f) : 0;
                for (int i = 0; i < NROW; i++) {
                    if (g_row_y[i] < 0) continue;
                    float y0 = (float)g_row_y[i], y1 = y0 + g_row_h;
                    if (y0 < rows_top) rows_top = y0; if (y1 > rows_bot) rows_bot = y1;
                    float p = g_close_at ? open : ease_out(((float)(now - g_open_at) / 1000.0f - 0.05f - k * stag) / 0.22f);
                    float sh = (1 - p) * 44 * s;
                    emit(-2, dx + sh, 0, fw, fh, 0, p, ex, 1, y0, y1);
                    k++;
                }
                if (rows_top > rows_bot) emit(-2, dx, 0, fw, fh, 0, 1.0f, ex, 1, 0, 0);
                else {
                    emit(-2, dx, 0, fw, fh, 0, 1.0f, ex, 1, 0, rows_top);
                    emit(-2, dx, 0, fw, fh, 0, 1.0f, ex, 1, rows_bot, fh);
                }
            }
        }
    }
    int n = g_emit_n;
    g_emit = NULL; g_emit_n = g_emit_max = 0;
    ReleaseSRWLockExclusive(&g_lock);
    return n;
}
int pcmenu_layer_read(int which, void *dst, int w, int h) {
    const Layer *l = which == 1 ? &g_hubs : which == 2 ? &g_words : NULL;
    int ok = 0;
    AcquireSRWLockExclusive(&g_lock);
    if (l && l->px && w == g_pw && h == g_ph && l->n == w * h && l->x1 > l->x0) { memcpy(dst, l->px, (size_t)w * h * 4); ok = 1; }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

/* ---------------------------------------------------------------- changes */
static void add_work(LONG bit) { LONG o; do o = g_work; while (InterlockedCompareExchange(&g_work, o | bit, o) != o); }
static void set_value(int i, int k) {                    /* value k of row i: written, and applied where it can be */
    const Row *r = &k_rows[i];
    if (g_lock_why[i] || k < 0 || k >= row_count(i)) return;
    if (r->apply == A_PAGE) return;
    if (r->apply == A_BIND) {
        if (k == g_cur[i] && g_b_on) return;
        uint8_t next[16];
        memcpy(next, g_b, 16);
        rb_swap(next, r->slot, (uint8_t)k);
        if (!nnpad_bind_set(next)) return;
        memcpy(g_b, next, 16);
        g_b_on = 1;
        binds_sync();
        printf("pcmenu: %s = %s (applied)\n", r->key, pad_name(k));
        return;
    }
    if (r->apply == A_PRESET) {
        if (k == g_cur[i]) return;
        if (k == 0) {
            if (!nnpad_bind_set(NULL)) return;
            g_b_on = 0;
            rb_preset(prompts_control_type(), g_b);
        } else {
            uint8_t next[16];
            rb_preset(k - 1, next);
            if (!nnpad_bind_set(next)) return;
            memcpy(g_b, next, 16);
            g_b_on = 1;
        }
        binds_sync();
        printf("pcmenu: bind_preset = %s (applied)\n", r->val[k]);
        return;
    }
    if (k == g_cur[i]) return;
    int ok;
    if (r->apply == A_SIZE) { char w[16], h[16]; sscanf(r->val[k], "%15[0-9]x%15[0-9]", w, h); ok = gfx_option_set("width", w) && gfx_option_set("height", h); }
    else if (r->apply == A_PROMPTS) { ok = nnpad_prompts_set(r->val[k]); g_fam = nnpad_button_prompts(); g_letter = nnpad_prompts_by_letter(); }      /* the hints and the bind names follow */
    else if (r->apply == A_MENU_SWAP) ok = nnpad_menu_swap_set(r->val[k]);
    else if (r->apply == A_LAYOUT) { ok = nnpad_layout_set(r->val[k]); g_letter = nnpad_prompts_by_letter(); }
    else ok = gfx_option_set(r->key, r->val[k]);
    if (!ok) return;
    g_cur[i] = k;
    switch (r->apply) {
    case A_VSYNC: display_vsync_apply(); add_work(2); break;
    case A_WINDOW: case A_SIZE: if (!display_window_apply()) g_nowin = 1; break;
    case A_SCALE: if (!g_open) add_work(4); break;         /* (the panel: when it closes) */
    default: break;
    }
    printf("pcmenu: %s = %s (%s)\n", r->key, r->val[k], !row_restart(i) ? "applied" : r->apply == A_SCALE ? "applies when the panel closes" : "applies at the next start");
}
/* a value's first two numbers (1.4; 1366x768; 16:10); 0 when it starts with none (auto, game) */
static int numbers2(const char *s, double *a, double *b) {
    char *e; *a = strtod(s, &e); *b = e != s && *e ? strtod(e + 1, NULL) : 0;
    return e != s;
}
static int before2(double a0, double a1, double b0, double b1) { return a0 < b0 || (a0 == b0 && a1 < b1); }
/* from a value not in the list (another version's, or the file edited by hand): the nearest one that way by its numbers
 * (1.4 -> 1.2 or 1.6, 16:10 -> 16:9 on the left); none that way: the ends when round (wrap), else -1 (stays) */
static int nearest(int i, int dir, int wrap) {
    double v0, v1, o0, o1, b0 = 0, b1 = 0;
    int n = row_count(i), best = -1, num = numbers2(g_raw[i], &v0, &v1), any = 0;      /* any: a row of numbers */
    for (int k = 0; k < n && num && k_rows[i].apply != A_BIND; k++) {
        if (!numbers2(k_rows[i].val[k], &o0, &o1)) continue;
        any = 1;
        if ((dir > 0 ? before2(v0, v1, o0, o1) : before2(o0, o1, v0, v1)) &&
            (best < 0 || (dir > 0 ? before2(o0, o1, b0, b1) : before2(b0, b1, o0, o1)))) { best = k; b0 = o0; b1 = o1; }
    }
    return best >= 0 ? best : wrap || !any ? (dir > 0 ? 0 : n - 1) : -1;
}
static void change(int i, int dir, int wrap) {             /* wrap 0 (left / right): stops at the ends; 1 (A, a click): round */
    int n = row_count(i);
    if (k_rows[i].apply == A_BIND) wrap = 1;
    set_value(i, g_cur[i] < 0 ? nearest(i, dir, wrap) : wrap ? (g_cur[i] + dir + n) % n : g_cur[i] + dir);
}
static int activate(int i, int dir) {                      /* A or a click on a row; 1 = it took the press and the row's value is not what changes */
    if (k_rows[i].apply == A_PAGE) { page_go(1, 0); return 1; }
    if (k_rows[i].apply == A_BIND) { g_cap = i; g_cap_t = GetTickCount64(); g_cap_armed = 0; return 1; }
    if (k_rows[i].apply == A_QUIT) {
        if (!g_quit_ask) { g_quit_ask = 1; return 1; }
        printf("pcmenu: Quit to desktop chosen\n");
        display_quit();
        return 1;
    }
    g_quit_ask = 0;
    change(i, dir, 1);
    return 0;
}

/* ---------------------------------------------------------------- the menu's thread */
static int g_log = -1;
static void mouse_at(int *x, int *y, int *down) {          /* window pixels; -1 = not over the window */
    *x = *y = -1;
    *down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#ifdef _WIN32
    POINT p; HWND h = GetForegroundWindow(); DWORD pid = 0;
    if (h) GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) { *down = 0; return; }       /* another program in front: its pointer and clicks are not ours (as nnhid.c's keyboard_ours) */
    if (GetCursorPos(&p) && ScreenToClient(h, &p)) { *x = p.x; *y = p.y; }
#else
    { extern int display_mouse_pixels(int *x, int *y); display_mouse_pixels(x, y); }
#endif
}
static DWORD WINAPI menu_thread(LPVOID arg) {
    (void)arg;
    int prev_dir = 0, prev_ok = 1, prev_back = 1, prev_click = 1, rep_at = 0, mx0 = -2, my0 = -2;
    ULONGLONG t_dir = 0;
    float sens; int inv;
    display_mouse_want(0);                                 /* the cursor back for the menu (the mouse camera lets it go) */
    while (g_open && !g_display_close) {
        uint64_t b; int lx, ly, mx, my, md;
        nnhid_menu_read(&b, &lx, &ly, g_swap);
        enum { BA = 1 << 0, BB = 1 << 1, BPLUS = 1 << 10, BMINUS = 1 << 11, BLEFT = 1 << 12, BUP = 1 << 13, BRIGHT = 1 << 14, BDOWN = 1 << 15 };
        int up = (b & BUP) || ly > 16000 || nnhid_menu_key(VK_UP), down = (b & BDOWN) || ly < -16000 || nnhid_menu_key(VK_DOWN);
        int left = (b & BLEFT) || lx < -16000 || nnhid_menu_key(VK_LEFT), right = (b & BRIGHT) || lx > 16000 || nnhid_menu_key(VK_RIGHT);
        int ok = (b & BA) || nnhid_menu_key(VK_RETURN) || nnhid_menu_key(VK_SPACE);
        int close_now = nnhid_menu_key(VK_F1) || (b & (BPLUS | BMINUS)) == (BPLUS | BMINUS);
        int back = (b & BB) || nnhid_menu_key(VK_ESCAPE) || nnhid_menu_key(VK_BACK) || close_now;
        int dir = up ? 1 : down ? 2 : left ? 3 : right ? 4 : 0;
        ULONGLONG now = GetTickCount64();
        int step = dir && (dir != prev_dir || (rep_at && now >= t_dir + (ULONGLONG)rep_at));
        if (dir != prev_dir) { t_dir = now; rep_at = 380; } else if (step) { t_dir = now; rep_at = 90; }
        prev_dir = dir;
        mouse_at(&mx, &my, &md);
        AcquireSRWLockExclusive(&g_lock);
        if (g_cap >= 0) {                                  /* waiting for a controller button; what is held now must release first */
            uint32_t p = nnhid_menu_pad();
            int esc = nnhid_menu_key(VK_ESCAPE) || close_now;
            g_cap_armed |= !p;
            if (esc || now - g_cap_t > 8000 || (g_cap_armed && p)) {
                if (!esc && p) {
                    int k = 0;
                    while (k < 15 && !(p >> k & 1)) k++;
                    set_value(g_cap, k);
                }
                g_cap = -1; g_ver++;
                prev_ok = 1; prev_back = 1; prev_dir = 0;
            }
            ReleaseSRWLockExclusive(&g_lock);
            prev_click = md;
            if (!InterlockedExchange(&g_job, 1) && !nvn_front_post_pcmenu()) InterlockedExchange(&g_job, 0);
            Sleep(16);
            continue;
        }
        int was_sel = g_sel, was_cur[NROW], was_page = g_page;
        memcpy(was_cur, g_cur, sizeof g_cur);
        if (step && (dir == 1 || dir == 2)) { do g_sel = (g_sel + (dir == 1 ? NROW - 1 : 1)) % NROW; while (!row_on(g_sel)); }
        if (step && (dir == 3 || dir == 4)) change(g_sel, dir == 3 ? -1 : 1, 0);
        if (ok && !prev_ok) { activate(g_sel, 1); g_ver++; }
        int px = mx - g_rect[0], py = my - g_rect[1], click = md && !prev_click;
        if (mx >= 0 && (mx != mx0 || my != my0 || click) && px >= 0 && px < g_pw && py >= 0 && py < g_ph && g_cap < 0) {      /* the mouse: hover chooses, a click changes */
            for (int i = 0; i < NROW; i++)                 /* a row's own span: the panel covers the window, its rows step down the diagonal */
                if (g_row_y[i] >= 0 && py >= g_row_y[i] && py < g_row_y[i] + g_row_h && px >= g_row_x0[i] && px < g_row_x1[i]) {
                    g_sel = i;
                    if (click && px >= g_val_x0[i] - g_row_h) {
                        if (k_rows[i].apply == A_BIND) {
                            if (px < g_val_x0[i] + g_row_h) change(i, -1, 1);
                            else if (px > g_val_x1[i] - g_row_h) change(i, 1, 1);
                            else activate(i, 1);
                        } else {
                            activate(i, px < g_val_x0[i] + g_row_h ? -1 : 1);
                        }
                        g_ver++; break;
                    }
                }
            if (click && px >= g_close_r[0] && px < g_close_r[0] + g_close_r[2] && py >= g_close_r[1] && py < g_close_r[1] + g_close_r[3]) back = 1, prev_back = 0;
        }
        mx0 = mx; my0 = my;
        if (g_sel != was_sel || g_page != was_page || memcmp(was_cur, g_cur, sizeof g_cur)) g_ver++;
        int do_back = back && !prev_back && !g_close_at;
        if (do_back) {
            if (g_page && !close_now) { page_go(0, 1); g_ver++; }
            else { g_close_at = now; g_ver++; }            /* the closing plays (PCM_CLOSE_MS); the game goes on after it */
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (g_log && (step || (ok && !prev_ok) || (back && !prev_back) || click)) printf("pcmenu: input: %s%s%s%s -> row %d (%s)\n", step ? (dir == 1 ? "up " : dir == 2 ? "down " : dir == 3 ? "left " : "right ") : "", ok && !prev_ok ? "A " : "", back && !prev_back ? "B " : "", click ? "click " : "", g_sel, k_rows[g_sel].key);
        prev_ok = ok; prev_click = md;
        prev_back = back;
        if (g_close_at && (now - g_close_at >= PCM_CLOSE_MS || !g_gpu)) break;
        if (!InterlockedExchange(&g_job, 1) && !nvn_front_post_pcmenu()) InterlockedExchange(&g_job, 0);
        Sleep(16);
    }
    InterlockedExchange(&g_release, 0xff);                 /* each player reads an idle pad until it has let go of everything */
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < NROW; i++) if (k_rows[i].apply == A_SCALE) g_start[i] = g_cur[i];
    InterlockedExchange(&g_open, 0);
    ReleaseSRWLockExclusive(&g_lock);
    if (!g_display_close) { add_work(4); nvn_front_post_pcmenu(); }      /* before the game goes on: its next commands come after the job */
    InterlockedExchange(&g_guest_frozen, 0);
    if (nnpad_mouse_cfg(&sens, &inv)) display_mouse_want(1);
    printf("pcmenu: closed; the game goes on\n");
    fflush(stdout);
    return 0;
}
static void menu_open(const char *why) {
    if (InterlockedExchange(&g_open, 1)) return;
    if (g_log < 0) g_log = getenv("MR_PCMENU_LOG") && atoi(getenv("MR_PCMENU_LOG"));
    AcquireSRWLockExclusive(&g_lock);
    if (!g_f_ui.data && !fonts_init()) { ReleaseSRWLockExclusive(&g_lock); InterlockedExchange(&g_open, 0); printf("pcmenu: the font cannot be read\n"); return; }
    if (!g_inited) menu_init();
    g_swap = nnpad_menu_swapped();
    g_fam = nnpad_button_prompts();
    g_letter = nnpad_prompts_by_letter();
    binds_load();
    g_open_at = GetTickCount64(); g_close_at = 0;
    page_go(0, 0);
    g_ver++;
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedExchange(&g_guest_frozen, 1);               /* the game stops at its next SDK call (hostrt.c) */
    printf("pcmenu: opened (%s); the game is held\n", why);
    fflush(stdout);
    HANDLE th = CreateThread(NULL, 0, menu_thread, NULL, 0, NULL);
    if (th) CloseHandle(th); else { InterlockedExchange(&g_guest_frozen, 0); InterlockedExchange(&g_open, 0); }
}
/* nnhid.c, at every pad state the game reads: opens the menu, and says when the game gets an idle pad */
int pcmenu_gate(uint32_t id, uint64_t buttons) {
    static int prev = 1, test = -1;
    static ULONGLONG t0;
    LONG bit = id < 8 ? 1L << id : 0, o;
    if (g_open) return 1;
    if (id == 0) {
        int f1 = nnhid_menu_key(VK_F1), combo = (buttons & 0xc00) == 0xc00;      /* Start + Select (the game uses L3 and R3) */
        if (test < 0) { const char *e = getenv("MR_PCMENU_OPEN"); test = e ? atoi(e) : 0; t0 = GetTickCount64(); }
        if (test > 0 && GetTickCount64() - t0 >= (ULONGLONG)test * 1000) { test = 0; menu_open("MR_PCMENU_OPEN"); return 1; }
        if ((f1 || combo) && !prev && !(g_release & 1)) { prev = 1; menu_open(f1 ? "F1" : "Start + Select"); return 1; }
        prev = f1 || combo;
        if (prev && (g_release & 1)) return 1;             /* the F1 that closed it is still down */
    }
    if (!(g_release & bit)) return 0;
    if (buttons) return 1;
    do o = g_release; while (InterlockedCompareExchange(&g_release, o & ~bit, o) != o);
    if (g_log > 0) printf("pcmenu: player %u's input reaches the game again\n", id + 1);
    return 0;
}
