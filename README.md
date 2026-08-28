# 3DCad

3DCad is a Windows desktop editor for the Iwamoto 3D-CAD file format. The GUI
uses SDL3 for its window and input platform layer and the system OpenGL library
for its existing OpenGL 1.1 renderer.

The root `CMakeLists.txt` is the canonical build and can generate Visual Studio
solutions for either x64 or x86. SDL3 is supplied through the vcpkg manifest;
no third-party binaries are stored in the repository.

## Requirements

- Windows 10 or later
- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.21 or later
- Git
- [vcpkg](https://github.com/microsoft/vcpkg)

SDL3 is declared by `vcpkg.json`; vcpkg installs it automatically during CMake
configuration when the vcpkg toolchain file is supplied. OpenGL and the native
font/dialog libraries come from the Windows SDK.

## Set up vcpkg

If vcpkg is not already installed:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "C:\vcpkg"
```

For a vcpkg installation in another location, set `VCPKG_ROOT` to that absolute
path. Set it again in each new PowerShell session, or make it a persistent user
environment variable.

## Configure and build (x64)

From the repository root:

```powershell
cmake -S . -B build\x64 `
  -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows

cmake --build build\x64 --config Release --target 3DCadGui cad23dg1
```

The GUI build copies `resources` into the executable directory. With the normal
dynamic `x64-windows` triplet, vcpkg's app-local deployment also places the
required SDL3 runtime DLL there.

## Run

The application resolves assets relative to its executable, so it can be
launched directly from any working directory:

```powershell
Push-Location build\x64\Release
.\3DCadGui.exe
Pop-Location
```

For a Debug build, replace `Release` with `Debug` in both the build and run
commands.

## Command-line converter

`cad23dg1` converts an Iwamoto `.cad` file to Fundoshi-Kun 3DG1 text format:

```powershell
.\build\x64\Release\cad23dg1.exe input.cad output.txt
```

The output path is optional; without it, the tool replaces the input extension
with `.txt`. Disable command-line tools at configuration time with
`-DTHREEDCAD_BUILD_TOOLS=OFF`.

## Architecture notes

- **x64 is recommended** and uses `-A x64` with the `x64-windows` vcpkg triplet.
- For a 32-bit build, use a separate directory, `-A Win32`, and
  `-DVCPKG_TARGET_TRIPLET=x86-windows`.
- Never reuse one CMake build directory for different architectures or vcpkg
  triplets; delete it or choose a new directory before switching.
- ARM64 and non-Windows builds are not currently validated. The source still
  uses Win32 APIs for bitmap fonts, native dialogs, and UTF-8 file paths.
- SDL3 owns the application lifecycle, window, high-DPI coordinate conversion,
  and input events. Rendering intentionally remains on the existing OpenGL 1.1
  compatibility path.
- The default dynamic vcpkg triplets are the supported configuration. Static
  triplets may require additional runtime and linkage validation.

## Build outputs

For the Visual Studio generator, executables are written under the selected
configuration directory, for example:

```text
build/x64/Release/3DCadGui.exe
build/x64/Release/resources/
build/x64/Release/cad23dg1.exe
```
