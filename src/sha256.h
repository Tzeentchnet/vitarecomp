/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VITARECOMP_SHA256_H
#define VITARECOMP_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vita_sha256_context {
    uint32_t state[8];
    uint8_t buffer[64];
    size_t buffer_size;
    uint64_t total_bytes;
} vita_sha256_context;

void vita_sha256_init(vita_sha256_context *context);
void vita_sha256_update(vita_sha256_context *context, const void *bytes,
                        size_t byte_count);
void vita_sha256_update_u32(vita_sha256_context *context, uint32_t value);
void vita_sha256_update_u64(vita_sha256_context *context, uint64_t value);
void vita_sha256_update_field(vita_sha256_context *context, const void *bytes,
                              size_t byte_count);
void vita_sha256_finish(vita_sha256_context *context, uint8_t digest[32]);

#ifdef __cplusplus
}
#endif

#endif
