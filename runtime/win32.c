#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "runtime.h"

extern const uint16_t cp1252_unicode[256];
extern const uint16_t cp1252_ctype1[256];
extern const uint8_t cp1252_upper[256];
extern const uint8_t cp1252_lower[256];

#define ARG(i) rd32(c->esp + 4u + 4u * (uint32_t)(i))
#define SET_ERROR(e) (c->thread->last_error = (e))
#define WAIT_TIMEOUT 0x102u
#define HKLM 0x80000002u

enum { K_NONE, K_EVENT, K_MUTEX, K_SEM, K_THREAD, K_PROCESS, K_FILE, K_FIND, K_MAPPING, K_KEY, K_MMIO };

typedef struct {
    int kind;
    int signaled, manual;
    int32_t count, max;
    Thread *owner;
    uint32_t depth, addr;
    FILE *f;
    char name[128];
    char path[600];
    char **hits;
    int nhits, pos;
    uint32_t chunks[8];
    int nchunks;
} Obj;

#define MAX_HANDLES 4096
static Obj *handles[MAX_HANDLES];

static uint32_t handle_new(Obj *o) {
    for (uint32_t i = 1; i < MAX_HANDLES; i++)
        if (!handles[i]) { handles[i] = o; return 0x1000u + 4u * i; }
    fprintf(stderr, "out of handles\n");
    exit(3);
}

static Obj *obj_new(int kind) {
    Obj *o = calloc(1, sizeof *o);
    o->kind = kind;
    return o;
}

static Obj *handle_get(CPU *c, uint32_t h) {
    uint32_t i = (h - 0x1000u) / 4u;
    if (h < 0x1000u || i >= MAX_HANDLES || !handles[i]) trap(c, h, "bad handle");
    return handles[i];
}

static uint32_t named(CPU *c, uint32_t name_ptr, Obj *create) {
    const char *name = name_ptr ? (const char *)M + name_ptr : NULL;
    if (name && *name) {
        for (uint32_t i = 1; i < MAX_HANDLES; i++)
            if (handles[i] && !strcmp(handles[i]->name, name)) {
                free(create);
                SET_ERROR(183);
                return 0x1000u + 4u * i;
            }
    }
    if (!create) { SET_ERROR(2); return 0; }
    if (name) snprintf(create->name, sizeof create->name, "%s", name);
    SET_ERROR(0);
    return handle_new(create);
}

static void resolve_case(char *path, const char *root, const char *rel);

typedef struct { const char *want; char found[260]; } CaseMatch;

static void case_cb(const char *name, int is_dir, uint64_t size, void *user) {
    CaseMatch *m = user;
    (void)is_dir; (void)size;
    const char *a = name, *b = m->want;
    while (*a && *b && (*a | 32) == (*b | 32)) a++, b++;
    if (!*a && !*b && !m->found[0]) snprintf(m->found, sizeof m->found, "%s", name);
}

static void resolve_case(char *path, const char *root, const char *rel) {
    snprintf(path, 600, "%s", root);
    char part[260];
    while (*rel) {
        size_t n = 0;
        while (rel[n] && rel[n] != '\\' && rel[n] != '/') n++;
        if (n && n < sizeof part) {
            memcpy(part, rel, n);
            part[n] = 0;
            CaseMatch m = { part, "" };
            size_t len = strlen(path);
            snprintf(path + len, 600 - len, "/%s", part);
            if (!plat_is_file(path) && !plat_is_dir(path)) {
                path[len] = 0;
                plat_list_dir(path, case_cb, &m);
                snprintf(path + len, 600 - len, "/%s", m.found[0] ? m.found : part);
            }
        }
        rel += n;
        while (*rel == '\\' || *rel == '/') rel++;
    }
}

static int host_path(const char *vpath, int write, char *out) {
    if (strstr(vpath, "..")) return 0;
    struct { const char *vroot; const char *root; int writable; } roots[] = {
        { VROOT, data_dir, 0 }, { VOUT, out_dir, 1 },
    };
    for (int i = 0; i < 2; i++) {
        size_t n = strlen(roots[i].vroot);
        const char *a = vpath, *b = roots[i].vroot;
        size_t k = 0;
        while (k < n && a[k] && (a[k] | 32) == (b[k] | 32)) k++;
        if (k == n && (vpath[n] == 0 || vpath[n] == '\\' || vpath[n] == '/')) {
            if (write && !roots[i].writable) return 0;
            resolve_case(out, roots[i].root, vpath + n);
            return 1;
        }
    }
    if (!write && !strchr(vpath, '\\') && !strchr(vpath, ':') && !strchr(vpath, '$')) {
        resolve_case(out, data_dir, vpath);
        return 1;
    }
    return 0;
}

static uint32_t put_str(uint32_t buf, uint32_t size, const char *s) {
    if (!size) return 0;
    uint32_t n = (uint32_t)strlen(s);
    if (n > size - 1) n = size - 1;
    memcpy(M + buf, s, n);
    M[buf + n] = 0;
    return n;
}

static uint32_t impl_GetVersion(CPU *c) { (void)c; return (2600u << 16) | (1u << 8) | 5u; }

static uint32_t impl_GetVersionExA(CPU *c) {
    uint32_t info = ARG(0), size = rd32(info);
    wr32(info + 4, 5); wr32(info + 8, 1); wr32(info + 12, 2600); wr32(info + 16, 2);
    memcpy(M + info + 20, "Service Pack 3", 15);
    if (size >= 156) { wr16(info + 148, 3); wr16(info + 150, 0); wr16(info + 152, 0x100); M[info + 154] = 1; M[info + 155] = 0; }
    return 1;
}

static uint32_t impl_GetCommandLineA(CPU *c) {
    static uint32_t cmd;
    (void)c;
    if (!cmd) cmd = guest_str("loqhost.exe");
    return cmd;
}

static uint32_t impl_GetEnvironmentStrings(CPU *c) { (void)c; return heap_alloc(4, 1); }
static uint32_t impl_FreeEnvironmentStrings(CPU *c) { (void)c; return 1; }
static uint32_t impl_GetEnvironmentVariableA(CPU *c) { SET_ERROR(203); return 0; }
static uint32_t impl_one(CPU *c) { (void)c; return 1; }
static uint32_t impl_zero(CPU *c) { (void)c; return 0; }
static uint32_t impl_GetStartupInfoA(CPU *c) { memset(M + ARG(0), 0, 68); wr32(ARG(0), 68); return 0; }

static uint32_t impl_GetStdHandle(CPU *c) {
    switch (ARG(0)) {
    case 0xFFFFFFF5u: return 0xE0000001u;
    case 0xFFFFFFF4u: return 0xE0000002u;
    case 0xFFFFFFF6u: return 0;
    default: return INVALID;
    }
}

