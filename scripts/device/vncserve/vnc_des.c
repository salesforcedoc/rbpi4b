/*
 * vnc_des.c -- DES (encrypt only, ECB, one block) and the VNC key transform.
 * See vnc_des.h for why this exists and why it is encrypt-only.
 *
 * Written from the FIPS 46-3 tables rather than adapted from anyone's code, so there
 * is one place to look when a vector in test_vnc_des.c fails. Every table below is
 * 1-based bit indices into the 64-bit block, which is the convention the standard
 * uses and the one that makes a transcription error visible.
 */
#include "vnc_des.h"

#include <string.h>

static const uint8_t IP[64] = {
    58, 50, 42, 34, 26, 18, 10, 2,
    60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6,
    64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17,  9, 1,
    59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5,
    63, 55, 47, 39, 31, 23, 15, 7
};

static const uint8_t FP[64] = {
    40, 8, 48, 16, 56, 24, 64, 32,
    39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30,
    37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28,
    35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26,
    33, 1, 41,  9, 49, 17, 57, 25
};

static const uint8_t E[48] = {
    32,  1,  2,  3,  4,  5,
     4,  5,  6,  7,  8,  9,
     8,  9, 10, 11, 12, 13,
    12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21,
    20, 21, 22, 23, 24, 25,
    24, 25, 26, 27, 28, 29,
    28, 29, 30, 31, 32,  1
};

static const uint8_t P[32] = {
    16,  7, 20, 21, 29, 12, 28, 17,
     1, 15, 23, 26,  5, 18, 31, 10,
     2,  8, 24, 14, 32, 27,  3,  9,
    19, 13, 30,  6, 22, 11,  4, 25
};

static const uint8_t PC1[56] = {
    57, 49, 41, 33, 25, 17,  9,
     1, 58, 50, 42, 34, 26, 18,
    10,  2, 59, 51, 43, 35, 27,
    19, 11,  3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15,
     7, 62, 54, 46, 38, 30, 22,
    14,  6, 61, 53, 45, 37, 29,
    21, 13,  5, 28, 20, 12,  4
};

static const uint8_t PC2[48] = {
    14, 17, 11, 24,  1,  5,
     3, 28, 15,  6, 21, 10,
    23, 19, 12,  4, 26,  8,
    16,  7, 27, 20, 13,  2,
    41, 52, 31, 37, 47, 55,
    30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53,
    46, 42, 50, 36, 29, 32
};

static const uint8_t SHIFT[16] = {
    1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1
};

static const uint8_t S[8][4][16] = {
    {   { 14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7 },
        { 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8 },
        { 4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0 },
        { 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13 } },
    {   { 15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10 },
        { 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5 },
        { 0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15 },
        { 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9 } },
    {   { 10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8 },
        { 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1 },
        { 13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7 },
        { 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12 } },
    {   { 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15 },
        { 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9 },
        { 10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4 },
        { 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14 } },
    {   { 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9 },
        { 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6 },
        { 4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14 },
        { 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3 } },
    {   { 12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11 },
        { 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8 },
        { 9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6 },
        { 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13 } },
    {   { 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1 },
        { 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6 },
        { 1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2 },
        { 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12 } },
    {   { 13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7 },
        { 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2 },
        { 7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8 },
        { 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11 } }
};

/* THE TABLES INDEX FROM BIT 1 OF AN n-BIT FIELD, NOT OF A 64-BIT WORD, and that
 * distinction is the whole of this function's contract. E and P index a 32-bit
 * value, PC2 a 56-bit one, IP and FP a 64-bit one. Indexing everything from the top
 * of a uint64 happens to be correct for IP and FP and silently wrong for the rest --
 * which is exactly what the first run of test_vnc_des.c caught: it produced a
 * correct-looking 8D 9F BB 10 ... for a vector whose answer is 3F A4 0E 8A ..., and
 * the identity function for an all-zero key and block. */
static uint64_t permute(uint64_t in, int in_bits, const uint8_t *table, int n_out)
{
    uint64_t out = 0;
    int i;
    for (i = 0; i < n_out; i++)
        out = (out << 1) | ((in >> (in_bits - table[i])) & 1ULL);
    return out;
}

static uint64_t rotl28(uint64_t x, int n)
{
    const uint64_t mask = 0x0FFFFFFFULL;
    x &= mask;
    return ((x << n) | (x >> (28 - n))) & mask;
}

void vnc_des_encrypt_block(const uint8_t key[8], const uint8_t in[8], uint8_t out[8])
{
    uint64_t k = 0, b = 0, c, d;
    uint64_t subkeys[16];
    int i, r;

    for (i = 0; i < 8; i++) {
        k = (k << 8) | key[i];
        b = (b << 8) | in[i];
    }

    /* Key schedule. */
    {
        uint64_t pc1 = permute(k, 64, PC1, 56);
        c = (pc1 >> 28) & 0x0FFFFFFFULL;
        d = pc1 & 0x0FFFFFFFULL;
        for (i = 0; i < 16; i++) {
            c = rotl28(c, SHIFT[i]);
            d = rotl28(d, SHIFT[i]);
            subkeys[i] = permute((c << 28) | d, 56, PC2, 48);
        }
    }

    /* 16 rounds. */
    b = permute(b, 64, IP, 64);
    for (r = 0; r < 16; r++) {
        uint64_t l = (b >> 32) & 0xFFFFFFFFULL;
        uint64_t rr = b & 0xFFFFFFFFULL;
        uint64_t f, e = permute(rr, 32, E, 48) ^ subkeys[r];
        uint64_t s = 0;
        int box;

        for (box = 0; box < 8; box++) {
            int six = (int)((e >> (42 - 6 * box)) & 0x3F);
            int row = ((six & 0x20) >> 4) | (six & 1);
            int col = (six >> 1) & 0x0F;
            s = (s << 4) | (uint64_t)S[box][row][col];
        }
        f = permute(s, 32, P, 32);
        b = ((rr << 32) | ((l ^ f) & 0xFFFFFFFFULL));
    }
    /* The final round swaps the halves, so undo that before the inverse permutation. */
    b = ((b & 0xFFFFFFFFULL) << 32) | ((b >> 32) & 0xFFFFFFFFULL);
    b = permute(b, 64, FP, 64);

    for (i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(b & 0xFF);
        b >>= 8;
    }
}

static uint8_t reverse_bits(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

void vnc_des_key_from_password(const char *password, uint8_t key_out[8])
{
    char padded[8];
    int i;

    /* Short passwords are NUL-padded, long ones truncated at eight -- the historical
     * behaviour, and the reason VNC passwords are famously weak: only the first eight
     * bytes are ever used. */
    memset(padded, 0, sizeof padded);
    for (i = 0; i < 8 && password[i]; i++)
        padded[i] = password[i];
    for (i = 0; i < 8; i++)
        key_out[i] = reverse_bits((uint8_t)padded[i]);
}

void vnc_auth_response(const char *password, const uint8_t challenge[16], uint8_t response[16])
{
    uint8_t key[8];
    vnc_des_key_from_password(password, key);
    vnc_des_encrypt_block(key, challenge, response);
    vnc_des_encrypt_block(key, challenge + 8, response + 8);
}
