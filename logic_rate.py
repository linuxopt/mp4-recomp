# Copyright 2026 Zorkats. Apache-2.0, as SwitchRecomp.
"""Game logic at any frame rate for Metroid Prime 4 Beyond 2.1.0 (main build 8275FBE1EC5191C2).

The game steps its logic by a fixed time per presented frame, step = flt_93C5400[mode] (mode 0: 1/60 s, mode 1: 1/30 s;
F_8E49AAC). Code that depends on the step reads the mode flag dword_982CD70 (the main loop F_8985E58 sets it to 1, the
60 fps mode, every frame) and picks one of two values: the step itself, a time in milliseconds, a count of sixtieths
used as a powf exponent (frame-rate independent smoothing), or a count of frames. Every one of the 78 flag reads is a
`cmp w, #0` whose result feeds exactly one FCSEL or CSET; the CSET indexes a two-entry table.

These overrides replace the consumer instructions so the chosen value follows k = sixtieths per step, exported by the
code module as swr_logic_k (1.0 unless the runtime sets SWR_LOGIC_HZ). swr_logic_val(site, v60, v30) returns v60 * k for
pairs where the 30 fps value is twice the 60 fps one (times, exponents), v60 / k where it is half (frame counts), and v60
otherwise. With k = 1 every site gives the value the game picks today. The FDIV sites divide by k right after an FCSEL
(1 - powf(1 - a/k, k)): with k < 1 they keep the numerator, giving 1 - (1 - a)^k, so the base stays in [0, 1].

Sites come from IDA cross references to dword_982CD70; each is checked against its instruction word."""

MAIN_BUILD_ID = bytes.fromhex('8275FBE1EC5191C2')
MAIN_BASE = 0x8006000
STEP_TABLE = 0x93C5400

