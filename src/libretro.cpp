// NuanceResurrection libretro core
// Wraps the NUON emulator for use in RetroArch

#include "libretro.h"
#include "basetypes.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include "gl_headers.h"
// GL/gl.h not needed; the header above already brings in every entry point
// this file calls, whether that is GLEW on the desktop or the platform's own
// GLES headers.
#include <mutex>
#include <thread>
#include <atomic>
#include <sched.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
#include <cstdarg>
#ifdef _WIN32
#include <direct.h>     // _mkdir
#include <windows.h>    // GetModuleHandleEx / GetModuleFileName
#else
#include <dlfcn.h>      // dladdr / Dl_info
#endif

#include "byteswap.h"
#include "Utility.h"
#include "comm.h"
#include "audio.h"
#include "mpe.h"
#include "NuonEnvironment.h"
#include "NuonMemoryMap.h"
#include "joystick.h"
#include "video.h"
#include "ExecuteMEM.h"
#include "timer.h"
#include "Bios.h"
#include "archive.h"
#include "NativeCodeCache.h"

extern unsigned perfVidSyncCalls, perfVidChangeBaseCalls, perfVidSyncFields; // video.cpp
static uint64_t perfAudioFrames = 0;
static uint64 perf_packets[4] = {}; // packets executed per MPE since the last NUANCE_PERF report

// Emulation thread state (see "Emulation thread" below).
static std::thread emuThread;
static bool emuThreaded = false;
static bool emuThreadWanted = false;
static std::atomic<bool> emuThreadStop{false};
static std::atomic<bool> emuMainWants{false};  // the main thread wants the emulation held
static std::atomic<bool> emuParked{true};      // the emulation thread is not inside RunEmulation
static std::atomic<uint64_t> emuLastRunCallUs{0};
static void StartEmuThread();
static void StopEmuThread();
static bool HoldEmuThread();    // true if there is a thread and it is now held
static void ReleaseEmuThread();

// GLWindow stubs for libretro (no windowing needed)
#include "GLWindow.h"
GLWindow::GLWindow() : bFullScreen(false), bVisible(false), keyDownHandler(nullptr), keyUpHandler(nullptr),
    paintHandler(nullptr), resizeHandler(nullptr), applyControllerState(nullptr), inputManager(nullptr),
    clientWidth(720), clientHeight(480), x(0), y(0), width(720), height(480),
    fullScreenWidth(1920), fullScreenHeight(1080), hInstance(nullptr), hWnd(nullptr), hDC(nullptr), hRC(nullptr),
    windowStyle(0), windowExtendedStyle(0), fullScreenWindowStyle(0), fullScreenWindowExtendedStyle(0),
    threadHandle(0), threadID(0), restoreWidth(720), restoreHeight(480), restoreX(0), restoreY(0) {}
GLWindow::~GLWindow() {}
void GLWindow::UpdateRestoreValues() {}
void GLWindow::ToggleFullscreen() {}
bool GLWindow::ChangeScreenResolution(int,int) { return false; }
bool GLWindow::CreateWindowGL() { return false; }
void GLWindow::CleanUp() {}
bool GLWindow::RegisterWindowClass() { return false; }
bool GLWindow::Create() { return false; }
void GLWindow::MessagePump() {}
void GLWindow::OnResize(int,int) {}
unsigned GLWindow::GLWindowMain(void*) { return 0; }
GLWindow display;

// Globals
NuonEnvironment nuonEnv;
bool bQuit = false;
bool bRun = false;
std::string g_ISOPath;
std::string g_ISOPrefix;

extern ControllerData *controller;
extern VidChannel structMainChannel, structOverlayChannel;
extern bool bOverlayChannelActive, bMainChannelActive;
extern vidTexInfo videoTexInfo;

// Libretro callbacks
static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_hw_render_callback hw_render;

static bool gl_initialized = false;
static bool game_loaded = false;

// Audio buffer
#define AUDIO_BUFFER_SIZE 4096
static int16_t audio_buffer[AUDIO_BUFFER_SIZE];

// Software framebuffer fallback
#define FB_WIDTH 720
#define FB_HEIGHT 480
static uint32_t framebuffer[FB_WIDTH * FB_HEIGHT];

// Into the frontend's log when it has one - stderr goes nowhere for a
// RetroArch started from a Windows desktop, so a report from there carried
// none of these lines - and to stderr otherwise.
static retro_log_printf_t log_cb = nullptr;

static void log_printf(const char* fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    // NUANCE_LOG_FILE=<path>: also append to a file of the core's own. RetroArch
    // closes its --log-file when it reloads its configuration for a core override,
    // which is what happens when a game is started from the menu.
    static FILE* const ownLog = getenv("NUANCE_LOG_FILE") ? fopen(getenv("NUANCE_LOG_FILE"), "a") : nullptr;
    if (ownLog)
    {
        fputs(line, ownLog);
        fflush(ownLog);
    }
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "%s", line);
    else
        fputs(line, stderr);
}

// For the rest of the core: a shader that does not compile, and the like.
void libretro_log_info(const char* text)
{
    log_printf("libretro: %s\n", text);
}

void libretro_log_message(const char* caption, const char* text)
{
    if (log_cb)
        log_cb(RETRO_LOG_ERROR, "libretro: %s: %s\n", caption ? caption : "message", text ? text : "");
    else
        fprintf(stderr, "[%s] %s\n", caption ? caption : "message", text ? text : "");
}

// Stub functions needed by other modules
void StopEmulation(int) { bRun = false; }

// InputManager stubs for libretro (input handled by retro_input_state)
#include "InputManager.h"
InputManager::~InputManager() {}
InputManager* InputManager::Create() { return nullptr; }
bool InputManager::StrToInputType(const char* str, InputType* type) {
    if (!strcasecmp("KEY", str)) { *type = KEY; return true; }
    if (!strcasecmp("JOYBUT", str)) { *type = JOYBUT; return true; }
    if (!strcasecmp("JOYAXIS", str)) { *type = JOYAXIS; return true; }
    if (!strcasecmp("JOYPOV", str)) { *type = JOYPOV; return true; }
    return false;
}
const char* InputManager::InputTypeToStr(InputType type) {
    switch (type) { case KEY: return "KEY"; case JOYBUT: return "JOYBUT"; case JOYAXIS: return "JOYAXIS"; case JOYPOV: return "JOYPOV"; default: return ""; }
}

static void ApplyControllerState(unsigned int controllerIdx, uint16 buttons)
{
    if (controller)
        controller[controllerIdx].buttons = SwapBytes(buttons);
}

