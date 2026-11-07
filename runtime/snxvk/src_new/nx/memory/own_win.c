/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Copyright 2026 Zorkats (the Windows traps without a mirror) */
/* SWR: memory/own.c (ScarletNX 504dc04f) for Windows, where guest memory is one private MEM_WRITE_WATCH reservation:
   no pool, so no second view of a page to land through while it stays trapped. The map of GPU-owned guest memory is
   upstream's (entries byte exact, never overlapping, the host pages under them armed); what changes is the trap and
   the landing:
   - a trap is the page made PAGE_NOACCESS, and a vectored exception handler takes the access violation on it (no
     committed guest page is NOACCESS otherwise: mem.c commits them read-write);
   - a landing reads the owners back into host memory first, then stops every other thread of the process, opens the
     pages, copies the bytes in, arms them again and lets the threads go. Only then are the pages no entry is on any
     more let go, as upstream. Nothing else runs while they are stopped, so no thread sees a page half landed, and the
     landing thread takes no lock and allocates nothing then (a stopped thread may hold the heap's);
   - an entry is whole pages only (own_begin answers -1 for anything else: it lands at its fence, as without traps):
     a page an entry is on has no CPU bytes beside it, which upstream reads through the mirror and this layer can't
     reach while the page is armed. A drop that cuts an entry inside a page leaves such bytes; own_stage then asks
     its caller to land the page (more owners than max) rather than read it.
   Locks as upstream: the producer's (vk_lock), then mem.c's, then g_trap_lk, then g_own_lk, which is never held across
   landing, a resource's release or a wait for the GPU. A thread holding the producer's lock that faults on an owned
   page ends the process (its landing would need that lock). A thread holding the kernel's lock may fault: the
   renderer's syncpoint increments under vk_lock signal kernel events only after it (snxvk_glue.c syncpt_add_quiet) */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "common/utilities/bits.h"
#include <stdlib.h>
#include <string.h>
#include "nx/memory/own.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

#define OWN_PAGE 4096ull

typedef struct {
    u64 lo, hi;   /* guest bytes */
    void* res;    /* the producer's copy, lo at res_off */
    u64 res_off;
    int landing;  /* being landed by some thread: the others wait */
} Own;

int g_swr_memory_traps = 1; /* [Graphics] vk_memory_traps (swr_settings.c): own_init arms traps only when it is 1 */
static u64 g_own_lands, g_own_stop_ns, g_own_stops;
static OwnHooks g_hooks;
static int g_traps;
static PMutex g_own_lk = PMUTEX_INIT;
static PMutex g_trap_lk = PMUTEX_INIT; /* arming and disarming pages, and the check that lets them go */
static PCond g_own_cv;
static Own* g_own;
u32 g_nown;
static u32 g_capown;
static _Thread_local int t_own_faulting; /* this thread is landing for its own fault (own_in_fault) */

#define OWN_REGION_SHIFT 22
static u32* g_own_region;
static u64 g_own_nregion;
static void cover_nl(const Own* o, u32 d) {
    if (!g_own_region || o->hi <= o->lo) return;
    for (u64 r = o->lo >> OWN_REGION_SHIFT, e = (o->hi - 1) >> OWN_REGION_SHIFT; r <= e && r < g_own_nregion; r++)
        __atomic_add_fetch(&g_own_region[r], d, __ATOMIC_RELEASE);
}
static int region_owned(u64 lo, u64 hi) {
    if (!g_own_region) return 1;
    if (hi <= lo) return 0;
    for (u64 r = lo >> OWN_REGION_SHIFT, e = (hi - 1) >> OWN_REGION_SHIFT; r <= e; r++)
        if (r >= g_own_nregion || __atomic_load_n(&g_own_region[r], __ATOMIC_ACQUIRE)) return 1;
    return 0;
}

/* ---------------------------------------------------------------- the host side: page protection, stopping threads */
static int page_arm(u64 a, u64 size) {
    DWORD old;
    return VirtualProtect(GP(a), (SIZE_T)size, PAGE_NOACCESS, &old) ? 0 : -1;
}
static void page_open(u64 a, u64 size) {
    DWORD old;
    if (VirtualProtect(GP(a), (SIZE_T)size, PAGE_READWRITE, &old)) return;
    for (u64 o = 0; o < size; o += OWN_PAGE) VirtualProtect(GP(a + o), OWN_PAGE, PAGE_READWRITE, &old); /* mixed pages */
}
/* whether the page at a is committed and armed (PAGE_NOACCESS): 1, or committed and open: 0, or not committed: -1 */
static int page_armed(u64 a) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(GP(a), &mi, sizeof mi) || mi.State != MEM_COMMIT) return -1;
    return (mi.Protect & 0xff) == PAGE_NOACCESS;
}