static uint32_t impl_GetFileType(CPU *c) {
    uint32_t h = ARG(0), i = (h - 0x1000u) / 4u;
    if (h == 0xE0000001u || h == 0xE0000002u) return 2;
    return h >= 0x1000u && i < MAX_HANDLES && handles[i] ? 1 : 0;
}

static uint32_t impl_SetHandleCount(CPU *c) { return ARG(0); }
static uint32_t impl_GetLastError(CPU *c) { return c->thread->last_error; }
static uint32_t impl_SetLastError(CPU *c) { c->thread->last_error = ARG(0); return 0; }
static uint32_t impl_GetCurrentProcess(CPU *c) { (void)c; return INVALID; }
static uint32_t impl_GetCurrentProcessId(CPU *c) { (void)c; return 1000; }
static uint32_t impl_GetCurrentThreadId(CPU *c) { return c->thread->tid; }
static uint32_t impl_OpenProcess(CPU *c) { (void)c; return handle_new(obj_new(K_PROCESS)); }

static uint32_t impl_ExitProcess(CPU *c) {
    fprintf(stderr, "engine called ExitProcess(%u)\n", ARG(0));
    exit(3);
}

static uint32_t impl_RaiseException(CPU *c) {
    trap(c, ARG(0), "RaiseException (guest exceptions are not supported)");
    return 0;
}

static uint32_t impl_OutputDebugStringA(CPU *c) {
    if (trace_on) fprintf(stderr, "[dbg] %s", (const char *)M + ARG(0));
    return 0;
}

static int is_system_dll(const char *name) {
    static const char *const sys[] = { "kernel32", "user32", "advapi32", "winmm", "ws2_32" };
    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    for (int i = 0; i < 5; i++) {
        size_t n = strlen(sys[i]);
        const char *a = base, *b = sys[i];
        size_t k = 0;
        while (k < n && (a[k] | 32) == b[k]) k++;
        if (k == n && (!a[n] || a[n] == '.')) return i + 1;
    }
    return 0;
}

static uint32_t impl_LoadLibraryA(CPU *c) {
    const char *name = (const char *)M + ARG(0);
    int i = image_index(name);
    if (i >= 0) return image_load(c, i);
    int s = is_system_dll(name);
    if (s) return 0x7E000000u + 0x10u * (uint32_t)s;
    SET_ERROR(126);
    return 0;
}

static uint32_t impl_GetModuleHandleA(CPU *c) {
    if (!ARG(0)) return FAKE_EXE;
    const char *name = (const char *)M + ARG(0);
    int i = image_index(name);
    if (i >= 0) return images[i].base;
    int s = is_system_dll(name);
    return s ? 0x7E000000u + 0x10u * (uint32_t)s : 0;
}

static uint32_t impl_GetProcAddress(CPU *c) {
    uint32_t h = ARG(0), sym = ARG(1);
    char name[260];
    if (sym <= 0xFFFF) snprintf(name, sizeof name, "#%u", sym);
    else snprintf(name, sizeof name, "%s", (const char *)M + sym);
    for (uint32_t i = 0; i < image_count; i++)
        if (images[i].base == h) return image_export((int)i, name);
    uint32_t a = shim_address(name);
    if (!a) SET_ERROR(127);
    return a;
}

static uint32_t impl_GetModuleFileNameA(CPU *c) {
    const char *name = "loqhost.exe";
    for (uint32_t i = 0; i < image_count; i++)
        if (images[i].base == ARG(0)) name = images[i].name;
    char full[300];
    snprintf(full, sizeof full, "%s\\%s", VROOT, name);
    return put_str(ARG(1), ARG(2), full);
}

static uint32_t impl_HeapCreate(CPU *c) { (void)c; return 0x00600000u; }
static uint32_t impl_HeapAlloc(CPU *c) { return heap_alloc(ARG(2), (ARG(1) & 8u) != 0); }
static uint32_t impl_HeapFree(CPU *c) { heap_free(ARG(2)); return 1; }

static uint32_t impl_HeapReAlloc(CPU *c) {
    uint32_t flags = ARG(1), p = ARG(2), size = ARG(3), old = heap_size(p);
    if (((size + 15u) & ~15u) <= old) return p;
    if (flags & 0x10u) return 0;
    uint32_t n = heap_alloc(size, (flags & 8u) != 0);
    memcpy(M + n, M + p, old);
    heap_free(p);
    return n;
}

static uint32_t impl_VirtualAlloc(CPU *c) {
    uint32_t addr = ARG(0), size = ARG(1);
    if (addr >= HEAP_BASE && addr < ARENA_SIZE) return addr & ~0xFFFu;
    uint32_t a = heap_alloc_aligned(size, 0x10000u);
    memset(M + a, 0, size);
    return a;
}

static uint32_t impl_LocalAlloc(CPU *c) { return heap_alloc(ARG(1), (ARG(0) & 0x40u) != 0); }
static uint32_t impl_LocalFree(CPU *c) { heap_free(ARG(0)); return 0; }
static uint32_t impl_InterlockedIncrement(CPU *c) { return atomic_add32(ARG(0), 1); }
static uint32_t impl_InterlockedDecrement(CPU *c) { return atomic_add32(ARG(0), INVALID); }

static uint32_t tls_next;

static uint32_t impl_TlsAlloc(CPU *c) {
    (void)c;
    plat_lock();
    uint32_t i = tls_next < 64 ? tls_next++ : INVALID;
    plat_unlock();
    return i;
}

static uint32_t impl_TlsGetValue(CPU *c) { c->thread->last_error = 0; return ARG(0) < 64 ? c->thread->tls[ARG(0)] : 0; }
static uint32_t impl_TlsSetValue(CPU *c) { if (ARG(0) < 64) c->thread->tls[ARG(0)] = ARG(1); return 1; }

static uint32_t impl_InitializeCriticalSection(CPU *c) { memset(M + ARG(0), 0, 24); return 0; }

static uint32_t impl_EnterCriticalSection(CPU *c) {
    uint32_t cs = ARG(0), me = c->thread->tid;
    plat_lock();
    while (rd32(cs + 12) && rd32(cs + 12) != me) plat_wait(INVALID);
    wr32(cs + 12, me);
    wr32(cs + 8, rd32(cs + 8) + 1);
    plat_unlock();
    return 0;
}

static uint32_t impl_LeaveCriticalSection(CPU *c) {
    uint32_t cs = ARG(0);
    plat_lock();
    if (rd32(cs + 12) == c->thread->tid) {
        wr32(cs + 8, rd32(cs + 8) - 1);
        if (!rd32(cs + 8)) { wr32(cs + 12, 0); plat_broadcast(); }
    }
    plat_unlock();
    return 0;
}

static uint32_t impl_CreateEventA(CPU *c) {
    Obj *o = obj_new(K_EVENT);
    o->manual = ARG(1) != 0;
    o->signaled = ARG(2) != 0;
    plat_lock();
    uint32_t h = named(c, ARG(3), o);
    plat_unlock();
    return h;
}

