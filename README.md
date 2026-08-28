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
- Transactional OBJ, 3DG1, and ASM-shape workflows. OBJ supports negative
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

Animation tags 3/4 embedded in native X11 CAD streams are preserved and
round-trip even though the full animation editor is deferred. Coordinate
transforms propagate to corresponding frame points. Topology-changing tools
require creating a confirmed static copy. The separate historical `3DAN` and
`3DGI` `.anm` project formats are not opened by this static-editor release.

See [docs/FILE_FORMATS.md](docs/FILE_FORMATS.md) for the recovered layouts and
validation rules.

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

## Tests and recovered corpus

CTest covers codecs, topology repair, animation round trips, history,
transactional failures, OBJ/3DG1 behavior, view projection, and hit testing.
GitHub Actions runs Windows Debug/Release GUI builds and a portable GUI-off
Linux core/tools build.

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

## Core API

`ThreeDCadCore` is independent of SDL and Win32 GUI code. `CadDocument` owns
model state, paths, dirty tracking, palette data, and the 64-entry undo/redo
history. `CadCodec_Decode`, `CadCodec_Encode`, and `CadCodec_Validate` operate
on caller-provided buffers and return `CadResult` with structured diagnostics.
All editor input paths share the `EditorTool_Begin` / `Update` / `Commit` /
`Cancel` transaction lifecycle, so one gesture produces one validated history
entry or rolls back completely.

## Command-line converter

`cad23dg1` converts a native CAD stream to 3DG1 text:

```powershell
.\build\x64\Release\cad23dg1.exe input.cad output.txt
```

Disable it with `-DTHREEDCAD_BUILD_TOOLS=OFF`.

## Deferred historical systems

Standalone `3DAN`/`3DGI` `.anm` import/export, full 64-frame animation
authoring, deterministic SF2 Transfer/export, floppy mounting, XWD printing,
and NEWS printer-port transport are intentionally not part of this
static-editor release. Dormant Object/Group/Extrude/Spin menus are also not
exposed.
