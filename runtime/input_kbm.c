/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   Shared keyboard and mouse layer: Copyright 2026 Zorkats, same licence. */
/* Keyboard and mouse as a Switch controller (input_kbm.h): the [Input] key_* bindings with their PrimeHack-style
   defaults, the press-a-key capture the in-game menu rebinds with, and mouse look with [Input] mouse_buttons. */
#include "input_kbm.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

extern int g_opt_mouse_look, g_opt_mouse_sens, g_opt_mouse_invert_y, g_opt_mouse_deadzone;   /* main.c [Input] */
extern char g_opt_mouse_buttons[];

/* ---------------------------------------------------------------- keyboard */
/* Switch button bits 0..15; 40..47 are stick directions, turned into axes in kbm_keyboard */
enum { STICK_L_LEFT = 40, STICK_L_UP, STICK_L_RIGHT, STICK_L_DOWN, STICK_R_LEFT, STICK_R_UP, STICK_R_RIGHT, STICK_R_DOWN };

/* The default layout follows PrimeHack's Metroid Prime defaults where Prime 4 has the same action: Space jumps (B),
   Shift locks on (ZL), Ctrl morphs (Y), E and Alt are the visor and spring ball button (X), 1-4 pick the D-pad
   weapons, Tab or ` pauses (-). The left mouse button fires (A, [Input] mouse_buttons). */
typedef struct { const char* key; const char* label; int bit; const char* def; } KbmAction;
static const KbmAction ACTIONS[] = {
  {"key_a", "A", 0, "Enter"},
  {"key_b", "B", 1, "Space"},
  {"key_x", "X", 2, "E, Alt"},
  {"key_y", "Y", 3, "Ctrl"},
  {"key_l", "L", 6, "Q"},
  {"key_r", "R", 7, "F"},
  {"key_zl", "ZL", 8, "Shift"},
  {"key_zr", "ZR", 9, "-"},
  {"key_plus", "+", 10, "R"},
  {"key_minus", "-", 11, "Tab, Grave"},
  {"key_lstick_click", "Left stick click", 4, "-"},
  {"key_rstick_click", "Right stick click", 5, "-"},
  {"key_dpad_up", "D-pad up", 13, "3"},
  {"key_dpad_down", "D-pad down", 15, "4"},
  {"key_dpad_left", "D-pad left", 12, "2"},
  {"key_dpad_right", "D-pad right", 14, "1"},
  {"key_lstick_up", "Move forward", STICK_L_UP, "W, Up"},
  {"key_lstick_down", "Move back", STICK_L_DOWN, "S, Down"},
  {"key_lstick_left", "Move left", STICK_L_LEFT, "A, Left"},
  {"key_lstick_right", "Move right", STICK_L_RIGHT, "D, Right"},
  {"key_rstick_up", "Look up", STICK_R_UP, "I"},
  {"key_rstick_down", "Look down", STICK_R_DOWN, "K"},
  {"key_rstick_left", "Look left", STICK_R_LEFT, "J"},
  {"key_rstick_right", "Look right", STICK_R_RIGHT, "L"},
};
#define KBM_NACT ((int)(sizeof ACTIONS / sizeof *ACTIONS))
#define KBM_CHORDS 6        /* per action */
#define KBM_CHORD_KEYS 4    /* per chord */
#define KBM_TEXT 128

int kbm_action_count(void){ return KBM_NACT; }
const char* kbm_action_key(int i){ return i >= 0 && i < KBM_NACT ? ACTIONS[i].key : ""; }
const char* kbm_action_label(int i){ return i >= 0 && i < KBM_NACT ? ACTIONS[i].label : ""; }
const char* kbm_action_default(int i){ return i >= 0 && i < KBM_NACT ? ACTIONS[i].def : ""; }
int kbm_action_find(const char* ini_key){
  for(int i = 0; i < KBM_NACT; i++) if(!strcmp(ACTIONS[i].key, ini_key)) return i;
  return -1;
}

