/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "sha256.h"

#include <stdint.h>
#include <string.h>

static const uint32_t ROUND_CONSTANTS[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
    UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
    UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
    UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
    UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
    UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
    UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
    UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
    UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
    UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
    UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
    UINT32_C(0xc67178f2),
};

static uint32_t rotate_right(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32u - count));
}

static void transform(vita_sha256_context *context, const uint8_t block[64]) {
    uint32_t words[64] = {0};
    uint32_t working[8];

    for (uint32_t index = 0u; index < 16u; index++) {
        const uint32_t offset = index * 4u;
        words[index] = ((uint32_t)block[offset] << 24u) |
                       ((uint32_t)block[offset + 1u] << 16u) |
                       ((uint32_t)block[offset + 2u] << 8u) |
                       (uint32_t)block[offset + 3u];
    }
    for (uint32_t index = 16u; index < 64u; index++) {
        const uint32_t sigma0 = rotate_right(words[index - 15u], 7u) ^
                                rotate_right(words[index - 15u], 18u) ^
                                (words[index - 15u] >> 3u);
        const uint32_t sigma1 = rotate_right(words[index - 2u], 17u) ^
                                rotate_right(words[index - 2u], 19u) ^
                                (words[index - 2u] >> 10u);
        words[index] = words[index - 16u] + sigma0 + words[index - 7u] + sigma1;
    }
    memcpy(working, context->state, sizeof(working));
    for (uint32_t index = 0u; index < 64u; index++) {
        const uint32_t sum1 = rotate_right(working[4], 6u) ^
                              rotate_right(working[4], 11u) ^
                              rotate_right(working[4], 25u);
        const uint32_t choose =
            (working[4] & working[5]) ^ ((~working[4]) & working[6]);
        const uint32_t temporary1 =
            working[7] + sum1 + choose + ROUND_CONSTANTS[index] + words[index];
        const uint32_t sum0 = rotate_right(working[0], 2u) ^
                              rotate_right(working[0], 13u) ^
                              rotate_right(working[0], 22u);
        const uint32_t majority = (working[0] & working[1]) ^
                                  (working[0] & working[2]) ^
                                  (working[1] & working[2]);
        const uint32_t temporary2 = sum0 + majority;

        working[7] = working[6];
        working[6] = working[5];
        working[5] = working[4];
        working[4] = working[3] + temporary1;
        working[3] = working[2];
        working[2] = working[1];
        working[1] = working[0];
        working[0] = temporary1 + temporary2;
    }
    for (uint32_t index = 0u; index < 8u; index++)
        context->state[index] += working[index];
}

void vita_sha256_init(vita_sha256_context *context) {
    static const uint32_t INITIAL_STATE[8] = {
        UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85), UINT32_C(0x3c6ef372),
        UINT32_C(0xa54ff53a), UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
        UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19),
    };
    memcpy(context->state, INITIAL_STATE, sizeof(INITIAL_STATE));
    memset(context->buffer, 0, sizeof(context->buffer));
    context->buffer_size = 0u;
    context->total_bytes = 0u;
}

void vita_sha256_update(vita_sha256_context *context, const void *bytes,
                        size_t byte_count) {
    const uint8_t *input = (const uint8_t *)bytes;
    context->total_bytes += (uint64_t)byte_count;
    while (byte_count != 0u) {
        size_t available = sizeof(context->buffer) - context->buffer_size;
        size_t copied = byte_count < available ? byte_count : available;
        memcpy(context->buffer + context->buffer_size, input, copied);
        context->buffer_size += copied;
        input += copied;
        byte_count -= copied;
        if (context->buffer_size == sizeof(context->buffer)) {
            transform(context, context->buffer);
            context->buffer_size = 0u;
        }
    }
}

void vita_sha256_update_u32(vita_sha256_context *context, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t index = 0u; index < 4u; index++)
        bytes[index] = (uint8_t)(value >> (index * 8u));
    vita_sha256_update(context, bytes, sizeof(bytes));
}

void vita_sha256_update_u64(vita_sha256_context *context, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t index = 0u; index < 8u; index++)
        bytes[index] = (uint8_t)(value >> (index * 8u));
    vita_sha256_update(context, bytes, sizeof(bytes));
}

void vita_sha256_update_field(vita_sha256_context *context, const void *bytes,
                              size_t byte_count) {
    vita_sha256_update_u64(context, (uint64_t)byte_count);
    vita_sha256_update(context, bytes, byte_count);
}

void vita_sha256_finish(vita_sha256_context *context, uint8_t digest[32]) {
    const uint64_t bit_count = context->total_bytes * UINT64_C(8);
    context->buffer[context->buffer_size++] = UINT8_C(0x80);
    if (context->buffer_size > 56u) {
        while (context->buffer_size < sizeof(context->buffer))
            context->buffer[context->buffer_size++] = 0u;
        transform(context, context->buffer);
        context->buffer_size = 0u;
    }
    while (context->buffer_size < 56u)
        context->buffer[context->buffer_size++] = 0u;
    for (uint32_t index = 0u; index < 8u; index++)
        context->buffer[63u - index] = (uint8_t)(bit_count >> (index * 8u));
    transform(context, context->buffer);

    for (uint32_t index = 0u; index < 8u; index++) {
        digest[index * 4u] = (uint8_t)(context->state[index] >> 24u);
        digest[index * 4u + 1u] = (uint8_t)(context->state[index] >> 16u);
        digest[index * 4u + 2u] = (uint8_t)(context->state[index] >> 8u);
        digest[index * 4u + 3u] = (uint8_t)context->state[index];
    }
}