static uint32_t set_event(CPU *c, int on) {
    plat_lock();
    handle_get(c, ARG(0))->signaled = on;
    if (on) plat_broadcast();
    plat_unlock();
    return 1;
}

static uint32_t impl_SetEvent(CPU *c) { return set_event(c, 1); }
static uint32_t impl_ResetEvent(CPU *c) { return set_event(c, 0); }

static uint32_t impl_CreateMutexA(CPU *c) {
    Obj *o = obj_new(K_MUTEX);
    if (ARG(1)) { o->owner = c->thread; o->depth = 1; }
    plat_lock();
    uint32_t h = named(c, ARG(2), o);
    plat_unlock();
    return h;
}

static uint32_t impl_OpenNamed(CPU *c) {
    plat_lock();
    uint32_t h = named(c, ARG(2), NULL);
    plat_unlock();
    return h;
}

static uint32_t impl_ReleaseMutex(CPU *c) {
    plat_lock();
    Obj *o = handle_get(c, ARG(0));
    if (o->owner == c->thread && --o->depth == 0) { o->owner = NULL; plat_broadcast(); }
    plat_unlock();
    return 1;
}

static uint32_t impl_CreateSemaphoreA(CPU *c) {
    Obj *o = obj_new(K_SEM);
    o->count = (int32_t)ARG(1);
    o->max = (int32_t)ARG(2);
    plat_lock();
    uint32_t h = named(c, ARG(3), o);
    plat_unlock();
    return h;
}

static uint32_t impl_ReleaseSemaphore(CPU *c) {
    plat_lock();
    Obj *o = handle_get(c, ARG(0));
    if (ARG(2)) wr32(ARG(2), (uint32_t)o->count);
    o->count += (int32_t)ARG(1);
    if (o->count > o->max) o->count = o->max;
    plat_broadcast();
    plat_unlock();
    return 1;
}

static int obj_ready(Obj *o, Thread *t) {
    switch (o->kind) {
    case K_SEM: return o->count > 0;
    case K_MUTEX: return !o->owner || o->owner == t;
    default: return o->signaled;
    }
}

static void obj_take(Obj *o, Thread *t) {
    if (o->kind == K_SEM) o->count--;
    else if (o->kind == K_MUTEX) { o->owner = t; o->depth++; }
    else if (o->kind == K_EVENT && !o->manual) o->signaled = 0;
}

static uint32_t wait_objects(CPU *c, Obj **objs, uint32_t n, int all, uint32_t ms) {
    uint64_t deadline = plat_ms() + ms;
    for (;;) {
        uint32_t ready = 0, first = INVALID;
        for (uint32_t i = 0; i < n; i++)
            if (obj_ready(objs[i], c->thread)) { ready++; if (first == INVALID) first = i; }
        if (all ? ready == n : ready > 0) {
            for (uint32_t i = 0; i < n; i++)
                if (all || i == first) obj_take(objs[i], c->thread);
            return all ? 0 : first;
        }
        if (ms != INVALID) {
            uint64_t now = plat_ms();
            if (now >= deadline) return WAIT_TIMEOUT;
            plat_wait((uint32_t)(deadline - now));
        } else {
            plat_wait(INVALID);
        }
    }
}

static uint32_t impl_WaitForSingleObject(CPU *c) {
    plat_lock();
    Obj *o = handle_get(c, ARG(0));
    uint32_t r = wait_objects(c, &o, 1, 0, ARG(1));
    plat_unlock();
    return r;
}

static uint32_t impl_MsgWaitForMultipleObjects(CPU *c) {
    uint32_t n = ARG(0);
    Obj *objs[64];
    if (n > 64) trap(c, n, "too many wait objects");
    plat_lock();
    for (uint32_t i = 0; i < n; i++) objs[i] = handle_get(c, rd32(ARG(1) + 4u * i));
    uint32_t r = wait_objects(c, objs, n, ARG(2) != 0, ARG(3));
    plat_unlock();
    return r;
}

static uint32_t impl_Sleep(CPU *c) { plat_sleep(ARG(0)); return 0; }

static void thread_main(void *p) {
    Thread *t = p;
    if (!setjmp(t->exit)) {
        uint32_t arg = t->param;
        guest_call(&t->cpu, t->start, 1, &arg);
    }
    plat_lock();
    handle_get(&t->cpu, t->handle)->signaled = 1;
    plat_broadcast();
    plat_unlock();
}

static uint32_t impl_CreateThread(CPU *c) {
    Thread *t = thread_new();
    t->start = ARG(2);
    t->param = ARG(3);
    plat_lock();
    t->handle = handle_new(obj_new(K_THREAD));
    plat_unlock();
    if (ARG(5)) wr32(ARG(5), t->tid);
    if (!(ARG(4) & 4u) && !plat_thread_start(thread_main, t, 0)) return 0;
    return t->handle;
}

static uint32_t impl_ExitThread(CPU *c) {
    if (c->thread->index == 0) trap(c, 0, "ExitThread on the main thread");
    longjmp(c->thread->exit, 1);
}

static uint32_t impl_CloseHandle(CPU *c) {
    uint32_t h = ARG(0), i = (h - 0x1000u) / 4u;
    if (h < 0x1000u || i >= MAX_HANDLES || !handles[i]) return 1;
    plat_lock();
    Obj *o = handles[i];
    if (o->kind == K_FILE && o->f) { fclose(o->f); o->f = NULL; }
    plat_unlock();
    return 1;
}

static uint32_t impl_CreateFileA(CPU *c) {
    const char *vpath = (const char *)M + ARG(0);
    uint32_t access = ARG(1), disp = ARG(4);
    int write = (access & 0x40000000u) != 0;
    char path[600];
    if (trace_on > 1) fprintf(stderr, "[api] CreateFileA path \"%s\"\n", vpath);
    if (!host_path(vpath, write, path)) { SET_ERROR(5); return INVALID; }
    int exists = plat_is_file(path);
    if (((disp == 3 || disp == 5) && !exists) || (disp == 1 && exists)) { SET_ERROR(exists ? 80 : 2); return INVALID; }
    const char *mode = !write ? "rb" : (exists && (disp == 3 || disp == 4)) ? "r+b" : "w+b";
    FILE *f = fopen(path, mode);
    if (!f) { SET_ERROR(5); return INVALID; }
    SET_ERROR(exists && (disp == 2 || disp == 4) ? 183 : 0);
    Obj *o = obj_new(K_FILE);
    o->f = f;
    snprintf(o->path, sizeof o->path, "%s", path);
    plat_lock();
    uint32_t h = handle_new(o);
    plat_unlock();
    return h;
}

static uint32_t impl_ReadFile(CPU *c) {
    Obj *o = handle_get(c, ARG(0));
    uint32_t n = o->f ? (uint32_t)fread(M + ARG(1), 1, ARG(2), o->f) : 0;
    if (ARG(3)) wr32(ARG(3), n);
    return 1;
}

