# Standalone apps: plan

Status: proposed, 2026-11-15; iOS dropped for now (see Decisions). Nothing
here is started. It follows the Arm64 JIT
work in [arm64-jit-plan.md](arm64-jit-plan.md).

## Goal

Native apps that play NUON games without RetroArch:

1. A Mac app and an iOS app.
2. Then an Android app with the same features.

Each has:

- a game library built from a folder the user points it at;
- button remapping;
- a scanline on/off switch, using the CRT shader the core already has (the one
  RetroArch shows today);
- controller support, and on phones and tablets an on-screen controller.

## The main constraint: iOS cannot JIT

Apple does not let App Store or TestFlight apps generate and run code. An iOS
app is therefore interpreter-only, and on the interpreter Tempest 3000 ran at
about a third of the speed it reaches with the JIT on an M4 Pro, which is
itself short of full speed. iPhones are slower than that Mac.

So the plan treats the platforms differently:

| Platform | JIT | Expectation |
|---|---|---|
| macOS | Yes (needs the `allow-jit` entitlement, which is allowed) | Same speed as the RetroArch core today |
| Android | Yes (the Arm64 backend should carry over) | Depends on the device; untested |
| iOS / iPadOS | No | Light games may be playable; Tempest 3000 almost certainly not |

A build run from Xcode with the debugger attached can JIT, which is useful for
your own devices but is not something that can be shipped.

Because of this, **step 0 is a measurement, not app work**: build the core for
iOS, run two or three games on your iPhone with the perf log, and see what the
interpreter actually delivers. That decides whether the iOS app is worth
building now, and how much of the effort should go into making the interpreter
faster instead.

## Approach

Build each app as a thin native frontend over the existing libretro core.

- The core already builds for macOS, iOS, tvOS and Android, with an OpenGL ES 3
  renderer for the mobile targets. Its libretro interface (video, audio, input,
  load game) is small, exercised daily by RetroArch, and is what the
  differential test host already drives.
- The apps supply what RetroArch supplies today: a window with a GL context,
  audio output, controller input, and the user interface.
- The Linux standalone frontend (X11, GLX, Dear ImGui) is not reused. Porting
  it would mean replacing all of it anyway.

Three layers:

1. **Core** (this repository): unchanged apart from the small additions below.
2. **Host layer** (new, C++ with a C interface): owns the libretro callbacks,
   the audio ring, input state and mapping, and settings. Shared by all three
   apps so behaviour is the same everywhere.
3. **App** (new, per platform): SwiftUI on Mac and iOS from one Xcode project
   with shared code; Kotlin with Jetpack Compose on Android, calling the host
   layer through JNI.

### Additions the core needs

- **Scanlines as a runtime switch.** `UseCRTshader` is read from `nuance.cfg`
  when the shaders are first installed. It becomes a libretro core option that
  reinstalls the fragment shader when changed. The CRT shader already has an
  OpenGL ES path.
- **Saves.** Game saves live in the emulated flash EEPROM, which the core
  writes next to itself. It needs to go to a per-game file in a directory the
  frontend chooses.
- **Analog input.** The libretro core only reports digital buttons. If any
  game needs the analog stick, that has to be added. To be checked per game.
- **No config file dependency.** The apps set options through the interface
  instead of shipping and editing `nuance.cfg`.

Status of these, 2026-11-15 (fork commit after `90dcce4`):

- **Scanlines:** done. Core option `nuance_scanlines` (`config file`,
  `enabled`, `disabled`), applied to the running game by reinstalling the
  fragment shader. Checked from a start-up value; the live change has not
  been exercised from a frontend yet.
- **Frame cap:** done. Core option `nuance_max_fps` (`config file`,
  `unlimited`, `30`, `20`, `15`).
- **Saves:** done for the emulated flash memory. One file per game,
  `<game>.nuonsave`, in the frontend's save directory, written within five
  seconds of a change and on unload. **Open question:** Tempest 3000 wrote
  nothing to flash in any session so far, including scripted games played
  to game over, so where it keeps high scores, and whether the emulator
  supports that at all, is not known yet.
- **No config file:** done. The core runs from a system directory holding
  only the three BIOS files; the Arm64 compiler is on by default on Macs.
