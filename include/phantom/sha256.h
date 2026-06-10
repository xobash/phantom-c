#ifndef PHANTOM_SHA256_H
#define PHANTOM_SHA256_H
/* Self-contained SHA-256 (FIPS 180-4) so catalog integrity checks work
 * identically on Windows, macOS, and Linux without shelling out. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buffer[64];
    size_t buffer_len;
} ph_sha256_ctx;

void ph_sha256_init(ph_sha256_ctx *ctx);
void ph_sha256_update(ph_sha256_ctx *ctx, const void *data, size_t len);
void ph_sha256_final(ph_sha256_ctx *ctx, uint8_t digest[32]);

/* Hash a file and write the lowercase hex digest into out[65]. */
bool ph_sha256_file_hex(const char *path, char out[65]);
#endif
