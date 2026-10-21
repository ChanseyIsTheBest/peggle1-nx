# peggle2-nx (2026-10-19): Renut's Direct3D-level renderer (reNut by masterspike52, github.com/masterspike52/reNut;
# its reuse in the user's ports permitted by its author) as banjo-nx carries it (src/renut_engine/gpu, with banjo-nx's
# Switch, deko3d and tuning work), chosen at boot with peggle1_renderer = "renut" (src/renut_engine/gpu/
# banjo_renut_select.h). No PM4 ring and no EDRAM model: the game's Direct3D calls run natively through plume (Vulkan
# on the PC; deko3d or Vulkan on the Switch). From banjo-nx's app/cmake/banjo_renut_gpu.cmake; docs/peggle1-notes.md,
# "Renut's renderer (deko3d)".
#
# An OBJECT library, not a static one: its hooks replace the generated code's weak sub_X definitions, and a static
# archive's members are only pulled in for undefined symbols, so the hooks would silently not be linked.

# The shader containers its shader cache is built from (tools/renut_shader_dump.py: the image's containers and the
# ones a run dumped with peggle1_dump_shaders; Renut's own capture of the shaders its cache lacked is added by the
# build). Game data: out/, never committed. A normal variable, so it shadows gpu.cmake's cache entry.
set(RENUT_SHADER_DUMP_DIR "${CMAKE_SOURCE_DIR}/../out/renut_shader_dump")
set(RENUT_ASSETS_DIR "${CMAKE_SOURCE_DIR}/../assets/game_root" CACHE PATH "Game assets (default.xex)")
if(NOT EXISTS "${RENUT_SHADER_DUMP_DIR}")
    message(FATAL_ERROR "No ${RENUT_SHADER_DUMP_DIR}: run tools/renut_shader_dump.py first (Renut's renderer "
                        "builds its shader cache from it)")
endif()

# Renut's XenosRecomp is built as an ExternalProject (gpu.cmake): it gets this build's whole host toolchain.
set(BANJO_RENUT_XENOS_RECOMP_ARGS "")
foreach(_peggle1_var CMAKE_C_COMPILER_TARGET CMAKE_CXX_COMPILER_TARGET CMAKE_LINKER_TYPE CMAKE_C_FLAGS CMAKE_CXX_FLAGS
                     CMAKE_AR CMAKE_RC_COMPILER CMAKE_MT)
    if(DEFINED ${_peggle1_var} AND NOT "${${_peggle1_var}}" STREQUAL "")
        list(APPEND BANJO_RENUT_XENOS_RECOMP_ARGS "-D${_peggle1_var}=${${_peggle1_var}}")
    endif()
endforeach()

# Renut's SDK exports o1heap as rex::o1heap.
if(TARGET o1heap AND NOT TARGET rex::o1heap)
    add_library(rex::o1heap ALIAS o1heap)
endif()

