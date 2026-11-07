/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
#define _GNU_SOURCE
/* startup: load GAME_code.dll/.so, set up memory + services + main thread, then run the frontend
   loop (window, input, vsync). */
#include "rt.h"
#include <sys/stat.h>
#include "input_kbm.h"
#define KBM_INI_HELP \
  "; Keyboard: key_<button> = keys or chords, comma separated (key_x=E, Alt). A chord is keys held together,\n" \
  ";   joined by + (key_y=Shift+1); - = none. Keys: A-Z 0-9 Space Enter Backspace Tab Grave (left of 1) Shift Ctrl\n" \
  ";   Alt CapsLock Insert Delete Home End PageUp PageDown Up Down Left Right Dash Equals LBracket RBracket Backslash\n" \
  ";   Semicolon Apostrophe Comma Period Slash Num0-Num9 NumAdd NumSub NumMul NumDiv NumDecimal. Punctuation keys go\n" \
  ";   by their place on a US keyboard. Esc and F1-F12 stay with the game window (quit, menu, screenshots...); Tab is\n" \
  ";   fast forward while fast_forward=1. The in-game menu (F1 > Controls) rebinds them by pressing the key\n"

const SwrModule* g_mod;
GuestFn* g_disp;
GuestFn swr_interp_entry;
char g_datadir[1024], g_exename[256], g_exedir[1024];
volatile int g_quit;
int g_frames;
int g_opt_video_async_retire=0,g_opt_video_avx2=1,g_opt_video_block_copy=1;   /* both checked byte for byte against the plain versions */
int g_present_pacing_candidate;
int g_opt_audio_surround_downmix=1;   /* Prime 4 mixes in 5.1 with dialogue in the center channel: front-only stereo dropped the voices */
/* FPS counter ([Graphics] show_fps): a seven-segment "123 FPS" in the top-left corner as plain rectangles, so every
   build and renderer can draw it with clears (gpu_gl.c window pass, snxvk hdr.c present pass). Counted from presents,
   i.e. the frames that reach the screen, over half a second. Each rect: x, y (top-left origin), w, h, 0 back / 1 text.
   With FSR frame generation running it counts presents of the game's own frames only, so it shows two rows: what
   reaches the screen (those plus the generated ones) as "FPS", and under it what the game rendered as "rEnd" */
int g_opt_show_fps=0;
volatile unsigned long long g_fg_generated_frames;   /* frames FSR frame generation made so far (snxvk fsr_fg.c) */
static int g_fps_rows=1;
int fps_overlay_rows(void){ return g_opt_show_fps ? g_fps_rows : 0; }   /* snxvk hdr.c: its skip prompt goes below */
volatile int g_skip_prompt;            /* the native Vulkan renderer's top-left prompt overlay (snxvk hdr.c): nothing sets it */
volatile float g_skip_hold;
static void env_switch(const char* name, int on);
/* one row of seven-segment glyphs, its back first; returns the new rect count */
static int fps_row(int (*r)[5],int n,int max,int x0,int y0,float s,int value,const u8* label,int nl){
  static const u8 digit[10]={0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F};
  u8 glyph[12];int ng=0;
  if(value<0)glyph[ng++]=0x40;
  else{char t[8];int k=snprintf(t,sizeof t,"%d",value>999?999:value);for(int i=0;i<k;i++)glyph[ng++]=digit[t[i]-'0'];}
  glyph[ng++]=0;for(int i=0;i<nl;i++)glyph[ng++]=label[i];
  int dh=(int)(26*s+0.5f),dw=(int)(14*s+0.5f),t=(int)(3*s+0.5f),gap=(int)(6*s+0.5f),pad=(int)(8*s+0.5f);
  if(t<2)t=2;
  int total=ng*dw+(ng-1)*gap;
  if(n<max){r[n][0]=x0-pad;r[n][1]=y0-pad;r[n][2]=total+2*pad;r[n][3]=dh+2*pad;r[n][4]=0;n++;}
  int half=dh/2;
  for(int i=0;i<ng;i++){int x=x0+i*(dw+gap),y=y0;
    const int seg[7][4]={{x,y,dw,t},{x+dw-t,y,t,half+t/2},{x+dw-t,y+half-t/2,t,dh-half+t/2},{x,y+dh-t,dw,t},{x,y+half-t/2,t,dh-half+t/2},{x,y,t,half+t/2},{x,y+half-t/2,dw,t}};
    for(int k=0;k<7&&n<max;k++)if(glyph[i]>>k&1){r[n][0]=seg[k][0];r[n][1]=seg[k][1];r[n][2]=seg[k][2];r[n][3]=seg[k][3];r[n][4]=1;n++;}}
  return n;
}
int fps_overlay_rects(int W,int H,int (*r)[5],int max){
  static u64 t0,g0;static u32 frames;static int fps=-1,shown=-1;
  if(!g_opt_show_fps){t0=0;frames=0;fps=-1;shown=-1;g_fps_rows=1;return 0;}
  u64 now=plat_ns();if(!t0){t0=now;g0=g_fg_generated_frames;}else frames++;
  if(now-t0>=500000000ull){
    u64 g=g_fg_generated_frames;
    fps=(int)(((u64)frames*2000000000ull/(now-t0)+1)/2);
    shown=g!=g0?fps+(int)(((g-g0)*2000000000ull/(now-t0)+1)/2):-1;   /* -1: no frame generated in the half second */
    t0=now;frames=0;g0=g;}
  static const u8 FPS[3]={0x71,0x73,0x6D},REND[4]={0x50,0x79,0x54,0x5E};   /* F P S, r E n d */
  float s=(float)H/1080.f;if(s<0.5f)s=0.5f;(void)W;
  int x0=(int)(16*s+0.5f),y0=x0,n=0;
  if(shown<0){g_fps_rows=1;return fps_row(r,n,max,x0,y0,s,fps,FPS,3);}
  g_fps_rows=2;
  n=fps_row(r,n,max,x0,y0,s,shown,FPS,3);
  return fps_row(r,n,max,x0,y0+(int)(52*s+0.5f),s,fps,REND,4);
}
int g_opt_audio_eos_sample_count=1;   /* always on (cinematic clock ~2x and cut music without it); SWR_AUDIO_OLD_EOS=1 = off, tests only */
char g_opt_pad_map[128]="";   /* physical-position button remapping, empty = identity */
int g_opt_ctrl_api=0, g_opt_ctrl_index=0, g_opt_deadzone=7000;
char g_opt_button_icons[16]="auto";
int g_opt_mouse_look=1, g_opt_mouse_sens=100, g_opt_mouse_invert_y=0, g_opt_mouse_deadzone=15;   /* [Input] mouse look (input_kbm.c) */
char g_opt_mouse_buttons[64]="a,zl,r,-,-";   /* left, right, middle, back, forward -> Switch buttons */   /* [Input] button_icons: auto, switch, xbox, ps4, ps5 (svc_fs.c romfs_apply_mods) */   /* [Input]: api 0 auto, 1 SDL2, 2 XInput */
int g_opt_diag=0;
/* vsync: on by default off Windows. Without it the 60.00 FPS limiter beats against the display (59.95 Hz here): about once a
   second a frame came in the same refresh as the one before and the next refresh showed the old one again (59 of 3600
   frames in the intro, 2 with vsync) */
#ifdef _WIN32
#define VSYNC_DEFAULT 0
#else
#define VSYNC_DEFAULT 1
#endif
int g_opt_docked=0, g_opt_lang=1, g_opt_res=1, g_opt_fps=1, g_opt_vsync=VSYNC_DEFAULT, g_opt_drs=0;
int g_opt_media_turns=1;   /* serialize_media_threads, read by kernel.c core0_member */
int g_opt_sleep_waits=1;   /* sleep_waits, read by glq.c */
int g_opt_hdr=0, g_opt_hdr_paper=200, g_opt_hdr_peak=0;   /* HDR output (native Vulkan): on, SDR white in nits, peak (0 = the display's) */
int g_opt_aniso=0;         /* anisotropy: 0 = the game's, else at least that much (gpu_gl.c samplers, native Vulkan) */
int g_opt_tex_threads=0;   /* texture_threads, read by gpu_gl.c pf_run: 0 auto, 1 off */
int g_opt_ff=0, g_opt_ff_speed=3;   /* fast_forward (Tab / L3+R3 toggles a faster game clock), fast_forward_speed */
int g_opt_volume=100;      /* [Audio] volume: master output level in percent (audio.c), live */
float g_opt_resf=1.0f; int g_opt_shadows=1; int g_opt_cmp_low=1; int g_opt_resdyn=0; int g_opt_prof=0; float g_opt_resmin=0.6f;
static char g_hints_path[1200];

/* ============================================================ exports for the generated code */
static PMutex g_hint_lock; static u64* g_hints; static int g_nhints, g_caphints, g_hints_dirty;

static void rt_note_hint(u64 pc){
  pm_lock(&g_hint_lock);
  for(int i=g_nhints-1;i>=0 && i>=g_nhints-64;i--) if(g_hints[i]==pc){ pm_unlock(&g_hint_lock); return; }
  int found=0; for(int i=0;i<g_nhints;i++) if(g_hints[i]==pc){ found=1; break; }
  if(!found){ if(g_nhints==g_caphints){ g_caphints=g_caphints?g_caphints*2:256; g_hints=realloc(g_hints,8*g_caphints); } g_hints[g_nhints++]=pc; g_hints_dirty=1;
    LOGV("interpreter: entering %llx (not recompiled)\n",(unsigned long long)pc); }
  pm_unlock(&g_hint_lock); }

void save_hints(void){
  if(!g_hints_dirty) return; FILE* f=fopen(g_hints_path,"w"); if(!f) return;
  pm_lock(&g_hint_lock); for(int i=0;i<g_nhints;i++) fprintf(f,"%llx\n",(unsigned long long)g_hints[i]); g_hints_dirty=0; pm_unlock(&g_hint_lock); fclose(f); }

static void load_hints(void){ FILE* f=fopen(g_hints_path,"r"); if(!f) return; char ln[64]; while(fgets(ln,sizeof ln,f)){ u64 v=strtoull(ln,NULL,16); if(v){ if(g_nhints==g_caphints){ g_caphints=g_caphints?g_caphints*2:256; g_hints=realloc(g_hints,8*g_caphints);} g_hints[g_nhints++]=v; } } fclose(f); }

void kernel_dump_regs(CPU* c);

static void rt_undef(CPU* c, u64 pc, u32 w){
  WARN("\nUnsupported instruction %08x at %llx\n",w,(unsigned long long)pc); c->pc=pc; kernel_dump_regs(c);
  save_hints(); fatal("Unsupported instruction %08x at %llx",w,(unsigned long long)pc); }

static void rt_brk(CPU* c, u64 pc, u32 imm){ WARN("\nBRK #%u at %llx\n",imm,(unsigned long long)pc); c->pc=pc; kernel_dump_regs(c); save_hints(); fatal("The game executed BRK at %llx",(unsigned long long)pc); }
static u64 g_t0;

u64 g_t0_ns(void){ return g_t0; }
/* guest clock. Normally == real time. In smooth recording mode (F7) it runs slower than real time so that
   every frame spans ~1/recording_fps of guest time, which gives a steady video even when the PC can't keep up.
   It never stops, so guest timeouts still fire. vsync follows it too. */
volatile int g_vt_on, g_rec_active; int g_opt_rec_smooth=1, g_opt_rec_fps=60;
static PMutex g_vt_m; static int g_vt_mi;
static u64 g_vt_ar, g_vt_av, g_vt_lastfr; static double g_vt_s=1.0, g_vt_fr=0;   /* anchor + rate */
static volatile u64 g_vt_max;

static u64 guest_ns_raw(u64 r){ return g_vt_av+(u64)((double)(r-g_vt_ar)*g_vt_s); }

/* Fast forward: while active the guest clock runs g_ff_s times faster than real time (elevator and other
   rides are fixed-length animations driven by the game's measured delta time). Kept as an accumulated
   offset so the clock never steps back when it is switched on or off. Unused until switched on. */
static volatile int g_ff_used; static double g_ff_s=1.0; static u64 g_ff_r0, g_ff_off; static PMutex g_ff_m;
static u64 ff_off(u64 r){ return g_ff_s>1.0? g_ff_off+(u64)((double)(r-g_ff_r0)*(g_ff_s-1.0)) : g_ff_off; }
static void ff_set(double s){ static int mi; if(!mi){ pm_init(&g_ff_m); mi=1; } u64 r=plat_ns()-g_t0;
  pm_lock(&g_ff_m); g_ff_off=ff_off(r); g_ff_r0=r; g_ff_s=s; g_ff_used=1; pm_unlock(&g_ff_m); }
volatile int g_ff_presses;   /* Tab presses (backend) */
static void ff_poll(void){ static double cur=1.0; static int on, seen, pad_prev; double want=1.0;
  extern int g_opt_ff, g_opt_ff_speed;
  if(g_opt_ff){   /* Tab, or both stick clicks (L3+R3) on a controller, toggles it */
    int pad= (g_pad.buttons&0x30)==0x30; if(pad && !pad_prev) on=!on; pad_prev=pad;
    if(g_ff_presses!=seen){ seen=g_ff_presses; on=!on; } }
  else on=0;
  if(on) want= g_opt_ff_speed>=2? (double)g_opt_ff_speed : 3.0;
  const char* e=getenv("SWR_FF"); if(e && *e && atof(e)>1.0) want=atof(e);   /* test hook (SWR_TEST_SETENV) */
  if(want!=cur){ cur=want; ff_set(want); LOG("fast forward: game clock x%.1f\n",want);
    { extern void backend_ff_status(int,int); backend_ff_status(want>1.0,(int)want); } } }

