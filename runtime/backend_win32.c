/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* win32 backend: window, WGL, keyboard, XInput.
   F1 menu  F3 screenshot  F4 fps limit  F11 fullscreen  Esc quit
   The same window first shows the Play screen / setup (beyond_ui/fe.cpp, Direct3D 11, no pixel format), then the game. */
#include "rt.h"
#include <math.h>
#include <ctype.h>
#include "pad_remap.h"
#include "input_kbm.h"
#include <windows.h>
#include <mmsystem.h>  /* timeBeginPeriod (windows.h leaves it out with WIN32_LEAN_AND_MEAN) */

static HWND g_hwnd; static HDC g_hdc; static HGLRC g_ctx, g_wctx;
static HWND g_glwnd; static int g_want_vk;   /* renderer=vulkan: GL renders on the hidden g_glwnd, Vulkan presents on g_hwnd */
static volatile int g_ww=1280, g_wh=720; static int g_fullscreen=0, g_want_fs=1;
static WINDOWPLACEMENT g_wp={sizeof(WINDOWPLACEMENT)};
static int g_seconds=0; static char g_shot[1024]; static u64 g_start; static volatile int g_shot_req;
static char g_script[8192];
static HANDLE g_ready;
static volatile int g_fe_mode;   /* the front end owns the window (its messages go to fe_wndproc) */
static HANDLE g_fe_done, g_video_go; static int g_fe_result, g_win_started;
static HWND g_fe_hwnd;
/* mouse buttons held, window thread only: the mouse is captured while any is down, so releases outside still arrive */
static int g_mouse_down;
/* mouse look holds the cursor in the window: the input thread asks (WM_APP+12), the window thread clips */
static int g_cursor_clipped;
#define PAD_HOME (1ull<<30)   /* Home / Guide / PS: physical only, opens the in-game menu, never reaches the game */
extern int g_opt_fps, g_opt_ff;   /* main.c: fps_limit, fast_forward (Tab) */

static int g_ls_mode=0; static char g_ls_path[1024];

void backend_ini(const char* k, const char* v){
  if(!strcmp(k,"fullscreen")) g_want_fs=atoi(v);
  else if(!strcmp(k,"lossless_scaling")) g_ls_mode=atoi(v);
  else if(!strcmp(k,"lossless_path")) strncpy(g_ls_path,v,1023);
  else if(!strcmp(k,"seconds")) g_seconds=atoi(v);
  else if(!strcmp(k,"shot")) strncpy(g_shot,v,1023);
  else if(!strcmp(k,"script")) {strncpy(g_script,v,sizeof g_script-1);g_script[sizeof g_script-1]=0;}
  else if(!strcmp(k,"renderer")){ g_want_vk= !strcmp(v,"vulkan"); extern void snxvk_ini(const char*); snxvk_ini(v); }   /* vulkan_native: snxvk_glue.c */
}

/* a status after the window title (shader pre-build progress); NULL restores the title */
static void backend_status_win(const char* s){ if(!g_hwnd) return; char* c=NULL; if(s){ size_t n=strlen(s)+1; c=malloc(n); if(!c) return; memcpy(c,s,n); }
  if(!PostMessageA(g_hwnd,WM_APP+10,0,(LPARAM)c)) free(c); }
void backend_ff_status(int on, int speed){ if(g_hwnd) PostMessageA(g_hwnd,WM_APP+9,(WPARAM)on,(LPARAM)speed); }
int video_has_gl(void){ return 1; }

void win_fatal_box(const char* msg){ MessageBoxA(g_hwnd,msg,"Metroid Prime 4 Beyond",MB_OK|MB_ICONERROR); }

/* ---------------------------------------------------------------- XInput, loaded dynamically */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XGP;
typedef struct { DWORD dwPacketNumber; XGP Gamepad; } XST;
typedef DWORD (WINAPI *XGetState)(DWORD, XST*);
static XGetState xget, xget_ex;   /* xget_ex: XInputGetStateEx (ordinal 100), the same plus the Guide button 0x0400 */
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } XVIB;
typedef DWORD (WINAPI *XSetState)(DWORD, XVIB*);
static XSetState xset; static int g_xpad=-1;   /* the XInput pad input last came from: rumble goes there */

static void xinput_load(void){
  static int done; if(done) return; done=1;   /* also called early by backend_pad_family */
  const char* dl[]={"xinput1_4.dll","xinput1_3.dll","xinput9_1_0.dll"};
  for(int i=0;i<3 && !xget;i++){ HMODULE h=LoadLibraryA(dl[i]); if(h){ xget=(XGetState)GetProcAddress(h,"XInputGetState"); xset=(XSetState)GetProcAddress(h,"XInputSetState"); if(i<2) xget_ex=(XGetState)GetProcAddress(h,(LPCSTR)(size_t)100); } }
  if(xset){ extern void xinput_rumble_off(void); atexit(xinput_rumble_off); }
}
void xinput_rumble_off(void){ if(xset){ XVIB v={0,0}; for(DWORD i=0;i<4;i++) xset(i,&v); } }   /* a motor left on would keep going after exit */

/* ---------------------------------------------------------------- SDL2 game controllers, loaded dynamically
   SDL2.dll next to the game (or on PATH) adds every controller SDL knows over USB and Bluetooth:
   Switch Pro / Joy-Con pair, DualShock 4, DualSense, 8BitDo, Xbox. Buttons map by position like
   XInput (Switch A = right face button), using SDL's positional layout for Nintendo pads.
   [Input] controller_api: auto (SDL when available, else XInput), sdl, xinput. */
typedef void* SDLGC;
static int (*sdl_init)(unsigned); static int (*sdl_sethint)(const char*,const char*);
static int (*sdl_numjoy)(void); static int (*sdl_isgc)(int); static SDLGC (*sdl_gcopen)(int);
static int (*sdl_gcattached)(SDLGC); static void (*sdl_gcclose)(SDLGC); static const char* (*sdl_gcname)(SDLGC);
static unsigned char (*sdl_gcbutton)(SDLGC,int); static short (*sdl_gcaxis)(SDLGC,int); static void (*sdl_gcupdate)(void);
static void (*sdl_pump)(void);
static int (*sdl_gcrumble)(SDLGC,unsigned short,unsigned short,unsigned);
static int sdl_ok; static SDLGC sdl_pad[8]; static int sdl_npad; static u64 sdl_rescan;
static int (*sdl_gctype)(int);   /* SDL_GameControllerTypeForIndex, SDL 2.0.12+ (optional) */
static void sdl_load(void){
  static int done; if(done) return; done=1;   /* also called early by backend_pad_family */
  extern int g_opt_ctrl_api; if(g_opt_ctrl_api==2) return;
  extern char g_exedir[]; char p[1200]; snprintf(p,sizeof p,"%sSDL2.dll",g_exedir);
  HMODULE h=LoadLibraryA(p); if(!h) h=LoadLibraryA("SDL2.dll");
  if(!h){ if(g_opt_ctrl_api==1) LOG("input: controller_api=sdl but SDL2.dll was not found next to the game\n"); return; }
#define SDLF(v,n) *(FARPROC*)&v=GetProcAddress(h,n); if(!v){ LOG("input: SDL2.dll lacks %s, using XInput\n",n); return; }
  SDLF(sdl_init,"SDL_Init") SDLF(sdl_sethint,"SDL_SetHint") SDLF(sdl_numjoy,"SDL_NumJoysticks") SDLF(sdl_isgc,"SDL_IsGameController")
  SDLF(sdl_gcopen,"SDL_GameControllerOpen") SDLF(sdl_gcattached,"SDL_GameControllerGetAttached") SDLF(sdl_gcclose,"SDL_GameControllerClose")
  SDLF(sdl_gcname,"SDL_GameControllerName") SDLF(sdl_gcbutton,"SDL_GameControllerGetButton") SDLF(sdl_gcaxis,"SDL_GameControllerGetAxis")
  SDLF(sdl_gcupdate,"SDL_GameControllerUpdate") SDLF(sdl_pump,"SDL_PumpEvents")
#undef SDLF
  *(FARPROC*)&sdl_gcrumble=GetProcAddress(h,"SDL_GameControllerRumble");
  *(FARPROC*)&sdl_gctype=GetProcAddress(h,"SDL_GameControllerTypeForIndex");   /* SDL 2.0.9+; optional: no rumble without it */
  sdl_sethint("SDL_GAMECONTROLLER_USE_BUTTON_LABELS","0");   /* positions, not printed labels */
  sdl_sethint("SDL_JOYSTICK_HIDAPI_PS4","1"); sdl_sethint("SDL_JOYSTICK_HIDAPI_PS5","1");
  sdl_sethint("SDL_JOYSTICK_HIDAPI_SWITCH","1"); sdl_sethint("SDL_JOYSTICK_HIDAPI_JOY_CONS","1"); sdl_sethint("SDL_JOYSTICK_HIDAPI_COMBINE_JOY_CONS","1");
  sdl_sethint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS","1");
  /* We poll from the game's input thread, not the thread that started SDL, and pump events only once a second.
     RawInput (SDL's default for XInput pads) only updates through window messages, so it would see no presses:
     use the XInput backend for those pads and let SDL service device messages on its own thread. */
  sdl_sethint("SDL_JOYSTICK_RAWINPUT","0"); sdl_sethint("SDL_JOYSTICK_THREAD","1");
  if(sdl_init(0x00002000u|0x00000200u)!=0){ LOG("input: SDL2 could not start, using XInput\n"); return; }   /* GAMECONTROLLER|JOYSTICK */
  sdl_ok=1; LOG("input: SDL2 controllers active (XInput pads also read)\n"); }
