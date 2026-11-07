/* runtime/input_kbm.c on its own: the default key bindings, [Input] key_* overrides and chords, quick taps, the
   menu's press-a-key capture, mouse look on the right stick and the mouse buttons.
   Run: bash linux/input_kbm_test.sh (prints a FAIL line per mismatch and exits 1). Builds on Windows too:
   gcc -I runtime linux/input_kbm_test.c runtime/input_kbm.c */
#include <stdarg.h>
#include "input_kbm.h"

int g_frames, g_verbose;
int g_opt_mouse_look=1, g_opt_mouse_sens=100, g_opt_mouse_invert_y=0, g_opt_mouse_deadzone=15;
char g_opt_mouse_buttons[64]="a,zl,r,-,-";
void logf_(int l, const char* f, ...){ (void)l; (void)f; }

static int failed;
static void want_u(const char* what, u64 got, u64 want){
  if(got!=want){ printf("FAIL: %s = %#llx (want %#llx)\n",what,(unsigned long long)got,(unsigned long long)want); failed=1; }
  else printf("ok: %s\n",what); }
static void want_i(const char* what, i32 got, i32 want){
  if(got!=want){ printf("FAIL: %s = %d (want %d)\n",what,got,want); failed=1; }
  else printf("ok: %s\n",what); }
static void want_range(const char* what, i32 got, i32 lo, i32 hi){
  if(got<lo || got>hi){ printf("FAIL: %s = %d (want %d..%d)\n",what,got,lo,hi); failed=1; }
  else printf("ok: %s\n",what); }
static void want_s(const char* what, const char* got, const char* want){
  if(strcmp(got,want)){ printf("FAIL: %s = \"%s\" (want \"%s\")\n",what,got,want); failed=1; }
  else printf("ok: %s\n",what); }

typedef struct { u64 b; i32 lx, ly, rx, ry; } Poll;
static Poll keyboard(void){ Poll p; kbm_keyboard(&p.b,&p.lx,&p.ly,&p.rx,&p.ry); return p; }
/* a poll with the controller's right stick at (rx, ry) */
static Poll mouse(i32 rx, i32 ry, int active){ Poll p={0,0,0,rx,ry}; kbm_mouse(&p.b,&p.rx,&p.ry,active); return p; }
/* the buttons while key is held alone */
static u64 key_alone(int key){ kbm_release_all(); kbm_key(key,1); u64 b=keyboard().b; kbm_key(key,0); keyboard(); return b; }

