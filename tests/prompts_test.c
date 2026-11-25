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
int prompts_apply_menu_swap_at(int want_swap, uint32_t *swap_flag, uint32_t *confirm_bits,
                               uint32_t *cancel_bits, uint32_t *guide_raw,
                               int (*make_rw)(void *, size_t));

static int g_fam = 1, g_letter = 0, g_mouse = 1, g_swap = 0;
static int g_kb[24] = {
    0x02, 0x20, 0x01, 'E', 'C', 0x04, 'R', 'F', 'Q', 0xA0, 0x1B, 0x09,
    0x25, 0x26, 0x27, 0x28, 'A', 'W', 'D', 'S', 'J', 'I', 'L', 'K'
};
int nnpad_button_prompts(void) { return g_fam; }
int nnpad_prompts_by_letter(void) { return g_letter; }
int nnpad_menu_swapped(void) { return g_swap && g_fam != 4; }
int nnpad_mouse_cfg(float *s, int *i) { if (s) *s = 1.0f; if (i) *i = 0; return g_mouse; }
int nnpad_kb_vk(int idx) { return idx >= 0 && idx < 24 ? g_kb[idx] : 0; }

static int g_rw_calls = 0, g_rw_allow = 1;
static int fake_make_rw(void *p, size_t n) { (void)p; (void)n; g_rw_calls++; return g_rw_allow; }

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

    /* 6. In-game menu swap_confirm: syncs system flag, confirm/cancel bitmasks, and [0]/[1] guide hashes */
    {
        const uint32_t PIC_A = 0x1760995eu, PIC_B = 0x0e69c8e4u;
        uint32_t flag = 0, confirm = 0, cancel = 0;
        uint32_t guide[16] = {
            0, PIC_A, 0x40e00000u, 0x42200000u,   /* [0] Confirm */
            1, PIC_B, 0x40e00000u, 0x42200000u,   /* [1] Cancel */
            2, PIC_B, 0x40e00000u, 0x42200000u,   /* [2] B (gameplay) */
            3, PIC_A, 0x40e00000u, 0x42200000u,   /* [3] A (gameplay) */
        };
        g_rw_calls = 0; g_rw_allow = 1;
        /* Uninitialized UI masks (0, 0) must be ignored */
        CHECK(prompts_apply_menu_swap_at(1, &flag, &confirm, &cancel, guide, fake_make_rw) == 0,
              "swap_confirm ignores uninitialized confirm/cancel masks");
        CHECK(flag == 0 && guide[1] == PIC_A && guide[5] == PIC_B && g_rw_calls == 0,
              "swap_confirm leaves guest memory untouched before UI init");

        /* Enable swap_confirm after UI init (confirm=0x10, cancel=0x20) */
        confirm = 0x10u; cancel = 0x20u;
        CHECK(prompts_apply_menu_swap_at(1, &flag, &confirm, &cancel, guide, fake_make_rw) == 1,
              "swap_confirm=on reports change");
        CHECK(flag == 1u && confirm == 0x20u && cancel == 0x10u,
              "swap_confirm=on sets system flag=1, confirm=B(0x20), cancel=A(0x10)");
        CHECK(guide[1] == PIC_B && guide[5] == PIC_A && g_rw_calls == 1,
              "swap_confirm=on swaps guide [0]/[1] hashes");
        CHECK(guide[9] == PIC_B && guide[13] == PIC_A && guide[2] == 0x40e00000u && guide[6] == 0x40e00000u,
              "swap_confirm=on preserves gameplay [2]/[3] hashes and float fields");

        /* Calling again with same state is a no-op */
        CHECK(prompts_apply_menu_swap_at(1, &flag, &confirm, &cancel, guide, fake_make_rw) == 0,
              "swap_confirm=on steady state is a no-op");
        CHECK(g_rw_calls == 1, "steady state does not re-call make_rw");

        /* Self-heals if the game resets the system swap flag */
        flag = 0;
        CHECK(prompts_apply_menu_swap_at(1, &flag, &confirm, &cancel, guide, fake_make_rw) == 1 && flag == 1u,
              "swap_confirm self-heals if system flag is cleared");

        /* Disable swap_confirm restores A=confirm, B=cancel and original guide icons */
        CHECK(prompts_apply_menu_swap_at(0, &flag, &confirm, &cancel, guide, fake_make_rw) == 1,
              "swap_confirm=off reports change");
        CHECK(flag == 0u && confirm == 0x10u && cancel == 0x20u,
              "swap_confirm=off restores flag=0, confirm=A(0x10), cancel=B(0x20)");
        CHECK(guide[1] == PIC_A && guide[5] == PIC_B && g_rw_calls == 2,
              "swap_confirm=off restores guide [0]/[1] hashes");

        /* Failed make_rw skips guide write without corrupting hashes */
        g_rw_allow = 0;
        CHECK(prompts_apply_menu_swap_at(1, &flag, &confirm, &cancel, guide, fake_make_rw) == 1,
              "swap_confirm still updates writable masks if make_rw fails");
        CHECK(flag == 1u && confirm == 0x20u && cancel == 0x10u && guide[1] == PIC_A && guide[5] == PIC_B,
              "failed make_rw leaves guide hashes intact");
    }

    printf("prompts_test: %s (%d failures)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