typedef LONG(NTAPI* NextThreadFn)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
static NextThreadFn g_next_thread;
#define STOP_MAX 4096
static HANDLE g_stopped[STOP_MAX];
static DWORD g_stopped_tid[STOP_MAX];
static u32 g_nstopped;
static int stopped_has(DWORD tid) {
    for (u32 i = 0; i < g_nstopped; i++)
        if (g_stopped_tid[i] == tid) return 1;
    return 0;
}
static void world_go(void) {
    for (u32 i = 0; i < g_nstopped; i++) {
        ResumeThread(g_stopped[i]);
        CloseHandle(g_stopped[i]);
    }
    g_nstopped = 0;
}
/* every other thread of the process suspended, and stopped (GetThreadContext waits for that): passes over the
   process's threads until one finds none new, so a thread made meanwhile is stopped too. Called by a landing, under
   the producer's lock, so never by two threads at once. 0, or -1 with every thread let go (too many) */
static int world_stop(void) {
    DWORD self = GetCurrentThreadId();
    int added;
    u64 t0 = plat_ns();
    g_nstopped = 0;
    do {
        HANDLE h = NULL, next;
        int kept = 0;
        added = 0;
        while (!g_next_thread(GetCurrentProcess(), h, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                                          THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &next)) {
            if (h && !kept) CloseHandle(h);
            h = next;
            kept = 0;
            DWORD tid = GetThreadId(next);
            if (tid == self || stopped_has(tid)) continue;
            if (g_nstopped == STOP_MAX) {
                CloseHandle(h);
                world_go();
                return -1;
            }
            if (SuspendThread(next) == (DWORD)-1) continue; /* ending */
            g_stopped[g_nstopped] = next;
            g_stopped_tid[g_nstopped++] = tid;
            kept = 1;
            added = 1;
        }
        if (h && !kept) CloseHandle(h);
    } while (added);
    for (u32 i = 0; i < g_nstopped; i++) {
        CONTEXT c __attribute__((aligned(16)));
        c.ContextFlags = CONTEXT_CONTROL;
        GetThreadContext(g_stopped[i], &c);
    }
    __atomic_add_fetch(&g_own_stop_ns, plat_ns() - t0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_own_stops, 1, __ATOMIC_RELAXED);
    return 0;
}

/* ---------------------------------------------------------------- the entries, under g_own_lk */
static u32 first_after_nl(u64 a) {
    u32 lo = 0, hi = g_nown;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_own[mid].hi <= a)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
static void insert_nl(u32 i, const Own* o) {
    if (g_nown == g_capown) {
        u32 cap = g_capown ? g_capown * 2 : 64;
        Own* n = (Own*)realloc(g_own, sizeof *n * (size_t)cap);
        if (!n) fatal("Out of memory for the map of GPU-owned memory");
        g_own = n;
        g_capown = cap;
    }
    memmove(&g_own[i + 1], &g_own[i], sizeof *g_own * (size_t)(g_nown - i));
    g_own[i] = *o;
    cover_nl(o, 1);
    __atomic_store_n(&g_nown, g_nown + 1, __ATOMIC_RELEASE);
}
static void remove_nl(u32 i) {
    cover_nl(&g_own[i], (u32)-1);
    memmove(&g_own[i], &g_own[i + 1], sizeof *g_own * (size_t)(g_nown - i - 1));
    __atomic_store_n(&g_nown, g_nown - 1, __ATOMIC_RELEASE);
}
static int page_owned_nl(u64 page) {
    u32 i = first_after_nl(page);
    return i < g_nown && g_own[i].lo < page + OWN_PAGE;
}
static int landing_nl(u64 lo, u64 hi) {
    for (u32 i = first_after_nl(lo); i < g_nown && g_own[i].lo < hi; i++)
        if (g_own[i].landing) return 1;
    return 0;
}

typedef struct {
    void** res;
    u32 n, cap;
} ResList;
static void res_push(ResList* l, void* res) {
    if (l->n == l->cap) {
        u32 cap = l->cap ? l->cap * 2 : 8;
        void** r = (void**)realloc(l->res, sizeof *r * (size_t)cap);
        if (!r) fatal("Out of memory for the map of GPU-owned memory");
        l->res = r;
        l->cap = cap;
    }
    l->res[l->n++] = res;
}
static void res_release(ResList* l) {
    for (u32 i = 0; i < l->n; i++) g_hooks.release(l->res[i]);
    free(l->res);
}

