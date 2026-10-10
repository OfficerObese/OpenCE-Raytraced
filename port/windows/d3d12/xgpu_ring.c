/*
XGPU_RING.C

The game's side of the ring (xgpu_ring.h). Messages are written in place in
the shared section; the write position is published once a message is
whole, and the renderer is woken only when it has said it sleeps
(xgpu_header renderer_waiting).
*/

#include "platform.h"
#include "port_config.h"
#include "xgpu_ring.h"
#include "xgpu_host.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void renderer_lost(void)
{
	struct xgpu_header *header = xgpu_host_header();
	char message[640];

	snprintf(message, sizeof(message), "The renderer (halo_renderer.exe) has stopped: %s",
		header && header->error[0] ? header->error : "it exited.");
	platform_log("%s", message);
	/* (a run nobody watches: the log only) */
	if (!config_boolean("debug.hidden_window") && config_real("debug.exit_after") <= 0.0 &&
		config_integer("debug.scene_test") <= 0)
	{
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Halo", message, NULL);
	}
	exit(EXIT_FAILURE);
}

void *xgpu_ring_reserve(unsigned long opcode, unsigned long size)
{
	struct xgpu_header *header = xgpu_host_header();
	unsigned char *ring = xgpu_host_ring();
	unsigned long total = sizeof(struct xgpu_message_header) + XGPU_ALIGN(size);
	unsigned long position, offset, pad;
	struct xgpu_message_header *message;

	if (size > XGPU_MESSAGE_MAXIMUM + 4096)
	{
		platform_log("a renderer message of %lu bytes", size);
		abort();
	}
	position = header->write_position;
	offset = position & (XGPU_RING_SIZE - 1);
	pad = offset + total > XGPU_RING_SIZE ? XGPU_RING_SIZE - offset : 0;
	while (XGPU_RING_SIZE - (unsigned long)(position - header->read_position) < pad + total)
	{
		/* (whatever is waiting in the ring, the renderer must see) */
		xgpu_ring_flush();
		if (!xgpu_host_wait_space(100) || header->renderer_state == _xgpu_renderer_failed)
			renderer_lost();
	}
	if (pad)
	{
		message = (struct xgpu_message_header *)(ring + offset);
		message->opcode = _xgpu_op_pad;
		message->size = pad - sizeof(*message);
		position += pad;
		offset = 0;
	}
	message = (struct xgpu_message_header *)(ring + offset);
	message->opcode = opcode;
	message->size = size;
	message->reserved[0] = position + total;
	return message + 1;
}

void xgpu_ring_send(void *payload)
{
	struct xgpu_header *header = xgpu_host_header();
	struct xgpu_message_header *message = (struct xgpu_message_header *)payload - 1;

	/* (the message's bytes before the position that makes it visible) */
	__sync_synchronize();
	InterlockedExchange((LONG *)&header->write_position, (LONG)message->reserved[0]);
	if (header->renderer_waiting)
		xgpu_ring_flush();
}

void xgpu_ring_send_data(unsigned long opcode, const void *head, unsigned long head_size, const void *data,
	unsigned long data_size)
{
	unsigned char *payload = xgpu_ring_reserve(opcode, head_size + data_size);

	memcpy(payload, head, head_size);
	if (data_size)
		memcpy(payload + head_size, data, data_size);
	xgpu_ring_send(payload);
}

void xgpu_ring_flush(void)
{
	struct xgpu_header *header = xgpu_host_header();

	if (header)
	{
		header->renderer_waiting = 0;
		xgpu_host_signal_data();
	}
}
