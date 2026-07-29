/* Verifies the vendored BLAKE3 against the official test_vectors.json cases.
   Input is the repeating 251-byte pattern 0,1,2,...,250 defined by that file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blake3.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <input_len>\n", argv[0]); return 2; }
    size_t n = (size_t)strtoul(argv[1], NULL, 10);
    unsigned char *buf = malloc(n ? n : 1);
    for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)(i % 251);

    const char *key = "whats the Elvish word for friend";
    const char *ctx = "BLAKE3 2019-12-27 16:29:52 test vectors context";
    uint8_t out[131];
    blake3_hasher h;

    blake3_hasher_init(&h);
    blake3_hasher_update(&h, buf, n);
    blake3_hasher_finalize(&h, out, 131);
    for (int i = 0; i < 131; i++) printf("%02x", out[i]);
    printf("\n");

    blake3_hasher_init_keyed(&h, (const uint8_t *)key);
    blake3_hasher_update(&h, buf, n);
    blake3_hasher_finalize(&h, out, 131);
    for (int i = 0; i < 131; i++) printf("%02x", out[i]);
    printf("\n");

    blake3_hasher_init_derive_key(&h, ctx);
    blake3_hasher_update(&h, buf, n);
    blake3_hasher_finalize(&h, out, 131);
    for (int i = 0; i < 131; i++) printf("%02x", out[i]);
    printf("\n");

    free(buf);
    return 0;
}
