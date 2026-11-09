/* Cheat codes in the Atmosphere (dmnt) format and the FOV hook.

   Cheats: <game>_data/cheats/<BUILD ID>.txt, BUILD ID = the first 16 hex digits of main's build id (the name Atmosphere,
   Ryujinx and Yuzu use, so existing cheat files work as they are). Each [Name] starts a cheat, {Name} the master code that
   always runs. The cheats that are on are listed by name in <BUILD ID>.enabled next to it (the in-game menu writes it);
   both files are read again when they change, so a cheat can be edited while the game runs.
   The codes run once per controller update (hid_update, about 60 times a second), like dmnt's frame loop.

   Opcodes: 0 store static, 1 compare memory, 2 end/else, 3 loop, 4 load register, 5 load from memory, 6 store to
   register address, 7 legacy arithmetic, 8 keypress, 9 arithmetic, A store register, C0 register compare, C1/C2/C3
   save/restore registers, FF pause/resume (ignored). Memory regions: 0 main, 1 heap, 2 alias, 3 ASLR (the code region).
   The game's code is recompiled to native code ahead of time: a cheat that writes ARM instructions has no effect (data
   cheats, the common kind, work). Writes go only to mapped, writable guest memory.

   FOV hook: beyond/fov_hooks.txt (next to the recompiler, copied to <game>_data/fov_hooks.txt by setup) or
   <game>_data/cheats/<BUILD ID>.fov, one line: BUILD_ID region+offset [ptr_offset ...] (a pointer chain to the camera's
   field of view, a float). Each update the game's own value is scaled by [Gameplay] fov (percent, 100 = the game's). */
#include "rt.h"
#include <sys/stat.h>
#ifdef _WIN32
#define strncasecmp _strnicmp
#endif

int g_opt_cheats=1, g_opt_fov=100, g_opt_green10=0;
extern u64 g_pad_buttons_now;   /* svc_hid.c: buttons as the game sees them this update */

/* The instruction-patch cheats of the GBAtemp thread for this build (8275FBE1EC5191C2), rebuilt as hooks: a patch to ARM
   code can't reach recompiled code, so beyond_patches.json adds c->rt->svc(c,0xFF80+id) right after each instruction those
   cheats replace and this does what the replacement would have done, while its option ([Gameplay] cheat_*, live) is on.
   By Gamerjin, Luffysan and others, with pointers by ZiT866. The hooks cost a call that returns at once when off. */
#define NAT_N 17
static const char* const nat_key[NAT_N]={"cheat_missiles","cheat_super_shot","cheat_large_shot","cheat_power_bombs","cheat_ammo","cheat_health",
  "cheat_arm_cannon","cheat_elemental_ammo","cheat_speed","cheat_walls","cheat_flare_timer","cheat_crystals","cheat_jumps","cheat_ally_health",
  "cheat_ally_dying","cheat_one_hit","cheat_two_hit"};
