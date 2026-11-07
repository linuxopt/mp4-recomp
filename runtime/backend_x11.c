/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* X11 + GLX 4.3 backend, the Linux counterpart of backend_win32.c (Wayland desktops run it through
   XWayland). Controllers come from SDL2, loaded at run time like XInput on Windows.
   F1 help  F3 screenshot  F4 fps limit  F5 trace  F8 full-res screenshot  F11/Alt+Enter fullscreen  Esc quit
   Also what the Xvfb tests run on (screenshots, scripted input). */
#include "rt.h"
#include "pad_remap.h"
#include "input_kbm.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <GL/glx.h>
#include <dlfcn.h>
#include <time.h>

static Display* dpy; static Window win; static Atom wm_delete, a_state, a_fs; static Cursor g_blank;
static volatile int g_ww=1280, g_wh=720; static int g_fullscreen=0, g_want_fs=1;
static int g_seconds=0, g_maxframes=-1; static char g_shot[1024]; static u64 g_start; static u64 g_press=0, g_press_at=0, g_press_len=300;
static char g_script[1024]; static char g_title[256]; static volatile int g_shot_req;
static int g_focused, g_grabbed;   /* mouse look: the pointer is grabbed and kept near the window's centre */
extern int g_opt_fps;

void backend_ini(const char* k, const char* v){
  if(!strcmp(k,"fullscreen")) g_want_fs=atoi(v);
  else if(!strcmp(k,"seconds")) g_seconds=atoi(v);
  else if(!strcmp(k,"frames")) g_maxframes=atoi(v);
  else if(!strcmp(k,"shot")) strncpy(g_shot,v,1023);
  else if(!strcmp(k,"press")) g_press=strtoull(v,NULL,16);
  else if(!strcmp(k,"press_at")) g_press_at=strtoull(v,NULL,10);
  else if(!strcmp(k,"press_len")) g_press_len=strtoull(v,NULL,10);
  else if(!strcmp(k,"script")) strncpy(g_script,v,1023);
}

int video_has_gl(void){ return 1; }

/* ---------------------------------------------------------------- SDL2 game controllers, loaded dynamically */
typedef void SDLGC;
static struct {
  int (*Init)(u32); int (*SetHint)(const char*, const char*); int (*NumJoysticks)(void); int (*IsGameController)(int);
  SDLGC* (*Open)(int); int (*Attached)(SDLGC*); void (*Close)(SDLGC*); void (*Update)(void);
  u8 (*Button)(SDLGC*, int); i16 (*Axis)(SDLGC*, int); const char* (*Name)(SDLGC*);
} sdl;
static SDLGC* g_pad_gc; static u64 g_pad_scan;
static int (*sdl_rumble)(SDLGC*, u16, u16, u32);

static int (*sdl_gctype)(int);   /* SDL_GameControllerTypeForIndex, SDL 2.0.12+ (optional) */
static void sdl_load(void){
  static int done; if(done) return; done=1;   /* also called early by backend_pad_family */
  void* h=dlopen("libSDL2-2.0.so.0",RTLD_NOW|RTLD_LOCAL); if(!h) h=dlopen("libSDL2.so",RTLD_NOW|RTLD_LOCAL);
  if(!h){ LOG("controllers: SDL2 not found (install libsdl2), keyboard only\n"); return; }
  void** f[]={(void**)&sdl.Init,(void**)&sdl.SetHint,(void**)&sdl.NumJoysticks,(void**)&sdl.IsGameController,(void**)&sdl.Open,(void**)&sdl.Attached,
              (void**)&sdl.Close,(void**)&sdl.Update,(void**)&sdl.Button,(void**)&sdl.Axis,(void**)&sdl.Name};
  const char* n[]={"SDL_Init","SDL_SetHint","SDL_NumJoysticks","SDL_IsGameController","SDL_GameControllerOpen","SDL_GameControllerGetAttached",
                   "SDL_GameControllerClose","SDL_GameControllerUpdate","SDL_GameControllerGetButton","SDL_GameControllerGetAxis","SDL_GameControllerName"};
  for(int i=0;i<11;i++){ *f[i]=dlsym(h,n[i]); if(!*f[i]){ memset(&sdl,0,sizeof sdl); return; } }
  *(void**)&sdl_rumble=dlsym(h,"SDL_GameControllerRumble");   /* SDL 2.0.9+; optional */
  *(void**)&sdl_gctype=dlsym(h,"SDL_GameControllerTypeForIndex");
  /* buttons by position like on Windows, also for Nintendo controllers that SDL would map by label */
  sdl.SetHint("SDL_GAMECONTROLLER_USE_BUTTON_LABELS","0");
  sdl.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS","1");
  if(sdl.Init(0x2000 /* SDL_INIT_GAMECONTROLLER */)){ memset(&sdl,0,sizeof sdl); return; }
}

/* the kind of controller connected, for the button icons (svc_fs.c), asked before the window exists: 1 Xbox or any
   other (the Steam Deck's controls and Steam Input pads among them), 2 DualShock 4, 3 DualSense, 4 Switch, 0 none */
int backend_pad_family(void){
  sdl_load(); if(!sdl.Init) return 0;
  for(int i=0;i<sdl.NumJoysticks();i++) if(sdl.IsGameController(i)){
    int t= sdl_gctype? sdl_gctype(i) : 0;
    return t==4||t==3? 2 : t==7? 3 : t==5||t==11||t==12||t==13? 4 : 1; }
  return 0; }