static uint32_t impl_WriteFile(CPU *c) {
    uint32_t h = ARG(0), n = ARG(2);
    if (h == 0xE0000001u || h == 0xE0000002u) {
        if (trace_on) fwrite(M + ARG(1), 1, n, stderr);
    } else {
        Obj *o = handle_get(c, h);
        if (o->f) fwrite(M + ARG(1), 1, n, o->f);
    }
    if (ARG(3)) wr32(ARG(3), n);
    return 1;
}

static uint32_t impl_SetFilePointer(CPU *c) {
    Obj *o = handle_get(c, ARG(0));
    int64_t dist = (int32_t)ARG(1);
    if (ARG(2)) dist = (int64_t)(((uint64_t)rd32(ARG(2)) << 32) | ARG(1));
    fseek(o->f, (long)dist, (int)ARG(3));
    int64_t pos = ftell(o->f);
    if (ARG(2)) wr32(ARG(2), (uint32_t)(pos >> 32));
    return (uint32_t)pos;
}

static uint32_t impl_GetFileSize(CPU *c) {
    Obj *o = handle_get(c, ARG(0));
    long cur = ftell(o->f);
    fseek(o->f, 0, SEEK_END);
    long size = ftell(o->f);
    fseek(o->f, cur, SEEK_SET);
    if (ARG(1)) wr32(ARG(1), 0);
    return (uint32_t)size;
}

static uint32_t impl_DeleteFileA(CPU *c) {
    char path[600];
    if (host_path((const char *)M + ARG(0), 1, path) && plat_is_file(path) && !remove(path)) return 1;
    SET_ERROR(2);
    return 0;
}

typedef struct { const char *mask; char **hits; int n, cap; } FindCtx;

static int wild(const char *m, const char *s) {
    if (!*m) return !*s;
    if (*m == '*') return wild(m + 1, s) || (*s && wild(m, s + 1));
    if (!*s) return 0;
    return (*m == '?' || (*m | 32) == (*s | 32)) && wild(m + 1, s + 1);
}

static void find_cb(const char *name, int is_dir, uint64_t size, void *user) {
    FindCtx *f = user;
    (void)is_dir; (void)size;
    if (!wild(f->mask, name)) return;
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 8; f->hits = realloc(f->hits, sizeof(char *) * (size_t)f->cap); }
    size_t n = strlen(name) + 1;
    f->hits[f->n] = malloc(n);
    memcpy(f->hits[f->n++], name, n);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void find_fill(uint32_t data, const char *dir, const char *name) {
    char full[1200];
    snprintf(full, sizeof full, "%s/%s", dir, name);
    memset(M + data, 0, 320);
    wr32(data, plat_is_dir(full) ? 0x10u : 0x80u);
    FILE *f = fopen(full, "rb");
    if (f) { fseek(f, 0, SEEK_END); wr32(data + 32, (uint32_t)ftell(f)); fclose(f); }
    put_str(data + 44, 260, name);
}

static uint32_t impl_FindFirstFileA(CPU *c) {
    char vpattern[600], dir[600];
    snprintf(vpattern, sizeof vpattern, "%s", (const char *)M + ARG(0));
    if (trace_on > 1) fprintf(stderr, "[api] FindFirstFileA pattern \"%s\"\n", vpattern);
    for (char *p = vpattern; *p; p++)
        if (*p == '/') *p = '\\';
    char *slash = strrchr(vpattern, '\\');
    const char *mask = slash ? slash + 1 : vpattern;
    if (slash) *slash = 0;
    if (!host_path(slash ? vpattern : ".", 0, dir) || !plat_is_dir(dir)) { SET_ERROR(3); return INVALID; }
    FindCtx f = { mask, NULL, 0, 0 };
    plat_list_dir(dir, find_cb, &f);
    if (!f.n) { SET_ERROR(2); return INVALID; }
    qsort(f.hits, (size_t)f.n, sizeof(char *), cmp_str);
    find_fill(ARG(1), dir, f.hits[0]);
    Obj *o = obj_new(K_FIND);
    o->hits = f.hits;
    o->nhits = f.n;
    o->pos = 1;
    snprintf(o->path, sizeof o->path, "%s", dir);
    plat_lock();
    uint32_t h = handle_new(o);
    plat_unlock();
    return h;
}

static uint32_t impl_FindNextFileA(CPU *c) {
    Obj *o = handle_get(c, ARG(0));
    if (o->pos >= o->nhits) { SET_ERROR(18); return 0; }
    find_fill(ARG(1), o->path, o->hits[o->pos++]);
    return 1;
}

static uint32_t impl_CreateFileMappingA(CPU *c) {
    Obj *o = obj_new(K_MAPPING);
    if (ARG(0) == INVALID) {
        o->addr = heap_alloc_aligned(ARG(4), 0x1000u);
        memset(M + o->addr, 0, ARG(4));
    } else {
        Obj *file = handle_get(c, ARG(0));
        long cur = ftell(file->f);
        fseek(file->f, 0, SEEK_END);
        uint32_t size = (uint32_t)ftell(file->f);
        fseek(file->f, 0, SEEK_SET);
        o->addr = heap_alloc_aligned(size, 0x1000u);
        if (fread(M + o->addr, 1, size, file->f) != size) SET_ERROR(30);
        fseek(file->f, cur, SEEK_SET);
    }
    plat_lock();
    uint32_t h = named(c, ARG(5), o);
    plat_unlock();
    return h;
}

static uint32_t impl_OpenFileMappingA(CPU *c) { return impl_OpenNamed(c); }
static uint32_t impl_MapViewOfFile(CPU *c) { return handle_get(c, ARG(0))->addr + ARG(3); }

static uint64_t start_ms;
static uint32_t impl_GetTickCount(CPU *c) { (void)c; return (uint32_t)(plat_ms() - start_ms); }

static void system_time(uint32_t buf, int local) {
    time_t now = time(NULL);
    struct tm *t = local ? localtime(&now) : gmtime(&now);
    uint16_t v[8] = { (uint16_t)(t->tm_year + 1900), (uint16_t)(t->tm_mon + 1), (uint16_t)t->tm_wday, (uint16_t)t->tm_mday,
                      (uint16_t)t->tm_hour, (uint16_t)t->tm_min, (uint16_t)t->tm_sec, 0 };
    memcpy(M + buf, v, 16);
}

static uint32_t impl_GetLocalTime(CPU *c) { system_time(ARG(0), 1); return 0; }
static uint32_t impl_GetSystemTime(CPU *c) { system_time(ARG(0), 0); return 0; }

static uint32_t impl_GetSystemTimeAsFileTime(CPU *c) {
    wr64(ARG(0), ((uint64_t)time(NULL) + 11644473600ull) * 10000000ull);
    return 0;
}

