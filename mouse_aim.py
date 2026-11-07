# Copyright 2026 Zorkats. Apache-2.0, as SwitchRecomp.
"""Mouse aim for Metroid Prime 4 Beyond 2.1.0 (main build 8275FBE1EC5191C2): the mouse turns the first-person camera by
an angle per count, as motion controls do, instead of standing in for the right stick.

F_920C224 updates the first-person look once per logic step. Its gyro path calls F_8FA66A0 for the motion-control
deltas and keeps them in S9 (yaw) and S10 (pitch):
    0x920CBF8  FMOV S9, S0     yaw:   rate obj+0x35C = v118 * (S9 * -60 - stick term) deg/s, integrated by the step
                                       (k/60 s), so one step turns by -S9 * v118 * k degrees (negative turns right)
    0x920CBFC  FMOV S10, S1    pitch: obj+0x358 (degrees, positive looks up) += S10 * v118 * m, m = obj+0x37C
v118 is the game's aim slowdown near a target (it scales the stick too); m is the look multiplier the stick path also
applies. These overrides add the mouse counts taken since the last step: S9 += dx * deg_x * m / k (m keeps mouse yaw
in step with pitch and the stick), S10 += -dy * deg_y (screen y is down). The pitch added is limited so the mouse alone
never carries the view past 89 degrees up or down.

The runtime (input_kbm.c) adds the counts to swr_aim_pend, which this path takes, and reads swr_aim_steps to see
whether it is running. It isn't with lock-on (F_920C224 returns through F_83CBED0 before it), in morph ball, cutscenes
and menus; there the runtime empties swr_aim_pend and moves the right stick as before.

The overrides need swr_logic_k (logic_rate.py), so they are emitted only with its sites. Each is checked against its
instruction word."""

MAIN_BUILD_ID = bytes.fromhex('8275FBE1EC5191C2')
MAIN_BASE = 0x8006000

YAW_SITE, YAW_WORD = 0x920CBF8, 0x1E204009      # FMOV S9, S0
PITCH_SITE, PITCH_WORD = 0x920CBFC, 0x1E20402A  # FMOV S10, S1
OBJ_PITCH, OBJ_MULT = 0x358, 0x37C              # offsets from X19, the look component

# Definitions for module.c, after logic_rate.MODULE_C (swr_logic_k). The runtime finds the exports by name.
#   swr_aim_pend[2]   runtime adds, a step takes: mouse counts, x right and y down
#   swr_aim_deg[2]    runtime: degrees per count (sensitivity, invert y included)
#   swr_aim_steps     code: steps this path took (the runtime's sign that mouse aim is live)
#   swr_aim_mult      code: the last m, logged by the runtime
MODULE_C = r'''SWR_EXPORT volatile int swr_aim_pend[2];
SWR_EXPORT volatile float swr_aim_deg[2]; SWR_EXPORT volatile u32 swr_aim_steps; SWR_EXPORT volatile float swr_aim_mult;
static float aim_p;
float swr_aim_take(float pitch, float m){
  int d[2]={__atomic_exchange_n(&swr_aim_pend[0],0,__ATOMIC_RELAXED),__atomic_exchange_n(&swr_aim_pend[1],0,__ATOMIC_RELAXED)};
  __atomic_fetch_add(&swr_aim_steps,1,__ATOMIC_RELAXED); swr_aim_mult=m;
  float k=swr_logic_k; if(!(k>0.0f)) k=1.0f;
  float p=-(float)d[1]*swr_aim_deg[1], q=p*m;   /* q: the pitch change in degrees, up positive */
  if(q>0.0f && pitch+q>89.0f) p= pitch<89.0f? (89.0f-pitch)/m : 0.0f;
  else if(q<0.0f && pitch+q<-89.0f) p= pitch>-89.0f? (-89.0f-pitch)/m : 0.0f;
  aim_p=p;
  return (float)d[0]*swr_aim_deg[0]*m/k; }
float swr_aim_pitch(void){ float p=aim_p; aim_p=0.0f; return p; }'''
CHUNK_DECL = 'float swr_aim_take(float pitch, float m);\nfloat swr_aim_pitch(void);\n'


def overrides(mods, word):
    """{guest address: C statement} for the verified build, else {}. word(a) reads the instruction at a."""
    main = next((m for m in mods if m.name == 'main'), None)
    if main is None or main.base != MAIN_BASE or main.build_id[:8] != MAIN_BUILD_ID:
        return {}
    for addr, w in ((YAW_SITE, YAW_WORD), (PITCH_SITE, PITCH_WORD)):
        have = word(addr)
        if have != w:
            raise ValueError('mouse aim: instruction at %X is %08X, expected %08X; review mouse_aim.py' % (addr, have or 0, w))
    yaw = ('{ f32 r_=c->v[0].f[0]+swr_aim_take(RDF32(c->x[19]+0x%X),RDF32(c->x[19]+0x%X)); u32 b_; memcpy(&b_,&r_,4); SETSB(9,b_); }'
           % (OBJ_PITCH, OBJ_MULT))
    pitch = '{ f32 r_=c->v[1].f[0]+swr_aim_pitch(); u32 b_; memcpy(&b_,&r_,4); SETSB(10,b_); }'
    return {YAW_SITE: yaw, PITCH_SITE: pitch}