/* the kind of controller player 1 has, for the button icons (svc_fs.c), asked before the window and the input thread
   exist: 1 Xbox (or any other), 2 DualShock 4, 3 DualSense, 4 Switch controllers, 0 none */
static int sdl_family(int i){
  int t= sdl_gctype? sdl_gctype(i) : 0;   /* SDL_GameControllerType: 1/2 Xbox, 3 PS3, 4 PS4, 5 Switch Pro, 7 PS5, 11-13 Joy-Con */
  return t==4||t==3? 2 : t==7? 3 : t==5||t==11||t==12||t==13? 4 : 1; }
int backend_pad_family(void){
  extern int g_opt_ctrl_index; sdl_load(); xinput_load();
  if(sdl_ok){ sdl_pump(); int n=sdl_numjoy(), k=0;
    for(int j=0;j<n;j++) if(sdl_isgc(j)){ k++; if(g_opt_ctrl_index<=0 || k==g_opt_ctrl_index) return sdl_family(j); } }
  if(xget){ for(DWORD i=0;i<4;i++){ XST s; if(xget(i,&s)==0) return 1; } }
  return 0; }
static void sdl_scan(void){
  for(int i=0;i<sdl_npad;) if(!sdl_gcattached(sdl_pad[i])){ LOG("input: controller disconnected: %s\n",sdl_gcname(sdl_pad[i])? sdl_gcname(sdl_pad[i]) : "?"); sdl_gcclose(sdl_pad[i]); sdl_pad[i]=sdl_pad[--sdl_npad]; } else i++;
  int n=sdl_numjoy();
  for(int j=0;j<n && sdl_npad<8;j++){ if(!sdl_isgc(j)) continue; SDLGC g=sdl_gcopen(j); if(!g) continue;
    int have=0; for(int i=0;i<sdl_npad;i++) if(sdl_pad[i]==g) have=1;
    if(have) sdl_gcclose(g);   /* Open on an open pad only adds a reference */
    else { sdl_pad[sdl_npad++]=g; LOG("input: controller %d connected: %s\n",sdl_npad,sdl_gcname(g)? sdl_gcname(g) : "?"); } } }
/* returns 1 when SDL handled input this frame */
static int sdl_poll(u64* b, i32* lx, i32* ly, i32* rx, i32* ry){
  if(!sdl_ok) return 0;
  u64 now=plat_ns(); if(now-sdl_rescan>1000000000ull){ sdl_rescan=now; sdl_pump(); sdl_scan(); }
  sdl_gcupdate();
  extern int g_opt_ctrl_index, g_opt_deadzone;
  int first= g_opt_ctrl_index>0? g_opt_ctrl_index-1 : 0, last= g_opt_ctrl_index>0? g_opt_ctrl_index : sdl_npad;
  for(int i=first;i<last && i<sdl_npad;i++){ SDLGC g=sdl_pad[i];
    /* SDL: 0 A(south) 1 B(east) 2 X(west) 3 Y(north) 4 back 5 guide 6 start 7 lstick 8 rstick 9 lb 10 rb 11 up 12 down 13 left 14 right */
    if(sdl_gcbutton(g,1)) *b|=1<<0; if(sdl_gcbutton(g,0)) *b|=1<<1; if(sdl_gcbutton(g,3)) *b|=1<<2; if(sdl_gcbutton(g,2)) *b|=1<<3;
    if(sdl_gcbutton(g,7)) *b|=1<<4; if(sdl_gcbutton(g,8)) *b|=1<<5; if(sdl_gcbutton(g,9)) *b|=1<<6; if(sdl_gcbutton(g,10)) *b|=1<<7;
    if(sdl_gcaxis(g,4)>8000) *b|=1<<8; if(sdl_gcaxis(g,5)>8000) *b|=1<<9;
    if(sdl_gcbutton(g,6)) *b|=1<<10; if(sdl_gcbutton(g,4)) *b|=1<<11; if(sdl_gcbutton(g,5)) *b|=PAD_HOME;
    if(sdl_gcbutton(g,13)) *b|=1<<12; if(sdl_gcbutton(g,11)) *b|=1<<13; if(sdl_gcbutton(g,14)) *b|=1<<14; if(sdl_gcbutton(g,12)) *b|=1<<15;
    i32 ax=sdl_gcaxis(g,0), ay=-(i32)sdl_gcaxis(g,1), bx=sdl_gcaxis(g,2), by=-(i32)sdl_gcaxis(g,3);
    if(ay<-32767) ay=-32767; if(ay>32767) ay=32767; if(by<-32767) by=-32767; if(by>32767) by=32767;   /* SDL y is down-positive */
    int dzv=g_opt_deadzone; if(ax>-dzv && ax<dzv) ax=0; if(ay>-dzv && ay<dzv) ay=0; if(bx>-dzv && bx<dzv) bx=0; if(by>-dzv && by<dzv) by=0;
    if(!*lx && !*ly){ *lx=ax; *ly=ay; } if(!*rx && !*ry){ *rx=bx; *ry=by; } }
  return 1; }

static void set_fullscreen(int on){
  DWORD st=GetWindowLong(g_hwnd,GWL_STYLE);
  if(on && !g_fullscreen){
    MONITORINFO mi={sizeof mi};
    if(GetWindowPlacement(g_hwnd,&g_wp) && GetMonitorInfo(MonitorFromWindow(g_hwnd,MONITOR_DEFAULTTOPRIMARY),&mi)){
      SetWindowLong(g_hwnd,GWL_STYLE,st&~WS_OVERLAPPEDWINDOW);
      SetWindowPos(g_hwnd,HWND_TOP,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_NOOWNERZORDER|SWP_FRAMECHANGED); }
    g_fullscreen=1;   /* the cursor is hidden over the game by WM_SETCURSOR, so the menu can still show it */
  } else if(!on && g_fullscreen){
    SetWindowLong(g_hwnd,GWL_STYLE,st|WS_OVERLAPPEDWINDOW); SetWindowPlacement(g_hwnd,&g_wp);
    SetWindowPos(g_hwnd,NULL,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_FRAMECHANGED);
    g_fullscreen=0; }
}

