# 3DCad

3DCad is an SDL3 reimplementation and reimagining of the Iwamoto/3Ddraw editor
used in the Star Fox 2-era workflow. It keeps the program's distinctive four
coordinated views, ordered point-chain geometry, indexed colors, paired faces,
and compact tool palette while adding modern document safety, undo/redo,
high-DPI input, and maintainable format handling.

Historical source and screenshots are behavioral references rather than a
pixel-perfect UI specification. The current SDL3 interface remains the design
baseline.

## Current editor

- Top, Front, Right, and perspective 3D views with adaptive grids, panning,
  zooming, camera rotation/roll, wireframe or indexed-color shading, responsive
  cleanup, and recoverable window visibility.
- The recovered 24-tool static modeling vocabulary: point/face selection,
  guided two-view point creation, ordered face creation, insert, move, rotate,
  scale, copy, mirror, flip, reverse, side pairing, cut, color, delete, and
  undo. The historically inactive Primitive and deferred Transfer controls are
  visibly disabled.
- A modal STATE/TenKey panel applies numeric XYZ translation, rotation center
  and angles, and scale center and factors as one undoable operation.
- Rectangle selection, coplanarity checks, F.Support, change-first-point, face
  information, grid/point/polygon merge, polygon sorting, and clipboard tools.
- Transactional New/Open/Import/Replace/Quit flows, Save/Discard/Cancel prompts,
  atomic native saves, UTF-8 paths, dirty document titles, and 64-state
  undo/redo with an entire drag stored as one operation.
- Transactional OBJ, 3DG1, ANM, and ASM-shape workflows. OBJ supports negative
  indices and exports two-point faces as lines; 3DG1 preserves 2–16 point faces
  without repeated round-trip growth. Recovered `CLIP_PLANE` helpers preview as
  colored two-point normal guides with an explicit runtime-semantics warning.
- A palette editor can create, open, inspect, edit, and save both recovered
  color resources. `.COL` stores 256 little-endian SNES BGR555 colors; `.PAL`
  is the separate 0x8200-byte material map with 256 descriptors and 128 `.COL`
  indices per material. The editor exposes the complete 0–255 color/material
  range and previews a model through the active `.PAL` sample and `.COL` table.

## Native compatibility

New documents use the recovered later X11 record stream. The codec reads and
writes explicit big-endian fields for objects, polygons, points, animation
indices, and animation points; it never writes the host C structure layout.

The earlier packed CAD stream is also detected and imported. Because it cannot
represent animation or paired-side metadata, it is opened as an unnamed dirty
conversion and Save As writes the later compatible stream. The original source
is never silently overwritten.

Animation tags 3/4 embedded in native X11 CAD streams are preserved and edited
in place. An unchanged native document is saved from its retained, validated
source bytes so record ordering and uninterpreted recovered bytes remain exact;
the fixed-offset encoder takes over after a model edit. The compact timeline
supports 1–64 fixed-topology morph frames,
creation for all or selected faces, frame insertion/duplication/deletion,
whole-pose and selected-point copies, current/all-frame transforms, scrubbing,
12 FPS playback, looping, and display-only interpolation. An exact displayed
pose—including an interpolated pose—can be baked into an unnamed static copy.

Standalone `3DAN` and `3DGI` `.anm` files import transactionally as unnamed
native documents and export deterministically with the recovered rounding and
DOS-EOF behavior. ANM input paths are never reused as native save paths.
Topology-changing tools remain disabled while animation is attached, with the
reason exposed by the shared editor controller.

See [docs/FILE_FORMATS.md](docs/FILE_FORMATS.md) for recovered layouts,
[docs/ANIMATION.md](docs/ANIMATION.md) for the authoring workflow, and
[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) for the compatibility matrix.

## Requirements

- Meson 1.6.0 or later and Ninja
- A C11 compiler: MSVC, MinGW-w64 GCC, GCC, or Clang
- SDL3 3.2.12 or later and desktop OpenGL when building the GUI
- Python 3 to run Meson