static i32 dz(i32 v){ return (v>-7000 && v<7000)? 0 : v; }

static void pad_poll(u64* b, i32* lx, i32* ly, i32* rx, i32* ry){
  if(!sdl.Init) return;
  sdl.Update();
  if(g_pad_gc && !sdl.Attached(g_pad_gc)){ sdl.Close(g_pad_gc); g_pad_gc=NULL; LOG("controllers: disconnected\n"); }
  if(!g_pad_gc){ u64 now=plat_ns(); if(now-g_pad_scan<1000000000ull) return; g_pad_scan=now;
    for(int i=0;i<sdl.NumJoysticks() && !g_pad_gc;i++) if(sdl.IsGameController(i)) g_pad_gc=sdl.Open(i);
    if(!g_pad_gc) return;
    LOG("controllers: %s\n",sdl.Name(g_pad_gc)? sdl.Name(g_pad_gc) : "connected"); }
  SDLGC* g=g_pad_gc;
  /* SDL names buttons by Xbox position: A bottom, B right, X left, Y top */
  static const struct { int sdl, bit; } m[]={{1,0},{0,1},{3,2},{2,3},{7,4},{8,5},{9,6},{10,7},{6,10},{4,11},{13,12},{11,13},{14,14},{12,15}};
  for(unsigned i=0;i<sizeof m/sizeof m[0];i++) if(sdl.Button(g,m[i].sdl)) *b|=1ull<<m[i].bit;
  if(sdl.Button(g,5)) *b|=1ull<<30;   /* Guide / Home: the in-game menu only, never the game (video_poll) */
  { i32 lt=sdl.Axis(g,4), rt=sdl.Axis(g,5); if(lt>7700) *b|=1ull<<8; if(rt>7700) *b|=1ull<<9;
    /* log trigger presses (first 40) and the strongest pull seen, for "ZL does nothing" reports */
    static int was, said; static i32 maxlt; if(lt>maxlt){ maxlt=lt; if(said<40 && (maxlt>=32000 || maxlt%4096<64)) { said++; LOG("controller: left trigger reached %d of 32767 (ZL at 7700)\n",maxlt); } }
    int now= lt>7700; if(now!=was && said<400){ said++; LOG("controller: ZL (left trigger) %s, value %d\n",now? "pressed" : "released",lt); } was=now; }
  /* SDL's Y axes point down, the Switch's up */
  i32 x=sdl.Axis(g,0), y=-(i32)sdl.Axis(g,1), x2=sdl.Axis(g,2), y2=-(i32)sdl.Axis(g,3);
  if(y>32767) y=32767; if(y2>32767) y2=32767;
  if(!*lx && !*ly){ *lx=dz(x); *ly=dz(y); } if(!*rx && !*ry){ *rx=dz(x2); *ry=dz(y2); }
  if(sdl_rumble){ /* rumble (svc_hid.c hid_rumble_get): on a change, else refreshed every 100 ms; on a Steam Deck
                     Steam Input passes it to the haptics */
    extern void hid_rumble_get(float*,float*); float lo, hi; hid_rumble_get(&lo,&hi);
    static u16 plo=1, phi=1; static u64 last; u64 now=plat_ns(); u16 ulo=(u16)(lo*65535.0f), uhi=(u16)(hi*65535.0f);
    if(ulo!=plo || uhi!=phi || ((ulo||uhi) && now-last>100000000ull)){ plo=ulo; phi=uhi; last=now; sdl_rumble(g,ulo,uhi,ulo||uhi? 250 : 0); } }
}

/* ---------------------------------------------------------------- window */
static void set_fullscreen(int on){
  if(on==g_fullscreen) return;
  XEvent e; memset(&e,0,sizeof e); e.xclient.type=ClientMessage; e.xclient.window=win; e.xclient.message_type=a_state; e.xclient.format=32;
  e.xclient.data.l[0]=on? 1 : 0; e.xclient.data.l[1]=(long)a_fs; e.xclient.data.l[3]=1;
  XSendEvent(dpy,DefaultRootWindow(dpy),False,SubstructureRedirectMask|SubstructureNotifyMask,&e);
  XFlush(dpy); g_fullscreen=on; }   /* the cursor follows in cursor_sync */
/* the cursor is hidden in fullscreen, but shown while the in-game menu is open (its mouse / touch), as backend_win32.c
   does with WM_SETCURSOR; checked on every poll */
extern int ov_is_open(void);
static void cursor_sync(void){
  static int hidden=-1; int want= g_fullscreen && !ov_is_open();
  if(want==hidden) return;
  hidden=want; if(want) XDefineCursor(dpy,win,g_blank); else XUndefineCursor(dpy,win); XFlush(dpy); }

static void set_title(const char* t){ XStoreName(dpy,win,t); XFlush(dpy); }
/* fast forward on/off in the title, as backend_win32.c does it (main.c ff_*) */
void backend_ff_status(int on, int speed){ if(!dpy || !win) return; char t[320];
  if(on) snprintf(t,sizeof t,"%s  |  FAST FORWARD x%d (Tab or L3+R3)",g_title,speed); else snprintf(t,sizeof t,"%s",g_title); set_title(t); }

