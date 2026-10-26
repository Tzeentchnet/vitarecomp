# All tests are synthetic. No game data, no dump, no key material — a fresh
# clone runs the full suite with nothing installed and nothing supplied.

add_executable(test_container
    test_container.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/container.c
)

target_include_directories(test_container PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/armrecomp
)

# Release configurations define NDEBUG, which compiles every assert() in the
# suite to nothing — the tests then print "all passed" while checking nothing.
# Strip it for test targets. test_container.c also #errors if NDEBUG survives,
# so this cannot silently regress.
target_compile_options(test_container PRIVATE -UNDEBUG)

add_test(NAME container COMMAND test_container)

add_executable(test_inflate
    test_inflate.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/inflate.c
)

target_include_directories(test_inflate PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/armrecomp
)

target_compile_options(test_inflate PRIVATE -UNDEBUG)

add_test(NAME inflate COMMAND test_inflate)

add_executable(test_decode
    test_decode.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/decode.c
)

target_include_directories(test_decode PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/armrecomp
)

target_compile_options(test_decode PRIVATE -UNDEBUG)

add_test(NAME decode COMMAND test_decode)

add_executable(test_reloc
    test_reloc.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/container.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/reloc.c
    ${CMAKE_SOURCE_DIR}/src/reloc.c
)
target_include_directories(test_reloc PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/armrecomp
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_options(test_reloc PRIVATE -UNDEBUG)
add_test(NAME reloc COMMAND test_reloc)

add_executable(test_discover
    test_discover.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/analyze.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/decode.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/module.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/reloc.c
    ${CMAKE_SOURCE_DIR}/src/reloc.c
)
target_include_directories(test_discover PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/armrecomp
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_options(test_discover PRIVATE -UNDEBUG)
add_test(NAME discover COMMAND test_discover)

add_executable(test_emit
    test_emit.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/compose.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/emit.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/decode.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/module.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/nids.c
)
target_include_directories(test_emit PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/armrecomp
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_options(test_emit PRIVATE -UNDEBUG)
add_test(NAME emit COMMAND test_emit)

add_executable(test_compose
    test_compose.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/compose.c
)
target_include_directories(test_compose PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/armrecomp
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_options(test_compose PRIVATE -UNDEBUG)
add_test(NAME compose-generated-image-set COMMAND test_compose)
set_tests_properties(compose-generated-image-set PROPERTIES
    LABELS "armrecomp;composition;prior-aot;negative"
)

add_test(
    NAME homebrew-e2e
    COMMAND
        ${CMAKE_COMMAND}
        "-DARMRECOMP=$<TARGET_FILE:armrecomp>"
        "-DROOT=${CMAKE_SOURCE_DIR}"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/homebrew-e2e"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DCONFIG=$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/test_homebrew.cmake"
)
set_tests_properties(homebrew-e2e PROPERTIES TIMEOUT 180)

add_executable(test_runtime test_runtime.c)
target_link_libraries(test_runtime PRIVATE vitarecomp)
target_compile_options(test_runtime PRIVATE -UNDEBUG)
add_test(NAME runtime COMMAND test_runtime)

add_executable(test_entry_state test_entry_state.c)
target_link_libraries(test_entry_state PRIVATE vitarecomp)
add_test(NAME entry-state COMMAND test_entry_state)
set_tests_properties(entry-state PROPERTIES
    LABELS "runtime;entry-state;tls;negative"
)

add_executable(test_loader test_loader.c)
target_link_libraries(test_loader PRIVATE vitarecomp)
if(MSVC)
    target_compile_options(test_loader PRIVATE /UNDEBUG)
else()
    target_compile_options(test_loader PRIVATE -UNDEBUG)
endif()
add_test(NAME loader-strict COMMAND test_loader)
set_tests_properties(loader-strict PROPERTIES
    LABELS "runtime;loader;negative"
)

add_executable(test_binding test_binding.c)
target_link_libraries(test_binding PRIVATE vitarecomp)
if(MSVC)
    target_compile_options(test_binding PRIVATE /UNDEBUG)
else()
    target_compile_options(test_binding PRIVATE -UNDEBUG)
endif()
add_test(NAME module-set-binding COMMAND test_binding)
set_tests_properties(module-set-binding PROPERTIES
    LABELS "runtime;binding;dispatch;imports;negative"
)

add_executable(generate_binding_e2e
    generate_binding_e2e.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/emit.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/decode.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/module.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/nids.c
)
target_include_directories(generate_binding_e2e PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/armrecomp
    ${CMAKE_SOURCE_DIR}/include
)

set(_emitted_binding_dir "${CMAKE_CURRENT_BINARY_DIR}/emitted-binding")
set(_emitted_binding_sources
    "${_emitted_binding_dir}/binding_a.c"
    "${_emitted_binding_dir}/binding_b.c"
)
set(_emitted_binding_headers
    "${_emitted_binding_dir}/binding_a.h"
    "${_emitted_binding_dir}/binding_b.h"
)
set(_emitted_binding_defaults
    "${_emitted_binding_dir}/vita_hle_crossModule.c"
    "${_emitted_binding_dir}/vita_hle_hleOverride.c"
    "${_emitted_binding_dir}/vita_hle_namedUnsupported.c"
)
add_custom_command(
    OUTPUT
        ${_emitted_binding_sources}
        ${_emitted_binding_headers}
        ${_emitted_binding_defaults}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_emitted_binding_dir}"
    COMMAND $<TARGET_FILE:generate_binding_e2e> "${_emitted_binding_dir}"
    DEPENDS generate_binding_e2e
    VERBATIM
)
set_source_files_properties(
    ${_emitted_binding_sources}
    ${_emitted_binding_defaults}
    PROPERTIES GENERATED TRUE
)
add_library(test_emitted_binding_defaults STATIC ${_emitted_binding_defaults})
target_include_directories(test_emitted_binding_defaults PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    "${_emitted_binding_dir}"
)
add_executable(test_emitted_binding
    test_emitted_binding.c
    ${_emitted_binding_sources}
    ${_emitted_binding_headers}
)
target_include_directories(test_emitted_binding PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    "${_emitted_binding_dir}"
)
target_link_libraries(test_emitted_binding PRIVATE
    vitarecomp
    test_emitted_binding_defaults
)
if(MSVC)
    target_compile_options(test_emitted_binding PRIVATE /UNDEBUG)
    target_compile_options(test_emitted_binding_defaults PRIVATE /UNDEBUG)
else()
    target_compile_options(test_emitted_binding PRIVATE -UNDEBUG)
    target_compile_options(test_emitted_binding_defaults PRIVATE -UNDEBUG)
endif()
add_test(NAME emitted-module-set-binding COMMAND test_emitted_binding)
set_tests_properties(emitted-module-set-binding PROPERTIES
    LABELS "armrecomp;runtime;binding;dispatch;imports;negative"
)

add_executable(test_foundation test_foundation.c)
target_link_libraries(test_foundation PRIVATE vitarecomp)
if(MSVC)
    target_compile_options(test_foundation PRIVATE /UNDEBUG)
else()
    target_compile_options(test_foundation PRIVATE -UNDEBUG)
endif()
add_test(NAME runtime-foundation COMMAND test_foundation)
set_tests_properties(runtime-foundation PROPERTIES
    LABELS "runtime;threading;vfs;allocator;imports;shutdown"
    TIMEOUT 30
)

add_executable(test_prior_aot test_prior_aot.c)
find_package(Threads REQUIRED)
target_link_libraries(test_prior_aot PRIVATE vitarecomp Threads::Threads)
target_compile_options(test_prior_aot PRIVATE -UNDEBUG)
add_test(NAME prior-aot COMMAND test_prior_aot)

add_executable(test_dispatch_diagnostic test_dispatch_diagnostic.c)
target_link_libraries(test_dispatch_diagnostic PRIVATE vitarecomp)
target_compile_options(test_dispatch_diagnostic PRIVATE -UNDEBUG)
add_test(NAME prior-aot-dispatch-diagnostic COMMAND test_dispatch_diagnostic)
set_tests_properties(
    prior-aot-dispatch-diagnostic
    PROPERTIES
        ENVIRONMENT "VITARECOMP_TRACE=1"
        PASS_REGULAR_EXPRESSION
            "TRACE indirect 0x00001000 -> 0x00002001"
)

if(VITARECOMP_BUILD_GPU)
    add_executable(test_gpu_contract
        test_gpu_contract.cpp
        ${CMAKE_SOURCE_DIR}/src/gpu/test_only_null_backend.cpp
    )
    target_link_libraries(test_gpu_contract PRIVATE vitarecomp::gpu)
    target_include_directories(test_gpu_contract PRIVATE
        ${CMAKE_SOURCE_DIR}/src/gpu
    )
    target_compile_features(test_gpu_contract PRIVATE cxx_std_20)
    vitarecomp_project_warnings(test_gpu_contract)
    if(MSVC)
        target_compile_options(test_gpu_contract PRIVATE /UNDEBUG)
    else()
        target_compile_options(test_gpu_contract PRIVATE -UNDEBUG)
    endif()
    add_test(NAME gpu-contract COMMAND test_gpu_contract)

    add_executable(test_gpu_c_abi test_gpu_c_abi.c)
    target_link_libraries(test_gpu_c_abi PRIVATE vitarecomp::gpu)
    vitarecomp_project_warnings(test_gpu_c_abi)
    if(MSVC)
        target_compile_options(test_gpu_c_abi PRIVATE /UNDEBUG)
    else()
        target_compile_options(test_gpu_c_abi PRIVATE -UNDEBUG)
    endif()
    add_test(NAME gpu-c-abi COMMAND test_gpu_c_abi)

    if(WIN32 AND VITARECOMP_ENABLE_DX12)
        add_executable(test_dx12_warp test_dx12_warp.cpp)
        target_link_libraries(test_dx12_warp PRIVATE vitarecomp::gpu)
        target_compile_features(test_dx12_warp PRIVATE cxx_std_20)
        vitarecomp_project_warnings(test_dx12_warp)
        if(MSVC)
            target_compile_options(test_dx12_warp PRIVATE /UNDEBUG)
        else()
            target_compile_options(test_dx12_warp PRIVATE -UNDEBUG)
        endif()
        add_test(NAME gpu-dx12-warp-buffer COMMAND test_dx12_warp)
        set_tests_properties(gpu-dx12-warp-buffer PROPERTIES TIMEOUT 60)
    endif()

    if(WIN32 AND VITARECOMP_VULKAN_BACKEND_FOUND)
        add_test(
            NAME gpu-vulkan-invalid-icd-no-fallback
            COMMAND test_gpu_c_abi --expect-vulkan-unavailable
        )
        set(_invalid_vulkan_icd
            "${CMAKE_CURRENT_BINARY_DIR}/nonexistent-vulkan-icd.json")
        set_tests_properties(gpu-vulkan-invalid-icd-no-fallback PROPERTIES
            ENVIRONMENT
                "VK_ICD_FILENAMES=${_invalid_vulkan_icd};VK_DRIVER_FILES=${_invalid_vulkan_icd}"
            LABELS "gpu;vulkan;negative"
            TIMEOUT 60
        )
    endif()

    if(VITARECOMP_BUILD_DX12_CONFORMANCE OR
       VITARECOMP_BUILD_VULKAN_CONFORMANCE)
        if(NOT TARGET Vitarecomp::DXC)
            message(FATAL_ERROR
                "GPU conformance shaders require the pinned official "
                "Microsoft DXC ${VITARECOMP_DXC_PINNED_VERSION} "
                "(release ${VITARECOMP_DXC_RELEASE_TAG}). Set "
                "VITARECOMP_DXC_ROOT to the extracted release directory; "
                "the build never downloads compiler binaries.")
        endif()
        set(_gpu_conformance_shader_source
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gpu_conformance.hlsl")
        set(_gpu_conformance_shader_directory
            "${CMAKE_CURRENT_BINARY_DIR}/gpu-conformance-shaders")
        set(_gpu_conformance_shader_flags
            -HV 2021
            -Ges
            -WX
            -O3
            -Qstrip_debug
        )
    endif()

    if(VITARECOMP_BUILD_DX12_CONFORMANCE)
            set(_dx12_vertex_shader
                "${_gpu_conformance_shader_directory}/dx12_conformance_vs.dxil")
            set(_dx12_pixel_shader
                "${_gpu_conformance_shader_directory}/dx12_conformance_ps.dxil")
            add_custom_command(
                OUTPUT "${_dx12_vertex_shader}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${_gpu_conformance_shader_directory}"
                COMMAND $<TARGET_FILE:Vitarecomp::DXC>
                    ${_gpu_conformance_shader_flags}
                    -Qstrip_reflect
                    -T vs_6_0 -E vs_main
                    -Fo "${_dx12_vertex_shader}"
                    "${_gpu_conformance_shader_source}"
                DEPENDS "${_gpu_conformance_shader_source}" Vitarecomp::DXC
                VERBATIM
                COMMENT "Compiling deterministic DX12 conformance vertex shader"
            )
            add_custom_command(
                OUTPUT "${_dx12_pixel_shader}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${_gpu_conformance_shader_directory}"
                COMMAND $<TARGET_FILE:Vitarecomp::DXC>
                    ${_gpu_conformance_shader_flags}
                    -Qstrip_reflect
                    -T ps_6_0 -E ps_main
                    -Fo "${_dx12_pixel_shader}"
                    "${_gpu_conformance_shader_source}"
                DEPENDS "${_gpu_conformance_shader_source}" Vitarecomp::DXC
                VERBATIM
                COMMENT "Compiling deterministic DX12 conformance pixel shader"
            )
            add_custom_target(dx12_conformance_shaders
                DEPENDS "${_dx12_vertex_shader}" "${_dx12_pixel_shader}")

            add_executable(vitarecomp_dx12_conformance
                dx12_conformance.cpp
            )
            add_dependencies(vitarecomp_dx12_conformance
                dx12_conformance_shaders)
            target_link_libraries(vitarecomp_dx12_conformance PRIVATE
                vitarecomp::gpu
            )
            target_compile_features(vitarecomp_dx12_conformance PRIVATE
                cxx_std_20
            )
            vitarecomp_project_warnings(vitarecomp_dx12_conformance)
            if(MSVC)
                target_compile_options(vitarecomp_dx12_conformance PRIVATE
                    /UNDEBUG
                )
            else()
                target_compile_options(vitarecomp_dx12_conformance PRIVATE
                    -UNDEBUG
                )
            endif()
            add_custom_command(TARGET vitarecomp_dx12_conformance POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_dx12_vertex_shader}"
                    "$<TARGET_FILE_DIR:vitarecomp_dx12_conformance>/dx12_conformance_vs.dxil"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_dx12_pixel_shader}"
                    "$<TARGET_FILE_DIR:vitarecomp_dx12_conformance>/dx12_conformance_ps.dxil"
                VERBATIM
            )
            add_test(NAME gpu-dx12-warp-conformance
                COMMAND vitarecomp_dx12_conformance
                    --warp --hidden --iterations 3 --frames 4
            )
            set_tests_properties(gpu-dx12-warp-conformance PROPERTIES
                TIMEOUT 180
                LABELS "gpu;dx12;integration"
            )
    endif()

    if(VITARECOMP_BUILD_VULKAN_CONFORMANCE)
        set(_vulkan_vertex_spirv
            "${_gpu_conformance_shader_directory}/vulkan_conformance_vs.spv")
        set(_vulkan_pixel_spirv
            "${_gpu_conformance_shader_directory}/vulkan_conformance_ps.spv")
        add_custom_command(
            OUTPUT "${_vulkan_vertex_spirv}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${_gpu_conformance_shader_directory}"
            COMMAND $<TARGET_FILE:Vitarecomp::DXC>
                ${_gpu_conformance_shader_flags}
                -spirv
                -fspv-target-env=vulkan1.3
                -fspv-entrypoint-name=main
                -fvk-use-dx-layout
                -T vs_6_0
                -E vs_main
                -Fo "${_vulkan_vertex_spirv}"
                "${_gpu_conformance_shader_source}"
            DEPENDS "${_gpu_conformance_shader_source}" Vitarecomp::DXC
            VERBATIM
            COMMENT "Compiling deterministic Vulkan conformance vertex shader"
        )
        add_custom_command(
            OUTPUT "${_vulkan_pixel_spirv}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${_gpu_conformance_shader_directory}"
            COMMAND $<TARGET_FILE:Vitarecomp::DXC>
                ${_gpu_conformance_shader_flags}
                -spirv
                -fspv-target-env=vulkan1.3
                -fspv-entrypoint-name=main
                -fvk-use-dx-layout
                -T ps_6_0
                -E ps_main
                -Fo "${_vulkan_pixel_spirv}"
                "${_gpu_conformance_shader_source}"
            DEPENDS "${_gpu_conformance_shader_source}" Vitarecomp::DXC
            VERBATIM
            COMMENT "Compiling deterministic Vulkan conformance pixel shader"
        )
        add_executable(test_vulkan_conformance
            test_vulkan_conformance.cpp
            "${_vulkan_vertex_spirv}"
            "${_vulkan_pixel_spirv}"
        )
        target_link_libraries(test_vulkan_conformance PRIVATE
            vitarecomp::gpu
            user32
        )
        target_compile_features(test_vulkan_conformance PRIVATE cxx_std_20)
        vitarecomp_project_warnings(test_vulkan_conformance)
        if(MSVC)
            target_compile_options(test_vulkan_conformance PRIVATE /UNDEBUG)
        else()
            target_compile_options(test_vulkan_conformance PRIVATE -UNDEBUG)
        endif()
        add_test(
            NAME vulkan-conformance
            COMMAND test_vulkan_conformance
                "${_vulkan_vertex_spirv}"
                "${_vulkan_pixel_spirv}"
        )
        set_tests_properties(vulkan-conformance PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 120
            LABELS "gpu;vulkan;integration"
        )
    endif()
endif()

# Keep assertion, abort, and crash paths non-interactive under CTest. In
# particular, Debug MSVC assertions must report to stderr rather than opening a
# desktop dialog that can outlive the test process.
set(_vitarecomp_test_targets
    test_container
    test_inflate
    test_decode
    test_reloc
    test_discover
    test_emit
    test_runtime
    test_foundation
    test_prior_aot
    test_dispatch_diagnostic
    test_gpu_contract
    test_gpu_c_abi
    test_dx12_warp
    vitarecomp_dx12_conformance
    test_vulkan_conformance
)
foreach(_target IN LISTS _vitarecomp_test_targets)
    if(TARGET ${_target})
        target_sources(${_target} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/test_noninteractive.c"
        )
    endif()
endforeach()
