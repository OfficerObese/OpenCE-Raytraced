/*
XGPU_RING.H

The game's side of the ring of messages to the D3D12 renderer
(xgpu_protocol.h): a message is reserved, filled in place and sent.
*/

#ifndef __XGPU_RING_H
#define __XGPU_RING_H

#include "xgpu_protocol.h"

/* room for a message of opcode with size bytes after its header (at most
XGPU_MESSAGE_MAXIMUM plus a struct's), waiting for the renderer to make room
if it must; the message goes with xgpu_ring_send. A renderer that has ended
ends the game, with its reason. */
void *xgpu_ring_reserve(unsigned long opcode, unsigned long size);
void xgpu_ring_send(void *payload);
/* a message of a struct and the bytes after it */
void xgpu_ring_send_data(unsigned long opcode, const void *head, unsigned long head_size, const void *data,
	unsigned long data_size);
/* wakes the renderer for what has been sent (each frame's end) */
void xgpu_ring_flush(void);
/* the renderer has ended or stopped answering: the game ends, with its
reason in the log and a box (reason NULL: the renderer's own) */
void xgpu_ring_renderer_stopped(const char *reason) __attribute__((noreturn));

#endif
