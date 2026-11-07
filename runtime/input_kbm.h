/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   Shared keyboard and mouse layer: Copyright 2026 Zorkats, same licence. */
/* Keyboard and mouse as a Switch controller, shared by the window backends (backend_win32.c, backend_x11.c).
   A backend only translates its native events: keys to KBM_KEY_* codes, raw mouse movement to counts, mouse buttons
   to KBM_MOUSE_*. This module owns the bindings, the mouse look and the [Input] mouse_* settings.
   Threads: the event functions may run on the window thread; kbm_keyboard / kbm_mouse run on the input thread
   (video_poll). */
#ifndef SWR_INPUT_KBM_H
#define SWR_INPUT_KBM_H
#include "rt.h"

/* key codes: letters 'A'..'Z' and digits '0'..'9' are their ASCII upper-case codes, space is ' '.
   Punctuation keys are named by their place on a US keyboard (GRAVE is the key left of 1 on any layout) */
enum {
  KBM_KEY_NONE = 0,
  KBM_KEY_SPACE = ' ',
  KBM_KEY_ENTER = 128,
  KBM_KEY_BACKSPACE,
  KBM_KEY_LEFT,
  KBM_KEY_UP,
  KBM_KEY_RIGHT,
  KBM_KEY_DOWN,
  KBM_KEY_TAB,
  KBM_KEY_GRAVE,
  KBM_KEY_SHIFT,
  KBM_KEY_CTRL,
  KBM_KEY_ALT,
  KBM_KEY_CAPSLOCK,
  KBM_KEY_INSERT,
  KBM_KEY_DELETE,
  KBM_KEY_HOME,
  KBM_KEY_END,
  KBM_KEY_PAGEUP,
  KBM_KEY_PAGEDOWN,
  KBM_KEY_DASH,
  KBM_KEY_EQUALS,
  KBM_KEY_LBRACKET,
  KBM_KEY_RBRACKET,
  KBM_KEY_BACKSLASH,
  KBM_KEY_SEMICOLON,
  KBM_KEY_APOSTROPHE,
  KBM_KEY_COMMA,
  KBM_KEY_PERIOD,
  KBM_KEY_SLASH,
  KBM_KEY_NUM0,   /* .. KBM_KEY_NUM9 */
  KBM_KEY_NUM_ADD = KBM_KEY_NUM0 + 10,
  KBM_KEY_NUM_SUB,
  KBM_KEY_NUM_MUL,
  KBM_KEY_NUM_DIV,
  KBM_KEY_NUM_DECIMAL,
  KBM_KEY_LAST,
  KBM_KEY_COUNT = 256
};
/* mouse buttons, in the order [Input] mouse_buttons lists them */
enum { KBM_MOUSE_LEFT, KBM_MOUSE_RIGHT, KBM_MOUSE_MIDDLE, KBM_MOUSE_BACK, KBM_MOUSE_FORWARD, KBM_MOUSE_COUNT };

void kbm_key(int key, int down);
void kbm_mouse_motion(int dx, int dy);   /* relative device counts, screen axes (y down) */
void kbm_mouse_button(int button, int down);
void kbm_release_all(void);              /* focus lost, the in-game menu opened */
int kbm_mouse_look_enabled(void);        /* [Input] mouse_look: the backend captures and hides the cursor over the game */

/* the keyboard's buttons (bits 0..15) and stick axes from the [Input] key_* bindings; a stick the keyboard leaves at
   rest stays 0 */
void kbm_keyboard(u64* buttons, i32* lx, i32* ly, i32* rx, i32* ry);
/* the mouse onto the polled state: its buttons, and mouse look on the right stick while the mouse moves.
   active = 0 (the in-game menu is open) drops the movement and the clicks, so nothing jumps when it closes */
void kbm_mouse(u64* buttons, i32* rx, i32* ry, int active);
/* mouse aim (mouse_aim.py): the code module's exports, or NULL when it has none. While the game's look update takes
   the counts (steps advances), kbm_mouse leaves the right stick alone; otherwise it moves the stick as before */
typedef struct {
  volatile int* pend;          /* [2] counts the look update hasn't taken yet, x right, y down */
  volatile float* deg;         /* [2] degrees per count */
  volatile u32* steps;         /* look updates that took the counts */
  volatile float* mult;        /* the game's look multiplier at the last one */
} KbmAim;
void kbm_aim_bind(const KbmAim* aim);

void mouse_map_reset(void);              /* [Input] mouse_buttons changed (main.c ini_apply) */

/* ---- bindings: [Input] key_<action> = chord, chord, ...   a chord is keys joined by +, e.g. "Shift+1"; - = none.
   A chord counts while all its keys are held, unless a longer held chord contains all of its keys (so with
   "E" and "E+1" bound, holding E and 1 presses only the second). */
int kbm_action_count(void);
const char* kbm_action_key(int i);       /* "key_a" */
const char* kbm_action_label(int i);     /* "A", "D-pad up", ... */
const char* kbm_action_default(int i);
int kbm_action_find(const char* ini_key);   /* -1 when it is not a key_* option */
int kbm_binding_set(const char* ini_key, const char* value);   /* 1 = a key_* option (main.c ini_apply) */
void kbm_binding_get(int i, char* out, int cap);               /* the binding in use, as written in the .ini */
const char* kbm_key_name(int key);       /* "Shift", "A", ...; "" for none */
int kbm_key_from_name(const char* name); /* KBM_KEY_NONE when unknown */

/* ---- press-a-key capture for the in-game menu's Controls page.
   The window side feeds every key event to kbm_capture_key before anything else; it returns 1 when the capture took
   the event (the menu must not see it). The menu thread starts it, and polls kbm_capture_take for the chord:
   modifiers held plus the first other key, or a modifier pressed and let go alone. */
int kbm_capture_key(int key, int down);
void kbm_capture_begin(void);
void kbm_capture_cancel(void);
int kbm_capture_active(void);
int kbm_capture_take(char* out, int cap);   /* 1 = done, out = the chord's name ("Ctrl+1") */

#endif