static const char* HELP=
  "SwitchRecomp - controls\n\n"
  "Controller: same as the Switch controller (buttons by position).\n\n"
  "Keyboard:\n"
  "  Arrows = D-Pad     W A S D = Left stick    I J K L = Right stick\n"
  "  X / Space = A    Z = B    C = X    V = Y\n"
  "  Q = L   E = R   1 = ZL   3 = ZR   Enter = +   Backspace = -\n\n"
  "Mouse (mouse_look=1; the menu or another window frees the cursor):\n"
  "  Move = camera   Left = ZR   Right = ZL   Middle = R   (mouse_buttons in the .ini changes them)\n\n"
  "F1 = this help   F3 = screenshot   F4 = FPS limit 60 / unlimited   F5 = diagnostic dump (trace)\n"
  "F8 = instant full-resolution screenshot (PNG in the captures folder)\n"
  "F11 / Alt+Enter = fullscreen   Esc = quit";

void video_init(const char* title){
  snprintf(g_title,sizeof g_title,"%s - SwitchRecomp  (F1 help)",title);
  XInitThreads();
  /* let Mesa run its own glthread so driver work isn't on our GPU thread (Deck, AMD/Intel on Linux) */
  if(!getenv("SWR_NO_GLTHREAD")) setenv("mesa_glthread","true",0);
  /* the drivers' own shader caches sit under our program binaries: a translated shader the game changes slightly still
     reuses the driver's compiled pieces. Their defaults are small for a game this size (Mesa 1 GB, NVIDIA 128 MB and
     cleaned up), and a full cache throws away what it compiled. A value already in the environment wins */
  setenv("MESA_SHADER_CACHE_MAX_SIZE","8G",0); setenv("MESA_DISK_CACHE_MULTI_FILE","1",0);
  setenv("__GL_SHADER_DISK_CACHE","1",0); setenv("__GL_SHADER_DISK_CACHE_SIZE","8589934592",0); setenv("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP","1",0);
  dpy=XOpenDisplay(NULL); if(!dpy) fatal("No X11 display (DISPLAY is not set). Start the game from a desktop session.");
  int scr=DefaultScreen(dpy);
  static int att[]={GLX_X_RENDERABLE,True,GLX_DRAWABLE_TYPE,GLX_WINDOW_BIT,GLX_RENDER_TYPE,GLX_RGBA_BIT,GLX_RED_SIZE,8,GLX_GREEN_SIZE,8,GLX_BLUE_SIZE,8,GLX_ALPHA_SIZE,8,GLX_DEPTH_SIZE,24,GLX_DOUBLEBUFFER,True,None};
  int n=0; GLXFBConfig* fc=glXChooseFBConfig(dpy,scr,att,&n); if(!fc||!n) fatal("Your graphics driver has no OpenGL support for this display (GLX).");
  XVisualInfo* vi=glXGetVisualFromFBConfig(dpy,fc[0]);
  XSetWindowAttributes swa; memset(&swa,0,sizeof swa); swa.colormap=XCreateColormap(dpy,RootWindow(dpy,vi->screen),vi->visual,AllocNone);
  swa.background_pixel=BlackPixel(dpy,scr);
  swa.event_mask=KeyPressMask|KeyReleaseMask|StructureNotifyMask|FocusChangeMask|ButtonPressMask|ButtonReleaseMask|PointerMotionMask;   /* the mouse / touch: the in-game menu */
  win=XCreateWindow(dpy,RootWindow(dpy,vi->screen),0,0,g_ww,g_wh,0,vi->depth,InputOutput,vi->visual,CWColormap|CWEventMask|CWBackPixel,&swa);
  XStoreName(dpy,win,g_title);
  { XClassHint ch={(char*)"switchrecomp",(char*)"SwitchRecomp"}; XSetClassHint(dpy,win,&ch); }
  wm_delete=XInternAtom(dpy,"WM_DELETE_WINDOW",False); XSetWMProtocols(dpy,win,&wm_delete,1);
  a_state=XInternAtom(dpy,"_NET_WM_STATE",False); a_fs=XInternAtom(dpy,"_NET_WM_STATE_FULLSCREEN",False);
  { static char none[8]; XColor black; memset(&black,0,sizeof black);
    Pixmap p=XCreateBitmapFromData(dpy,win,none,8,8); g_blank=XCreatePixmapCursor(dpy,p,p,&black,&black,0,0); XFreePixmap(dpy,p); }
  /* without this X11 sends a release before every auto-repeated press */
  XkbSetDetectableAutoRepeat(dpy,True,NULL);
  /* start fullscreen: the window manager reads the state when the window is mapped */
  if(g_want_fs){ XChangeProperty(dpy,win,a_state,XA_ATOM,32,PropModeReplace,(unsigned char*)&a_fs,1); g_fullscreen=1; }
  XMapWindow(dpy,win); XFlush(dpy);
  sdl_load();
  g_start=plat_ns();
  extern void gpu_gl_start(void); gpu_gl_start();
}
/* GPU thread */
static GLXContext ctx, wctx[8]; static GLXFBConfig wfc; static int g_nwctx; static volatile int g_wnext;
/* shader compile threads (gpu_gl.c worker_start): Mesa and NVIDIA compile GLSL on the thread that asks, so each thread
   gets its own context in the game's share group and several shaders build at once. [Performance] shader_threads */
int g_opt_shader_threads;

static int xerr_ignore(Display* d, XErrorEvent* e){ (void)d;(void)e; return 0; }