/* fullscreen on/off from any thread (in-game menu, settings): done on the window thread */
/* the front end (same thread as the window) switches fullscreen with the game's own code, cursor kept visible */
void backend_fe_fullscreen(int on){ if((on!=0)==(g_fullscreen!=0)) return; set_fullscreen(on); }
void backend_set_fullscreen(int on){ if(g_hwnd) PostMessageA(g_hwnd,WM_APP+11,(WPARAM)(on!=0),0); }
int backend_is_fullscreen(void){ return g_fullscreen; }
void backend_release_keys(void){ kbm_release_all(); }
void backend_request_quit(void){ if(g_hwnd) PostMessageA(g_hwnd,WM_CLOSE,0,0); }   /* the in-game menu's Quit */
void* backend_hwnd(void){ return g_hwnd; }

/* optional: launch the Lossless Scaling app next to the game (lossless_scaling=1).
   This path doesn't touch its dll, the app just captures our window. */
static int file_exists_(const char* p){ DWORD a=GetFileAttributesA(p); return a!=INVALID_FILE_ATTRIBUTES && !(a&FILE_ATTRIBUTE_DIRECTORY); }

static int ls_find(char* out){
  if(g_ls_path[0] && file_exists_(g_ls_path)){ strcpy(out,g_ls_path); return 1; }
  static char libs[10][600]; int nl=0;
  typedef LONG (WINAPI*RGV)(HKEY,LPCSTR,LPCSTR,DWORD,LPDWORD,PVOID,LPDWORD);
  HMODULE a=LoadLibraryA("advapi32.dll"); RGV rgv= a? (RGV)GetProcAddress(a,"RegGetValueA") : NULL;
  char sp[600]; DWORD sz=sizeof sp;
  if(rgv && rgv(HKEY_CURRENT_USER,"Software\\Valve\\Steam","SteamPath",0x0000ffff,NULL,sp,&sz)==0 && sp[0]){ strncpy(libs[nl],sp,599); libs[nl][599]=0; nl++; }
  strcpy(libs[nl++],"C:\\Program Files (x86)\\Steam"); strcpy(libs[nl++],"C:\\Program Files\\Steam");
  { char vdf[700]; snprintf(vdf,sizeof vdf,"%s\\steamapps\\libraryfolders.vdf",libs[0]); FILE* f=fopen(vdf,"r");
    if(f){ char ln[800]; while(fgets(ln,sizeof ln,f) && nl<10){ char* p=strstr(ln,"\"path\""); if(!p) continue; p=strchr(p+6,'"'); if(!p) continue; p++; char* e=strchr(p,'"'); if(!e) continue; *e=0;
        int k=0; for(char* q=p;*q && k<599;q++){ if(q[0]=='\\' && q[1]=='\\') q++; libs[nl][k++]=*q; } libs[nl][k]=0; nl++; }
      fclose(f); } }
  for(int i=0;i<nl;i++){ char c[800]; snprintf(c,sizeof c,"%s\\steamapps\\common\\Lossless Scaling\\LosslessScaling.exe",libs[i]); for(char* q=c;*q;q++) if(*q=='/') *q='\\';
    if(file_exists_(c)){ strcpy(out,c); return 1; } }
  return 0; }

static void ls_launch(void){
  if(!g_ls_mode) return;
  if(FindWindowA(NULL,"Lossless Scaling")){ LOG("lossless scaling: already running\n"); return; }
  char p[1024]; if(!ls_find(p)){ LOG("lossless scaling: not found (install it from Steam, or set lossless_path=C:\\...\\LosslessScaling.exe in the .ini)\n"); return; }
  char dir[1024]; strcpy(dir,p); char* sl=strrchr(dir,'\\'); if(sl) *sl=0;
  STARTUPINFOA si; memset(&si,0,sizeof si); si.cb=sizeof si; si.dwFlags=STARTF_USESHOWWINDOW; si.wShowWindow=SW_SHOWMINNOACTIVE; PROCESS_INFORMATION pi;
  if(CreateProcessA(p,NULL,NULL,NULL,FALSE,0,NULL,dir,&si,&pi)){ CloseHandle(pi.hThread); CloseHandle(pi.hProcess); LOG("lossless scaling: started (%s)\n",p); }
  else LOG("lossless scaling: could not open %s (error %lu)\n",p,(unsigned long)GetLastError());
  SetForegroundWindow(g_hwnd); }

/* window title update for lsfg on/off, posted from the GPU thread */
static char g_title[256];

void backend_lsfg_state(int on, int mult){ if(g_hwnd) PostMessageA(g_hwnd,WM_APP+7,(WPARAM)on,(LPARAM)mult); }
/* F7 recording state in the title bar */
volatile int g_rec_toggle;

void backend_rec_state(int on){ if(g_hwnd) PostMessageA(g_hwnd,WM_APP+8,(WPARAM)on,0); }
static const char* HELP=
  "Metroid Prime 4 Beyond - controls\n\n"
  "XInput (Xbox) controller: same as the Switch controller (A/B swapped by position).\n\n"
  "Keyboard (PrimeHack-style; F1 > Controls or key_* in the .ini changes them):\n"
  "  W A S D / arrows = move    Space = B (jump)    Ctrl = Y (morph)    Shift = ZL (lock on)\n"
  "  E / Alt = X (visor)   F = R   Q = L   Enter = A   1 2 3 4 = D-pad weapons   R = +   Tab / ` = -\n\n"
  "Mouse (mouse_look=1; the menu or another window frees the cursor):\n"
  "  Move = camera   Left = A (fire)   Right = ZL   Middle = R   (mouse_buttons in the .ini changes them)\n\n"
  "F1 = menu   F3 = screenshot   F4 = FPS limit 60 / unlimited   F5 = diagnostic dump (trace)\n"
  "F7 = record .mp4 video (press F7 again to stop; saved in the recordings folder)\n"
  "F8 = instant full-resolution screenshot (PNG in the captures folder)\n"
  "F11 / Alt+Enter = fullscreen   Esc = quit";

/* key messages -> input_kbm.c key codes (its [Input] key_* bindings map them to the controller). Letters and digits
   follow the layout (their VK codes are their ASCII); punctuation goes by the key's place (its scan code), named as
   on a US keyboard, since layouts put different characters there */
