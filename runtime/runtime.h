#ifndef RUNTIME_H
#define RUNTIME_H

#include <setjmp.h>
#include <stdio.h>

#include "recomp.h"

#define ARENA_SIZE  0x19000000u
#define SHIM_BASE   0x00010000u
#define FAKE_EXE    0x00400000u
#define TEB_BASE    0x00500000u
#define STACK_BASE  0x01000000u
#define STACK_SIZE  0x00100000u
#define MAX_THREADS 16
#define HEAP_BASE   0x11000000u
#define INVALID     0xFFFFFFFFu
#define VROOT       "c:\\loq"
#define VOUT        "c:\\out"

struct Thread {
    CPU cpu;
    int index;
    uint32_t tid, handle, last_error;
    uint32_t tls[64];
    uint32_t start, param;
    jmp_buf exit;
};

extern int trace_on;
extern char data_dir[512], out_dir[512];
extern uint32_t g_user_text_ptr;
void runtime_init(void);
Thread *thread_new(void);
uint32_t heap_alloc(uint32_t size, int zero);
uint32_t heap_alloc_aligned(uint32_t size, uint32_t alignment);
void heap_free(uint32_t a);
uint32_t heap_size(uint32_t a);
uint32_t guest_call(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
uint32_t guest_str(const char *s);
int image_index(const char *name);
uint32_t image_load(CPU *c, int index);
uint32_t image_export(int index, const char *name);

void shims_init(void);
uint32_t shim_address(const char *name);
void call_extra_shim(CPU *c, uint32_t k);
extern uint32_t extra_shim_count;

void plat_init(void);
void plat_lock(void);
void plat_unlock(void);
void plat_broadcast(void);
int plat_wait(uint32_t ms);
void plat_sleep(uint32_t ms);
uint64_t plat_ms(void);
int plat_thread_start(void (*fn)(void *), void *arg, int join);
typedef void (*dir_cb)(const char *name, int is_dir, uint64_t size, void *user);
int plat_list_dir(const char *dir, dir_cb cb, void *user);
int plat_is_file(const char *path);
int plat_is_dir(const char *path);
char **plat_args(int *argc, char **argv);
int plat_exe_dir(char *out, size_t n);
void plat_temp_wav(char *out, size_t n);
int plat_play(const char *path);

#endif
