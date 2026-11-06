#ifndef RECOMP_H
#define RECOMP_H

#include <math.h>
#include <stdint.h>
#include <string.h>

extern uint32_t g_user_text_ptr;

typedef struct Thread Thread;

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t cf, zf, sf, of, df;
    uint32_t fs_base;
    double st[8];
    uint32_t ftop, fcw, fsw;
    Thread *thread;
} CPU;

typedef void (*gfn)(CPU *c);
typedef struct { const char *name; uint32_t addr; } Export;
typedef struct {
    const char *name;
    uint32_t base, size, entry, data_len;
    const uint8_t *data;
    uint32_t export_count;
    const Export *exports;
    uint32_t ordinal_count;
    const Export *ordinals;
} Image;

extern const uint32_t func_count;
extern const uint32_t func_addr[];
extern const gfn func_ptr[];
extern const uint32_t import_count;
extern const char *const import_name[];
extern const uint32_t image_count;
extern const Image images[];

extern uint8_t *M;

static inline uint8_t rd8(uint32_t a) { return M[a]; }
static inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, M + a, 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, M + a, 4); return v; }
static inline uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, M + a, 8); return v; }
static inline void wr8(uint32_t a, uint8_t v) { M[a] = v; }
static inline void wr16(uint32_t a, uint16_t v) { memcpy(M + a, &v, 2); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(M + a, &v, 4); }
static inline void wr64(uint32_t a, uint64_t v) { memcpy(M + a, &v, 8); }
static inline double rdf32(uint32_t a) { float v; memcpy(&v, M + a, 4); return v; }
static inline double rdf64(uint32_t a) { double v; memcpy(&v, M + a, 8); return v; }
static inline void wrf32(uint32_t a, double v) { float f = (float)v; memcpy(M + a, &f, 4); }
static inline void wrf64(uint32_t a, double v) { memcpy(M + a, &v, 8); }

void call_indirect(CPU *c, uint32_t target);
void call_import(CPU *c, uint32_t idx);
void trap(CPU *c, uint32_t addr, const char *what);
uint32_t atomic_add32(uint32_t a, uint32_t delta);
uint32_t atomic_xchg32(uint32_t a, uint32_t v);

#define ST(i) c->st[(c->ftop + (i)) & 7]
static inline void fpush(CPU *c, double v) { c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = v; }
static inline void fpop(CPU *c) { c->ftop = (c->ftop + 1) & 7; }
static inline uint16_t fsw(CPU *c) { return (uint16_t)((c->fsw & ~0x3800u) | (c->ftop << 11)); }
static inline void fcom_set(CPU *c, double a, double b) {
    c->fsw &= ~0x4700u;
    if (a != a || b != b) c->fsw |= 0x4500u;
    else if (a < b) c->fsw |= 0x0100u;
    else if (a == b) c->fsw |= 0x4000u;
}
double fround(CPU *c, double v);
int64_t fround_int(CPU *c, double v, int bits);
double rdf80(uint32_t a);
void wrf80(uint32_t a, double v);
void fxam(CPU *c);
void fnsave(CPU *c, uint32_t a);
void frstor(CPU *c, uint32_t a);
void fnstenv(CPU *c, uint32_t a);
void fldenv(CPU *c, uint32_t a);

#define LOAD uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, \
    esp = c->esp, ebp = c->ebp, esi = c->esi, edi = c->edi, \
    cf = c->cf, zf = c->zf, sf = c->sf, of = c->of
#define SPILL (c->eax = eax, c->ecx = ecx, c->edx = edx, c->ebx = ebx, c->esp = esp, c->ebp = ebp, \
    c->esi = esi, c->edi = edi, c->cf = cf, c->zf = zf, c->sf = sf, c->of = of)
#define RELOAD (eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esp = c->esp, ebp = c->ebp, \
    esi = c->esi, edi = c->edi, cf = c->cf, zf = c->zf, sf = c->sf, of = c->of)

#endif