static int kbm_from_vk(WPARAM k, LPARAM l){
  if((k>='A' && k<='Z') || (k>='0' && k<='9')) return (int)k;
  if(k>=VK_NUMPAD0 && k<=VK_NUMPAD9) return KBM_KEY_NUM0+(int)(k-VK_NUMPAD0);
  switch(k){
    case VK_SPACE: return KBM_KEY_SPACE;
    case VK_RETURN: return KBM_KEY_ENTER;
    case VK_BACK: return KBM_KEY_BACKSPACE;
    case VK_LEFT: return KBM_KEY_LEFT;
    case VK_UP: return KBM_KEY_UP;
    case VK_RIGHT: return KBM_KEY_RIGHT;
    case VK_DOWN: return KBM_KEY_DOWN;
    case VK_TAB: return KBM_KEY_TAB;
    case VK_SHIFT: return KBM_KEY_SHIFT;
    case VK_CONTROL: return KBM_KEY_CTRL;
    case VK_MENU: return KBM_KEY_ALT;
    case VK_CAPITAL: return KBM_KEY_CAPSLOCK;
    case VK_INSERT: return KBM_KEY_INSERT;
    case VK_DELETE: return KBM_KEY_DELETE;
    case VK_HOME: return KBM_KEY_HOME;
    case VK_END: return KBM_KEY_END;
    case VK_PRIOR: return KBM_KEY_PAGEUP;
    case VK_NEXT: return KBM_KEY_PAGEDOWN;
    case VK_ADD: return KBM_KEY_NUM_ADD;
    case VK_SUBTRACT: return KBM_KEY_NUM_SUB;
    case VK_MULTIPLY: return KBM_KEY_NUM_MUL;
    case VK_DIVIDE: return KBM_KEY_NUM_DIV;
    case VK_DECIMAL: return KBM_KEY_NUM_DECIMAL;
  }
  if(!(l & (1L<<24)))   /* not an extended key: the punctuation block, by scan code (PC set 1) */
    switch((l>>16) & 0xFF){
      case 0x29: return KBM_KEY_GRAVE;
      case 0x0C: return KBM_KEY_DASH;
      case 0x0D: return KBM_KEY_EQUALS;
      case 0x1A: return KBM_KEY_LBRACKET;
      case 0x1B: return KBM_KEY_RBRACKET;
      case 0x2B: return KBM_KEY_BACKSLASH;
      case 0x27: return KBM_KEY_SEMICOLON;
      case 0x28: return KBM_KEY_APOSTROPHE;
      case 0x33: return KBM_KEY_COMMA;
      case 0x34: return KBM_KEY_PERIOD;
      case 0x35: return KBM_KEY_SLASH;
    }
  return KBM_KEY_NONE; }

/* mouse look: hold the cursor inside the client area (Windows drops the clip when the window loses focus or moves) */
static void cursor_clip(HWND h, int on){
  if(on){ RECT c; GetClientRect(h,&c); MapWindowPoints(h,NULL,(POINT*)&c,2); ClipCursor(&c); }
  else if(g_cursor_clipped) ClipCursor(NULL);
  g_cursor_clipped=on; }

extern int fe_wndproc(void* h, unsigned m, unsigned long long w, long long l, long long* result);   /* beyond_ui/fe.cpp */
extern int ov_wndproc(void* h, unsigned m, unsigned long long w, long long l);   /* beyond_ui/ov.cpp: 1 = the menu took it */
extern int ov_is_open(void);
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l){
  if(h==g_fe_hwnd) return DefWindowProcA(h,m,w,l);
  if(m==WM_SIZE){ g_ww=LOWORD(l); g_wh=HIWORD(l); if(g_ww<1) g_ww=1; if(g_wh<1) g_wh=1; }
  if(g_fe_mode){ long long r=0; if(fe_wndproc(h,m,w,l,&r)) return (LRESULT)r; return DefWindowProcA(h,m,w,l); }
  if(m==WM_APP+11){ set_fullscreen((int)w); return 0; }
  /* the in-game menu's press-a-key rebinding sees keys first, so the key being bound doesn't also act in the menu */
  if((m==WM_KEYDOWN||m==WM_SYSKEYDOWN||m==WM_KEYUP||m==WM_SYSKEYUP) && kbm_capture_key(kbm_from_vk(w,l),m==WM_KEYDOWN||m==WM_SYSKEYDOWN)) return 0;
  if(!(m==WM_KEYDOWN && w==VK_F11) && !(m==WM_SYSKEYDOWN && w==VK_RETURN) && ov_wndproc(h,m,w,l)) return 0;
  switch(m){
  case WM_CLOSE: LOG("window: closed by the user or the system\n"); g_quit=1; return 0;
  case WM_QUERYENDSESSION: LOG("window: Windows is logging off / shutting down\n"); break;
  case WM_POWERBROADCAST: if(w==PBT_APMSUSPEND) LOG("window: the PC is going to sleep\n"); else if(w==PBT_APMRESUMEAUTOMATIC) LOG("window: the PC resumed from sleep\n"); break;
  case WM_ERASEBKGND: return 1;   /* GL/Vulkan cover the whole client area: a GDI erase with the black class brush blinks the screen (fullscreen Vulkan) */
  case WM_SIZE: g_ww=LOWORD(l); g_wh=HIWORD(l); if(g_ww<1) g_ww=1; if(g_wh<1) g_wh=1; return 0;
  case WM_SYSKEYDOWN: if(w==VK_RETURN){ if(!(l&(1<<30))) set_fullscreen(!g_fullscreen); return 0; } if(w==VK_F4) break; /* let Alt+F4 through */
  /* fallthrough */
  case WM_KEYDOWN:
    /* a held key auto-repeats (bit 30): the one-shot actions below only fire on the first press,
       or holding F7 would start and stop the recording every frame */
    if((l&(1<<30)) && (w==VK_ESCAPE||w==VK_F1||w==VK_F3||w==VK_F4||w==VK_F5||w==VK_F7||w==VK_F8||w==VK_F11)) return 0;
    if(w==VK_ESCAPE){ g_quit=1; return 0; }
    if(w==VK_F11){ set_fullscreen(!g_fullscreen); return 0; }
    if(w==VK_F1){ return 0; }   /* the in-game menu (ov_wndproc) */
    if(0){ MessageBoxA(h,HELP,"Metroid Prime 4 Beyond",MB_OK); }
    if(w==VK_F3){ g_shot_req=1; return 0; }
    if(w==VK_F7){ g_rec_toggle=1; return 0; }
    if(w==VK_TAB && g_opt_ff){ if(!(l & (1L<<30))){ extern volatile int g_ff_presses; g_ff_presses++; } return 0; }   /* fast forward toggle (fast_forward=1); else a bindable key */
    if(w==VK_F8){ extern volatile int g_shot8_req; g_shot8_req=1; return 0; }
    if(w==VK_F5){ extern void gpu_gl_dump_rts(void); extern void kernel_dump_threads(void); extern __thread FILE* g_log_redirect;
      CreateDirectoryA("captures",NULL); FILE* f=fopen("captures/threads.txt","w");   /* written next to the trace */
      if(f){ g_log_redirect=f; kernel_dump_threads(); g_log_redirect=NULL; fclose(f); }
      kernel_dump_threads(); gpu_gl_dump_rts(); return 0; }
    if(w==VK_F4){ if(l & (1L<<30)) return 0; g_opt_fps= g_opt_fps==0? 1 : 0; char t[64]; snprintf(t,sizeof t,"FPS limit: %s",g_opt_fps==0? "unlimited" : "60"); SetWindowTextA(h,t); LOG("settings: F4 fps_limit=%d\n",g_opt_fps); { extern void ini_set(const char*, const char*); char v[8]; snprintf(v,sizeof v,"%d",g_opt_fps); ini_set("fps_limit",v); } return 0; }
    kbm_key(kbm_from_vk(w,l),1); return 0;
  case WM_SYSKEYUP: case WM_KEYUP: kbm_key(kbm_from_vk(w,l),0); return 0;   /* also keeps a lone Alt from opening the window menu */
  case WM_KILLFOCUS: kbm_release_all(); g_mouse_down=0; cursor_clip(h,0); ReleaseCapture(); return 0;
  case WM_APP+12: cursor_clip(h,(int)w); return 0;   /* mouse look on / off, from video_poll */
  case WM_APP+13: { POINT p; if(GetCursorPos(&p) && WindowFromPoint(p)==h) SendMessageA(h,WM_SETCURSOR,(WPARAM)h,MAKELPARAM(HTCLIENT,WM_MOUSEMOVE)); return 0; }   /* the menu opened or closed: show or hide the cursor now, not at the next mouse move */
  case WM_APP+8: { char t[300]; if(w) snprintf(t,sizeof t,"%s  |  RECORDING (F7 to stop)",g_title); else snprintf(t,sizeof t,"%s  |  Video saved in the recordings folder",g_title); SetWindowTextA(h,t); return 0; }
  case WM_APP+10: { char t[400]; char* s=(char*)l; if(s){ snprintf(t,sizeof t,"%s  |  %s",g_title,s); free(s); } else snprintf(t,sizeof t,"%s",g_title); SetWindowTextA(h,t); return 0; }   /* backend_status_win */
  case WM_APP+9: { char t[160]; if(w) snprintf(t,sizeof t,"%s  |  FAST FORWARD x%d (Tab or L3+R3)",g_title,(int)l); else snprintf(t,sizeof t,"%s",g_title); SetWindowTextA(h,t); return 0; }
  case WM_APP+7: { char t[160]; if(w) snprintf(t,sizeof t,"%s  |  LSFG ON x%d (frame generation)",g_title,(int)l); else snprintf(t,sizeof t,"%s  |  LSFG standby",g_title); SetWindowTextA(h,t); return 0; }
  case WM_INPUT: { RAWINPUT ri; UINT sz=sizeof ri;
      if(GetRawInputData((HRAWINPUT)l,RID_INPUT,&ri,&sz,sizeof(RAWINPUTHEADER))!=(UINT)-1 && ri.header.dwType==RIM_TYPEMOUSE &&
         !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) kbm_mouse_motion((int)ri.data.mouse.lLastX,(int)ri.data.mouse.lLastY);
      break; }   /* DefWindowProc cleans up after the read */
  case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
  case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP: {
      int button= (m==WM_LBUTTONDOWN||m==WM_LBUTTONUP)? KBM_MOUSE_LEFT : (m==WM_RBUTTONDOWN||m==WM_RBUTTONUP)? KBM_MOUSE_RIGHT
                : (m==WM_MBUTTONDOWN||m==WM_MBUTTONUP)? KBM_MOUSE_MIDDLE : (HIWORD(w)==XBUTTON1? KBM_MOUSE_BACK : KBM_MOUSE_FORWARD);
      int down= m==WM_LBUTTONDOWN||m==WM_RBUTTONDOWN||m==WM_MBUTTONDOWN||m==WM_XBUTTONDOWN;
      kbm_mouse_button(button,down);
      g_mouse_down= down? (g_mouse_down|1<<button) : (g_mouse_down&~(1<<button));
      if(g_mouse_down && GetCapture()!=h) SetCapture(h); else if(!g_mouse_down) ReleaseCapture();   /* releases still arrive outside the window */
      return (m==WM_XBUTTONDOWN||m==WM_XBUTTONUP)? TRUE : 0; }
  case WM_SETCURSOR: if(LOWORD(l)==HTCLIENT && ov_is_open()){ SetCursor(LoadCursor(NULL,IDC_ARROW)); return TRUE; }
    if(kbm_mouse_look_enabled() && LOWORD(l)==HTCLIENT){ SetCursor(NULL); return TRUE; }   /* mouse look: no cursor over the game */
    if(g_fullscreen && LOWORD(l)==HTCLIENT){ SetCursor(NULL); return TRUE; } break;
  }
  return DefWindowProcA(h,m,w,l);
}

