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
