# vitarecomp â€” a static-recompilation toolkit for PlayStation Vita games.
#
# This file builds three things:
#   1. the `vitarecomp` runtime static library â€” what recompiled game code links
#      against (CPU state, memory, semantic helpers)
#   2. `armrecomp` â€” the tool that turns a Vita module into C
#   3. `vitarecomp_gpu` - the optional renderer-neutral C ABI and C++20 core
#
# A game repo consumes this repo as a git submodule and does:
#     add_subdirectory(vitarecomp)
#     add_subdirectory(games/<game>)
# and links its host against the `vitarecomp` target.
#
# The core has no external dependencies on purpose. SDL2, zlib and friends only
# ever appear behind an opt-in option, so a fresh clone builds with nothing
# installed. (zlib matters here in a way it did not for psprecomp: SELF segments
# are routinely zlib-compressed *as well as* encrypted. The inflate path is
# opt-in and the container parser reports compression without needing it.)

cmake_minimum_required(VERSION 3.16)
project(vitarecomp LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /WX /permissive-)
    # The generated C is millions of lines; MSVC's default object format
    # cannot hold that many sections.
    add_compile_options(/bigobj)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# --- the runtime library ------------------------------------------------------
#
# The runtime remains C-only and title-neutral. Archive inflation reuses the
# repository's small audited inflater privately; it is not part of the public
# runtime API.

find_package(Threads REQUIRED)

add_library(vitarecomp STATIC
    src/cpu.c
    src/mem.c
    src/excl.c
    src/trap.c
    src/loader.c
    src/reloc.c
    src/binding.c
    src/dispatch.c
    src/vfp.c
    src/hle_libc.c
    src/hle_clib.c
    src/hle_scanf.c
    src/hle_libm.c
    src/hle_mspace.c
    src/hle_registry.c
    src/hle_kernel.c
    src/hle_sysmem.c
    src/hle_threadmgr.c
    src/hle_io.c
    src/hle_fios.c
    src/hle_ult.c
    src/clock.c
    src/sched.c
    src/runtime.c
    src/vfs.c
    src/psarc.c
    tools/armrecomp/inflate.c
)

