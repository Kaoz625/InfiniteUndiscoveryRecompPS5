# cmake/IuPs5.cmake
# PlayStation 5 homebrew title build, with the PS5 payload SDK's toolchain
# (toolchain/prospero.cmake) and Mihawk's PS5_Vulkan (RADV linked into the
# title). Modelled on Fable 2 Recomp PS5's cmake/Fable2Ps5.cmake. See docs/PS5.md.
#
#   IU_PS5_VULKAN_DIR   PS5_Vulkan checkout with its RADV release archive
#   IU_PS5_TITLE_ID     PPSAxxxxx title id written to sce_sys/param.json
#   REXSDK_DIR          the ReXGlue SDK tree with its PS5 changes
#                       (thirdparty/rexglue-sdk, branch fable2-ps5:
#                       the PS5 port with the Xenos GPU on Vulkan)
#   REXGLUE_HOST_TOOL   host rexglue from that tree (runs codegen)
#
# Like Fable 2, Infinite Undiscovery renders through the SDK's Xenos GPU (Vulkan), which
# translates the game's shaders at run time: there is no host shader step.

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# The console's CPU is an 8-core Zen 2.
add_compile_options(-march=znver2)

get_filename_component(_iu_ps5_parent "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
set(IU_PS5_VULKAN_DIR "${_iu_ps5_parent}/PS5_Vulkan" CACHE PATH "PS5_Vulkan checkout (RADV for the console)")
set(IU_PS5_TITLE_ID "PPSA99215" CACHE STRING "Title id of the PS5 homebrew title")
set(IU_PS5_TITLE_NAME "Infinite Undiscovery" CACHE STRING "Home screen name of the PS5 homebrew title")
set(IU_PS5_DIST_DIR "${CMAKE_BINARY_DIR}/dist" CACHE PATH "Where the title folder <TITLE_ID>/ is written")

if(NOT EXISTS "${IU_PS5_VULKAN_DIR}/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a")
    message(WARNING "No RADV release archive in ${IU_PS5_VULKAN_DIR}; the title link will fail. "
        "Build it with PS5_Vulkan's tools/build-radv.sh release (docs/PS5.md).")
endif()

# Every executable is linked as a PS5 title by ps5/link-title.sh, which gets
# CMake's objects and fully resolved static libraries and runs PS5_Vulkan's
# link recipe (RADV, the SDK's platform layer, the CRT, the fake SELF).
set(_iu_ps5_link_config "${CMAKE_BINARY_DIR}/ps5-link.conf")
file(WRITE "${_iu_ps5_link_config}"
    "vulkan='${IU_PS5_VULKAN_DIR}'\n"
    "sdk='${PS5_PAYLOAD_SDK}'\n"
    "title_id='${IU_PS5_TITLE_ID}'\n"
    "title_name='${IU_PS5_TITLE_NAME}'\n"
    "dist='${IU_PS5_DIST_DIR}'\n"
    "param_template='${CMAKE_SOURCE_DIR}/ps5/sce_sys/param.json'\n"
    "icon='${CMAKE_SOURCE_DIR}/ps5/sce_sys/icon0.png'\n"
    "env_example='${CMAKE_SOURCE_DIR}/ps5/ps5-env.example.txt'\n"
    "work='${CMAKE_BINARY_DIR}/ps5-link'\n")
set(CMAKE_CXX_LINK_EXECUTABLE
    "bash \"${CMAKE_SOURCE_DIR}/ps5/link-title.sh\" \"${_iu_ps5_link_config}\" <TARGET> <OBJECTS> <LINK_LIBRARIES>")
set(CMAKE_C_LINK_EXECUTABLE "${CMAKE_CXX_LINK_EXECUTABLE}")