static uint32_t impl_GetTimeZoneInformation(CPU *c) { memset(M + ARG(0), 0, 172); return 0; }

static uint8_t unicode_to_cp1252(uint16_t u) {
    for (int b = 0; b < 256; b++)
        if (cp1252_unicode[b] == u) return (uint8_t)b;
    return '?';
}

static uint32_t impl_GetACP(CPU *c) { (void)c; return 1252; }
static uint32_t impl_GetOEMCP(CPU *c) { (void)c; return 850; }

static uint32_t impl_GetCPInfo(CPU *c) {
    uint32_t info = ARG(1);
    memset(M + info, 0, 20);
    wr32(info, 1);
    M[info + 4] = '?';
    return 1;
}

static uint32_t alen(uint32_t p, uint32_t n) { return n == INVALID ? (uint32_t)strlen((const char *)M + p) + 1 : n; }

static uint32_t wlen(uint32_t p, uint32_t n) {
    if (n != INVALID) return n;
    uint32_t k = 0;
    while (rd16(p + 2 * k)) k++;
    return k + 1;
}

static uint32_t impl_MultiByteToWideChar(CPU *c) {
    uint32_t src = ARG(2), n = alen(src, ARG(3)), dst = ARG(4), dst_n = ARG(5);
    if (!dst_n) return n;
    if (n > dst_n) { SET_ERROR(122); return 0; }
    for (uint32_t i = 0; i < n; i++) wr16(dst + 2 * i, cp1252_unicode[M[src + i]]);
    return n;
}

static uint32_t impl_WideCharToMultiByte(CPU *c) {
    uint32_t src = ARG(2), n = wlen(src, ARG(3)), dst = ARG(4), dst_n = ARG(5);
    if (!dst_n) return n;
    if (n > dst_n) { SET_ERROR(122); return 0; }
    for (uint32_t i = 0; i < n; i++) M[dst + i] = unicode_to_cp1252(rd16(src + 2 * i));
    if (ARG(7)) wr32(ARG(7), 0);
    return n;
}

static uint32_t string_type(CPU *c, uint32_t kind, const uint8_t *bytes, uint32_t n, uint32_t out) {
    (void)c;
    for (uint32_t i = 0; i < n; i++) wr16(out + 2 * i, kind == 1 ? cp1252_ctype1[bytes[i]] : 0);
    return 1;
}

static uint32_t impl_GetStringTypeA(CPU *c) {
    uint32_t n = alen(ARG(2), ARG(3));
    return string_type(c, ARG(1), M + ARG(2), n, ARG(4));
}

static uint32_t impl_GetStringTypeW(CPU *c) {
    uint32_t src = ARG(1), n = wlen(src, ARG(2));
    uint8_t *tmp = malloc(n + 1);
    for (uint32_t i = 0; i < n; i++) tmp[i] = unicode_to_cp1252(rd16(src + 2 * i));
    string_type(c, ARG(0), tmp, n, ARG(3));
    free(tmp);
    return 1;
}

static uint8_t map_case(uint32_t flags, uint8_t b) {
    return flags & 0x100u ? cp1252_lower[b] : flags & 0x200u ? cp1252_upper[b] : b;
}

static uint32_t impl_LCMapStringA(CPU *c) {
    uint32_t flags = ARG(1), src = ARG(2), n = alen(src, ARG(3)), dst = ARG(4), dst_n = ARG(5);
    if (!dst_n) return n;
    if (n > dst_n) n = dst_n;
    for (uint32_t i = 0; i < n; i++) M[dst + i] = map_case(flags, M[src + i]);
    return n;
}

static uint32_t impl_LCMapStringW(CPU *c) {
    uint32_t flags = ARG(1), src = ARG(2), n = wlen(src, ARG(3)), dst = ARG(4), dst_n = ARG(5);
    if (!dst_n) return n;
    if (n > dst_n) n = dst_n;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t u = rd16(src + 2 * i);
        uint8_t b = unicode_to_cp1252(u);
        wr16(dst + 2 * i, (b == '?' && u != '?') ? u : cp1252_unicode[map_case(flags, b)]);
    }
    return n;
}

static uint32_t compare(const uint16_t *a, uint32_t na, const uint16_t *b, uint32_t nb) {
    for (uint32_t i = 0; i < na && i < nb; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? 1 : 3;
    return na < nb ? 1 : na > nb ? 3 : 2;
}

static uint32_t compare_strings(uint32_t flags, uint16_t *a, uint32_t na, uint16_t *b, uint32_t nb) {
    while (na && !a[na - 1]) na--;
    while (nb && !b[nb - 1]) nb--;
    if (flags & 1u) {
        for (uint32_t i = 0; i < na; i++) a[i] = cp1252_unicode[cp1252_lower[unicode_to_cp1252(a[i])]];
        for (uint32_t i = 0; i < nb; i++) b[i] = cp1252_unicode[cp1252_lower[unicode_to_cp1252(b[i])]];
    }
    return compare(a, na, b, nb);
}

static uint32_t impl_CompareStringA(CPU *c) {
    uint32_t na = alen(ARG(2), ARG(3)), nb = alen(ARG(4), ARG(5));
    uint16_t *a = malloc(2 * (na + 1)), *b = malloc(2 * (nb + 1));
    for (uint32_t i = 0; i < na; i++) a[i] = cp1252_unicode[M[ARG(2) + i]];
    for (uint32_t i = 0; i < nb; i++) b[i] = cp1252_unicode[M[ARG(4) + i]];
    uint32_t r = compare_strings(ARG(1), a, na, b, nb);
    free(a);
    free(b);
    return r;
}

static uint32_t impl_CompareStringW(CPU *c) {
    uint32_t na = wlen(ARG(2), ARG(3)), nb = wlen(ARG(4), ARG(5));
    uint16_t *a = malloc(2 * (na + 1)), *b = malloc(2 * (nb + 1));
    for (uint32_t i = 0; i < na; i++) a[i] = rd16(ARG(2) + 2 * i);
    for (uint32_t i = 0; i < nb; i++) b[i] = rd16(ARG(4) + 2 * i);
    uint32_t r = compare_strings(ARG(1), a, na, b, nb);
    free(a);
    free(b);
    return r;
}

static uint32_t impl_MessageBoxA(CPU *c) {
    fprintf(stderr, "[engine message] %s: %s\n", ARG(2) ? (const char *)M + ARG(2) : "", (const char *)M + ARG(1));
    return 1;
}

static uint32_t impl_CharToOemA(CPU *c) {
    memmove(M + ARG(1), M + ARG(0), strlen((const char *)M + ARG(0)) + 1);
    return 1;
}

static uint32_t impl_wsprintfA(CPU *c) {
    const char *fmt = (const char *)M + ARG(1);
    uint32_t argp = c->esp + 12;
    char out[1025];
    size_t o = 0;
    while (*fmt && o < sizeof out - 1) {
        if (*fmt != '%') { out[o++] = *fmt++; continue; }
        char spec[32];
        size_t s = 0;
        spec[s++] = *fmt++;
        while (*fmt && strchr("-+ #0123456789.", *fmt) && s < 20) spec[s++] = *fmt++;
        while (*fmt && strchr("lhI", *fmt)) fmt++;
        char kind = *fmt ? *fmt++ : 0;
        char piece[600];
        if (kind == '%') { out[o++] = '%'; continue; }
        uint32_t v = rd32(argp);
        argp += 4;
        spec[s] = 0;
        if (kind == 'd' || kind == 'i') { spec[s] = 'd'; spec[s + 1] = 0; snprintf(piece, sizeof piece, spec, (int32_t)v); }
        else if (kind == 's' || kind == 'S') { spec[s] = 's'; spec[s + 1] = 0; snprintf(piece, sizeof piece, spec, v ? (const char *)M + v : "(null)"); }
        else if (kind == 'c') { piece[0] = (char)v; piece[1] = 0; }
        else { spec[s] = strchr("uxXo", kind) ? kind : 'X'; spec[s + 1] = 0; snprintf(piece, sizeof piece, spec, v); }
        for (const char *p = piece; *p && o < sizeof out - 1; p++) out[o++] = *p;
    }
    out[o] = 0;
    return put_str(ARG(0), sizeof out, out);
}

typedef struct { char key[200]; char name[64]; int is_dword; uint32_t dword; char str[260]; } RegValue;
static RegValue reg[64];
static int reg_n;
static char reg_keys[64][200];
static int reg_keys_n;

static void lower(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32); }