static uint16 GetInputButtons()
{
    uint16 buttons = 0;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))    buttons |= CTRLR_DPAD_UP;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))  buttons |= CTRLR_DPAD_DOWN;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))  buttons |= CTRLR_DPAD_LEFT;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT)) buttons |= CTRLR_DPAD_RIGHT;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))     buttons |= CTRLR_BUTTON_A;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))     buttons |= CTRLR_BUTTON_B;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START)) buttons |= CTRLR_BUTTON_START;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT))buttons |= CTRLR_BUTTON_NUON;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L))     buttons |= CTRLR_BUTTON_L;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R))     buttons |= CTRLR_BUTTON_R;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X))     buttons |= CTRLR_BUTTON_C_UP;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))     buttons |= CTRLR_BUTTON_C_LEFT;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2))    buttons |= CTRLR_BUTTON_C_DOWN;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2))    buttons |= CTRLR_BUTTON_C_RIGHT;

    // NUANCE_AUTOPRESS=<frames>[,<buttons>]: hold the named buttons (START if
    // none are named; any of start a b left right up down, separated by '+')
    // for ten frames out of every <frames>, so an unattended run gets past
    // title and prompt screens and into a game. For benchmarking and
    // automated checks; nobody plays like this.
    static int autoPress = -1;
    static uint16 autoButtons = 0;
    if (autoPress < 0) {
        const char* const spec = getenv("NUANCE_AUTOPRESS");
        autoPress = spec ? atoi(spec) : 0;
        const char* const names = spec ? strchr(spec, ',') : nullptr;
        if (!names) autoButtons = CTRLR_BUTTON_START;
        else {
            if (strstr(names, "start")) autoButtons |= CTRLR_BUTTON_START;
            if (strstr(names, "left"))  autoButtons |= CTRLR_DPAD_LEFT;
            if (strstr(names, "right")) autoButtons |= CTRLR_DPAD_RIGHT;
            if (strstr(names, "up"))    autoButtons |= CTRLR_DPAD_UP;
            if (strstr(names, "down"))  autoButtons |= CTRLR_DPAD_DOWN;
            if (strstr(names, "+a") || strstr(names, ",a")) autoButtons |= CTRLR_BUTTON_A;
            if (strstr(names, "+b") || strstr(names, ",b")) autoButtons |= CTRLR_BUTTON_B;
        }
    }
    if (autoPress > 10) {
        // NUANCE_AUTOPRESS_START_UNTIL=<frame>: leave START out after that many
        // polls. START gets a run into a game, but in the game it is the pause
        // button, and a run that keeps pressing it measures a game that is
        // paused half the time.
        static const int startUntil = getenv("NUANCE_AUTOPRESS_START_UNTIL") ? atoi(getenv("NUANCE_AUTOPRESS_START_UNTIL")) : 0;
        static int autoPressFrame = 0;
        if ((++autoPressFrame % autoPress) < 10)
            buttons |= (startUntil && autoPressFrame > startUntil) ? (autoButtons & ~CTRLR_BUTTON_START) : autoButtons;
    }
    return buttons;
}

static void context_reset(void)
{
#if defined(NUANCE_GLES)
    log_printf("libretro: GLES context, no loader needed\n");
#else
    glewExperimental = GL_TRUE;
    GLenum glew_err = glewInit();
    // GLEW often reports GLEW_ERROR_NO_GL_VERSION on core/forward-compatible contexts
    // because glGetString(GL_VERSION) is unavailable; the function pointers still load.
    if (glew_err != GLEW_OK && glew_err != 4 /* GLEW_ERROR_NO_GL_VERSION */) {
        log_printf("libretro: glewInit failed: %s\n", glewGetErrorString(glew_err));
        gl_initialized = false;
        return;
    }
    log_printf("libretro: GL context reset OK (GL %s)\n", (const char*)glGetString(GL_VERSION));
#endif

    gl_initialized = true;

    // The GL context was just (re)created. RetroArch can call context_reset more
    // than once per session (e.g. it tears down and rebuilds the GL context on a
    // windowed->fullscreen switch, which happens when its primary video driver is
    // Vulkan). All GL object names from the previous context are now dead, so tell
    // the video module to rebuild its textures/shaders; otherwise it keeps the
    // one-time-init guards set and renders with stale handles -> black screen.
    VideoInvalidateGLState();

    // Init CPU extensions and lookup tables (deferred from retro_load_game)
    static bool tables_initialized = false;
    if (!tables_initialized) {
        init_supported_CPU_extensions();
        GenerateMirrorLookupTable();
        GenerateSaturateColorTables();
        tables_initialized = true;
        log_printf("libretro: tables initialized in context_reset\n");
    }

    // Setup GL state
    glViewport(0, 0, FB_WIDTH, FB_HEIGHT);
#if !defined(NUANCE_GLES)
    // The matrix stack is only still here for the no-shader fallback: the
    // shader path gets the same projection through the u_mvp uniform, and GLES
    // has no fixed-function matrices at all.
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 1.0, 0.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
#endif
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 1);
}

static void context_destroy(void)
{
    gl_initialized = false;
    // GL objects die with the context; make sure they are rebuilt after the next
    // context_reset rather than reused with stale handles.
    VideoInvalidateGLState();
}

// Analog controls. A NUON controller can carry an analog stick or a
// quadrature spinner besides its buttons, and Tempest 3000 reads both. The
// spinner is the interesting one: the arcade Tempest was played with a
// weighted rotary knob, and "spinner" here is that knob driven from a
// gamepad. The left stick pushes a flywheel round - the further over, the
// faster it is driven - and the flywheel keeps turning for a moment when the
// stick is let go, the more so the heavier it is set. A mouse or trackpad
// turns it directly: that already is a hand on a knob.
// (The libretro.h in this tree is a cut-down one; these are the standard values.)
#ifndef RETRO_DEVICE_MOUSE
#define RETRO_DEVICE_MOUSE 2
#endif
#ifndef RETRO_DEVICE_ANALOG
#define RETRO_DEVICE_ANALOG 5
#endif
#ifndef RETRO_DEVICE_INDEX_ANALOG_LEFT
#define RETRO_DEVICE_INDEX_ANALOG_LEFT 0
#define RETRO_DEVICE_ID_ANALOG_X 0
#define RETRO_DEVICE_ID_ANALOG_Y 1
#endif
#ifndef RETRO_DEVICE_ID_MOUSE_X
#define RETRO_DEVICE_ID_MOUSE_X 0
#endif
extern uint32 nuonControllerProperties; // joystick.cpp
enum { CONTROL_DPAD, CONTROL_SPINNER, CONTROL_ANALOG };
static int controlMode = CONTROL_DPAD;
static float spinnerSensitivity = 1.0f; // 1 = full stick turns the knob about one revolution of a 16-lane web a second
static float spinnerInertia = 0.5f;     // 0 = stops with the stick, 1 = barely slows
static float spinnerDirection = 1.0f;
static float spinnerVelocity = 0.0f;    // counts per frame
static double spinnerPosition = 0.0;    // counts; the game sees the low 8 bits

