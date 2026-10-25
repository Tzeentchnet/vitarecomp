/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

static void __cdecl vitarecomp_test_configure_noninteractive_failures(void) {
    (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#if defined(_MSC_VER)
    (void)_set_error_mode(_OUT_TO_STDERR);
    (void)_set_abort_behavior(
        0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#if defined(_DEBUG)
    (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
#endif
}

#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU"))
static void (__cdecl *const vitarecomp_test_noninteractive_initializer)(void) =
    vitarecomp_test_configure_noninteractive_failures;
#else
__attribute__((constructor))
static void vitarecomp_test_noninteractive_initializer(void) {
    vitarecomp_test_configure_noninteractive_failures();
}
#endif
#else
typedef int vitarecomp_test_noninteractive_translation_unit;
#endif