static void reg_put(const char *key, const char *name, int is_dword, uint32_t dword, const char *str) {
    char lname[64];
    snprintf(lname, sizeof lname, "%s", name);
    lower(lname);
    int i = 0;
    while (i < reg_n && (strcmp(reg[i].key, key) || strcmp(reg[i].name, lname))) i++;
    if (i == 64) return;
    if (i == reg_n) reg_n++;
    RegValue *v = &reg[i];
    snprintf(v->key, sizeof v->key, "%s", key);
    snprintf(v->name, sizeof v->name, "%s", name);
    lower(v->name);
    v->is_dword = is_dword;
    v->dword = dword;
    snprintf(v->str, sizeof v->str, "%s", str ? str : "");
}

static void reg_add_key(const char *key) {
    for (int i = 0; i < reg_keys_n; i++)
        if (!strcmp(reg_keys[i], key)) return;
    if (reg_keys_n < 64) snprintf(reg_keys[reg_keys_n++], 200, "%s", key);
}

static uint32_t open_key(CPU *c, uint32_t hkey, uint32_t sub, uint32_t out, int create) {
    char path[200];
    const char *root = hkey == HKLM ? "hklm" : hkey == 0x80000001u ? "hkcu" : hkey == 0x80000000u ? "hkcr" : NULL;
    if (root) snprintf(path, sizeof path, "%s", root);
    else snprintf(path, sizeof path, "%.199s", handle_get(c, hkey)->path);
    if (sub && M[sub]) {
        size_t n = strlen(path);
        snprintf(path + n, sizeof path - n, "\\%s", (const char *)M + sub);
    }
    lower(path);
    size_t n = strlen(path);
    while (n && path[n - 1] == '\\') path[--n] = 0;
    int found = 0;
    for (int i = 0; i < reg_keys_n && !found; i++)
        found = !strncmp(reg_keys[i], path, n) && (!reg_keys[i][n] || reg_keys[i][n] == '\\');
    if (!found && !create) return 2;
    if (!found) reg_add_key(path);
    Obj *o = obj_new(K_KEY);
    snprintf(o->path, sizeof o->path, "%s", path);
    plat_lock();
    wr32(out, handle_new(o));
    plat_unlock();
    return 0;
}

static uint32_t impl_RegOpenKeyExA(CPU *c) { return open_key(c, ARG(0), ARG(1), ARG(4), 0); }
static uint32_t impl_RegOpenKeyA(CPU *c) { return open_key(c, ARG(0), ARG(1), ARG(2), 0); }
static uint32_t impl_RegCreateKeyA(CPU *c) { return open_key(c, ARG(0), ARG(1), ARG(2), 1); }
static uint32_t impl_RegCreateKeyExA(CPU *c) { return open_key(c, ARG(0), ARG(1), ARG(7), 1); }

static uint32_t impl_RegQueryValueExA(CPU *c) {
    const char *key = handle_get(c, ARG(0))->path;
    char name[64];
    snprintf(name, sizeof name, "%s", ARG(1) ? (const char *)M + ARG(1) : "");
    lower(name);
    for (int i = 0; i < reg_n; i++) {
        if (strcmp(reg[i].key, key) || strcmp(reg[i].name, name)) continue;
        uint32_t size = reg[i].is_dword ? 4 : (uint32_t)strlen(reg[i].str) + 1;
        if (ARG(3)) wr32(ARG(3), reg[i].is_dword ? 4 : 1);
        if (!ARG(4)) { wr32(ARG(5), size); return 0; }
        if (rd32(ARG(5)) < size) { wr32(ARG(5), size); return 234; }
        if (reg[i].is_dword) wr32(ARG(4), reg[i].dword); else memcpy(M + ARG(4), reg[i].str, size);
        wr32(ARG(5), size);
        return 0;
    }
    return 2;
}

static uint32_t impl_RegSetValueExA(CPU *c) {
    const char *key = handle_get(c, ARG(0))->path;
    const char *name = ARG(1) ? (const char *)M + ARG(1) : "";
    char str[260] = "";
    uint32_t size = ARG(5) < sizeof str - 1 ? ARG(5) : (uint32_t)sizeof str - 1;
    if (ARG(3) != 4) memcpy(str, M + ARG(4), size);
    plat_lock();
    reg_put(key, name, ARG(3) == 4, ARG(3) == 4 ? rd32(ARG(4)) : 0, str);
    plat_unlock();
    return 0;
}

static uint32_t impl_AllocateAndInitializeSid(CPU *c) { wr32(ARG(10), heap_alloc(64, 1)); return 1; }
static uint32_t impl_GetLengthSid(CPU *c) { (void)c; return 12; }

static uint32_t impl_GetAdaptersInfo(CPU *c) { (void)c; return 232; }
static uint32_t impl_Netbios(CPU *c) { (void)c; return 0x40; }
static uint32_t impl_WSAStartup(CPU *c) { (void)c; return 10091; }

static void patch_sizes(Obj *o) {
    long end = ftell(o->f);
    for (int i = 0; i < o->nchunks; i++) {
        uint32_t size = (uint32_t)(end - (long)o->chunks[i]);
        fseek(o->f, (long)o->chunks[i] - 4, SEEK_SET);
        fwrite(&size, 4, 1, o->f);
    }
    fseek(o->f, end, SEEK_SET);
}