target_include_directories(vitarecomp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(vitarecomp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/armrecomp
)
target_compile_definitions(vitarecomp PUBLIC VITARECOMP_THREADED=1)
target_link_libraries(vitarecomp PUBLIC Threads::Threads)
if(NOT WIN32)
    target_link_libraries(vitarecomp PUBLIC m)
endif()

# --- renderer-neutral GPU contract ------------------------------------------
#
# The public surface is C. C++20 is confined to this target and its contract
# tests so the runtime, generated code, and recompiler remain C-only.

option(VITARECOMP_BUILD_GPU "Build the renderer-neutral GPU contract" ON)
option(VITARECOMP_ENABLE_DX12
    "Build the production DirectX 12 backend on Windows" ON)
option(VITARECOMP_ENABLE_VULKAN
    "Build the Windows Vulkan production backend when headers are available"
    ON)
set(VITARECOMP_VULKAN_HEADERS_ROOT "" CACHE PATH
    "Directory containing a pinned Vulkan-Headers include tree")
option(VITARECOMP_REQUIRE_VULKAN_BACKEND
    "Fail configure when the requested Windows Vulkan backend is unavailable"
    OFF)
option(VITARECOMP_BUILD_DX12_CONFORMANCE
    "Build the Win32 DX12 shader/presentation conformance executable" OFF)
option(VITARECOMP_BUILD_VULKAN_CONFORMANCE
    "Build the Win32 Vulkan shader/presentation conformance executable" OFF)

function(vitarecomp_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror
        )
    endif()
endfunction()

if((VITARECOMP_BUILD_DX12_CONFORMANCE OR
    VITARECOMP_BUILD_VULKAN_CONFORMANCE) AND NOT VITARECOMP_BUILD_GPU)
    message(FATAL_ERROR
        "GPU conformance executables require VITARECOMP_BUILD_GPU=ON")
endif()

if(VITARECOMP_BUILD_GPU)
    add_library(vitarecomp_gpu STATIC
        src/gpu/backend_registry.cpp
        src/gpu/gpu_api.cpp
        src/gpu/gpu_runtime.cpp
        src/gpu/gpu_validation.cpp
        src/gpu/shader_contract.cpp
    )

    add_library(vitarecomp::gpu ALIAS vitarecomp_gpu)

    target_include_directories(vitarecomp_gpu
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu
    )
    target_compile_features(vitarecomp_gpu PRIVATE cxx_std_20)
    target_compile_definitions(vitarecomp_gpu PRIVATE VITA_GPU_BUILD)
    vitarecomp_project_warnings(vitarecomp_gpu)

    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/VitarecompDxc.cmake)
    vitarecomp_find_dxc()

    if(WIN32 AND VITARECOMP_ENABLE_DX12)
        target_sources(vitarecomp_gpu PRIVATE
            src/gpu/dx12_backend.cpp
        )
        target_compile_definitions(vitarecomp_gpu PRIVATE
            VITA_GPU_HAS_DX12_BACKEND
        )
        target_link_libraries(vitarecomp_gpu PRIVATE
            d3d12
            dxgi
        )
    elseif(VITARECOMP_BUILD_DX12_CONFORMANCE)
        message(FATAL_ERROR
            "VITARECOMP_BUILD_DX12_CONFORMANCE requires Windows and "
            "VITARECOMP_ENABLE_DX12=ON")
    endif()

    if(WIN32 AND VITARECOMP_ENABLE_VULKAN)
        set(_vitarecomp_vulkan_include_dirs "")
        if(VITARECOMP_VULKAN_HEADERS_ROOT)
            find_path(VITARECOMP_VULKAN_INCLUDE_DIR
                NAMES vulkan/vulkan.h
                PATHS "${VITARECOMP_VULKAN_HEADERS_ROOT}"
                PATH_SUFFIXES include Include
                NO_DEFAULT_PATH
            )
            if(VITARECOMP_VULKAN_INCLUDE_DIR AND
               EXISTS
                   "${VITARECOMP_VULKAN_INCLUDE_DIR}/vulkan/vulkan.hpp")
                set(_vitarecomp_vulkan_include_dirs
                    "${VITARECOMP_VULKAN_INCLUDE_DIR}")
            endif()
        else()
            find_package(Vulkan 1.3 QUIET)
            if(Vulkan_FOUND)
                set(_vitarecomp_vulkan_include_dirs
                    "${Vulkan_INCLUDE_DIRS}")
            endif()
        endif()

        if(_vitarecomp_vulkan_include_dirs)
            set(VITARECOMP_VULKAN_BACKEND_FOUND TRUE CACHE INTERNAL
                "Whether the Windows Vulkan backend is compiled" FORCE)
            target_sources(vitarecomp_gpu PRIVATE
                src/gpu/vulkan_backend.cpp
            )
            target_include_directories(vitarecomp_gpu SYSTEM PRIVATE
                ${_vitarecomp_vulkan_include_dirs}
            )
            target_compile_definitions(vitarecomp_gpu PRIVATE
                VITA_GPU_HAS_VULKAN_BACKEND
            )
            target_link_libraries(vitarecomp_gpu PRIVATE user32)
            message(STATUS
                "vitarecomp: Windows Vulkan production backend enabled")
        else()
            set(VITARECOMP_VULKAN_BACKEND_FOUND FALSE CACHE INTERNAL
                "Whether the Windows Vulkan backend is compiled" FORCE)
            message(STATUS
                "vitarecomp: Vulkan 1.3 headers not found; Vulkan backend unavailable")
        endif()
    elseif(VITARECOMP_BUILD_VULKAN_CONFORMANCE)
        message(FATAL_ERROR
            "VITARECOMP_BUILD_VULKAN_CONFORMANCE requires Windows and "
            "VITARECOMP_ENABLE_VULKAN=ON")
    endif()

    if(WIN32 AND VITARECOMP_ENABLE_VULKAN AND
       VITARECOMP_REQUIRE_VULKAN_BACKEND AND
       NOT VITARECOMP_VULKAN_BACKEND_FOUND)
        message(FATAL_ERROR
            "VITARECOMP_REQUIRE_VULKAN_BACKEND=ON requires Vulkan 1.3 "
            "headers. Set VITARECOMP_VULKAN_HEADERS_ROOT to a pinned "
            "Vulkan-Headers include tree or install the Vulkan SDK.")
    endif()

    if(VITARECOMP_BUILD_VULKAN_CONFORMANCE AND
       NOT VITARECOMP_VULKAN_BACKEND_FOUND)
        message(FATAL_ERROR
            "VITARECOMP_BUILD_VULKAN_CONFORMANCE requires Vulkan 1.3 headers")
    endif()
endif()

# --- the recompiler -----------------------------------------------------------

add_subdirectory(tools/armrecomp)

# --- tests (only when vitarecomp is the top-level project, not a submodule) ----

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(tests)
endif()
