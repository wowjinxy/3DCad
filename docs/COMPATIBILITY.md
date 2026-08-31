# Compatibility matrix

## Platforms and toolchains

| Platform/toolchain | Core and tools | SDL3/OpenGL GUI | Continuous verification |
| --- | :---: | :---: | --- |
| Windows 10+ / MSVC x64 | Yes | Yes | Debug and release builds plus the complete automated test suite |
| Windows 10+ / MinGW-w64 UCRT64 | Yes | Yes | Debug-optimized build plus the complete automated test suite |
| Linux / Clang | Yes | Optional | GUI-disabled ASan/UBSan build and complete core test suite |
| Linux / GCC | Yes | Yes | GUI build and complete test suite; one hidden frame under Xvfb and software OpenGL |

Meson is the sole project build system. The `gui` feature may be `enabled`,
`disabled`, or left at `auto`; disabling it proves that the core, converter,
and their tests do not acquire SDL3 or OpenGL dependencies. The `tools` and
`tests` options independently control the converter and test executables.

Windows CI obtains SDL3 either from the official verified VC development
archive or the MSYS2 UCRT64 package. Linux GUI builds use the distribution's
SDL3 and OpenGL development packages. Native file dialogs, confirmation
prompts, directory enumeration, and bitmap-font rendering use portable
backends rather than Win32-only GUI APIs.

## File formats and workflows

| Format/workflow | Read | Write | Notes |
| --- | :---: | :---: | --- |
| Later X11 CAD records | Yes | Yes | Native fixed-offset big-endian stream, including tags 3/4; unchanged sources save byte-exactly |
| Legacy packed CAD | Yes | Conversion only | Unnamed import; Save As writes later X11 CAD |
| `3DAN` animation text | Yes | Yes | Unnamed import; deterministic CRLF plus DOS EOF export |
| `3DGI` animation text | Yes | Yes | Same recovered grammar with alternate header |
| 3DG1 | Yes | Yes | Preserves 2–16-point faces |
| Wavefront OBJ | Yes | Yes | Negative indices and two-point line export supported |
| ASM shape libraries | Yes | No | Search/preview followed by explicit Replace; recovered `CLIP_PLANE` directives become static colored normal guides |
| `.COL` color table | Yes | Yes | Create/view/edit/save; 256 little-endian SNES BGR555 words, including lossless bit-15 preservation |
| `.PAL` material map | Yes | Yes | Create/view/edit/save; 256 descriptors plus 128 raw `.COL` indices per material |
| SF2 Transfer | No | No | Deferred until deterministic game export is recovered |

All imports parse into temporary data and replace the live document only after
complete validation and unsaved-change confirmation. Native saves and ANM
exports encode to memory and use bounded UTF-8 platform services for flushed,
atomic replacement.

Native CAD, `.COL`, and `.PAL` are associated resources rather than one
combined file. Each keeps its own source/save path and dirty state. Palette
opens are transactional, palette saves use atomic replacement, and saving the
model never clears unsaved palette edits (or vice versa). A polygon's 0–255
color value addresses a `.PAL` material when a material map is present; the
chosen preview sample then supplies the `.COL` index used for display.

The optional corpus harnesses validate 500 recovered CAD files (475 X11 and 25
legacy), 379 recovered ANM files (358 `3DAN` and 21 `3DGI`), and an ASM catalog
of 445 decoded entries. The two clipping-plane helpers preserve their endpoints
and plane-slot colors as static guides, not executable runtime clipping state.
Recovered assets remain external to the repository.

`ThreeDCadPaletteTests` also accepts external `.COL`/`.PAL` paths after its
synthetic checks and verifies decode, semantic preservation, and deterministic
canonical re-encoding without adding those files to the repository.