static void ApplyAnalogControls()
{
    if (!controller || controlMode == CONTROL_DPAD)
        return;

    float x = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X) / 32767.0f;
    float y = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y) / 32767.0f;
    x = x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
    y = y < -1.0f ? -1.0f : (y > 1.0f ? 1.0f : y);

    if (controlMode == CONTROL_ANALOG) {
        controller[1].d1.xAxis = (int8)(x * 127.0f);
        controller[1].d2.yAxis = (int8)(-y * 127.0f);
        return;
    }

    // Dead zone, then a squared response: small movements for fine aim, the
    // full throw for a fast spin.
    constexpr float deadZone = 0.12f;
    float drive = 0.0f;
    if (x > deadZone || x < -deadZone) {
        const float mag = ((x < 0 ? -x : x) - deadZone) / (1.0f - deadZone);
        drive = (x < 0 ? -1.0f : 1.0f) * mag * mag;
    }

    // The game reads an 8-bit count and takes the difference between reads,
    // a few frames apart, so the knob must not turn more than this per frame
    // or the difference wraps and the claw jumps backwards.
    constexpr float maxCountsPerFrame = 40.0f;
    const float target = drive * maxCountsPerFrame * spinnerSensitivity * spinnerDirection;
    if (drive != 0.0f) {
        // Driven: the flywheel is brought up to speed, slower when heavier.
        const float grip = 1.0f - 0.85f * spinnerInertia;
        spinnerVelocity += (target - spinnerVelocity) * grip;
    } else {
        // Coasting: friction only.
        const float keep = (spinnerInertia <= 0.0f) ? 0.0f : 0.60f + 0.39f * spinnerInertia;
        spinnerVelocity *= keep;
        if (spinnerVelocity > -0.05f && spinnerVelocity < 0.05f)
            spinnerVelocity = 0.0f;
    }
    if (spinnerVelocity > maxCountsPerFrame) spinnerVelocity = maxCountsPerFrame;
    if (spinnerVelocity < -maxCountsPerFrame) spinnerVelocity = -maxCountsPerFrame;

    // Mouse or trackpad: straight onto the knob, no flywheel of ours.
    float mouse = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X) * 1.5f * spinnerSensitivity * spinnerDirection;
    if (mouse > maxCountsPerFrame) mouse = maxCountsPerFrame;
    if (mouse < -maxCountsPerFrame) mouse = -maxCountsPerFrame;

    spinnerPosition += spinnerVelocity + mouse;
    controller[1].d5.spinner1 = (int8)(uint8)((long long)spinnerPosition & 0xFF);
}

// Core options (see retro_set_environment). Called once the game is loaded,
// after nuance.cfg has been read, and again whenever the frontend says an
// option has changed.
void VideoReinstallShaders(); // video.cpp
static void ApplyCoreOptions()
{
    struct retro_variable var = { "nuance_scanlines", nullptr };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && strcmp(var.value, "config file") != 0) {
        const bool bWanted = strcmp(var.value, "enabled") == 0;
        if (bWanted != nuonEnv.bUseCRTshader) {
            nuonEnv.bUseCRTshader = bWanted;
            VideoReinstallShaders();
        }
    }
    var = { "nuance_controller", nullptr };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        const int mode = !strcmp(var.value, "spinner") ? CONTROL_SPINNER : (!strcmp(var.value, "analog stick") ? CONTROL_ANALOG : CONTROL_DPAD);
        const uint32 properties = KEYBOARD_JOYSTICK_PROPERTIES | (mode == CONTROL_SPINNER ? CTRLR_QUADSPINNER1 : 0) | (mode == CONTROL_ANALOG ? CTRLR_ANALOG1 : 0);
        if (mode != controlMode || properties != nuonControllerProperties) {
            controlMode = mode;
            nuonControllerProperties = properties;
            spinnerVelocity = 0.0f;
            if (controller) {
                // A different controller has been plugged in, as far as the game can tell.
                controller[1].d1.xAxis = 0;
                controller[1].d2.yAxis = 0;
                controller[1].config = SwapBytes((uint32)CONTROLLER_CHANGED_BIT | properties);
            }
        }
    }
    var = { "nuance_skip_controller_message", nullptr };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        extern bool nuonControllerAnnounceAtBoot; // read when the BIOS sets the controller up
        nuonControllerAnnounceAtBoot = strcmp(var.value, "enabled") != 0;
    }
    var = { "nuance_spinner_sensitivity", nullptr };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        spinnerSensitivity = atoi(var.value) / 100.0f;
    var = { "nuance_spinner_inertia", nullptr };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        spinnerInertia = atoi(var.value) / 100.0f;
    var = { "nuance_spinner_invert", nullptr };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        spinnerDirection = !strcmp(var.value, "reversed") ? -1.0f : 1.0f;
    var = { "nuance_max_fps", nullptr };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && strcmp(var.value, "config file") != 0) {
        const int fps = atoi(var.value); // "unlimited" reads as 0
        nuonEnv.minFieldsPerFrame = (fps > 0 && fps < 60) ? (uint32)((60 + fps / 2) / fps) : 0;
    }
}

// --- Libretro API ---

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;
    struct retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        log_cb = logging.log;
    bool no_game = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

    // Core options, so that a frontend can set these without a nuance.cfg.
    // "config file" leaves the value from nuance.cfg (or the built-in default).
    static const struct retro_variable variables[] = {
        { "nuance_scanlines", "Scanlines (CRT shader); config file|enabled|disabled" },
        { "nuance_max_fps", "Maximum game frame rate; config file|unlimited|30|20|15" },
        { "nuance_controller", "Controller; d-pad only|spinner|analog stick" },
        { "nuance_skip_controller_message", "Skip the 'controller configuration has changed' screen; disabled|enabled" },
        { "nuance_spinner_sensitivity", "Spinner sensitivity; 100%|10%|20%|30%|40%|50%|60%|70%|80%|90%|110%|120%|130%|140%|150%|175%|200%|250%|300%|400%" },
        { "nuance_spinner_inertia", "Spinner weight (inertia); 50%|0%|10%|20%|30%|40%|60%|70%|80%|90%|100%" },
        { "nuance_spinner_invert", "Spinner direction; normal|reversed" },
        { nullptr, nullptr }
    };
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)variables);

    static struct retro_input_descriptor desc[] = {
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "NUON" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L Shoulder" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R Shoulder" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "C-Up" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "C-Left" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "C-Down" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "C-Right" },
        { 0, 0, 0, 0, NULL },
    };
    cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "Nuance";
    info->library_version = "0.6.7";
    info->valid_extensions = "run|cof|nuon|cd|iso|img|chd";
    info->need_fullpath = true;
    info->block_extract = false; // let RetroArch extract ZIPs for us
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    info->geometry.base_width = FB_WIDTH;
    info->geometry.base_height = FB_HEIGHT;
    info->geometry.max_width = FB_WIDTH;
    info->geometry.max_height = FB_HEIGHT;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps = VIDEO_HZ;
    info->timing.sample_rate = 32000.0;
}