u64 guest_ns(void){
  u64 r=plat_ns()-g_t0, v;
  if(!g_vt_mi){ v=r; if(g_ff_used){ pm_lock(&g_ff_m); v=r+ff_off(r); pm_unlock(&g_ff_m); } } else { pm_lock(&g_vt_m); v=guest_ns_raw(r); pm_unlock(&g_vt_m); }
  u64 m=__atomic_load_n(&g_vt_max,__ATOMIC_RELAXED);
  while(v>m){ if(__atomic_compare_exchange_n(&g_vt_max,&m,v,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) return v; }
  return m; }

static void vt_rebase(u64 r, double s){ g_vt_av=guest_ns_raw(r); g_vt_ar=r; g_vt_s=s; }

static void vt_frame(void){
  if(!g_vt_mi) return;
  u64 r=plat_ns()-g_t0; pm_lock(&g_vt_m);
  /* cost per frame = GPU thread busy time. don't use full frame time, the game's own waits
     would feed back and slow the clock further */
  { extern u64 gpu_busy_ns(void); u64 b=gpu_busy_ns(); static u64 lb;
    if(g_vt_lastfr){ double d=(double)(b-lb); if(d>2e9) d=2e9;
      /* attack fast, release slow */
      if(g_vt_fr<=0 || d>g_vt_fr) g_vt_fr=d; else g_vt_fr=g_vt_fr*0.92+d*0.08; } lb=b; }
  g_vt_lastfr=r;
  double s=1.0; if(g_vt_on && g_vt_fr>0){ s=(1e9/(double)g_opt_rec_fps)/(g_vt_fr*1.25); if(s>1) s=1; if(s<0.02) s=0.02; }
  { static double fs=-1; if(fs<0){ const char* e=getenv("SWR_VT_SCALE"); fs= e? atof(e) : 0; } if(g_vt_on && fs>0) s=fs; }   /* test hook: force the clock rate */
  vt_rebase(r,s); pm_unlock(&g_vt_m); }

void vt_set(int on){
  if(!g_vt_mi){ pm_init(&g_vt_m); u64 r=plat_ns()-g_t0; g_vt_ar=r; g_vt_av=r; g_vt_s=1.0; g_vt_mi=1; }
  pm_lock(&g_vt_m); vt_rebase(plat_ns()-g_t0,1.0); g_vt_fr=0; g_vt_lastfr=0; pm_unlock(&g_vt_m);
  g_vt_on= on && g_opt_rec_smooth; }

/* vsync ticks on guest time */
static void vsync_tick_vt(void){
  static u64 next; if(!g_vt_on){ next=0; vi_vsync_tick(); return; }
  u64 v=guest_ns(); if(!next) next=v;
  if(v>=next){ vi_vsync_tick(); next+=16666667ull; if(v>next+50000000ull) next=v; } }

u64 g_t0_ticks;   /* tick-domain counterpart of g_t0, taken right after it */
/* Hottest callee (every guest CNTVCT read and svcGetSystemTick). Normal play: one QPC read and one
   multiply-shift, instead of guest_ns() (double divide + a CAS on a global every thread contends on,
   which the game's busy-wait frame limiter hammers) and a second double multiply. Same epoch, same
   19.2 MHz rate, same truncation. The F7 smooth-recording clock (g_vt_mi) keeps the old path. */
static u64 rt_ticks(void){
  u64 value= LIKELY(!g_vt_mi && !g_ff_used)? plat_ticks_raw()-g_t0_ticks : (u64)((double)guest_ns()*0.0192);
  /* Observation only: layout resolved from F_8085CB8 in this game's C.
     No clock changes or sleeps; validate mapped storage before reading. */
  {static int checked;static FILE*f;static int frame=-1;static u64 polls;
   if(!checked){checked=1;const char*p=getenv("SWR_FRAME_CLOCK_CSV");if(p&&*p){f=fopen(p,"w");if(f)fprintf(f,"ns,frame,object,previous_polls,previous_us,current_us,elapsed_us,minimum_ms,target_fps,clamp_dt,output_dt\n");}}
   if(f){KThread*t=kthread_cur();if(t&&t->is_main&&t->cpu.x[30]==0x8085d70ull){
     if(frame!=g_frames){u64 obj=t->cpu.x[19];MemBlock b;mem_query(obj,&b);
       if((b.perm&PERM_R)&&obj>=b.addr&&b.size>=76&&obj-b.addr<=b.size-76){
         i64 prev,cur,elapsed,minimum;float fps,clamp,dt;
         memcpy(&prev,GP(obj+16),8);memcpy(&cur,GP(obj+24),8);memcpy(&elapsed,GP(obj+32),8);memcpy(&minimum,GP(obj+56),8);
         memcpy(&fps,GP(obj+64),4);memcpy(&clamp,GP(obj+68),4);memcpy(&dt,GP(obj+48),4);
         fprintf(f,"%llu,%d,%llx,%llu,%lld,%lld,%lld,%lld,%.9g,%.9g,%.9g\n",(unsigned long long)plat_ns(),g_frames,(unsigned long long)obj,(unsigned long long)polls,(long long)prev,(long long)cur,(long long)elapsed,(long long)minimum,fps,clamp,dt);
         if(g_frames%60==0)fflush(f);
       }frame=g_frames;polls=0;
     }polls++;
   }}
  }

  /* Inert diagnostic: observe actual SDK tick callers on the guest main thread.
     Return the same unscaled timestamp; never alter game timing. */
  static int checked;static FILE*trace;
  if(!checked){checked=1;const char*path=getenv("SWR_TRACE_MAIN_TICKS");if(path&&*path){trace=fopen(path,"w");if(trace)fprintf(trace,"guest_frame,pc,lr,ticks,host_ns,x19,x20,x21,x22\n");}}
  if(trace){KThread*t=kthread_cur();if(t&&t->is_main){
    static u64 last_lr[128];static int last_frame[128],used,rows;u64 lr=t->cpu.x[30];int slot=0;
    for(;slot<used;slot++)if(last_lr[slot]==lr)break;
    if(slot==used&&used<128){last_lr[used]=lr;last_frame[used++]=-1;}
    if(slot<128&&last_frame[slot]!=g_frames){last_frame[slot]=g_frames;
      fprintf(trace,"%d,%llx,%llx,%llu,%llu,%llx,%llx,%llx,%llx\n",g_frames,(unsigned long long)t->cpu.pc,(unsigned long long)lr,(unsigned long long)value,(unsigned long long)plat_ns(),(unsigned long long)t->cpu.x[19],(unsigned long long)t->cpu.x[20],(unsigned long long)t->cpu.x[21],(unsigned long long)t->cpu.x[22]);if((++rows&255)==0)fflush(trace);
    }
  }}return value;
}

void swr_fpcr_apply(CPU* c){
#if defined(__aarch64__)
  /* arm64 host: just load the guest FPCR (FZ, rounding, DN, AHP) */
  u64 v=(u64)(c->fpcr&0x07C00000u); __asm__ __volatile__("msr fpcr, %0"::"r"(v));
#elif defined(__x86_64__) || defined(_M_X64)
  u32 mx; __asm__ __volatile__("stmxcsr %0":"=m"(mx));
  mx&=~(0x8040u|0x6000u);
  if(c->fpcr&(1u<<24)) mx|=0x8040;                        /* FZ -> FTZ+DAZ */
  static const u32 rm[4]={0,0x4000,0x2000,0x6000}; mx|=rm[(c->fpcr>>22)&3];
  __asm__ __volatile__("ldmxcsr %0"::"m"(mx));
#else
  (void)c;
#endif
}

static void rt_fpcr(CPU* c){ swr_fpcr_apply(c); }

static void rt_sev(void){}

static void rt_wfe(CPU* c){ (void)c; plat_yield(); }
const RtAPI g_rtapi={swr_svc,rt_undef,rt_note_hint,rt_ticks,rt_brk,rt_fpcr,rt_sev,rt_wfe};

/* ============================================================ system errors */
void crash_report(void* host_pc, void* fault, int sig){
  KThread* t=kthread_cur();
  u64 ga=(u64)((u8*)fault-g_mem);
  logf_(0,"\n*** Crash (signal %d) at host pc=%p, access to %p (game address %llx) ***\n",sig,host_pc,fault,(unsigned long long)ga);
  { extern void mem_report(char*,int); char mr[200]; mem_report(mr,sizeof mr); logf_(0,"  memory: %s\n",mr);
#ifdef _WIN32
    { extern char __ImageBase; u64 b=(u64)(size_t)&__ImageBase, p=(u64)(size_t)host_pc; if(p>=b && p<b+0x10000000ull) logf_(0,"  in the runtime itself (exe offset +%llx), thread %s\n",(unsigned long long)(p-b), kthread_cur()? "of the game" : "of the runtime (GPU, audio or window)");
      /* elsewhere: which DLL (a graphics driver's, a system one, the game code's) */
      else { extern int plat_module_of(const void*, char*, int, u64*); char mn[260]; u64 mo=0;
        if(plat_module_of(host_pc,mn,sizeof mn,&mo)) logf_(0,"  in %s (+%llx), a thread %s\n",mn,(unsigned long long)mo, kthread_cur()? "of the game" : "of the runtime (GPU, audio, video or window): a graphics driver's DLL here points at the driver");
        else logf_(0,"  in no loaded module, a thread %s\n", kthread_cur()? "of the game" : "of the runtime"); } }
#endif
    if(ga && ga<0x1000000ull) logf_(0,"  the address is a null pointer plus an offset: the game probably got NULL from its allocator (out of memory?). Try memory_gb=6 in the .ini\n"); }
  if(g_mod){ /* closest recompiled fn below host pc */
    u64 best=0, ga2=0; for(u32 i=0;i<g_mod->nentries;i++){ u64 f=(u64)(size_t)g_mod->entry_fn[i]; if(f<=(u64)(size_t)host_pc && f>best){ best=f; ga2=g_mod->entry_addr[i]; } }
    if(best && (u64)(size_t)host_pc-best<0x100000) logf_(0,"  inside the recompiled code of game function %llx (+%llx on the host)\n",(unsigned long long)ga2,(unsigned long long)((u64)(size_t)host_pc-best)); }
  if(t){ logf_(0,"  thread %llu (%s)\n",(unsigned long long)t->id,t->name); kernel_dump_regs(&t->cpu); }
  save_hints();
  fatal("The game crashed (invalid memory access). See the .log.");
}

/* ============================================================ options */
/* legacy (spanish) key names -> current ones */
static const char* const INI_ALIAS[][2]={
  {"modo_tv","docked"},{"idioma","language"},{"resolucion","resolution"},{"resolucion_dinamica","dynamic_resolution"},
  {"resolucion_minima","dynamic_resolution_min"},{"resolucion_dinamica_del_juego","game_dynamic_resolution"},{"sombras","shadows"},
  {"perfil","profile"},{"compilador_prioridad_baja","low_priority_shader_compiler"},{"vsync_forzado","forced_vsync"},
  {"grabacion_fluida","smooth_recording"},{"grabacion_fps","recording_fps"},{"lsfg_objetivo","lsfg_target"},
  {"lsfg_multiplicador","lsfg_multiplier"},{"lsfg_flujo","lsfg_flow"},{"lsfg_resolucion","lsfg_resolution"},
  {"lossless_ruta","lossless_path"},{"controles","touch_controls"},{"controles_opacidad","touch_controls_opacity"},
  {"mouse_aim","mouse_look"},{NULL,NULL}};   /* mouse_aim: the Metroid Dread mouse free aim that mouse look replaced */

static const char* ini_norm(const char* k){ for(int i=0;INI_ALIAS[i][0];i++) if(!strcmp(k,INI_ALIAS[i][0])) return INI_ALIAS[i][1]; return k; }
/* everything we read, so the rewrite keeps unknown keys */
#define INI_MAXK 256
static char g_ini_k[INI_MAXK][64], g_ini_v[INI_MAXK][1024]; static int g_ini_n;

static void ini_store(const char* k, const char* v){ for(int i=0;i<g_ini_n;i++) if(!strcmp(g_ini_k[i],k)){ strncpy(g_ini_v[i],v,1023); return; }
  if(g_ini_n<INI_MAXK){ strncpy(g_ini_k[g_ini_n],k,63); strncpy(g_ini_v[g_ini_n],v,1023); g_ini_n++; } }

static const char* ini_get(const char* k, const char* def){ for(int i=0;i<g_ini_n;i++) if(!strcmp(g_ini_k[i],k)) return g_ini_v[i]; return def; }

static void env_switch(const char* name, int on);
static void ini_apply(const char* k, const char* v){
    if(!strcmp(k,"docked")) g_opt_docked=atoi(v);
    else if(!strcmp(k,"controller_map")){ snprintf(g_opt_pad_map,sizeof g_opt_pad_map,"%s",v); }
    else if(!strcmp(k,"mouse_look")) g_opt_mouse_look=atoi(v)!=0;
    else if(!strcmp(k,"mouse_sensitivity")){ g_opt_mouse_sens=atoi(v); if(g_opt_mouse_sens<10) g_opt_mouse_sens=10; if(g_opt_mouse_sens>1000) g_opt_mouse_sens=1000; }
    else if(!strcmp(k,"mouse_invert_y")) g_opt_mouse_invert_y=atoi(v)!=0;
    else if(!strcmp(k,"mouse_deadzone")){ g_opt_mouse_deadzone=atoi(v); if(g_opt_mouse_deadzone<0) g_opt_mouse_deadzone=0; if(g_opt_mouse_deadzone>50) g_opt_mouse_deadzone=50; }
    else if(!strcmp(k,"mouse_buttons")){
      /* the Metroid Dread layout (left Y, right L for its free aim) means nothing in a first-person game: the current default */
      if(!strcmp(v,"y,l,r,-,-")){ v="a,zl,r,-,-"; ini_store(k,v); }
      snprintf(g_opt_mouse_buttons,sizeof g_opt_mouse_buttons,"%s",v); extern void mouse_map_reset(void); mouse_map_reset(); }
    else if(!strncmp(k,"key_",4) && kbm_binding_set(k,v)) {}   /* input_kbm.c keyboard bindings, live */
    else if(!strcmp(k,"button_icons")) snprintf(g_opt_button_icons,sizeof g_opt_button_icons,"%s",v);
    else if(!strcmp(k,"controller_api")) g_opt_ctrl_api= !strcmp(v,"sdl")? 1 : !strcmp(v,"xinput")? 2 : 0;
    else if(!strcmp(k,"controller_index")){ g_opt_ctrl_index=atoi(v); if(g_opt_ctrl_index<0) g_opt_ctrl_index=0; }
    else if(!strcmp(k,"rumble")){ extern int g_opt_rumble; g_opt_rumble=atoi(v); if(g_opt_rumble<0) g_opt_rumble=0; if(g_opt_rumble>200) g_opt_rumble=200; }   /* svc_hid.c */
    else if(!strcmp(k,"stick_deadzone")){ g_opt_deadzone=atoi(v); if(g_opt_deadzone<0) g_opt_deadzone=0; if(g_opt_deadzone>30000) g_opt_deadzone=30000; }
    else if(!strcmp(k,"language")) g_opt_lang=atoi(v);
    else if(!strcmp(k,"log")) g_verbose=atoi(v);
    else if(!strcmp(k,"diag")) g_opt_diag=atoi(v);
    else if(!strcmp(k,"smooth_recording")) g_opt_rec_smooth=atoi(v);
    else if(!strcmp(k,"recording_fps")){ g_opt_rec_fps=atoi(v); if(g_opt_rec_fps<15) g_opt_rec_fps=15; if(g_opt_rec_fps>60) g_opt_rec_fps=60; }
    else if(!strcmp(k,"fps_limit")) g_opt_fps=atoi(v);
    else if(!strcmp(k,"video_async_retire")) g_opt_video_async_retire=atoi(v)!=0;
    else if(!strcmp(k,"video_avx2")) g_opt_video_avx2=atoi(v)!=0;
    else if(!strcmp(k,"video_block_copy")) g_opt_video_block_copy=atoi(v)!=0;
    else if(!strcmp(k,"audio_eos_sample_count")) {}   /* always on since v0.3.1: old .ini values (often 0) are ignored */
    else if(!strcmp(k,"audio_surround_downmix")) g_opt_audio_surround_downmix=atoi(v)!=0;
    else if(!strcmp(k,"resolution")){ g_opt_resf=(float)atof(v); if(g_opt_resf<0.25f) g_opt_resf=0.25f; if(g_opt_resf>4) g_opt_resf=4; }
    else if(!strcmp(k,"shadows")) g_opt_shadows=atoi(v);
    else if(!strcmp(k,"profile")) g_opt_prof=atoi(v);
    else if(!strcmp(k,"low_priority_shader_compiler")) g_opt_cmp_low=atoi(v);
#if !defined(_WIN32) && !defined(__ANDROID__)
    else if(!strcmp(k,"shader_threads")){ extern int g_opt_shader_threads; g_opt_shader_threads=atoi(v); if(g_opt_shader_threads<0) g_opt_shader_threads=0; if(g_opt_shader_threads>8) g_opt_shader_threads=8; }   /* backend_x11.c */
#endif
    else if(!strcmp(k,"dynamic_resolution")) g_opt_resdyn=atoi(v);
    else if(!strcmp(k,"dynamic_resolution_min")){ g_opt_resmin=(float)atof(v); if(g_opt_resmin<0.25f) g_opt_resmin=0.25f; if(g_opt_resmin>1) g_opt_resmin=1; }
    else if(!strcmp(k,"forced_vsync")) g_opt_vsync=atoi(v);
    else if(!strcmp(k,"game_dynamic_resolution")) g_opt_drs=atoi(v);   /* 1 = let the game's own DRS work */
    else if(!strcmp(k,"memory_gb")){ extern int g_opt_mem_gb; g_opt_mem_gb=atoi(v); }
    else if(!strcmp(k,"serialize_media_threads")) g_opt_media_turns=atoi(v);
    else if(!strcmp(k,"sleep_waits")) g_opt_sleep_waits=atoi(v);
    else if(!strcmp(k,"hdr")) g_opt_hdr=atoi(v);   /* native Vulkan HDR output (snxvk hdr.c) */
    else if(!strcmp(k,"hdr_paper_white")){ g_opt_hdr_paper=atoi(v); if(g_opt_hdr_paper<80) g_opt_hdr_paper=80; if(g_opt_hdr_paper>1000) g_opt_hdr_paper=1000; }
    else if(!strcmp(k,"hdr_peak")){ g_opt_hdr_peak=atoi(v); if(g_opt_hdr_peak<0) g_opt_hdr_peak=0; if(g_opt_hdr_peak>10000) g_opt_hdr_peak=10000; }
    else if(!strcmp(k,"prebuild_shaders")){ extern int g_opt_prebuild; g_opt_prebuild=atoi(v); }   /* gpu_gl.c prebuild_shaders */
    else if(!strcmp(k,"upscaler")){ extern int g_opt_upscaler; g_opt_upscaler=!strcmp(v,"fsr"); }   /* gpu_gl.c fsr_draw, live */
    else if(!strcmp(k,"fsr_sharpness")){ extern int g_opt_fsr_sharp; g_opt_fsr_sharp=atoi(v); if(g_opt_fsr_sharp<0) g_opt_fsr_sharp=0; if(g_opt_fsr_sharp>100) g_opt_fsr_sharp=100; }
    else if(!strcmp(k,"antialiasing")){ extern int g_opt_aa; g_opt_aa=atoi(v)!=0; }
    else if(!strcmp(k,"sharpening")){ extern int g_opt_sharp; g_opt_sharp=atoi(v); if(g_opt_sharp<0) g_opt_sharp=0; if(g_opt_sharp>100) g_opt_sharp=100; }   /* gpu_gl.c present pass, snxvk hdr.c */   /* FXAA: gpu_gl.c present pass, snxvk hdr.c */
    else if(!strcmp(k,"show_fps")) g_opt_show_fps=atoi(v)!=0;   /* FPS counter, live */
    else if(!strcmp(k,"cheats")){ extern int g_opt_cheats; g_opt_cheats=atoi(v)!=0; }   /* cheats.c, live */
    else if(!strcmp(k,"fov")){ extern int g_opt_fov; g_opt_fov=atoi(v); if(g_opt_fov<60) g_opt_fov=60; if(g_opt_fov>160) g_opt_fov=160; }   /* cheats.c FOV hook, live */
    else if(!strcmp(k,"anisotropy")){ g_opt_aniso=atoi(v); if(g_opt_aniso<0) g_opt_aniso=0; if(g_opt_aniso>16) g_opt_aniso=16; }
    else if(!strncmp(k,"vk_",3)){ extern int snxvk_tuning_set(const char*,int); snxvk_tuning_set(k,atoi(v)); }   /* native Vulkan tunings (snxvk_glue.c) */
    else if(!strcmp(k,"dlss")){ extern int snxvk_dlss_mode; snxvk_dlss_mode=atoi(v); }   /* native Vulkan: snxvk dlss.c; OpenGL: dlss_gl.c + gpu_gl.c dlg_* */
    else if(!strcmp(k,"fsr_frame_generation")){ extern int snxvk_fsr_fg; snxvk_fsr_fg=atoi(v); }   /* native Vulkan: snxvk fsr_fg.c */
    else if(!strcmp(k,"texture_threads")){ g_opt_tex_threads=atoi(v); if(g_opt_tex_threads<0) g_opt_tex_threads=0; if(g_opt_tex_threads>16) g_opt_tex_threads=16; }
    else if(!strcmp(k,"fast_forward")) g_opt_ff=atoi(v);
    else if(!strcmp(k,"fast_forward_speed")){ g_opt_ff_speed=atoi(v); if(g_opt_ff_speed<2) g_opt_ff_speed=2; if(g_opt_ff_speed>8) g_opt_ff_speed=8; }
    else if(!strcmp(k,"volume")){ g_opt_volume=atoi(v); if(g_opt_volume<0) g_opt_volume=0; if(g_opt_volume>200) g_opt_volume=200; }
    else if(!strcmp(k,"heap_max_mb")){ extern u64 g_heap_max; g_heap_max=(u64)atoi(v)<<20; }
    { extern void lsfg_ini(const char*, const char*); lsfg_ini(k,v); }
    backend_ini(k,v); }

void ini_load(const char* path){
  FILE* f=fopen(path,"r"); if(!f) return; char ln[1200];
  while(fgets(ln,sizeof ln,f)){ char* eq=strchr(ln,'='); if(!eq||ln[0]==';'||ln[0]=='#'||ln[0]=='[') continue; *eq=0; char* v=eq+1; char* nl=strpbrk(v,"\r\n"); if(nl) *nl=0;
    char* k=ln; while(*k==' '||*k=='\t') k++; char* ke=k+strlen(k); while(ke>k && (ke[-1]==' '||ke[-1]=='\t')) *--ke=0; while(*v==' '||*v=='\t') v++;
    { char* ve=v+strlen(v); while(ve>v && (ve[-1]==' '||ve[-1]=='\t')) *--ve=0; }
    if(!strcmp(k,"vsync")) continue;   /* removed option */
    const char* kn=ini_norm(k); ini_store(kn,v); ini_apply(kn,v); }
  fclose(f); }
/* rewrite the ini on every start: sorted, all keys, with help text */
typedef struct { const char* key; const char* def; const char* help; } IniOpt;
typedef struct { const char* title; const IniOpt* o; } IniSec;
static const IniOpt INI_DISPLAY[]={
  {"docked","0","1 = TV mode (1920x1080 / 1600x900 games), 0 = handheld mode (1280x720)"},
  {"fullscreen","1","1 = start in fullscreen (F11 or Alt+Enter toggles it in game)"},
  {"language","1","Game language: 0 Japanese, 1 English (US), 2 French, 3 German, 4 Italian, 5 Spanish,\n;   6 Chinese, 7 Korean, 8 Dutch, 10 Russian, 11 Taiwanese, 12 English (UK),\n;   13 French (Canada), 14 Spanish (Latin America)"},
  {NULL,NULL,NULL}};
static const IniOpt INI_GRAPHICS[]={
  {"resolution","1","Internal 3D resolution: 1 = native, 2 = double, 0.75 / 0.5 = faster (applies live)"},
  {"dynamic_resolution","0","1 = lower the resolution automatically when the GPU can't keep up (down to dynamic_resolution_min)"},
  {"dynamic_resolution_min","0.6","Lowest internal resolution allowed by dynamic_resolution"},
  {"game_dynamic_resolution","0","1 = let the game lower its own resolution like on the console (0 = always full resolution)"},
  {"show_fps","0","1 = FPS counter in the top-left corner (frames that reach the screen)"},
  {"upscaler","bilinear","How a picture smaller than the window is enlarged: bilinear, or fsr = AMD FidelityFX FSR 1\n;   (edge-adaptive upscaling + sharpening; OpenGL renderer). Pair it with resolution below 1, e.g. 0.67"},
  {"fsr_sharpness","80","FSR 1 sharpening (RCAS), 0-100"},
  {"antialiasing","0","1 = smooth jagged edges (FXAA on the final picture; OpenGL and native Vulkan). Not needed at\n;   resolution above 1 (the downscale already averages) or with DLAA"},
  {"sharpening","0","0 = off, 1..100 = sharpen the final picture (contrast-adaptive, like AMD CAS; OpenGL and native Vulkan)"},
  {"anisotropy","0","Texture filtering at an angle: 0 = as the game asks, 2/4/8/16 = at least that much\n;   (sharper floors and walls seen at an angle; samplers are made at first use, so a change applies on the next start)"},
  {"hdr","0","1 = HDR output (renderer=vulkan_native, Windows HDR on): the game's picture on an HDR swapchain, highlights\n;   expanded toward the display's peak (like Windows Auto HDR). Falls back to SDR when the display offers no HDR"},
  {"hdr_paper_white","200","HDR output: brightness of normal (SDR white) content in nits (80-1000)"},
  {"hdr_peak","0","HDR output: brightest highlight in nits; 0 = what the display reports"},
  {"prebuild_shaders","1","1 = build every shader before the game draws with it: the shaders it ships in romfs/system/shd, and the ones\n;   it unpacks into GPU memory (Metroid Prime 4 at boot), compiled in the background and kept ready. 0 = at first use"},
  {"shadows","1","0 = disable shadow maps (more FPS)"},
  {"vk_fast_writeback","1","native Vulkan: 1 = a shader's storage image lands in guest memory a block at a time where the game left\n;   that memory alone (the per-byte merge held the frame back); 0 = byte by byte, as ScarletNX does"},
  {"vk_async_pipelines","0","native Vulkan: 1 = a draw whose shader isn't built yet is skipped while it builds, instead of the game\n;   waiting for it: no hitch where a new effect first appears, but it shows up a few frames late. 0 = wait"},
  {"vk_memory_traps","1","native Vulkan: 1 = what the GPU's shaders write to game memory stays on the GPU until the CPU\n;   touches it (its pages are trapped), instead of being copied back every frame. 0 = copied back"},
  {"vk_dynamic_offsets","1","native Vulkan: 1 = the busiest buffer bindings take their offsets when a draw binds them, so draws\n;   that only move within the same buffers reuse their descriptor sets instead of writing new ones. 0 = new sets"},
  {"renderer","opengl","opengl, or vulkan = the game still renders with OpenGL but Vulkan puts the frames on screen\n;   (falls back to OpenGL output if the GPU driver can't share images between the two; restart required),\n;   or vulkan_native = EXPERIMENTAL native Vulkan renderer from ScarletNX (GPL-3.0; falls back to OpenGL if it can't start)"},
  {"dlss","0","1 = NVIDIA DLAA anti-aliasing (renderer=opengl or vulkan_native, RTX GPU, nvngx_dlss.dll in the game folder):\n;   DLSS at the internal resolution with the game's own motion vectors; the HUD is untouched. 0 = off"},
  {"fsr_frame_generation","0","1 = AMD FSR 3.1 frame generation (renderer=vulkan_native, Windows; amd_fidelityfx_loader_vk.dll and\n;   amd_fidelityfx_framegeneration_vk.dll in the game folder): one generated frame between each pair the game renders. 0 = off"},
  {NULL,NULL,NULL}};
static const IniOpt INI_PERF[]={
  {"audio_surround_downmix","1","1 = mix all six audio channels into the stereo output, as the console does (0 = front left/right only: no center channel, so no dialogue)"},
  {"cheats","1","1 = run the cheat codes that are on (Atmosphere format, <game>_data/cheats/<build id>.txt; turn them\n;   on and off in the in-game menu, Cheats page)"},
  {"fov","100","Field of view in percent of the game's own (60-160, applies live). Needs a FOV hook for this game\n;   build (beyond/fov_hooks.txt)"},
  {"fps_limit","1","1 = 60 FPS (the speed of a Switch), otherwise the cap (60 to 240), 0 = unlimited. The game keeps real-time speed at any frame rate when its code was built with logic_rate.py; older builds run faster above 60. Values under 60 are treated as 60. F4 changes it in game"},
  {"video_async_retire","0","1 = retire obsolete video pictures on a bounded worker (experimental; restart required)"},
  {"video_avx2","1","1 = AVX2 video color conversion when the CPU has it (same result as the plain conversion, faster; restart required)"},
  {"video_block_copy","1","1 = video pictures written to tiled memory 64 bytes at a time (same result, faster; restart required)"},
#ifdef _WIN32
  {"forced_vsync","0","1 = always wait for vsync (can lower FPS a lot on slow PCs)"},
#else
  {"forced_vsync","1","1 = wait for vsync: frames reach the screen evenly (0 = no wait: can lower stutter-free FPS on slow PCs,\n;   but frames beat against the display's refresh and tear in fullscreen)"},
#endif
  {"memory_gb","0","Memory the game sees: 0 = automatic (3.2 GB like the console, more for games that need it,\n;   e.g. Tears of the Kingdom), 4 = console (3.2 GB), 6 or 8 = extended (like yuzu's extended memory layout)"},
  {"low_priority_shader_compiler","1","1 = compile shaders at low priority (less stutter on weak CPUs)"},
  {"shader_threads","0","Linux: threads that compile new shaders at the same time, 0 = automatic (a quarter of the CPU\n;   threads, 2 to 8), 1 = one like older versions (restart)"},
  {"fast_forward","0","1 = Tab (or both stick clicks, L3+R3) toggles fast forward: the game clock runs faster, e.g. to\n;   shorten elevator and train rides (their animations follow the clock). Turn it off again before playing"},
  {"fast_forward_speed","3","Fast forward speed, 2-8 times"},
  {"sleep_waits","1","1 = threads waiting on the OpenGL thread sleep instead of spinning (frees CPU on 2-4 core\n;   PCs and laptops); 0 = spin like older versions"},
  {"texture_threads","0","Threads that unpack texture loads: 0 = automatic (all cores), 1 = one thread like older versions"},
  {"serialize_media_threads","1","1 = video/media threads that share a core on the Switch take turns like on the console\n;   (guards against a rare freeze when a video stops); 0 = run them fully in parallel"},
  {NULL,NULL,NULL}};
static const IniOpt INI_FG[]={
#ifndef SWR_RELEASE
  {"lsfg","0","In-game frame generation (LSFG). Uses the Lossless.dll from YOUR Lossless Scaling\n;   (copy it into this folder or set lsfg_dll). Only generates frames when the game is below lsfg_target"},
  {"lsfg_target","60","Target FPS for frame generation"},
  {"lsfg_multiplier","2","2 = x2, 3 = x3, 4 = x4"},
  {"lsfg_flow","0.5","Flow scale: 1 = best quality, 0.5 = faster (recommended on integrated GPUs)"},
  {"lsfg_resolution","720","Resolution (height) used for frame generation"},
  {"lsfg_dll","","Path to Lossless.dll (empty = search automatically)"},
  {"lossless_scaling","0","1 = also launch the Lossless Scaling app with the game (not needed when using lsfg)"},
  {"lossless_path","","Path to LosslessScaling.exe (empty = search in Steam)"},
#endif
  {NULL,NULL,NULL}};
static const IniOpt INI_INPUT[]={
  {"controller_map","","16 comma-separated button destinations (0..15, -1 disabled); empty = default. Keyboard and sticks unchanged."},
  {"mouse_look","1","1 = the mouse turns the camera (the right stick) while the game window has focus. The cursor is\n;   held in the window and hidden; F1 (the in-game menu) or switching to another window frees it"},
  {"mouse_sensitivity","100","mouse look: how far the camera turns for the same mouse movement, in percent (10-1000). At 100%,\n;   one mouse count turns it 0.05 degrees in first person, with code built with mouse_aim.py"},
  {"mouse_invert_y","0","1 = moving the mouse up looks down"},
  {"mouse_deadzone","15","mouse look where the mouse moves the right stick (lock-on, morph ball, menus, code without\n;   mouse_aim.py): the smallest stick push any mouse movement makes, in percent (0-50). Raise it if slow\n;   mouse movement doesn't turn the camera, lower it if small movements turn it too far"},
  {"mouse_buttons","a,zl,r,-,-","mouse buttons left, right, middle, back, forward as Switch buttons: a b x y l r zl zr plus\n;   minus lstick rstick, - for none"},
  {"button_icons","auto","the game's button pictures: auto = from the controller connected at start (Xbox and Steam Deck:\n;   Xbox icons, DualShock 4: PS4, DualSense: PS5, Switch or none: the originals), or switch, xbox, ps4, ps5.\n;   Needs the icon packs in Metroid Prime 4 Beyond_data\\mods (the Play screen downloads them: Controls > Button icons)"},
  {"controller_api","auto","auto = SDL2 when SDL2.dll is next to the game (Bluetooth/USB Switch Pro, DualShock 4,\n;   DualSense, 8BitDo, Xbox...), else XInput. sdl or xinput to force one"},
  {"controller_index","0","0 = first connected controller, 1.. = that controller (in connection order) is player 1"},
  {"stick_deadzone","7000","stick dead zone (0..30000, out of 32767)"},
  {"rumble","100","controller rumble strength in percent (0 = off, up to 200): the game's HD rumble on the pad's two motors"},
  {NULL,NULL,NULL}};
static const IniOpt INI_AUDIO[]={
  {"volume","100","Master volume in percent (0-200, applies live)"},
  {NULL,NULL,NULL}};
/* front end (beyond_ui/fe.cpp) and in-game menu (beyond_ui/ov.cpp); the runtime itself only stores them */
static const IniOpt INI_LAUNCHER[]={
  {"wait_for_shaders","1","1 = wait for new shaders before drawing (no missing objects, a short stutter the first time);\n;   0 = compile them in the background (no stutter, objects can pop in). Applies on the next start"},
  {"skip_launcher","0","1 = start the game directly, without the Play screen"},
  {"menu_shortcut","home","Controller button that opens the in-game menu (F1 on the keyboard always works):\n;   home = Home / Guide / PS button, minus_plus = - and + together, l3_r3 = both stick clicks held 1 second"},
  {"launcher_nav","1","1 = navigate the Play screen and menus with a controller"},
  {"launcher_prompts","auto","Button prompts: auto, xbox, playstation, nintendo, positions"},
  {"launcher_bindings","1,0,6,7,10","Menu buttons by position (confirm, back, previous page, next page, play):\n;   0 right, 1 bottom, 2 top, 3 left face button, 6 L, 7 R, 10 +, 11 -"},
  {"migrated_from","","Where settings and saves were imported from on the first start (informational)"},
  {"setup_base","","The game folder used by the last setup (for Check game files and rebuild)"},
  {"setup_update","","The separate update folder used by the last setup, if any"},
  {NULL,NULL,NULL}};
static const IniOpt INI_REC[]={
  {"smooth_recording","1","1 = F7 recordings are always smooth: the game runs in slow motion while recording\n;   if the PC can't keep up, but the exported video plays at normal speed with no stutter"},
  {"recording_fps","60","FPS of the exported video (constant)"},
  {NULL,NULL,NULL}};
static const IniOpt INI_DBG[]={
  {"log","1","Log detail: 1 = normal, 2 = everything (slow)"},
  {"diag","0","1 = automatic diagnostic dumps (F5 always works)"},
  {"profile","0","1 = write profile.txt every 10 seconds (CPU profiler); with vulkan_native also the GPU's time per pass in the log"},
  {NULL,NULL,NULL}};
#ifndef SWR_RELEASE
static const IniSec INI_SECS[]={{"Display",INI_DISPLAY},{"Graphics",INI_GRAPHICS},{"Performance",INI_PERF},{"Frame generation",INI_FG},{"Input",INI_INPUT},{"Audio",INI_AUDIO},{"Launcher",INI_LAUNCHER},{"Recording",INI_REC},{"Debug",INI_DBG},{NULL,NULL}};
#else
static const IniSec INI_SECS[]={{"Display",INI_DISPLAY},{"Graphics",INI_GRAPHICS},{"Performance",INI_PERF},{"Input",INI_INPUT},{"Audio",INI_AUDIO},{"Launcher",INI_LAUNCHER},{"Recording",INI_REC},{"Debug",INI_DBG},{NULL,NULL}};
#endif
static char g_ini_path[1400]; static u64 g_ini_mtime;

static u64 file_mtime(const char* p){ struct stat st; if(stat(p,&st)) return 0; return (u64)st.st_mtime; }

static int ini_known(const char* k){ if(kbm_action_find(k)>=0) return 1; for(int s=0;INI_SECS[s].title;s++) for(const IniOpt* o=INI_SECS[s].o;o->key;o++) if(!strcmp(o->key,k)) return 1; return 0; }

void ini_write(void){
  if(!g_ini_path[0]) return;
  char tmp[1450]; snprintf(tmp,sizeof tmp,"%s.tmp",g_ini_path); FILE* f=fopen(tmp,"w"); if(!f) return;
  fprintf(f,"; =====================================================================\n");
  fprintf(f,";  Metroid Prime 4 Beyond - settings\n");
  fprintf(f,";  Edit a value and save: most settings apply live while the game runs\n");
  fprintf(f,";  (docked, language, fullscreen and frame generation apply on restart).\n");
  fprintf(f,";  Keys in game: F1 menu, F3 screenshot, F4 FPS limit, F5 trace,\n");
  fprintf(f,";  F7 record video, F8 full-res screenshot, F11 fullscreen.\n");
  fprintf(f,"; =====================================================================\n");
  for(int s=0;INI_SECS[s].title;s++){
    fprintf(f,"\n[%s]\n",INI_SECS[s].title);
    for(const IniOpt* o=INI_SECS[s].o;o->key;o++) fprintf(f,"; %s\n%s=%s\n",o->help,o->key,ini_get(o->key,o->def));
    if(INI_SECS[s].o==INI_INPUT){
      fprintf(f,"%s",KBM_INI_HELP);
      for(int i=0;i<kbm_action_count();i++){ char b[128]; kbm_binding_get(i,b,sizeof b); fprintf(f,"%s=%s\n",kbm_action_key(i),ini_get(kbm_action_key(i),b)); } } }
  int other=0; for(int i=0;i<g_ini_n;i++) if(!ini_known(g_ini_k[i])){ if(!other){ fprintf(f,"\n[Other]\n"); other=1; } fprintf(f,"%s=%s\n",g_ini_k[i],g_ini_v[i]); }
  fclose(f);
  remove(g_ini_path); if(rename(tmp,g_ini_path)) { remove(tmp); return; }
  g_ini_mtime=file_mtime(g_ini_path); }
/* in-game toggles (F4) get persisted */
volatile int g_ini_dirty;

void ini_set(const char* k, const char* v){ ini_store(k,v); g_ini_dirty=1; }

void ini_open(const char* path){ strncpy(g_ini_path,path,sizeof g_ini_path-1); ini_load(path);
  /* first start with the rebindable keyboard (no key_* lines yet): the mouse moves from the old default (left ZR) to
     the PrimeHack layout's left = A, as the keys do. Any other mouse_buttons value was chosen and stays */
  if(!ini_get("key_a",NULL) && !strcmp(ini_get("mouse_buttons",""),"zr,zl,r,-,-")){
    ini_store("mouse_buttons","a,zl,r,-,-"); ini_apply("mouse_buttons","a,zl,r,-,-"); LOG("settings: mouse_buttons moved to the new default a,zl,r,-,-\n"); }
  ini_write(); }

/* ini edited while running: reload and apply what we can live */
void ini_poll(void){
  if(!g_ini_path[0]) return; if(g_ini_dirty){ g_ini_dirty=0; ini_write(); return; } u64 m=file_mtime(g_ini_path); if(!m || m==g_ini_mtime) return;
  g_ini_mtime=m; g_ini_n=0; ini_load(g_ini_path); LOG("settings: %s reloaded\n",g_ini_path); ini_write(); }

/* settings changed by the in-game menu (GL thread) or the test hook: applied on the 60 Hz frontend loop like an ini
   edit, then written back. Launcher keys that the game DLL reads from the environment are switched here too. */
static PMutex g_setq_lock; static int g_setq_init; static char g_setq[32][2][256]; static int g_setq_n;
void settings_queue(const char* k, const char* v){
  if(!g_setq_init) return; pm_lock(&g_setq_lock);
  if(g_setq_n<32){ snprintf(g_setq[g_setq_n][0],256,"%s",k); snprintf(g_setq[g_setq_n][1],256,"%s",v); g_setq_n++; }
  pm_unlock(&g_setq_lock); }
static void env_switch(const char* name, int on){
#if defined(_WIN32)
  SetEnvironmentVariableA(name,on?"1":NULL); { char b[160]; snprintf(b,sizeof b,"%s=%s",name,on?"1":""); _putenv(b); }
#else
  if(on) setenv(name,"1",1); else unsetenv(name);
#endif
  }
const char* settings_get(const char* k, const char* def){ return ini_get(k,def); }
static void settings_drain(void){
  char q[32][2][256]; int n; pm_lock(&g_setq_lock); n=g_setq_n; memcpy(q,g_setq,sizeof q); g_setq_n=0; pm_unlock(&g_setq_lock);
  for(int i=0;i<n;i++){ const char* k=q[i][0]; const char* v=q[i][1];
    ini_store(k,v); ini_apply(k,v); g_ini_dirty=1; LOG("settings: %s=%s (in game)\n",k,v);
    if(!strcmp(k,"fullscreen")){ extern void backend_set_fullscreen(int); backend_set_fullscreen(atoi(v)!=0); } } }
/* test only: SWR_TEST_SETTING="FRAME:key=value;FRAME:key=value" queues those changes at those guest frames */
static void settings_test_hook(void){
  static const char* s; static int init; if(!init){ init=1; s=getenv("SWR_TEST_SETTING"); }
  while(s && *s){ char* e; long f=strtol(s,&e,10); if(e==s||*e!=':'){ s=NULL; break; } if(g_frames<f) break;
    const char* kv=e+1; const char* end=strchr(kv,';'); const char* eq=strchr(kv,'='); size_t n=end? (size_t)(end-kv) : strlen(kv);
    if(eq && eq<kv+n){ char k[128],v[256]; size_t kl=(size_t)(eq-kv); if(kl>127) kl=127; memcpy(k,kv,kl); k[kl]=0; size_t vl=n-kl-1; if(vl>255) vl=255; memcpy(v,eq+1,vl); v[vl]=0;
      LOG("test setting: frame %d %s=%s\n",g_frames,k,v); settings_queue(k,v); }
    s= end? end+1 : NULL; } }

/* ============================================================ present */
static u32* g_frame; static int g_fw, g_fh; static PMutex g_flock;
static u64 g_last_present;

void vi_convert_cpu(u64 a, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, u32* out){
  u32 bpp= (fmt==4||fmt==6||fmt==7)? 2 : 4;
  u8* lin=malloc((size_t)w*h*bpp);
  if(layout==3 || layout==2){
    extern void deswizzle(u8* dst, u32 dst_pitch, const u8* src, u32 w_bytes, u32 h, u32 bh_log2);
    deswizzle(lin,w*bpp,GP(a),w*bpp,h,bhl);
  } else { for(u32 y=0;y<h;y++) memcpy(lin+(size_t)y*w*bpp,GP(a+(u64)y*pitch),w*bpp); }
  if(bpp==4){ if(fmt==5){ for(size_t i=0;i<(size_t)w*h;i++){ u32 p=((u32*)lin)[i]; out[i]=(p&0xFF00FF00u)|((p>>16)&0xFF)|((p&0xFF)<<16); } } else memcpy(out,lin,(size_t)w*h*4); }
  else { u16* s=(u16*)lin; for(size_t i=0;i<(size_t)w*h;i++){ u32 p=s[i], r,g,b;
      if(fmt==4){ r=(p>>11)&31; g=(p>>5)&63; b=p&31; r=(r<<3)|(r>>2); g=(g<<2)|(g>>4); b=(b<<3)|(b>>2); }
      else if(fmt==6){ r=(p>>11)&31; g=(p>>6)&31; b=(p>>1)&31; r=(r<<3)|(r>>2); g=(g<<3)|(g>>2); b=(b<<3)|(b>>2); }
      else { r=((p>>12)&15)*17; g=((p>>8)&15)*17; b=((p>>4)&15)*17; }
      out[i]=r|(g<<8)|(b<<16)|0xFF000000u; } }
  free(lin);
}

/* the frame cap the pacers use */
volatile int g_pace_fps=1;
static void pace_update(void){ g_pace_fps=g_opt_fps; }

/* Game logic at any frame rate. The game steps its logic by flt_93C5400[mode] seconds (mode 0 = 1/60, mode 1 = 1/30,
   F_8E49AAC) once per presented frame, so above 60 fps it ran faster than real time. Code built with logic_rate.py
   exports swr_logic_k (sixtieths per step): every site that picks a value by the mode follows it (step, times,
   smoothing exponents, frame counts). Each frame k follows the measured frame time: exactly the cap while frames
   arrive on time (k = 1 at 60 fps, as on the Switch), the smoothed real time while they come late, and never more
   than 1/30 s, the game's own slowest step. SWR_LOGIC_HZ=N fixes k at 60/N (60..240), SWR_LOGIC_HZ=0 turns it off.
   An older code DLL only gets SWR_LOGIC_HZ=N, as 1/N in the step table (smoothing then runs N/60 times faster).
   Build 8275FBE1EC5191C2 only. */
static float* g_logic_k; static volatile u64* g_logic_unknown; static volatile u32* g_logic_unknown_n;
static void logic_hz_apply(void){
  static int state; /* 0 = first frame, 1 = off or fixed, 2 = following the frame time */
  static u32 said, snapped; static u64 last, frames, t0; static double ema, ksum, debt;
  if(g_logic_unknown_n) for(u32 n=*g_logic_unknown_n; said<n && said<32; said++)   /* sites with no 2:1 pair, once */
    LOG("game logic rate: site %llX keeps its 60 fps value (not a 2:1 pair)\n",(unsigned long long)g_logic_unknown[said]);
  if(state==1) return;
  if(state==0){ state=1;
    static const u8 bid[8]={0x82,0x75,0xFB,0xE1,0xEC,0x51,0x91,0xC2};
    const SwrModInfo* m=NULL;
    if(g_mod) for(u32 i=0;i<g_mod->nmods;i++) if(!strcmp(g_mod->mods[i].name,"main")) m=&g_mod->mods[i];
    int ours= m && m->base==0x8006000ull && !memcmp(m->build_id,bid,8);
    const char* s=getenv("SWR_LOGIC_HZ");
    if(s && *s){ int hz=atoi(s);
      if(hz==0){ LOG("game logic rate: off (SWR_LOGIC_HZ=0), one 1/60 s step per frame\n"); return; }
      if(hz<60 || hz>240){ WARN("SWR_LOGIC_HZ=%s ignored (60..240, or 0 for off)\n",s); return; }
      if(!ours){ WARN("SWR_LOGIC_HZ: not build 8275FBE1EC5191C2, ignored\n"); return; }
      if(g_logic_k){ *g_logic_k=60.0f/(float)hz; LOG("game logic rate: fixed at %d steps a second (k=%g)\n",hz,*g_logic_k); return; }
      float t[2]; memcpy(t,GP(0x93C5400ull),8);
      if(t[0]!=1.0f/60.0f || t[1]!=1.0f/30.0f){ WARN("SWR_LOGIC_HZ: step table holds %g, %g, not 1/60, 1/30: ignored\n",t[0],t[1]); return; }
      t[0]=1.0f/(float)hz; memcpy(GP(0x93C5400ull),t,4);
      LOG("game logic rate: step 1/%d s; this code DLL has no swr_logic_k, so smoothing stays at 1/60\n",hz); return; }
    if(!ours || !g_logic_k) return;   /* the step stays 1/60 s per frame */
    LOG("game logic rate: follows the frame time\n"); state=2; last=t0=plat_ns(); return; }
  /* the cap: fps_limit 1 = 60, N = N (60..240), 0 = none; recording keeps 60, smooth recording paces in guest time */
  int pf=g_pace_fps; double per= pf>=60 && pf<=240? 1.0/pf : (pf==0 && !g_rec_active)? 0 : 1.0/60;
  u64 now=plat_ns(); double dt=(now-last)*1e-9; last=now;
  if(dt>1.0/30) dt=1.0/30;   /* a hitch: the game slows down, as it does under 30 fps on the Switch */
  ema= ema>0? ema+(dt-ema)*0.25 : dt;
  int on_cap= per>0 && (g_vt_on || (ema>per*0.97 && ema<per*1.03)); double step= on_cap? per : ema; snapped+=on_cap;
  /* debt = real time the steps have not covered yet. The average lags the pacer's catch-up frames after a slow one,
     so past 2 ms a tenth of the debt is paid back each frame; inside 2 ms the step stays exact (k = 1 at 60 fps) */
  if(!g_vt_on){ debt+=dt; if(debt>0.002 || debt<-0.002) step+=debt*0.1; if(step<0.5*ema) step=0.5*ema; }
  float k=(float)(step*60.0); if(k<0.1f) k=0.1f; if(k>2.0f) k=2.0f;
  if(!g_vt_on){ debt-=k/60.0; if(debt>0.1 || debt<-0.1) debt=0; }   /* a k clamp or a long stall: start over */
  *g_logic_k=k; ksum+=k;
  if(++frames%600==0){ /* every 600 frames: frame rate, frames at the cap, game time against real time */
    LOG("game logic rate: %.1f steps a second, %u of 600 at the cap, game time %.1f%% of real time\n",600.0*60.0/ksum,snapped,100.0*(ksum/60.0)/((now-t0)*1e-9));
    ksum=0; snapped=0; t0=now; } }

void vi_show_buffer(u32 nvmap_id, u32 offset, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt){
  pace_update(); logic_hz_apply();
  static int trace_configured;static FILE*limiter_trace;static unsigned trace_rows;
  if(!trace_configured){trace_configured=1;const char*path=getenv("SWR_TRACE_LIMITER");if(path&&path[0]){limiter_trace=fopen(path,"w");if(limiter_trace)fprintf(limiter_trace,"frame,thread,enter_ns,enqueued_ns,limit_begin_ns,end_ns,requested_sleep_ms,actual_sleep_ms,deadline_ns\n");}}
  u64 trace_enter=limiter_trace?plat_ns():0,trace_enqueued=0,trace_begin=0,trace_sleep=0,trace_actual=0,trace_deadline=0;
  extern u64 nvmap_addr_by_id(u32 id); extern int g_gl;
  u64 a=nvmap_addr_by_id(nvmap_id); if(!a){ WARN("vi: buffer without memory (nvmap %u)\n",nvmap_id); return; }
  a+=offset;
  { extern int g_snxvk_on; extern void snxvk_present(u64,u32,u32,u32,u32,u32,u32); if(g_snxvk_on) snxvk_present(a,w,h,pitch,layout,bhl,fmt);
  else if(g_gl){ extern void gpu_gl_present_buffer(u64,u32,u32,u32,u32,u32,u32); gpu_gl_present_buffer(a,w,h,pitch,layout,bhl,fmt); }
  else {
    pm_lock(&g_flock);
    if(g_fw!=(int)w || g_fh!=(int)h){ free(g_frame); g_frame=malloc((size_t)w*h*4); g_fw=(int)w; g_fh=(int)h; }
    vi_convert_cpu(a,w,h,pitch,layout,bhl,fmt,g_frame);
    video_present_rgba(g_frame,(int)w,(int)h,(int)w);
    pm_unlock(&g_flock);
  } }
  { static int once; if(!once){ once=1; if(getenv("SWR_VT")) vt_set(1); } }   /* test smooth clock w/o recording */
  if(limiter_trace)trace_enqueued=plat_ns();
  g_frames++; vt_frame();
  { /* SWR_VIDEO_TIMING=1: how evenly frames reach the screen, every 120 presents: average, worst and late (> 20 ms) */
    static int timing=-1; static u64 last, sum, mx; static u32 n, late; if(timing<0) timing=!getenv("SWR_NO_VIDEO_TIMING");
    if(timing){ u64 now=plat_ns(); if(last){ u64 dt=now-last; sum+=dt; if(dt>mx) mx=dt; if(dt>20000000ull) late++; n++;
        if(n==120){ LOG("present timing: %.2f ms average, %.2f ms worst, %u of 120 over 20 ms\n",sum/120e6,mx/1e6,late); sum=mx=0; n=late=0; } } last=now; } }
  /* fps_limit: 1 = 60, otherwise N fps */
  /* in smooth recording the limiter has to count guest time, not real time.
     games that tick once per frame would otherwise run sped up in the video */
  if(g_vt_on){ u64 per= g_opt_fps>=60 && g_opt_fps<=240? 1000000000ull/(u64)g_opt_fps : 16666667ull;
    static u64 last_g; u64 g=guest_ns(), next=last_g+per, t0=plat_ns();
    if(last_g && g<next && next-g<per*2){ while(g<next && plat_ns()-t0<1000000000ull){ plat_sleep_ns(500000); g=guest_ns(); } }
    g=guest_ns(); last_g= (last_g && g-last_g<2*per)? next : g; g_last_present=plat_ns(); return; }
  {extern int gpu_gl_present_pacing(void);extern void gpu_gl_frame_consumed(void);
    { extern int snxvk_pacing(void); if(snxvk_pacing()){ g_last_present=plat_ns(); goto limiter_finished; } }   /* native Vulkan paces before its present */
    if(g_gl && gpu_gl_present_pacing()){
      /* Guest RAM is no longer read for this frame before the next guest
         update starts. The existing GL queue bounds outstanding presents.
         (2026-10-23: skipping this wait measured no gain at 240: in heavy rooms the GL call thread is the limit) */
      gpu_gl_frame_consumed();g_last_present=plat_ns();goto limiter_finished;
    }
  }
  /* while recording keep at least the 60 cap, uncapped + per-frame logic = sped up video */
  if(g_opt_fps || g_rec_active){ int pf=g_pace_fps; u64 per= pf>=30 && pf<=240? 1000000000ull/(u64)pf : 16666667ull;
    if(g_opt_fps>=10 && g_opt_fps<60){ static int said; if(!said){ said=1; WARN("fps_limit=%d would run the game at %d%% speed (one game step per image): using 60\n",g_opt_fps,g_opt_fps*100/60); } }
    u64 now=plat_ns(); u64 next=g_last_present+per;
    if(limiter_trace){trace_begin=now;trace_deadline=next;}
    if(g_last_present && now<next){if(limiter_trace)trace_sleep=next-now;plat_sleep_ns((i64)(next-now));}
    now=plat_ns();if(limiter_trace&&trace_sleep)trace_actual=now-trace_begin;
    g_last_present= (g_last_present && now-g_last_present<2*per)? next : now; }
limiter_finished:
  if(limiter_trace){extern u64 kernel_current_thread_id(void);u64 end=plat_ns();
    fprintf(limiter_trace,"%d,%llu,%llu,%llu,%llu,%llu,%.6f,%.6f,%llu\n",g_frames,(unsigned long long)kernel_current_thread_id(),(unsigned long long)trace_enter,(unsigned long long)trace_enqueued,(unsigned long long)trace_begin,(unsigned long long)end,trace_sleep/1e6,trace_actual/1e6,(unsigned long long)trace_deadline);
    if((++trace_rows&255)==0)fflush(limiter_trace);
  }
}

/* ============================================================ code module */
typedef const SwrModule* (*ModFn)(void);
#if !defined(_WIN32)
__attribute__((weak)) const SwrModule* swr_module_static(void);
#else
const SwrModule* swr_module_static(void) __attribute__((weak));
#endif
int g_code_tcc;

static int load_code(void){
  if(swr_module_static){ g_mod=swr_module_static(); return g_mod? 0 : -1; }
  char p[1200];
#if defined(_WIN32)
  snprintf(p,sizeof p,"%s%s_code.dll",g_exedir,g_exename);
#elif defined(__ANDROID__)
  snprintf(p,sizeof p,"lib%s_code.so",g_exename);   /* lives in the apk's lib dir */
#else
  snprintf(p,sizeof p,"%s%s_code.so",g_exedir,g_exename);
#endif
  void* h=plat_dl_open(p);
  if(!h){
#if !defined(_WIN32)
    extern char* dlerror(void); WARN("dlopen: %s\n",dlerror());
#endif
    WARN("Cannot find the recompiled code: %s\n",p); return -1; }
  ModFn f=(ModFn)plat_dl_sym(h,"swr_module"); if(!f){ WARN("%s is not a SwitchRecomp module\n",p); return -1; }
#if defined(_WIN32)
  { /* tell gcc from tcc builds by the PE linker version (ld 2.x vs tcc 6.0). tcc code is a lot slower */
    const u8* b=(const u8*)h; u32 lf; memcpy(&lf,b+0x3C,4); u8 mj=b[lf+24+2], mn=b[lf+24+3];
    g_code_tcc= mj==6 && mn==0;
    if(g_code_tcc) LOG("code: %s_code.dll was compiled with TCC. It runs several times slower than with GCC: recompile with GCC (SwitchRecomp, compiler GCC) for much better performance\n",g_exename);
    else LOG("code: %s_code.dll compiled with GCC (linker %u.%u)\n",g_exename,mj,mn); }
#endif
  g_logic_k=(float*)plat_dl_sym(h,"swr_logic_k");   /* logic_rate.py builds only */
  g_logic_unknown=(volatile u64*)plat_dl_sym(h,"swr_logic_unknown"); g_logic_unknown_n=(volatile u32*)plat_dl_sym(h,"swr_logic_unknown_n");
  { KbmAim a={(volatile int*)plat_dl_sym(h,"swr_aim_pend"),(volatile float*)plat_dl_sym(h,"swr_aim_deg"),
      (volatile u32*)plat_dl_sym(h,"swr_aim_steps"),(volatile float*)plat_dl_sym(h,"swr_aim_mult")};
    kbm_aim_bind(&a);   /* mouse_aim.py builds only */
    LOG("code: mouse aim %s\n",a.pend? "built in (the mouse turns the camera directly in first person)" : "not built in (the mouse moves the right stick)"); }
  g_mod=f(); return 0; }

static void build_dispatch(void){
  g_code_lo=g_mod->code_lo; g_code_hi=g_mod->code_hi;
  u64 n=(g_code_hi-g_code_lo)/4;
  g_disp=calloc(n?n:1,sizeof(GuestFn));
  for(u32 i=0;i<g_mod->nentries;i++){ u64 a=g_mod->entry_addr[i]; if(a>=g_code_lo && a<g_code_hi) g_disp[(a-g_code_lo)/4]=g_mod->entry_fn[i]; }
  swr_interp_entry=g_mod->interp;
}

/* Diagnostic, opt-in. SWR_TRACE_HOOK=path plus either SWR_TRACE_HOOK_PTR=<guest
   address of a function-pointer global> (hooked once the game stores a pointer
   there) or SWR_TRACE_HOOK_PC=<pc>[,<pc>...] (hooked at startup). The dispatch
   entry is swapped for a wrapper that records the call -- entry pc, caller lr,
   x0..x3, six u32 at x2, four u32 at x0+SWR_TRACE_HOOK_HOFF (default 8) and the u32
   at the register named by SWR_TRACE_HOOK_W (1, 2 or 3; default 1), both read before the
   call), host time inside -- and then calls the original unchanged. Only CALLDYN
   calls are seen; direct calls from generated code bypass the dispatch table.
   Every hooked function must take pointer arguments where these reads happen. */
#define HOOKMAX 8
typedef struct { u64 t, ns, pc, x0, x1, x3, lr; i32 r[6]; u32 h[4], w1, tid; } HookRec;
static HookRec* g_hk; static u32 g_hk_cap; static volatile u32 g_hk_n; static volatile u64 g_hk_total, g_hk_lost;
static GuestFn g_hk_orig[HOOKMAX]; static u64 g_hk_pc[HOOKMAX]; static int g_hk_cnt; static i64 g_hk_hoff=8; static int g_hk_wreg=1;
/* SWR_TRACE_HOOK_TEXHASH=1 (with the nvnTextureWriteTexels hook): once uploads to a
   texture have been quiet for 200 ms, hash its backing store. Layout from the
   game's texture-create helper F_8165F60: object = x0-24, width @0, height @4,
   storage guest address @248; hashed size = width*height*4. Main thread only. */
static int g_hk_texhash; static u64 g_hk_tex[8]; static volatile u32 g_hk_texn[8]; static volatile u64 g_hk_last; static u64 g_hk_ptr; static const char* g_hk_path; static u32 g_hk_done;
/* Guest reads from hooked arguments must not fault: a hooked function's registers
   are not necessarily pointers. Only read committed, readable host pages. */
static int hook_readable(const u8* M, u64 a, u64 n){
  if(a>(1ull<<40)) return 0;
#ifndef _WIN32
  (void)M; (void)n; return 1;
#else
  MEMORY_BASIC_INFORMATION mi; const u8* p=M+a;
  if(!VirtualQuery(p,&mi,sizeof mi) || mi.State!=MEM_COMMIT || (mi.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return 0;
  return p+n <= (const u8*)mi.BaseAddress+mi.RegionSize;
#endif
}
static u32 hook_rd32(const u8* M, u64 a){ return hook_readable(M,a,4)? *(const u32*)(M+a) : 0; }
static void hook_wrap(CPU* RESTRICT c, u64 e){
  u8* const M=c->mem; u64 lr=c->x[30], a0=c->x[0], a1=c->x[1], a2=c->x[2], a3=c->x[3]; u32 hv[4]={0,0,0,0}, w1=0;
  GuestFn orig=NULL; for(int k=0;k<g_hk_cnt;k++) if(g_hk_pc[k]==e){ orig=g_hk_orig[k]; break; }
  if(!orig){ c->pc=e; swr_interp_entry(c,e); return; }
  if(a0) for(int k=0;k<4;k++) hv[k]=hook_rd32(M,a0+(u64)g_hk_hoff+4*k);
  { u64 wp= g_hk_wreg==3? a3 : g_hk_wreg==2? a2 : a1; if(wp) w1=hook_rd32(M,wp); }
  u64 t0=plat_ns(); orig(c,e); u64 t1=plat_ns();
  if(g_hk_texhash && a0){ int k; for(k=0;k<8 && g_hk_tex[k] && g_hk_tex[k]!=a0;k++){} if(k<8){ g_hk_tex[k]=a0; g_hk_texn[k]++; } g_hk_last=t1; }
  __atomic_fetch_add(&g_hk_total,1,__ATOMIC_RELAXED);
  u32 i=__atomic_fetch_add(&g_hk_n,1,__ATOMIC_RELAXED);
  if(i>=g_hk_cap){ __atomic_fetch_add(&g_hk_lost,1,__ATOMIC_RELAXED); return; }
  HookRec* h=&g_hk[i]; h->t=t0; h->ns=t1-t0; h->pc=e; h->x0=a0; h->x1=a1; h->x3=a3; h->lr=lr; h->w1=w1;
  for(int k=0;k<4;k++) h->h[k]=hv[k];
  u32 tid= c->thread? (u32)((KThread*)c->thread)->id : 0;
  for(int k=0;k<6;k++) h->r[k]= a2? (i32)hook_rd32(M,a2+4*k) : 0;
  __atomic_store_n(&h->tid,tid|0x80000000u,__ATOMIC_RELEASE); }
static void hook_flush(int final){
  FILE* f=fopen(g_hk_path,"a"); if(!f) return;
  u32 n=g_hk_n; if(n>g_hk_cap) n=g_hk_cap;
  for(u32 i=g_hk_done;i<n;i++){ HookRec* h=&g_hk[i]; u32 tid=__atomic_load_n(&h->tid,__ATOMIC_ACQUIRE); if(!(tid&0x80000000u)){ n=i; break; }
    fprintf(f,"%llu,%llu,%u,%llx,%llx,%llx,%llx,%llx,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u\n",(unsigned long long)h->t,(unsigned long long)h->ns,tid&0x7FFFFFFFu,
      (unsigned long long)h->pc,(unsigned long long)h->lr,(unsigned long long)h->x0,(unsigned long long)h->x1,(unsigned long long)h->x3,
      h->r[0],h->r[1],h->r[2],h->r[3],h->r[4],h->r[5],h->h[0],h->h[1],h->h[2],h->h[3],h->w1); }
  g_hk_done=n;
  if(final) fprintf(f,"# end: %llu calls, %llu not recorded (buffer full)\n",(unsigned long long)g_hk_total,(unsigned long long)g_hk_lost);
  fclose(f); }
static void hook_atexit(void){ hook_flush(1); }
static int hook_install(u64 pc){
  if(g_hk_cnt>=HOOKMAX || !pc || pc<g_code_lo || pc>=g_code_hi || (pc&3) || !g_disp[(pc-g_code_lo)/4]) return 0;
  g_hk_pc[g_hk_cnt]=pc; g_hk_orig[g_hk_cnt]=g_disp[(pc-g_code_lo)/4]; g_hk_cnt++;
  g_disp[(pc-g_code_lo)/4]=hook_wrap; return 1; }
static void hook_thread(void* unused){ (void)unused; int ptr_done= g_hk_ptr==0;
  for(;;){ plat_sleep_ns(20000000ull);
    if(!ptr_done){ u64 pc=*(volatile u64*)GP(g_hk_ptr); if(pc && getenv("SWR_TRACE_HOOK_DEREF")) pc=*(volatile u64*)GP(pc);   /* GOT slot -> table entry -> function */
      if(pc && hook_install(pc)){ ptr_done=1; LOG("hook: [%llx] = %llx, dispatch wrapped -> %s\n",(unsigned long long)g_hk_ptr,(unsigned long long)pc,g_hk_path); } }
    if(g_hk_texhash && g_hk_last && plat_ns()-g_hk_last>200000000ull)
      for(int k=0;k<8 && g_hk_tex[k];k++){ u32 n=g_hk_texn[k]; if(!n) continue;
        u64 obj=g_hk_tex[k]-24; u32 w=*(u32*)GP(obj), hgt=*(u32*)GP(obj+4); u64 st=*(u64*)GP(obj+248);
        u64 sz=(u64)w*hgt*4, h=1469598103934665603ull; const u8* d=GP(st);
        if(st && sz && sz<=(64u<<20)) for(u64 i=0;i<sz;i++){ h^=d[i]; h*=1099511628211ull; }
        LOG("hook: texture %llx (%ux%u, storage %llx) after %u writes: fnv64 %016llx\n",(unsigned long long)g_hk_tex[k],w,hgt,(unsigned long long)st,n,(unsigned long long)h);
        g_hk_texn[k]-=n; }
    { static u64 lastf; u64 now=plat_ns(); if(g_hk_n-g_hk_done>=4096 || (g_hk_n!=g_hk_done && now-lastf>2000000000ull)){ lastf=now; hook_flush(0); } } } }
static void hook_init(void){
  const char* p=getenv("SWR_TRACE_HOOK_PTR"), *pcs=getenv("SWR_TRACE_HOOK_PC"), *path=getenv("SWR_TRACE_HOOK");
  if((!p && !pcs) || !path || !path[0]) return;
  g_hk_texhash= getenv("SWR_TRACE_HOOK_TEXHASH")!=NULL;
  { const char* o=getenv("SWR_TRACE_HOOK_HOFF"); if(o) g_hk_hoff=strtoll(o,NULL,0); o=getenv("SWR_TRACE_HOOK_W"); if(o) g_hk_wreg=atoi(o); }
  g_hk_cap=4u<<20; g_hk=calloc(g_hk_cap,sizeof(HookRec)); if(!g_hk) return; g_hk_path=path;
  FILE* f=fopen(path,"w"); if(f){ fprintf(f,"t_ns,inside_ns,tid,pc,lr,x0,x1,x3,r0,r1,r2,r3,r4,r5,h0,h1,h2,h3,w1\n"); fclose(f); }
  if(pcs){ const char* q=pcs; while(*q){ char* e; u64 pc=strtoull(q,&e,16); if(e==q) break;
      LOG("hook: pc %llx %s\n",(unsigned long long)pc,hook_install(pc)? "wrapped" : "NOT in dispatch table"); q= *e==','? e+1 : e; } }
  if(p){ g_hk_ptr=strtoull(p,NULL,0); LOG("hook: watching guest pointer %llx\n",(unsigned long long)g_hk_ptr); }
  atexit(hook_atexit); plat_thread(hook_thread,NULL,64*1024); }

void services_init(void){
  extern void svc_core_init(void), svc_hid_init(void), svc_fs_init(void), svc_vi_init(void), svc_pl_init(void), svc_launcher_init(void);
  svc_core_init(); svc_hid_init(); svc_fs_init(); svc_vi_init(); nv_init(); svc_pl_init(); svc_launcher_init();
}

static void split_path(const char* argv0){
  char p[1024]; strncpy(p,argv0?argv0:"",1023); p[1023]=0;
  char* s1=strrchr(p,'/'), *s2=strrchr(p,'\\'), *s=s1>s2? s1 : s2;
  const char* base= s? s+1 : p;
  strncpy(g_exename,base,255); char* dot=strrchr(g_exename,'.'); if(dot && (!strcmp(dot,".exe")||!strcmp(dot,".EXE"))) *dot=0;
  if(s){ s[1]=0; strncpy(g_exedir,p,1023); } else g_exedir[0]=0;
}

int swr_main(int argc, char** argv){
  /* setup's game-file reader (nxextract.c): an .xci / .nsp to exefs + romfs folders, then exit */
  if(argc>1 && !strcmp(argv[1],"--nx-extract")){ extern int nx_extract_main(int,char**); return nx_extract_main(argc-1,argv+1); }
  split_path(argv[0]);
  char ini[1400];
  snprintf(ini,sizeof ini,"%s%s.ini",g_exedir,g_exename);
  pm_init(&g_setq_lock); g_setq_init=1;
#if defined(SWR_HAVE_FRONTEND)
  { /* Play screen / first-run setup in the game window (beyond_ui/fe.cpp; on Linux its own window); returns 0 = start the game, 1 = quit */
    extern int backend_frontend(int argc, char** argv, const char* exedir, const char* exename);
#ifdef _WIN32
    if(backend_frontend(argc,argv,g_exedir,g_exename)){ log_flush(); fflush(NULL); TerminateProcess(GetCurrentProcess(),0); } }
#else
    if(backend_frontend(argc,argv,g_exedir,g_exename)){ log_flush(); fflush(NULL); _exit(0); } }
#endif
#endif
  ini_open(ini);
#if !defined(_WIN32)
  { /* no Play screen here (Linux; on Windows the Play screen sets them, or the old Python launcher does): the [Launcher] choices the game reads as switches, as fe.cpp sets them on Windows.
       A switch already in the environment (tests, a launch script) is left as it is */
    static const char* const L[][3]={{"wait_for_shaders","SWR_SYNC_SHADERS","1"}};
    if(!getenv("SWR_TEST_SCRIPT") && !getenv("SWR_TEST_EXIT_FRAME"))
      for(int i=0;i<1;i++) if(!getenv(L[i][1]) && atoi(ini_get(L[i][0],L[i][2]))) env_switch(L[i][1],1); }
#endif
  for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"-v")) g_verbose=2; else if(!strncmp(argv[i],"--",2) && strchr(argv[i],'=')){ char k[256]; const char* opt=argv[i]+2; const char* e=strchr(opt,'='); size_t key_len=(size_t)(e-opt); if(key_len>=sizeof k) continue; memcpy(k,opt,key_len); k[key_len]=0; const char* value=e+1; backend_ini(k,value); if(!strcmp(k,"docked")) g_opt_docked=atoi(value); if(!strcmp(k,"fps_limit")) g_opt_fps=atoi(value); else if(!strcmp(k,"profile")) g_opt_prof=atoi(value); } }
  snprintf(g_datadir,sizeof g_datadir,"%s%s_data/",g_exedir,g_exename);
  snprintf(g_hints_path,sizeof g_hints_path,"%s%s.hints",g_exedir,g_exename);
  pm_init(&g_hint_lock); pm_init(&g_flock);
  plat_install_crash_handler();
  g_t0=plat_ns(); g_t0_ticks=plat_ticks_raw();
  g_present_pacing_candidate=!getenv("SWR_NO_PRESENT_PACING")&&(getenv("SWR_PRESENT_PACING")!=NULL||!strcmp(g_exename,"Metroid Prime 4 Beyond"));
  LOG("%s (recompiled; SwitchRecomp runtime by MZ Labs)\n",g_exename); LOG("settings: startup fps_limit=%d, forced_vsync=%d\n",g_opt_fps,g_opt_vsync);
  if(load_code()) fatal("Could not load the game's recompiled code.");
  if(g_mod->abi!=1) fatal("The recompiled code is from another SwitchRecomp version.");
  g_title_id=g_mod->title_id;
  mem_init(); kernel_init(); build_dispatch(); hook_init();
  if(g_mod->init) g_mod->init(&g_rtapi);
  load_hints();
  if(loader_load()) fatal("Could not load the game executables from %sexefs/",g_datadir);
  { /* npdm SystemResourceSize; newer SDKs only init their VM allocator when > 0 */
    extern u64 g_sysres_size; char q[1200]; snprintf(q,sizeof q,"%sexefs/main.npdm",g_datadir); FILE* f=fopen(q,"rb");
    if(f){ u8 h[0x20]; if(fread(h,1,0x20,f)==0x20 && !memcmp(h,"META",4)){ u32 v; memcpy(&v,h+0x14,4); g_sysres_size=v; } fclose(f); }
    LOG("npdm: system resource %.1f MB\n",g_sysres_size/1048576.0); }
  services_init(); romfs_init(); gpu_init(); { extern void snxvk_gpu_init(void); snxvk_gpu_init(); }
  video_init(g_mod->title? g_mod->title : g_exename);
  { extern void plat_prof_start(int); if(g_opt_prof){ plat_prof_start(0); LOG("profile: active (profile.txt every 10 s)\n"); } }
  kernel_run_main(g_mod->mods[0].base+g_mod->mods[0].text_off,g_mod->main_stack?g_mod->main_stack:0x100000,(int)g_mod->main_prio,(int)g_mod->main_core);
  plat_thread_diag_start();
  /* 60Hz frontend loop */
  u64 next=plat_ns(); int tick=0;
  while(!g_quit){
    video_poll(); hid_update(); vsync_tick_vt(); ff_poll(); settings_test_hook(); settings_drain();
    if(tick%60==0) ini_poll();
    { extern void kernel_heal_mutexes(void); kernel_heal_mutexes(); }
    /* bail out of the slow clock if nothing was presented for 3s */
    { static int lf=-1, still; if(g_vt_on){ if(g_frames!=lf){ lf=g_frames; still=0; } else if(++still==180){ vt_set(0); WARN("smooth recording: the game stopped presenting images; back to the normal clock (recording continues)\n"); } } else { lf=-1; still=0; } }
    frontend_frame();
    if(++tick%300==0) save_hints();
    { extern void snxvk_tick(int); snxvk_tick(tick); }   /* native Vulkan: saves its pipeline cache every 5 s */
    /* SWR_STALL_DUMP=1: thread states when no frame came for 3 ticks (threads_stall_N.txt, first 6) */
    { static int on=-1, lf, still, nd; if(on<0) on=getenv("SWR_STALL_DUMP")!=NULL;
      if(on && nd<6){ if(g_frames!=lf){ lf=g_frames; still=0; } else if(++still==3 && g_frames>600){ extern __thread FILE* g_log_redirect; extern void kernel_dump_threads(void); char p[1200];
          snprintf(p,sizeof p,"%sthreads_stall_%d.txt",g_exedir,nd++); FILE* f=fopen(p,"w"); if(f){ fprintf(f,"tick %d, frames %d\n",tick,g_frames); g_log_redirect=f; kernel_dump_threads(); g_log_redirect=NULL; fclose(f); } } } }
    /* dump thread states to threads.txt every 15s, useful when something hangs */
    if(tick%900==0){ extern __thread FILE* g_log_redirect; extern void kernel_dump_threads(void); char p[1200]; snprintf(p,sizeof p,"%sthreads.txt",g_exedir);
      FILE* f=fopen(p,"w"); if(f){ fprintf(f,"second %d, frames %d\n",tick/60,g_frames); g_log_redirect=f; kernel_dump_threads(); g_log_redirect=NULL; fclose(f); } }
    /* auto diag only with diag=1, F5 always works */
    { extern int g_opt_diag; if(g_opt_diag && (tick==60*20 || tick==60*40)){ extern void gpu_gl_dump_rts(void); gpu_gl_dump_rts(); } }
    { static int at=-2; if(at==-2){ const char* e=getenv("SWR_DUMP_RTS_AT_FRAME"); at= e? atoi(e) : -1; }   /* test only: dump render targets once at this guest frame */
      if(at>0 && g_frames>=at){ at=-1; extern void gpu_gl_dump_rts(void); gpu_gl_dump_rts(); } }
    { static int last_frames=-1, still=0, dumps=0; if(g_frames!=last_frames){ last_frames=g_frames; still=0; }
      else if(++still==60*15 && dumps<4){ dumps++; still=0; extern void kernel_dump_threads(void);
        LOG("warning: %d s without a new image (frames=%d, GPU submits=%llu)\n",15,g_frames,(unsigned long long)g_gpu_submits); kernel_dump_threads(); } }
    next+=16666667ull; u64 now=plat_ns(); if(next>now) plat_sleep_ns((i64)(next-now)); else if(now-next>100000000ull) next=now;
  }
  save_hints(); { extern void gpu_gl_rec_finish(void); gpu_gl_rec_finish(); } { extern void snxvk_exit(void); snxvk_exit(); } LOG("Exit.\n"); log_flush(); fflush(NULL);
  { extern void launcher_on_exit(void); launcher_on_exit(); }