static volatile int g_nat[NAT_N];
int cheats_nat_opt(const char* k, const char* v){ for(int i=0;i<NAT_N;i++) if(!strcmp(k,nat_key[i])){ g_nat[i]=atoi(v)!=0; return 1; } return 0; }
static inline void nat_s(CPU* c, int r, f32 v){ V128* p=&c->v[r]; p->d[0]=0; p->d[1]=0; p->f[0]=v; }
static inline u32 nat_r32(u64 a){ u32 v; memcpy(&v,GP(a),4); return v; }
static inline void nat_w32(u64 a, u32 v){ memcpy(GP(a),&v,4); }
/* the guest instruction before each hook has just used these addresses, so they are mapped */
void cheats_native(CPU* c, u32 id){
  extern int g_opt_cheats;
  if(id>=NAT_N || !g_nat[id] || !g_opt_cheats) return;
  { static volatile int said[NAT_N]; if(!said[id]){ said[id]=1; LOG("cheats: %s is working\n",nat_key[id]); } }
  switch(id){
  case 0: case 1: case 2: case 3: c->x[2]=0; break;   /* the amount a shot uses */
  case 4: nat_s(c,0,0); break;                        /* the amount any ammo changes by */
  case 5: nat_w32(c->x[8],c->v[1].s[0]); break;       /* health = its maximum */
  case 6: c->x[0]=1; break;                           /* "has this elemental shot" */
  case 7: nat_s(c,1,20); break;                       /* the elemental ammo check reads 20 */
  case 8: nat_s(c,0,(g_pad_buttons_now&2)? 200 : 100); break;   /* B = faster */
  case 9: { f32 v= (g_pad_buttons_now&0x40)? -1.5f : 1.5f; nat_s(c,6,v); nat_w32(c->x[0]+60,c->v[6].s[0]); break; }   /* L = the other way */
  case 10: { u16 h; memcpy(&h,GP(c->x[19]+141),2); if(h==561){ nat_s(c,2,0); nat_w32(c->x[19]+136,0); } break; }   /* only the Flare Pool countdown */
  case 11: if(g_pad_buttons_now&0x100) c->x[9]=204; break;   /* while ZL is held */
  case 12: c->x[0]=1; break;                          /* "can jump" */
  case 13: nat_s(c,0,0); c->v[0].d[0]=nat_r32(c->x[0]+24); break;   /* reads the maximum, not the current health */
  case 14: c->x[0]=0; break;                          /* "is dying" */
  case 15: c->v[0].f[0]+=c->v[2].f[0]; c->v[0].s[1]=0; c->v[0].d[1]=0; break;   /* damage + current health */
  case 16: { if(c->x[0]){ f32 h; u32 v=nat_r32(c->x[0]+28); memcpy(&h,&v,4); if(h>2.0f){ h=2.0f; memcpy(&v,&h,4); nat_w32(c->x[0]+28,v); } } break; }   /* health over 2 becomes 2 */
  } }

#define MAXC 256
#define MAXW 4096
typedef struct { char name[64]; u32 off, n; int on, master; } Cheat;
static Cheat g_ch[MAXC]; static u32 g_nch; static u32 g_words[MAXW]; static u32 g_nwords;
static PMutex g_m; static int g_m_init;
static char g_bid[17], g_path[1200], g_en_path[1200], g_fov_path[1200];
static u64 g_mt_txt, g_mt_en;
static u64 g_reg[16], g_saved[16];
static u64 g_main_base, g_aslr_base;

static void lock(void){ if(!g_m_init){ pm_init(&g_m); g_m_init=1; } pm_lock(&g_m); }
static void unlock(void){ pm_unlock(&g_m); }
#ifdef _WIN32
static u64 mtime(const char* p){ struct stat st; return stat(p,&st)? 0 : (u64)st.st_mtime*1000000000ull+(u64)st.st_size; }
#else
static u64 mtime(const char* p){ struct stat st; return stat(p,&st)? 0 : (u64)st.st_mtime*1000000000ull+(u64)st.st_mtim.tv_nsec; }
#endif

static void ids(void){
  if(g_bid[0] || !g_mod) return;
  for(u32 i=0;i<g_mod->nmods;i++){ const SwrModInfo* m=&g_mod->mods[i];
    if(!strcmp(m->name,"main")){ g_main_base=m->base; for(int k=0;k<8;k++) snprintf(g_bid+2*k,3,"%02X",m->build_id[k]); }
    if(!g_aslr_base || m->base<g_aslr_base) g_aslr_base=m->base; }
  snprintf(g_path,sizeof g_path,"%scheats/%s.txt",g_datadir,g_bid);
  snprintf(g_en_path,sizeof g_en_path,"%scheats/%s.enabled",g_datadir,g_bid); }

static int name_enabled(const char* list, const char* name){
  if(!list) return 0; size_t n=strlen(name);
  for(const char* p=list;*p;){ const char* e=strchr(p,'\n'); size_t l= e? (size_t)(e-p) : strlen(p);
    while(l && (p[l-1]=='\r' || p[l-1]==' ')) l--;
    if(l==n && !memcmp(p,name,n)) return 1; if(!e) break; p=e+1; }
  return 0; }