void retro_init(void)
{
    log_printf("libretro: retro_init start\n");
    fflush(stderr);
    // Defer heavy init to retro_load_game
    log_printf("libretro: retro_init done\n");
    fflush(stderr);
}

void retro_deinit(void)
{
    StopEmuThread();
    log_printf("libretro: retro_deinit\n");
    CleanupArchives();
}

void retro_set_controller_port_device(unsigned, unsigned) {}

void retro_reset(void)
{
    const bool bHeld = HoldEmuThread();
    for (int i = 0; i < 4; i++)
        nuonEnv.mpe[i].Reset();
    if (bHeld)
        ReleaseEmuThread();
}

bool retro_load_game(const struct retro_game_info *game)
{
    if (!game || !game->path) return false;

    log_printf("libretro: loading %s\n", game->path); fflush(stderr);

    // Defer CPU init to context_reset to avoid potential issues

    // Request OpenGL context
#if defined(NUANCE_GLES)
    // Android, iOS and tvOS have OpenGL ES and nothing else. Version 3 rather
    // than 2 for one reason: the texture formats. GL_RGBA8, GL_RG8 and GL_RG
    // are what the video buffers are declared with, and ES2 has no sized
    // internal formats and no two-channel one at all.
    hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES3;
#else
    hw_render.context_type = RETRO_HW_CONTEXT_OPENGL;
#endif
    hw_render.context_reset = context_reset;
    hw_render.context_destroy = context_destroy;
    hw_render.depth = false;
    hw_render.stencil = false;
    hw_render.bottom_left_origin = true;
    hw_render.version_major = 2;
    hw_render.version_minor = 1;
    hw_render.cache_context = true;

    log_printf("libretro: requesting HW render...\n"); fflush(stderr);
    if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render)) {
        log_printf("libretro: HW render not available, using software\n"); fflush(stderr);
    } else {
        log_printf("libretro: HW render OK\n"); fflush(stderr);
    }

    // Find game file
    log_printf("libretro: ResolveGameFile(%s)...\n", game->path); fflush(stderr);
    std::string gamePath = ResolveGameFile(game->path);
    log_printf("libretro: ResolveGameFile returned: '%s'\n", gamePath.c_str()); fflush(stderr);
    if (gamePath.empty()) {
        log_printf("libretro: cannot find NUON game in %s\n", game->path);
        return false;
    }

    log_printf("libretro: game file found: %s\n", gamePath.c_str()); fflush(stderr);

    // Initialize emulator
    // Find bios files - check system directory BEFORE Init (bios.cof is loaded during Init)
    const char* sysDir = nullptr;
    environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sysDir);
    if (sysDir) {
        std::string biosPath = std::string(sysDir) + "/nuance/bios.cof";
        if (access(biosPath.c_str(), F_OK) == 0) {
            chdir((std::string(sysDir) + "/nuance").c_str());
            log_printf("libretro: bios dir: %s/nuance\n", sysDir); fflush(stderr);
        }
    }
    // Also check build directory (for development)
    if (access("bios.cof", F_OK) != 0) {
        // Try the directory containing the core library
        std::string soDir;
#ifdef _WIN32
        HMODULE hm = nullptr;
        char modPath[MAX_PATH];
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(void*)retro_init, &hm) &&
            GetModuleFileNameA(hm, modPath, sizeof(modPath))) {
            soDir = modPath;
            size_t pos = soDir.find_last_of("/\\");
            if (pos != std::string::npos) soDir = soDir.substr(0, pos);
            else soDir.clear();
        }
#else
        Dl_info dl_info;
        if (dladdr((void*)retro_init, &dl_info) && dl_info.dli_fname) {
            soDir = dl_info.dli_fname;
            size_t pos = soDir.rfind('/');
            if (pos != std::string::npos) soDir = soDir.substr(0, pos);
            else soDir.clear();
        }
#endif
        if (!soDir.empty() && access((soDir + "/bios.cof").c_str(), F_OK) == 0) {
            chdir(soDir.c_str());
            log_printf("libretro: bios dir (from core lib): %s\n", soDir.c_str()); fflush(stderr);
        }
    }

    log_printf("libretro: nuonEnv.Init()...\n"); fflush(stderr);
    ApplyCoreOptions(); // the controller's kind has to be known before the BIOS sets it up
    nuonEnv.Init();
    log_printf("libretro: nuonEnv.Init() done\n"); fflush(stderr);
    ApplyCoreOptions();

    // Saves (the emulated flash memory, where games keep high scores and
    // progress): one file per game in the frontend's save directory. Without
    // a save directory, FlashEEPROM.bin beside the BIOS files as before - which
    // is also where a save from before this was per game is picked up from.
    {
        const char* saveDir = nullptr;
        environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &saveDir);
        std::string name = game->path;
        const size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos) name = name.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
        char cwd[1024] = {};
        const std::string legacy = std::string(getcwd(cwd, sizeof(cwd)) ? cwd : ".") + "/FlashEEPROM.bin";
        const std::string saveFile = (saveDir && *saveDir) ? std::string(saveDir) + "/" + name + ".nuonsave" : legacy;
        nuonEnv.flashEEPROM.UseSaveFile(saveFile.c_str(), legacy.c_str());
        log_printf("libretro: saves in %s\n", saveFile.c_str());
    }

#ifdef USE_A64_JIT
    // The Arm64 emitter is opt-in. When asked for, prove on this machine that
    // generated code can be written, made executable and run before any game
    // code depends on it; otherwise stay with the interpreter.
    if (const char* const chain = getenv("NUANCE_A64_CHAIN"))
        a64ChainPackets = (unsigned)atoi(chain);
    if (const char* const v = getenv("NUANCE_A64_IRAM_THRESHOLD"))
        a64IramThreshold = (unsigned)atoi(v);
    if (const char* const v = getenv("NUANCE_A64_SLOT_BRANCHES"))
        a64CompileSlotBranches = (unsigned)atoi(v);
    if (const char* const v = getenv("NUANCE_A64_RECOMPILE_MARKED"))
        a64RecompileMarked = (unsigned)atoi(v);
    if (nuonEnv.compilerOptions.bArm64NativeCode) {
        char selfTest[256];
        const bool passed = A64_JitSelfTest(selfTest, sizeof(selfTest));
        log_printf("libretro: Arm64 JIT self-test: %s\n", selfTest);
        if (!passed) {
            nuonEnv.compilerOptions.bArm64NativeCode = false;
            log_printf("libretro: Arm64 JIT disabled, using the interpreter\n");
        } else if (const char* const diffTest = getenv("NUANCE_A64_DIFFTEST")) {
            // <trials>[,<seed>]: check the emitter against the interpreter on random code.
            unsigned trials = 0, seed = 1;
            sscanf(diffTest, "%u,%u", &trials, &seed);
            A64_DiffTest(trials, seed);
        }
    } else {
        log_printf("libretro: Arm64 JIT available but off (set NUANCE_ARM64_JIT=1 or [Arm64NativeCode] to try it)\n");
    }
