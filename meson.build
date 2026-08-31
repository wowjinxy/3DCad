project(
    'ThreeDCad',
    'c',
    version: '0.3.0',
    meson_version: '>=1.6.0',
    default_options: ['c_std=c11', 'warning_level=3'],
)

cc = meson.get_compiler('c')
fs = import('fs')
host_system = host_machine.system()

if cc.get_argument_syntax() == 'msvc'
    add_project_arguments('/utf-8', language: 'c')
endif

if host_system == 'windows'
    add_project_arguments('-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', language: 'c')
endif

version_parts = meson.project_version().split('.')
version_configuration = configuration_data()
version_configuration.set('PROJECT_VERSION_MAJOR', version_parts[0])
version_configuration.set('PROJECT_VERSION_MINOR', version_parts[1])
version_configuration.set('PROJECT_VERSION_PATCH', version_parts[2])
version_configuration.set('PROJECT_VERSION', meson.project_version())

configure_file(
    input: 'include/cad_version.h.in',
    output: 'cad_version.h',
    configuration: version_configuration,
)

project_includes = include_directories('include', '.')

core_sources = files(
    'src/animation_panel.c',
    'src/cad_animation.c',
    'src/cad_anm_codec.c',
    'src/cad_codec.c',
    'src/cad_core.c',
    'src/cad_document.c',
    'src/cad_export_3dg1.c',
    'src/cad_export_obj.c',
    'src/cad_file.c',
    'src/cad_geometry.c',
    'src/cad_import_3dg1.c',
    'src/cad_import_asm.c',
    'src/cad_import_obj.c',
    'src/cad_palette.c',
    'src/desktop_layout.c',
    'src/editor_controller.c',
    'src/editor_tool.c',
    'src/palette_panel.c',
    'src/platform_fs.c',
)

core_link_dependencies = []
if host_system != 'windows'
    core_link_dependencies += cc.find_library('m', required: true)
endif

threedcad_core = static_library(
    'ThreeDCadCore',
    core_sources,
    include_directories: project_includes,
    dependencies: core_link_dependencies,
)

threedcad_core_dep = declare_dependency(
    link_with: threedcad_core,
    include_directories: project_includes,
    dependencies: core_link_dependencies,
)

if get_option('tools')
    tool_dependencies = [threedcad_core_dep]
    tool_c_args = []

    if host_system == 'windows'
        tool_dependencies += cc.find_library('shell32', required: true)
        tool_c_args += '-D_CRT_SECURE_NO_WARNINGS'
    endif

    executable(
        'cad23dg1',
        'tools/cad23dg1/cad23dg1.c',
        c_args: tool_c_args,
        dependencies: tool_dependencies,
    )
endif

gui_option = get_option('gui')
gui_built = false
sdl3_dep = disabler()
opengl_dep = disabler()

if not gui_option.disabled()
    sdl3_root = get_option('sdl3_root')

    if sdl3_root != ''
        if host_system != 'windows'
            error('-Dsdl3_root is only valid for Windows builds')
        endif
        if cc.get_argument_syntax() != 'msvc'
            error(
                '-Dsdl3_root expects the official Visual C++ SDL3 SDK and an MSVC-compatible compiler',
            )
        endif
        if not fs.is_absolute(sdl3_root)
            sdl3_root = meson.project_source_root() / sdl3_root
        endif

        sdl3_arch = {'x86': 'x86', 'x86_64': 'x64', 'aarch64': 'arm64'}.get(
            host_machine.cpu_family(),
            '',
        )
        if sdl3_arch == ''
            error(
                'The official SDL3 Visual C++ SDK has no configured library mapping for ' + host_machine.cpu_family(),
            )
        endif

        sdl3_include_dir = sdl3_root / 'include'
        sdl3_library_dir = sdl3_root / 'lib' / sdl3_arch
        sdl3_import_library = sdl3_library_dir / 'SDL3.lib'
        sdl3_runtime_library = sdl3_library_dir / 'SDL3.dll'
        sdl3_version_header = sdl3_include_dir / 'SDL3' / 'SDL_version.h'

        foreach required_path : [
            sdl3_version_header,
            sdl3_import_library,
            sdl3_runtime_library,
        ]
            if not fs.is_file(required_path)
                error('The SDL3 Visual C++ SDK is missing ' + required_path)
            endif
        endforeach

        sdl3_sdk_library = cc.find_library(
            'SDL3',
            dirs: sdl3_library_dir,
            required: true,
        )
        sdl3_dep = declare_dependency(
            compile_args: ['/I' + sdl3_include_dir],
            dependencies: sdl3_sdk_library,
        )

        if not cc.compiles(
            '''
      #include <SDL3/SDL_version.h>
      #if !SDL_VERSION_ATLEAST(3, 2, 12)
      #error SDL 3.2.12 or newer is required
      #endif
      int main(void) { return 0; }
      ''',
            dependencies: sdl3_dep,
            name: 'SDL3 Visual C++ SDK is version 3.2.12 or newer',
        )
            error('-Dsdl3_root must refer to SDL 3.2.12 or newer')
        endif

        configure_file(
            input: sdl3_runtime_library,
            output: 'SDL3.dll',
            copy: true,
        )
    else
        sdl3_dep = dependency(
            'sdl3',
            version: '>=3.2.12',
            required: gui_option,
            fallback: ['sdl3', 'sdl3_dep'],
            default_options: [
                'default_library=static',
                'warning_level=0',
                'werror=false',
            ],
        )
    endif

    if host_system == 'darwin'
        opengl_dep = dependency(
            'appleframeworks',
            modules: 'OpenGL',
            required: gui_option,
        )
    else
        opengl_dep = dependency('gl', required: gui_option)
    endif

    if sdl3_dep.found() and opengl_dep.found()
        gui_built = true
        subdir('resources')

        gui_sources = files(
            'src/cad_view.c',
            'src/file_dialog.c',
            'src/font.c',
            'src/gui.c',
            'src/main.c',
            'src/render_gl.c',
        )

        executable(
            '3DCadGui',
            gui_sources,
            include_directories: project_includes,
            dependencies: [threedcad_core_dep, sdl3_dep, opengl_dep],
            win_subsystem: 'console',
        )
    endif