static char* slurp(const char* p){ FILE* f=fopen(p,"rb"); if(!f) return NULL; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  char* b=malloc((size_t)(n>0?n:0)+1); size_t r=fread(b,1,(size_t)(n>0?n:0),f); b[r]=0; fclose(f); return b; }

/* caller holds the lock */
static void load(void){
  g_nch=0; g_nwords=0; g_mt_txt=mtime(g_path); g_mt_en=mtime(g_en_path);
  char* t=slurp(g_path); if(!t) return; char* en=slurp(g_en_path);
  Cheat* cur=NULL;
  for(char* p=t;*p;){
    while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n') p++;
    if(!*p) break;
    if(*p=='[' || *p=='{'){ char close= *p=='['? ']' : '}'; char* e=strchr(p+1,close); if(!e) break;
      if(g_nch>=MAXC) break; cur=&g_ch[g_nch++]; memset(cur,0,sizeof *cur);
      size_t l=(size_t)(e-p-1); if(l>63) l=63; memcpy(cur->name,p+1,l); cur->name[l]=0;
      cur->master= close=='}'; cur->off=g_nwords; cur->on= cur->master || name_enabled(en,cur->name);
      p=e+1; continue; }
    char* q=p; u32 v=0; int nd=0;
    while(nd<8 && ((*q>='0'&&*q<='9')||(*q>='a'&&*q<='f')||(*q>='A'&&*q<='F'))){ v=v*16+(u32)(*q<='9'? *q-'0' : (*q|32)-'a'+10); q++; nd++; }
    if(nd==8 && cur && g_nwords<MAXW){ g_words[g_nwords++]=v; cur->n++; p=q; continue; }
    while(*p && *p!='\n') p++;   /* anything else on the line: a comment */
  }
  free(t); free(en);
  u32 on=0; for(u32 i=0;i<g_nch;i++) on+=g_ch[i].on;
  LOG("cheats: %u in %s (%u on)\n",g_nch,g_path,on); }

static void save_enabled(void){
  char dir[1200]; snprintf(dir,sizeof dir,"%scheats",g_datadir); plat_mkdir(dir);
  FILE* f=fopen(g_en_path,"w"); if(!f) return;
  for(u32 i=0;i<g_nch;i++) if(g_ch[i].on && !g_ch[i].master) fprintf(f,"%s\n",g_ch[i].name);
  fclose(f); g_mt_en=mtime(g_en_path); }

/* ------------------------------------------------------------------ guest memory */
static int ok_range(u64 a, u64 n, int write){
  if(!a || a>=AS_SIZE || n>AS_SIZE-a) return 0; MemBlock b; mem_query(a,&b);
  if(a<b.addr || a-b.addr+n>b.size || !(b.perm&PERM_R)) return 0;
  return !write || (b.perm&PERM_W); }
static int rd(u64 a, int w, u64* v){ if(!ok_range(a,(u64)w,0)) return 0; u64 x=0; memcpy(&x,GP(a),(size_t)w); *v=x; return 1; }
static void wr(u64 a, int w, u64 v){ if(ok_range(a,(u64)w,1)) memcpy(GP(a),&v,(size_t)w); }
static u64 region(int m){ return m==0? g_main_base : m==1? HEAP_BASE : m==2? ALIAS_BASE : g_aslr_base; }
static u64 mask(int w, u64 v){ return w>=8? v : v&((1ull<<(w*8))-1); }
static int cmp(int c, u64 a, u64 b){ switch(c){ case 1: return a>b; case 2: return a>=b; case 3: return a<b; case 4: return a<=b; case 5: return a==b; case 6: return a!=b; } return 0; }
static u64 arith(int op, u64 a, u64 b){ switch(op){ case 0: return a+b; case 1: return a-b; case 2: return a*b; case 3: return a<<(b&63); case 4: return a>>(b&63);
  case 5: return a&b; case 6: return a|b; case 7: return ~a; case 8: return a^b; case 9: return a; } return a; }

