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

    if(WIN32 AND VITARECOMP_VULKAN_BACKEND_FOUND AND
       TARGET Vitarecomp::DXC)
        set(_vulkan_shader_source
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan_conformance.hlsl)
        set(_vulkan_vertex_spirv
            ${CMAKE_CURRENT_BINARY_DIR}/vulkan_conformance_vs.spv)
        set(_vulkan_pixel_spirv
            ${CMAKE_CURRENT_BINARY_DIR}/vulkan_conformance_ps.spv)
        add_custom_command(
            OUTPUT ${_vulkan_vertex_spirv}
            COMMAND $<TARGET_FILE:Vitarecomp::DXC>
                -spirv
                -fspv-target-env=vulkan1.3
                -fvk-use-dx-layout
                -Ges
                -WX
                -T vs_6_0
                -E main
                -D VITARECOMP_VERTEX_SHADER=1
                -Fo ${_vulkan_vertex_spirv}
                ${_vulkan_shader_source}
            DEPENDS ${_vulkan_shader_source}
            VERBATIM
            COMMENT "Compiling conformance vertex HLSL with pinned DXC"
        )
        add_custom_command(
            OUTPUT ${_vulkan_pixel_spirv}
            COMMAND $<TARGET_FILE:Vitarecomp::DXC>
                -spirv
                -fspv-target-env=vulkan1.3
                -fvk-use-dx-layout
                -Ges
                -WX
                -T ps_6_0
                -E main
                -Fo ${_vulkan_pixel_spirv}
                ${_vulkan_shader_source}
            DEPENDS ${_vulkan_shader_source}
            VERBATIM
            COMMENT "Compiling conformance pixel HLSL with pinned DXC"
        )
        add_executable(test_vulkan_conformance
            test_vulkan_conformance.cpp
            ${_vulkan_vertex_spirv}
            ${_vulkan_pixel_spirv}
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
                ${_vulkan_vertex_spirv}
                ${_vulkan_pixel_spirv}
        )
        set_tests_properties(vulkan-conformance PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 120
            LABELS "gpu;vulkan;integration"
        )
    endif()
endif()
