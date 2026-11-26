/* [moonriver] Gameplay button rebinding (Bayonetta 2 v1.2).
 *
 * A custom layout is a bijection r[16] over Nintendo Pro Controller buttons:
 *   r[b] = physical Pro button index that triggers Type A's logical button b.
 * Menus keep their own buttons (and swap_confirm); only gameplay input and
 * gameplay prompts follow r[]. Keyboard & mouse bindings stay in [keyboard] of
 * controllers.ini, so r[] is only active when a controller is in use.
 *
 * Pro button numbering matches nnpad.h / nnhid.c:
 *   0 A, 1 B, 2 X, 3 Y, 4 STICK_L, 5 STICK_R, 6 L, 7 R, 8 ZL, 9 ZR,
 *   10 PLUS, 11 MINUS, 12 LEFT, 13 UP, 14 RIGHT, 15 DOWN.
 *
 * Bayonetta 2 internal pad bitmask (f_7100d9f83c / f_7100d9eef4, c_0122.c):
 *   low 16 bits: 0x0001 Left, 0x0002 Right, 0x0004 Down, 0x0008 Up,
 *                0x0010 A, 0x0020 B, 0x0040 X, 0x0080 Y,
 *                0x0100 Plus, 0x0200 Minus,
 *                0x0400 L, 0x0800 ZL, 0x1000 L3, 0x2000 R, 0x4000 ZR, 0x8000 R3.
 *   bits 24..27: non-exclusive D-pad Left/Right/Down/Up (0x01000000..0x08000000).
 *   bits 16..23: left/right stick directions; bit 28: touch (preserved as-is). */
#ifndef MOONRIVER_REBIND_H
#define MOONRIVER_REBIND_H

#include <stdint.h>

enum {
    RB_A = 0, RB_B, RB_X, RB_Y,
    RB_STICK_L, RB_STICK_R,
    RB_L, RB_R, RB_ZL, RB_ZR,
    RB_PLUS, RB_MINUS,
    RB_LEFT, RB_UP, RB_RIGHT, RB_DOWN
};

/* Bayonetta 2 low-16-bit mask for each Pro button 0..15. */
static const uint32_t rb_game_bit[16] = {
    0x0010u, 0x0020u, 0x0040u, 0x0080u,   /* A, B, X, Y */
    0x1000u, 0x8000u,                     /* STICK_L, STICK_R */
    0x0400u, 0x2000u, 0x0800u, 0x4000u,   /* L, R, ZL, ZR */
    0x0100u, 0x0200u,                     /* PLUS, MINUS */
    0x0001u, 0x0008u, 0x0002u, 0x0004u    /* LEFT, UP, RIGHT, DOWN */
};

/* Bayonetta 2 non-exclusive D-pad high bits (bits 24..27) for Pro buttons 0..15. */
static const uint32_t rb_dpad_hi[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x01000000u, 0x08000000u, 0x02000000u, 0x04000000u
};

/* Bayonetta 2 inline button-prompt table IDs (0x7101537d88, searched by f_7100bd1b0c)
 * for each Pro button 0..15. IDs 0 (Confirm) and 1 (Cancel) are menu-only and untouched. */
static const uint8_t rb_pro_to_btn_id[16] = {
    3,  2,  5,  4,    /* A, B, X, Y */
    20, 18,           /* STICK_L, STICK_R */
    8,  6,  9,  7,    /* L, R, ZL, ZR */
    21, 22,           /* PLUS, MINUS */
    15, 13, 16, 14    /* LEFT, UP, RIGHT, DOWN */
};

/* Check that r[0..15] is a valid permutation of 0..15. */
static inline int rb_valid(const uint8_t r[16]) {
    unsigned seen = 0;
    for (int i = 0; i < 16; i++) {
        if (r[i] >= 16) return 0;
        seen |= 1u << r[i];
    }
    return seen == 0xffffu;
}

/* Fill out[0..15] with Bayonetta 2's built-in Control Type (0=A, 1=B, 2=C, 3=D).
 * Verified against save_obj+36 action table (c_0121.c) and f_7100bee668:
 *   Type A: L=Umbran Climax, ZL=Weapon Set, R=Lock-On,      ZR=Dodge
 *   Type B: L=Umbran Climax, R=Weapon Set,  ZL=Lock-On,     ZR=Dodge
 *   Type C: ZL=Umbran Climax, L=Weapon Set, ZR=Lock-On,     R=Dodge
 *   Type D: R=Umbran Climax, ZR=Weapon Set, L=Lock-On,      ZL=Dodge */