# The Switch build is a cross build on a Windows host: Renut's XenosRecomp (a host tool) cannot be built with the
# console's compiler. Its shader cache comes from a PC build of the same tree (tools/renut_switch_shader_cache.py).
if(REXGLUE_PLATFORM_SWITCH)
    set(PLUME_PLATFORM_SWITCH ON CACHE BOOL "" FORCE)
    set(PLUME_SWITCH_DEVKITPRO "${DEVKITPRO}" CACHE PATH "" FORCE)
    set(PLUME_SWITCH_NVK_ROOT "${REXGLUE_SWITCH_NVK_SDK}" CACHE PATH "" FORCE)
    set(PLUME_SWITCH_NVK_LIBRARY "${REXGLUE_SWITCH_NVK_SDK}/lib/libvulkan.a" CACHE FILEPATH "" FORCE)
    # peggle2-nx (2026-10-20): plume links the driver only when the title does (app/CMakeLists.txt).
    set(PLUME_SWITCH_LINK_NVK ${REXGLUE_SWITCH_LINK_NVK} CACHE BOOL "" FORCE)
    # plume's deko3d back end (renut_gpu_backend = "deko3d") and the uam it compiles shader variants with on the
    # console (banjo-nx's docs/deko3d-renderer.md).
    set(PLUME_SWITCH_UAM_DIR "${CMAKE_SOURCE_DIR}/thirdparty/uam" CACHE PATH "" FORCE)
    set(PLUME_DEKO3D ON CACHE BOOL "deko3d back end (Switch)" FORCE)
    set(BANJO_DEKO3D_HOST_SHADERS ON)
    set(BANJO_DEKO3D_SPIRV_CROSS "${CMAKE_SOURCE_DIR}/../out/deko3d_tools/spirv-cross-build/spirv-cross.exe" CACHE FILEPATH
        "SPIRV-Cross for the deko3d host shaders (tools/deko3d_build_tools.sh)")
    set(BANJO_DEKO3D_UAM "${CMAKE_SOURCE_DIR}/../out/deko3d_tools/uam-build/uam.exe" CACHE FILEPATH
        "uam (thirdparty/uam built for the PC) for the deko3d host shaders (tools/deko3d_build_tools.sh)")
    foreach(_peggle1_tool BANJO_DEKO3D_SPIRV_CROSS BANJO_DEKO3D_UAM)
        if(NOT EXISTS "${${_peggle1_tool}}")
            message(FATAL_ERROR "No ${${_peggle1_tool}}: run tools/deko3d_build_tools.sh first")
        endif()
    endforeach()
    # peggle2-nx (2026-10-20): ON = the files of tools/renut_release_shaders.py (out/renut_switch_release): only the
    # port's own shaders built in, the shader table without SPIR-V; the shaders stored in default.xex come from
    # deko3d_glsl.bin beside the NRO (the web installer's). OFF = every translated shader built in (development).
    option(PEGGLE1_RENUT_RELEASE_SHADERS "Switch: no translated game shader in the NRO (deko3d_glsl.bin beside it)" OFF)
    if(PEGGLE1_RENUT_RELEASE_SHADERS)
        set(_peggle1_renut_shaders "${CMAKE_SOURCE_DIR}/../out/renut_switch_release")
    else()
        set(_peggle1_renut_shaders "${CMAKE_SOURCE_DIR}/../out/renut_switch")
    endif()
    set(BANJO_DEKO3D_GLSL_CPP "${_peggle1_renut_shaders}/deko3d_glsl_switch.cpp" CACHE FILEPATH
        "The deko3d GLSL package's source (tools/deko3d_shader_package.py)" FORCE)
    set(BANJO_RENUT_SHADER_CACHE_CPP "${_peggle1_renut_shaders}/shader_cache_switch.cpp" CACHE FILEPATH
        "Renut's shader cache for the Switch build (from a PC build: tools/renut_switch_shader_cache.py)" FORCE)
    function(banjo_renut_shader_cache_override TARGET)
        if(NOT EXISTS "${BANJO_RENUT_SHADER_CACHE_CPP}")
            message(FATAL_ERROR "No ${BANJO_RENUT_SHADER_CACHE_CPP}: build the PC tree first, then run "
                                "tools/renut_switch_shader_cache.py (Renut's XenosRecomp runs on the host)")
        endif()
        target_sources(${TARGET} PRIVATE "${BANJO_RENUT_SHADER_CACHE_CPP}")
        set_source_files_properties("${BANJO_RENUT_SHADER_CACHE_CPP}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endfunction()
endif()

include("${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/gpu.cmake")

add_library(banjo_renut_gpu OBJECT
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/banjo_renut_select.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/banjo_renut_tuning.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/banjo_renut_checks.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/banjo_renut_record.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/display_sync.cpp"
    "${CMAKE_SOURCE_DIR}/src/renut_engine/perf/frame_stats.cpp"
    # Peggle 2's side: banjo-nx's enhancement switches and the cvars Renut's tuning reads, on Peggle 2's switches.
    "${CMAKE_SOURCE_DIR}/src/renut_engine/peggle1_renut_glue.cpp")
target_include_directories(banjo_renut_gpu PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}"
    "${REXSDK_DIR}/src/core")
target_link_libraries(banjo_renut_gpu PRIVATE rex::runtime)
if(REXGLUE_PLATFORM_SWITCH)
    target_sources(banjo_renut_gpu PRIVATE "${CMAKE_SOURCE_DIR}/src/renut_engine/gpu/banjo_renut_deko3d.cpp")
    if(EXISTS "${BANJO_DEKO3D_GLSL_CPP}")
        target_sources(banjo_renut_gpu PRIVATE "${BANJO_DEKO3D_GLSL_CPP}")
        set_source_files_properties("${BANJO_DEKO3D_GLSL_CPP}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    else()
        message(WARNING "No ${BANJO_DEKO3D_GLSL_CPP}: renut_gpu_backend = deko3d will have no game shaders "
                        "(tools/deko3d_shader_package.py)")
    endif()
endif()
if(COMMAND rexglue_apply_target_settings)
    rexglue_apply_target_settings(banjo_renut_gpu)
endif()
renut_gpu_setup(banjo_renut_gpu)
# peggle2-nx (2026-10-20): a Switch build without the Vulkan driver runs Renut's renderer always (banjo_renut_select.cpp).
if(REXGLUE_PLATFORM_SWITCH AND NOT REXGLUE_SWITCH_LINK_NVK)
    target_compile_definitions(banjo_renut_gpu PRIVATE PEGGLE1_NO_VULKAN=1)
endif()
target_link_libraries(peggle1 PRIVATE banjo_renut_gpu)
target_compile_definitions(peggle1 PRIVATE PEGGLE1_RENUT_RENDERER=1)