#if defined(_WIN32)
  TerminateProcess(GetCurrentProcess(),0);   /* don't join the guest threads */
#endif
  _exit(0);
}

#if !defined(_WIN32)
#include <dlfcn.h>

int dladdr_name(void* pc, char* out, int n){ Dl_info i; if(dladdr(pc,&i) && i.dli_sname){ snprintf(out,n,"%s+%lx",i.dli_sname,(unsigned long)((u8*)pc-(u8*)i.dli_saddr)); return 1; } return 0; }
#endif

/* ============================================================ audio renderer */
/* RequestUpdate status and 240-sample PCM output for each renderer frame. */

/* RequestUpdate layout as in yuzu's InfoUpdater.
   header (in and out): revision, behaviour, memory_pools, voices, voice_resources, effects, mixes, sinks,
   performance, splitter, render_info, pad[4], total. */
int audren_update(SvcObj* o, IpcCtx* c){
  { /* SWR_VIDEO_TIMING=1: how often the game updates its audio renderer (200 a second on the console, 5 ms each) */
    static int timing=-1; static u64 t0; static u32 n; if(timing<0) timing=!getenv("SWR_NO_VIDEO_TIMING");
    if(timing){ u64 now=plat_ns(); if(!t0) t0=now; if(++n%1000==0){ LOG("audio timing: renderer updated %.1f times a second (renderer %p)\n",1000e9/(double)(now-t0),(void*)o); t0=now; } } }
  u64 ia,is,oa,os; buf_in(c,0,&ia,&is); buf_out(c,0,&oa,&os);
  if(!ia || !oa || is<0x40) return 0;
  u8* in=GP(ia); u8* out=GP(oa);
  u32 ih[16]; memcpy(ih,in,64);
  u64 required=64;
  for(int k=1;k<=10;k++)required+=(u64)ih[k];
  /* newer renderers (REV12+, e.g. Metroid Prime 4's REV15) send 0x188-byte voices: the two biquad filters hold float
     coefficients (0x18 bytes each instead of 0xC), so every field after +0x24 is 0x18 later. The voice count is the
     voice resources' (0x70 each); those voices are converted to the 0x170 layout the mixer reads */
  u32 v2n= ih[4]/0x70, v2= ih[3] && ih[4] && ih[3]%0x170 && (u64)v2n*0x188==ih[3];
  if(required>is || (ih[3]%0x170 && !v2) || ih[4]%0x70 || ih[2]%0x20 || ih[7]%0x140){
    WARN("audren: malformed update layout (in %llu bytes; header %08x %x %x %x %x %x %x %x %x %x %x .. total %x)\n",(unsigned long long)is,
         ih[0],ih[1],ih[2],ih[3],ih[4],ih[5],ih[6],ih[7],ih[8],ih[9],ih[10],ih[15]);return RC(153,4);
  }
  u32 rev=ih[0]; int revn=(int)((rev>>24)&0xFF)-'0'; if(revn<0) revn=0;
  u32 behav=ih[1], mp=ih[2], vo=ih[3], vr=ih[4], ef=ih[5];
  u32 npools=mp/0x20, nvoices= v2? v2n : vo/0x170, neff=ef/0xC0, nsinks=ih[7]/0x140;
  u8* p=in+0x40+behav;
  u8* pools_in=p; p+=mp; p+=vr; u8* voices_in=p; p+=vo; u8* eff_in=p;
  static u8* v1buf; static u32 v1cap;
  if(v2){ if(nvoices*0x170>v1cap){ v1cap=nvoices*0x170; free(v1buf); v1buf=malloc(v1cap); }
    for(u32 i=0;i<nvoices;i++){ const u8* s=voices_in+i*0x188; u8* d=v1buf+i*0x170;
      memcpy(d,s,0x24); memcpy(d+0x3C,s+0x54,0x170-0x3C);
      for(int k=0;k<2;k++){ const u8* fs=s+0x24+k*0x18; u8* fd=d+0x24+k*12; fd[0]=fs[0]; fd[1]=0;
        for(int j=0;j<5;j++){ float f; memcpy(&f,fs+4+j*4,4); float q=f*16384.0f; i16 v= !(q==q)? 0 : q>32767.f? 32767 : q<-32768.f? -32768 : (i16)(q<0? q-0.5f : q+0.5f); memcpy(fd+2+j*2,&v,2); } } }
    voices_in=v1buf; }
  int eff_v2 = revn>=9;
  u32 o_mp=npools*0x10, o_vo=nvoices*0x10, o_ef=neff*(eff_v2? 0x90 : 0x10), o_sk=nsinks*0x20, o_pf=0x10, o_bh=0xB0, o_ri= revn>=5? 0x10 : 0;
  u32 oh[16]; memset(oh,0,sizeof oh);
  oh[0]=rev; oh[1]=o_bh; oh[2]=o_mp; oh[3]=o_vo; oh[4]=0; oh[5]=o_ef; oh[6]=0; oh[7]=o_sk; oh[8]=o_pf; oh[9]=0; oh[10]=o_ri;
  oh[15]=0x40+o_mp+o_vo+o_ef+o_sk+o_pf+o_bh+o_ri;
  if(oh[15]>os){ WARN("audren: output buffer too small (%u > %llu)\n",oh[15],(unsigned long long)os); return RC(153,4); }
  static int said; if(!said){ said=1; LOG("audren: REV%d, %u pools, %u voices, %u effects, %u sinks\n",revn,npools,nvoices,neff,nsinks); }
  memset(out,0,oh[15]); memcpy(out,oh,64);
  u8* q=out+0x40;
  /* pools: RequestAttach(4)->Attached(5), RequestDetach(2)->Detached(3) */
  for(u32 i=0;i<npools;i++){ u32 st; memcpy(&st,pools_in+i*0x20+0x10,4);
    u32 ns= st==4? 5 : st==2? 3 : st; memcpy(q+i*0x10,&ns,4); }
  q+=o_mp;
  extern void audio_render(SvcObj*,const u32*,const u8*,u64,const u8*,u32,u8*);
  /* The console's audio hardware mixes 5 ms every 5 ms on its own; RequestUpdate only changes what it mixes. A game
     that updates every 5 ms needs one block per update. Metroid Prime 4 updates once a frame (60 a second), and one
     block per update gave 0.3 s of sound per second: choppy audio with gaps. Mix as many 5 ms blocks as real time has
     passed since the last update (at least one, at most 8 after a stall; the remainder carries over, so the long-run
     rate is exactly 48 kHz). The extra blocks see the same voices without the once-per-update actions (a new voice's
     reset, buffer flushes) */
  /* Several updates within 5 ms mix nothing (status only): Metroid Prime 4 calls RequestUpdate about 20000 times a
     second for half a second during a transition, and one block per call made 50 s of sound for a 160 ms queue (11735
     blocks dropped, the sound out of step afterwards). Audio advances with real time only, as on the console */
  { u64 now=plat_ns(); u32 blocks=1; double owed=0;
    if(o->u[6]){ memcpy(&owed,&o->u[7],8); owed+=(double)(now-o->u[6])/5000000.0; if(owed>8) owed=8; blocks=(u32)owed; owed-=blocks; memcpy(&o->u[7],&owed,8); }
    o->u[6]=now;
    { extern int g_audio_status_only; g_audio_status_only= blocks==0; audio_render(o,ih,in,is,voices_in,nvoices,q); g_audio_status_only=0; }
    if(blocks>1){ static u8* again; static u32 again_cap; u32 vbytes=nvoices*0x170;
      if(vbytes>again_cap){ free(again); again=malloc(vbytes); again_cap=vbytes; }
      memcpy(again,voices_in,vbytes); for(u32 i=0;i<nvoices;i++){ again[i*0x170+8]=0; again[i*0x170+0x159]=0; }
      for(u32 b=1;b<blocks;b++) audio_render(o,ih,in,is,again,nvoices,q); } }
  q+=o_vo;
  /* effects: Enabled(3) / Disabled(4) */
  for(u32 i=0;i<neff;i++){ u8 en=eff_in[i*0xC0+2]; q[i*(eff_v2?0x90:0x10)]= en? 3 : 4; }
  q+=o_ef; q+=o_sk; q+=o_pf; q+=o_bh;
  if(o_ri){ u64 frames=o->u[5]++; memcpy(q,&frames,8); }
  return 0;
}