SITES = [
    (0x801E294, 0x1E280C21, 'fcsel'), (0x802A880, 0xBC6B5940, 'ldr'), (0x802E650, 0xBC685928, 'ldr'),
    (0x807F58C, 0xBC685928, 'ldr'), (0x809B3E4, 0x1E220C21, 'fcsel'), (0x8176250, 0x1E2C0C01, 'fcsel'),
    (0x81BF87C, 0x0D408106, 'ld1'), (0x81D5B64, 0x1E280C21, 'fcsel'), (0x81F853C, 0xBC685920, 'ldr'),
    (0x81FF410, 0x1E2C0C01, 'fcsel'), (0x820D44C, 0x1E2E0C21, 'fcsel'), (0x8242C8C, 0xBC685928, 'ldr'),
    (0x8257684, 0x1E2A0C21, 'fcsel'), (0x8257688, 0x1E211942, 'fdiv'), (0x825EEC8, 0x1E2B0C21, 'fcsel'),
    (0x826BF4C, 0xBC685920, 'ldr'), (0x8292AF4, 0x1E2B0C21, 'fcsel'), (0x82B0B68, 0xBC695902, 'ldr'),
    (0x8304CB4, 0x0D408101, 'ld1'), (0x831C9BC, 0x1E280C21, 'fcsel'), (0x831C9C0, 0x1E211902, 'fdiv'),
    (0x8326D0C, 0xBC685AA1, 'ldr'), (0x8326DE8, 0xBC695AA1, 'ldr'), (0x836FCC8, 0x1E2E0C21, 'fcsel'),
    (0x83CC9A8, 0xBC695901, 'ldr'), (0x8421F2C, 0xBC685921, 'ldr'), (0x84267A8, 0x1E2B0C21, 'fcsel'),
    (0x84267AC, 0x1E2119C2, 'fdiv'), (0x845D234, 0x1E390DE2, 'fcsel'), (0x845D378, 0x1E390DE3, 'fcsel'),
    (0x845D764, 0x1E390DE3, 'fcsel'), (0x845DA44, 0x1E390DE3, 'fcsel'), (0x845E290, 0x1E390DE3, 'fcsel'),
    (0x845EB48, 0x1E220D42, 'fcsel'), (0x84E53E4, 0xBC685928, 'ldr'), (0x850EE1C, 0xBC68592A, 'ldr'),
    (0x8596F5C, 0x1E230C21, 'fcsel'), (0x8596F64, 0x1E211922, 'fdiv'), (0x85FAAAC, 0x1E2A0C01, 'fcsel'),
    (0x85FAAB0, 0x1E211940, 'fdiv'), (0x8640694, 0x1E2B0C21, 'fcsel'), (0x8642FB0, 0x1E280C01, 'fcsel'),
    (0x868CCC8, 0xBC695900, 'ldr'), (0x86E4F58, 0x1E2B0D82, 'fcsel'), (0x86E4F78, 0x1E2B0D81, 'fcsel'),
    (0x86E88AC, 0xBC695900, 'ldr'), (0x8771E00, 0x1E280C21, 'fcsel'), (0x8771E04, 0x1E211902, 'fdiv'),
    (0x88A4078, 0xBC695900, 'ldr'), (0x88C1674, 0x1E2E0C21, 'fcsel'), (0x88C1BC4, 0x1E2E0C01, 'fcsel'),
    (0x88C43B0, 0x1E2B0C01, 'fcsel'), (0x88C483C, 0x1E290C01, 'fcsel'), (0x88E0CB0, 0xBC685920, 'ldr'),
    (0x891DD64, 0x1E230C42, 'fcsel'), (0x897BFB8, 0xBC685920, 'ldr'), (0x8986938, 0xBC685920, 'ldr'),
    (0x89EA210, 0x1E200C20, 'fcsel'), (0x8A902DC, 0xBC6A5901, 'ldr'), (0x8BD4128, 0xBC685940, 'ldr'),
    (0x8CC0B48, 0x1E2B0C21, 'fcsel'), (0x8E49B30, 0xBC685AC0, 'ldr'), (0x8E49C00, 0xBC685AC9, 'ldr'),
    (0x8EF0B9C, 0xBC6A5920, 'ldr'), (0x8F02864, 0x1E280C21, 'fcsel'), (0x8F028AC, 0x1E2A0C01, 'fcsel'),
    (0x8F028F4, 0x1E210C41, 'fcsel'), (0x8FCBF7C, 0xBC685920, 'ldr'), (0x8FE22E8, 0x1E2F0D01, 'fcsel'),
    (0x8FE241C, 0x1E2F0D01, 'fcsel'), (0x8FE2550, 0x1E2F0D01, 'fcsel'), (0x90D5774, 0x1E290C01, 'fcsel'),
    (0x90D5BA4, 0x1E2B0C01, 'fcsel'), (0x91515B8, 0x1E290C21, 'fcsel'), (0x91E41F4, 0xBC685920, 'ldr'),
    (0x91E45AC, 0xBC695900, 'ldr'), (0x91E49F8, 0xBC685928, 'ldr'), (0x91E73B8, 0xBC755900, 'ldr'),
    (0x920DB90, 0xBC685923, 'ldr'), (0x922ACC0, 0x1E290C21, 'fcsel'), (0x92530EC, 0xBC695940, 'ldr'),
    (0x925DF9C, 0xBC6A5966, 'ldr'), (0x92F3FE0, 0x1E280C01, 'fcsel'), (0x936412C, 0x1E290C01, 'fcsel'),
]
LD1_TABLE = {0x81BF87C: STEP_TABLE, 0x8304CB4: STEP_TABLE}   # the CSET index was folded into the address

