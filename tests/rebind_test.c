/* Standalone unit test for Moon River gameplay controller button rebinding (runtime/rebind.h).
 * Build & run:
 *   clang -O2 -Wall -Wextra -I runtime tests/rebind_test.c -o /tmp/rebind_test && /tmp/rebind_test */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rebind.h"

static int g_pass, g_fail;
static void check(int ok, const char *what) {
    if (ok) g_pass++;
    else { g_fail++; printf("  FAIL: %s\n", what); }
}

int main(void) {
    uint8_t r[16];

    /* 1. Bayonetta 2 Control Types A-D presets match save_obj+36 action table (c_0121.c) */
    for (int t = 0; t < 4; t++) {
        rb_preset(t, r);
        check(rb_valid(r), "preset is a valid permutation");
        check(rb_which_preset(r) == t, "rb_which_preset identifies Type A-D");
    }
    /* Type A: identity */
    rb_preset(0, r);
    check(r[RB_L] == RB_L && r[RB_ZL] == RB_ZL && r[RB_R] == RB_R && r[RB_ZR] == RB_ZR,
          "Type A: L=Umbran Climax, ZL=Weapon Set, R=Lock-On, ZR=Dodge");
    /* Type B: R <-> ZL */
    rb_preset(1, r);
    check(r[RB_L] == RB_L && r[RB_ZL] == RB_R && r[RB_R] == RB_ZL && r[RB_ZR] == RB_ZR,
          "Type B: L=Umbran Climax, R=Weapon Set, ZL=Lock-On, ZR=Dodge");
    /* Type C: L <-> ZL, R <-> ZR */
    rb_preset(2, r);
    check(r[RB_L] == RB_ZL && r[RB_ZL] == RB_L && r[RB_R] == RB_ZR && r[RB_ZR] == RB_R,
          "Type C: ZL=Umbran Climax, L=Weapon Set, ZR=Lock-On, R=Dodge");
    /* Type D: L <-> R, ZL <-> ZR */
    rb_preset(3, r);
    check(r[RB_L] == RB_R && r[RB_ZL] == RB_ZR && r[RB_R] == RB_L && r[RB_ZR] == RB_ZL,
          "Type D: R=Umbran Climax, ZR=Weapon Set, L=Lock-On, ZL=Dodge");

    /* 2. rb_valid rejects duplicates and out-of-range entries */
    rb_preset(0, r);
    r[2] = r[3];
    check(!rb_valid(r), "rb_valid rejects duplicate button");
    rb_preset(0, r);
    r[0] = 16;
    check(!rb_valid(r), "rb_valid rejects out-of-range button");

    /* 3. rb_swap preserves bijection and swaps with previous owner */
    rb_preset(0, r);
    rb_swap(r, RB_ZR, RB_B); /* Dodge -> B, Jump -> ZR */
    check(rb_valid(r) && r[RB_ZR] == RB_B && r[RB_B] == RB_ZR && rb_which_preset(r) == -1,
          "rb_swap swaps Dodge (ZR) and Jump (B) cleanly and reports Custom (-1)");

    /* 4. rb_to_game / rb_to_pad exact round-trip across all buttons, chords, D-pad high bits, and sticks */
    {
        /* Non-trivial layout: swap Punch(X)<->L, Dodge(ZR)<->B, Item Left(LEFT)<->Y, Pause(PLUS)<->MINUS */
        rb_preset(0, r);
        rb_swap(r, RB_X, RB_L);
        rb_swap(r, RB_ZR, RB_B);
        rb_swap(r, RB_LEFT, RB_Y);
        rb_swap(r, RB_PLUS, RB_MINUS);
        check(rb_valid(r), "custom multi-swap layout is valid");

        int all_ok = 1;
        for (int b = 0; b < 16; b++) {
            uint32_t phys = rb_game_bit[r[b]] | rb_dpad_hi[r[b]];
            uint32_t expect = rb_game_bit[b] | rb_dpad_hi[b];
            uint32_t got = rb_to_game(r, phys);
            uint32_t back = rb_to_pad(r, got);
            if (got != expect || back != phys) all_ok = 0;
        }
        check(all_ok, "rb_to_game / rb_to_pad round-trip every button 0..15 including D-pad high bits");

        /* Non-button bits (sticks 0x00ff0000 and touch 0x10000000) pass through untouched */
        uint32_t sticks = 0x10a50000u;
        uint32_t chord = sticks | rb_game_bit[RB_L] | rb_game_bit[RB_B]; /* physical L + B -> logical X (Punch) + ZR (Dodge) */
        uint32_t out = rb_to_game(r, chord);
        check(out == (sticks | rb_game_bit[RB_X] | rb_game_bit[RB_ZR]),
              "rb_to_game preserves stick/touch bits and remaps chords");
        check(rb_to_pad(r, out) == chord, "rb_to_pad inverts chord with stick/touch bits");

        /* Physical D-pad high bit alone (e.g. simultaneous diagonal) triggers logical button */
        uint32_t only_hi = rb_dpad_hi[RB_LEFT]; /* physical LEFT -> logical Y */
        check(rb_to_game(r, only_hi) == rb_game_bit[RB_Y],
              "rb_to_game accepts non-exclusive D-pad high bit");
    }

    /* 5. rb_btn_id preserves menu Confirm(0)/Cancel(1) and non-button IDs, remaps all 16 gameplay IDs */
    {
        rb_preset(0, r);
        rb_swap(r, RB_ZR, RB_B);     /* Dodge(id 7) -> B(id 2), Jump(id 2) -> ZR(id 7) */
        rb_swap(r, RB_X, RB_L);      /* Punch(id 5) -> L(id 8), Umbran Climax(id 8) -> X(id 5) */
        rb_swap(r, RB_PLUS, RB_MINUS);

        check(rb_btn_id(r, 0) == 0 && rb_btn_id(r, 1) == 1,
              "rb_btn_id never touches menu Confirm (0) or Cancel (1)");
        check(rb_btn_id(r, 10) == 10 && rb_btn_id(r, 17) == 17 && rb_btn_id(r, 19) == 19,
              "rb_btn_id leaves D-pad combo and stick IDs untouched");
        check(rb_btn_id(r, 7) == 2 && rb_btn_id(r, 2) == 7,
              "rb_btn_id swaps Dodge (7) and Jump (2)");
        check(rb_btn_id(r, 5) == 8 && rb_btn_id(r, 8) == 5,
              "rb_btn_id swaps Punch (5) and Umbran Climax (8)");
        check(rb_btn_id(r, 21) == 22 && rb_btn_id(r, 22) == 21,
              "rb_btn_id swaps Plus (21) and Minus (22)");

        /* All 16 Pro buttons round-trip through rb_pro_to_btn_id / rb_btn_id_to_pro */
        int id_ok = 1;
        for (int b = 0; b < 16; b++) {
            if (rb_btn_id_to_pro(rb_pro_to_btn_id[b]) != b) id_ok = 0;
        }
        check(id_ok, "rb_pro_to_btn_id and rb_btn_id_to_pro are exact inverses");
    }

    printf("rebind_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
