// Metroid Prime 4 Beyond front end and in-game menu: shared theme, widgets and the settings table.
// Original design: dark translucent panels, cyan lines, red accents, Bahnschrift (a Windows font, not bundled).
#pragma once
#include "imgui.h"
#include <string>
#include <vector>
#include <functional>

namespace ui {

// ---------------------------------------------------------------- colours
// The theme sets these (theme_apply); C_RED is the accent and C_CYAN the line colour whatever their hue.
// C_OK, C_WARN and C_BAD are status colours and stay the same in every theme.
extern ImU32 C_BG0, C_BG1, C_PANEL, C_PANEL_HI, C_LINE, C_CYAN, C_CYAN_DIM, C_RED, C_RED_HI, C_TEXT, C_DIM;
extern const ImU32 C_OK, C_WARN, C_BAD;
ImU32 alpha(ImU32 c, int a);   // c with its alpha replaced
ImU32 surface(float k);        // an opaque panel tone: the background mixed with the line colour, k = 0 to 1

// ---------------------------------------------------------------- themes (the launcher's look, as data)
struct Theme {
    const char* key; const char* label;
    ImU32 bg0, bg1, panel, panel_hi, line, cyan, red, red_hi, text, dim;
};
const std::vector<Theme>& themes();   // the built-in ones; "beyond" first (the default)
// key: a built-in theme, or "custom" to read custom_path (key=#RRGGBB lines, missing keys from "beyond").
// Returns false (and keeps "beyond") when the key is unknown or the custom file can't be read; *note says why.
bool theme_apply(const std::string& key, const std::string& custom_path, std::string* note);
std::string theme_template();          // a commented starter theme.ini (the "beyond" colours)
float contrast(ImU32 a, ImU32 b);      // WCAG contrast ratio, 1 to 21
extern bool g_reduce_motion;           // no turning rings, no moving progress stripes

// ---------------------------------------------------------------- fonts / style
struct Fonts { ImFont* body = nullptr; ImFont* head = nullptr; };
extern Fonts g_fonts;
void load_fonts(ImGuiIO& io);
void apply_style(float scale);
float S(float px);            // px at 1080p -> current scale
extern float g_scale;

// ---------------------------------------------------------------- controller navigation (both UIs)
// Physical button positions, the runtime's order: 0 right face, 1 bottom face, 2 top face, 3 left face, 4 L3, 5 R3,
// 6 L, 7 R, 8 ZL, 9 ZR, 10 +, 11 -, 12 left, 13 up, 14 right, 15 down.
enum NavAction { NAV_CONFIRM, NAV_BACK, NAV_PREV, NAV_NEXT, NAV_PLAY, NAV_COUNT };
extern const char* const NAV_NAMES[NAV_COUNT];
extern const int NAV_DEFAULT[NAV_COUNT];
extern const char* const POSITION_NAMES[16];
struct Nav {
    int bind[NAV_COUNT] = {1, 0, 6, 7, 10};
    bool enabled = true;
    // filled each frame by update()
    bool up = false, down = false, left = false, right = false;   // edge + repeat
    bool act[NAV_COUNT] = {};                                      // edge
    bool any_pad = false;                                          // a pad was used last (prompts on)
    void update(unsigned long long phys, int lx, int ly, double now);
    void feed_imgui(ImGuiIO& io);                                  // up/down + confirm/back into ImGui's gamepad nav
    bool reset_hold(double now, unsigned long long phys);          // hold + and - for 3 s: default bindings back
    bool valid() const;
private:
    unsigned long long prev = 0; int dir = 0, pdir = 0; double next_rep = 0, reset_t0 = 0;
    bool pm_chord = false;   // + and - were held together: no action from them (the reset gesture)
};
std::string bindings_to_string(const int b[NAV_COUNT]);
bool bindings_from_string(const char* s, int b[NAV_COUNT]);

enum PromptStyle { PROMPT_AUTO, PROMPT_XBOX, PROMPT_PS, PROMPT_NINTENDO, PROMPT_POSITIONS };
PromptStyle prompt_style_from(const char* s);
const char* prompt_style_key(PromptStyle p);
PromptStyle prompt_resolve(PromptStyle p, const char* pad_name);
void draw_prompt(ImDrawList* dl, ImVec2 c, float r, int position, PromptStyle style);
float prompt_bar(const std::vector<std::pair<int, const char*>>& items, PromptStyle style, ImVec2 pos);   // returns width

// ---------------------------------------------------------------- settings table (shared by both UIs)
enum OptType { OPT_BOOL, OPT_CHOICE, OPT_FLOAT, OPT_INT };
enum OptLive { LIVE_YES, LIVE_RESTART, LIVE_GL };   // LIVE_GL: live with OpenGL / Vulkan output, restart with Native Vulkan
struct Choice { const char* value; const char* label; };
struct Opt {
    const char* key; const char* section; const char* label; const char* help;
    OptType type; OptLive live;
    std::vector<Choice> choices;      // OPT_CHOICE
    float lo = 0, hi = 1, step = 1;   // OPT_FLOAT / OPT_INT
    const char* def = "0";
    const char* page = "";
    bool menu = true;                 // shown in the in-game menu
};
const std::vector<Opt>& options();
const Opt* find_option(const char* key);

// ---------------------------------------------------------------- row widgets (console-style option rows)
// Every row is one focusable item: up/down moves, left/right (or a click) changes the value.
// vertical controller navigation done here, not by ImGui (whose gamepad nav needs the window to have OS focus):
// every focusable row registers itself (nav_item), and after the frame nav_move(+1 down / -1 up) moves the focus
void nav_frame_begin();
void nav_item();
void nav_move(int dir);
extern bool g_focus_first;   // the next row / big button takes the focus (a page or tab just opened)
struct RowCtx { bool pad_left = false, pad_right = false; bool confirm = false; };
extern RowCtx g_row;
bool row_bool(const char* id, const char* label, bool* v, const char* tag = nullptr);
bool row_choice(const char* id, const char* label, int* idx, const std::vector<std::string>& items, const char* tag = nullptr);
bool row_slider(const char* id, const char* label, float* v, float lo, float hi, float step, const char* value_text,
                const char* tag = nullptr);
bool row_button(const char* id, const char* label, const char* value = nullptr, bool primary = false);
void row_text(const char* label, const char* value, ImU32 col = 0);
void help_text(const char* s);
void section(const char* title);
void page_title(const char* title, const char* sub);
bool big_button(const char* id, const char* label, ImVec2 size, bool primary = true);
void chamfer_rect(ImDrawList* dl, ImVec2 a, ImVec2 b, float cut, ImU32 fill, ImU32 border, float thick = 1.0f);
void background(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, bool translucent);
void logo(ImDrawList* dl, ImVec2 pos, float size, bool with_tag);
void progress_bar(float frac, ImVec2 size, const char* text);

// helpers
std::string fmt(const char* f, ...);
float game_base_w(bool docked, const char* aspect);
float game_base_h(bool docked, const char* aspect);
std::string res_text(float scale, bool docked, const char* aspect);   // "2.00× (3200 × 1800)"

} // namespace ui