The core library, converter, and tests have no SDL or OpenGL dependency. The
GUI is supported on Windows 10 or later and Linux. SDL3 supplies the portable
window, input, dialog, and platform services; the editor uses an embedded
bitmap font and therefore does not require GDI, SDL_ttf, or a system font.

## Configure and build

With SDL3 and OpenGL installed in the compiler's normal search path, a complete
build uses the same commands on every platform:

```sh
meson setup build --buildtype=release \
  -Dgui=enabled -Dtools=true -Dtests=true
meson compile -C build
meson test -C build --print-errorlogs
```

If SDL3 is not available through the compiler's normal dependency search,
Meson can build the pinned SDL3 wrap as a static fallback. Pass
`--wrap-mode=nofallback` during setup when an installed SDL3 is required.

The executable, converter, and copied `resources` directory are written below
the selected build directory. Keep a separate build directory for each
compiler, target architecture, and configuration.

### Visual Studio / MSVC

Run these commands from an x64 or x86 Native Tools command prompt. Download an
official SDL3 VC development archive, extract it, and point `sdl3_root` at the
directory containing its `include` and `lib` directories:

```powershell
py -m pip install meson ninja
meson setup build\msvc --backend=ninja --buildtype=release `
  -Dgui=enabled -Dtools=true -Dtests=true `
  -Dsdl3_root="C:\Libraries\SDL3"
meson compile -C build\msvc
meson test -C build\msvc --print-errorlogs
```

Meson copies the matching SDL3 runtime beside the build-tree executables. The
CI workflow downloads the latest official VC archive and verifies it against
the SHA-256 digest published by GitHub before using it.

### MinGW-w64 / MSYS2

The UCRT64 environment provides SDL3 directly through its package manager:

```sh
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-meson \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-sdl3

meson setup build/mingw --buildtype=release \
  -Dgui=enabled -Dtools=true -Dtests=true
meson compile -C build/mingw
meson test -C build/mingw --print-errorlogs
```

### Linux

Install Meson, Ninja, pkg-config, SDL3 development files, and OpenGL development
files using the distribution package manager. For example, Ubuntu 26.04 uses
`meson`, `ninja-build`, `pkg-config`, `libsdl3-dev`, and `libgl-dev`. Then run
the common build commands above.

To prove that the core, tools, and tests do not acquire GUI dependencies, use a
separate GUI-disabled build:

```sh
meson setup build/core --buildtype=debugoptimized \
  -Dgui=disabled -Dtools=true -Dtests=true
meson compile -C build/core
meson test -C build/core --print-errorlogs
```

## Controls

- `Ctrl+N`, `Ctrl+O`, `Ctrl+S`, `Ctrl+Shift+S`: document operations
- `Ctrl+Z`, `Ctrl+Shift+Z`/`Ctrl+Y`: undo and redo
- `Ctrl+A`: select all points or faces in the active selection mode
- `Ctrl+Shift+A` or idle `Escape`: deselect everything
- `Ctrl+C`, `Ctrl+V`: copy and paste the selection
- `Delete`: delete the active point/face selection
- `Escape`: cancel the current placement, selection rectangle, or drag
- 3D left-drag: rotate X/Y; Shift+left-drag: roll Z
- 3D middle-drag or wheel: zoom; right-drag: pan
- Point tool: click one orthographic view, then a compatible second view to
  supply the hidden coordinate
- Face tool: click vertices in order; Backspace removes the last, Enter closes,
  and Escape cancels
- Color tool: left-click increases a face or line's palette index; right-click
  decreases it. Indices wrap between 0 and 255; right-drag still pans the view
- Palette Editor: create or open `.COL` and `.PAL` resources, select entries in
  the 16x16 grids, edit BGR555 channels or material descriptors/sample indices,
  and apply the selected index to selected faces or two-point lines
