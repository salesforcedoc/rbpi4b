/*
 * test_vnc_des.c -- DES against the FIPS vectors, and the VNC key transform.
 *
 * A DES with a typo in a table still runs and still returns sixteen plausible bytes,
 * so "it produced output" proves nothing. These are the published vectors, and the
 * first failing index tells you which table to look at.
 *
 * make -C scripts/device/vncserve test
 */
#include <stdio.h>
#include <string.h>

#include "vnc_des.h"

static int fails, checks;

static void eq_mem(const uint8_t *got, const uint8_t *want, int n, const char *what)
{
    int i;
    checks++;
    if (memcmp(got, want, (size_t)n) != 0) {
        fails++;
        printf("  FAIL %s\n    got ", what);
        for (i = 0; i < n; i++) printf("%02X", got[i]);
        printf("\n    want");
        for (i = 0; i < n; i++) printf(" %02X", want[i]);
        printf("\n");
    }
}

static void block(const uint8_t key[8], const uint8_t pt[8], const uint8_t want[8], const char *what)
{
    uint8_t out[8];
    vnc_des_encrypt_block(key, pt, out);
    eq_mem(out, want, 8, what);
}

static void test_fips_vectors(void)
{
    /* FIPS 81 / the standard textbook pair. */
    {
        static const uint8_t key[8] = { 0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF };
        static const uint8_t pt[8]  = { 0x4E,0x6F,0x77,0x20,0x69,0x73,0x20,0x74 }; /* "Now is t" */
        static const uint8_t ct[8]  = { 0x3F,0xA4,0x0E,0x8A,0x98,0x4D,0x48,0x15 };
        block(key, pt, ct, "FIPS: 0123456789ABCDEF / \"Now is t\"");
    }
    {
        static const uint8_t key[8] = { 0x13,0x34,0x57,0x79,0x9B,0xBC,0xDF,0xF1 };
        static const uint8_t pt[8]  = { 0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF };
        static const uint8_t ct[8]  = { 0x85,0xE8,0x13,0x54,0x0F,0x0A,0xB4,0x05 };
        block(key, pt, ct, "FIPS: 133457799BBCDFF1 / 0123456789ABCDEF");
    }
    /* All-zero key and plaintext: the classic weak-key case, and a good detector for
     * a key schedule that forgets the rotations. */
    {
        static const uint8_t key[8] = { 0,0,0,0,0,0,0,0 };
        static const uint8_t pt[8]  = { 0,0,0,0,0,0,0,0 };
        static const uint8_t ct[8]  = { 0x8C,0xA6,0x4D,0xE9,0xC1,0xB1,0x23,0xA7 };
        block(key, pt, ct, "FIPS: zero key, zero block");
    }
    /* All-ones. */
    {
        static const uint8_t key[8] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
        static const uint8_t pt[8]  = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
        static const uint8_t ct[8]  = { 0x73,0x59,0xB2,0x16,0x3E,0x4E,0xDC,0x58 };
        block(key, pt, ct, "FIPS: 0xFF x8 / 0xFF x8");
    }
}

static void test_vnc_key_transform(void)
{
    uint8_t key[8];
    /* The definition, spelled out: every byte's bits are reversed. This is the whole
     * of what separates VNC auth from plain DES-ECB, and a server that gets it wrong
     * rejects every password with no symptom but "authentication failed". */
    vnc_des_key_from_password("password", key);
    {
        static const uint8_t want[8] = { 0x0E,0x86,0xCE,0xCE,0xEE,0xF6,0x4E,0x26 };
        eq_mem(key, want, 8, "\"password\" -> reversed-bit key");
    }

    /* Padding and truncation: eight bytes, NUL-filled, from the left. */
    vnc_des_key_from_password("ab", key);
    {
        static const uint8_t want[8] = { 0x86,0x46,0,0,0,0,0,0 };
        eq_mem(key, want, 8, "\"ab\" is NUL padded to eight");
    }
    vnc_des_key_from_password("", key);
    {
        static const uint8_t want[8] = { 0,0,0,0,0,0,0,0 };
        eq_mem(key, want, 8, "empty password is all zeroes");
    }
    vnc_des_key_from_password("123456789", key);
    {
        static const uint8_t want[8] = { 0x8C,0x4C,0xCC,0x2C,0xAC,0x6C,0xEC,0x1C };
        eq_mem(key, want, 8, "ninth byte is discarded");
    }
}

static void test_auth_response_shape(void)
{
    uint8_t chal[16], resp[16], again[16];
    int i;

    for (i = 0; i < 16; i++) chal[i] = (uint8_t)i;

    vnc_auth_response("rbp", chal, resp);
    vnc_auth_response("rbp", chal, again);
    eq_mem(again, resp, 16, "the same challenge and password give the same bytes");

    /* The two 8-byte halves must be independent DES blocks, not one 16-byte thing:
     * a challenge with identical halves under the same key gives identical halves. */
    {
        uint8_t chal2[16], resp2[16];
        for (i = 0; i < 8; i++) { chal2[i] = 0x55; chal2[8 + i] = 0x55; }
        vnc_auth_response("rbp", chal2, resp2);
        eq_mem(resp2 + 8, resp2, 8, "identical challenge halves give identical halves");
    }

    /* And a different password must give different bytes -- the cheapest check that
     * the password is actually reaching the cipher. */
    {
        uint8_t other[16];
        vnc_auth_response("rbq", chal, other);
        checks++;
        if (memcmp(other, resp, 16) == 0) {
            fails++;
            printf("  FAIL two different passwords gave the same response\n");
        }
    }
}

int main(void)
{
    printf("test_vnc_des\n");
    test_fips_vectors();
    test_vnc_key_transform();
    test_auth_response_shape();
    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
