# infinite_undiscovery - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(infinite_undiscovery LANGUAGES CXX)

if(WIN32)
    enable_language(RC)
endif()

# PlayStation 5 homebrew (the PS5 payload SDK's toolchain sets PS5): Vulkan
# through the SDK's Xenos GPU, linked and packaged as a title. See
# cmake/IuPs5.cmake and docs/PS5.md.
if(PS5)
    include(cmake/IuPs5.cmake)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
if(PS5)
    # The console has no setup wizard, Win32 dialogs or D3D12 diagnostics: the
    # title folder holds the installed assets (docs/PS5.md), and
    # src/ps5/iu_ps5_app.h is the app. Title setup before main, and libc names
    # the console does not export, come from src/platform/.
    set(INFINITEUNDISCOVERY_SOURCES
        src/main.cpp
        src/platform/ps5_title.cpp
        src/platform/ps5_radv_wait.cpp
        src/platform/ps5_libc.cpp
        src/platform/ps5_debug_heap.cpp)
else()
    set(INFINITEUNDISCOVERY_SOURCES
        src/main.cpp
        src/asset_setup.cpp
        src/asset_dlc.cpp
        src/portable_setup.cpp
    )
endif()
# PS5: the checking allocator (src/platform/ps5_debug_heap.cpp), for chasing
# heap damage. Only that file's flags change, so switching it rebuilds one file.
option(IU_PS5_DEBUG_HEAP "PS5: guard every heap block and report the first damaged one" OFF)
if(PS5 AND IU_PS5_DEBUG_HEAP)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/platform/ps5_debug_heap.cpp"
        PROPERTIES COMPILE_DEFINITIONS IU_PS5_DEBUG_HEAP=1 SKIP_PRECOMPILE_HEADERS ON)
endif()

if(WIN32)
    list(APPEND INFINITEUNDISCOVERY_SOURCES src/infinite_undiscovery.rc)
endif()

option(IU_BUILD_SETUP_TESTS "Build local asset installer validation executable" OFF)
if(IU_BUILD_SETUP_TESTS)
    add_executable(iu_asset_setup_tests tests/asset_setup_tests.cpp src/asset_setup.cpp src/asset_dlc.cpp src/portable_setup.cpp)
    target_link_libraries(iu_asset_setup_tests PRIVATE rex::runtime bcrypt comctl32 ole32 shell32 uuid gdi32)
    target_include_directories(iu_asset_setup_tests PRIVATE src)
    rexglue_apply_target_settings(iu_asset_setup_tests)
endif()

if(WIN32)
    add_executable(infinite_undiscovery WIN32 ${INFINITEUNDISCOVERY_SOURCES})
else()
    add_executable(infinite_undiscovery ${INFINITEUNDISCOVERY_SOURCES})
endif()

# GPU_PLUGINS xenos makes the SDK stage rexgpu-xenos.dll next to the host. The
# GPU plugin is runtime-loaded (never linked), so TARGET_RUNTIME_DLLS misses it
# and the release build output would otherwise only contain rexruntime.dll.
rexglue_setup_target(infinite_undiscovery GPU_PLUGINS xenos)
# Preserve the historical PoC executable; publish this stage under its new name.
set_target_properties(infinite_undiscovery PROPERTIES OUTPUT_NAME "InfiniteUndiscoveryRecomp")

if(WIN32)
    target_link_libraries(infinite_undiscovery PRIVATE bcrypt comctl32 ole32 shell32 uuid gdi32)

    # Release hardening: refuse to produce a build whose staged ReXGlue DLLs are
    # the old precompiled-SDK copies. Aborts the build with a clear message.
    add_custom_command(TARGET infinite_undiscovery POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DVERIFY_DIR=$<TARGET_FILE_DIR:infinite_undiscovery>"
            "-DREQUIRE_ALL=ON"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_rexglue_dlls.cmake"
        COMMENT "Verifying staged ReXGlue DLLs (auditable v0.10.0 hashes)"
        VERBATIM)
elseif(NOT PS5)
    message(FATAL_ERROR "Asset Setup currently requires Windows; do not imply cross-platform support.")
endif()