int backend_gl_create(void){
  int scr=DefaultScreen(dpy);
  static int att[]={GLX_X_RENDERABLE,True,GLX_DRAWABLE_TYPE,GLX_WINDOW_BIT,GLX_RENDER_TYPE,GLX_RGBA_BIT,GLX_RED_SIZE,8,GLX_GREEN_SIZE,8,GLX_BLUE_SIZE,8,GLX_ALPHA_SIZE,8,GLX_DOUBLEBUFFER,True,None};
  int n=0; GLXFBConfig* fc=glXChooseFBConfig(dpy,scr,att,&n); if(!fc||!n) return 0;
  typedef GLXContext (*CCA)(Display*,GLXFBConfig,GLXContext,Bool,const int*);
  CCA cca=(CCA)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
  static const int vers[4][2]={{4,6},{4,5},{4,4},{4,3}};
  int (*old)(Display*,XErrorEvent*)=XSetErrorHandler(xerr_ignore);
  for(int i=0;i<4 && !ctx && cca;i++){ int ca[]={0x2091,vers[i][0],0x2092,vers[i][1],0x9126,0x00000001,None}; ctx=cca(dpy,fc[0],NULL,True,ca); XSync(dpy,False);
    if(ctx){ long nc=sysconf(_SC_NPROCESSORS_ONLN); int want=g_opt_shader_threads>0? g_opt_shader_threads : (int)(nc/4);
      if(want<2 && g_opt_shader_threads<=0) want=2; if(want>8) want=8;
      for(g_nwctx=0;g_nwctx<want;g_nwctx++){ wctx[g_nwctx]=cca(dpy,fc[0],ctx,True,ca); XSync(dpy,False); if(!wctx[g_nwctx]) break; }
      { extern int g_gl_workers; g_gl_workers= g_nwctx? g_nwctx : 1; } } }
  wfc=fc[0];
  XSetErrorHandler(old);
  if(!ctx){ fatal("Your graphics driver does not support OpenGL 4.3.\nPlease update Mesa or your GPU driver."); return 0; }
  glXMakeCurrent(dpy,win,ctx);
  extern int g_opt_vsync;
  typedef void (*SIE)(Display*,GLXDrawable,int); typedef int (*SIM)(unsigned);
  SIE sie=(SIE)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalEXT"); SIM sim=(SIM)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalMESA");
  if(sie) sie(dpy,win,g_opt_vsync? 1 : 0); else if(sim) sim(g_opt_vsync? 1 : 0);
  return 1; }

void* backend_gl_proc(const char* n){ return (void*)glXGetProcAddressARB((const GLubyte*)n); }

void backend_gl_swap(void){ glXSwapBuffers(dpy,win);
  /* SWR_VIDEO_TIMING=1: what reaches the monitor. The display's refresh counter (GLX_OML_sync_control MSC) after each
     swap: +1 = a clean new frame, +0 = two swaps within one refresh (a torn or never-seen frame), +2 or more = the
     previous frame shown again (a stutter). Every 120 swaps */
  static int timing=-1; static Bool (*gsv)(Display*,GLXDrawable,int64_t*,int64_t*,int64_t*); static int64_t last_msc; static u32 n, h[4];
  if(timing<0){ timing=!getenv("SWR_NO_VIDEO_TIMING"); if(timing) gsv=(Bool(*)(Display*,GLXDrawable,int64_t*,int64_t*,int64_t*))glXGetProcAddressARB((const GLubyte*)"glXGetSyncValuesOML");
    if(timing && !gsv) LOG("display timing: GLX_OML_sync_control not available\n"); }
  if(timing && gsv){ int64_t ust=0,msc=0,sbc=0; if(gsv(dpy,win,&ust,&msc,&sbc)){ if(last_msc){ int64_t d=msc-last_msc; h[d<=0? 0 : d==1? 1 : d==2? 2 : 3]++;
        if(++n==120){ LOG("display timing: of 120 swaps %u clean, %u in the same refresh (torn/unseen), %u repeated once, %u repeated more\n",h[1],h[0],h[2],h[3]); n=0; h[0]=h[1]=h[2]=h[3]=0; } }
      last_msc=msc; } } }
/* forced_vsync applies live (glq.c, the GL thread) */
void backend_gl_vsync(int on){
  typedef void (*SIE)(Display*,GLXDrawable,int); typedef int (*SIM)(unsigned);
  static SIE sie; static SIM sim; static int got;
  if(!got){ got=1; sie=(SIE)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalEXT"); sim=(SIM)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalMESA"); }
  if(sie) sie(dpy,win,on? 1 : 0); else if(sim) sim(on? 1 : 0); }
/* fullscreen from another thread (settings): done by the event loop, Xlib being single-threaded here */
static volatile int g_fs_req=-1;
void backend_set_fullscreen(int on){ g_fs_req= on!=0; }
int backend_is_fullscreen(void){ return g_fullscreen; }
void backend_release_keys(void){ kbm_release_all(); }

int backend_gl_worker_current(void){
  int k=__atomic_fetch_add(&g_wnext,1,__ATOMIC_ACQ_REL); if(k>=g_nwctx) return 0; int pa[]={GLX_PBUFFER_WIDTH,4,GLX_PBUFFER_HEIGHT,4,None};
  int (*old)(Display*,XErrorEvent*)=XSetErrorHandler(xerr_ignore); GLXPbuffer pb=glXCreatePbuffer(dpy,wfc,pa); XSync(dpy,False); XSetErrorHandler(old);
  if(pb && glXMakeContextCurrent(dpy,pb,pb,wctx[k])) return 1;
  return glXMakeContextCurrent(dpy,None,None,wctx[k])? 1 : 0; }