#endif

    // Load game
    bool ok = nuonEnv.mpe[3].LoadNuonRomFile(gamePath.c_str());
    if (!ok) ok = nuonEnv.mpe[3].LoadCoffFile(gamePath.c_str());
    if (!ok) {
        log_printf("libretro: failed to load %s\n", gamePath.c_str());
        return false;
    }

    nuonEnv.SetDVDBaseFromFileName(gamePath.c_str());
    for(int i=0;i<4;i++) log_printf("libretro: mpe%d inten1=%08X inten2sel=%u intsrc=%08X intctl=%08X mpectl=%08X\n", i, nuonEnv.mpe[i].inten1, nuonEnv.mpe[i].inten2sel, nuonEnv.mpe[i].intsrc, nuonEnv.mpe[i].intctl, nuonEnv.mpe[i].mpectl); fflush(stderr);
    nuonEnv.mpe[3].Go();
    // Process any pending comm requests from BIOS init before starting emulation
    while (nuonEnv.pendingCommRequests) {
        log_printf("libretro: processing %d pending comm requests from init\n", nuonEnv.pendingCommRequests); fflush(stderr);
        DoCommBusController();
    }
    bRun = true;
    game_loaded = true;

    {
        bool bWantThread = false;
#ifdef USE_A64_JIT
        bWantThread = nuonEnv.compilerOptions.bArm64NativeCode;
#endif
        if (const char* const choice = getenv("NUANCE_EMU_THREAD"))
            bWantThread = (atoi(choice) != 0);
        // Started by the first retro_run, not here: a host that only loads a
        // game (the differential test's) never needs it.
        emuThreadWanted = bWantThread;
    }

    log_printf("libretro: game loaded successfully\n");
    return true;
}

bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

void retro_unload_game(void)
{
    StopEmuThread();
    nuonEnv.flashEEPROM.SaveIfChanged();
    bRun = false;
    game_loaded = false;
    VideoCleanup();
}