/* words needed by the opcode at w[0] (to skip it inside a false condition) */
static u32 op_len(const u32* w){
  u32 o=w[0]>>28, t=(w[0]>>24)&15;
  switch(o){
  case 0: case 1: return t==8? 4 : 3;
  case 4: return 3; case 5: return 2; case 6: return 3; case 7: return 2;
  case 9: return ((w[0]>>8)&15)==1? (t==8? 3 : 2) : 1;
  case 0xA: { u32 ot=(w[0]>>8)&15; return (ot==2 || ot==4 || ot==5)? 2 : 1; }
  case 0xC: { u32 sub=(w[0]>>24)&15; if(sub!=0) return 1; u32 x=(w[0]>>8)&15, tt=(w[0]>>20)&15;
    return (x==0 || x==2)? 2 : x==4? (tt==8? 3 : 2) : 1; }
  default: return 1; } }

/* skip forward from i past the block that ends at the matching 2 (end) or, when stop_else, stops after a matching else */
static u32 skip_block(const u32* w, u32 n, u32 i, int stop_else){
  int depth=0;
  while(i<n){ u32 o=w[i]>>28;
    if(o==2){ int els=((w[i]>>24)&15)==1; if(depth==0 && (!els || stop_else)) return i+1; if(!els) depth--; i++; continue; }
    if(o==1 || o==8 || (o==0xC && ((w[i]>>24)&15)==0)) depth++;
    i+=op_len(w+i); }
  return n; }