void backend_window_size(int* w, int* h){ *w=g_ww; *h=g_wh; }

void video_present_rgba(const u32* px, int w, int h, int stride){ extern void gpu_gl_present_cpu(const u32*,int,int,int); gpu_gl_present_cpu(px,w,h,stride); }

/* key events -> input_kbm.c key codes, which its [Input] key_* bindings map to the controller as on Windows.
   Letters and digits follow the layout (the unshifted keysym); the keypad and the punctuation keys go by the key's
   place (its evdev keycode, scan code + 8), punctuation named as on a US keyboard, since layouts put different
   characters there */
static int kbm_from_xkey(KeySym k, unsigned code){
  static const struct { unsigned char code; short key; } BY_PLACE[] = {
    {49, KBM_KEY_GRAVE}, {20, KBM_KEY_DASH}, {21, KBM_KEY_EQUALS}, {34, KBM_KEY_LBRACKET}, {35, KBM_KEY_RBRACKET},
    {51, KBM_KEY_BACKSLASH}, {47, KBM_KEY_SEMICOLON}, {48, KBM_KEY_APOSTROPHE}, {59, KBM_KEY_COMMA}, {60, KBM_KEY_PERIOD},
    {61, KBM_KEY_SLASH}, {90, KBM_KEY_NUM0}, {87, KBM_KEY_NUM0+1}, {88, KBM_KEY_NUM0+2}, {89, KBM_KEY_NUM0+3},
    {83, KBM_KEY_NUM0+4}, {84, KBM_KEY_NUM0+5}, {85, KBM_KEY_NUM0+6}, {79, KBM_KEY_NUM0+7}, {80, KBM_KEY_NUM0+8},
    {81, KBM_KEY_NUM0+9}, {86, KBM_KEY_NUM_ADD}, {82, KBM_KEY_NUM_SUB}, {63, KBM_KEY_NUM_MUL}, {106, KBM_KEY_NUM_DIV},
    {91, KBM_KEY_NUM_DECIMAL},
  };
  if(k>=XK_a && k<=XK_z) return 'A'+(int)(k-XK_a);
  if(code>=10 && code<=19) return code==19? '0' : '1'+(int)(code-10);   /* the number row by place: AZERTY's unshifted row is & e " ... */
  for(size_t i=0;i<sizeof BY_PLACE/sizeof *BY_PLACE;i++) if(BY_PLACE[i].code==code) return BY_PLACE[i].key;
  switch(k){
    case XK_space: return KBM_KEY_SPACE;
    case XK_Return: case XK_KP_Enter: return KBM_KEY_ENTER;
    case XK_BackSpace: return KBM_KEY_BACKSPACE;
    case XK_Left: return KBM_KEY_LEFT;
    case XK_Up: return KBM_KEY_UP;
    case XK_Right: return KBM_KEY_RIGHT;
    case XK_Down: return KBM_KEY_DOWN;
    case XK_Tab: case XK_ISO_Left_Tab: return KBM_KEY_TAB;
    case XK_Shift_L: case XK_Shift_R: return KBM_KEY_SHIFT;
    case XK_Control_L: case XK_Control_R: return KBM_KEY_CTRL;
    case XK_Alt_L: case XK_Alt_R: case XK_Meta_L: case XK_Meta_R: return KBM_KEY_ALT;
    case XK_Caps_Lock: return KBM_KEY_CAPSLOCK;
    case XK_Insert: return KBM_KEY_INSERT;
    case XK_Delete: return KBM_KEY_DELETE;
    case XK_Home: return KBM_KEY_HOME;
    case XK_End: return KBM_KEY_END;
    case XK_Prior: return KBM_KEY_PAGEUP;
    case XK_Next: return KBM_KEY_PAGEDOWN;
  }
  return KBM_KEY_NONE; }

/* X11 buttons 1 left, 2 middle, 3 right, 8 back, 9 forward (4..7 are the wheel) -> input_kbm.c buttons */
static int kbm_from_xbutton(unsigned b){
  switch(b){
    case 1: return KBM_MOUSE_LEFT;
    case 2: return KBM_MOUSE_MIDDLE;
    case 3: return KBM_MOUSE_RIGHT;
    case 8: return KBM_MOUSE_BACK;
    case 9: return KBM_MOUSE_FORWARD;
  }
  return -1; }

/* Mouse look: while the game has the mouse (the window focused, the in-game menu closed) the pointer is grabbed and
   hidden, and the movement is each motion event's distance from the one before. Once the pointer strays a quarter of
   the window from the centre it is warped back there. Motion events already queued before a warp still carry
   positions from before it, so the previous position only jumps to the centre when the warp's own event (at the
   centre) arrives. XWayland turns the hidden pointer's warps into a pointer lock with relative motion. */
static int g_last_x, g_last_y, g_warping;
static void mouse_warp_centre(void){
  g_warping=1; XWarpPointer(dpy,None,win,0,0,0,0,g_ww/2,g_wh/2); XFlush(dpy); }