static inline void rb_preset(int type, uint8_t out[16]) {
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)i;
    switch (type) {
    case 1: /* Type B: swap R <-> ZL */
        out[RB_R] = RB_ZL; out[RB_ZL] = RB_R;
        break;
    case 2: /* Type C: swap L <-> ZL and R <-> ZR */
        out[RB_L] = RB_ZL; out[RB_ZL] = RB_L;
        out[RB_R] = RB_ZR; out[RB_ZR] = RB_R;
        break;
    case 3: /* Type D: swap L <-> R and ZL <-> ZR */
        out[RB_L] = RB_R;  out[RB_R]  = RB_L;
        out[RB_ZL] = RB_ZR; out[RB_ZR] = RB_ZL;
        break;
    default: /* Type A: identity */
        break;
    }
}

/* Return 0..3 if r[] matches Type A..D, or -1 if custom. */
static inline int rb_which_preset(const uint8_t r[16]) {
    uint8_t p[16];
    for (int t = 0; t < 4; t++) {
        rb_preset(t, p);
        int eq = 1;
        for (int i = 0; i < 16; i++) if (r[i] != p[i]) { eq = 0; break; }
        if (eq) return t;
    }
    return -1;
}

/* Assign physical button `to` to logical slot `b`, swapping with whichever slot
 * currently holds `to` so r[] remains a valid permutation. */
static inline void rb_swap(uint8_t r[16], int b, uint8_t to) {
    if (b < 0 || b >= 16 || to >= 16 || r[b] == to) return;
    for (int i = 0; i < 16; i++) {
        if (r[i] == to) { r[i] = r[b]; break; }
    }
    r[b] = to;
}

/* Remap a Bayonetta 2 physical pad word into logical Type A gameplay bits.
 * Non-button bits (sticks 0x00ff0000, touch 0x10000000) pass through unchanged. */
static inline uint32_t rb_to_game(const uint8_t r[16], uint32_t w) {
    uint32_t out = w & ~0x0f00ffffu;
    for (int b = 0; b < 16; b++) {
        uint8_t p = r[b];
        if (w & (rb_game_bit[p] | rb_dpad_hi[p]))
            out |= rb_game_bit[b] | rb_dpad_hi[b];
    }
    return out;
}

/* Inverse of rb_to_game: logical Type A gameplay bits -> physical pad word. */
static inline uint32_t rb_to_pad(const uint8_t r[16], uint32_t w) {
    uint32_t out = w & ~0x0f00ffffu;
    for (int b = 0; b < 16; b++) {
        if (w & (rb_game_bit[b] | rb_dpad_hi[b])) {
            uint8_t p = r[b];
            out |= rb_game_bit[p] | rb_dpad_hi[p];
        }
    }
    return out;
}

/* Map a Bayonetta 2 guide table button ID (0x7101537d88) to Pro button 0..15, or -1. */
static inline int rb_btn_id_to_pro(uint32_t id) {
    switch (id) {
    case 3:  return RB_A;
    case 2:  return RB_B;
    case 5:  return RB_X;
    case 4:  return RB_Y;
    case 20: return RB_STICK_L;
    case 18: return RB_STICK_R;
    case 8:  return RB_L;
    case 6:  return RB_R;
    case 9:  return RB_ZL;
    case 7:  return RB_ZR;
    case 21: return RB_PLUS;
    case 22: return RB_MINUS;
    case 15: return RB_LEFT;
    case 13: return RB_UP;
    case 16: return RB_RIGHT;
    case 14: return RB_DOWN;
    default: return -1;
    }
}

/* Remap a Bayonetta 2 guide table button ID through r[].
 * Leaves menu Confirm (0), Cancel (1), stick icons, and multi-way D-pad icons untouched. */
static inline uint32_t rb_btn_id(const uint8_t r[16], uint32_t id) {
    int b = rb_btn_id_to_pro(id);
    return b >= 0 ? (uint32_t)rb_pro_to_btn_id[r[b]] : id;
}

#endif
