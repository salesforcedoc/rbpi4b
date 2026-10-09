/*
 * vnc_des.h -- DES, and the two things VNC does to it.
 *
 * WHY THERE IS A DES IN THIS REPO AT ALL. macOS's Screen Sharing will not connect
 * to a server that offers security type None: offered a list of [None] it reads it
 * and closes the socket without a byte back (measured 2026-10-09, and the reason
 * this file exists). The type it will take is 2 -- "VNC authentication" -- which is
 * a 16-byte challenge, DES-encrypted by the client under a key derived from the
 * password, sent back for the server to check.
 *
 * glibc has no DES and the unit has no libdes, so it is implemented here. It is
 * encrypt-only, ECB, one block: that is the entire surface VNC auth needs, and
 * anything more would be code nobody calls. It is pinned against the FIPS test
 * vectors in test_vnc_des.c rather than trusted, because a DES with a table typo in
 * it still runs and still returns sixteen plausible bytes.
 */
#ifndef RBPI4B_VNC_DES_H
#define RBPI4B_VNC_DES_H

#include <stdint.h>

/* Encrypt one 8-byte block in place-style: `out` may not alias `key`.
 * key is 8 bytes, and the eight DES parity bits in it are ignored, as DES requires. */
void vnc_des_encrypt_block(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]);

/* The VNC password transform: DES takes a key whose top bit of each byte is a parity
 * bit, but VNC passwords use all eight bits, so every byte's bits are reversed before
 * the password is used as a key. That reversal is the whole of the difference between
 * VNC auth and plain DES-ECB, and getting it wrong yields a server that rejects every
 * password -- including the right one -- with no other symptom. */
void vnc_des_key_from_password(const char *password, uint8_t key_out[8]);

/* The full challenge/response: DES-ECB the 16-byte challenge as two 8-byte blocks
 * under the password's transformed key. This is what the server sends and what it
 * compares the client's 16 bytes against. */
void vnc_auth_response(const char *password, const uint8_t challenge[16], uint8_t response[16]);

#endif /* RBPI4B_VNC_DES_H */