static char g_title[256];

static void set_pixel_format(HDC dc){
  PIXELFORMATDESCRIPTOR pfd={sizeof pfd,1,PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,PFD_TYPE_RGBA,32};
  pfd.cDepthBits=24; pfd.cStencilBits=8; pfd.iLayerType=PFD_MAIN_PLANE;
  SetPixelFormat(dc,ChoosePixelFormat(dc,&pfd),&pfd); }

/* window + message pump on their own thread so nothing stalls them */
extern int fe_run(void* hwnd);   /* beyond_ui/fe.cpp: its own message loop; 0 = play, 1 = quit */
static void win_thread(void* arg){
  HINSTANCE hi=GetModuleHandle(NULL);
  WNDCLASSA wc={0}; wc.style=CS_OWNDC; wc.lpfnWndProc=wndproc; wc.hInstance=hi; wc.hCursor=LoadCursor(NULL,IDC_ARROW);
  wc.hIcon=LoadIcon(hi,MAKEINTRESOURCE(1)); if(!wc.hIcon) wc.hIcon=LoadIcon(NULL,IDI_APPLICATION);
  wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH); wc.lpszClassName="MetroidPrime4RecompWnd";
  RegisterClassA(&wc);
  RECT r={0,0,1280,720}; AdjustWindowRect(&r,WS_OVERLAPPEDWINDOW,FALSE);
  g_hwnd=CreateWindowA("MetroidPrime4RecompWnd",g_title,WS_OVERLAPPEDWINDOW|(arg? 0 : WS_VISIBLE),CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,NULL,NULL,hi,NULL);
  if(arg){   /* front end first, in this window; the game continues in it after Play */
    g_fe_mode=1; g_fe_result=fe_run(g_hwnd); g_fe_mode=0;
    if(g_fe_result){ SetEvent(g_fe_done); return; }   /* the process ends */
    SetEvent(g_fe_done); WaitForSingleObject(g_video_go,INFINITE);
    { /* The front end drew with a D3D11 flip-model swapchain. Once DXGI has had such a swapchain on a window, OpenGL
         and Vulkan presents to that window never reach the screen (MEASURED 2026-10-22: after Play the Play screen
         stayed up while the game ran, OpenGL and Vulkan, windowed and fullscreen). The game gets a new window in the
         same place, size and fullscreen state; the front end's goes. g_wp keeps the windowed placement for F11 */
      HWND old=g_hwnd; int fs=g_fullscreen; WINDOWPLACEMENT wp={sizeof wp};
      if(fs) wp=g_wp; else GetWindowPlacement(old,&wp);   /* fullscreen: the windowed placement it was entered from */
      HWND nw=CreateWindowA("MetroidPrime4RecompWnd",g_title,WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1280,720,NULL,NULL,hi,NULL);
      if(nw){ g_fe_hwnd=old; g_hwnd=nw;
        if(fs){ wp.showCmd=SW_HIDE; SetWindowPlacement(nw,&wp); g_fullscreen=0; set_fullscreen(1); }   /* as F11 does: borderless over the monitor, g_wp saved */
        else SetWindowPlacement(nw,&wp);   /* normal or maximized, as the player left it */
        ShowWindow(nw,SW_SHOW); SetForegroundWindow(nw);
        ShowWindow(old,SW_HIDE); DestroyWindow(old);
        { RECT cr; GetClientRect(nw,&cr); g_ww=cr.right>0? cr.right : 1; g_wh=cr.bottom>0? cr.bottom : 1; }
        LOG("window: the game continues in a new window (%dx%d%s)\n",g_ww,g_wh,g_fullscreen? ", fullscreen" : ""); }
      else WARN("window: could not make the game's window: the picture may not show\n"); }
    SetWindowTextA(g_hwnd,g_title);
    if(!g_want_fs && g_fullscreen) set_fullscreen(0); }
  /* a window with a GL pixel format can't take a Vulkan swapchain reliably: with renderer=vulkan the GL
     context lives on a hidden window and the visible one is left to Vulkan */
  if(g_want_vk){ WNDCLASSA gc={0}; gc.style=CS_OWNDC; gc.lpfnWndProc=DefWindowProcA; gc.hInstance=hi; gc.lpszClassName="MetroidPrime4RecompGL";   /* own class: its size must not reach wndproc */
    RegisterClassA(&gc); g_glwnd=CreateWindowA("MetroidPrime4RecompGL","Metroid Prime 4 Beyond GL",WS_POPUP,0,0,64,64,NULL,NULL,hi,NULL); }
  g_hdc=GetDC(g_glwnd? g_glwnd : g_hwnd);
  { extern int g_snxvk_want; if(!g_snxvk_want) set_pixel_format(g_hdc); }   /* native Vulkan: no GL pixel format on its window (set later if it falls back) */
  if(g_want_fs) set_fullscreen(1);
  ShowWindow(g_hwnd,SW_SHOW);
  { RAWINPUTDEVICE d={1,2,0,g_hwnd}; if(!RegisterRawInputDevices(&d,1,sizeof d)) LOG("input: raw mouse input unavailable: no mouse look\n"); }   /* generic desktop mouse */
  ls_launch();
  SetEvent(g_ready);
  MSG msg; while(GetMessageA(&msg,NULL,0,0)>0){ TranslateMessage(&msg); DispatchMessageA(&msg); }
  g_quit=1;
}

