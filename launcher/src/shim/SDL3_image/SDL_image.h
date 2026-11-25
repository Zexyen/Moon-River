// Stand-in for SDL3_image, which RmlUi's SDL GPU renderer includes for its image files. The launcher draws most of its
// pictures procedurally (ui_render.cpp) and loads its PNG logo and icon through SDL3's built-in PNG loader (SDL 3.4+).
#pragma once
#include <SDL3/SDL.h>

static inline SDL_Surface* IMG_LoadTyped_IO(SDL_IOStream* src, bool closeio, const char* type)
{
#if SDL_VERSION_ATLEAST(3, 4, 0)
	if (type && SDL_strcasecmp(type, "png") == 0)
		return SDL_LoadPNG_IO(src, closeio);
#endif
	(void)type;
	return SDL_LoadBMP_IO(src, closeio);
}