static void cut_nl(u64 lo, u64 hi, ResList* rel) {
    u32 i = first_after_nl(lo);
    while (i < g_nown && g_own[i].lo < hi) {
        Own* o = &g_own[i];
        if (o->lo < lo && o->hi > hi) {
            Own right = *o;
            right.res_off += hi - o->lo;
            right.lo = hi;
            cover_nl(o, (u32)-1);
            o->hi = lo;
            cover_nl(o, 1);
            g_hooks.retain(right.res);
            insert_nl(i + 1, &right);
            return;
        }
        if (o->lo < lo) {
            cover_nl(o, (u32)-1);
            o->hi = lo;
            cover_nl(o, 1);
            i++;
        } else if (o->hi > hi) {
            o->res_off += hi - o->lo;
            cover_nl(o, (u32)-1);
            o->lo = hi;
            cover_nl(o, 1);
            return;
        } else {
            res_push(rel, o->res);
            remove_nl(i);
        }
    }
}

/* the pages [lo, hi) no entry is on any more opened (page aligned) */
static void let_go(u64 lo, u64 hi) {
    u64 a = lo;
    pm_lock(&g_trap_lk);
    while (a < hi) {
        u64 run;
        int free_page;
        pm_lock(&g_own_lk);
        free_page = !page_owned_nl(a);
        pm_unlock(&g_own_lk);
        if (!free_page) {
            a += OWN_PAGE;
            continue;
        }
        for (run = a + OWN_PAGE; run < hi; run += OWN_PAGE) {
            int owned;
            pm_lock(&g_own_lk);
            owned = page_owned_nl(run);
            pm_unlock(&g_own_lk);
            if (owned) break;
        }
        page_open(a, run - a);
        a = run;
    }
    pm_unlock(&g_trap_lk);
}

static int owned_nl(u64 lo, u64 hi) {
    u32 i = first_after_nl(lo);
    return i < g_nown && g_own[i].lo < hi;
}
static void producer_lock(void) {
    if (g_hooks.lock) g_hooks.lock();
}
static void producer_unlock(void) {
    if (g_hooks.unlock) g_hooks.unlock();
}

/* every entry on the pages [lo, hi) landed and let go, as upstream's land_pages. The owners' bytes are read back into
   host memory (whole pages each, its pages' span), then copied in with every other thread stopped */