void video_init(const char* title){
  { extern void (*g_backend_status)(const char*); g_backend_status=backend_status_win; }
  snprintf(g_title,sizeof g_title,"%s",title);
  xinput_load(); sdl_load(); timeBeginPeriod(1);
  if(!g_win_started){ g_ready=CreateEventA(NULL,TRUE,FALSE,NULL); _beginthread(win_thread,1<<20,NULL); }
  else SetEvent(g_video_go);   /* the front end ran in the window: continue there */
  WaitForSingleObject(g_ready,INFINITE);
  g_start=plat_ns(); if(getenv("SWR_REC")) g_rec_toggle=1;
  { /* under Proton the driver is Mesa; glthread has to be enabled from the Steam launch options */
    HMODULE nt=GetModuleHandleA("ntdll.dll"); if(nt && GetProcAddress(nt,"wine_get_version"))
      LOG("Proton/Wine detected: for more FPS put   mesa_glthread=true %%command%%   in the game's Steam launch options (moves the OpenGL driver work to another core)\n"); }
  { extern int snxvk_start(void); extern int g_snxvk_want;
    if(g_snxvk_want && snxvk_start()) return;   /* renderer=vulkan_native: the ScarletNX Vulkan renderer draws, no GL */
    if(g_snxvk_want) set_pixel_format(g_hdc); }
  extern void gpu_gl_start(void); gpu_gl_start();
}

/* Play screen / first-run setup before the game starts (main.c). 0 = go on with the game, 1 = quit.
   Skipped for test runs (replays, exit frame, --no-launcher) and when the front end decides to (skip_launcher=1, --play). */
extern int fe_should_show(int argc, char** argv, const char* exedir, const char* exename);   /* beyond_ui/fe.cpp */
int backend_frontend(int argc, char** argv, const char* exedir, const char* exename){
  if(!getenv("SWR_FE_TEST") && (getenv("SWR_TEST_SCRIPT") || getenv("SWR_TEST_EXIT_FRAME") || getenv("SWR_TEST_RECORD"))) return 0;   /* SWR_FE_TEST: front-end tests */
  for(int i=1;i<argc;i++) if(!strcmp(argv[i],"--no-launcher")) return 0;
  if(!fe_should_show(argc,argv,exedir,exename)) return 0;
  snprintf(g_title,sizeof g_title,"Metroid Prime 4 Beyond");
  g_ready=CreateEventA(NULL,TRUE,FALSE,NULL); g_fe_done=CreateEventA(NULL,TRUE,FALSE,NULL); g_video_go=CreateEventA(NULL,TRUE,FALSE,NULL);
  g_win_started=1; _beginthread(win_thread,1<<20,(void*)1);
  WaitForSingleObject(g_fe_done,INFINITE);
  return g_fe_result; }

#ifdef SWR_HAVE_SNXVK
/* the window side of renderer=vulkan_native (snxvk_glue.c): the loader, the instance extensions and a surface on g_hwnd */
void* backend_vk_loader(void){ HMODULE m=LoadLibraryA("vulkan-1.dll"); return m? (void*)GetProcAddress(m,"vkGetInstanceProcAddr") : NULL; }
const char* const* backend_vk_extensions(u32* n){ static const char* const e[2]={"VK_KHR_surface","VK_KHR_win32_surface"}; *n=2; return e; }
int backend_vk_surface(void* instance, u64* surface){
  typedef void* (__stdcall *GIPA)(void*,const char*); typedef int (__stdcall *CWS)(void*,const void*,const void*,u64*);
  struct { int sType; const void* pNext; u32 flags; HINSTANCE hinstance; HWND hwnd; } ci={1000009000,NULL,0,GetModuleHandle(NULL),g_hwnd};
  GIPA gipa=(GIPA)backend_vk_loader(); CWS cws= gipa? (CWS)gipa(instance,"vkCreateWin32SurfaceKHR") : NULL;
  if(!cws || cws(instance,&ci,NULL,surface)){ LOG("vulkan_native: no Vulkan surface on the window\n"); return 0; }
  return 1; }
u32 backend_window_generation(void){ return 1; }   /* the swapchain follows the window size by itself */
void backend_window_safe(int* x, int* y, int* w, int* h){ *x=0; *y=0; *w=g_ww; *h=g_wh; }
int backend_window_shown(void){ return g_hwnd && !IsIconic(g_hwnd); }
void* backend_window_monitor(void){ return g_hwnd? (void*)MonitorFromWindow(g_hwnd,MONITOR_DEFAULTTOPRIMARY) : NULL; }   /* HDR peak lookup (snxvk_glue.c) */
#endif

/* ---------------------------------------------------------------- GL, GPU thread */
typedef HGLRC (WINAPI *CCA)(HDC,HGLRC,const int*);
typedef BOOL (WINAPI *SWI)(int);
static void* backend_gl_proc_raw(const char* n);
#include "vk_present.inc"

int backend_gl_create(void){
  HGLRC tmp=wglCreateContext(g_hdc); if(!tmp) return 0;
  wglMakeCurrent(g_hdc,tmp);
  CCA cca=(CCA)wglGetProcAddress("wglCreateContextAttribsARB");
  static const int vers[4][2]={{4,6},{4,5},{4,4},{4,3}};
  for(int i=0;i<4 && !g_ctx && cca;i++){ int ca[]={0x2091,vers[i][0],0x2092,vers[i][1],0x9126,0x00000001,0x2094,getenv("SWR_GL_DRIVER_DEBUG")?1:0,0}; g_ctx=cca(g_hdc,NULL,ca);
    if(g_ctx) g_wctx=cca(g_hdc,g_ctx,ca); }   /* second shared context for the shader compile thread */
  wglMakeCurrent(NULL,NULL); wglDeleteContext(tmp);
  if(!g_ctx){ fatal("Your graphics card does not support OpenGL 4.3.\nPlease update your GPU driver."); return 0; }
  wglMakeCurrent(g_hdc,g_ctx);
  if(g_glwnd && !vkp_init()){
    /* Vulkan unavailable: give the real window the same pixel format and render to it with GL as usual */
    HDC dc=GetDC(g_hwnd); set_pixel_format(dc);
    if(wglMakeCurrent(dc,g_ctx)){ g_hdc=dc; vkp.fallback_window=1; }
    else fatal("Vulkan output failed and the OpenGL fallback could not use the window.\nSet renderer=opengl in the .ini (or pick OpenGL in the launcher)."); }
  SWI si=(SWI)wglGetProcAddress("wglSwapIntervalEXT"); extern int g_opt_vsync; if(si) si(g_opt_vsync?1:0);
  return 1; }

static void* backend_gl_proc_raw(const char* n){
  void* p=(void*)wglGetProcAddress(n);
  if(!p || p==(void*)1 || p==(void*)2 || p==(void*)3 || p==(void*)-1){ static HMODULE gl; if(!gl) gl=LoadLibraryA("opengl32.dll"); p=(void*)GetProcAddress(gl,n); }
  return p; }

void* backend_gl_proc(const char* n){
  if(vkp.on && !strcmp(n,"glBindFramebuffer")) return (void*)vkp_bind_framebuffer;   /* framebuffer 0 = the image Vulkan presents */
  return backend_gl_proc_raw(n); }

