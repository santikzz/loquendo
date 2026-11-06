#include <stdio.h>
#include <stdlib.h>

#include "runtime.h"

uint8_t *M;
int trace_on;
char data_dir[512], out_dir[512];
uint32_t g_user_text_ptr;

static Thread *threads[MAX_THREADS];
static int thread_count;
static uint8_t image_ready[16];

#define SMALL_CLASSES 4096
static uint32_t brk_ = HEAP_BASE;
static uint32_t small_free[SMALL_CLASSES];
static uint32_t big_free;

static uint32_t bump(uint32_t size, uint32_t alignment) {
    uint32_t a = (brk_ + alignment - 1) & ~(alignment - 1);
    if (a + size < a || a + size > ARENA_SIZE) {
        fprintf(stderr, "guest heap exhausted\n");
        exit(3);
    }
    brk_ = a + size;
    return a;
}

uint32_t heap_alloc(uint32_t size, int zero) {
    uint32_t need = (size + 15u) & ~15u;
    if (!need) need = 16;
    uint32_t a = 0;
    plat_lock();
    if (need / 16 < SMALL_CLASSES) {
        uint32_t *head = &small_free[need / 16];
        if (*head) { a = *head; *head = rd32(a); }
    } else {
        for (uint32_t prev = 0, b = big_free; b; prev = b, b = rd32(b)) {
            if (rd32(b - 16) != need) continue;
            if (prev) wr32(prev, rd32(b)); else big_free = rd32(b);
            a = b;
            break;
        }
    }
    int reused = a != 0;
    if (!a) {
        a = bump(need + 16, 16) + 16;
        wr32(a - 16, need);
    }
    plat_unlock();
    if (zero && reused) memset(M + a, 0, need);
    return a;
}

uint32_t heap_alloc_aligned(uint32_t size, uint32_t alignment) {
    plat_lock();
    uint32_t a = bump((size + 0xFFFu) & ~0xFFFu, alignment);
    plat_unlock();
    return a;
}

uint32_t heap_size(uint32_t a) {
    return a >= HEAP_BASE + 16 && a < brk_ ? rd32(a - 16) : 0;
}

void heap_free(uint32_t a) {
    uint32_t need = heap_size(a);
    if (!need) return;
    plat_lock();
    if (need / 16 < SMALL_CLASSES) {
        wr32(a, small_free[need / 16]);
        small_free[need / 16] = a;
    } else {
        wr32(a, big_free);
        big_free = a;
    }
    plat_unlock();
}

uint32_t guest_str(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t a = heap_alloc(n, 0);
    memcpy(M + a, s, n);
    return a;
}

Thread *thread_new(void) {
    plat_lock();
    if (thread_count >= MAX_THREADS) {
        fprintf(stderr, "too many guest threads\n");
        exit(3);
    }
    Thread *t = calloc(1, sizeof *t);
    t->index = thread_count;
    threads[thread_count++] = t;
    plat_unlock();
    t->tid = 100 + 4u * (uint32_t)t->index;
    t->cpu.thread = t;
    t->cpu.fcw = 0x27F; 
    t->cpu.fs_base = TEB_BASE + 0x1000u * (uint32_t)t->index;
    uint32_t stack = STACK_BASE + STACK_SIZE * (uint32_t)t->index;
    t->cpu.esp = stack + STACK_SIZE - 64;
    wr32(t->cpu.fs_base + 0x00, INVALID);              
    wr32(t->cpu.fs_base + 0x04, stack + STACK_SIZE);   
    wr32(t->cpu.fs_base + 0x08, stack);                
    wr32(t->cpu.fs_base + 0x18, t->cpu.fs_base);       
    wr32(t->cpu.fs_base + 0x24, t->tid);
    wr32(t->cpu.fs_base + 0x30, TEB_BASE + 0xF000u);   
    return t;
}