static void land_pages(u64 lo, u64 hi) {
    OwnRange* r;
    u8** dst;
    u8** span;
    u64 elo = lo, ehi = hi;
    u32 i, first, n = 0;
    int any;
    ResList rel = {0};
    if (!region_owned(lo, hi)) return;
    pm_lock(&g_own_lk);
    any = owned_nl(lo, hi);
    pm_unlock(&g_own_lk);
    if (!any) return;
    producer_lock();
    pm_lock(&g_own_lk);
    while (landing_nl(lo, hi)) pc_wait(&g_own_cv, &g_own_lk, -1);
    first = first_after_nl(lo);
    for (i = first; i < g_nown && g_own[i].lo < hi; i++) n++;
    if (!n) {
        pm_unlock(&g_own_lk);
        producer_unlock();
        return;
    }
    while (first && g_own[first - 1].res == g_own[first].res && g_own[first - 1].hi == g_own[first].lo &&
           !g_own[first - 1].landing)
        first--, n++;
    while (first + n < g_nown && g_own[first + n].res == g_own[first + n - 1].res &&
           g_own[first + n].lo == g_own[first + n - 1].hi && !g_own[first + n].landing)
        n++;
    r = (OwnRange*)malloc(sizeof *r * n);
    dst = (u8**)malloc(sizeof *dst * n);
    span = (u8**)calloc(n, sizeof *span);
    if (!r || !dst || !span) fatal("Out of memory landing GPU-owned memory");
    for (i = 0; i < n; i++) {
        Own* o = &g_own[first + i];
        o->landing = 1;
        r[i].addr = o->lo;
        r[i].size = o->hi - o->lo;
        r[i].res = o->res;
        r[i].res_off = o->res_off;
        if (o->lo < elo) elo = o->lo;
        if (o->hi > ehi) ehi = o->hi;
    }
    pm_unlock(&g_own_lk);
    for (i = 0; i < n; i++) { /* each range's pages, its bytes at their place in them */
        u64 plo = r[i].addr & ~(OWN_PAGE - 1), phi = (r[i].addr + r[i].size + OWN_PAGE - 1) & ~(OWN_PAGE - 1);
        if (!(span[i] = (u8*)malloc((size_t)(phi - plo)))) fatal("Out of memory landing GPU-owned memory");
        dst[i] = span[i] + (r[i].addr - plo);
    }
    g_hooks.land(r, dst, n);
    __atomic_add_fetch(&g_own_lands, 1, __ATOMIC_RELAXED);
    /* in with every other thread stopped: the pages opened, the bytes copied, and armed again (some may stay owned
       by entries beside these); the bytes beside a range on its pages read into its span after all are in, so the
       span is its pages as the landing left them */
    if (world_stop()) fatal("Too many threads to land GPU-owned memory");
    for (i = 0; i < n; i++) {
        u64 plo = r[i].addr & ~(OWN_PAGE - 1), phi = (r[i].addr + r[i].size + OWN_PAGE - 1) & ~(OWN_PAGE - 1);
        DWORD old;
        if (!VirtualProtect(GP(plo), (SIZE_T)(phi - plo), PAGE_READWRITE, &old)) {
            world_go();
            fatal("Could not open GPU-owned guest memory at %llx", (unsigned long long)plo);
        }
    }
    for (i = 0; i < n; i++) memcpy(GP(r[i].addr), dst[i], (size_t)r[i].size);
    for (i = 0; i < n; i++) {
        u64 plo = r[i].addr & ~(OWN_PAGE - 1), phi = (r[i].addr + r[i].size + OWN_PAGE - 1) & ~(OWN_PAGE - 1);
        u64 end = r[i].addr + r[i].size;
        memcpy(span[i], GP(plo), (size_t)(r[i].addr - plo));
        memcpy(span[i] + (end - plo), GP(end), (size_t)(phi - end));
    }
    for (i = 0; i < n; i++) {
        u64 plo = r[i].addr & ~(OWN_PAGE - 1), phi = (r[i].addr + r[i].size + OWN_PAGE - 1) & ~(OWN_PAGE - 1);
        page_arm(plo, phi - plo);
    }
    world_go();
    pm_lock(&g_own_lk);
    for (i = 0; i < n; i++) cut_nl(r[i].addr, r[i].addr + r[i].size, &rel);
    pc_broadcast(&g_own_cv);
    pm_unlock(&g_own_lk);
    let_go(elo & ~(OWN_PAGE - 1), (ehi + OWN_PAGE - 1) & ~(OWN_PAGE - 1));
    /* the landed pages let go, with the bytes the landing left on them (hashed from the span: a CPU write since
       changes them, and counts): the write tracking reports the landing's own writes, which aren't the CPU's. A page
       still owned beside them stays armed and gets no note (its report counts) */
    if (g_hooks.landed)
        for (i = 0; i < n; i++) {
            u64 plo = r[i].addr & ~(OWN_PAGE - 1), phi = (r[i].addr + r[i].size + OWN_PAGE - 1) & ~(OWN_PAGE - 1);
            for (u64 a = plo; a < phi;) {
                u64 e = a;
                pm_lock(&g_own_lk);
                while (e < phi && !page_owned_nl(e)) e += OWN_PAGE;
                pm_unlock(&g_own_lk);
                if (e > a) g_hooks.landed(a, e - a, span[i] + (a - plo));
                a = e + (e < phi ? OWN_PAGE : 0);
            }
        }
    for (i = 0; i < n; i++) free(span[i]);
    free(span);
    free(r);
    free(dst);
    res_release(&rel);
    producer_unlock();
}