void backend_gl_vsync(int on){ SWI si=(SWI)wglGetProcAddress("wglSwapIntervalEXT"); if(si) si(on?1:0); }   /* GL thread */
void backend_gl_swap(void){ if(vkp.on) vkp_swap(); else SwapBuffers(g_hdc); }

int backend_gl_worker_current(void){ return g_wctx && wglMakeCurrent(g_hdc,g_wctx); }

void backend_window_size(int* w, int* h){ *w=g_ww; *h=g_wh; }

void video_present_rgba(const u32* px, int w, int h, int stride){ extern void gpu_gl_present_cpu(const u32*,int,int,int); gpu_gl_present_cpu(px,w,h,stride); }

static i32 dz(i32 v){ return (v>-7000 && v<7000)? 0 : v; }


/* Test-only virtual-controller replay. CSV: guest_frame,hex_buttons,lx,ly,rx,ry.
   State holds until the next row. Normal input is untouched when env is unset.
   Frame numbers are present submissions, not wall-clock timestamps. */
typedef struct {u32 frame;u64 buttons;i32 lx,ly,rx,ry;} TestInput;
static TestInput* test_inputs;static u32 test_n,test_i;static int test_loaded,test_replay;
static FILE*test_record;static TestInput test_previous;static int test_recorded;
static void test_input_load(void){
  if(test_loaded)return;test_loaded=1;
  const char*path=getenv("SWR_TEST_SCRIPT");
  if(path&&*path){test_replay=1;FILE*f=fopen(path,"r");char line[256];u32 cap=8192;
    if(!f){WARN("test input: cannot open replay %s\n",path);ExitProcess(2);}
    test_inputs=calloc(cap,sizeof*test_inputs);if(!test_inputs){fclose(f);ExitProcess(2);}
    while(fgets(line,sizeof line,f)){if(line[0]=='#'||line[0]=='\n')continue;TestInput v={0};unsigned long long mask;
      if(sscanf(line,"%u,%llx,%d,%d,%d,%d",&v.frame,&mask,&v.lx,&v.ly,&v.rx,&v.ry)!=6||test_n==cap||(test_n&&v.frame<test_inputs[test_n-1].frame)||v.lx< -32768||v.lx>32767||v.ly< -32768||v.ly>32767||v.rx< -32768||v.rx>32767||v.ry< -32768||v.ry>32767){fclose(f);WARN("test input: invalid replay row\n");ExitProcess(2);}
      v.buttons=(u64)mask;test_inputs[test_n++]=v;
    }fclose(f);if(!test_n||test_inputs[0].frame!=0){WARN("test input: replay must start at frame 0\n");ExitProcess(2);}
    LOG("test input: frame replay active, %u states\n",test_n);
  }
  path=getenv("SWR_TEST_RECORD");if(path&&*path){test_record=fopen(path,"w");if(!test_record)ExitProcess(2);fprintf(test_record,"# guest_frame,buttons,lx,ly,rx,ry\n");}
}
static void test_input_apply(u64*b,i32*lx,i32*ly,i32*rx,i32*ry){
  test_input_load();u32 frame=(u32)g_frames;
  { static int done; const char*se; if(!done&&(se=getenv("SWR_TEST_SETENV"))&&*se){ u32 at=(u32)strtoul(se,NULL,10); const char*kv=strchr(se,':');   /* test only: "FRAME:NAME=VALUE" sets an env var at that guest frame */
      if(!kv){done=1;} else if(frame>=at){ char k[64]; const char*eq=strchr(kv+1,'='); if(eq&&eq-kv-1<64){ memcpy(k,kv+1,eq-kv-1); k[eq-kv-1]=0; SetEnvironmentVariableA(k,eq+1); { char pe[200]; snprintf(pe,sizeof pe,"%s=%s",k,eq+1); _putenv(pe); } LOG("test: frame %u set %s=%s\n",frame,k,eq+1); } done=1; } } }
  if(test_replay){while(test_i+1<test_n&&test_inputs[test_i+1].frame<=frame)test_i++;TestInput*v=&test_inputs[test_i];*b=v->buttons;*lx=v->lx;*ly=v->ly;*rx=v->rx;*ry=v->ry;}
  if(test_record&&(!test_recorded||*b!=test_previous.buttons||*lx!=test_previous.lx||*ly!=test_previous.ly||*rx!=test_previous.rx||*ry!=test_previous.ry)){
    fprintf(test_record,"%u,%llx,%d,%d,%d,%d\n",frame,(unsigned long long)*b,*lx,*ly,*rx,*ry);fflush(test_record);
    test_previous=(TestInput){frame,*b,*lx,*ly,*rx,*ry};test_recorded=1;
  }
}

/* mouse look holds the cursor in the window while the game has the mouse: the window focused, the menu closed */
static void cursor_sync(void){
  static int clipped=-1; static u64 last;
  int want= kbm_mouse_look_enabled() && !ov_is_open() && GetForegroundWindow()==g_hwnd;
  u64 now=plat_ns();
  if(want==clipped && !(want && now-last>500000000ull)) return;   /* while on, renewed twice a second: the window may have moved */
  clipped=want; last=now; PostMessageA(g_hwnd,WM_APP+12,(WPARAM)want,0); }
static void cursor_menu_sync(void){
  static int was=-1; int open=ov_is_open();
  if(open!=was){ was=open; PostMessageA(g_hwnd,WM_APP+13,0,0); } }