/* pl:u shared fonts. We don't ship Nintendo's: use GAME_data/fonts/<Name>.bfttf|.ttf if there,
   else a system ttf (Arial, Segoe UI, DejaVu) wrapped in BFTTF. */
static const char* g_font_names[6]={"FontStandard","FontChineseSimplified","FontExtendedChineseSimplified","FontChineseTraditional","FontKorean","FontNintendoExtended"};
static u32 g_font_off[6], g_font_size[6]; static int g_fonts_ready;

static u8* read_file_all(const char* path, u64* size){
  FILE* f=fopen(path,"rb"); if(!f) return NULL; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  if(n<=0){ fclose(f); return NULL; } u8* d=malloc((size_t)n+4); memset(d+n,0,4); if(fread(d,1,(size_t)n,f)!=(size_t)n){ fclose(f); free(d); return NULL; } fclose(f); *size=(u64)n; return d; }

static u32 bswap32_(u32 v){ return (v>>24)|((v>>8)&0xFF00)|((v<<8)&0xFF0000)|(v<<24); }

static u8* font_fallback(u64* size){
  const char* cands[8]; int n=0; char w1[512], w2[512], w3[512];
#if defined(_WIN32)
  const char* wd=getenv("WINDIR"); if(!wd) wd="C:\\Windows";
  snprintf(w1,sizeof w1,"%s\\Fonts\\arial.ttf",wd); snprintf(w2,sizeof w2,"%s\\Fonts\\segoeui.ttf",wd); snprintf(w3,sizeof w3,"%s\\Fonts\\tahoma.ttf",wd);
  cands[n++]=w1; cands[n++]=w2; cands[n++]=w3;
#else
  (void)w1; (void)w2; (void)w3;
  cands[n++]="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"; cands[n++]="/usr/share/fonts/TTF/DejaVuSans.ttf"; cands[n++]="/usr/share/fonts/truetype/freefont/FreeSans.ttf";
#endif
  for(int i=0;i<n;i++){ u8* d=read_file_all(cands[i],size); if(d){ LOG("pl: system font -> %s\n",cands[i]); return d; } }
  return NULL; }

