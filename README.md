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
  without repeated round-trip growth.
- Historical `.COL` and `.PAL` loading, full 0–255 polygon color indices, and a
  BGR555 palette preview. `.COL` consumes the historical first 0x200 bytes and
  tolerates trailing data; `.PAL` uses the exact recovered 0x8200-byte layout.

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

- Windows 10 or later for the SDL3 GUI
- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.21 or later
- [vcpkg](https://github.com/microsoft/vcpkg)

SDL3 is declared by `vcpkg.json`. OpenGL and the native font/dialog libraries
come from the Windows SDK.

## Configure and build

```powershell
cmake -S . -B build\x64 `
  -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DBUILD_TESTING=ON

cmake --build build\x64 --config Release --parallel
ctest --test-dir build\x64 -C Release --output-on-failure
```

The executable and copied resources are written to `build/x64/Release`. Build
x86 in a separate directory with `-A Win32` and `x86-windows`; never reuse a
configured directory across architectures.

The core, tools, and tests do not require the Win32 GUI layer:

```sh
cmake -S . -B build/core -DTHREEDCAD_BUILD_GUI=OFF -DBUILD_TESTING=ON
cmake --build build/core
ctest --test-dir build/core --output-on-failure
```

## Controls

- `Ctrl+N`, `Ctrl+O`, `Ctrl+S`, `Ctrl+Shift+S`: document operations
- `Ctrl+Z`, `Ctrl+Shift+Z`/`Ctrl+Y`: undo and redo
- `Ctrl+A`, `Ctrl+C`, `Ctrl+V`: selection and clipboard
- `Delete`: delete the active point/face selection
- `Escape`: cancel the current placement, selection rectangle, or drag
- 3D left-drag: rotate X/Y; Shift+left-drag: roll Z
- 3D middle-drag or wheel: zoom; right-drag: pan
- Point tool: click one orthographic view, then a compatible second view to
  supply the hidden coordinate
- Face tool: click vertices in order; Backspace removes the last, Enter closes,
  and Escape cancels
- `F`: frame the active selection; Home frames the complete document
- Timeline: scrub the zero-based strip, use Play/Pause/Stop and frame controls,
  and toggle interpolation, looping, or All Frames

## Tests and recovered corpus

CTest covers codecs, topology repair, animation lifecycle and playback,
named/composite history, transactional failures, ANM/OBJ/3DG1 behavior,
platform filesystem failures, view projection, and hit testing. GitHub Actions
runs Windows Debug/Release GUI builds and a portable GUI-off Linux Clang build
under AddressSanitizer and UndefinedBehaviorSanitizer.

Recovered assets are not copied into this repository. To enable the optional
external corpus test, configure with the directory containing the recovered
`.cad` files:

```powershell
cmake -S . -B build\x64 `
  -DTHREEDCAD_RECOVERY_CORPUS="D:\recovered\watanabe" `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
```

The current recovery corpus contains 500 files: 475 later X11 streams and 25
legacy packed streams. All are expected to decode and validate.

The optional standalone animation corpus is configured separately:

```powershell
cmake -S . -B build\x64 `
  -DTHREEDCAD_ANM_CORPUS="D:\recovered\animations" `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
```

The recovered ANM corpus contains 379 files: 358 `3DAN` and 21 `3DGI`. The
harness checks native validation, semantic round trips, and deterministic
second encodings without committing recovered assets.

`ThreeDCadAsmImportTests --corpus <shape files...> --constants <include
files...>` audits recovered ASM catalogs. Set `THREEDCAD_EXPECT_ASM_TOTAL`,
`THREEDCAD_EXPECT_ASM_DECODED`, and `THREEDCAD_EXPECT_ASM_UNSUPPORTED` to make
the census exact. The current SF2 catalog resolves 445 entries: 442 decode and
the three known unsupported entries remain classified explicitly.

## Core API

`ThreeDCadCore` is independent of SDL and Win32 GUI code. `CadDocument` owns
model state, paths, dirty tracking, palette data, and the 64-entry named
undo/redo history. `CadCodec_*` and `CadAnmCodec_*` operate on caller-provided
buffers and return `CadResult` with structured diagnostics; bounded UTF-8 file
access and atomic replacement live in platform services. `EditorController`
provides one capability/disabled-reason source and the shared begin/update/
commit/cancel mutation boundary. Invalid geometry rolls back without changing
history, revision, or dirty state.

`CadAnimation_*`, `CadPose`, `CadScene`, and `CadAnimationSession` provide the
fixed-topology authoring and allocation-free playback path. Stable static point
IDs map to frame points by face-chain ordinal, and one immutable posed scene is
shared by rendering, picking, and coordinate display each GUI iteration.

## Command-line converter

`cad23dg1` converts a native CAD stream to 3DG1 text:

```powershell
.\build\x64\Release\cad23dg1.exe input.cad output.txt
```

Disable it with `-DTHREEDCAD_BUILD_TOOLS=OFF`.

## Deferred historical systems

Advanced onion skinning, range editing, ping-pong playback, per-frame timing,
bones/curves, and animated topology/colors remain out of scope. Deterministic
SF2 Transfer/export, floppy mounting, XWD printing, NEWS printer-port transport,
and dormant Object/Group/Extrude/Spin authoring are also deferred.