- **Analog input:** done, pending Jake's feel test. Core option
  `nuance_controller`: `d-pad only`, `analog stick`, or `NUON analog stick
  (not for Tempest 3000)`. "Analog stick" drives a flywheel from the left
  stick (or a mouse) and feeds it to the game as a quadrature spinner, which
  Tempest 3000 reads; the d-pad keeps working beside it.
  `nuance_spinner_sensitivity` (10%-400%), `nuance_spinner_inertia`
  (0%-100%) and `nuance_spinner_invert` tune it; the apps should show the
  first two as sliders.

  How the game treats the spinner was measured, not guessed, with three
  test tools now in the core: `NUANCE_INPUT_SCRIPT` (buttons and stick by
  game frame), `NUANCE_SHOT` (picture and optionally memory at a game
  frame) and `NUANCE_T3K_TRACE` (the claw's position each game frame; it
  is a 16.16 lane number at `0x800DC100`, found by comparing memory between
  runs that differed only in how long the d-pad was held). Results: one
  count is about 0.14 of a lane; the game moves at most 15/16 of a lane per
  frame, reached at about 13 counts; it acts three frames after the input
  and eases to rest by itself; the d-pad accelerates by 0.23 of a lane per
  frame held and stops when released; both turn the same way.

  The first version fed 40 counts per 60 Hz frame at full stick, which is
  why Jake found it janky: any push was full speed, and when the frame rate
  dropped the 8-bit difference wrapped and the claw ran backwards. Now full
  stick is 14 counts per game frame, handed over once per game frame.
  Traced at quarter, half and full stick and through a reversal: about 0.7,
  5 and 28 lanes a second, no backward steps.

  With a spinner connected, Tempest 3000 ignores the d-pad on the
  level-select and name-entry screens (seen on level select: the same
  d-pad presses change the level in d-pad-only mode and do nothing with a
  spinner present). Jake could not enter his initials. The spinner is now
  presented only while it is turning (stick deflected, flywheel coasting or
  mouse moving, plus a quarter of a second), by rewriting the controller's
  property word without the "changed" bit, which the game accepts without
  its controller-changed screen. Pressing the d-pad stops the flywheel and
  removes the spinner at once. Checked: d-pad changes the level again in
  stick mode; stick and d-pad both move the claw in play; a d-pad press
  during full stick takes over. Not solved: on the name-entry screen the
  stick still acts as a spinner, where one count is a letter, so the d-pad
  is the way to enter initials. Telling that screen from play would need a
  state variable; a search (`NUANCE_T3K_STATE`, `NUANCE_T3K_DUMP_ON_SCORE`,
  which capture memory when a score goes on the table) found none that is
  clearly right.

  The game's own analog-stick mode (`CTRLR_ANALOG1`) is not usable for
  Tempest 3000: with it the buttons did not respond at the title screen.
- **Controller message:** done. Tempest 3000 opens with "your controller
  configuration has changed, please press start" every time, because the
  emulated controller is announced at start-up as newly plugged in. Core
  option `nuance_skip_controller_message` presents it as already connected
  and the game goes straight to its title. Off by default in the core; the
  apps turn it on. (The steady 18.75 frames a second measured earlier on
  "the title screen" with no input was this message screen.)
- **High scores for Tempest 3000:** done. The game keeps its table only in
  memory (on the real machine it was lost at power-off). It is in two
  places in system-bus RAM: the text of the high-score screen at
  `0x8010C3A0` (338 bytes; holds the names), and eight 32-bit scores at
  `0x80113420`. Found by comparing memory snapshots (`NUANCE_DUMP_RAM`)
  from a game Jake played before and after a new score went on. The core
  copies both to `<game>.hiscores` in the save directory when they change
  and puts them back once the untouched text is seen at that address, which
  is also how it knows the game is Tempest 3000. Core option
  `nuance_t3k_high_scores`: `keep` (default), `do not keep`, `erase`.
  Restore, save and erase each checked with a scripted run; not yet
  confirmed by entering a new score in play and restarting.

Found on the way: the core extracts `nuon.run` to a new directory under
`/tmp` on every load and only removes it in `retro_deinit`, so a frontend
that is killed leaves 5 MB behind each time (153 directories had built up
here). The host layer should unload the core properly, and the extraction
should be looked at.

Phase 1's host layer exists as of 2026-11-15 in nagori3k (`host/`,
`mac/nagori_cli.mm`, `build.sh`). A scripted run through it reached level
one of Tempest 3000 at 30 game frames a second with the Arm64 compiler on,
audio arriving at the right rate and the core's picture correct in the
host's frame buffer. Not yet checked by a person: the picture as drawn in
the window, the sound by ear, and a real controller.

