/* SPDX-License-Identifier: MIT */
/* Pi shim keys bypass the RX3's physical pad dispatcher. The mapper also
 * consumes physical SHIFT and mode buttons, so offer one optional callback.
 * Inserted after the upstream broker's state declarations. */
static int rx3_pi_pad_control_mode(unsigned int key, unsigned int operation, unsigned int channel,
                                    unsigned int mode)
{
    if (key >= 0x4113u && key <= 0x4116u && operation == 0u) {
        void (*action)(void) = __atomic_load_n(&mode_key_action, __ATOMIC_SEQ_CST);
        if (action) action();
    }
    if (key < 0x4117u || key > 0x411eu || channel < 1u || channel > 2u) return 0;
    const struct rx3_pad_event event = {key, operation, channel, channel - 1u, mode};
    for (unsigned int i = 0; i < pad_client_count; i++) {
        rx3_pad_handler handler = __atomic_load_n(&pad_clients[i].handler, __ATOMIC_SEQ_CST);
        if (handler && handler(&event)) return 1;
    }
    return 0;
}

static int rx3_pi_pad_control(unsigned int key, unsigned int operation, unsigned int channel)
{
    return rx3_pi_pad_control_mode(key, operation, channel, 0u);
}

/* Mode-aware bridge: Stems owns only primary SLIP LOOP (mode2), while
 * Samples can take any bank. Returning zero preserves native pad behavior. */
__attribute__((visibility("default")))
int rx3_pi_pad(unsigned int key, unsigned int operation, unsigned int channel, unsigned int mode)
{
    return rx3_pi_pad_control_mode(key, operation, channel, mode);
}

__attribute__((visibility("default")))
int rx3_pi_control(unsigned int key, unsigned int operation, unsigned int channel)
{
    if (key == RX3_KEY_SHIFT && channel < KEY_CHANNELS) {
        if (operation == 0u) shift_held[channel] = 1u;
        else if ((operation & ~1u) == 2u) shift_held[channel] = 0u;
        const struct rx3_key_event event = {key, operation, channel};
        for (unsigned int i = 0; i < key_client_count; i++) {
            rx3_key_handler handler = __atomic_load_n(&key_clients[i].handler, __ATOMIC_SEQ_CST);
            if (handler) (void)handler(&event);
        }
    }
    return rx3_pi_pad_control(key, operation, channel);
}