# Definitions for module.c. The runtime finds swr_logic_k by name and logs the sites swr_logic_unknown lists.
MODULE_C = r'''SWR_EXPORT float swr_logic_k=1.0f;
SWR_EXPORT volatile u64 swr_logic_unknown[32]; SWR_EXPORT volatile u32 swr_logic_unknown_n;
float swr_logic_val(u64 site, float v60, float v30){
  float k=swr_logic_k; if(k==1.0f) return v60;
  float d=v30-2.0f*v60, e=v60-2.0f*v30, t=1e-5f*(v30<0? -v30 : v30), u=1e-5f*(v60<0? -v60 : v60);
  if((d<0? -d : d)<=t) return v60*k;
  if((e<0? -e : e)<=u) return v60/k;
  for(u32 i=0;i<swr_logic_unknown_n && i<32;i++) if(swr_logic_unknown[i]==site) return v60;
  u32 n=__atomic_fetch_add(&swr_logic_unknown_n,1,__ATOMIC_RELAXED); if(n<32) swr_logic_unknown[n]=site;
  return v60; }'''
CHUNK_DECL = 'float swr_logic_val(u64 site, float v60, float v30);\n'


def _f(w, lo):
    return (w >> lo) & 31


def _c(addr, w, kind):
    if kind == 'fcsel':   # FCSEL Sd, Sn (30 fps value), Sm (60 fps value), EQ (EQ: flag == 0)
        assert w & 0xFFE0FC00 == 0x1E200C00, hex(addr)
        d, n, m = _f(w, 0), _f(w, 5), _f(w, 16)
        return ('{ f32 r_= c->z? c->v[%d].f[0] : swr_logic_val(0x%XULL,c->v[%d].f[0],c->v[%d].f[0]); u32 b_; memcpy(&b_,&r_,4); SETSB(%d,b_); }'
                % (n, addr, m, n, d))
    if kind == 'ldr':     # LDR St, [Xn, Wm, UXTW #2]: Wm is the CSET (1 in the 30 fps mode)
        assert w & 0xFFE0FC00 == 0xBC605800, hex(addr)
        t, n, m = _f(w, 0), _f(w, 5), _f(w, 16)
        assert n != 31 and m != 31, hex(addr)
        return ('{ u64 a_=c->x[%d]; u32 i_=(u32)c->x[%d]; f32 r_= i_? RDF32(a_+((u64)i_<<2)) : swr_logic_val(0x%XULL,RDF32(a_),RDF32(a_+4)); '
                'u32 b_; memcpy(&b_,&r_,4); SETSB(%d,b_); }' % (n, m, addr, t))
    if kind == 'ld1':     # LD1 {Vt.S}[0], [Xn], Xn = table + 4 * CSET
        assert w & 0xFFFFFC00 == 0x0D408000, hex(addr)
        t, n = _f(w, 0), _f(w, 5)
        assert n != 31, hex(addr)
        tab = LD1_TABLE[addr]
        return ('{ u64 a_=c->x[%d]; f32 r_= a_!=0x%XULL? RDF32(a_) : swr_logic_val(0x%XULL,RDF32(0x%XULL),RDF32(0x%XULL)); memcpy(&c->v[%d].s[0],&r_,4); }'
                % (n, tab, addr, tab, tab + 4, t))
    if kind == 'fdiv':    # FDIV Sd, Sn (+-1.0), Sm (k from the FCSEL just before)
        assert w & 0xFFE0FC00 == 0x1E201800, hex(addr)
        d, n, m = _f(w, 0), _f(w, 5), _f(w, 16)
        return ('{ f32 m_=c->v[%d].f[0], r_= m_<1.0f? c->v[%d].f[0] : c->v[%d].f[0]/m_; u32 b_; memcpy(&b_,&r_,4); SETSB(%d,b_); }'
                % (m, n, n, d))
    raise ValueError(kind)


def overrides(mods, word):
    """{guest address: C statement} for the verified build, else {}. word(a) reads the instruction at a."""
    main = next((m for m in mods if m.name == 'main'), None)
    if main is None or main.base != MAIN_BASE or main.build_id[:8] != MAIN_BUILD_ID:
        return {}
    out = {}
    for addr, w, kind in SITES:
        have = word(addr)
        if have != w:
            raise ValueError('logic rate: instruction at %X is %08X, expected %08X; review logic_rate.py' % (addr, have or 0, w))
        out[addr] = _c(addr, w, kind)
    return out