Phase 2, the Mac app, has a first version as of 2026-11-15
(`nagori3k/mac/App`, built by `build.sh` into `build/nagori3k.app`):
library from a folder with cover art (a picture beside the game, else
Wikipedia's cover for an exactly matching article title), play in the
window, settings (stick on/off, sensitivity, weight, direction, scanlines,
stretch to fill, 30 frames-a-second limit, skip the controller message,
keep or delete high scores), button remapping for controller and
keyboard, and full screen with no toolbar, Command-F, Escape to leave and
the pointer hidden. Minimum macOS 14, Arm64 only, ad-hoc signed.

Checked by test runs: it builds, the library and button screens render,
a game runs with sound produced, stopping and starting a game in one
process works, and in full screen the toolbar is gone. Confirmed by Jake:
the picture and the controller in the bare player. Still for Jake to try:
the Game settings tab, remapping, switching games, the full-screen keys.

Not done: an Xcode project, Developer ID signing with the allow-jit
entitlement, notarisation, an app icon. Only `.iso` and `.chd` load;
Jake's other titles are `.mdf` or `.zip`, which the core does not read.
Tempest 3000 has no widescreen mode (it never calls `VidQueryConfig`),
hence the stretch option.

### Features

- **Library.** The user picks a folder; the app keeps access to it across
  launches (a security-scoped bookmark on Apple platforms, a document-tree
  permission on Android) and lists the ISO, CHD, ZIP and COFF files in it.
  Titles come from file names at first. Box art is a later nicety. Your games
  are on a network share, so the folder has to work when it is a mounted
  share, and the 700 MB disc images are read in place, not copied.
- **Remapping.** A per-controller table from physical buttons to the 14 NUON
  buttons the core knows, edited in a settings screen, stored by the host
  layer, with a reset to defaults. Keyboard mapping on Mac.
- **Scanlines.** One toggle in settings, applied live.
- **On-screen controller** (iOS, Android): D-pad, A, B, the four C buttons, L,
  R, Start and NUON. Hidden when a physical controller is connected.

### Rendering

OpenGL on macOS and OpenGL ES on iOS are deprecated but still work, and the
core's renderer is written for them. The apps use them as they are. A Metal
renderer is a separate project and is not in this plan.

## Phases

| # | Deliverable | Done when | Rough effort |
|---|---|---|---|
| 0 | iOS speed measurement | Perf numbers for two or three games on your iPhone, and a decision on iOS scope | 1-2 days |
| 1 | Core additions and the host layer | A command-line host plays a game with sound and a controller on the Mac, with scanlines switchable and saves in a chosen folder | 1 week |
| 2 | Mac app | Library from a folder, play, remap, scanline toggle, JIT on; signed and notarised build | 1.5-2 weeks |
| 3 | iOS app | Same features plus the on-screen controller, on your iPhone through TestFlight | 1.5-2 weeks, less if phase 0 narrows it |
| 4 | Android app | Same features on the Pad, JIT on, release-signed APK | 2-3 weeks |
| 5 | Polish | Box art, per-game settings, save management, whatever testing turns up | Open-ended |

Phases 2 and 3 share most of their code. Phase 4 reuses the host layer and
repeats the interface work in Kotlin.

Before phase 1, the JIT work should reach a stopping point: gameplay tested in
more than one game, the dispatcher speed work done or deliberately deferred,
and the emitter on by default for macOS. The apps inherit whatever state the
core is in.

## Decisions

Made on 2026-11-15:

1. **Where the apps live:** a separate repository, with this one as a
   submodule.
2. **Name:** **nagori3k** (decided later the same day, replacing the working
   name "Nu-one"). *Nagori* (名残) is the trace that lingers after something
   has passed. Its stated focus is bringing Tempest 3000 back for fans of
   Jeff Minter. The repository is `jake74/nagori3k` (private for now), with
   this fork as the submodule `core/`.
3. **iOS scope:** dropped for now (decided later the same day). An App Store
   or TestFlight build cannot use the dynamic compiler, and interpreted,
   Tempest 3000 ran at about 5 frames a second on an M4 Pro. The apps are
   Mac first, then Android. Phases 0 and 3 below are on hold, and the parts
   of this plan that mention iOS describe what would be needed if that
   changes.
4. **Minimum OS versions:** macOS 14, iOS 17, Android 10 (API 29), Arm64 only.

Still open:

- **Distribution:** personal builds only, TestFlight, or the stores. The
  original Nuance licence says the emulator "may not be redistributed for
  profit by itself or as part of any other package", so anything distributed
  has to be free, with no ads or purchases.

## Risks

- **iOS speed**, as above. The largest risk in the plan, which is why it is
  measured first.
- **Interpreter improvements are not free.** Pre-decoded block execution
  exists in the code but breaks Tempest 3000 on Arm64 (see the JIT plan), and
  running the four processors on separate threads changes timing between
  them. Either would be its own project.
- **Android JIT.** The backend has only run on macOS. Android differs in how
  executable memory is obtained and in device variety; it needs the
  differential test run on the device before anything else.
- **Deprecated OpenGL.** It works today on every target. If Apple removes it,
  the renderer has to be rewritten for Metal.
- **Game compatibility.** Only Tempest 3000 and the test disc have been run on
  this port. Other games may need fixes in the core that take time away from
  the apps.
- **BIOS files.** The three `.cof` files the core needs are the project's own
  clean-room stubs and can be bundled. Games are the user's own disc images
  and are never bundled.
