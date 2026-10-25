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
)
target_include_directories(test_reloc PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/armrecomp
)
target_compile_options(test_reloc PRIVATE -UNDEBUG)
add_test(NAME reloc COMMAND test_reloc)

add_executable(test_discover
    test_discover.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/analyze.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/decode.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/module.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/reloc.c
)
target_include_directories(test_discover PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/armrecomp
)
target_compile_options(test_discover PRIVATE -UNDEBUG)
add_test(NAME discover COMMAND test_discover)

add_executable(test_emit
    test_emit.c
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

add_executable(test_foundation test_foundation.c)
target_link_libraries(test_foundation PRIVATE vitarecomp)
if(MSVC)
    target_compile_options(test_foundation PRIVATE /UNDEBUG)
else()
    target_compile_options(test_foundation PRIVATE -UNDEBUG)
endif()
add_test(NAME runtime-foundation COMMAND test_foundation)
set_tests_properties(runtime-foundation PROPERTIES TIMEOUT 30)

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
endif()