static int find_func(uint32_t addr) {
    int lo = 0, hi = (int)func_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (func_addr[mid] == addr) return mid;
        if (func_addr[mid] < addr) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

void call_indirect(CPU *c, uint32_t target) {
    if (target >= SHIM_BASE && target < SHIM_BASE + 16u * (import_count + extra_shim_count) && !(target & 15u)) {
        uint32_t idx = (target - SHIM_BASE) / 16u;
        if (idx < import_count) call_import(c, idx);
        else call_extra_shim(c, idx - import_count);
        return;
    }
    int i = find_func(target);
    if (i < 0) trap(c, target, "indirect call to unknown address");
    func_ptr[i](c);
}

uint32_t guest_call(CPU *c, uint32_t fn, int nargs, const uint32_t *args) {
    uint32_t saved = c->esp;
    c->esp -= 4u * (uint32_t)nargs;
    for (int i = 0; i < nargs; i++) wr32(c->esp + 4u * (uint32_t)i, args[i]);
    c->esp -= 4;
    wr32(c->esp, 0);
    call_indirect(c, fn);
    c->esp = saved;
    return c->eax;
}

void trap(CPU *c, uint32_t addr, const char *what) {
    fprintf(stderr, "trap: %s at 0x%08X (thread %u)\n", what, addr, c->thread ? c->thread->tid : 0);
    exit(3);
}

uint32_t atomic_add32(uint32_t a, uint32_t delta) {
    plat_lock();
    uint32_t v = rd32(a) + delta;
    wr32(a, v);
    plat_unlock();
    return v;
}

uint32_t atomic_xchg32(uint32_t a, uint32_t v) {
    plat_lock();
    uint32_t old = rd32(a);
    wr32(a, v);
    plat_unlock();
    return old;
}

int image_index(const char *name) {
    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    char want[260];
    snprintf(want, sizeof want, "%s", base);
    size_t n = strlen(want);
    if (n < 4 || (want[n - 4] != '.')) snprintf(want + n, sizeof want - n, ".dll");
    for (uint32_t i = 0; i < image_count; i++) {
        const char *a = images[i].name, *b = want;
        while (*a && *b && (*a | 32) == (*b | 32)) a++, b++;
        if (!*a && !*b) return (int)i;
    }
    return -1;
}

uint32_t image_export(int index, const char *name) {
    const Image *img = &images[index];
    for (uint32_t i = 0; i < img->export_count; i++)
        if (!strcmp(img->exports[i].name, name)) return img->exports[i].addr;
    for (uint32_t i = 0; i < img->ordinal_count; i++)
        if (!strcmp(img->ordinals[i].name, name)) return img->ordinals[i].addr;
    return 0;
}

uint32_t image_load(CPU *c, int index) {
    const Image *img = &images[index];
    if (!image_ready[index]) {
        image_ready[index] = 1;
        uint32_t args[3] = { img->base, 1, 0 };
        if (!guest_call(c, img->entry, 3, args)) fprintf(stderr, "DllMain of %s failed\n", img->name);
    }
    return img->base;
}

void runtime_init(void) {
    uint32_t probe = 1;
    if (*(uint8_t *)&probe != 1) {
        fprintf(stderr, "big-endian hosts are not supported\n");
        exit(3);
    }
    M = calloc(ARENA_SIZE, 1);
    if (!M) {
        fprintf(stderr, "cannot reserve %u MB for guest memory\n", ARENA_SIZE >> 20);
        exit(3);
    }
    for (uint32_t i = 0; i < image_count; i++) memcpy(M + images[i].base, images[i].data, images[i].data_len);
    M[FAKE_EXE] = 'M';
    M[FAKE_EXE + 1] = 'Z';
    wr32(TEB_BASE + 0xF000u + 8, FAKE_EXE);
    plat_init();
    shims_init();
}

double fround(CPU *c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 1: return floor(v);
    case 2: return ceil(v);
    case 3: return trunc(v);
    default: return rint(v);
    }
}

int64_t fround_int(CPU *c, double v, int bits) {
    double r = fround(c, v);
    double lim = ldexp(1.0, bits - 1);
    if (!(r >= -lim && r < lim)) return bits == 64 ? INT64_MIN : -(int64_t)lim;
    return (int64_t)r;
}

double rdf80(uint32_t a) {
    uint64_t m = rd64(a);
    uint16_t se = rd16(a + 8);
    int e = se & 0x7FFF;
    double sign = (se & 0x8000) ? -1.0 : 1.0;
    if (e == 0x7FFF) return (m << 1) ? NAN : sign * INFINITY;
    if (!e && !m) return sign * 0.0;
    return sign * ldexp((double)m, e - 16383 - 63);
}

void wrf80(uint32_t a, double v) {
    uint16_t sign = signbit(v) ? 0x8000 : 0;
    uint64_t m = 0;
    int e = 0;
    if (isnan(v)) { m = 0xC000000000000000ull; e = 0x7FFF; }
    else if (isinf(v)) { m = 0x8000000000000000ull; e = 0x7FFF; }
    else if (v != 0.0) {
        int ex;
        double f = frexp(fabs(v), &ex);
        m = (uint64_t)ldexp(f, 64);
        e = ex + 16382;
    }
    wr64(a, m);
    wr16(a + 8, (uint16_t)(sign | e));
}

void fxam(CPU *c) {
    double v = ST(0);
    c->fsw &= ~0x4700u;
    if (signbit(v)) c->fsw |= 0x0200u;
    if (isnan(v)) c->fsw |= 0x0100u;
    else if (isinf(v)) c->fsw |= 0x0500u;
    else if (v == 0.0) c->fsw |= 0x4000u;
    else if (fpclassify(v) == FP_SUBNORMAL) c->fsw |= 0x4400u;
    else c->fsw |= 0x0400u;
}

void fnstenv(CPU *c, uint32_t a) {
    memset(M + a, 0, 28);
    wr16(a, (uint16_t)c->fcw);
    wr16(a + 4, fsw(c));
    c->fcw |= 0x3Fu;
}

void fldenv(CPU *c, uint32_t a) {
    c->fcw = rd16(a);
    c->fsw = rd16(a + 4) & ~0x3800u;
    c->ftop = (rd16(a + 4) >> 11) & 7u;
}

void fnsave(CPU *c, uint32_t a) {
    memset(M + a, 0, 28);
    wr16(a, (uint16_t)c->fcw);
    wr16(a + 4, fsw(c));
    for (int i = 0; i < 8; i++) wrf80(a + 28u + 10u * (uint32_t)i, ST(i));
    c->fcw = 0x37F;
    c->fsw = 0;
    c->ftop = 0;
}

void frstor(CPU *c, uint32_t a) {
    fldenv(c, a);
    for (int i = 0; i < 8; i++) ST(i) = rdf80(a + 28u + 10u * (uint32_t)i);
}