static void mouse_grab_sync(void){
  static u64 retry;
  int want= kbm_mouse_look_enabled() && g_focused && !ov_is_open();
  if(want && !g_grabbed){
    u64 now=plat_ns(); if(now-retry<250000000ull) return;   /* a refused grab is tried again 4 times a second */
    retry=now;
    if(XGrabPointer(dpy,win,True,ButtonPressMask|ButtonReleaseMask|PointerMotionMask,GrabModeAsync,GrabModeAsync,win,g_blank,CurrentTime)!=GrabSuccess) return;
    g_grabbed=1;
    Window root, child; int rx, ry, x, y; unsigned mask;
    if(!XQueryPointer(dpy,win,&root,&child,&rx,&ry,&x,&y,&mask)){ x=g_ww/2; y=g_wh/2; }
    g_last_x=x; g_last_y=y; g_warping=0;
    if(x!=g_ww/2 || y!=g_wh/2) mouse_warp_centre();   /* a warp onto the pointer's own position sends no event */
  } else if(!want && g_grabbed){
    XUngrabPointer(dpy,CurrentTime); XFlush(dpy); g_grabbed=0; }
}
extern void ov_input_mouse(float x, float y);
static void mouse_motion(int x, int y){
  if(!g_grabbed){ ov_input_mouse((float)x,(float)y); return; }
  int cx=g_ww/2, cy=g_wh/2;
  if(g_warping && x==cx && y==cy){ g_warping=0; g_last_x=x; g_last_y=y; return; }   /* the warp itself, not movement */
  kbm_mouse_motion(x-g_last_x,y-g_last_y);
  g_last_x=x; g_last_y=y;
  if(!g_warping && (abs(x-cx)>g_ww/4 || abs(y-cy)>g_wh/4)) mouse_warp_centre(); }

/* one-shot keys act on the first press only, not on auto-repeat */
static int key_once(KeySym k, int down){
  static KeySym held[8]; int i;
  for(i=0;i<8;i++) if(held[i]==k) break;
  if(!down){ if(i<8) held[i]=0; return 0; }
  if(i<8) return 0;
  for(i=0;i<8;i++) if(!held[i]){ held[i]=k; break; }
  return 1; }

/* the in-game menu (beyond_ui/ov.cpp): F1 toggles it; while it is open it takes the keyboard and the mouse */
extern void ov_input_toggle(void); extern void ov_input_key(int ovk, int down); extern void ov_input_char(unsigned c);
extern void ov_input_mouse(float x, float y); extern void ov_input_button(int b, int down); extern void ov_input_wheel(float dy);
extern int ov_is_open(void); extern int ov_pad(unsigned long long phys, int lx, int ly);
static int ovk_from_keysym(KeySym k){
  switch(k){ case XK_Up: return 1; case XK_Down: return 2; case XK_Left: return 3; case XK_Right: return 4; case XK_Return: case XK_KP_Enter: return 5;
    case XK_Escape: return 6; case XK_Tab: return 7; case XK_Prior: return 8; case XK_Next: return 9; case XK_space: return 10; case XK_Home: return 11; case XK_End: return 12; }
  return 0; }
void backend_request_quit(void){ g_quit=1; }
static void key_down(XKeyEvent* e){
  KeySym k=XLookupKeysym(e,0);
  int once=key_once(k,1);
  if(k==XK_F1){ if(once) ov_input_toggle(); return; }
  if(kbm_capture_key(kbm_from_xkey(k,e->keycode),1)) return;   /* the menu's press-a-key rebinding takes it first */
  if(ov_is_open() && k!=XK_F11){ ov_input_key(ovk_from_keysym(k),1);
    { char c[8]; KeySym ks; int n=XLookupString(e,c,sizeof c,&ks,NULL); if(n==1 && (unsigned char)c[0]>=32) ov_input_char((unsigned char)c[0]); }
    return; }
  if(k==XK_Return && (e->state&Mod1Mask)){ if(once) set_fullscreen(!g_fullscreen); return; }
  switch(k){
    case XK_Escape: g_quit=1; return;
    case XK_F11: if(once) set_fullscreen(!g_fullscreen); return;
    case XK_F3: if(once) g_shot_req=1; return;
    case XK_F7: if(once) LOG("F7 video recording is only available on Windows\n"); return;
    case XK_F8: if(once){ extern volatile int g_shot8_req; g_shot8_req=1; } return;
    case XK_F5: if(once){ extern void gpu_gl_dump_rts(void); extern void kernel_dump_threads(void); extern __thread FILE* g_log_redirect;
        plat_mkdir("captures"); FILE* f=fopen("captures/threads.txt","w");   /* written next to the trace */
        if(f){ g_log_redirect=f; kernel_dump_threads(); g_log_redirect=NULL; fclose(f); }
        kernel_dump_threads(); gpu_gl_dump_rts(); } return;
    case XK_F4: if(once){ g_opt_fps= g_opt_fps==0? 1 : 0; char t[300]; snprintf(t,sizeof t,"%s  |  FPS limit: %s",g_title,g_opt_fps==0? "unlimited" : "60"); set_title(t);
        extern void ini_set(const char*, const char*); char v[8]; snprintf(v,sizeof v,"%d",g_opt_fps); ini_set("fps_limit",v); } return;
  }
  kbm_key(kbm_from_xkey(k,e->keycode),1); }

/* Test-only frame-indexed replay, as backend_win32.c has it (the same route files run on both): SWR_TEST_SCRIPT CSV
   guest_frame,hex_buttons,lx,ly,rx,ry, held until the next row; SWR_TEST_SETENV=FRAME:NAME=VALUE; captures at
   SWR_TEST_CAPTURE_FRAMES (SWR_TEST_CAPTURE_PREFIX-fN); SWR_TEST_EXIT_FRAME. Inert when unset */