/* ---------------------------------------------------------------- traps */
/* the access violation at p: 1 when it is answered (the access goes again), 0 when it isn't ours */
static int own_fault(u8* p, int write, u32 tid) {
    u64 a = (u64)(p - g_mem), page = a & ~(OWN_PAGE - 1);
    int owned, armed;
    pm_lock(&g_trap_lk);
    pm_lock(&g_own_lk);
    owned = page_owned_nl(page);
    pm_unlock(&g_own_lk);
    /* no entry on it: landed meanwhile, or cut and not let go yet (opened here, as upstream's plat_trap_continue), or
       a fault of the guest's own (not committed): not ours */
    armed = owned ? 1 : page_armed(page);
    if (!owned && armed == 1) page_open(page, OWN_PAGE);
    pm_unlock(&g_trap_lk);
    if (!owned) return armed >= 0;
    if (tid && g_hooks.busy_tid && tid == g_hooks.busy_tid())
        fatal("Thread %u %s GPU-owned guest memory at %llx while it held the renderer: the runtime must call "
              "own_acquire first",
              tid, write ? "wrote" : "read", (unsigned long long)a);
    t_own_faulting++;
    land_pages(page, page + OWN_PAGE);
    t_own_faulting--;
    return 1;
}
static LONG CALLBACK own_veh(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* x = e->ExceptionRecord;
    u8* p;
    /* a read (0) or a write (1); an execution (8) is no trap's */
    if (x->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || x->NumberParameters < 2 || !g_traps ||
        x->ExceptionInformation[0] > 1)
        return EXCEPTION_CONTINUE_SEARCH;
    p = (u8*)x->ExceptionInformation[1];
    if (p < g_mem || p >= GP(AS_SIZE)) return EXCEPTION_CONTINUE_SEARCH;
    return own_fault(p, x->ExceptionInformation[0] == 1, (u32)GetCurrentThreadId()) ? EXCEPTION_CONTINUE_EXECUTION
                                                                                    : EXCEPTION_CONTINUE_SEARCH;
}
int own_in_fault(void) { return t_own_faulting; }

int own_init(const OwnHooks* h) {
    g_hooks = *h;
    pc_init(&g_own_cv);
    if (!g_swr_memory_traps) return -1; /* GPU engines land their results at their fence */
    g_next_thread = (NextThreadFn)(void*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtGetNextThread");
    if (!g_next_thread || !AddVectoredExceptionHandler(1, own_veh)) {
        LOG("memory: vk_memory_traps=1, but this Windows can't trap: GPU engines land their results at their fence\n");
        return -1;
    }
    g_own_nregion = (AS_SIZE + (1ull << OWN_REGION_SHIFT) - 1) >> OWN_REGION_SHIFT;
    g_own_region = (u32*)calloc((size_t)g_own_nregion, sizeof *g_own_region);
    g_traps = 1;
    LOG("memory: GPU engines keep their results (vk_memory_traps=1), guest memory is trapped until the CPU needs it\n");
    return 0;
}

int own_begin(u64 addr, u64 size, void* res, u64 res_off) {
    u64 end = addr + size;
    ResList rel = {0};
    Own o;
    if (!g_traps || !size || addr >= AS_SIZE || size > AS_SIZE - addr || ((addr | size) & (OWN_PAGE - 1))) return -1;
    /* committed (an armed page here is an entry's, which the cut below ends), a region of pages at a time */
    for (u64 a = addr; a < end;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(GP(a), &mi, sizeof mi) || mi.State != MEM_COMMIT) return -1;
        a = (u64)((u8*)mi.BaseAddress + mi.RegionSize - g_mem);
    }
    pm_lock(&g_own_lk);
    while (landing_nl(addr, end)) pc_wait(&g_own_cv, &g_own_lk, -1);
    cut_nl(addr, end, &rel);
    o.lo = addr;
    o.hi = end;
    o.res = res;
    o.res_off = res_off;
    o.landing = 0;
    g_hooks.retain(res);
    insert_nl(first_after_nl(addr), &o);
    pm_unlock(&g_own_lk);
    res_release(&rel);
    pm_lock(&g_trap_lk);
    if (page_arm(addr, size)) fatal("Could not trap guest memory at %llx", (unsigned long long)addr);
    pm_unlock(&g_trap_lk);
    return 0;
}

u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max) {
    u64 end = addr + size;
    u32 i, n = 0;
    if (!own_any() || !region_owned(addr, end)) return 0;
    pm_lock(&g_own_lk);
    for (i = first_after_nl(addr); i < g_nown && g_own[i].lo < end; i++, n++)
        if (n < max) {
            const Own* o = &g_own[i];
            u64 lo = o->lo > addr ? o->lo : addr, hi = o->hi < end ? o->hi : end;
            out[n].addr = lo;
            out[n].size = hi - lo;
            out[n].res = o->res;
            out[n].res_off = o->res_off + (lo - o->lo);
        }
    pm_unlock(&g_own_lk);
    return n;
}

/* the gaps between the owned parts copied as they are; a gap on a page an entry is on (a drop cut it) can't be read
   while armed: max + 1, nothing copied, and the caller lands the range (own_acquire) and reads it */