void video_poll(void){
  u64 k=0, b=0; i32 lx, ly, rx, ry;
  kbm_keyboard(&k,&lx,&ly,&rx,&ry);
  int sdl_on=sdl_poll(&b,&lx,&ly,&rx,&ry);
  extern int g_opt_ctrl_api;
  { static int seen; if(sdl_on && !(seen&1) && (b&0xFFFF)){ seen|=1; LOG("input: first button press seen through SDL2\n"); } }
  if(xget && (!sdl_on || g_opt_ctrl_api!=1)){   /* auto: XInput pads also read directly, so they work even if SDL misses them */
    for(DWORD i=0;i<4;i++){ XST s; if((xget_ex? xget_ex : xget)(i,&s)) continue; XGP* g=&s.Gamepad; WORD x=g->wButtons; if(x&0x0400) b|=PAD_HOME;
      /* map by position like a Switch pad: A right (xbox B), B bottom (xbox A), X top, Y left */
      if(x&0x2000) b|=1<<0; if(x&0x1000) b|=1<<1; if(x&0x8000) b|=1<<2; if(x&0x4000) b|=1<<3;
      if(x&0x0040) b|=1<<4; if(x&0x0080) b|=1<<5; if(x&0x0100) b|=1<<6; if(x&0x0200) b|=1<<7;
      if(g->bLeftTrigger>60) b|=1<<8; if(g->bRightTrigger>60) b|=1<<9;
      if(x&0x0010) b|=1<<10; if(x&0x0020) b|=1<<11;
      if(x&0x0004) b|=1<<12; if(x&0x0001) b|=1<<13; if(x&0x0008) b|=1<<14; if(x&0x0002) b|=1<<15;
      if(!lx&&!ly){ lx=dz(g->sThumbLX); ly=dz(g->sThumbLY); } if(!rx&&!ry){ rx=dz(g->sThumbRX); ry=dz(g->sThumbRY); }
      { static int xseen; if(!xseen && x){ xseen=1; LOG("input: first button press seen through XInput (pad %lu)\n",(unsigned long)i); } }
      g_xpad=(int)i;
      break; }
  }
  u64 phys=b; i32 plx=lx, ply=ly; b&=~PAD_HOME;
  b=swr_pad_remap(b)|(k&0xFFFFull);
  /* stick-as-button bits 16..23 */
  if(lx<-16000) b|=1ull<<16; if(ly>16000) b|=1ull<<17; if(lx>16000) b|=1ull<<18; if(ly<-16000) b|=1ull<<19;
  if(rx<-16000) b|=1ull<<20; if(ry>16000) b|=1ull<<21; if(rx>16000) b|=1ull<<22; if(ry<-16000) b|=1ull<<23;
  if(g_script[0]){ /* test input like the X11 backend: "ms:hex,ms:hex,..." holds those buttons for 300 ms */
    u64 el=(plat_ns()-g_start)/1000000ull; const char* p=g_script;
    while(*p){ u64 t=strtoull(p,(char**)&p,10); if(*p!=':') break; p++; u64 m=strtoull(p,(char**)&p,16); if(el>=t && el<t+300){ b|=m; if(m&(1ull<<16))lx=-32767; if(m&(1ull<<18))lx=32767; if(m&(1ull<<17))ly=32767; if(m&(1ull<<19))ly=-32767; if(m&(1ull<<20))rx=-32767; if(m&(1ull<<22))rx=32767; if(m&(1ull<<21))ry=32767; if(m&(1ull<<23))ry=-32767; } if(*p==',') p++; } }
  kbm_mouse(&b,&rx,&ry,!ov_is_open());   /* the menu uses the mouse itself while open */
  cursor_sync(); cursor_menu_sync();
  test_input_apply(&b,&lx,&ly,&rx,&ry);
  if(test_replay){ phys=b; plx=lx; ply=ly; }   /* tests drive the menu with the replayed buttons */
  { extern int ov_pad(unsigned long long phys, int lx, int ly);   /* beyond_ui/ov.cpp: 1 = the menu has the controller */
    if(ov_pad(phys,plx,ply)){ b=0; lx=ly=rx=ry=0; } }
  { /* rumble (svc_hid.c hid_rumble_get) to the pads input comes from: on a change, else refreshed every 100 ms
       (SDL's rumble runs for the duration given, so it stops by itself when the game stops sending) */
    extern void hid_rumble_get(float*,float*); float lo, hi; hid_rumble_get(&lo,&hi);
    static unsigned short plo=1, phi=1; static u64 last; u64 now=plat_ns();
    unsigned short ulo=(unsigned short)(lo*65535.0f), uhi=(unsigned short)(hi*65535.0f);
    if(ulo!=plo || uhi!=phi || ((ulo||uhi) && now-last>100000000ull)){ plo=ulo; phi=uhi; last=now;
      extern int g_opt_ctrl_index;
      if(sdl_ok && sdl_gcrumble){ int first= g_opt_ctrl_index>0? g_opt_ctrl_index-1 : 0, lastp= g_opt_ctrl_index>0? g_opt_ctrl_index : sdl_npad;
        for(int i=first;i<lastp && i<sdl_npad;i++) sdl_gcrumble(sdl_pad[i],ulo,uhi,ulo||uhi? 250 : 0); }
      if(xset && g_xpad>=0){ XVIB v={ulo,uhi}; xset((DWORD)g_xpad,&v); } } }
  g_pad.buttons=b; g_pad.lx=lx; g_pad.ly=ly; g_pad.rx=rx; g_pad.ry=ry;
}

void backend_test_guest_start(void){if(g_script[0]&&getenv("SWR_SCRIPT_GUEST_CLOCK"))g_start=plat_ns();}

void frontend_frame(void){
  extern u64 gpu_gl_screenshot(const char* path);
  if(g_script[0]){
    static int configured;static const char*times;static const char*prefix;static u64 next;
    if(!configured){configured=1;times=getenv("SWR_TEST_CAPTURE_MS");prefix=getenv("SWR_TEST_CAPTURE_PREFIX");if(times&&prefix)next=strtoull(times,(char**)&times,10);else times=NULL;}
    u64 elapsed=(plat_ns()-g_start)/1000000ull;
    if(times&&next&&elapsed>=next){char path[240];snprintf(path,sizeof path,"%s-%llu",prefix,(unsigned long long)next);
      LOG("test capture: due=%llu elapsed=%llu ns=%llu\n",(unsigned long long)next,(unsigned long long)elapsed,(unsigned long long)plat_ns());gpu_gl_screenshot(path);
      if(*times==','){times++;next=strtoull(times,(char**)&times,10);}else{times=NULL;next=0;}
    }
  }


  if(getenv("SWR_TEST_SCRIPT")){
    static int configured;static const char*times;static const char*prefix;static u64 next;
    if(!configured){configured=1;times=getenv("SWR_TEST_CAPTURE_FRAMES");prefix=getenv("SWR_TEST_CAPTURE_PREFIX");if(times&&prefix)next=strtoull(times,(char**)&times,10);else times=NULL;}
    if(times&&next&&(u64)g_frames>=next){char path[1024];snprintf(path,sizeof path,"%s-f%llu",prefix,(unsigned long long)next);
      LOG("test capture: requested_frame=%llu actual_frame=%d ns=%llu\n",(unsigned long long)next,g_frames,(unsigned long long)plat_ns());gpu_gl_screenshot(path);
      if(*times==','){times++;next=strtoull(times,(char**)&times,10);}else{times=NULL;next=0;}
    }

    {static int changed;const char*target=getenv("SWR_TEST_FPS_AFTER_LOAD");
      if(target&&!changed&&g_frames>=2300){int fps=atoi(target);if(fps!=0&&fps!=60&&fps!=120&&fps!=240){WARN("test FPS: invalid target\n");ExitProcess(2);}
        changed=1;g_opt_fps=fps;LOG("test FPS: selected=%d frame=%d ns=%llu\n",fps,g_frames,(unsigned long long)plat_ns());
      }
    }
    static u64 heartbeat;u64 now=plat_ns();if(!heartbeat||now-heartbeat>=1000000000ull){heartbeat=now;LOG("test heartbeat: frame=%d fps_limit=%d ns=%llu\n",g_frames,g_opt_fps,(unsigned long long)now);}
    /* SWR_TEST_CRASH_FRAME=N: a crash inside a C-library DLL on this runtime thread (tests the crash report) */
    { const char*cr=getenv("SWR_TEST_CRASH_FRAME"); if(cr&&(u64)g_frames>=strtoull(cr,NULL,10)){ void*(*volatile ms)(void*,int,size_t)=memset; ms((void*)(uintptr_t)0x10,0,16); } }
    const char*stop=getenv("SWR_TEST_EXIT_FRAME");if(stop&&(u64)g_frames>=strtoull(stop,NULL,10)){LOG("test complete: frame=%d\n",g_frames);extern void save_hints(void);save_hints();if(getenv("SWR_VK_RECIPES_RECORD")){extern void snxvk_exit(void);snxvk_exit();}log_flush();fflush(NULL);ExitProcess(0);}
  }

  if(g_shot_req){ g_shot_req=0; char p[MAX_PATH]; SYSTEMTIME t; GetLocalTime(&t); CreateDirectoryA("captures",NULL);
    snprintf(p,sizeof p,"captures/screenshot_%04d%02d%02d_%02d%02d%02d",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond); gpu_gl_screenshot(p); }
  if(g_seconds && plat_ns()-g_start>=(u64)g_seconds*1000000000ull){
    u64 h=gpu_gl_screenshot(g_shot[0]? g_shot : NULL); fprintf(stderr,"\nframes=%d hash=%016llx\n",g_frames,(unsigned long long)h);
    extern void save_hints(void); save_hints(); { extern void gpu_gl_rec_finish(void); gpu_gl_rec_finish(); } log_flush(); fflush(NULL); ExitProcess(0); }
}

int backend_glq_ok(void){ return 1; }   /* GL calls go through glq */