/* ---- key names, as the .ini and the menu write them */
static const struct { int key; const char* name; } KEY_NAMES[] = {
  {KBM_KEY_SPACE, "Space"}, {KBM_KEY_ENTER, "Enter"}, {KBM_KEY_BACKSPACE, "Backspace"}, {KBM_KEY_TAB, "Tab"},
  {KBM_KEY_GRAVE, "Grave"}, {KBM_KEY_SHIFT, "Shift"}, {KBM_KEY_CTRL, "Ctrl"}, {KBM_KEY_ALT, "Alt"},
  {KBM_KEY_LEFT, "Left"}, {KBM_KEY_UP, "Up"}, {KBM_KEY_RIGHT, "Right"}, {KBM_KEY_DOWN, "Down"},
  {KBM_KEY_CAPSLOCK, "CapsLock"}, {KBM_KEY_INSERT, "Insert"}, {KBM_KEY_DELETE, "Delete"}, {KBM_KEY_HOME, "Home"},
  {KBM_KEY_END, "End"}, {KBM_KEY_PAGEUP, "PageUp"}, {KBM_KEY_PAGEDOWN, "PageDown"}, {KBM_KEY_DASH, "Dash"},
  {KBM_KEY_EQUALS, "Equals"}, {KBM_KEY_LBRACKET, "LBracket"}, {KBM_KEY_RBRACKET, "RBracket"},
  {KBM_KEY_BACKSLASH, "Backslash"}, {KBM_KEY_SEMICOLON, "Semicolon"}, {KBM_KEY_APOSTROPHE, "Apostrophe"},
  {KBM_KEY_COMMA, "Comma"}, {KBM_KEY_PERIOD, "Period"}, {KBM_KEY_SLASH, "Slash"},
  {KBM_KEY_NUM0, "Num0"}, {KBM_KEY_NUM0 + 1, "Num1"}, {KBM_KEY_NUM0 + 2, "Num2"}, {KBM_KEY_NUM0 + 3, "Num3"},
  {KBM_KEY_NUM0 + 4, "Num4"}, {KBM_KEY_NUM0 + 5, "Num5"}, {KBM_KEY_NUM0 + 6, "Num6"}, {KBM_KEY_NUM0 + 7, "Num7"},
  {KBM_KEY_NUM0 + 8, "Num8"}, {KBM_KEY_NUM0 + 9, "Num9"}, {KBM_KEY_NUM_ADD, "NumAdd"}, {KBM_KEY_NUM_SUB, "NumSub"},
  {KBM_KEY_NUM_MUL, "NumMul"}, {KBM_KEY_NUM_DIV, "NumDiv"}, {KBM_KEY_NUM_DECIMAL, "NumDecimal"},
};
/* other spellings the .ini accepts */
static const struct { const char* alias; int key; } KEY_ALIASES[] = {
  {"`", KBM_KEY_GRAVE}, {"Return", KBM_KEY_ENTER}, {"Control", KBM_KEY_CTRL}, {"Minus", KBM_KEY_DASH},
};
static const char LETTERS[] = "A\0" "B\0" "C\0" "D\0" "E\0" "F\0" "G\0" "H\0" "I\0" "J\0" "K\0" "L\0" "M\0" "N\0" "O\0"
                              "P\0" "Q\0" "R\0" "S\0" "T\0" "U\0" "V\0" "W\0" "X\0" "Y\0" "Z";
static const char DIGITS[] = "0\0" "1\0" "2\0" "3\0" "4\0" "5\0" "6\0" "7\0" "8\0" "9";

const char* kbm_key_name(int key){
  if(key >= 'A' && key <= 'Z') return LETTERS + 2 * (key - 'A');
  if(key >= '0' && key <= '9') return DIGITS + 2 * (key - '0');
  for(size_t i = 0; i < sizeof KEY_NAMES / sizeof *KEY_NAMES; i++) if(KEY_NAMES[i].key == key) return KEY_NAMES[i].name;
  return "";
}