typedef struct { u32 frame; u64 buttons; i32 lx, ly, rx, ry; } TestInput;
static TestInput* test_inputs; static u32 test_n, test_i; static int test_loaded, test_replay;
static void test_input_apply(u64* b, i32* lx, i32* ly, i32* rx, i32* ry){
  if(!test_loaded){ test_loaded=1; const char* path=getenv("SWR_TEST_SCRIPT");
    if(path && *path){ FILE* f=fopen(path,"r"); char line[256]; u32 cap=8192;
      if(!f){ WARN("test input: cannot open replay %s\n",path); _exit(2); }
      test_inputs=calloc(cap,sizeof *test_inputs);
      while(fgets(line,sizeof line,f)){ if(line[0]=='#'||line[0]=='\n'||line[0]=='\r') continue; TestInput v={0}; unsigned long long m;
        if(sscanf(line,"%u,%llx,%d,%d,%d,%d",&v.frame,&m,&v.lx,&v.ly,&v.rx,&v.ry)!=6 || test_n==cap){ fclose(f); WARN("test input: invalid replay row\n"); _exit(2); }
        v.buttons=(u64)m; test_inputs[test_n++]=v; }
      fclose(f); test_replay= test_n>0; LOG("test input: frame replay active, %u states\n",test_n); } }
  u32 frame=(u32)g_frames;
  { static int done; const char* se; if(!done && (se=getenv("SWR_TEST_SETENV")) && *se){ u32 at=(u32)strtoul(se,NULL,10); const char* kv=strchr(se,':');
      if(!kv) done=1; else if(frame>=at){ char k[64]; const char* eq=strchr(kv+1,'=');
        if(eq && eq-kv-1<64){ memcpy(k,kv+1,eq-kv-1); k[eq-kv-1]=0; setenv(k,eq+1,1); LOG("test: frame %u set %s=%s\n",frame,k,eq+1); } done=1; } } }
  if(test_replay){ while(test_i+1<test_n && test_inputs[test_i+1].frame<=frame) test_i++; TestInput* v=&test_inputs[test_i];
    *b=v->buttons; *lx=v->lx; *ly=v->ly; *rx=v->rx; *ry=v->ry; }
}
static void test_frame_hooks(void){
  extern u64 gpu_gl_screenshot(const char* path);
  if(!getenv("SWR_TEST_SCRIPT")) return;
  static int configured; static const char* times; static const char* prefix; static u64 next;
  if(!configured){ configured=1; times=getenv("SWR_TEST_CAPTURE_FRAMES"); prefix=getenv("SWR_TEST_CAPTURE_PREFIX"); if(times&&prefix) next=strtoull(times,(char**)&times,10); else times=NULL; }
  if(times && next && (u64)g_frames>=next){ char path[1024]; snprintf(path,sizeof path,"%s-f%llu",prefix,(unsigned long long)next);
    LOG("test capture: requested_frame=%llu actual_frame=%d ns=%llu\n",(unsigned long long)next,g_frames,(unsigned long long)plat_ns()); gpu_gl_screenshot(path);
    if(*times==','){ times++; next=strtoull(times,(char**)&times,10); } else { times=NULL; next=0; } }
  static u64 heartbeat; u64 now=plat_ns(); if(!heartbeat || now-heartbeat>=1000000000ull){ heartbeat=now; LOG("test heartbeat: frame=%d fps_limit=%d ns=%llu\n",g_frames,g_opt_fps,(unsigned long long)now); }
  const char* stop=getenv("SWR_TEST_EXIT_FRAME"); if(stop && (u64)g_frames>=strtoull(stop,NULL,10)){ LOG("test complete: frame=%d\n",g_frames); extern void save_hints(void); save_hints(); log_flush(); fflush(NULL); _exit(0); }
}

