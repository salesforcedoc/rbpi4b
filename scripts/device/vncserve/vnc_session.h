/*
 * vnc_session.h -- the RFB server: one client's lifetime, and the frame timer.
 *
 * WHAT IS HERE AND WHAT IS NOT. This is the shell: sockets, a poll() loop, a frame
 * clock, and the per-client state machine that walks a connection from the greeting
 * to a stream of FramebufferUpdates. The bytes themselves are all in vnc_rfb, and the
 * picture is vnc_capture's. This file decides *when* to say things; the other two
 * decide *what*.
 *
 * THE HANDSHAKE IS A STATE MACHINE AND NOT A BLOCKING SEQUENCE, for one concrete
 * reason: with VNC auth the client is sent a challenge and then, on the operator's
 * own desk, sits at a password dialog while a human types. A server that waited there
 * in a blocking read would be frozen for as long as the password takes -- and since
 * the same process will serve the control page, the page would be frozen with it. So
 * every wait is a state, a deadline, and a return to the loop.
 *
 * ONE FRAME PER TICK, ENCODED ONCE, SENT TO EVERYONE. The capture composites the DRM
 * planes and costs about 10 ms; doing it per client would make a second viewer cost
 * as much as the first, which for a program whose whole purpose is to be free on a
 * live player is the wrong shape.
 */
#ifndef RBPI4B_VNC_SESSION_H
#define RBPI4B_VNC_SESSION_H

/* The VNC password. Required: without it there is no authentication, and macOS's
 * Screen Sharing will not connect to a server that does not ask for one (see
 * vnc_des.h). An empty string is accepted and means an empty password. */
#define VNC_PASSWORD_MAX 64

struct vnc_session_opts {
    const char *bindaddr;        /* NULL = every local interface */
    int port;                    /* 5900 by convention, 5901 if 5900 is taken */
    const char *password;

    /* The dialect the SERVER announces. It is not the dialect that gets used: RFB
     * takes the lower of the two versions, and macOS's client answers 3.003 to our
     * 3.008, so the session runs at 3.3 whatever we say. Announcing 3.008 is the
     * honest default; 3 is what Apple's own server says, and is here because the
     * choice was once worth being able to change without a rebuild. */
    int announce_minor;          /* 3 or 8 */

    const char *name;            /* the desktop name the client shows */
    int fps;                     /* capture ceiling; nothing is sent when idle */

    /* The zlib level for Tight "basic compression". ZERO TURNS THE WHOLE OPTIMISATION
     * OFF and every client goes back to one Raw rectangle per update, which is what
     * this server did before 2026-10-09. It is a level and a switch in one because 0
     * is zlib's own "no compression" and a level this code must never pass through.
     *
     * Default 1. Measured on the unit against the live screen: level 1 compresses a
     * client's 32-bpp frame 32x in 45 ms; level 6 gets 64x for 96 ms and level 9 75x
     * for 320 ms. On a machine whose other job is playing audio, and where the whole
     * update is usually a few bands rather than a frame, the cheap end is the right
     * one -- see RB_VNC_ZLIB_LEVEL in rb.conf. */
    int zlib_level;

    /* WHICH ENCODER, AND HOW HARD IT WORKS. Two nodes can produce the Motion-JPEG this server
     * sends: the video encoder, which has no quality control, and the image encoder, which
     * does. NULL/0 keeps the node this unit has always used with its own defaults -- see
     * vnc_jpeg.h for the measured reason the defaults are the conservative ones. */
    const char *jpeg_dev;
    int jpeg_quality;

    /* THE SWITCH. The mode file is what the operator's page writes and what this loop
     * reads; passing the path in rather than letting the session hardcode it is what
     * lets a test run against a file in a scratch directory. NULL means "no switch",
     * and the session then runs raw for ever. */
    const char *mode_path;
    int default_mode;            /* VNC_MODE_RAW or VNC_MODE_HWJPEG before any file */

    /* The control page. A port of 0 turns it off entirely -- which also turns off any
     * possibility of the hardware encoder running for a preview. The picture-only view
     * is the page's own tapped state, not a port; there is no second one. */
    int http_port;               /* VNC_HTTP_PORT_DEFAULT, or 0 for none */

    /* THE INPUT, WHICH IS THE ONE OPTION HERE THAT CAN MOVE THE OPERATOR'S PLAYER.
     * A PointerEvent from a VNC client becomes a press on the panel, so a click in the
     * picture is a real press on the real glass -- see vnc_input.h, which is where the
     * whole of the argument for that lives. Three settings, and only the last is
     * interesting: `input_path` is the enable file the page writes (NULL for
     * VNC_INPUT_PATH), `input_dev` pins the panel node (NULL scans for it), and
     * `default_input` is what the switch starts at when no file exists yet. */
    const char *input_path;
    const char *input_dev;
    int default_input;           /* VNC_INPUT_OFF or VNC_INPUT_ON */

    /* WHETHER THE SCREEN IS SERVED AT ALL, which is the one switch here that decides
     * whether this program goes near the display. OFF -- the default, and what a boot
     * starts in -- means the process serves only the control page: no /dev/fb0, no
     * /dev/dri/card1, no RFB listener, no hardware encoder. See vnc_live.h for why
     * that matters (the cold-boot blank screen) and why the page has to stay up
     * anyway. NULL for `live_path` means VNC_LIVE_PATH. */
    const char *live_path;
    int default_live;            /* VNC_LIVE_OFF or VNC_LIVE_ON */
};

/* Run until a client-visible stop condition, or forever. Returns a process exit
 * status: 0 only for a clean stop. */
int vnc_session_run(const struct vnc_session_opts *o);

#endif /* RBPI4B_VNC_SESSION_H */