u32 own_stage(u64 addr, u64 size, u8* dst, OwnRange* out, u32 max) {
    u64 at = addr;
    u32 n = own_lookup(addr, size, out, max), i;
    if (n > max) return n;
    if (!n && !region_owned(addr & ~(OWN_PAGE - 1), (addr + size + OWN_PAGE - 1) & ~(OWN_PAGE - 1))) {
        memcpy(dst, GP(addr), (size_t)size);
        return 0;
    }
    pm_lock(&g_own_lk);
    for (i = 0; i <= n; i++) {
        u64 end = i < n ? out[i].addr : addr + size;
        for (u64 p = at & ~(OWN_PAGE - 1); p < end; p += OWN_PAGE)
            if (page_owned_nl(p)) {
                pm_unlock(&g_own_lk);
                return max + 1;
            }
        if (i < n) at = out[i].addr + out[i].size;
    }
    pm_unlock(&g_own_lk);
    /* none of the gaps' pages is owned, and none can be armed while the caller holds the producer's lock (own_begin's
       callers hold it, and a landing lets its pages go before it lets go of the lock) */
    at = addr;
    for (i = 0; i <= n; i++) {
        u64 end = i < n ? out[i].addr : addr + size;
        if (end > at) memcpy(dst + (at - addr), GP(at), (size_t)(end - at));
        if (i < n) at = out[i].addr + out[i].size;
    }
    return n;
}

int own_hash(u64 addr, u64 size, u64* h) {
    u64 lo = addr & ~(OWN_PAGE - 1), hi = (addr + size + OWN_PAGE - 1) & ~(OWN_PAGE - 1);
    int any = 0;
    if (!size || !own_any() || !region_owned(lo, hi)) return 0;
    pm_lock(&g_own_lk);
    for (u32 i = first_after_nl(lo); i < g_nown && g_own[i].lo < hi; i++) {
        const Own* o = &g_own[i];
        u64 v[4] = {o->lo, o->hi, (u64)(uintptr_t)o->res, o->res_off};
        for (u32 k = 0; k < 4; k++) *h = snx_fnv_step(*h, v[k]);
        any = 1;
    }
    pm_unlock(&g_own_lk);
    return any;
}

void own_acquire(u64 addr, u64 size) {
    if (!size || !own_any()) return;
    land_pages(addr & ~(OWN_PAGE - 1), (addr + size + OWN_PAGE - 1) & ~(OWN_PAGE - 1));
}

void own_drop(u64 addr, u64 size) {
    ResList rel = {0};
    if (!size || !own_any()) return;
    pm_lock(&g_own_lk);
    while (landing_nl(addr, addr + size)) pc_wait(&g_own_cv, &g_own_lk, -1);
    u32 first = first_after_nl(addr);
    if (first == g_nown || g_own[first].lo >= addr + size) {
        pm_unlock(&g_own_lk);
        return;
    }
    u64 lo = g_own[first].lo > addr ? g_own[first].lo : addr, hi = g_own[first].hi;
    for (u32 i = first + 1; i < g_nown && g_own[i].lo < addr + size; i++) hi = g_own[i].hi;
    if (hi > addr + size) hi = addr + size;
    cut_nl(addr, addr + size, &rel);
    pm_unlock(&g_own_lk);
    let_go(lo & ~(OWN_PAGE - 1), (hi + OWN_PAGE - 1) & ~(OWN_PAGE - 1));
    res_release(&rel);
}

/* SWR: [addr, addr + size) copied to dst when none of its pages is armed (checked and copied under g_trap_lk, so
   none is armed or let go meanwhile): 0; else -1, nothing copied. For a reader that may not land (the write tracking,
   which holds its own locks) */
int own_read_open(u64 addr, u64 size, u8* dst) {
    int r = 0;
    if (!size) return 0;
    pm_lock(&g_trap_lk);
    for (u64 p = addr & ~(OWN_PAGE - 1); p < addr + size && !r; p += OWN_PAGE)
        if (page_armed(p) != 0) r = -1;
    if (!r) memcpy(dst, GP(addr), (size_t)size);
    pm_unlock(&g_trap_lk);
    return r;
}
u64 own_landings(void) { return __atomic_load_n(&g_own_lands, __ATOMIC_RELAXED); }
/* SWR: the threads stopped for landings so far, and the time it took them (for the statistics) */
void own_stop_stats(u64* stops, u64* ns) {
    *stops = __atomic_load_n(&g_own_stops, __ATOMIC_RELAXED);
    *ns = __atomic_load_n(&g_own_stop_ns, __ATOMIC_RELAXED);
}
int own_traps(void) { return g_traps; }
