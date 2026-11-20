/* SPDX-License-Identifier: MIT */
/* One DSYSMOSTServer client per process (libsys_dsi_base, opened at run time): the UDS tunnel to the DME, MOST messages to
 * any device, every received message to a hook, and the MOST registry. See dsi.c. */
#ifndef SL_DSI_H
#define SL_DSI_H

#include "sltypes.h"

enum { DSI_UP = 0, DSI_NO_LIBRARY = -1, DSI_NO_SERVER = -2, DSI_NO_TARGET = -3 };
enum { DSI_REPLY = 0, DSI_NEGATIVE = 1, DSI_TIMEOUT = -1, DSI_REFUSED = -2, DSI_CLOSED = -3 };

/* A received message record. Inbound records are kind 2; kind 2 with flags 4..9 or 0x13..0x15 is the transmission
   status of a message we sent, not a message. p is valid during the hook call only. */
struct dsi_msg {
    u32 t_us, kind, transport, flags, client, len;
    u16 tgt, src, fkt;
    u8 fb, inst, op;
    const u8 *p;
};
typedef void (*dsi_hook_fn)(const struct dsi_msg *m); /* runs on the DSI thread: keep it short */

void dsi_set_hook(dsi_hook_fn h);                      /* before dsi_start */
/* before dsi_start: also subscribe to every message notification, as the HMI's client and st1ng's do (their kSubs);
   without it the server only delivers what is addressed to us */
void dsi_set_listen_all(int on);
int dsi_start(const char *name, u32 wait_ms);          /* DSI_UP once the server registered the client */
const char *dsi_why(void);                             /* what the last failure was */
int dsi_tunnel_open(u32 wait_ms);                      /* register 06/F0, wait for informationConnect */
/* One UDS request to the DME through the tunnel; one in flight. DSI_REPLY with the positive reply in reply[0..*len),
   DSI_NEGATIVE with *nrc, DSI_REFUSED (the tunnel's 7F 31, *nrc), DSI_TIMEOUT, DSI_CLOSED. *rt_us: send to reply. */
int dsi_tunnel_request(const u8 *uds, u32 n, u8 *reply, u32 cap, u32 *len, u32 timeout_ms, u32 *rt_us, u8 *nrc);
/* A message to device dst (request 0x02 on the control channel). 0 when the library took it. */
int dsi_send(u16 dst, u8 fb, u8 inst, u16 fkt, u8 op, const u8 *p, u32 n);
u16 dsi_self(void);                                    /* our device address */
u16 dsi_host_of(u16 key);                              /* the registry's device for fblock<<8|inst, 0 if not listed */
int dsi_registry(u16 *dev, u16 *key, int max);         /* the registry pairs seen so far */
int dsi_up(void);                                      /* the client is registered and running */
int dsi_tunnel_up(void);
void dsi_stop(void);                                   /* unregister, stop the DSI loop, bounded (~3.5 s) */

#endif