// The emulation proper: the four MPEs in turn, the comm bus between them, and
// the events that happen on the clock (timers, the video field, audio). Called
// from retro_run once per frame, or - with the emulation thread - run
// continuously on that thread, where it returns only to let the main thread in
// or because the frontend has stopped asking for frames.
static void RunEmulation(const bool bThreaded)
{
    nuonEnv.trigger_render_video = false;
    uint32 cycles = 0;
    static uint64 last_time0 = useconds_since_start();
    static uint64 last_time1 = useconds_since_start();
    static uint64 last_time2 = useconds_since_start();
    static uint64 last_time3 = useconds_since_start();
    const uint64 frame_budget_start = useconds_since_start();
    // During BIOS init (before video is configured), allow longer execution time
    // Use 100ms budget for first 100 frames to allow BIOS init to complete
    static int init_frames = 0;
    const uint64 frame_budget_us = (init_frames < 100) ? 50000 : 16000;
    init_frames++;
    // No MPE runs faster than the real one: an Aries MPE executes at most one
    // packet per cycle of its 54 MHz clock. Games that do not wait for the
    // display, Tempest 3000 among them, otherwise play too fast once the host
    // can outrun the hardware. Each MPE earns packets with elapsed time and
    // sits out its turns while it has none, which also leaves the host's time
    // to the MPEs that are behind. NUANCE_MPE_HZ=<packets per second>
    // changes the rate; 0 removes the limit.
    static const int64_t mpeHz = getenv("NUANCE_MPE_HZ") ? atoll(getenv("NUANCE_MPE_HZ")) : 54000000;
    static int64_t mpeCredit[4] = { 1, 1, 1, 1 };
    static uint64 mpeCreditTime = useconds_since_start();
    while (bRun && (bThreaded ? (!emuMainWants.load(std::memory_order_relaxed) && !emuThreadStop.load(std::memory_order_relaxed)) : !nuonEnv.trigger_render_video))
    {
        cycles++;

        // MPE3 takes its turn only in a round where all the others take theirs.
        // Tempest 3000 hangs if MPE3 gains on another MPE (see bMPE3PacketHack),
        // which it would if it kept running while they sat out. It ends up
        // slower than without the limit; the game's frame rate does not notice.
        const bool bOthersRun = !mpeHz || (mpeCredit[0] > 0 && mpeCredit[1] > 0 && mpeCredit[2] > 0);
        // Nothing to run until more time has passed: go and look at the clock.
        const bool bIdleRound = mpeHz && mpeCredit[0] <= 0 && mpeCredit[1] <= 0 && mpeCredit[2] <= 0;
        for (int i = 3; i >= 0; --i)
            if ((i == 3) ? (bOthersRun && nuonEnv.MPE3wait_fieldCounter == 0) : (!mpeHz || mpeCredit[i] > 0)) {
                nuonEnv.mpe[i].FetchDecodeExecute();
                perf_packets[i] += nuonEnv.mpe[i].cycleCounter;
                mpeCredit[i] -= nuonEnv.mpe[i].cycleCounter;
            }

        if (nuonEnv.pendingCommRequests)
            DoCommBusController();

        if ((cycles % 5000) == 0 || bIdleRound)
        {
            if (bIdleRound && bThreaded)
                usleep(50);
            const uint64 new_time = useconds_since_start();
            {
                // NUANCE_PERF: report any gap of more than 20 ms between two looks
                // at the clock - the emulation stood still for that long.
                static uint64 lastCheck = 0;
                static const bool bStalls = getenv("NUANCE_PERF") != nullptr;
                if (bStalls && lastCheck && new_time - lastCheck > 20000)
                    log_printf("libretro: perf: emulation stalled for %.0f ms (MPE pcs %08X %08X %08X %08X)\n", (new_time - lastCheck) / 1000.0,
                        nuonEnv.mpe[0].pcexec, nuonEnv.mpe[1].pcexec, nuonEnv.mpe[2].pcexec, nuonEnv.mpe[3].pcexec);
                lastCheck = new_time;
            }
            if (mpeHz) {
                // At most 4 ms of credit, so a stall is not followed by a burst.
                const int64_t earned = (int64_t)((new_time - mpeCreditTime) * (uint64)mpeHz / 1000000);
                const int64_t most = (int64_t)mpeHz / 250;
                for (int i = 0; i < 4; i++)
                    mpeCredit[i] = (mpeCredit[i] + earned > most) ? most : mpeCredit[i] + earned;
                mpeCreditTime = new_time;
            }
            // On its own thread the emulation has no frame to return for; it only
            // stops when the frontend has stopped asking for frames (paused, or
            // in its menu), so that a paused game does not run on.
            if (bThreaded) {
                if (new_time - emuLastRunCallUs.load(std::memory_order_relaxed) > 300000)
                    break;
            } else
            // Time-based frame budget: break after ~16ms to return control to retroarch
            if ((new_time - frame_budget_start) >= frame_budget_us) {
                static int flog = 0;
                if (flog < 5) { fprintf(stderr, "FRAME[%d]: %u cycles in %lums budget=%lums\n", init_frames, cycles, (unsigned long)(new_time - frame_budget_start)/1000, (unsigned long)frame_budget_us/1000); flog++; }
                break;
            }

            if (nuonEnv.timer_rate[0] > 0 && new_time >= last_time0 + (uint64)nuonEnv.timer_rate[0]) {
                nuonEnv.ScheduleInterrupt(INT_SYSTIMER0); last_time0 = new_time;
            } else if (nuonEnv.timer_rate[0] <= 0) last_time0 = new_time;

            if (nuonEnv.timer_rate[1] > 0 && new_time >= last_time1 + (uint64)nuonEnv.timer_rate[1]) {
                nuonEnv.ScheduleInterrupt(INT_SYSTIMER1); last_time1 = new_time;
            } else if (nuonEnv.timer_rate[1] <= 0) last_time1 = new_time;

            {
                // Video field counter update: use timer_rate[2] if set, otherwise default ~60Hz (16667us)
                const uint64 vidRate = (nuonEnv.timer_rate[2] > 0) ? (uint64)nuonEnv.timer_rate[2] : 16667;
                if (new_time >= last_time2 + vidRate) {
                    IncrementVideoFieldCounter();
                    nuonEnv.TriggerVideoInterrupt();
                    nuonEnv.trigger_render_video = true;
                    const uint32 fieldCounter = SwapBytes(*((uint32*)&nuonEnv.systemBusDRAM[VIDEO_FIELD_COUNTER_ADDRESS & SYSTEM_BUS_VALID_MEMORY_MASK]));
                    if (fieldCounter >= nuonEnv.MPE3wait_fieldCounter)
                        nuonEnv.MPE3wait_fieldCounter = 0;
                    last_time2 = new_time;
                }
            }

            // Audio: feed the host audio ring (RetroArch drains it via
            // DrainAudioRing below). Push one Nuon audio period when the game has
            // re-armed its HALF/WRAP interrupt and INT_AUDIO is not already
            // pending. The pacing interval is the period's TRUE playback duration
            // at the Nuon rate (AudioPeriodIntervalUs), not the 60Hz video field:
            // a period is half the game-chosen DMA buffer (1K..64K), so pacing on
            // the field overproduces for >4K buffers (RetroArch audio_sync then
            // throttles the whole frontend -> slowdown) and underruns for <4K.
            const uint64 audioIntervalUs = nuonEnv.AudioPeriodIntervalUs();
            if (audioIntervalUs > 0) {
                if (nuonEnv.pNuonAudioBuffer &&
                    (new_time >= last_time3 + audioIntervalUs) &&
                    ((nuonEnv.nuonAudioChannelMode & (ENABLE_WRAP_INT | ENABLE_HALF_INT)) != (nuonEnv.oldNuonAudioChannelMode & (ENABLE_WRAP_INT | ENABLE_HALF_INT))) &&
                    ((((nuonEnv.mpe[0].intsrc & nuonEnv.mpe[0].inten1) | (nuonEnv.mpe[1].intsrc & nuonEnv.mpe[1].inten1) | (nuonEnv.mpe[2].intsrc & nuonEnv.mpe[2].inten1) | (nuonEnv.mpe[3].intsrc & nuonEnv.mpe[3].inten1)) & INT_AUDIO) == 0)) {
                    if (nuonEnv.TryPushAudioPeriod()) {
                        // Keep to the schedule, not to when this check happened to run:
                        // the check is only made every few thousand emulation steps and
                        // not at all between retro_run calls, so restarting the interval
                        // from "now" made every period a little late and delivered about
                        // 8% less audio than playback consumes - audible as gaps. If the
                        // game has fallen a whole period behind there is nothing to catch
                        // up to, so start again from here.
                        last_time3 += audioIntervalUs;
                        if (new_time - last_time3 >= audioIntervalUs)
                            last_time3 = new_time;
                    }
                }
            } else last_time3 = new_time;
        }

        nuonEnv.TriggerScheduledInterrupts();
    }
}

// --- Emulation thread ---
//
// RetroArch calls retro_run once per display frame and then waits for the
// display. Run from there, the emulation gets the time between two such waits
// and no more; on its own thread it gets all of it. retro_run then only reads
// input, takes the picture and hands over the audio. The picture is taken with
// the thread held at a step boundary (EmuPause), so the video state it reads is
// never half-written.
//
// On for Arm64 JIT builds when the native emitter is in use; NUANCE_EMU_THREAD=0
// or =1 in the environment overrides that either way.

static void EmuThreadMain()
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0); // keep it on a performance core
#endif
    while (!emuThreadStop.load()) {
        const bool paused = (useconds_since_start() - emuLastRunCallUs.load()) > 300000;
        if (emuMainWants.load() || paused || !bRun) {
            emuParked.store(true);
            usleep(paused ? 2000 : 20);
            continue;
        }
        emuParked.store(false);
        if (emuMainWants.load()) // asked for between the check above and here
            continue;
        RunEmulation(true);
        emuParked.store(true);
    }
    emuParked.store(true);
}

static void StartEmuThread()
{
    if (emuThreaded)
        return;
    emuThreadStop.store(false);
    emuMainWants.store(false);
    emuParked.store(true);
    emuLastRunCallUs.store(useconds_since_start());
    emuThreaded = true;
    emuThread = std::thread(EmuThreadMain);
}

static void StopEmuThread()
{
    if (!emuThreaded)
        return;
    emuThreadStop.store(true);
    emuThread.join();
    emuThreaded = false;
}

// A frontend that quits without unloading the game (RetroArch's Quit menu
// item calls exit()) would otherwise reach the destructor of a std::thread
// that is still running, which aborts the process. This object is defined
// after the emulation's own globals, so it is destroyed, and the thread
// stopped, before they are.
static struct EmuThreadAtExit { ~EmuThreadAtExit() { StopEmuThread(); } } emuThreadAtExit;

