/* SPDX-License-Identifier: MIT */
/* Pi integration code: use exact argv entries, not comm or substring matches.
 * The explicit RX3 loader remains /proc/self/comm when it starts the player.
 * No system headers: this is included by both the ARM core and host tests. */
#ifndef RBPI4B_TOOLKIT_PI_PROCESS_H
#define RBPI4B_TOOLKIT_PI_PROCESS_H

static int rx3_pi_arg_equal(const char *args, unsigned int length,
                            unsigned int *position, const char *expected)
{
    unsigned int start = *position, size = 0;
    while (*position < length && args[*position]) (*position)++;
    if (*position == length) return 0;
    while (expected[size]) {
        if (start + size >= *position || args[start + size] != expected[size]) return 0;
        size++;
    }
    if (start + size != *position) return 0;
    (*position)++;
    return 1;
}

static int rx3_pi_player_cmdline(const char *args, unsigned int length,
                                const char *player, const char *loader)
{
    if (!args || !length || args[length - 1] || !player || player[0] != '/') return 0;
    unsigned int position = 0;
    if (rx3_pi_arg_equal(args, length, &position, player)) return 1;
    if (!loader || loader[0] != '/') return 0;
    position = 0;
    return rx3_pi_arg_equal(args, length, &position, loader) &&
           rx3_pi_arg_equal(args, length, &position, player);
}

#endif
