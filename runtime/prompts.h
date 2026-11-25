/* In-game controller / keyboard button prompt atlas replacement (games/<game>/runtime/prompts.c).
 * Called on the render thread from nvn_gl.c (for both Vulkan and OpenGL backends). */
#ifndef PROMPTS_H
#define PROMPTS_H
#include <stddef.h>
#include <stdint.h>

/* Current prompt state key (prompt family, face-button layout, active keyboard bindings): changes whenever the
 * in-game button prompt atlases need to be re-uploaded. Cheap (called per-batch on recognized prompt atlases). */
uint64_t prompts_state_key(void);

/* Returns >0 (game-defined atlas id) when this texture's dimensions, format and swizzled bytes match a known
 * unmodified button prompt atlas, else 0. External RomFS prompt mods have different bytes and return 0. */
int prompts_identify(int nvn_format, int w, int h, int levels, int layers, size_t data_size, const uint8_t *swizzled);

/* Patches level 0 tightly-packed texels (RGBA8 or BC3 blocks) in-place for the current prompt family / bindings. */
void prompts_patch(int atlas_id, uint8_t *linear, int w, int h, int nvn_format);

/* Keeps the game's in-game menu confirm/cancel masks and menu button-guide icons in sync with swap_confirm.
 * Called from player 1's pad poll (nnhid.c) after device-use tracking updates. */
void prompts_menu_swap_tick(void);

#endif
