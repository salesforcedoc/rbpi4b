/*
 * vnc_net.h -- the socket and log plumbing every part of vncserve shares.
 *
 * Deliberately dull: a timestamped log line, a listening socket, a write that
 * finishes, and a read that gives up. Nothing here knows what RFB is.
 *
 * The one opinion it does hold is that a timeout is not an error. A VNC client sits
 * at a password prompt for as long as it likes, and the probe's whole job is to
 * listen to a client that may say nothing at all -- so a read that runs out of
 * patience returns 0, and only a genuine socket failure or a closed peer returns
 * -1. Callers that confuse the two turn ordinary silence into a hangup.
 */
#ifndef RBPI4B_VNC_NET_H
#define RBPI4B_VNC_NET_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Where the log goes. NULL -- the default -- means stdout. Set once at startup
 * with vnc_log_to(), before anything else runs. */
FILE *vnc_logfp(void);
void vnc_log_to(FILE *f);

/* One timestamped line, flushed. Flushed because the interesting failure is rbp
 * dying, or the client vanishing, and a log still sitting in stdio's buffer when
 * that happens is a log with nothing in it. */
void vlog(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* A listening TCP socket on `bindaddr` (NULL for every interface), or -1 having
 * said why. */
int vnc_net_listen(const char *bindaddr, int port);

/* Send all of `n` bytes, retrying on EINTR. 0 on success, -1 if the peer went away.
 * SIGPIPE is ignored process-wide, so a dead peer surfaces as EPIPE here rather
 * than as a signal that kills the server mid-frame. */
int vnc_net_write_all(int fd, const void *buf, size_t n);

/* Read exactly `n` bytes, giving up after `ms` of silence. 1 if all n arrived,
 * 0 on timeout or EOF. See the header comment for why those are not the same as
 * an error. */
int vnc_net_read_to(int fd, void *buf, size_t n, int ms);

/* Big-endian 32, the only integer width RFB ever puts on the wire. */
uint32_t vnc_net_be32(const uint8_t *p);

#endif /* RBPI4B_VNC_NET_H */