void video_poll(void){
  { int r=g_fs_req; if(r>=0){ g_fs_req=-1; set_fullscreen(r); } }
  cursor_sync();
  while(XPending(dpy)){ XEvent e; XNextEvent(dpy,&e);
    if(e.type==KeyPress) key_down(&e.xkey);
    else if(e.type==KeyRelease){ KeySym k=XLookupKeysym(&e.xkey,0); int kk=kbm_from_xkey(k,e.xkey.keycode); key_once(k,0); kbm_key(kk,0);
      if(!kbm_capture_key(kk,0) && ov_is_open()) ov_input_key(ovk_from_keysym(k),0); }
    else if(e.type==MotionNotify) mouse_motion(e.xmotion.x,e.xmotion.y);
    else if(e.type==ButtonPress || e.type==ButtonRelease){ unsigned bt=e.xbutton.button; int down= e.type==ButtonPress;
      kbm_mouse_button(kbm_from_xbutton(bt),down);   /* the game ignores it while the menu is open (kbm_mouse) */
      if(!g_grabbed) ov_input_mouse((float)e.xbutton.x,(float)e.xbutton.y);
      if(bt==1 || bt==3) ov_input_button(bt==1? 0 : 1, down); else if(down && (bt==4 || bt==5)) ov_input_wheel(bt==4? 1.0f : -1.0f); }
    else if(e.type==FocusIn) g_focused=1;
    else if(e.type==FocusOut){ g_focused=0; kbm_release_all(); }
    else if(e.type==ConfigureNotify){ g_ww=e.xconfigure.width>0? e.xconfigure.width : 1; g_wh=e.xconfigure.height>0? e.xconfigure.height : 1; }
    else if(e.type==ClientMessage && (Atom)e.xclient.data.l[0]==wm_delete){ LOG("window: closed by the user\n"); g_quit=1; } }
  mouse_grab_sync();
  u64 k=0, b=0; i32 lx, ly, rx, ry;
  kbm_keyboard(&k,&lx,&ly,&rx,&ry);
  pad_poll(&b,&lx,&ly,&rx,&ry);
  u64 phys=b; i32 plx=lx, ply=ly; b&=~(1ull<<30);
  b=swr_pad_remap(b)|(k&0xFFFFull);
  /* stick-as-button bits 16..23 */
  if(lx<-16000) b|=1ull<<16; if(ly>16000) b|=1ull<<17; if(lx>16000) b|=1ull<<18; if(ly<-16000) b|=1ull<<19;
  if(rx<-16000) b|=1ull<<20; if(ry>16000) b|=1ull<<21; if(rx>16000) b|=1ull<<22; if(ry<-16000) b|=1ull<<23;
  /* test input: --press=hex --press_at=ms, or --script=ms:hex,ms:hex,... */
  u64 el=(plat_ns()-g_start)/1000000ull;
  if(g_press && el>=g_press_at && el<g_press_at+g_press_len) b|=g_press;
  if(g_script[0]){ const char* p=g_script; while(*p){ u64 t=strtoull(p,(char**)&p,10); if(*p!=':') break; p++; u64 m=strtoull(p,(char**)&p,16); if(el>=t && el<t+g_press_len) b|=m; if(*p==',') p++; } }
  kbm_mouse(&b,&rx,&ry,!ov_is_open());   /* the menu uses the mouse itself while open */
  test_input_apply(&b,&lx,&ly,&rx,&ry);
  /* every session's input goes to input_record.csv (the SWR_TEST_SCRIPT replay format, a row per change), so a
     problem spot can be reached again by replaying the route there. SWR_NO_INPUT_RECORD=1: off */
  if(!test_replay){ static FILE* rf; static int rinit; static u64 pb=~0ull; static i32 plx=1<<30, ply, prx, pry;
    if(!rinit){ rinit=1; if(!getenv("SWR_NO_INPUT_RECORD")){ extern char g_exedir[]; char p[1200]; snprintf(p,sizeof p,"%sinput_record.csv",g_exedir); rf=fopen(p,"w");
        if(rf) fprintf(rf,"# guest_frame,hex_buttons,lx,ly,rx,ry (replay: SWR_TEST_SCRIPT=this file)\n"); } }
    if(rf && (b!=pb || lx!=plx || ly!=ply || rx!=prx || ry!=pry)){ fprintf(rf,"%d,%llx,%d,%d,%d,%d\n",g_frames,(unsigned long long)b,lx,ly,rx,ry); pb=b; plx=lx; ply=ly; prx=rx; pry=ry;
      static int nfl; if(++nfl%64==0) fflush(rf); } }
  if(test_replay){ phys=b; plx=lx; ply=ly; }   /* tests drive the menu with the replayed buttons */
  if(ov_pad(phys,plx,ply)){ b=0; lx=ly=rx=ry=0; }   /* the menu has the controller (and the game neutral input) */
  g_pad.buttons=b; g_pad.lx=lx; g_pad.ly=ly; g_pad.rx=rx; g_pad.ry=ry;
}

void frontend_frame(void){
  extern u64 gpu_gl_screenshot(const char* path);
  test_frame_hooks();
  if(g_shot_req){ g_shot_req=0; char p[300]; time_t now=time(NULL); struct tm t; localtime_r(&now,&t); plat_mkdir("captures");
    snprintf(p,sizeof p,"captures/screenshot_%04d%02d%02d_%02d%02d%02d",t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec); gpu_gl_screenshot(p); }
  u64 el=plat_ns()-g_start;
  if((g_maxframes>=0 && g_frames>=g_maxframes) || (g_seconds && el>=(u64)g_seconds*1000000000ull)){
    u64 h=gpu_gl_screenshot(g_shot[0]? g_shot : NULL);
    fprintf(stderr,"\nframes=%d hash=%016llx\n",g_frames,(unsigned long long)h);
    extern void save_hints(void); save_hints(); log_flush(); fflush(NULL); _exit(0); }
}

int backend_glq_ok(void){ return 1; }   /* GL calls go through glq */

/* Play screen / first-run setup (beyond_ui/fe.cpp, in its own window) before the game starts (main.c). 0 = go on with the
   game, 1 = quit. Skipped for test runs and --no-launcher, as backend_win32.c does; builds without it link stub_fe.c */
extern int fe_should_show(int argc, char** argv, const char* exedir, const char* exename); extern int fe_run(void* hwnd);
int backend_frontend(int argc, char** argv, const char* exedir, const char* exename){
  if(!getenv("SWR_FE_TEST") && (getenv("SWR_TEST_SCRIPT") || getenv("SWR_TEST_EXIT_FRAME") || getenv("SWR_TEST_RECORD"))) return 0;
  for(int i=1;i<argc;i++) if(!strcmp(argv[i],"--no-launcher") || !strncmp(argv[i],"--seconds=",10) || !strncmp(argv[i],"--frames=",9)) return 0;   /* timed runs are tests */
  if(!fe_should_show(argc,argv,exedir,exename)) return 0;
  return fe_run(NULL); }
