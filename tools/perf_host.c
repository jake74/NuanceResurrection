// Headless host for measuring the core on a machine with no display to
// offer: loads the core, loads a game, and calls retro_run sixty times a
// second for a while. No graphics context is ever given to the core, so it
// emulates and logs (NUANCE_PERF=1) but draws nothing. Used to find out how
// fast a device is before any app exists for it.
//
//   perf_host <core library> <system dir> <save dir> <seconds> <game> [key=value ...]
//
// The key=value pairs are core options. Environment variables such as
// NUANCE_PERF, NUANCE_INPUT_SCRIPT and NUANCE_LOG_FILE work as usual.
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *systemDir, *saveDir;
static char **options;
static int numOptions;
static char optionValue[256];

static void LogLine(int level, const char *fmt, ...)
{
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

struct LogCallback { void (*log)(int, const char *, ...); };
struct Variable { const char *key; const char *value; };

static bool Environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case 27: ((struct LogCallback *)data)->log = LogLine; return true;       // GET_LOG_INTERFACE
    case 9:  *(const char **)data = systemDir; return true;                  // GET_SYSTEM_DIRECTORY
    case 31: *(const char **)data = saveDir; return true;                    // GET_SAVE_DIRECTORY
    case 14: return true;                                                    // SET_HW_RENDER: accepted, never delivered
    case 10: case 11: case 16: case 18: return true;
    case 17: *(bool *)data = false; return true;                             // GET_VARIABLE_UPDATE
    case 15: {                                                               // GET_VARIABLE
        struct Variable *var = (struct Variable *)data;
        for (int i = 0; i < numOptions; i++) {
            const char *eq = strchr(options[i], '=');
            if (eq && var->key && strlen(var->key) == (size_t)(eq - options[i]) && !strncmp(options[i], var->key, eq - options[i])) {
                snprintf(optionValue, sizeof(optionValue), "%s", eq + 1);
                var->value = optionValue;
                return true;
            }
        }
        var->value = NULL;
        return false;
    }
    default: return false;
    }
}

static void Video(const void *d, unsigned w, unsigned h, size_t p) { (void)d; (void)w; (void)h; (void)p; }
static void AudioSample(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t AudioBatch(const int16_t *d, size_t frames) { (void)d; return frames; }
static void InputPoll(void) {}
static int16_t InputState(unsigned a, unsigned b, unsigned c, unsigned d) { (void)a; (void)b; (void)c; (void)d; return 0; }

struct GameInfo { const char *path; const void *data; size_t size; const char *meta; };

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: perf_host <core library> <system dir> <save dir> <seconds> <game> [key=value ...]\n");
        return 2;
    }
    systemDir = argv[2]; saveDir = argv[3];
    const double seconds = atof(argv[4]);
    options = argv + 6; numOptions = argc - 6;

    void *core = dlopen(argv[1], RTLD_NOW);
    if (!core) { fprintf(stderr, "perf_host: %s\n", dlerror()); return 1; }
    typedef void (*SetEnvironment)(bool (*)(unsigned, void *));
    typedef void (*SetVideo)(void (*)(const void *, unsigned, unsigned, size_t));
    typedef void (*SetAudioSample)(void (*)(int16_t, int16_t));
    typedef void (*SetAudioBatch)(size_t (*)(const int16_t *, size_t));
    typedef void (*SetInputPoll)(void (*)(void));
    typedef void (*SetInputState)(int16_t (*)(unsigned, unsigned, unsigned, unsigned));
    typedef void (*Plain)(void);
    typedef bool (*LoadGame)(const struct GameInfo *);
#define SYM(type, name) type name = (type)dlsym(core, #name); if (!name) { fprintf(stderr, "perf_host: no %s\n", #name); return 1; }
    SYM(SetEnvironment, retro_set_environment)
    SYM(SetVideo, retro_set_video_refresh)
    SYM(SetAudioSample, retro_set_audio_sample)
    SYM(SetAudioBatch, retro_set_audio_sample_batch)
    SYM(SetInputPoll, retro_set_input_poll)
    SYM(SetInputState, retro_set_input_state)
    SYM(Plain, retro_init)
    SYM(LoadGame, retro_load_game)
    SYM(Plain, retro_run)
    SYM(Plain, retro_unload_game)

    retro_set_environment(Environment);
    retro_set_video_refresh(Video);
    retro_set_audio_sample(AudioSample);
    retro_set_audio_sample_batch(AudioBatch);
    retro_set_input_poll(InputPoll);
    retro_set_input_state(InputState);
    retro_init();
    struct GameInfo game = { argv[5], NULL, 0, NULL };
    if (!retro_load_game(&game)) { fprintf(stderr, "perf_host: the core could not load %s\n", argv[5]); return 1; }

    struct timespec start, now, next;
    clock_gettime(CLOCK_MONOTONIC, &start);
    next = start;
    unsigned long frames = 0;
    for (;;) {
        retro_run();
        frames++;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9 >= seconds) break;
        next.tv_nsec += 16666667;
        if (next.tv_nsec >= 1000000000) { next.tv_nsec -= 1000000000; next.tv_sec++; }
        clock_gettime(CLOCK_MONOTONIC, &now);
        struct timespec wait = { next.tv_sec - now.tv_sec, next.tv_nsec - now.tv_nsec };
        if (wait.tv_nsec < 0) { wait.tv_nsec += 1000000000; wait.tv_sec--; }
        if (wait.tv_sec >= 0) nanosleep(&wait, NULL);
        else next = now; // fell behind: do not try to catch up
    }
    fprintf(stderr, "perf_host: %lu frames in %.1f s\n", frames, seconds);
    // The game's own frame count (VidSync calls), when the core exports it: the
    // frame rate of a run without NUANCE_PERF, to the frame.
    {
        const unsigned *gameFrames = (const unsigned *)dlsym(core, "perfVidSyncTotal");
        if (gameFrames) fprintf(stderr, "perf_host: game frames %u\n", *gameFrames);
    }
    // No retro_unload_game, retro_deinit or dlclose: the process is about to
    // end, and the core's own shutdown expects a graphics context it was never
    // given. Unloading crashed in VideoCleanup on every run (glIsList with no
    // context), leaving a crash report behind each time.
    fflush(stderr);
    return 0;
}
