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

add_executable(test_runtime test_runtime.c)
target_link_libraries(test_runtime PRIVATE vitarecomp)
target_compile_options(test_runtime PRIVATE -UNDEBUG)
add_test(NAME runtime COMMAND test_runtime)

add_executable(test_prior_aot test_prior_aot.c)
target_link_libraries(test_prior_aot PRIVATE vitarecomp)
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