endif

if get_option('tests')
    test_definitions = [
        [
            'ThreeDCadImportExportTests',
            'import_export',
            'tests/test_import_export.c',
        ],
        ['ThreeDCadCoreTests', 'core_document_codec', 'tests/test_core.c'],
        ['ThreeDCadGeometryTests', 'cad_geometry', 'tests/test_cad_geometry.c'],
        [
            'ThreeDCadAnimationTests',
            'animation_authoring',
            'tests/test_animation.c',
        ],
        [
            'ThreeDCadAnimationPanelTests',
            'animation_panel',
            'tests/test_animation_panel.c',
        ],
        [
            'ThreeDCadPaletteTests',
            'palette_codec_document',
            'tests/test_palette.c',
        ],
        [
            'ThreeDCadPalettePanelTests',
            'palette_panel',
            'tests/test_palette_panel.c',
        ],
        [
            'ThreeDCadAnimationDocumentTests',
            'animation_document_history',
            'tests/test_animation_document.c',
        ],
        ['ThreeDCadAnmCodecTests', 'anm_codec', 'tests/test_anm_codec.c'],
        [
            'ThreeDCadAnmDocumentTests',
            'anm_document_workflow',
            'tests/test_document_anm.c',
        ],
        [
            'ThreeDCadEditorControllerTests',
            'editor_controller',
            'tests/test_editor_controller.c',
        ],
        [
            'ThreeDCadPlatformFsTests',
            'platform_filesystem',
            'tests/test_platform_fs.c',
        ],
        ['ThreeDCadAsmImportTests', 'asm_importer', 'tests/test_import_asm.c'],
        [
            'ThreeDCadDesktopLayoutTests',
            'desktop_layout',
            'tests/test_desktop_layout.c',
        ],
    ]

    foreach test_definition : test_definitions
        test_executable = executable(
            test_definition[0],
            test_definition[2],
            dependencies: threedcad_core_dep,
        )
        test(test_definition[1], test_executable)
    endforeach

    recovery_corpus_executable = executable(
        'ThreeDCadCorpusTests',
        'tests/test_recovered_corpus.c',
        dependencies: threedcad_core_dep,
    )
    recovery_corpus = get_option('recovery_corpus')
    if recovery_corpus != ''
        recovery_environment = environment()
        recovery_environment.set(
            'THREEDCAD_EXPECT_CAD_TOTAL',
            get_option('recovery_expected_total').to_string(),
        )
        recovery_environment.set(
            'THREEDCAD_EXPECT_CAD_X11',
            get_option('recovery_expected_x11').to_string(),
        )
        recovery_environment.set(
            'THREEDCAD_EXPECT_CAD_LEGACY',
            get_option('recovery_expected_legacy').to_string(),
        )
        test(
            'recovered_corpus',
            recovery_corpus_executable,
            args: [recovery_corpus],
            env: recovery_environment,
        )
    endif

    anm_corpus_executable = executable(
        'ThreeDCadAnmCorpusTests',
        'tests/test_anm_corpus.c',
        dependencies: threedcad_core_dep,
    )
    anm_corpus = get_option('anm_corpus')
    if anm_corpus != ''
        anm_environment = environment()
        anm_environment.set(
            'THREEDCAD_EXPECT_ANM_TOTAL',
            get_option('anm_expected_total').to_string(),
        )
        anm_environment.set(
            'THREEDCAD_EXPECT_ANM_3DAN',
            get_option('anm_expected_3dan').to_string(),
        )
        anm_environment.set(
            'THREEDCAD_EXPECT_ANM_3DGI',
            get_option('anm_expected_3dgi').to_string(),
        )
        test(
            'recovered_anm_corpus',
            anm_corpus_executable,
            args: [anm_corpus],
            env: anm_environment,
        )
    endif

    if gui_built
        view_test_executable = executable(
            'ThreeDCadViewTests',
            files('src/cad_view.c', 'src/render_gl.c', 'tests/test_cad_view.c'),
            include_directories: project_includes,
            dependencies: [threedcad_core_dep, sdl3_dep, opengl_dep],
        )
        test('view_math_and_hit_testing', view_test_executable)
    endif
endif

summary(
    {
        'Desktop editor': gui_built,
        'Command-line tools': get_option('tools'),
        'Tests': get_option('tests'),
        'Recovered CAD corpus': get_option('recovery_corpus') != '',
        'Recovered ANM corpus': get_option('anm_corpus') != '',
    },
    section: '3DCad build',
    bool_yn: true,
)
