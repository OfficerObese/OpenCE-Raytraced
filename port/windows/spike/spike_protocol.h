/*
SPIKE_PROTOCOL.H

The memory the 32-bit game-side process (spike_host.c) and the 64-bit
renderer process (spike_renderer.c) share: a header page, then a ring of
messages the host writes and the renderer reads. Fixed-width types only, so
that both builds lay it out alike.

Positions count bytes since the start, modulo 2^32; the ring's size is a
power of two, so a position's offset in the ring is its low bits. A message
never wraps: one that would is preceded by a pad message to the ring's end.
*/

#ifndef SPIKE_PROTOCOL_H
#define SPIKE_PROTOCOL_H

#include <stdint.h>

#define SPIKE_PROTOCOL_VERSION 1

#define SPIKE_HEADER_SIZE 4096u
#define SPIKE_RING_SIZE (64u << 20)
#define SPIKE_SECTION_SIZE (SPIKE_HEADER_SIZE + SPIKE_RING_SIZE)
#define SPIKE_MESSAGE_ALIGNMENT 16u
/* the most one message carries; larger payloads are sent in chunks */
#define SPIKE_CHUNK_SIZE (1u << 20)
/* the largest payload (as one texture of the game's) */
#define SPIKE_PAYLOAD_MAXIMUM (16u << 20)
/* frames the host may send before the renderer has presented them */
#define SPIKE_FRAMES_IN_FLIGHT 2

enum spike_opcode
{
	_spike_opcode_pad = 0,
	_spike_opcode_resize,
	_spike_opcode_frame,
	_spike_opcode_payload_begin,
	_spike_opcode_payload_chunk,
	_spike_opcode_payload_end,
	_spike_opcode_quit,
	NUMBER_OF_SPIKE_OPCODES
};

enum spike_renderer_state
{
	_spike_renderer_starting = 0,
	_spike_renderer_ready,
	_spike_renderer_failed,
};

struct spike_message_header
{
	uint32_t opcode;
	/* bytes after the header, before alignment */
	uint32_t size;
	uint32_t reserved[2];
};

struct spike_resize
{
	uint32_t width, height;
};

struct spike_frame
{
	uint32_t frame;
	float red, green, blue;
};

struct spike_payload_begin
{
	uint32_t total_size;
};

struct spike_shared
{
	uint32_t version;
	uint32_t ring_size;
	/* written by the host */
	volatile uint32_t write_position;
	/* written by the renderer */
	volatile uint32_t read_position;
	volatile uint32_t renderer_state;
	volatile uint32_t frames_presented;
	volatile uint32_t payloads_completed;
	/* the last payload: from its first byte read to its GPU copy done, and
	the GPU copy alone */
	volatile uint32_t payload_microseconds;
	volatile uint32_t gpu_copy_microseconds;
	char adapter_name[128];
	char error[256];
};

#define SPIKE_ALIGN(size) (((size) + SPIKE_MESSAGE_ALIGNMENT - 1) & ~(SPIKE_MESSAGE_ALIGNMENT - 1))

_Static_assert(sizeof(struct spike_message_header) == SPIKE_MESSAGE_ALIGNMENT, "message header size");
_Static_assert(sizeof(struct spike_shared) <= SPIKE_HEADER_SIZE, "shared header size");
_Static_assert((SPIKE_RING_SIZE & (SPIKE_RING_SIZE - 1)) == 0, "ring size is a power of two");
_Static_assert(SPIKE_CHUNK_SIZE + 2 * sizeof(struct spike_message_header) <= SPIKE_RING_SIZE, "chunk fits the ring");

#endif