static void run(const u32* w, u32 n){
  u32 loop_start[16]={0}; int steps=0;
  for(u32 i=0;i<n && steps<100000;steps++){
    u32 a=w[i], o=a>>28; int t=(int)((a>>24)&15); u32 len=op_len(w+i);
    if(i+len>n) return;
    switch(o){
    case 0: { int m=(a>>20)&15, r=(a>>16)&15; u64 ad=region(m)+(((u64)(a&0xFF)<<32)|w[i+1])+g_reg[r];
      u64 v= t==8? ((u64)w[i+2]<<32|w[i+3]) : w[i+2]; wr(ad,t,mask(t,v)); break; }
    case 1: { int m=(a>>20)&15, c=(a>>16)&15; u64 ad=region(m)+(((u64)(a&0xFF)<<32)|w[i+1]);
      u64 v= t==8? ((u64)w[i+2]<<32|w[i+3]) : w[i+2], cur=0;
      if(!rd(ad,t,&cur) || !cmp(c,mask(t,cur),mask(t,v))){ i=skip_block(w,n,i+len,1); continue; } break; }
    case 2: if(((a>>24)&15)==1){ i=skip_block(w,n,i+1,0); continue; } break;   /* reaching an else: the if part ran, skip to the end */
    case 3: { int r=(a>>16)&15;
      if(((a>>24)&15)==0){ g_reg[r]=w[i+1]; loop_start[r]=i+2; i+=2; continue; }
      if(g_reg[r]>1){ g_reg[r]--; i=loop_start[r]; continue; } g_reg[r]=0; break; }
    case 4: g_reg[(a>>16)&15]=(u64)w[i+1]<<32|w[i+2]; break;
    case 5: { int m=(a>>20)&15, r=(a>>16)&15, mode=(a>>12)&15; u64 off=((u64)(a&0xFF)<<32)|w[i+1];
      u64 ad= mode==1? g_reg[r]+off : region(m)+off, v=0; g_reg[r]= rd(ad,t,&v)? mask(t,v) : 0; break; }
    case 6: { int r=(a>>16)&15, inc=(a>>12)&15, useo=(a>>8)&15, ro=(a>>4)&15; u64 ad=g_reg[r]+(useo? g_reg[ro] : 0);
      wr(ad,t,mask(t,(u64)w[i+1]<<32|w[i+2])); if(inc) g_reg[r]+=(u64)t; break; }
    case 7: { int r=(a>>16)&15, op=(a>>12)&15; g_reg[r]=mask(t,arith(op,g_reg[r],w[i+1])); break; }
    case 8: { u64 k=a&0x0FFFFFFF; if((g_pad_buttons_now&k)!=k){ i=skip_block(w,n,i+1,1); continue; } break; }
    case 9: { int op=(a>>20)&15, r=(a>>16)&15, s=(a>>12)&15, imm=(a>>8)&15; u64 rhs;
      if(imm) rhs= t==8? ((u64)w[i+1]<<32|w[i+2]) : w[i+1]; else rhs=g_reg[(a>>4)&15];
      g_reg[r]=mask(t,arith(op,g_reg[s],rhs)); break; }
    case 0xA: { int s=(a>>20)&15, r=(a>>16)&15, inc=(a>>12)&15, ot=(a>>8)&15, x=(a>>4)&15; u64 ad;
      u64 imm= len==2? (((u64)(a&15)<<32)|w[i+1]) : 0;
      switch(ot){ case 1: ad=g_reg[r]+g_reg[x]; break; case 2: ad=g_reg[r]+imm; break; case 3: ad=region(x)+g_reg[r]; break;
        case 4: ad=region(x)+imm; break; case 5: ad=region(x)+g_reg[r]+imm; break; default: ad=g_reg[r]; }
      wr(ad,t,mask(t,g_reg[s])); if(inc) g_reg[r]+=(u64)t; break; }
    case 0xC: { int sub=(a>>24)&15;
      if(sub==0){ int tt=(a>>20)&15, c=(a>>16)&15, s=(a>>12)&15, x=(a>>8)&15, m=(a>>4)&15, r2=a&15; u64 rhs=0; int ok=1;
        switch(x){ case 0: ok=rd(region(m)+(((u64)r2<<32)|w[i+1]),tt,&rhs); break; case 1: ok=rd(region(m)+g_reg[r2],tt,&rhs); break;
          case 2: ok=rd(g_reg[m]+(((u64)r2<<32)|w[i+1]),tt,&rhs); break; case 3: ok=rd(g_reg[m]+g_reg[r2],tt,&rhs); break;
          case 4: rhs= tt==8? ((u64)w[i+1]<<32|w[i+2]) : w[i+1]; break; case 5: rhs=g_reg[m]; break; default: ok=0; }
        if(!ok || !cmp(c,mask(tt,g_reg[s]),mask(tt,rhs))){ i=skip_block(w,n,i+len,1); continue; } }
      else if(sub==1){ int dst=(a>>8)&15, src=(a>>4)&15, op=a&15; if(op==0) g_saved[dst]=g_reg[src]; else if(op==1) g_reg[dst]=g_saved[src]; else if(op==2) g_saved[dst]=0; else g_reg[dst]=0; }
      else if(sub==2){ int op=(a>>20)&15; u32 bits=a&0xFFFF; for(int k=0;k<16;k++) if(bits&(1u<<k)){ if(op==0) g_saved[k]=g_reg[k]; else if(op==1) g_reg[k]=g_saved[k]; else if(op==2) g_saved[k]=0; else g_reg[k]=0; } }
      break; }
    default: break; }   /* FF (pause/resume, debug log) and unknown: no effect */
    i+=len; } }

/* ------------------------------------------------------------------ FOV hook */
typedef struct { int have; int region; u64 off; int nptr; u64 ptr[8]; } FovHook;
static FovHook g_fov; static int g_fov_loaded; static float g_fov_game, g_fov_written;

static int fov_parse(const char* line, FovHook* h){
  char id[40]; char rg[16]; unsigned long long off; int pos=0;
  if(sscanf(line,"%39s %15[a-z]+%llx%n",id,rg,&off,&pos)<3 || strncasecmp(id,g_bid,16)) return 0;
  memset(h,0,sizeof *h); h->region= !strcmp(rg,"heap")? 1 : !strcmp(rg,"alias")? 2 : !strcmp(rg,"aslr")? 3 : 0; h->off=off;
  for(const char* p=line+pos;h->nptr<8;){ unsigned long long v; int k=0; if(sscanf(p," %llx%n",&v,&k)!=1) break; h->ptr[h->nptr++]=v; p+=k; }
  h->have=1; return 1; }

