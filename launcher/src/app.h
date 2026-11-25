// The launcher: one SDL window, RmlUi screens on SDL's GPU API (ui/launcher.rml), the build (BuildRunner) and the start
// of the game (game_host). Screens: title menu, first run (the dump), building, mods, settings.
#pragma once
#include "build_runner.h"
#include "config.h"
#include "RmlUi_Platform_SDL.h"
#include "ui_render.h"
#include <RmlUi/Core.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/* test aid: MR_SLOWMO=<n> slows every animation n times (RmlUi's clock), so frames of a 0.3 s motion can be grabbed */
struct SlowSystem : SystemInterface_SDL
{
	double slow = 1;
	explicit SlowSystem(SDL_Window* w) : SystemInterface_SDL(w) { if (const char* e = std::getenv("MR_SLOWMO")) slow = std::max(1.0, std::atof(e)); }
	double GetElapsedTime() override { return SystemInterface_SDL::GetElapsedTime() / slow; }
	// RmlUi's own drops a query (it rebuilds the path from the URL's parts); ui_render.cpp reads query parameters on gen: sources
	void JoinPath(Rml::String& out, const Rml::String& document, const Rml::String& path) override
	{
		const size_t q = path.find('?');
		SystemInterface_SDL::JoinPath(out, document, path.substr(0, q));
		if (q != Rml::String::npos)
			out += path.substr(q);
	}
};

struct CheckRow { Rml::String text, right, state; };
struct StepRow { Rml::String num, name, note, right, state; };
struct ModRow { Rml::String name, note, kind; bool on = true; };
struct SettingRow {
	Rml::String name, note, value;
	int index = 0;
	Rml::String bump = "none";   // the value's nudge animation after a change
	bool changed = false;        // changed in this session (rendered in the warm accent)
};
struct MenuRow {
	Rml::String label, desc, tag;
	std::string action;
	Rml::String transform;       // translateY + scale relative to the selected title menu item
	float opacity = 1.f;
	bool selected = false;
	bool far = false;            // 3+ steps away: hidden and not clickable
};

class App {
public:
	bool init(int argc, char** argv);
	int run();

private:
	Paths paths;
	SDL_Window* window = nullptr;
	SDL_GPUDevice* gpu = nullptr;
	std::unique_ptr<UiRender> render;
	std::unique_ptr<SlowSystem> system;
	TextInputMethodEditor_SDL ime;
	Rml::Context* ctx = nullptr;
	Rml::DataModelHandle model;
	std::vector<SDL_Gamepad*> pads;
	int prompts = 4;                // footer hints: 1 Switch, 2 PlayStation, 3 Xbox, 4 keyboard
	bool prompts_fixed = false;     // [general] button_prompts names one; else they follow what was used last
	int stick_x = 0, stick_y = 0;   // left-stick directional state for navigation
	double stick_next = 0;
	void prompts_read();
	void prompts_use(int family);
	void prompts_use(SDL_Gamepad* pad);
	std::vector<std::vector<Rml::byte>> font_data;

	// what the screens show (the data model "mr")
	Rml::String screen = "title";
	bool built = false, status_bad = false;
	Rml::String moon_src, shimmer_src, moon_phase_name;
	std::time_t moon_checked_at = 0;
	int moon_phase_q = -1;             // quantized phase (0..999)
	bool moon_dim_last = false;
	Rml::String status_title, status_line, menu_desc, footer_info, setup_footer, build_footer, version;
	std::vector<MenuRow> menu;
	int menu_sel = 0;
	Rml::String update_tag;            // a newer release to install, or empty
	bool stale = false;                // built by another release than this launcher
	int dump_mode = 1;                 // setup: 0 extracted folder, 1 NSP / XCI packages
	Rml::String dump, update_pkg, prod_keys;
	bool checking = false, verified = false;
	bool move_dump = false;            // setup moves the dump's game data into the build folder instead of copying it
	bool dump_modified = false;        // the last check found files that differ from the supported version
	bool accept_modified = false;      // setup runs with --accept-modified: such a dump is used anyway
	std::vector<CheckRow> checks;
	bool building = false, converting = false;   // converting: the textures (building is true then too)
	float progress = 0;
	int percent = 0;
	Rml::String phase, build_aside, build_line, build_error;
	bool show_log = false;             // the build screen's log box
	std::vector<StepRow> steps;
	std::vector<Rml::String> log;
	std::vector<ModRow> mods;
	std::vector<ModEntry> mod_entries;
	Rml::String mods_summary, mods_error;
	bool mods_rebuild = false;
	std::vector<SettingRow> settings;  // the rows of the tab shown
	int tab = 0;                       // settings: 0 Display, 1 Graphics, 2 Controls
	bool leaving = false;              // a screen is on its way out (its leaving animations play); next_screen then opens
	Rml::String next_screen;
	double leave_at = 0;
	int bump_count[64] = {};           // per setting row: how many changes, signed by the last one
	bool session_changed[64] = {};     // per setting row: changed in this session (warm accent)
	Rml::String app_name = APP_NAME;
	Rml::String tex_note, tex_action, tex_switch;  // settings: the converted textures (#54); tex_switch: "Turn off"/"Turn on" (#136)
	bool tex_remove = false;
	Rml::String shortcut_note = "Starts the game straight away.";   // settings: the desktop shortcut (#143), then where it is

	// work for the main loop
	BuildRunner runner;
	enum class Job { none, verify, build, textures } job = Job::none;
	bool running = true, want_play = false, want_focus = true;
	int program = 0;                   // the program the next Play starts (game_host.h; a program switch sets it)
	bool after_switch = false;
	bool play_only = false;            // --play: the game starts at once, and the launcher ends with it
	bool update_offered = false;       // --play held for a newer release once; Play then starts the game and still ends with it
	bool play_after_stop = false;      // Play during the texture conversion: it is stopped, and the game starts when it has
	bool typed = false;                // the dump folder's box was typed or clicked in since the screen opened
	int returned_code = -1;            // --returned <code>: the game just ended
	std::mutex picked_mutex;
	std::string picked_folder;         // from the file/folder dialog (another thread)
	int picked_target = 0;             // 0 = dump, 1 = update_pkg, 2 = prod_keys
	bool picked = false;
	SDL_AudioStream* sound_stream = nullptr;   // navigation sounds (issue 141): open from the first one until Play
	bool sound_failed = false;
	Rml::Element* last_focus = nullptr;        // the control the focus was in at the last frame
	int update_seen = 0;               // the update's state and line as the title last showed them
	Rml::String update_message;

	bool create_window();
	bool create_ui();
	void bind_model();
	void handle(SDL_Event& ev);
	bool key(Rml::Input::KeyIdentifier k);
	void hover_focus();
	bool hint_click(float x);
	void back();
	void tick();
	void tick_moon(bool force = false);
	void tick_stick();
	void draw();
	void save_shot(const char* path);
	void rescale();
	void focus_first();

	void go(const Rml::String& to);
	void go_now(const Rml::String& to);
	void select_menu(int index, bool play_sound = false);
	void activate_menu(int index);
	void refresh_menu_layout();
	void refresh_title();
	void refresh_build();
	void refresh_mods();
	void refresh_settings(int bumped = -1);
	void set_tab(int t);
	void set_dump_mode(int mode);
	void step_setting(int index, int dir);
	void convert_textures();
	void remove_textures();
	void switch_textures();
	void toggle_mod(int index);
	void check_dump();
	void start_build();
	void browse(int target = 0);
	void play();
	void release_window();
	void sound(int which);
	void close_sound();
};
