/*
 * Loads the Nuance libretro core without a frontend, far enough for
 * retro_load_game to run. With NUANCE_ARM64_JIT=1 and NUANCE_A64_DIFFTEST set
 * in the environment, that is where the core runs the Arm64 JIT self-test and
 * differential test (src/a64_difftest.cpp); their report goes to stderr.
 *
 *   cc -O2 -o build-libretro/a64_difftest_host tools/a64_difftest_host.c
 *   NUANCE_ARM64_JIT=1 NUANCE_A64_DIFFTEST=200000 \
 *     build-libretro/a64_difftest_host build-libretro/nuance_libretro.dylib NUON.CD
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };

static bool environment(unsigned cmd, void *data) { (void)cmd; (void)data; return false; }
static void video_refresh(const void *d, unsigned w, unsigned h, size_t p) { (void)d; (void)w; (void)h; (void)p; }
static void audio_sample(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_sample_batch(const int16_t *d, size_t f) { (void)d; return f; }
static void input_poll(void) {}
static int16_t input_state(unsigned p, unsigned d, unsigned i, unsigned id) { (void)p; (void)d; (void)i; (void)id; return 0; }

int main(int argc, char **argv)
{
  if (argc < 3) {
    fprintf(stderr, "usage: %s <nuance_libretro core> <content>\n", argv[0]);
    return 2;
  }

  void *core = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!core) {
    fprintf(stderr, "cannot load %s: %s\n", argv[1], dlerror());
    return 1;
  }

#define SYM(name) dlsym(core, name)
  void (*set_environment)(bool (*)(unsigned, void *)) = SYM("retro_set_environment");
  void (*set_video_refresh)(void (*)(const void *, unsigned, unsigned, size_t)) = SYM("retro_set_video_refresh");
  void (*set_audio_sample)(void (*)(int16_t, int16_t)) = SYM("retro_set_audio_sample");
  void (*set_audio_sample_batch)(size_t (*)(const int16_t *, size_t)) = SYM("retro_set_audio_sample_batch");
  void (*set_input_poll)(void (*)(void)) = SYM("retro_set_input_poll");
  void (*set_input_state)(int16_t (*)(unsigned, unsigned, unsigned, unsigned)) = SYM("retro_set_input_state");
  void (*init)(void) = SYM("retro_init");
  bool (*load_game)(const struct retro_game_info *) = SYM("retro_load_game");
  if (!set_environment || !init || !load_game) {
    fprintf(stderr, "%s is not a libretro core\n", argv[1]);
    return 1;
  }

  set_environment(environment);
  if (set_video_refresh) set_video_refresh(video_refresh);
  if (set_audio_sample) set_audio_sample(audio_sample);
  if (set_audio_sample_batch) set_audio_sample_batch(audio_sample_batch);
  if (set_input_poll) set_input_poll(input_poll);
  if (set_input_state) set_input_state(input_state);
  init();

  const struct retro_game_info game = { argv[2], NULL, 0, NULL };
  const bool loaded = load_game(&game);
  fprintf(stderr, "retro_load_game: %s\n", loaded ? "ok" : "failed");
  return loaded ? 0 : 1;
}
