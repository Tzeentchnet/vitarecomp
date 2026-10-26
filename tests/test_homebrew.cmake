if(NOT DEFINED ARMRECOMP OR NOT DEFINED ROOT OR NOT DEFINED WORK)
    message(FATAL_ERROR "ARMRECOMP, ROOT, and WORK are required")
endif()
if(NOT DEFINED CONFIG OR CONFIG STREQUAL "")
    set(CONFIG Release)
endif()

function(run_checked label)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${label} failed (${result})\n${output}\n${error}")
    endif()
endfunction()

function(snapshot_tree directory out_var)
    file(GLOB_RECURSE files
        LIST_DIRECTORIES false
        RELATIVE "${directory}"
        "${directory}/*"
    )
    list(SORT files)
    set(snapshot "")
    foreach(file IN LISTS files)
        file(SHA256 "${directory}/${file}" hash)
        string(APPEND snapshot "${file}:${hash}\n")
    endforeach()
    set(${out_var} "${snapshot}" PARENT_SCOPE)
endfunction()

foreach(sample IN ITEMS fbdemo hello threads)
    set(sample_work "${WORK}/${sample}")
    set(generated "${sample_work}/generated")
    set(elf "${sample_work}/${sample}.elf")
    file(REMOVE_RECURSE "${sample_work}")
    file(MAKE_DIRECTORY "${generated}")

    run_checked(
        "${sample} extract"
        "${ARMRECOMP}" extract
        "${ROOT}/samples/${sample}/prebuilt/eboot.bin"
        "${elf}"
    )
    run_checked(
        "${sample} emit"
        "${ARMRECOMP}" emit "${elf}" "${generated}/module.c"
    )
    if(NOT EXISTS "${generated}/module.module.json" OR
       NOT EXISTS "${generated}/module.report.json" OR
       NOT EXISTS "${generated}/module.report.txt")
        message(FATAL_ERROR "${sample}: deterministic generation metadata missing")
    endif()

    snapshot_tree("${generated}" first_snapshot)
    run_checked(
        "${sample} repeat emit"
        "${ARMRECOMP}" emit "${elf}" "${generated}/module.c"
    )
    snapshot_tree("${generated}" second_snapshot)
    if(NOT first_snapshot STREQUAL second_snapshot)
        message(FATAL_ERROR "${sample}: generated output changed on repeat")
    endif()

    file(GLOB_RECURSE generated_sources
        LIST_DIRECTORIES false
        "${generated}/*.c"
    )
    if(NOT generated_sources)
        message(FATAL_ERROR "${sample}: emitter produced no C sources")
    endif()

    set(source_lines "")
    foreach(source IN LISTS generated_sources)
        string(APPEND source_lines "  \"${source}\"\n")
    endforeach()
    file(WRITE "${sample_work}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.16)\n"
        "project(vitarecomp_homebrew_${sample} LANGUAGES C)\n"
        "set(CMAKE_C_STANDARD 11)\n"
        "add_library(generated OBJECT\n${source_lines})\n"
        "target_include_directories(generated PRIVATE\n"
        "  \"${ROOT}/include\"\n"
        "  \"${generated}\"\n"
        ")\n"
        "if(MSVC)\n"
        "  target_compile_options(generated PRIVATE /W4 /WX /permissive- /bigobj)\n"
        "  target_compile_definitions(generated PRIVATE _CRT_SECURE_NO_WARNINGS)\n"
        "else()\n"
        "  target_compile_options(generated PRIVATE -Wall -Wextra -Wpedantic -Werror)\n"
        "endif()\n"
    )

    set(configure_command
        "${CMAKE_COMMAND}" -S "${sample_work}" -B "${sample_work}/build"
        -G "${GENERATOR}"
    )
    if(DEFINED GENERATOR_PLATFORM AND NOT GENERATOR_PLATFORM STREQUAL "")
        list(APPEND configure_command -A "${GENERATOR_PLATFORM}")
    endif()
    if(DEFINED GENERATOR_TOOLSET AND NOT GENERATOR_TOOLSET STREQUAL "")
        list(APPEND configure_command -T "${GENERATOR_TOOLSET}")
    endif()
    run_checked("${sample} generated configure" ${configure_command})
    run_checked(
        "${sample} generated compile"
        "${CMAKE_COMMAND}" --build "${sample_work}/build"
        --config "${CONFIG}" --target generated --parallel
    )
endforeach()

message(STATUS "public homebrew extract/emit/repeat/compile fixtures passed")
