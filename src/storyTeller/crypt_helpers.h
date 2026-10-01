#ifndef CRYPT_HELPERS_
#define CRYPT_HELPERS_

#include <stdint.h>
#include <string.h>

static uint64_t string_hash(const char *path) {
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*path != '\0') {
        h ^= (uint8_t) *path;
        h *= 0x100000001b3ULL;
        path++;
    }
    return h;
}

typedef struct {
    uint32_t state[4];
    uint64_t bitLength;
    uint8_t buffer[64];
    size_t bufferLen;
} Md5Context;

static const uint32_t md5K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};

static const uint32_t md5S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static uint32_t md5_rotl(uint32_t x, uint32_t s) {
    return (x << s) | (x >> (32 - s));
}

static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t w[16];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t) block[i * 4]
             | ((uint32_t) block[i * 4 + 1] << 8)
             | ((uint32_t) block[i * 4 + 2] << 16)
             | ((uint32_t) block[i * 4 + 3] << 24);
    }
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        uint32_t dTemp = d;
        d = c;
        c = b;
        b = b + md5_rotl(f + a + w[g] + md5K[i], md5S[i]);
        a = dTemp;
    }
    state[0] = state[0] + a;
    state[1] = state[1] + b;
    state[2] = state[2] + c;
    state[3] = state[3] + d;
}

static void md5_init(Md5Context *ctx) {
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xefcdab89u;
    ctx->state[2] = 0x98badcfeu;
    ctx->state[3] = 0x10325476u;
    ctx->bitLength = 0;
    ctx->bufferLen = 0;
}

static void md5_update(Md5Context *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *) data;
    ctx->bitLength += (uint64_t) len * 8u;
    if (ctx->bufferLen > 0) {
        size_t need = 64 - ctx->bufferLen;
        if (need > len) {
            need = len;
        }
        memcpy(ctx->buffer + ctx->bufferLen, p, need);
        ctx->bufferLen += need;
        p += need;
        len -= need;
        if (ctx->bufferLen == 64) {
            md5_transform(ctx->state, ctx->buffer);
            ctx->bufferLen = 0;
        }
    }
    while (len >= 64) {
        md5_transform(ctx->state, p);
        p += 64;
        len -= 64;
    }
    if (len > 0) {
        memcpy(ctx->buffer, p, len);
        ctx->bufferLen = len;
    }
}

static void md5_final(Md5Context *ctx, uint8_t digest[16]) {
    static const uint8_t zero[64] = {0};
    uint8_t bitLen[8];
    for (int i = 0; i < 8; i++) {
        bitLen[i] = (uint8_t) ((ctx->bitLength >> (8 * i)) & 0xFFu);
    }
    uint8_t padByte = 0x80u;
    md5_update(ctx, &padByte, 1);
    while (ctx->bufferLen > 56) {
        md5_update(ctx, zero, 64 - ctx->bufferLen);
    }
    md5_update(ctx, zero, 56 - ctx->bufferLen);
    md5_update(ctx, bitLen, 8);
    for (int i = 0; i < 4; i++) {
        digest[i * 4 + 0] = (uint8_t) (ctx->state[i] & 0xFFu);
        digest[i * 4 + 1] = (uint8_t) ((ctx->state[i] >> 8) & 0xFFu);
        digest[i * 4 + 2] = (uint8_t) ((ctx->state[i] >> 16) & 0xFFu);
        digest[i * 4 + 3] = (uint8_t) ((ctx->state[i] >> 24) & 0xFFu);
    }
}

static void md5_string(const char *str, char *out) {
    Md5Context ctx;
    md5_init(&ctx);
    md5_update(&ctx, str, strlen(str));
    uint8_t digest[16];
    md5_final(&ctx, digest);
    static const char hexDigits[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2 + 0] = hexDigits[(digest[i] >> 4) & 0x0Fu];
        out[i * 2 + 1] = hexDigits[digest[i] & 0x0Fu];
    }
    out[32] = '\0';
}

#endif // CRYPT_HELPERS_