static void fonts_fill(u8* shm, u64 cap){
  if(g_fonts_ready) return; g_fonts_ready=1;
  const u32 RESULT=0x7f9a0218u, MAGIC=0x36f81a1eu; u32 key=bswap32_(RESULT^MAGIC);
  u64 off=0; u8* fb=NULL; u64 fbs=0; u32 fb_off=0, fb_size=0;
  for(int i=0;i<6;i++){
    char p[1200]; u64 sz=0; u8* d=NULL; int is_bfttf=0;
    snprintf(p,sizeof p,"%sfonts/%s.bfttf",g_datadir,g_font_names[i]); d=read_file_all(p,&sz); if(d) is_bfttf=1;
    if(!d){ snprintf(p,sizeof p,"%sfonts/%s.ttf",g_datadir,g_font_names[i]); d=read_file_all(p,&sz); }
    if(!d){ /* one fallback font for every slot */
      if(fb_size){ g_font_off[i]=fb_off; g_font_size[i]=fb_size; continue; }
      if(!fb) fb=font_fallback(&fbs); if(!fb){ WARN("pl: no TTF font found; system text will not be shown\n"); return; }
      d=fb; sz=fbs; }
    else LOG("pl: %s <- %s\n",g_font_names[i],p);
    u64 words=(sz+3)/4;
    if(off+8+words*4>cap){ WARN("pl: font %s does not fit\n",g_font_names[i]); if(d!=fb) free(d); continue; }
    u32 h0=bswap32_(MAGIC), h1=bswap32_((u32)(words*4))^key; memcpy(shm+off,&h0,4); memcpy(shm+off+4,&h1,4);
    if(is_bfttf){ /* bfttf -> decrypt, shmem holds plain ttf */
      u32 w0, w1; memcpy(&w0,d,4); memcpy(&w1,d+4,4); u32 k2=w0^RESULT; u32 tsz=bswap32_(w1^k2); if(tsz>sz-8) tsz=(u32)(sz-8);
      for(u64 k=0;k+8<sz;k+=4){ u32 w; memcpy(&w,d+8+k,4); w=bswap32_(w^k2); memcpy(shm+off+8+k,&w,4); }
      g_font_off[i]=(u32)off+8; g_font_size[i]=tsz; }
    else { memcpy(shm+off+8,d,sz); g_font_off[i]=(u32)off+8; g_font_size[i]=(u32)sz; }
    if(d==fb){ fb_off=g_font_off[i]; fb_size=g_font_size[i]; }
    off=(off+8+words*4+0xF)&~0xFull;
    if(d!=fb) free(d);
  }
  if(fb) free(fb);
}

