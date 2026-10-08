/*
 * rbp_transport.c -- the shared state itself. Nothing else belongs here.
 *
 * This object is linked into BOTH fbshim.so and knobshim.so and is built with
 * default visibility, so there is exactly one instance of `transport` in the
 * process and both libraries resolve these accessors to it. See rbp_transport.h
 * for why that is the arrangement and what went wrong the other way.
 *
 * The static data is static on purpose: the FUNCTIONS are the interface, and they
 * are what the dynamic linker shares. A caller in either shim that calls
 * rbp_transport_set() lands on the first definition in the search order, which is
 * the same one every other caller lands on, so the writes and the reads cannot
 * meet in two different arrays.
 */
#include "rbp_transport.h"

static int transport[2];    /* packed RBP_TRANSPORT_* bits, one entry per deck */
static int have_read;       /* 0 = nothing read yet; rbp_transport_get returns -1 */
static int wanted;          /* a drawer is open */

int rbp_transport_get(int deck)
{
    if (!have_read || deck < 0 || deck > 1)
        return -1;
    return transport[deck];
}

void rbp_transport_set(int deck, int bits)
{
    if (deck < 0 || deck > 1)
        return;
    transport[deck] = bits;
    have_read = 1;
}

void rbp_transport_clear(void)
{
    have_read = 0;
}

void rbp_transport_want(int on)
{
    wanted = on ? 1 : 0;
}

int rbp_transport_wanted(void)
{
    return wanted;
}
