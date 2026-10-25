/* hle_kernel.c — SceLibKernel: the process.
 *
 * The first of the kernel surface. Threads, synchronisation and timing follow
 * here as the milestone needs them (docs/PLAN.md, milestone A).
 */

#include "clock.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"

#include <time.h>

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