static uint32_t impl_mmioOpenA(CPU *c) {
    char path[600];
    if (!host_path((const char *)M + ARG(0), 1, path)) return 0;
    FILE *f = fopen(path, "w+b");
    if (!f) return 0;
    Obj *o = obj_new(K_MMIO);
    o->f = f;
    plat_lock();
    uint32_t h = handle_new(o);
    plat_unlock();
    return h;
}

static uint32_t impl_mmioCreateChunk(CPU *c) {
    Obj *o = handle_get(c, ARG(0));
    uint32_t ck = ARG(1), flags = ARG(2), ckid = rd32(ck), fcc = rd32(ck + 8), zero = 0;
    if (flags & 0x20u) memcpy(&ckid, "RIFF", 4);
    else if (flags & 0x40u) memcpy(&ckid, "LIST", 4);
    fwrite(&ckid, 4, 1, o->f);
    fwrite(&zero, 4, 1, o->f);
    uint32_t offset = (uint32_t)ftell(o->f);
    if (flags & 0x60u) fwrite(&fcc, 4, 1, o->f);
    if (o->nchunks < 8) o->chunks[o->nchunks++] = offset;
    patch_sizes(o);
    wr32(ck, ckid);
    wr32(ck + 12, offset);
    wr32(ck + 16, 0x10);
    return 0;
}

static uint32_t impl_mmioWrite(CPU *c) {
    Obj *o = handle_get(c, ARG(0));
    fwrite(M + ARG(1), 1, ARG(2), o->f);
    patch_sizes(o);
    return ARG(2);
}

static uint32_t impl_mmioAscend(CPU *c) {
    Obj *o = handle_get(c, ARG(0));
    uint32_t offset = rd32(ARG(1) + 12);
    patch_sizes(o);
    for (int i = 0; i < o->nchunks; i++)
        if (o->chunks[i] == offset) { o->chunks[i] = o->chunks[--o->nchunks]; break; }
    long end = ftell(o->f);
    if ((end - (long)offset) & 1) { fputc(0, o->f); patch_sizes(o); }
    wr32(ARG(1) + 4, (uint32_t)(end - (long)offset));
    return 0;
}

static uint32_t impl_mmioClose(CPU *c) {
    Obj *o = handle_get(c, ARG(0));
    if (o->f) { fclose(o->f); o->f = NULL; }
    return 0;
}

#define SHIMS \
    X(AddAccessAllowedAce, 4, impl_one) X(AllocateAndInitializeSid, 11, impl_AllocateAndInitializeSid) \
    X(CharToOemA, 2, impl_CharToOemA) X(CloseHandle, 1, impl_CloseHandle) X(CompareStringA, 6, impl_CompareStringA) \
    X(CompareStringW, 6, impl_CompareStringW) X(CreateEventA, 4, impl_CreateEventA) X(CreateFileA, 7, impl_CreateFileA) \
    X(CreateFileMappingA, 6, impl_CreateFileMappingA) X(CreateMutexA, 3, impl_CreateMutexA) \
    X(CreateSemaphoreA, 4, impl_CreateSemaphoreA) X(CreateThread, 6, impl_CreateThread) \
    X(DeleteCriticalSection, 1, impl_zero) X(DeleteFileA, 1, impl_DeleteFileA) X(DispatchMessageA, 1, impl_zero) \
    X(EnterCriticalSection, 1, impl_EnterCriticalSection) X(ExitProcess, 1, impl_ExitProcess) X(ExitThread, 1, impl_ExitThread) \
    X(FindClose, 1, impl_one) X(FindFirstFileA, 2, impl_FindFirstFileA) X(FindNextFileA, 2, impl_FindNextFileA) \
    X(FlushFileBuffers, 1, impl_one) X(FormatMessageA, 7, impl_zero) X(FreeEnvironmentStringsA, 1, impl_FreeEnvironmentStrings) \
    X(FreeEnvironmentStringsW, 1, impl_FreeEnvironmentStrings) X(FreeLibrary, 1, impl_one) X(FreeSid, 1, impl_zero) \
    X(GetACP, 0, impl_GetACP) X(GetAdaptersInfo, 2, impl_GetAdaptersInfo) X(GetCPInfo, 2, impl_GetCPInfo) \
    X(GetCommandLineA, 0, impl_GetCommandLineA) X(GetCurrentProcess, 0, impl_GetCurrentProcess) \
    X(GetCurrentProcessId, 0, impl_GetCurrentProcessId) X(GetCurrentThreadId, 0, impl_GetCurrentThreadId) \
    X(GetEnvironmentStrings, 0, impl_GetEnvironmentStrings) X(GetEnvironmentStringsW, 0, impl_GetEnvironmentStrings) \
    X(GetEnvironmentVariableA, 3, impl_GetEnvironmentVariableA) X(GetFileSize, 2, impl_GetFileSize) \
    X(GetFileType, 1, impl_GetFileType) X(GetLastError, 0, impl_GetLastError) X(GetLengthSid, 1, impl_GetLengthSid) \
    X(GetLocalTime, 1, impl_GetLocalTime) X(GetModuleFileNameA, 3, impl_GetModuleFileNameA) \
    X(GetModuleHandleA, 1, impl_GetModuleHandleA) X(GetOEMCP, 0, impl_GetOEMCP) X(GetProcAddress, 2, impl_GetProcAddress) \
    X(GetProcessHeap, 0, impl_HeapCreate) X(GetStartupInfoA, 1, impl_GetStartupInfoA) X(GetStdHandle, 1, impl_GetStdHandle) \
    X(GetStringTypeA, 5, impl_GetStringTypeA) X(GetStringTypeW, 4, impl_GetStringTypeW) X(GetSystemTime, 1, impl_GetSystemTime) \
    X(GetSystemTimeAsFileTime, 1, impl_GetSystemTimeAsFileTime) X(GetTickCount, 0, impl_GetTickCount) \
    X(GetTimeZoneInformation, 1, impl_GetTimeZoneInformation) X(GetVersion, 0, impl_GetVersion) \
    X(GetVersionExA, 1, impl_GetVersionExA) X(GlobalAlloc, 2, impl_LocalAlloc) X(GlobalFree, 1, impl_LocalFree) \
    X(HeapAlloc, 3, impl_HeapAlloc) X(HeapCreate, 3, impl_HeapCreate) X(HeapDestroy, 1, impl_one) X(HeapFree, 3, impl_HeapFree) \
    X(HeapReAlloc, 4, impl_HeapReAlloc) X(InitializeAcl, 3, impl_one) \
    X(InitializeCriticalSection, 1, impl_InitializeCriticalSection) X(InitializeSecurityDescriptor, 2, impl_one) \
    X(InterlockedDecrement, 1, impl_InterlockedDecrement) X(InterlockedIncrement, 1, impl_InterlockedIncrement) \
    X(IsBadCodePtr, 1, impl_zero) X(IsBadReadPtr, 2, impl_zero) X(IsBadWritePtr, 2, impl_zero) \
    X(LCMapStringA, 6, impl_LCMapStringA) X(LCMapStringW, 6, impl_LCMapStringW) \
    X(LeaveCriticalSection, 1, impl_LeaveCriticalSection) X(LoadLibraryA, 1, impl_LoadLibraryA) \
    X(LocalAlloc, 2, impl_LocalAlloc) X(LocalFree, 1, impl_LocalFree) X(MapViewOfFile, 5, impl_MapViewOfFile) \
    X(MessageBoxA, 4, impl_MessageBoxA) X(MsgWaitForMultipleObjects, 5, impl_MsgWaitForMultipleObjects) \
    X(MultiByteToWideChar, 6, impl_MultiByteToWideChar) X(Netbios, 1, impl_Netbios) X(OemToCharA, 2, impl_CharToOemA) \
    X(OpenFileMappingA, 3, impl_OpenFileMappingA) X(OpenMutexA, 3, impl_OpenNamed) X(OpenProcess, 3, impl_OpenProcess) \
    X(OpenSemaphoreA, 3, impl_OpenNamed) X(OutputDebugStringA, 1, impl_OutputDebugStringA) X(PeekMessageA, 5, impl_zero) \
    X(RaiseException, 4, impl_RaiseException) X(ReadFile, 5, impl_ReadFile) X(RegCloseKey, 1, impl_zero) \
    X(RegCreateKeyA, 3, impl_RegCreateKeyA) X(RegCreateKeyExA, 9, impl_RegCreateKeyExA) X(RegDeleteKeyA, 2, impl_zero) \
    X(RegOpenKeyA, 3, impl_RegOpenKeyA) X(RegOpenKeyExA, 5, impl_RegOpenKeyExA) X(RegQueryValueExA, 6, impl_RegQueryValueExA) \
    X(RegSetValueExA, 6, impl_RegSetValueExA) X(ReleaseMutex, 1, impl_ReleaseMutex) \
    X(ReleaseSemaphore, 3, impl_ReleaseSemaphore) X(ResetEvent, 1, impl_ResetEvent) X(RtlUnwind, 4, impl_RaiseException) \
    X(SendMessageA, 4, impl_zero) X(SetEndOfFile, 1, impl_one) X(SetEnvironmentVariableA, 2, impl_one) \
    X(SetErrorMode, 1, impl_zero) X(SetEvent, 1, impl_SetEvent) X(SetFilePointer, 4, impl_SetFilePointer) \
    X(SetHandleCount, 1, impl_SetHandleCount) X(SetLastError, 1, impl_SetLastError) \
    X(SetSecurityDescriptorDacl, 4, impl_one) X(SetStdHandle, 2, impl_one) X(SetUnhandledExceptionFilter, 1, impl_zero) \
    X(Sleep, 1, impl_Sleep) X(TerminateProcess, 2, impl_ExitProcess) X(TlsAlloc, 0, impl_TlsAlloc) X(TlsFree, 1, impl_one) \
    X(TlsGetValue, 1, impl_TlsGetValue) X(TlsSetValue, 2, impl_TlsSetValue) X(UnhandledExceptionFilter, 1, impl_zero) \
    X(UnmapViewOfFile, 1, impl_one) X(VirtualAlloc, 4, impl_VirtualAlloc) X(VirtualFree, 3, impl_one) \
    X(WSAStartup, 2, impl_WSAStartup) X(WaitForSingleObject, 2, impl_WaitForSingleObject) \
    X(WideCharToMultiByte, 8, impl_WideCharToMultiByte) X(WriteFile, 5, impl_WriteFile) X(mmioAscend, 3, impl_mmioAscend) \
    X(mmioClose, 2, impl_mmioClose) X(mmioCreateChunk, 3, impl_mmioCreateChunk) X(mmioOpenA, 3, impl_mmioOpenA) \
    X(mmioWrite, 3, impl_mmioWrite) X(wsprintfA, -1, impl_wsprintfA) \
    X(IsProcessorFeaturePresent, 1, impl_zero) X(DisableThreadLibraryCalls, 1, impl_one) \
    X(GetActiveWindow, 0, impl_zero) X(GetLastActivePopup, 1, impl_zero)

