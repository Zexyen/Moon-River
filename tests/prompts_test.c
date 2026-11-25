/* Standalone unit test for built-in PlayStation / Xbox / Keyboard & Mouse button prompts (prompts.c).
 * Build & run:
 *   clang -O2 -Wall -Wextra -Wno-unused-function -DPROMPTS_NO_WIN -D_stricmp=strcasecmp \
 *         -I runtime tests/prompts_test.c games/bayonetta2/runtime/prompts.c runtime/tex_decode.c runtime/tex_pack.c -lm -o /tmp/prompts_test && /tmp/prompts_test */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "prompts.h"
#include "tex_decode.h"
#include "tex_pack.h"

void prompts_patch_ex(int atlas_id, uint8_t *linear, int w, int h, int nvn_format,
                      int fam, int by_letter, int mouse_cam, const int kb[24]);

static int g_fam = 1, g_letter = 0, g_mouse = 1;
static int g_kb[24] = {
    0x02, 0x20, 0x01, 'E', 'C', 0x04, 'R', 'F', 'Q', 0xA0, 0x1B, 0x09,
    0x25, 0x26, 0x27, 0x28, 'A', 'W', 'D', 'S', 'J', 'I', 'L', 'K'
};
int nnpad_button_prompts(void) { return g_fam; }
int nnpad_prompts_by_letter(void) { return g_letter; }
int nnpad_mouse_cfg(float *s, int *i) { if (s) *s = 1.0f; if (i) *i = 0; return g_mouse; }
int nnpad_kb_vk(int idx) { return idx >= 0 && idx < 24 ? g_kb[idx] : 0; }

static int g_fail;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } } while (0)

static int rect_diff(const uint8_t *a, const uint8_t *b, int stride, int x0, int y0, int rw, int rh) {
    int n = 0;
    for (int j = 0; j < rh; j++)
        for (int i = 0; i < rw; i++) {
            const uint8_t *pa = a + (size_t)(y0 + j) * stride + (size_t)(x0 + i) * 4;
            const uint8_t *pb = b + (size_t)(y0 + j) * stride + (size_t)(x0 + i) * 4;
            if (memcmp(pa, pb, 4) != 0) n++;
        }
    return n;
}

static int rect_has_color(const uint8_t *img, int stride, int x0, int y0, int rw, int rh, int ch_dom) {
    int hits = 0;
    for (int j = 0; j < rh; j++)
        for (int i = 0; i < rw; i++) {
            const uint8_t *p = img + (size_t)(y0 + j) * stride + (size_t)(x0 + i) * 4;
            if (p[3] < 180) continue;
            int r = p[0], g = p[1], b = p[2];
            if (ch_dom == 0 && r > g + 50 && r > b + 50) hits++;       /* red (Circle / Xbox B) */
            else if (ch_dom == 1 && g > r + 40 && g > b + 40) hits++;  /* green (Xbox A / Triangle) */
            else if (ch_dom == 2 && b > r + 40 && b > g + 20) hits++;  /* blue (Cross / Xbox X) */
        }
    return hits;
}

