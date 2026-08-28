cmake_minimum_required(VERSION 3.21)

project(ThreeDCad VERSION 0.1.0 LANGUAGES C)

if(NOT WIN32)
    message(FATAL_ERROR
        "3DCad currently requires Windows because its font, file-dialog, and "
        "UTF-8 file-path implementations use Win32 APIs.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(THREEDCAD_BUILD_TOOLS "Build command-line conversion tools" ON)

find_package(SDL3 3.2 CONFIG REQUIRED COMPONENTS SDL3)
find_package(OpenGL REQUIRED)

function(threedcad_configure_target target)
    target_compile_definitions(${target} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

set(THREEDCAD_CORE_SOURCES
    src/cad_core.c
    src/cad_export_3dg1.c
    src/cad_export_obj.c
    src/cad_file.c
    src/cad_import_3dg1.c
    src/cad_import_obj.c
)

add_library(ThreeDCadCore STATIC ${THREEDCAD_CORE_SOURCES})
target_include_directories(ThreeDCadCore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
threedcad_configure_target(ThreeDCadCore)

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

if(THREEDCAD_BUILD_TOOLS)
    set_source_files_properties(tools/cad23dg1/cad23dg1.c PROPERTIES
        COMPILE_DEFINITIONS _CRT_SECURE_NO_WARNINGS
    )

    add_executable(cad23dg1
        tools/cad23dg1/cad23dg1.c
    )

    target_link_libraries(cad23dg1 PRIVATE ThreeDCadCore)
    threedcad_configure_target(cad23dg1)
endif()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${THREEDCAD_CORE_SOURCES} ${THREEDCAD_GUI_SOURCES}
)
