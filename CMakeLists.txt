cmake_minimum_required(VERSION 3.21)

project(ThreeDCad VERSION 0.3.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(THREEDCAD_BUILD_TOOLS "Build command-line conversion tools" ON)
option(THREEDCAD_BUILD_GUI "Build the SDL3 desktop editor" ON)
option(THREEDCAD_ENABLE_SANITIZERS
    "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
set(THREEDCAD_RECOVERY_CORPUS "" CACHE PATH
    "Optional recovered-data root used by the external corpus validation test")
set(THREEDCAD_ANM_CORPUS "" CACHE PATH
    "Optional recovered-data root used by the external ANM corpus test")
set(THREEDCAD_RECOVERY_EXPECTED_TOTAL 500 CACHE STRING
    "Expected CAD files in the canonical external recovery corpus")
set(THREEDCAD_RECOVERY_EXPECTED_X11 475 CACHE STRING
    "Expected later-X11 files in the canonical external recovery corpus")
set(THREEDCAD_RECOVERY_EXPECTED_LEGACY 25 CACHE STRING
    "Expected legacy files in the canonical external recovery corpus")
set(THREEDCAD_ANM_EXPECTED_TOTAL 379 CACHE STRING
    "Expected files in the canonical external ANM corpus")
set(THREEDCAD_ANM_EXPECTED_3DAN 358 CACHE STRING
    "Expected 3DAN files in the canonical external ANM corpus")
set(THREEDCAD_ANM_EXPECTED_3DGI 21 CACHE STRING
    "Expected 3DGI files in the canonical external ANM corpus")

include(CTest)

if(THREEDCAD_ENABLE_SANITIZERS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    else()
        message(WARNING "THREEDCAD_ENABLE_SANITIZERS requires Clang or GCC")
    endif()
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/cad_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/cad_version.h"
    @ONLY
)

function(threedcad_configure_target target)
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
        )
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

set(THREEDCAD_CORE_SOURCES
    src/animation_panel.c
    src/cad_animation.c
    src/cad_anm_codec.c
    src/cad_codec.c
    src/cad_core.c
    src/cad_document.c
    src/cad_export_3dg1.c
    src/cad_export_obj.c
    src/cad_file.c
    src/cad_geometry.c
    src/cad_import_3dg1.c
    src/cad_import_asm.c
    src/cad_import_obj.c
    src/cad_palette.c
    src/desktop_layout.c
    src/editor_controller.c
    src/editor_tool.c
    src/platform_fs.c
    src/palette_panel.c
)

add_library(ThreeDCadCore STATIC ${THREEDCAD_CORE_SOURCES})
target_include_directories(ThreeDCadCore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
threedcad_configure_target(ThreeDCadCore)

if(NOT WIN32)
    target_link_libraries(ThreeDCadCore PUBLIC m)
endif()

set(THREEDCAD_GUI_SOURCES)
if(THREEDCAD_BUILD_GUI)
    if(NOT WIN32)
        message(FATAL_ERROR
            "THREEDCAD_BUILD_GUI currently requires Windows. The core library, "
            "tools, and tests can be built portably with -DTHREEDCAD_BUILD_GUI=OFF.")
    endif()

    find_package(SDL3 3.2 CONFIG REQUIRED COMPONENTS SDL3)
    find_package(OpenGL REQUIRED)

    set(THREEDCAD_GUI_SOURCES
        src/cad_view.c
        src/file_dialog.c
        src/font_win32.c
        src/gui.c
        src/main.c
        src/render_gl.c
    )

    add_executable(3DCadGui ${THREEDCAD_GUI_SOURCES})
    target_include_directories(3DCadGui PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(3DCadGui PRIVATE
        ThreeDCadCore
        SDL3::SDL3
        OpenGL::GL
        comdlg32
        gdi32
        ole32
        shell32
        uuid
    )
    threedcad_configure_target(3DCadGui)

    set(THREEDCAD_RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")
    add_custom_command(TARGET 3DCadGui POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${THREEDCAD_RESOURCE_DIR}"
            "$<TARGET_FILE_DIR:3DCadGui>/resources"
        COMMENT "Copying 3DCad resources beside the executable"
        VERBATIM
    )

    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT 3DCadGui)
    set_target_properties(3DCadGui PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:3DCadGui>"
    )
endif()

if(THREEDCAD_BUILD_TOOLS)
    set_source_files_properties(tools/cad23dg1/cad23dg1.c PROPERTIES
        COMPILE_DEFINITIONS _CRT_SECURE_NO_WARNINGS
    )

    add_executable(cad23dg1
        tools/cad23dg1/cad23dg1.c
    )

    target_link_libraries(cad23dg1 PRIVATE ThreeDCadCore)
    if(WIN32)
        target_link_libraries(cad23dg1 PRIVATE shell32)
    endif()
    threedcad_configure_target(cad23dg1)
endif()

if(BUILD_TESTING)
    add_executable(ThreeDCadImportExportTests
        tests/test_import_export.c
    )
    target_link_libraries(ThreeDCadImportExportTests PRIVATE ThreeDCadCore)
    threedcad_configure_target(ThreeDCadImportExportTests)
    add_test(NAME import_export COMMAND ThreeDCadImportExportTests)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_core.c")
        add_executable(ThreeDCadCoreTests tests/test_core.c)
        target_link_libraries(ThreeDCadCoreTests PRIVATE ThreeDCadCore)
        threedcad_configure_target(ThreeDCadCoreTests)
        add_test(NAME core_document_codec COMMAND ThreeDCadCoreTests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cad_geometry.c")
        add_executable(ThreeDCadGeometryTests tests/test_cad_geometry.c)
        target_link_libraries(ThreeDCadGeometryTests PRIVATE ThreeDCadCore)
        threedcad_configure_target(ThreeDCadGeometryTests)
        add_test(NAME cad_geometry COMMAND ThreeDCadGeometryTests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_animation.c")
        add_executable(ThreeDCadAnimationTests tests/test_animation.c)
        target_link_libraries(ThreeDCadAnimationTests PRIVATE ThreeDCadCore)
        threedcad_configure_target(ThreeDCadAnimationTests)
        add_test(NAME animation_authoring COMMAND ThreeDCadAnimationTests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_animation_panel.c")
        add_executable(ThreeDCadAnimationPanelTests
            tests/test_animation_panel.c)
        target_link_libraries(ThreeDCadAnimationPanelTests PRIVATE ThreeDCadCore)
        threedcad_configure_target(ThreeDCadAnimationPanelTests)
        add_test(NAME animation_panel COMMAND ThreeDCadAnimationPanelTests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_palette.c")
        add_executable(ThreeDCadPaletteTests tests/test_palette.c)
        target_link_libraries(ThreeDCadPaletteTests PRIVATE ThreeDCadCore)
        threedcad_configure_target(ThreeDCadPaletteTests)
        add_test(NAME palette_codec_document COMMAND ThreeDCadPaletteTests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_palette_panel.c")
        add_executable(ThreeDCadPalettePanelTests tests/test_palette_panel.c)
        target_link_libraries(ThreeDCadPalettePanelTests PRIVATE ThreeDCadCore)
        threedcad_configure_target(ThreeDCadPalettePanelTests)
        add_test(NAME palette_panel COMMAND ThreeDCadPalettePanelTests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_animation_document.c")
        add_executable(ThreeDCadAnimationDocumentTests
            tests/test_animation_document.c)
        target_link_libraries(ThreeDCadAnimationDocumentTests PRIVATE ThreeDCadCore)
        threedcad_configure_target(ThreeDCadAnimationDocumentTests)
        add_test(NAME animation_document_history
            COMMAND ThreeDCadAnimationDocumentTests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_anm_codec.c")
        add_executable(ThreeDCadAnmCodecTests tests/test_anm_codec.c)
        target_link_libraries(ThreeDCadAnmCodecTests PRIVATE ThreeDCadCore)
        threedcad_configure_target(ThreeDCadAnmCodecTests)
        add_test(NAME anm_codec COMMAND ThreeDCadAnmCodecTests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_document_anm.c")
        add_executable(ThreeDCadAnmDocumentTests tests/test_document_anm.c)
        target_link_libraries(ThreeDCadAnmDocumentTests PRIVATE ThreeDCadCore)
        threedcad_configure_target(ThreeDCadAnmDocumentTests)
        add_test(NAME anm_document_workflow COMMAND ThreeDCadAnmDocumentTests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_editor_controller.c")
        add_executable(ThreeDCadEditorControllerTests
            tests/test_editor_controller.c)
        target_link_libraries(ThreeDCadEditorControllerTests PRIVATE ThreeDCadCore)
        threedcad_configure_target(ThreeDCadEditorControllerTests)
        add_test(NAME editor_controller COMMAND ThreeDCadEditorControllerTests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_platform_fs.c")
        add_executable(ThreeDCadPlatformFsTests tests/test_platform_fs.c)
        target_link_libraries(ThreeDCadPlatformFsTests PRIVATE ThreeDCadCore)
        threedcad_configure_target(ThreeDCadPlatformFsTests)
        add_test(NAME platform_filesystem COMMAND ThreeDCadPlatformFsTests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_import_asm.c")
        add_executable(ThreeDCadAsmImportTests tests/test_import_asm.c)
        target_link_libraries(ThreeDCadAsmImportTests PRIVATE ThreeDCadCore)
        threedcad_configure_target(ThreeDCadAsmImportTests)
        add_test(NAME asm_importer COMMAND ThreeDCadAsmImportTests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_desktop_layout.c")
        add_executable(ThreeDCadDesktopLayoutTests
            tests/test_desktop_layout.c)
        target_link_libraries(ThreeDCadDesktopLayoutTests PRIVATE ThreeDCadCore)
        threedcad_configure_target(ThreeDCadDesktopLayoutTests)
        add_test(NAME desktop_layout COMMAND ThreeDCadDesktopLayoutTests)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_recovered_corpus.c")
        add_executable(ThreeDCadCorpusTests tests/test_recovered_corpus.c)
        target_link_libraries(ThreeDCadCorpusTests PRIVATE ThreeDCadCore)
        threedcad_configure_target(ThreeDCadCorpusTests)
        if(THREEDCAD_RECOVERY_CORPUS)
            add_test(NAME recovered_corpus
                COMMAND ThreeDCadCorpusTests "${THREEDCAD_RECOVERY_CORPUS}")
            set_tests_properties(recovered_corpus PROPERTIES ENVIRONMENT
                "THREEDCAD_EXPECT_CAD_TOTAL=${THREEDCAD_RECOVERY_EXPECTED_TOTAL};THREEDCAD_EXPECT_CAD_X11=${THREEDCAD_RECOVERY_EXPECTED_X11};THREEDCAD_EXPECT_CAD_LEGACY=${THREEDCAD_RECOVERY_EXPECTED_LEGACY}")
        endif()
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_anm_corpus.c")
        add_executable(ThreeDCadAnmCorpusTests tests/test_anm_corpus.c)
        target_link_libraries(ThreeDCadAnmCorpusTests PRIVATE ThreeDCadCore)
        threedcad_configure_target(ThreeDCadAnmCorpusTests)
        if(THREEDCAD_ANM_CORPUS)
            add_test(NAME recovered_anm_corpus
                COMMAND ThreeDCadAnmCorpusTests "${THREEDCAD_ANM_CORPUS}")
            set_tests_properties(recovered_anm_corpus PROPERTIES ENVIRONMENT
                "THREEDCAD_EXPECT_ANM_TOTAL=${THREEDCAD_ANM_EXPECTED_TOTAL};THREEDCAD_EXPECT_ANM_3DAN=${THREEDCAD_ANM_EXPECTED_3DAN};THREEDCAD_EXPECT_ANM_3DGI=${THREEDCAD_ANM_EXPECTED_3DGI}")
        endif()
    endif()

    if(THREEDCAD_BUILD_GUI AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cad_view.c")
        add_executable(ThreeDCadViewTests
            tests/test_cad_view.c
            src/cad_view.c
            src/render_gl.c
        )
        target_include_directories(ThreeDCadViewTests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(ThreeDCadViewTests PRIVATE
            ThreeDCadCore SDL3::SDL3 OpenGL::GL
        )
        threedcad_configure_target(ThreeDCadViewTests)
        add_test(NAME view_math_and_hit_testing COMMAND ThreeDCadViewTests)
    endif()
endif()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${THREEDCAD_CORE_SOURCES} ${THREEDCAD_GUI_SOURCES}
)