int main(void) {
    /* 1. State key changes whenever family, layout, mouse_camera or a keyboard binding changes */
    g_fam = 1; g_letter = 0;
    uint64_t k_sw = prompts_state_key();
    g_fam = 2; g_letter = 0;
    uint64_t k_ps_pos = prompts_state_key();
    g_letter = 1;
    uint64_t k_ps_lbl = prompts_state_key();
    g_fam = 3; g_letter = 0;
    uint64_t k_xb_pos = prompts_state_key();
    g_letter = 1;
    uint64_t k_xb_lbl = prompts_state_key();
    g_fam = 4; g_mouse = 1;
    uint64_t k_kb1 = prompts_state_key();
    g_mouse = 0;
    uint64_t k_kb2 = prompts_state_key();
    g_kb[0] = 'K';
    uint64_t k_kb3 = prompts_state_key();
    g_kb[0] = 0x02; g_mouse = 1;

    CHECK(k_sw != k_ps_pos && k_ps_pos != k_ps_lbl, "PS pos/lbl keys distinct from Switch");
    CHECK(k_ps_pos != k_xb_pos && k_xb_pos != k_xb_lbl, "Xbox pos/lbl keys distinct from PS");
    CHECK(k_kb1 != k_kb2 && k_kb2 != k_kb3, "KB key reflects mouse_camera and keybind changes");

    /* 2. Unknown buffers are never falsely identified */
    uint8_t *zeros = calloc(1, 524288);
    CHECK(prompts_identify(TEX_FMT_RGBA8, 256, 256, 1, 1, 262144, zeros) == 0, "zero 256x256 RGBA8 not identified");
    CHECK(prompts_identify(TEX_FMT_RGBA_DXT5, 512, 1024, 1, 1, 524288, zeros) == 0, "zero 512x1024 BC3 not identified");
    CHECK(prompts_identify(TEX_FMT_RGBA8, 128, 64, 1, 1, 32768, zeros) == 0, "zero 128x64 RGBA8 not identified");
    CHECK(prompts_identify(TEX_FMT_RGBA8, 256, 256, 2, 1, 262144, zeros) == 0, "mipmapped texture ignored");
    free(zeros);

    /* 3. core_sys_btn patching: Switch untouched; PS/Xbox pos vs lbl swap face colors; non-button icons untouched */
    uint8_t *base = malloc(256 * 256 * 4);
    uint8_t *buf  = malloc(256 * 256 * 4);
    memset(base, 0x5A, 256 * 256 * 4);

    memcpy(buf, base, 256 * 256 * 4);
    prompts_patch_ex(1, buf, 256, 256, TEX_FMT_RGBA8, 1, 0, 1, g_kb);
    CHECK(memcmp(buf, base, 256 * 256 * 4) == 0, "Switch mode leaves core_sys_btn untouched");

    /* PlayStation positional: A (96,208) = Circle (red), B (96,168) = Cross (blue) */
    memcpy(buf, base, 256 * 256 * 4);
    prompts_patch_ex(1, buf, 256, 256, TEX_FMT_RGBA8, 2, 0, 1, g_kb);
    CHECK(rect_has_color(buf, 256 * 4, 96, 208, 32, 32, 0) > 20, "PS positional: A slot is red Circle");
    CHECK(rect_has_color(buf, 256 * 4, 96, 168, 32, 32, 2) > 20, "PS positional: B slot is blue Cross");
    /* icon_kick at (212,0,32,32) must remain untouched! */
    CHECK(rect_diff(buf, base, 256 * 4, 212, 0, 32, 32) == 0, "PS keeps non-button icon_kick untouched");

    /* PlayStation printed-letter: A (96,208) = Cross (blue), B (96,168) = Circle (red) */
    memcpy(buf, base, 256 * 256 * 4);
    prompts_patch_ex(1, buf, 256, 256, TEX_FMT_RGBA8, 2, 1, 1, g_kb);
    CHECK(rect_has_color(buf, 256 * 4, 96, 208, 32, 32, 2) > 20, "PS labels: A slot is blue Cross");
    CHECK(rect_has_color(buf, 256 * 4, 96, 168, 32, 32, 0) > 20, "PS labels: B slot is red Circle");

    /* Xbox positional: A (96,208) = B (red), B (96,168) = A (green) */
    memcpy(buf, base, 256 * 256 * 4);
    prompts_patch_ex(1, buf, 256, 256, TEX_FMT_RGBA8, 3, 0, 1, g_kb);
    CHECK(rect_has_color(buf, 256 * 4, 96, 208, 32, 32, 0) > 15, "Xbox positional: A slot is red B");
    CHECK(rect_has_color(buf, 256 * 4, 96, 168, 32, 32, 1) > 15, "Xbox positional: B slot is green A");

    /* Xbox printed-letter: A (96,208) = A (green), B (96,168) = B (red) */
    memcpy(buf, base, 256 * 256 * 4);
    prompts_patch_ex(1, buf, 256, 256, TEX_FMT_RGBA8, 3, 1, 1, g_kb);
    CHECK(rect_has_color(buf, 256 * 4, 96, 208, 32, 32, 1) > 15, "Xbox labels: A slot is green A");
    CHECK(rect_has_color(buf, 256 * 4, 96, 168, 32, 32, 0) > 15, "Xbox labels: B slot is red B");

    /* Keyboard: all 5 regional layouts patch cleanly without touching icon_hold/icon_air/etc. */
    for (int id = 1; id <= 5; id++) {
        memcpy(buf, base, 256 * 256 * 4);
        prompts_patch_ex(id, buf, 256, 256, TEX_FMT_RGBA8, 4, 0, 1, g_kb);
        CHECK(memcmp(buf, base, 256 * 256 * 4) != 0, "KB patches core_sys_btn regional layout");
    }
    free(base); free(buf);

    /* 4. hud1_main BC3 partial patching: only the 4 action button rects change, unrelated blocks stay bit-exact */
    uint8_t *bc3_base = malloc(524288);
    uint8_t *bc3_buf  = malloc(524288);
    memset(bc3_base, 0x33, 524288);
    memcpy(bc3_buf, bc3_base, 524288);
    prompts_patch_ex(6, bc3_buf, 512, 1024, TEX_FMT_RGBA_DXT5, 2, 0, 1, g_kb);
    /* Block (0, 0) is far from all 4 action buttons -> must be bit-exact untouched */
    CHECK(memcmp(bc3_buf, bc3_base, 16 * 32) == 0, "hud1_main preserves unrelated BC3 blocks bit-exact");
    /* Block at (400, 16) = block (100, 4) is inside hud1_actbutton_ZL -> must change */
    size_t zl_off = ((size_t)4 * 128 + 100) * 16;
    CHECK(memcmp(bc3_buf + zl_off, bc3_base + zl_off, 16) != 0, "hud1_main patches hud1_actbutton_ZL BC3 blocks");
    free(bc3_base); free(bc3_buf);

    /* 5. tutorial_btn: untouched on PS/Xbox, patched on KB */
    uint8_t *tut_base = malloc(128 * 64 * 4);
    uint8_t *tut_buf  = malloc(128 * 64 * 4);
    memset(tut_base, 0x42, 128 * 64 * 4);
    memcpy(tut_buf, tut_base, 128 * 64 * 4);
    prompts_patch_ex(7, tut_buf, 128, 64, TEX_FMT_RGBA8, 2, 0, 1, g_kb);
    CHECK(memcmp(tut_buf, tut_base, 128 * 64 * 4) == 0, "tutorial_btn untouched on PlayStation");
    prompts_patch_ex(7, tut_buf, 128, 64, TEX_FMT_RGBA8, 4, 0, 1, g_kb);
    CHECK(rect_diff(tut_buf, tut_base, 128 * 4, 0, 0, 38, 38) > 100, "tutorial_btn btn_029 patched on KB");
    CHECK(rect_diff(tut_buf, tut_base, 128 * 4, 40, 0, 38, 38) > 100, "tutorial_btn btn_030 patched on KB");
    CHECK(rect_diff(tut_buf, tut_base, 128 * 4, 80, 0, 48, 64) == 0, "tutorial_btn unused area untouched");
    free(tut_base); free(tut_buf);

    printf("prompts_test: %s (%d failures)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
