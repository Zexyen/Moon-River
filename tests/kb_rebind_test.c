/* Standalone test of keyboard & mouse rebinding persistence in runtime/nnpad_sdl.c:
 * section-aware INI edits ([keyboard] vs [general] / [device ...]), primary+secondary key round-trip
 * (including mouse buttons MOUSE1..MOUSE5, modifiers, punctuation), mouse camera settings, and full reset.
 *
 * Build & run:
 *   clang -O2 -Wall -Wextra -D_GNU_SOURCE -D'__declspec(x)=' -Iruntime/posix -Iruntime \
 *         tests/kb_rebind_test.c runtime/nnpad_sdl.c runtime/plat_posix.c -lSDL3 -lm -o /tmp/kb_rebind_test && /tmp/kb_rebind_test */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "nnpad.h"

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    assert(b != NULL);
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = 0;
    return b;
}

static void spit(const char *path, const char *s) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    fputs(s, f);
    fclose(f);
}

int main(void) {
    const char *ini = "/tmp/mr19_kb_rebind_test.ini";
    remove(ini);
    setenv("MR_CONFIG", ini, 1);

    /* 1. Default file created automatically; nnpad_kb_is_default() is true */
    assert(nnpad_kb_is_default() == 1);
    assert(nnpad_kb_vk(0) == (VK_RBUTTON | (VK_RETURN << 8)));
    assert(nnpad_kb_vk(1) == (VK_SPACE | (VK_BACK << 8)));
    assert(nnpad_kb_vk(2) == VK_LBUTTON);
    assert(nnpad_kb_vk(9) == VK_LSHIFT);

    /* 2. Edit primary on A (Kick/Confirm) while keeping secondary Enter, and put Mouse 2 on ZR (Dodge) */
    assert(nnpad_kb_set(0, VK_LSHIFT | (VK_RETURN << 8)) == 1);
    assert(nnpad_kb_set(9, VK_RBUTTON) == 1);
    assert(nnpad_kb_vk(0) == (VK_LSHIFT | (VK_RETURN << 8)));
    assert(nnpad_kb_vk(9) == VK_RBUTTON);
    assert(nnpad_kb_is_default() == 0);

    /* 3. Verify [keyboard] lines on disk, keeping comments and other sections intact */
    char *txt = slurp(ini);
    assert(strstr(txt, "A = LSHIFT, ENTER\r\n") != NULL);
    assert(strstr(txt, "ZR = MOUSE2\r\n") != NULL);
    assert(strstr(txt, "[general]\r\n") != NULL);
    assert(strstr(txt, "[players]\r\n") != NULL);
    assert(strstr(txt, "; kick; confirm in menus\r\n") != NULL);
    free(txt);

    /* 4. Secondary promotion when primary is cleared (0 | vk2<<8 -> vk2), dedupe (vk | vk<<8 -> vk), and unbind (0 -> none) */
    assert(nnpad_kb_set(0, VK_RETURN << 8) == 1);
    assert(nnpad_kb_vk(0) == VK_RETURN);
    assert(nnpad_kb_set(3, 'G' | ('G' << 8)) == 1);
    assert(nnpad_kb_vk(3) == 'G');
    assert(nnpad_kb_set(4, 0) == 1);
    assert(nnpad_kb_vk(4) == 0);
    txt = slurp(ini);
    assert(strstr(txt, "A = ENTER\r\n") != NULL);
    assert(strstr(txt, "Y = G\r\n") != NULL);
    assert(strstr(txt, "STICK_L = none\r\n") != NULL);
    free(txt);

    /* 5. Mouse camera settings persist under [keyboard] without colliding with [general] */
    assert(nnpad_mouse_cam_set("off") == 1);
    assert(nnpad_mouse_sens_set("2.0") == 1);
    assert(nnpad_mouse_inv_set("on") == 1);
    float sens = 0.0f; int inv = 0;
    assert(nnpad_mouse_cfg(&sens, &inv) == 0);
    assert(fabsf(sens - 2.0f) < 0.01f && inv == 1);
    txt = slurp(ini);
    assert(strstr(txt, "mouse_camera = off\r\n") != NULL);
    assert(strstr(txt, "mouse_sensitivity = 2.0\r\n") != NULL);
    assert(strstr(txt, "mouse_invert_y = on\r\n") != NULL);
    free(txt);

    /* 6. Extra keys: Mouse 5, Num 7, F6, Del (0x2E), semicolon (0xBA) round-trip through parse_vk */
    assert(nnpad_kb_set(8, VK_XBUTTON2 | (0x2E << 8)) == 1);
    assert(nnpad_kb_set(6, (VK_NUMPAD0 + 7) | ((VK_F1 + 5) << 8)) == 1);
    assert(nnpad_kb_set(7, 0xBA) == 1);
    txt = slurp(ini);
    assert(strstr(txt, "ZL = MOUSE5, 0x2E\r\n") != NULL);
    assert(strstr(txt, "L = NUM7, F6\r\n") != NULL);
    assert(strstr(txt, "R = 0xBA\r\n") != NULL);
    free(txt);

    /* 7. Reset restores all 25 bindings + mouse settings to built-in defaults */
    assert(nnpad_kb_reset() == 1);
    assert(nnpad_kb_is_default() == 1);
    assert(nnpad_kb_vk(0) == nnpad_kb_default_vk(0));
    assert(nnpad_kb_vk(8) == nnpad_kb_default_vk(8));
    assert(nnpad_mouse_cfg(&sens, &inv) == 1 && fabsf(sens - 1.0f) < 0.01f && inv == 0);

    /* 8. Section isolation & LF preservation: a minimal LF file with a [device] section after [keyboard] */
    spit(ini, "[general]\nlayout = auto\n[keyboard]\nX = MOUSE1\n[device 1234]\nname = Pad\n");
    assert(nnpad_kb_set(0, VK_RBUTTON | (VK_RETURN << 8)) == 1);
    assert(nnpad_mouse_sens_set("1.5") == 1);
    txt = slurp(ini);
    assert(!strstr(txt, "\r\n"));
    const char *p_kb = strstr(txt, "[keyboard]\n");
    const char *p_a = strstr(txt, "A = MOUSE2, ENTER\n");
    const char *p_ms = strstr(txt, "mouse_sensitivity = 1.5\n");
    const char *p_dev = strstr(txt, "[device 1234]\n");
    assert(p_kb && p_a && p_ms && p_dev && p_kb < p_a && p_a < p_dev && p_kb < p_ms && p_ms < p_dev);
    free(txt);

    remove(ini);
    printf("kb_rebind_test: ALL PASSED\n");
    return 0;
}