#define X(name, nargs, impl) \
    static void shim_##name(CPU *c) { \
        uint32_t a0 = ARG(0), a1 = ARG(1), a2 = ARG(2), r = impl(c); \
        if (trace_on > 1) fprintf(stderr, "[api] %s(%x, %x, %x) = %x\n", #name, a0, a1, a2, r); \
        c->eax = r; \
        c->esp += 4u + 4u * (uint32_t)((nargs) > 0 ? (nargs) : 0); \
    }
SHIMS
#undef X

typedef struct { const char *name; void (*fn)(CPU *); } Shim;
#define X(name, nargs, impl) { #name, shim_##name },
static const Shim shim_list[] = { SHIMS };
#undef X

#define SHIM_LIST_N (sizeof shim_list / sizeof shim_list[0])
uint32_t extra_shim_count = SHIM_LIST_N;
static void (*import_fn[512])(CPU *);

void shims_init(void) {
    if (import_count > 512) { fprintf(stderr, "too many imports\n"); exit(3); }
    for (uint32_t i = 0; i < import_count; i++)
        for (size_t k = 0; k < SHIM_LIST_N; k++)
            if (!strcmp(import_name[i], shim_list[k].name)) import_fn[i] = shim_list[k].fn;
    start_ms = plat_ms();
    static const char *const keys[] = { "hklm\\software\\loquendo\\ltts", "hklm\\software\\loquendo\\ltts\\default.session" };
    for (int i = 0; i < 2; i++) {
        reg_add_key(keys[i]);
        reg_put(keys[i], "DataPath", 0, 0, VROOT);
        reg_put(keys[i], "LibraryPath", 0, 0, VROOT);
    }
    reg_add_key("hklm\\software\\loquendo\\actorlicense");
    reg_put("hklm\\software\\loquendo\\actorlicense", "Code", 0, 0, "FFFFFFFF");
}

uint32_t shim_address(const char *name) {
    for (uint32_t i = 0; i < import_count; i++)
        if (!strcmp(import_name[i], name)) return SHIM_BASE + 16u * i;
    for (size_t k = 0; k < SHIM_LIST_N; k++)
        if (!strcmp(shim_list[k].name, name)) return SHIM_BASE + 16u * (import_count + (uint32_t)k);
    return 0;
}

void call_import(CPU *c, uint32_t idx) {
    if (!import_fn[idx]) trap(c, idx, import_name[idx]);
    import_fn[idx](c);
}

void call_extra_shim(CPU *c, uint32_t k) { shim_list[k].fn(c); }
