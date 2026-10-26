/* hle_kernel.c — SceLibKernel: the process.
 *
 * The first of the kernel surface. Threads, synchronisation and timing follow
 * here as the milestone needs them (docs/PLAN.md, milestone A).
 */

#include "clock.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"

#include <string.h>
#include <time.h>

#define SCE_APPUTIL_ERROR_PARAMETER UINT32_C(0x80100600)
#define SCE_SYSTEM_PARAM_USERNAME_MAXSIZE 17u

static uint32_t g_process_param;

vita_result vita_process_param_set(uint32_t address) {
    if (!vita_runtime_is_initialized())
        return VITA_ERROR_NOT_INITIALIZED;
    if (!address)
        return VITA_ERROR_INVALID_ARGUMENT;
    if (!vita_mem_ptr(address, sizeof(uint32_t)))
        return VITA_ERROR_OUT_OF_BOUNDS;
    g_process_param = address;
    return VITA_OK;
}

void vita_process_param_reset(void) { g_process_param = 0u; }

/* const void *sceKernelGetProcessParam(void) */
void vita_hle_sceKernelGetProcessParam(void) { r0 = g_process_param; }

void vita_hle___stack_chk_fail(void) {
    vita_hle_trace(
        "__stack_chk_fail(lr=0x%08X, current=0x%08X, saved=0x%08X, guard=0x%08X)",
        lr, r0, r1, r6);
    vita_trap_import(lr & ~UINT32_C(1), UINT32_C(0x37691BF8),
                     "SceLibKernel::__stack_chk_fail");
}

/* int sceKernelExitProcess(int status): never returns. */
void vita_hle_sceKernelExitProcess(void) {
    vita_host_exit((int)r0);
}

/* --- the process clock ---------------------------------------------------------
 *
 * Microseconds since the process started, from the runtime's clock
 * (clock.h): the host's unless the host asked for a virtual one, and never
 * the frame counter alone, because a program that spins on this clock until
 * it moves must see it move. */

/* SceUInt64 sceKernelGetProcessTimeWide(void): a 64-bit result, in r0:r1 */
void vita_hle_sceKernelGetProcessTimeWide(void) {
    uint64_t t = vita_clock_us();
    r0 = (uint32_t)t;
    r1 = (uint32_t)(t >> 32);
}

/* SceUInt32 sceKernelGetProcessTimeLow(void) */
void vita_hle_sceKernelGetProcessTimeLow(void) { r0 = (uint32_t)vita_clock_us(); }

/* int sceKernelPowerTick(int type): keeps the console awake; nothing to keep. */
void vita_hle_sceKernelPowerTick(void) { r0 = 0; }

void vita_hle_sceSysmoduleLoadModule(void) {
    (void)vita_hle_unimplemented(vita_last_pc, 0, 0,
                                 "SceSysmodule", "sceSysmoduleLoadModule");
}
void vita_hle_sceSysmoduleUnloadModule(void) {
    (void)vita_hle_unimplemented(vita_last_pc, 0, 0,
                                 "SceSysmodule", "sceSysmoduleUnloadModule");
}
void vita_hle_sceSysmoduleIsLoaded(void) {
    (void)vita_hle_unimplemented(vita_last_pc, 0, 0,
                                 "SceSysmodule", "sceSysmoduleIsLoaded");
}
void vita_hle_scePowerSetConfigurationMode(void) {
    (void)vita_hle_unimplemented(vita_last_pc, 0, 0,
                                 "ScePower", "scePowerSetConfigurationMode");
}
void vita_hle_scePowerSetArmClockFrequency(void) {
    (void)vita_hle_unimplemented(vita_last_pc, 0, 0,
                                 "ScePower", "scePowerSetArmClockFrequency");
}
void vita_hle_scePowerSetGpuClockFrequency(void) {
    (void)vita_hle_unimplemented(vita_last_pc, 0, 0,
                                 "ScePower", "scePowerSetGpuClockFrequency");
}
void vita_hle_scePowerSetBusClockFrequency(void) {
    (void)vita_hle_unimplemented(vita_last_pc, 0, 0,
                                 "ScePower", "scePowerSetBusClockFrequency");
}
void vita_hle_scePowerSetGpuXbarClockFrequency(void) {
    (void)vita_hle_unimplemented(vita_last_pc, 0, 0,
                                 "ScePower", "scePowerSetGpuXbarClockFrequency");
}

void vita_hle_sceAppUtilInit(void) {
    (void)vita_hle_unimplemented(vita_last_pc, 0, 0,
                                 "SceAppUtil", "sceAppUtilInit");
}
void vita_hle_sceAppUtilShutdown(void) {
    (void)vita_hle_unimplemented(vita_last_pc, 0, 0,
                                 "SceAppUtil", "sceAppUtilShutdown");
}

/* int sceAppUtilSystemParamGetInt(unsigned int paramId, int *value) */
void vita_hle_sceAppUtilSystemParamGetInt(void) {
    uint32_t param_id = r0;
    uint32_t value_address = r1;
    uint32_t value;

    if (!vita_mem_ptr(value_address, sizeof(value))) {
        r0 = (uint32_t)VITA_ERROR_OUT_OF_BOUNDS;
        vita_trap_unimpl(value_address, sizeof(value),
                         "sceAppUtilSystemParamGetInt value");
        return;
    }

    switch (param_id) {
    case 1u:
        value = 1u; /* SCE_SYSTEM_PARAM_LANG_ENGLISH_US */
        break;
    case 2u:
        value = 1u; /* SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS */
        break;
    default:
        r0 = (uint32_t)VITA_ERROR_INVALID_ARGUMENT;
        vita_trap_unimpl(vita_last_pc, param_id,
                         "sceAppUtilSystemParamGetInt parameter");
        return;
    }

    vita_write32(value_address, value);
    vita_hle_trace("sceAppUtilSystemParamGetInt(%u) -> %u", param_id, value);
    r0 = 0u;
}

/* int sceAppUtilSystemParamGetString(unsigned int paramId, char *buf,
 *                                    SceSize bufSize) */
void vita_hle_sceAppUtilSystemParamGetString(void) {
    static const char username[] = "Player";
    uint32_t param_id = r0;
    uint32_t buffer = r1;
    uint32_t buffer_size = r2;

    if (param_id != 3u) {
        r0 = (uint32_t)VITA_ERROR_INVALID_ARGUMENT;
        vita_trap_unimpl(vita_last_pc, param_id,
                         "sceAppUtilSystemParamGetString parameter");
        return;
    }
    if (!buffer_size) {
        r0 = SCE_APPUTIL_ERROR_PARAMETER;
        return;
    }

    uint32_t copy_size = buffer_size < SCE_SYSTEM_PARAM_USERNAME_MAXSIZE
        ? buffer_size
        : SCE_SYSTEM_PARAM_USERNAME_MAXSIZE;
    if (!vita_mem_ptr(buffer, copy_size)) {
        r0 = (uint32_t)VITA_ERROR_OUT_OF_BOUNDS;
        vita_trap_unimpl(buffer, copy_size,
                         "sceAppUtilSystemParamGetString buffer");
        return;
    }

    size_t length = strlen(username);
    if (length >= copy_size) length = copy_size - 1u;
    if (length) (void)vita_mem_copy_to_guest(buffer, username, length);
    vita_write8(buffer + (uint32_t)length, 0u);
    vita_hle_trace("sceAppUtilSystemParamGetString(%u) -> \"%s\"",
                   param_id, username);
    r0 = 0u;
}
