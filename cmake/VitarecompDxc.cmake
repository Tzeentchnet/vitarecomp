# DXC discovery contract for the shared HLSL shader pipeline.
#
# This module never downloads a compiler. Tests and the mock backend do not
# require DXC. A production backend can link Vitarecomp::DXC only when the
# executable reports the exact pinned version below.

set(VITARECOMP_DXC_RELEASE_TAG "v1.8.2505.1")
set(VITARECOMP_DXC_PINNED_VERSION "1.8.2505.32")
set(VITARECOMP_DXC_ROOT "" CACHE PATH
    "Directory containing the pinned dxc executable")
set(VITARECOMP_DXC_REQUIRED OFF CACHE BOOL
    "Fail configure when the exact pinned DXC is unavailable")

function(vitarecomp_find_dxc)
    if(TARGET Vitarecomp::DXC)
        return()
    endif()

    find_program(VITARECOMP_DXC_EXECUTABLE
        NAMES dxc dxc.exe
        HINTS
            "${VITARECOMP_DXC_ROOT}"
            "$ENV{VITARECOMP_DXC_ROOT}"
        PATH_SUFFIXES bin/x64 bin
        DOC "Pinned DirectX Shader Compiler executable")

    set(_dxc_accepted FALSE)
    set(_dxc_version "")
    if(VITARECOMP_DXC_EXECUTABLE)
        execute_process(
            COMMAND "${VITARECOMP_DXC_EXECUTABLE}" --version
            RESULT_VARIABLE _dxc_result
            OUTPUT_VARIABLE _dxc_stdout
            ERROR_VARIABLE _dxc_stderr
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_STRIP_TRAILING_WHITESPACE
        )
        set(_dxc_output "${_dxc_stdout}\n${_dxc_stderr}")
        string(REGEX MATCH
            "[0-9]+\\.[0-9]+\\.[0-9]+(\\.[0-9]+)?"
            _dxc_version
            "${_dxc_output}"
        )
        if(_dxc_result EQUAL 0 AND
           _dxc_version STREQUAL VITARECOMP_DXC_PINNED_VERSION)
            set(_dxc_accepted TRUE)
        endif()
    endif()

    if(_dxc_accepted)
        add_executable(Vitarecomp_DXC IMPORTED GLOBAL)
        set_property(TARGET Vitarecomp_DXC PROPERTY
            IMPORTED_LOCATION "${VITARECOMP_DXC_EXECUTABLE}")
        add_executable(Vitarecomp::DXC ALIAS Vitarecomp_DXC)
        set(VITARECOMP_DXC_FOUND TRUE CACHE BOOL
            "Whether the exact pinned DXC was found" FORCE)
        message(STATUS
            "vitarecomp: found pinned DXC ${_dxc_version}: "
            "${VITARECOMP_DXC_EXECUTABLE}")
    else()
        set(VITARECOMP_DXC_FOUND FALSE CACHE BOOL
            "Whether the exact pinned DXC was found" FORCE)
        if(VITARECOMP_DXC_REQUIRED)
            if(VITARECOMP_DXC_EXECUTABLE)
                message(FATAL_ERROR
                    "vitarecomp requires DXC "
                    "${VITARECOMP_DXC_PINNED_VERSION}, but "
                    "'${VITARECOMP_DXC_EXECUTABLE}' reported "
                    "'${_dxc_version}'")
            else()
                message(FATAL_ERROR
                    "vitarecomp requires DXC "
                    "${VITARECOMP_DXC_PINNED_VERSION}; set "
                    "VITARECOMP_DXC_ROOT to its installation")
            endif()
        elseif(VITARECOMP_DXC_EXECUTABLE)
            message(STATUS
                "vitarecomp: ignoring DXC '${VITARECOMP_DXC_EXECUTABLE}' "
                "(reported '${_dxc_version}', pinned "
                "${VITARECOMP_DXC_PINNED_VERSION})")
        else()
            message(STATUS
                "vitarecomp: pinned DXC "
                "${VITARECOMP_DXC_PINNED_VERSION} not found "
                "(not required for contract tests)")
        endif()
    endif()

    set(VITARECOMP_DXC_EXECUTABLE "${VITARECOMP_DXC_EXECUTABLE}"
        PARENT_SCOPE)
    set(VITARECOMP_DXC_FOUND "${VITARECOMP_DXC_FOUND}" PARENT_SCOPE)
endfunction()
