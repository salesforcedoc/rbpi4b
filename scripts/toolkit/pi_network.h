/* SPDX-License-Identifier: MIT */
/* Pi Now Playing destination, in the little-endian ARM sockaddr byte order. */
#ifndef RBPI4B_TOOLKIT_PI_NETWORK_H
#define RBPI4B_TOOLKIT_PI_NETWORK_H
static unsigned int rx3_pi_ipv4(const char *text)
{
    unsigned int address = 0;
    if (!text) return 0x0100007fu;
    for (unsigned int part = 0; part < 4; part++) {
        unsigned int value = 0, digits = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10u + (unsigned int)(*text++ - '0');
            if (++digits > 3 || value > 255) return 0x0100007fu;
        }
        if (!digits || (part < 3 ? *text++ != '.' : *text != '\0')) return 0x0100007fu;
        address |= value << (part * 8u);
    }
    return address;
}
#endif