- `F`: frame the active selection; Home frames the complete document
- Timeline: scrub the zero-based strip, use Play/Pause/Stop and frame controls,
  and toggle interpolation, looping, or All Frames

## Tests and recovered corpus

`meson test` covers codecs, topology repair, animation lifecycle and playback,
named/composite history, transactional failures, ANM/OBJ/3DG1 behavior,
platform filesystem failures, view projection, and hit testing. GitHub Actions
builds and tests the GUI with both MSVC and MinGW-w64, tests a GUI-disabled
Linux Clang build under AddressSanitizer and UndefinedBehaviorSanitizer, and
starts the Linux GUI under Xvfb with software OpenGL for a one-frame smoke test.

Recovered assets are not copied into this repository. To enable the optional
external corpus test, configure with the directory containing the recovered
`.cad` files:

```powershell
meson setup build\corpus `
  -Dgui=disabled -Dtools=true -Dtests=true `
  -Drecovery_corpus="D:\recovered\watanabe" `
  -Drecovery_expected_total=500 `
  -Drecovery_expected_x11=475 `
  -Drecovery_expected_legacy=25
meson compile -C build\corpus
meson test -C build\corpus --print-errorlogs
```

The current recovery corpus contains 500 files: 475 later X11 streams and 25
legacy packed streams. All are expected to decode and validate.

The optional standalone animation corpus is configured separately:

```powershell
meson setup build\anm-corpus `
  -Dgui=disabled -Dtools=true -Dtests=true `
  -Danm_corpus="D:\recovered\animations" `
  -Danm_expected_total=379 `
  -Danm_expected_3dan=358 `
  -Danm_expected_3dgi=21
meson compile -C build\anm-corpus
meson test -C build\anm-corpus --print-errorlogs
```

The recovered ANM corpus contains 379 files: 358 `3DAN` and 21 `3DGI`. The
harness checks native validation, semantic round trips, and deterministic
second encodings without committing recovered assets.

`ThreeDCadAsmImportTests --corpus <shape files...> --constants <include
files...>` audits recovered ASM catalogs. Set `THREEDCAD_EXPECT_ASM_TOTAL`,
`THREEDCAD_EXPECT_ASM_DECODED`, and `THREEDCAD_EXPECT_ASM_UNSUPPORTED` to make
the census exact. The current SF2 catalog resolves and decodes all 445 entries.
The two recovered clipping-plane helpers are represented as static colored
normal guides rather than emulating the game runtime's plane-slot behavior.

## Core API

`ThreeDCadCore` is independent of SDL and native GUI code. `CadDocument` owns
model state, paths, dirty tracking, palette resources, and the 64-entry named
undo/redo history. Native CAD, `.COL`, and `.PAL` resources have independent
source/save paths, revisions, and dirty state; saving one never marks either of
the others clean. `CadCodec_*`, `CadAnmCodec_*`, and `CadPalette_*` operate on
caller-provided buffers and return `CadResult` with structured diagnostics;
bounded UTF-8 file access and atomic replacement live in platform services.
`EditorController` provides one capability/disabled-reason source and the
shared begin/update/commit/cancel mutation boundary. Invalid geometry rolls
back without changing history, revision, or dirty state.

`CadAnimation_*`, `CadPose`, `CadScene`, and `CadAnimationSession` provide the
fixed-topology authoring and allocation-free playback path. Stable static point
IDs map to frame points by face-chain ordinal, and one immutable posed scene is
shared by rendering, picking, and coordinate display each GUI iteration.

## Command-line converter

`cad23dg1` converts a native CAD stream to 3DG1 text:

```powershell
.\build\cad23dg1.exe input.cad output.txt
```

Disable it with `-Dtools=false`.

## Deferred historical systems

Advanced onion skinning, range editing, ping-pong playback, per-frame timing,
bones/curves, and animated topology/colors remain out of scope. Deterministic
SF2 Transfer/export, floppy mounting, XWD printing, NEWS printer-port transport,
and dormant Object/Group/Extrude/Spin authoring are also deferred.
