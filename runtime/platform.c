#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdlib.h>

#include "runtime.h"

#define HOST_STACK (64u << 20)

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <process.h>
#include <shellapi.h>

static SRWLOCK lock_ = SRWLOCK_INIT;
static CONDITION_VARIABLE cond_ = CONDITION_VARIABLE_INIT;

void plat_init(void) {}
void plat_lock(void) { AcquireSRWLockExclusive(&lock_); }
void plat_unlock(void) { ReleaseSRWLockExclusive(&lock_); }
void plat_broadcast(void) { WakeAllConditionVariable(&cond_); }
int plat_wait(uint32_t ms) { return SleepConditionVariableSRW(&cond_, &lock_, ms, 0) ? 1 : 0; }
void plat_sleep(uint32_t ms) { Sleep(ms); }
uint64_t plat_ms(void) { return GetTickCount64(); }

typedef struct { void (*fn)(void *); void *arg; } Start;

static unsigned __stdcall trampoline(void *p) {
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

int plat_thread_start(void (*fn)(void *), void *arg, int join) {
    Start *s = malloc(sizeof *s);
    if (!s) return 0;
    s->fn = fn;
    s->arg = arg;
    HANDLE h = (HANDLE)_beginthreadex(NULL, HOST_STACK, trampoline, s, 0, NULL);
    if (!h) { free(s); return 0; }
    if (join) WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return 1;
}

int plat_list_dir(const char *dir, dir_cb cb, void *user) {
    char pattern[600];
    WIN32_FIND_DATAA fd;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
            cb(fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
               ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, user);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 1;
}

int plat_is_file(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int plat_is_dir(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

char **plat_args(int *argc, char **argv) {
    int n;
    LPWSTR *w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) return argv;
    char **out = calloc((size_t)n + 1, sizeof *out);
    for (int i = 0; out && i < n; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
        out[i] = len > 0 ? malloc((size_t)len) : NULL;
        if (!out[i]) { LocalFree(w); return argv; }
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, out[i], len, NULL, NULL);
    }
    LocalFree(w);
    if (!out) return argv;
    *argc = n;
    return out;
}

int plat_exe_dir(char *out, size_t n) {
    DWORD len = GetModuleFileNameA(NULL, out, (DWORD)n);
    if (!len || len >= n) return 0;
    char *slash = strrchr(out, '\\');
    if (!slash) return 0;
    *slash = 0;
    return 1;
}

void plat_temp_wav(char *out, size_t n) {
    char dir[MAX_PATH + 1];
    DWORD len = GetTempPathA(sizeof dir, dir);
    if (!len || len >= sizeof dir) snprintf(dir, sizeof dir, ".\\");
    snprintf(out, n, "%sjorge-%lu.wav", dir, (unsigned long)GetCurrentProcessId());
}

int plat_play(const char *path) { return PlaySoundA(path, NULL, SND_FILENAME | SND_SYNC | SND_NODEFAULT) != 0; }

#else
#include <dirent.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lock_ = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_ = PTHREAD_COND_INITIALIZER;

void plat_init(void) {}
void plat_lock(void) { pthread_mutex_lock(&lock_); }
void plat_unlock(void) { pthread_mutex_unlock(&lock_); }
void plat_broadcast(void) { pthread_cond_broadcast(&cond_); }

int plat_wait(uint32_t ms) {
    if (ms == INVALID) return pthread_cond_wait(&cond_, &lock_) == 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(&cond_, &lock_, &ts) == 0;
}

void plat_sleep(uint32_t ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

uint64_t plat_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

typedef struct { void (*fn)(void *); void *arg; } Start;

static void *trampoline(void *p) {
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

int plat_thread_start(void (*fn)(void *), void *arg, int join) {
    Start *s = malloc(sizeof *s);
    if (!s) return 0;
    s->fn = fn;
    s->arg = arg;
    pthread_attr_t attr;
    pthread_t t;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, HOST_STACK);
    int ok = pthread_create(&t, &attr, trampoline, s) == 0;
    pthread_attr_destroy(&attr);
    if (!ok) { free(s); return 0; }
    if (join) pthread_join(t, NULL);
    else pthread_detach(t);
    return 1;
}

int plat_list_dir(const char *dir, dir_cb cb, void *user) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[1100];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        if (stat(full, &st)) continue;
        cb(e->d_name, S_ISDIR(st.st_mode), (uint64_t)st.st_size, user);
    }
    closedir(d);
    return 1;
}

int plat_is_file(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISREG(st.st_mode);
}

int plat_is_dir(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

char **plat_args(int *argc, char **argv) { (void)argc; return argv; }

int plat_exe_dir(char *out, size_t n) {
    ssize_t len = readlink("/proc/self/exe", out, n - 1);
    if (len <= 0) return 0;
    out[len] = 0;
    char *slash = strrchr(out, '/');
    if (!slash) return 0;
    *slash = 0;
    return 1;
}

void plat_temp_wav(char *out, size_t n) {
    const char *dir = getenv("TMPDIR");
    snprintf(out, n, "%s/jorge-%d.wav", dir && *dir ? dir : "/tmp", (int)getpid());
}

int plat_play(const char *path) {
    char cmd[700];
#ifdef __APPLE__
    snprintf(cmd, sizeof cmd, "afplay '%s'", path);
#else
    snprintf(cmd, sizeof cmd, "aplay -q '%s'", path);
#endif
    return system(cmd) == 0;
}
#endif