static int pl_disp(SvcObj* o, IpcCtx* c){
  static KShmem* shm; extern KShmem* kshmem_new(u64,const char*); extern u8* kshmem_ptr(KShmem*);
  if(!shm){ shm=kshmem_new(0x1100000,"font"); kobj_ref(&shm->o); fonts_fill(kshmem_ptr(shm),0x1100000); }
  u32 id=in32(c,0); if(id>5) id=0;
  switch(c->cmd){
    case 0: return 0;                                                       /* RequestLoad */
    case 1: out32(c,1); return 0;                                           /* GetLoadState: loaded */
    case 2: out32(c,g_font_size[id]); return 0;                             /* GetSize */
    case 3: out32(c,g_font_off[id]); return 0;                              /* GetSharedMemoryAddressOffset */
    case 4: out_copy_h(c,h_new(&shm->o)); return 0;
    case 5: case 6: {                                                       /* GetSharedFontInOrderOfPriority */
      u64 a0,s0,a1,s1,a2,s2; buf_out(c,0,&a0,&s0); buf_out(c,1,&a1,&s1); buf_out(c,2,&a2,&s2);
      u32 n=6; for(u32 i=0;i<n;i++){ if(a0 && (i+1)*4<=s0) gw32(a0+i*4,i); if(a1 && (i+1)*4<=s1) gw32(a1+i*4,g_font_off[i]); if(a2 && (i+1)*4<=s2) gw32(a2+i*4,g_font_size[i]); }
      out8(c,1); c->out_size=4; out32(c,n); return 0; }
  }
  return stub_ok(o,c); }

static SvcObj* new_pl(void){ return svcobj_new("pl:u",pl_disp,0); }

void svc_pl_init(void){ svc_register("pl:u",new_pl); svc_register("pl:s",new_pl); }