static int name_eq(const char* a, const char* b){
  while(*a && *b){
    if(tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    a++;
    b++;
  }
  return !*a && !*b;
}

int kbm_key_from_name(const char* name){
  if(name[0] && !name[1]){
    int c = toupper((unsigned char)name[0]);
    if((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
  }
  for(size_t i = 0; i < sizeof KEY_NAMES / sizeof *KEY_NAMES; i++) if(name_eq(name, KEY_NAMES[i].name)) return KEY_NAMES[i].key;
  for(size_t i = 0; i < sizeof KEY_ALIASES / sizeof *KEY_ALIASES; i++) if(name_eq(name, KEY_ALIASES[i].alias)) return KEY_ALIASES[i].key;
  return KBM_KEY_NONE;
}

/* ---- the bindings in use. Whatever thread applies settings (main.c ini_apply) writes the text and marks it dirty;
   the input thread parses it into chords at its next poll. g_bind_lock guards the text */
typedef struct { unsigned char key[KBM_CHORD_KEYS]; unsigned char n; } KbmChord;
static char g_bind_text[KBM_NACT][KBM_TEXT];
static unsigned char g_bind_from_ini[KBM_NACT];
static volatile char g_bind_lock;
static volatile int g_bind_dirty = 1;
static KbmChord g_chords[KBM_NACT][KBM_CHORDS];   /* input thread */
static int g_nchords[KBM_NACT];

static void bind_lock(void){ while(__atomic_test_and_set(&g_bind_lock, __ATOMIC_ACQUIRE)) {} }
static void bind_unlock(void){ __atomic_clear(&g_bind_lock, __ATOMIC_RELEASE); }

int kbm_binding_set(const char* ini_key, const char* value){
  int i = kbm_action_find(ini_key);
  if(i < 0) return 0;
  bind_lock();
  snprintf(g_bind_text[i], KBM_TEXT, "%s", value);
  g_bind_from_ini[i] = 1;
  bind_unlock();
  __atomic_store_n(&g_bind_dirty, 1, __ATOMIC_RELEASE);
  return 1;
}

void kbm_binding_get(int i, char* out, int cap){
  if(cap <= 0) return;
  out[0] = 0;
  if(i < 0 || i >= KBM_NACT) return;
  bind_lock();
  snprintf(out, (size_t)cap, "%s", g_bind_from_ini[i] ? g_bind_text[i] : ACTIONS[i].def);
  bind_unlock();
}

/* "E, Alt+1, -" -> chords; a chord with an unknown key name is dropped, and logged */
static int chords_parse(const char* action, const char* s, KbmChord* out){
  int n = 0;
  while(*s && n < KBM_CHORDS){
    char tok[64];
    int len = 0;
    while(*s == ' ' || *s == '\t') s++;
    while(*s && *s != ',' && len < 63) tok[len++] = *s++;
    while(*s && *s != ',') s++;
    if(*s == ',') s++;
    while(len > 0 && (tok[len - 1] == ' ' || tok[len - 1] == '\t')) len--;
    tok[len] = 0;
    if(!len || !strcmp(tok, "-")) continue;
    KbmChord c = {{0}, 0};
    int ok = 1;
    for(const char* p = tok; *p;){
      char name[32];
      int k = 0;
      while(*p == ' ') p++;
      while(*p && *p != '+' && k < 31) name[k++] = *p++;
      while(k > 0 && name[k - 1] == ' ') k--;
      name[k] = 0;
      if(*p == '+') p++;
      int key = kbm_key_from_name(name);
      if(key == KBM_KEY_NONE || c.n == KBM_CHORD_KEYS){
        ok = 0;
        break;
      }
      c.key[c.n++] = (unsigned char)key;
    }
    if(ok && c.n) out[n++] = c;
    else LOG("input: %s: \"%s\" is not a key or chord this build knows, ignored\n", action, tok);
  }
  return n;
}

static void bindings_rebuild(void){
  if(!__atomic_exchange_n(&g_bind_dirty, 0, __ATOMIC_ACQ_REL)) return;
  for(int i = 0; i < KBM_NACT; i++){
    char text[KBM_TEXT];
    kbm_binding_get(i, text, sizeof text);
    g_nchords[i] = chords_parse(ACTIONS[i].key, text, g_chords[i]);
  }
}

/* per key, not per Switch button: Tab and ` both press -, and letting go of one must not release the other.
   tapped keeps a press that came and went between two polls, so a quick tap still reaches the game */
static volatile unsigned char g_key_held[KBM_KEY_COUNT], g_key_tapped[KBM_KEY_COUNT];

void kbm_key(int key, int down){
  if(key <= KBM_KEY_NONE || key >= KBM_KEY_COUNT) return;
  g_key_held[key] = down ? 1 : 0;
  if(down) g_key_tapped[key] = 1;
}

static i32 keys_axis(u64 dirs, int neg, int pos){
  return ((dirs >> pos) & 1 ? 32767 : 0) - ((dirs >> neg) & 1 ? 32767 : 0);
}

/* every key of chord a is in the longer chord b */
static int chord_within(const KbmChord* a, const KbmChord* b){
  if(a->n >= b->n) return 0;
  for(int i = 0; i < a->n; i++){
    int found = 0;
    for(int j = 0; j < b->n; j++) found |= a->key[i] == b->key[j];
    if(!found) return 0;
  }
  return 1;
}

void kbm_keyboard(u64* buttons, i32* lx, i32* ly, i32* rx, i32* ry){
  bindings_rebuild();
  unsigned char down[KBM_KEY_COUNT];
  for(int k = 0; k < KBM_KEY_COUNT; k++){
    down[k] = g_key_held[k] | g_key_tapped[k];
    g_key_tapped[k] = 0;
  }
  const KbmChord* on[KBM_NACT * KBM_CHORDS];
  int on_bit[KBM_NACT * KBM_CHORDS], non = 0;
  for(int i = 0; i < KBM_NACT; i++)
    for(int c = 0; c < g_nchords[i]; c++){
      const KbmChord* ch = &g_chords[i][c];
      int all = 1;
      for(int k = 0; k < ch->n; k++) all &= down[ch->key[k]] != 0;
      if(all){
        on[non] = ch;
        on_bit[non++] = ACTIONS[i].bit;
      }
    }
  u64 bits = 0;
  for(int i = 0; i < non; i++){
    int covered = 0;
    for(int j = 0; j < non && !covered; j++) covered = chord_within(on[i], on[j]);
    if(!covered) bits |= 1ull << on_bit[i];
  }
  *buttons = bits & 0xFFFFull;
  *lx = keys_axis(bits, STICK_L_LEFT, STICK_L_RIGHT);
  *ly = keys_axis(bits, STICK_L_DOWN, STICK_L_UP);
  *rx = keys_axis(bits, STICK_R_LEFT, STICK_R_RIGHT);
  *ry = keys_axis(bits, STICK_R_DOWN, STICK_R_UP);
}

/* ---- press-a-key capture (input_kbm.h). The window side owns g_cap_held, g_cap_ignore, g_cap_mods and
   g_cap_swallow; the menu thread only starts, cancels and takes. g_cap_state: 0 idle, 1 waiting, 2 a chord is ready */
static volatile int g_cap_state;
static unsigned char g_cap_held[KBM_KEY_COUNT], g_cap_ignore[KBM_KEY_COUNT];
static int g_cap_mods, g_cap_swallow;
static char g_cap_out[64];
static const int CAP_MODS[3] = {KBM_KEY_CTRL, KBM_KEY_SHIFT, KBM_KEY_ALT};   /* the order a chord's name lists them */

static int cap_mod_bit(int key){
  for(int i = 0; i < 3; i++) if(CAP_MODS[i] == key) return 1 << i;
  return 0;
}

static void cap_finish(int key){
  char out[64] = "";
  for(int i = 0; i < 3; i++)
    if(g_cap_mods & 1 << i){
      if(out[0]) strcat(out, "+");
      strcat(out, kbm_key_name(CAP_MODS[i]));
    }
  if(key != KBM_KEY_NONE){
    if(out[0]) strcat(out, "+");
    strcat(out, kbm_key_name(key));
  }
  memcpy(g_cap_out, out, sizeof out);
  __atomic_store_n(&g_cap_state, 2, __ATOMIC_RELEASE);
}

int kbm_capture_key(int key, int down){
  if(key <= KBM_KEY_NONE || key >= KBM_KEY_COUNT || !kbm_key_name(key)[0]) return 0;
  int repeat = down && g_cap_held[key];
  g_cap_held[key] = down ? 1 : 0;
  if(!down) g_cap_ignore[key] = 0;
  if(g_cap_swallow == key){   /* the key that finished a capture: its auto-repeat and its release belong to it too */
    if(!down) g_cap_swallow = 0;
    return 1;
  }
  if(__atomic_load_n(&g_cap_state, __ATOMIC_ACQUIRE) != 1) return 0;
  if(down && (repeat || g_cap_ignore[key])) return 1;   /* held from before the capture (the Enter that started it) */
  int mod = cap_mod_bit(key);
  if(down){
    if(mod){
      g_cap_mods |= mod;
      return 1;
    }
    cap_finish(key);
    g_cap_swallow = key;
    return 1;
  }
  if(mod && (g_cap_mods & mod)) cap_finish(KBM_KEY_NONE);   /* a modifier pressed and let go on its own */
  return 1;
}

void kbm_capture_begin(void){
  memcpy(g_cap_ignore, g_cap_held, sizeof g_cap_ignore);
  g_cap_mods = 0;
  __atomic_store_n(&g_cap_state, 1, __ATOMIC_RELEASE);
}

void kbm_capture_cancel(void){ __atomic_store_n(&g_cap_state, 0, __ATOMIC_RELEASE); }

int kbm_capture_active(void){ return __atomic_load_n(&g_cap_state, __ATOMIC_ACQUIRE) == 1; }

int kbm_capture_take(char* out, int cap){
  if(__atomic_load_n(&g_cap_state, __ATOMIC_ACQUIRE) != 2) return 0;
  snprintf(out, (size_t)cap, "%s", g_cap_out);
  __atomic_store_n(&g_cap_state, 0, __ATOMIC_RELEASE);
  return 1;
}

/* ---------------------------------------------------------------- mouse */
/* movement since the last poll and the buttons held (bit per KBM_MOUSE_*); any thread adds, the input thread takes */
static volatile int g_mouse_dx, g_mouse_dy, g_mouse_held, g_mouse_tapped;

void kbm_mouse_motion(int dx, int dy){
  __atomic_fetch_add(&g_mouse_dx, dx, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_mouse_dy, dy, __ATOMIC_RELAXED);
}

void kbm_mouse_button(int button, int down){
  if(button < 0 || button >= KBM_MOUSE_COUNT) return;
  int bit = 1 << button;
  if(down){
    __atomic_fetch_or(&g_mouse_held, bit, __ATOMIC_RELAXED);
    __atomic_fetch_or(&g_mouse_tapped, bit, __ATOMIC_RELAXED);
  } else {
    __atomic_fetch_and(&g_mouse_held, ~bit, __ATOMIC_RELAXED);
  }
}

void kbm_release_all(void){
  for(int i = 0; i < KBM_KEY_COUNT; i++){
    g_key_held[i] = 0;
    g_key_tapped[i] = 0;
  }
  __atomic_store_n(&g_mouse_held, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_mouse_tapped, 0, __ATOMIC_RELAXED);
}

int kbm_mouse_look_enabled(void){ return g_opt_mouse_look; }

/* [Input] mouse_buttons: "left,right,middle,back,forward" as Switch buttons, - for none */
static int g_mouse_bit[KBM_MOUSE_COUNT] = {-2, -2, -2, -2, -2};   /* -2 not parsed yet, -1 none */
void mouse_map_reset(void){ g_mouse_bit[0] = -2; }
static void mouse_map(void){
  if(g_mouse_bit[0] != -2) return;
  static const struct { const char* name; int bit; } NAMES[] = {
    {"a", 0}, {"b", 1}, {"x", 2}, {"y", 3}, {"lstick", 4}, {"rstick", 5}, {"l", 6}, {"r", 7},
    {"zl", 8}, {"zr", 9}, {"plus", 10}, {"minus", 11},
  };
  const char* p = g_opt_mouse_buttons;
  for(int i = 0; i < KBM_MOUSE_COUNT; i++){
    char name[16];
    int n = 0;
    while(*p == ' ') p++;
    while(*p && *p != ',' && n < 15) name[n++] = (char)tolower((unsigned char)*p++);
    while(n > 0 && name[n - 1] == ' ') n--;
    name[n] = 0;
    while(*p && *p != ',') p++;
    if(*p == ',') p++;
    g_mouse_bit[i] = -1;
    for(size_t j = 0; j < sizeof NAMES / sizeof *NAMES; j++)
      if(!strcmp(name, NAMES[j].name)) g_mouse_bit[i] = NAMES[j].bit;
  }
}

/* test only: SWR_TEST_MOUSE="FRAME:DX,DY,BUTTONS;..." adds that movement at that guest frame and holds those buttons
   (hex, 1 left 2 right 4 middle) from there to the next entry */
static void mouse_test(int* dx, int* dy, int* held){
  static const char* s;
  static int init, test_held;
  if(!init){
    init = 1;
    s = getenv("SWR_TEST_MOUSE");
  }
  if(!s) return;
  while(*s){
    char* e;
    long frame = strtol(s, &e, 10);
    if(e == s || *e != ':' || g_frames < frame) break;
    long x = strtol(e + 1, &e, 10);
    if(*e == ',') e++;
    long y = strtol(e, &e, 10);
    if(*e == ',') e++;
    long b = strtol(e, &e, 16);
    *dx += (int)x;
    *dy += (int)y;
    test_held = (int)b;
    s = *e == ';' ? e + 1 : e;
  }
  *held |= test_held;
}

/* Mouse aim (mouse_aim.py builds): the game's look update adds the counts to its motion-control deltas, so a count
   turns the camera by a fixed angle, KBM_AIM_DEG_PER_COUNT at 100% sensitivity. It runs once per logic step and
   counts the steps in *steps; when that stops for KBM_AIM_LIVE_POLLS polls (lock-on, morph ball, menus, a code DLL
   without it) the mouse moves the right stick instead, and the counts it left in *pend are dropped. */
#define KBM_AIM_DEG_PER_COUNT 0.05f
#define KBM_AIM_LIVE_POLLS 6
#define KBM_ZL_BIT 8   /* the Switch ZL button: lock-on */
static KbmAim g_aim;
static u32 g_aim_seen;
static int g_aim_idle = KBM_AIM_LIVE_POLLS;

void kbm_aim_bind(const KbmAim* aim){
  if(aim && aim->pend && aim->deg && aim->steps && aim->mult) g_aim = *aim;
  else memset(&g_aim, 0, sizeof g_aim);
  g_aim_seen = g_aim.steps ? __atomic_load_n(g_aim.steps, __ATOMIC_RELAXED) : 0;
  g_aim_idle = KBM_AIM_LIVE_POLLS;
}

/* once per poll: 1 while the look update took the counts in the last KBM_AIM_LIVE_POLLS polls */
static int aim_live(void){
  if(!g_aim.pend) return 0;
  u32 s = __atomic_load_n(g_aim.steps, __ATOMIC_RELAXED);
  if(s != g_aim_seen){ g_aim_seen = s; g_aim_idle = 0; }
  else if(g_aim_idle < KBM_AIM_LIVE_POLLS) g_aim_idle++;
  return g_aim_idle < KBM_AIM_LIVE_POLLS;
}

static void aim_drop(void){
  if(!g_aim.pend) return;
  for(int i = 0; i < 2; i++) __atomic_store_n(&g_aim.pend[i], 0, __ATOMIC_RELAXED);
}

/* Mouse look on the right stick, where mouse aim isn't running. The game turns the camera at a speed set by how far
   the right stick is pushed, so the mouse's speed (counts per poll, 60 a second) becomes a stick deflection:
   KBM_LOOK_FULL_COUNTS counts in one poll at 100% sensitivity is a full push. The game ignores small pushes (its
   stick dead zone), so any movement starts at [Input] mouse_deadzone percent, and slow mouse movement still turns
   the camera slowly. */
#define KBM_LOOK_FULL_COUNTS 40.0f
static float look_axis(int counts, float gain, float dead){
  if(counts == 0) return 0.0f;
  float push = dead + fabsf((float)counts) * gain * (1.0f - dead);
  if(push > 1.0f) push = 1.0f;
  return counts < 0 ? -push : push;
}

void kbm_mouse(u64* buttons, i32* rx, i32* ry, int active){
  int dx = __atomic_exchange_n(&g_mouse_dx, 0, __ATOMIC_RELAXED);
  int dy = __atomic_exchange_n(&g_mouse_dy, 0, __ATOMIC_RELAXED);
  int held = __atomic_load_n(&g_mouse_held, __ATOMIC_RELAXED) | __atomic_exchange_n(&g_mouse_tapped, 0, __ATOMIC_RELAXED);
  mouse_test(&dx, &dy, &held);
  int live = aim_live();
  if(!active || !g_opt_mouse_look){ aim_drop(); return; }
  mouse_map();
  for(int i = 0; i < KBM_MOUSE_COUNT; i++)
    if((held >> i & 1) && g_mouse_bit[i] >= 0) *buttons |= 1ull << g_mouse_bit[i];
  if(live){
    float deg = KBM_AIM_DEG_PER_COUNT * (float)g_opt_mouse_sens / 100.0f;
    g_aim.deg[0] = deg;
    g_aim.deg[1] = g_opt_mouse_invert_y ? -deg : deg;
    if(dx) __atomic_fetch_add(&g_aim.pend[0], dx, __ATOMIC_RELAXED);
    if(dy) __atomic_fetch_add(&g_aim.pend[1], dy, __ATOMIC_RELAXED);
    if(dx || dy){ static int said; if(!said){ said = 1; LOG("input: mouse aim in use (look multiplier %g)\n", (double)*g_aim.mult); } }
    return;
  }
  aim_drop();
  /* ZL held (from any source) while the look update isn't running: locked on. The game keeps the camera on the
     target, and a right stick push would pull it off, so the mouse does nothing until ZL is released */
  if(g_aim.pend && (*buttons >> KBM_ZL_BIT & 1)) return;
  if(!dx && !dy) return;   /* the mouse is still: the controller's right stick, if any, stays in charge */
  float gain = (float)g_opt_mouse_sens / 100.0f / KBM_LOOK_FULL_COUNTS;
  float dead = (float)g_opt_mouse_deadzone / 100.0f;
  float sx = look_axis(dx, gain, dead);
  float sy = look_axis(-dy, gain, dead);   /* screen y is down, the stick's up */
  if(g_opt_mouse_invert_y) sy = -sy;
  *rx = (i32)(sx * 32767.0f);
  *ry = (i32)(sy * 32767.0f);
  { static int said; if(!said){ said = 1; LOG("input: mouse look on the right stick in use\n"); } }
}