static bool HoldEmuThread()
{
    if (!emuThreaded)
        return false;
    emuMainWants.store(true);
    while (!emuParked.load())
        sched_yield();
    return true;
}

static void ReleaseEmuThread()
{
    emuMainWants.store(false);
}

// Holds the emulation thread at a step boundary for as long as it lives (or
// until Release). Does nothing when there is no such thread.
struct EmuPause
{
    EmuPause() : bHeld(HoldEmuThread()) {}
    void Release()
    {
        if (bHeld) {
            ReleaseEmuThread();
            bHeld = false;
        }
    }
    ~EmuPause() { Release(); }
    bool bHeld;
};

void retro_run(void)
{
    if (!game_loaded) return;

    {
        bool bOptionsChanged = false;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &bOptionsChanged) && bOptionsChanged)
            ApplyCoreOptions();
        // A changed save reaches the disk within a few seconds, not only when
        // the game is unloaded: an app can be killed without warning.
        static unsigned saveCheck = 0;
        if ((++saveCheck % 300) == 0)
            nuonEnv.flashEEPROM.SaveIfChanged();
    }

    // Input
    input_poll_cb();
    uint16 buttons = GetInputButtons();
    ApplyControllerState(1, buttons);
    ApplyAnalogControls();
    {
        // NUANCE_DUMP_RAM=<path prefix>[,<frame>,<frame>...]: write main and system
        // bus RAM to <prefix>-<frame>.bin at those frames, or every twenty
        // seconds when no frames are given. For finding where a game keeps
        // something.
        static const char* const dumpSpec = getenv("NUANCE_DUMP_RAM");
        static unsigned dumpFrame = 0;
        if (dumpSpec) {
            ++dumpFrame;
            std::string spec = dumpSpec;
            size_t comma = spec.find(',');
            if (comma == std::string::npos) {
                // No frames given: every twenty seconds.
                if ((dumpFrame % 1200) == 0) {
                    spec += "," + std::to_string(dumpFrame);
                    comma = spec.find(',');
                }
            }
            if (comma != std::string::npos) {
                const std::string prefix = spec.substr(0, comma);
                size_t pos = comma;
                while (pos != std::string::npos) {
                    if ((unsigned)atoi(spec.c_str() + pos + 1) == dumpFrame) {
                        const bool bHeld = HoldEmuThread();
                        const std::string name = prefix + "-" + std::to_string(dumpFrame) + ".bin";
                        if (FILE* f = fopen(name.c_str(), "wb")) {
                            fwrite(nuonEnv.mainBusDRAM, 1, MAIN_BUS_SIZE, f);
                            fwrite(nuonEnv.systemBusDRAM, 1, SYSTEM_BUS_SIZE, f);
                            fclose(f);
                        }
                        if (bHeld) ReleaseEmuThread();
                    }
                    pos = spec.find(',', pos + 1);
                }
            }
        }
    }

    // NUANCE_PERF=1 in the environment: every 300 frames, log how many packets
    // each MPE executed per second of wall time. The one number that says how
    // fast the CPU emulation is, whatever the game happens to be drawing.
    static const bool perf_enabled = getenv("NUANCE_PERF") != nullptr;
#ifdef USE_A64_JIT
    a64PerfSampleInterpreted = perf_enabled;
#endif
    static uint64 perf_last_time = useconds_since_start();
    static unsigned perf_frames = 0;
    if (perf_enabled && ++perf_frames == 300) {
        const uint64 now = useconds_since_start();
        const double secs = (double)(now - perf_last_time) / 1e6;
        // VidSync: how often the game asks for the next frame, and how many
        // 60 Hz fields it asks to wait each time. 60 divided by the second is
        // the frame rate the game is aiming for; the first falls short of
        // that when the emulation cannot keep up.
        log_printf("libretro: perf: packets/s MPE0 %.0f MPE1 %.0f MPE2 %.0f MPE3 %.0f total %.0f; game VidSync %.1f/s waiting %.1f fields each, VidChangeBase %.1f/s\n",
            perf_packets[0] / secs, perf_packets[1] / secs, perf_packets[2] / secs, perf_packets[3] / secs,
            (perf_packets[0] + perf_packets[1] + perf_packets[2] + perf_packets[3]) / secs,
            perfVidSyncCalls / secs, perfVidSyncCalls ? (double)perfVidSyncFields / perfVidSyncCalls : 0.0, perfVidChangeBaseCalls / secs);
        perfVidSyncCalls = perfVidChangeBaseCalls = perfVidSyncFields = 0;
        // Audio handed to the frontend against what continuous playback
        // needs. Less than the nominal rate means the game did not refill its
        // buffer in time and there are gaps in the sound.
        {
            log_printf("libretro: perf: audio %.0f frames/s delivered, %.0f needed for continuous sound\n", perfAudioFrames / secs, (double)nuonEnv.nuonAudioPlaybackRate);
            perfAudioFrames = 0;
        }
#ifdef USE_A64_JIT
        if (nuonEnv.compilerOptions.bArm64NativeCode) {
            char stats[1024];
            A64_FormatStats(stats, sizeof(stats));
            log_printf("libretro: perf: Arm64 JIT %s\n", stats);
            {
                A64InterpretedPacket top[12];
                const unsigned n = A64_TopInterpretedPackets(top, 12);
                for (unsigned k = 0; k < n; k++) {
                    char text[1024];
                    nuonEnv.mpe[top[k].mpeIndex].PrintInstructionCachePacket(text, sizeof(text), top[k].address);
                    for (char* c = text; *c; c++) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
                    log_printf("libretro: perf: interpreted %.0f/s MPE%u %08X%s: %s\n", top[k].count * 16 / secs, top[k].mpeIndex, top[k].address,
                        (top[k].packetInfo & PACKETINFO_NEVERCOMPILE) ? " (never compiled)" : "", text);
                }
            }
            for (int i = 0; i < 4; i++) {
                log_printf("libretro: perf: MPE%d per second: %.0f native blocks (%.1f packets each), %.0f packets interpreted, %.0f of them never compilable\n", i,
                    a64PerfNativeBlocks[i] / secs,
                    a64PerfNativeBlocks[i] ? (double)(perf_packets[i] - a64PerfInterpreted[i]) / a64PerfNativeBlocks[i] : 0.0,
                    a64PerfInterpreted[i] / secs, a64PerfNeverCompile[i] / secs);
                log_printf("libretro: perf: MPE%d compilable but interpreted, per second: %.0f in a delay slot, %.0f newly decoded, %.0f compiled for another overlay, %.0f below the threshold\n", i,
                    a64PerfWhy[i][0] / secs, a64PerfWhy[i][1] / secs, a64PerfWhy[i][2] / secs, a64PerfWhy[i][3] / secs);
                {
                    static uint64 lastNative[4], lastInterp[4], lastFind[4];
                    MPE& m = nuonEnv.mpe[i];
                    log_printf("libretro: perf: MPE%d per second: %.0f overlay lookups, %.0f native cache flushes, %.0f interpreter cache flushes; %u overlays in use, %u KB of code\n", i,
                        (m.a64OverlayLookups - lastFind[i]) / secs, (m.numNativeCodeCacheFlushes - lastNative[i]) / secs, (m.numInterpreterCacheFlushes - lastInterp[i]) / secs, m.overlayManager.GetOverlaysInUse(), m.nativeCodeCache.GetUsedCodeBufferSize() / 1024);
                    lastFind[i] = m.a64OverlayLookups; lastNative[i] = m.numNativeCodeCacheFlushes; lastInterp[i] = m.numInterpreterCacheFlushes;
                }
                a64PerfWhy[i][0] = a64PerfWhy[i][1] = a64PerfWhy[i][2] = a64PerfWhy[i][3] = 0;
                a64PerfNativeBlocks[i] = a64PerfInterpreted[i] = a64PerfNeverCompile[i] = 0;
            }
        }
#endif
        perf_packets[0] = perf_packets[1] = perf_packets[2] = perf_packets[3] = 0;
        perf_last_time = now;
        perf_frames = 0;
    }