int main(void){
  /* the default (PrimeHack-style) layout */
  want_u("Enter is A",key_alone(KBM_KEY_ENTER),1ull<<0);
  want_u("Space is B",key_alone(KBM_KEY_SPACE),1ull<<1);
  want_u("E is X",key_alone('E'),1ull<<2);
  want_u("Alt is X",key_alone(KBM_KEY_ALT),1ull<<2);
  want_u("Ctrl is Y",key_alone(KBM_KEY_CTRL),1ull<<3);
  want_u("Q is L",key_alone('Q'),1ull<<6);
  want_u("F is R",key_alone('F'),1ull<<7);
  want_u("Shift is ZL",key_alone(KBM_KEY_SHIFT),1ull<<8);
  want_u("R is +",key_alone('R'),1ull<<10);
  want_u("Tab is -",key_alone(KBM_KEY_TAB),1ull<<11);
  want_u("Grave is -",key_alone(KBM_KEY_GRAVE),1ull<<11);
  want_u("1 is D-pad right",key_alone('1'),1ull<<14);
  want_u("2 is D-pad left",key_alone('2'),1ull<<12);
  want_u("3 is D-pad up",key_alone('3'),1ull<<13);
  want_u("4 is D-pad down",key_alone('4'),1ull<<15);
  want_u("ZR is unbound",key_alone('Z'),0);
  want_u("Backspace is unbound",key_alone(KBM_KEY_BACKSPACE),0);
  kbm_release_all(); keyboard();
  kbm_key(KBM_KEY_TAB,1); kbm_key(KBM_KEY_GRAVE,1); kbm_key(KBM_KEY_TAB,0); keyboard();
  want_u("letting go of Tab keeps - while Grave is held",keyboard().b,1ull<<11);
  kbm_key(KBM_KEY_GRAVE,0); want_u("Grave released",keyboard().b,0);
  kbm_key(KBM_KEY_SPACE,1); kbm_key(KBM_KEY_SPACE,0);
  want_u("a tap between two polls reaches one poll",keyboard().b,1ull<<1);
  want_u("and only one",keyboard().b,0);
  kbm_key('W',1); kbm_key('A',1); kbm_key('D',1);
  { Poll p=keyboard(); want_i("W pushes the left stick up",p.ly,32767); want_i("A and D cancel",p.lx,0); want_i("no right stick",p.rx,0); }
  kbm_release_all(); kbm_key(KBM_KEY_DOWN,1); want_i("the down arrow pulls the left stick back",keyboard().ly,-32767);
  kbm_release_all(); kbm_key('L',1); want_i("L pushes the right stick right",keyboard().rx,32767);
  kbm_release_all(); { Poll p=keyboard(); want_u("released all: no buttons",p.b,0); want_i("released all: no stick",p.rx,0); }
  kbm_key(-5,1); kbm_key(KBM_KEY_COUNT,1); want_u("out-of-range keys ignored",keyboard().b,0);

  /* [Input] key_* overrides */
  want_i("key_zr is a binding",kbm_binding_set("key_zr","Shift+1"),1);
  want_i("key_nonsense is not",kbm_binding_set("key_nonsense","A"),0);
  kbm_key(KBM_KEY_SHIFT,1); want_u("Shift alone is still ZL",keyboard().b,1ull<<8);
  kbm_key('1',1); want_u("Shift+1 presses ZR only (not ZL, not D-pad right)",keyboard().b,1ull<<9);
  kbm_key(KBM_KEY_SHIFT,0); want_u("let go of Shift: 1 is D-pad right again",keyboard().b,1ull<<14);
  kbm_release_all(); keyboard();
  kbm_binding_set("key_zr"," bogus , z ");
  want_u("an unknown key name is dropped, the rest kept (lower case too)",key_alone('Z'),1ull<<9);
  kbm_binding_set("key_a","-");
  want_u("- leaves A unbound",key_alone(KBM_KEY_ENTER),0);
  kbm_binding_set("key_minus","`, Minus");
  want_u("` and Minus are other spellings of Grave and Dash",key_alone(KBM_KEY_DASH),1ull<<11);
  { char b[128]; kbm_binding_get(kbm_action_find("key_minus"),b,sizeof b); want_s("the binding reads back as written",b,"`, Minus");
    kbm_binding_get(kbm_action_find("key_b"),b,sizeof b); want_s("an unset binding reads as its default",b,"Space"); }
  want_i("Shift by name, any case",kbm_key_from_name("sHiFt"),KBM_KEY_SHIFT);
  want_s("the name of Num7",kbm_key_name(KBM_KEY_NUM0+7),"Num7");
  want_s("the name of Q",kbm_key_name('Q'),"Q");
  for(int i=0;i<kbm_action_count();i++) kbm_binding_set(kbm_action_key(i),kbm_action_default(i));
  want_u("defaults back",key_alone(KBM_KEY_ENTER),1ull<<0);

  /* press-a-key capture (the menu's Controls page) */
  { char c[64];
    want_i("no capture: keys pass",kbm_capture_key('A',1),0); kbm_capture_key('A',0);
    kbm_capture_key(KBM_KEY_ENTER,1);   /* the Enter that selects the row */
    kbm_capture_begin();
    want_i("the selecting Enter's auto-repeat is taken",kbm_capture_key(KBM_KEY_ENTER,1),1);
    want_i("and doesn't bind",kbm_capture_take(c,sizeof c),0);
    kbm_capture_key(KBM_KEY_ENTER,0);
    want_i("Shift is taken",kbm_capture_key(KBM_KEY_SHIFT,1),1);
    want_i("1 is taken",kbm_capture_key('1',1),1);
    want_i("a chord is ready",kbm_capture_take(c,sizeof c),1); want_s("Shift held with 1",c,"Shift+1");
    want_i("its auto-repeat still goes to the capture",kbm_capture_key('1',1),1);
    want_i("its release too",kbm_capture_key('1',0),1);
    want_i("then keys pass again",kbm_capture_key('1',1),0); kbm_capture_key('1',0); kbm_capture_key(KBM_KEY_SHIFT,0);
    kbm_capture_begin(); kbm_capture_key(KBM_KEY_CTRL,1); kbm_capture_key(KBM_KEY_CTRL,0);
    want_i("a modifier alone",kbm_capture_take(c,sizeof c),1); want_s("is a binding by itself",c,"Ctrl");
    kbm_capture_begin(); kbm_capture_cancel();
    want_i("cancelled: keys pass",kbm_capture_key('G',1),0); kbm_capture_key('G',0);
    want_i("cancelled: nothing to take",kbm_capture_take(c,sizeof c),0); }

  /* mouse look */
  want_i("a still mouse leaves the controller's right stick",mouse(1234,-99,1).rx,1234);
  kbm_mouse_motion(40,0); { Poll p=mouse(0,0,1); want_i("40 counts at 100% is a full push right",p.rx,32767); want_i("no vertical movement, no vertical push",p.ry,0); }
  kbm_mouse_motion(4,0); want_range("4 counts: the dead zone push plus a little",mouse(0,0,1).rx,7600,7800);   /* (0.15+0.1*0.85)*32767 */
  kbm_mouse_motion(-1,0); want_range("1 count left still turns: at least the dead zone push",mouse(0,0,1).rx,-5800,-4915);
  kbm_mouse_motion(0,-10); want_range("the mouse moving up looks up",mouse(0,0,1).ry,1,32767);
  g_opt_mouse_invert_y=1; kbm_mouse_motion(0,-10); want_range("inverted: up looks down",mouse(0,0,1).ry,-32767,-1); g_opt_mouse_invert_y=0;
  kbm_mouse_motion(1000,1000); { Poll p=mouse(0,0,1); want_i("a huge movement is clamped (x)",p.rx,32767); want_i("a huge movement is clamped (y)",p.ry,-32767); }
  g_opt_mouse_sens=200; kbm_mouse_motion(20,0); want_i("200% sensitivity: half the movement for a full push",mouse(0,0,1).rx,32767); g_opt_mouse_sens=100;
  kbm_mouse_motion(40,0); want_i("menu open: the movement is dropped",mouse(0,0,0).rx,0);
  want_i("and doesn't arrive once it closes",mouse(0,0,1).rx,0);
  g_opt_mouse_look=0; kbm_mouse_motion(40,0); want_i("mouse_look=0: no camera",mouse(0,0,1).rx,0); g_opt_mouse_look=1;

  /* mouse aim: the code module's look update takes the pending counts once per step and counts its steps */
  { static volatile int pend[2]; static volatile float deg[2], mult=1.5f; static volatile u32 steps;
    KbmAim a={pend,deg,&steps,&mult}; kbm_aim_bind(&a);
    kbm_mouse_motion(40,0); want_i("bound, look update not running: the stick still moves",mouse(0,0,1).rx,32767);
    want_i("and nothing is left pending",pend[0],0);
    steps++; kbm_mouse_motion(25,-10); { Poll p=mouse(1234,-99,1);
      want_i("look update running: the controller's stick is left alone (x)",p.rx,1234); want_i("(y)",p.ry,-99); }
    want_i("the counts wait for the look update (x)",pend[0],25); want_i("(y, screen down)",pend[1],-10);
    want_i("100% sensitivity: 0.05 degrees a count",(i32)(deg[0]*1000.0f+0.5f),50);
    want_i("vertical the same",(i32)(deg[1]*1000.0f+0.5f),50);
    kbm_mouse_motion(-5,0); mouse(0,0,1); want_i("they add up until a step takes them",pend[0],20);
    pend[0]=pend[1]=0; steps++;   /* a step took them */
    g_opt_mouse_invert_y=1; kbm_mouse_motion(0,3); mouse(0,0,1);
    want_i("inverted: the vertical degrees flip",(i32)(deg[1]*1000.0f-0.5f),-50); g_opt_mouse_invert_y=0;
    g_opt_mouse_sens=200; kbm_mouse_motion(1,0); mouse(0,0,1);
    want_i("200%: 0.1 degrees a count",(i32)(deg[0]*1000.0f+0.5f),100); g_opt_mouse_sens=100;
    steps++; kbm_mouse_motion(40,0); mouse(0,0,0);
    want_i("menu open: the movement and what was pending are dropped (x)",pend[0],0); want_i("(y)",pend[1],0);
    for(int i=0;i<4;i++) mouse(0,0,1);
    kbm_mouse_motion(40,0); want_i("5 polls without a step: still mouse aim",mouse(0,0,1).rx,0);
    want_i("its counts pending",pend[0],40);
    kbm_mouse_motion(40,0); want_i("6 polls without a step (lock-on): the stick again",mouse(0,0,1).rx,32767);
    want_i("and the counts no step took are dropped",pend[0],0);
    kbm_mouse_button(KBM_MOUSE_RIGHT,1); kbm_mouse_motion(40,0); { Poll p=mouse(0,0,1);
      want_i("locked on (right click, ZL) without the look update: the mouse leaves the stick",p.rx,0); want_u("ZL still pressed",p.b,1ull<<8); }
    kbm_mouse_button(KBM_MOUSE_RIGHT,0);
    { Poll p={1ull<<8,0,0,0,0}; kbm_mouse_motion(40,0); kbm_mouse(&p.b,&p.rx,&p.ry,1); want_i("ZL from the keyboard or a controller too",p.rx,0); }
    kbm_mouse_motion(40,0); want_i("ZL released: the stick again",mouse(0,0,1).rx,32767);
    kbm_mouse_button(KBM_MOUSE_RIGHT,1); steps++; kbm_mouse_motion(7,0); mouse(0,0,1);
    want_i("ZL held with the look update running (no target): mouse aim",pend[0],7); pend[0]=0; kbm_mouse_button(KBM_MOUSE_RIGHT,0);
    steps++; kbm_mouse_motion(7,0); want_i("a step again: mouse aim again",mouse(0,0,1).rx,0); want_i("with only the new counts",pend[0],7);
    kbm_aim_bind(0); kbm_mouse_motion(40,0); want_i("unbound: the stick",mouse(0,0,1).rx,32767); }

  /* mouse buttons */
  kbm_mouse_button(KBM_MOUSE_LEFT,1); want_u("left click is A",mouse(0,0,1).b,1ull<<0);
  kbm_mouse_button(KBM_MOUSE_LEFT,0); want_u("released",mouse(0,0,1).b,0);
  kbm_mouse_button(KBM_MOUSE_RIGHT,1); kbm_mouse_button(KBM_MOUSE_MIDDLE,1); want_u("right ZL, middle R",mouse(0,0,1).b,(1ull<<8)|(1ull<<7));
  kbm_mouse_button(KBM_MOUSE_RIGHT,0); kbm_mouse_button(KBM_MOUSE_MIDDLE,0); mouse(0,0,1);
  kbm_mouse_button(KBM_MOUSE_LEFT,1); kbm_mouse_button(KBM_MOUSE_LEFT,0); want_u("a click between two polls reaches one poll",mouse(0,0,1).b,1ull<<0);
  want_u("and only one",mouse(0,0,1).b,0);
  kbm_mouse_button(KBM_MOUSE_BACK,1); want_u("back is unbound by default",mouse(0,0,1).b,0); kbm_mouse_button(KBM_MOUSE_BACK,0);
  kbm_mouse_button(KBM_MOUSE_LEFT,1); want_u("menu open: no clicks",mouse(0,0,0).b,0); kbm_mouse_button(KBM_MOUSE_LEFT,0); mouse(0,0,1);
  snprintf(g_opt_mouse_buttons,sizeof g_opt_mouse_buttons," A , minus ,nonsense,y,b"); mouse_map_reset();
  kbm_mouse_button(KBM_MOUSE_LEFT,1); kbm_mouse_button(KBM_MOUSE_RIGHT,1); kbm_mouse_button(KBM_MOUSE_MIDDLE,1);
  kbm_mouse_button(KBM_MOUSE_BACK,1); kbm_mouse_button(KBM_MOUSE_FORWARD,1);
  want_u("mouse_buttons with spaces, upper case and an unknown name",mouse(0,0,1).b,(1ull<<0)|(1ull<<11)|(1ull<<3)|(1ull<<1));
  kbm_release_all(); want_u("released all: no mouse buttons",mouse(0,0,1).b,0);

  printf("input_kbm test: %s\n",failed? "FAILED" : "all passed");
  return failed;
}
