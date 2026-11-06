#include <stdio.h>
#include <stdlib.h>

#include "runtime.h"

extern const uint16_t cp1252_unicode[256];

typedef struct { int argc; char **argv; int status; } Job;

static int tts;
static CPU *cpu;

static uint32_t api(const char *name, int nargs, const uint32_t *args) {
    uint32_t fn = image_export(tts, name);
    if (!fn) { fprintf(stderr, "engine has no export %s\n", name); exit(3); }
    return guest_call(cpu, fn, nargs, args);
}

static void check(uint32_t handle, const char *what, uint32_t code) {
    if (!code) return;
    const char *msg = "";
    if (handle) {
        uint32_t m = api("ttsGetError", 1, &handle);
        if (m) msg = (const char *)M + m;
    }
    fprintf(stderr, "error: %s failed with 0x%08X: %s\n", what, code, msg);
    exit(1);
}

static uint32_t new_obj(uint32_t parent, const char *name, int nargs, uint32_t *args) {
    uint32_t out = heap_alloc(4, 1);
    args[0] = out;
    check(parent, name, api(name, nargs, args));
    return rd32(out);
}

static uint32_t guest_text(const char *s) {
    size_t n = strlen(s);
    uint32_t a = heap_alloc((uint32_t)n + 1, 1);
    uint32_t o = a;
    for (size_t i = 0; i < n;) {
        unsigned char b = (unsigned char)s[i];
        uint32_t u = b, len = 1;
        if (b >= 0xC0 && b < 0xE0 && i + 1 < n && (s[i + 1] & 0xC0) == 0x80) { u = ((b & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu); len = 2; }
        else if (b >= 0xE0 && b < 0xF0 && i + 2 < n && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80) {
            u = ((b & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
            len = 3;
        }
        else if (b >= 0xF0 && b < 0xF8 && i + 3 < n && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80 &&
                 (s[i + 3] & 0xC0) == 0x80) {
            u = 0x10000;
            len = 4;
        }
        uint8_t out = len == 1 ? b : ' ';
        for (int k = 0; k < 256 && len > 1; k++)
            if (cp1252_unicode[k] == u) { out = (uint8_t)k; break; }
        M[o++] = out;
        i += len;
    }
    return a;
}

static void split_out(const char *path, char *dir, char *file) {
    const char *slash = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') slash = p;
    if (!slash) { snprintf(dir, 512, "."); snprintf(file, 260, "%s", path); return; }
    snprintf(dir, 512, "%.*s", (int)(slash - path), path);
    snprintf(file, 260, "%s", slash + 1);
}

static void default_data_dir(void) {
    char dir[480];
    if (plat_exe_dir(dir, sizeof dir)) {
        snprintf(data_dir, sizeof data_dir, "%s/voice", dir);
        if (plat_is_dir(data_dir)) return;
    }
    snprintf(data_dir, sizeof data_dir, "voice");
}

static int is_flag(const char *arg, const char *a, const char *b) { return !strcmp(arg, a) || (b && !strcmp(arg, b)); }

static void run(void *p) {
    Job *job = p;
    const char *save = NULL;
    size_t size = 1;
    for (int i = 1; i < job->argc; i++) size += strlen(job->argv[i]) + 1;
    char *text = calloc(size, 1);
    if (!text) { fprintf(stderr, "out of memory\n"); job->status = 3; return; }
    job->status = 2;
    default_data_dir();
    for (int i = 1; i < job->argc; i++) {
        const char *arg = job->argv[i];
        int has_value = i + 1 < job->argc;
        if (is_flag(arg, "-s", "--save") || is_flag(arg, "-o", NULL)) {
            if (!has_value) { fprintf(stderr, "%s needs a file name\n", arg); return; }
            save = job->argv[++i];
        } else if (is_flag(arg, "--data", NULL)) {
            if (!has_value) { fprintf(stderr, "--data needs a folder\n"); return; }
            snprintf(data_dir, sizeof data_dir, "%s", job->argv[++i]);
        } else if (is_flag(arg, "--trace", NULL)) {
            trace_on++;
        } else {
            if (*text) strcat(text, " ");
            strcat(text, arg);
        }
    }
    if (strspn(text, " \t\r\n") == strlen(text)) {
        fprintf(stderr, "usage: jorge [-s|--save out.wav] [--data DIR] [--trace] texto...\n");
        return;
    }
    char temp[600];
    plat_temp_wav(temp, sizeof temp);
    const char *out = save ? save : temp;
    char file[260];
    split_out(out, out_dir, file);
    if (!*file) { fprintf(stderr, "not a file name: %s\n", out); return; }
    if (!plat_is_dir(data_dir)) { fprintf(stderr, "voice folder not found: %s\n", data_dir); job->status = 2; return; }

    runtime_init();

    M[0x10220E90] = 0xAB;

    Thread *main_thread = thread_new();
    cpu = &main_thread->cpu;
    int util = image_index("LoqTTS6_util.dll");
    tts = image_index("LoqTTS6.dll");
    image_load(cpu, util);
    image_load(cpu, tts);

    uint32_t a[6];
    a[1] = 0;
    uint32_t session = new_obj(0, "ttsNewSession", 2, a);
    a[1] = session; a[2] = 0;
    uint32_t instance = new_obj(session, "ttsNewInstance", 3, a);
    uint32_t inst_data = rd32(instance + 4);
    wr8(inst_data + 0x4BE5, 1);
    wr8(inst_data + 0x4BF5, 1);
    wr8(instance + 0x2C, 0);
    a[1] = instance; a[2] = guest_str("Jorge"); a[3] = 16000; a[4] = guest_str("loq210");
    new_obj(instance, "ttsNewVoice", 5, a);

    wr8(inst_data + 0x4BE4, 0);
    wr32(inst_data + 0x4BE8, 0);

    uint32_t param[3] = { instance, guest_str("ProcessingMode"), guest_str("blocking") };
    check(instance, "ttsSetInstanceParam", api("ttsSetInstanceParam", 3, param));

    char dest[300];
    snprintf(dest, sizeof dest, "%s\\%s", VOUT, file);
    uint32_t audio[5] = { instance, guest_str("LoqAudioFile"), guest_str(dest), 0, 0 };
    check(instance, "ttsSetAudio", api("ttsSetAudio", 5, audio));

    uint32_t user_text = guest_text(text);
    g_user_text_ptr = user_text;
    uint32_t read[6] = { instance, user_text, 0, 0, 0, 0 };
    check(instance, "ttsRead", api("ttsRead", 6, read));
    check(0, "ttsDeleteInstance", api("ttsDeleteInstance", 1, &instance));
    if (save) {
        printf("%s\n", save);
        job->status = 0;
        return;
    }
    int played = plat_play(temp);
    remove(temp);
    if (!played) { fprintf(stderr, "cannot play audio\n"); job->status = 1; return; }
    job->status = 0;
}

int main(int argc, char **argv) {
    argv = plat_args(&argc, argv);
    Job job = { argc, argv, 3 };
    if (!plat_thread_start(run, &job, 1)) { fprintf(stderr, "cannot start worker thread\n"); return 3; }
    fflush(stdout);
    exit(job.status);
}