#ifdef USE_A64_JIT
    // NUANCE_A64_WATCHDOG=1: if retro_run stops being called for five seconds
    // while the native emitter is on, say which block was running and stop.
    // Generated code that never returns leaves no other trace.
    static volatile unsigned watchdog_frames = 0;
    static bool watchdog_started = false;
    if (!watchdog_started && getenv("NUANCE_A64_WATCHDOG") && nuonEnv.compilerOptions.bArm64NativeCode) {
        watchdog_started = true;
        std::thread([]() {
            unsigned last = watchdog_frames, stalled = 0;
            for (;;) {
                usleep(1000000);
                const unsigned now = watchdog_frames;
                stalled = (now == last) ? stalled + 1 : 0;
                last = now;
                if (stalled >= 5) {
                    // Two looks, a moment apart: the same block still running
                    // both times is a block that does not return.
                    for (int look = 0; look < 2; look++) {
                        log_printf("libretro: Arm64 JIT watchdog: no frame for 5 s; %s native block MPE%u at %08X; pcexec %08X %08X %08X %08X\n",
                            a64DebugBlockRunning ? "inside" : "last",
                            (unsigned)a64DebugBlockMpe, (unsigned)a64DebugBlockPc,
                            nuonEnv.mpe[0].pcexec, nuonEnv.mpe[1].pcexec, nuonEnv.mpe[2].pcexec, nuonEnv.mpe[3].pcexec);
                        usleep(200000);
                    }
                    abort();
                }
            }
        }).detach();
    }
    watchdog_frames = watchdog_frames + 1;
#endif

    // Run emulation: here, until the next video field or the time budget is
    // used up - or, with the emulation thread, not here at all.
    emuLastRunCallUs.store(useconds_since_start());
    if (emuThreadWanted && !emuThreaded) {
        StartEmuThread();
        log_printf("libretro: emulation runs on its own thread\n");
    }
    if (!emuThreaded)
        RunEmulation(false);

    // The emulation thread is held at a step boundary while the picture is
    // taken from its memory, and let go before the frame is handed over:
    // presenting waits for the display and is the slow part.
    EmuPause emuPause;

    // Render video
    if (gl_initialized) {
        glBindFramebuffer(GL_FRAMEBUFFER, hw_render.get_current_framebuffer());
        glViewport(0, 0, FB_WIDTH, FB_HEIGHT);
        // Once: what a black screen comes down to is in these.
        static bool reported = false;
        const GLenum fbStatus = reported ? GL_FRAMEBUFFER_COMPLETE : glCheckFramebufferStatus(GL_FRAMEBUFFER);
        RenderVideo(FB_WIDTH, FB_HEIGHT);
        if (!reported) {
            reported = true;
            log_printf("libretro: first frame: GL %s on %s, framebuffer %u (status 0x%04X), GL error 0x%04X\n",
                (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER),
                (unsigned)hw_render.get_current_framebuffer(), (unsigned)fbStatus, (unsigned)glGetError());
        }
        // A few times once the game runs: whether the Nuon has a picture,
        // whether drawing it raised a GL error, and what landed in
        // RetroArch's framebuffer - which of the three a black screen is
        // (#79). At about 5, 20 and 60 seconds.
        {
            static unsigned frames = 0;
            ++frames;
            if (frames == 300 || frames == 1200 || frames == 3600) {
                char state[256];
                VideoDebugState(state, sizeof(state));
                const GLenum err = glGetError();
                unsigned char px[3][4] = {};
                glReadPixels(FB_WIDTH / 2, FB_HEIGHT / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[0]);
                glReadPixels(FB_WIDTH / 4, FB_HEIGHT / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[1]);
                glReadPixels(FB_WIDTH * 3 / 4, FB_HEIGHT * 3 / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[2]);
                log_printf("libretro: frame %u: %s; GL error 0x%04X; framebuffer %u pixels %02X%02X%02X %02X%02X%02X %02X%02X%02X\n",
                    frames, state, (unsigned)err, (unsigned)hw_render.get_current_framebuffer(),
                    px[0][0], px[0][1], px[0][2], px[1][0], px[1][1], px[1][2], px[2][0], px[2][1], px[2][2]);
            }
        }
        emuPause.Release();
        video_cb(RETRO_HW_FRAME_BUFFER_VALID, FB_WIDTH, FB_HEIGHT, 0);
    } else {
        // Software fallback - black frame
        memset(framebuffer, 0, sizeof(framebuffer));
        emuPause.Release();
        video_cb(framebuffer, FB_WIDTH, FB_HEIGHT, FB_WIDTH * 4);
    }

    // Audio: drain the host audio ring. DrainAudioRing emits native-endian s16
    // (the ring holds little-endian samples; swapped only on big-endian hosts),
    // so what audio_batch_cb expects
    uint32 frames = nuonEnv.DrainAudioRing(audio_buffer, AUDIO_BUFFER_SIZE / 2);
    perfAudioFrames += frames;
    if (frames)
        audio_batch_cb(audio_buffer, frames);
}

size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void*, size_t) { return false; }
bool retro_unserialize(const void*, size_t) { return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned, bool, const char*) {}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned) { return nullptr; }
size_t retro_get_memory_size(unsigned) { return 0; }

// Swap buffers stub for video.cpp
void SDL2_SwapWindow() {}