static void fov_load(void){
  g_fov_loaded=1; memset(&g_fov,0,sizeof g_fov);
  const char* files[2]; char a[1200], b[1200];
  snprintf(a,sizeof a,"%scheats/%s.fov",g_datadir,g_bid); snprintf(b,sizeof b,"%sfov_hooks.txt",g_datadir); files[0]=a; files[1]=b;
  for(int k=0;k<2 && !g_fov.have;k++){ FILE* f=fopen(files[k],"r"); if(!f) continue; char ln[512];
    while(fgets(ln,sizeof ln,f)) if(ln[0]!='#' && ln[0]!=';' && fov_parse(ln,&g_fov)){ snprintf(g_fov_path,sizeof g_fov_path,"%s",files[k]); break; }
    fclose(f); }
  if(g_fov.have) LOG("fov: hook for build %s from %s\n",g_bid,g_fov_path);
  else LOG("fov: no FOV hook for build %s yet (beyond/fov_hooks.txt): the FOV setting has no effect\n",g_bid); }

int fov_available(void){ lock(); ids(); if(!g_fov_loaded) fov_load(); int r=g_fov.have; unlock(); return r; }

static void fov_tick(void){
  if(!g_fov_loaded) fov_load();
  if(!g_fov.have) return;
  u64 ad=region(g_fov.region)+g_fov.off;
  for(int k=0;k<g_fov.nptr;k++){ u64 p=0; if(!rd(ad,8,&p) || !p) return; ad=p+g_fov.ptr[k]; }
  u64 raw=0; if(!rd(ad,4,&raw)) return; float cur; u32 r32=(u32)raw; memcpy(&cur,&r32,4);
  if(!(cur>1.0f && cur<179.0f)) return;   /* not a field of view in degrees: the chain points elsewhere right now */
  if(cur!=g_fov_written) g_fov_game=cur;   /* the game set a new value (camera change): scale that one */
  if(g_opt_fov==100 && cur==g_fov_game) return;
  float want=g_fov_game*(float)g_opt_fov/100.0f; if(want>170.0f) want=170.0f;
  u32 o32; memcpy(&o32,&want,4); wr(ad,4,o32); g_fov_written=want; }

/* ------------------------------------------------------------------ per update, and the menu's view */
void cheats_tick(void){
  if(!g_mod) return;
  lock(); ids();
  if(g_opt_cheats){
    static u32 n; if((++n&63)==0 && (mtime(g_path)!=g_mt_txt || mtime(g_en_path)!=g_mt_en)) load();   /* edited while playing */
    else if(n==1) load();
    for(u32 i=0;i<g_nch;i++) if(g_ch[i].on && g_ch[i].n){ memset(g_reg,0,sizeof g_reg); run(g_words+g_ch[i].off,g_ch[i].n); } }
  fov_tick();
  unlock(); }

int cheats_count(void){ lock(); ids(); if(!g_mt_txt && !g_nch) load(); int n=(int)g_nch; unlock(); return n; }
int cheats_info(int i, char* name, int cap, int* on, int* master){
  lock(); int ok= i>=0 && (u32)i<g_nch; if(ok){ snprintf(name,(size_t)cap,"%s",g_ch[i].name); *on=g_ch[i].on; *master=g_ch[i].master; } unlock(); return ok; }
void cheats_set(int i, int on){ lock(); if(i>=0 && (u32)i<g_nch && !g_ch[i].master){ g_ch[i].on=on!=0; save_enabled(); } unlock(); }
const char* cheats_path(void){ lock(); ids(); unlock(); return g_path; }
void cheats_reload(void){ lock(); ids(); load(); g_fov_loaded=0; unlock(); }
