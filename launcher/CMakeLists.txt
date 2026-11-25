# The Moon River launcher. Builds the game from the player's dump (mrbuild setup) and plays it in the same window
# (build/libmoonriver.so). SDL3 + RmlUi on SDL's GPU API; Linux, Windows (llvm-mingw) and macOS (the game runs as
# build/moon-river-game there, as on Windows).
#
#     cmake -S launcher -B build/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release
#     cmake --build build/launcher                      -> build/moon-river-launcher (Windows: build/moon-river-launcher.exe,
#                                                          and moon-river-launcher.exe at the top, which starts it)
#
# Needs the system's SDL3 (3.4 or newer) and FreeType. RmlUi is fetched at the pinned tag below, or taken from
# -DRMLUI_SOURCE_DIR=<folder> (an RmlUi checkout of that tag) when there is no network.
cmake_minimum_required(VERSION 3.20)
project(moon_river_launcher LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL3 REQUIRED CONFIG)
find_package(Freetype REQUIRED)

set(RMLUI_TAG 6.3)
set(RMLUI_SOURCE_DIR "" CACHE PATH "An RmlUi ${RMLUI_TAG} checkout to use instead of fetching it")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE freetype CACHE STRING "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
include(FetchContent)
if(RMLUI_SOURCE_DIR)
	FetchContent_Declare(rmlui SOURCE_DIR "${RMLUI_SOURCE_DIR}")
else()
	FetchContent_Declare(rmlui GIT_REPOSITORY https://github.com/mikke89/RmlUi.git GIT_TAG ${RMLUI_TAG} GIT_SHALLOW TRUE)
endif()
FetchContent_MakeAvailable(rmlui)
set(RMLUI_BACKENDS "${rmlui_SOURCE_DIR}/Backends")

add_executable(moon-river-launcher
	src/main.cpp
	src/app.cpp
	src/build_runner.cpp
	src/config.cpp
	src/game_host.cpp
	src/platform.cpp
	src/ui_render.cpp
	src/update.cpp
	"${RMLUI_BACKENDS}/RmlUi_Platform_SDL.cpp"
	"${RMLUI_BACKENDS}/RmlUi_Renderer_SDL_GPU.cpp")
# src/shim: <SDL3_image/SDL_image.h> for RmlUi's SDL GPU renderer, without the SDL3_image library (ui_render.cpp
# draws procedural textures; PNG files are loaded through SDL3's built-in PNG loader)
target_include_directories(moon-river-launcher PRIVATE src src/shim "${RMLUI_BACKENDS}")
set(MR_VERSION "dev build" CACHE STRING "The version shown in the launcher (release builds pass the tag, e.g. v0.1.0)")
target_compile_definitions(moon-river-launcher PRIVATE RMLUI_SDL_VERSION_MAJOR=3 MR_VERSION="${MR_VERSION}")
target_link_libraries(moon-river-launcher PRIVATE RmlUi::Core SDL3::SDL3 ${CMAKE_DL_LIBS})
# beside moon-river-game, game.so and libmoonriver.so: the launcher finds the repository from its own place (build/..)
# $ORIGIN: a release ships libSDL3.so.0 beside it, which libmoonriver.so (also $ORIGIN) then shares
if(APPLE)
	set(MR_ORIGIN "@loader_path")
else()
	set(MR_ORIGIN "$ORIGIN")
endif()
set_target_properties(moon-river-launcher PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../build"
	BUILD_RPATH "${MR_ORIGIN}")
if(APPLE)
	# build/Moon River.app: the same program in a bundle, for Finder and the Dock (macos/make_app.sh)
	configure_file(macos/Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
	add_custom_command(TARGET moon-river-launcher POST_BUILD
		COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/macos/make_app.sh" "$<TARGET_FILE:moon-river-launcher>" "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
			"${CMAKE_CURRENT_SOURCE_DIR}/ui/img/icon.png" "${CMAKE_CURRENT_SOURCE_DIR}/../build/Moon River.app"
		VERBATIM)
endif()
if(WIN32)
	# a window program (no console), with UTF-8 as its code page (win32/launcher.rc); C++ runtime linked in, so that it
	# needs nothing beside it but SDL3.dll
	target_sources(moon-river-launcher PRIVATE win32/launcher.rc)
	set_target_properties(moon-river-launcher PROPERTIES WIN32_EXECUTABLE ON)
	target_link_options(moon-river-launcher PRIVATE -static)
	target_link_libraries(moon-river-launcher PRIVATE ole32 uuid)    # the desktop shortcut (IShellLink, platform.cpp)
	# moon-river-launcher.exe at the top of a release: starts build/moon-river-launcher.exe (Windows has no symbolic link to ship there)
	add_executable(moon-river-launcher-start WIN32 win32/start.c win32/launcher.rc)
	target_compile_options(moon-river-launcher-start PRIVATE -municode)
	target_link_options(moon-river-launcher-start PRIVATE -static -municode)
	set_target_properties(moon-river-launcher-start PROPERTIES OUTPUT_NAME moon-river-launcher RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
