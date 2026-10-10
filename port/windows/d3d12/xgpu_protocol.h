/*
XGPU_PROTOCOL.H

What the game (32-bit, port/windows/d3d12) and the D3D12 renderer process
(64-bit, port/windows/renderer) say to each other, through a section they
share: a header page, a results area the renderer writes, then a ring of
messages the game writes and the renderer reads, in the order the game's
Direct3D calls made them.

Fixed-width types only, checked with _Static_assert, so that both builds lay
it out alike. Positions count bytes since the start, modulo 2^32; the ring's
size is a power of two. A message never wraps: one that would is preceded by
a pad message to the ring's end. Every message is 16-byte aligned.

The renderer never reads the game's memory: every byte the GPU needs (vertex
and index data, texels, shader text) is copied into the ring when the game
makes the call that hands it over, as the OpenGL device copies it into GL
buffers then. Everything the renderer reads from the ring is checked before
it is used: the game's data come from map files, which are untrusted.

Resources are named by numbers the game gives: textures (as glGenTextures
numbers them: xgpu_gl_textures.c), render targets, vertex and pixel shaders,
each counted from 1, 0 being none; and the mirror's segments of the Xbox's
contiguous memory (d3d8_d3d12.c), numbered by their place in it.
*/

#ifndef __XGPU_PROTOCOL_H
#define __XGPU_PROTOCOL_H

#include <stdint.h>

#define XGPU_PROTOCOL_VERSION 1

#define XGPU_HEADER_SIZE 0x1000u
#define XGPU_RESULTS_SIZE 0x10000u
#define XGPU_RING_SIZE (64u << 20)
#define XGPU_SECTION_SIZE (XGPU_HEADER_SIZE + XGPU_RESULTS_SIZE + XGPU_RING_SIZE)
#define XGPU_MESSAGE_ALIGNMENT 16u
/* the most one message carries; more (a large texture) goes in pieces */
#define XGPU_MESSAGE_MAXIMUM (1u << 20)
#define XGPU_ALIGN(size) (((size) + XGPU_MESSAGE_ALIGNMENT - 1) & ~(XGPU_MESSAGE_ALIGNMENT - 1))

/* the frames the game may present before the renderer has shown them, as
the Xbox's two queued behind its display */
#define XGPU_FRAMES_IN_FLIGHT 2

/* limits on what the game names (each checked by the renderer) */
#define XGPU_TEXTURE_LIMIT 0x40000u
#define XGPU_TARGET_LIMIT 0x4000u
#define XGPU_SHADER_LIMIT 0x10000u
#define XGPU_MIRROR_SEGMENT_SIZE 0x400000u
#define XGPU_MIRROR_SEGMENT_LIMIT 128u
#define XGPU_VISIBILITY_SLOTS 4096u
#define XGPU_TEXTURE_STAGES 4
#define XGPU_ATTRIBUTE_COUNT 16
#define XGPU_STREAM_COUNT 16
#define XGPU_CONSTANT_COUNT 192
#define XGPU_MODEL_LIGHT_COUNT 12
#define XGPU_PATH_SIZE 260

/* ---------- the shared section's header */

enum xgpu_renderer_state
{
	_xgpu_renderer_starting = 0,
	_xgpu_renderer_ready,
	_xgpu_renderer_failed,
};

struct xgpu_header
{
	uint32_t version;
	uint32_t ring_size;
	/* written by the game */
	volatile uint32_t write_position;
	/* written by the renderer */
	volatile uint32_t read_position;
	volatile uint32_t renderer_state;
	/* frames the renderer has presented, and whose GPU work has finished
	(the visibility counts and screenshots of a frame are in place once its
	work has) */
	volatile uint32_t frames_presented;
	volatile uint32_t frames_completed;
	/* raised by the renderer when the GPU was lost and the device made
	again: every resource is gone, and the game sends them again */
	volatile uint32_t device_resets;
	/* screenshots written (to the paths the game named) */
	volatile uint32_t screenshots_written;
	/* what the GPU can do, set before renderer_state is ready */
	uint32_t maximum_samples;
	uint32_t maximum_texture_size;
	/* set by the renderer before it sleeps on the data event, and checked
	again after: the game signals the event only while it is set (a signal
	for every message would be a system call for every draw) */
	volatile uint32_t renderer_waiting;
	uint32_t reserved[4];
	char adapter_name[128];
	char error[512];
};

/* ---------- the results area, written by the renderer */

struct xgpu_results
{
	/* each visibility slot's latest sample count, and the frame it came from
	(0: none yet) */
	volatile uint32_t visibility_samples[XGPU_VISIBILITY_SLOTS];
	volatile uint32_t visibility_frame[XGPU_VISIBILITY_SLOTS];
};

/* ---------- messages */

enum xgpu_opcode
{
	_xgpu_op_pad = 0,
	/* struct xgpu_configure */
	_xgpu_op_configure,
	/* struct xgpu_resize: the window's drawable size changed */
	_xgpu_op_resize,
	/* textures (xgpu_gl_textures.c) */
	_xgpu_op_texture_define,
	_xgpu_op_texture_data,
	_xgpu_op_texture_mipmaps,
	_xgpu_op_texture_delete,
	/* render targets */
	_xgpu_op_target_define,
	_xgpu_op_target_delete,
	_xgpu_op_target_resolve,
	_xgpu_op_target_copy,
	/* bytes of the Xbox's contiguous memory, into the mirror */
	_xgpu_op_mirror_write,
	/* shaders: struct xgpu_shader and its HLSL */
	_xgpu_op_vertex_shader,
	_xgpu_op_pixel_shader,
	/* constants */
	_xgpu_op_vertex_constants,
	_xgpu_op_draw_uniforms,
	_xgpu_op_model_lights,
	/* struct xgpu_draw_state, which the draws after it use */
	_xgpu_op_draw_state,
	/* struct xgpu_draw and its inline data */
	_xgpu_op_draw,
	_xgpu_op_clear,
	_xgpu_op_visibility_begin,
	_xgpu_op_visibility_end,
	_xgpu_op_anti_alias,
	_xgpu_op_present,
	_xgpu_op_screenshot,
	_xgpu_op_quit,
	/* struct xgpu_anti_alias_prepare and SMAA.hlsl's text: the passes of
	_xgpu_op_anti_alias made before they are first drawn */
	_xgpu_op_anti_alias_prepare,
	/* reserved for the path tracer: the scene in world space */
	_xgpu_op_scene_begin_map = 64,
	_xgpu_op_scene_mesh,
	_xgpu_op_scene_instances,
	_xgpu_op_scene_camera,
	_xgpu_op_scene_lights,
	_xgpu_op_scene_materials,
	NUMBER_OF_XGPU_OPCODES
};

struct xgpu_message_header
{
	uint32_t opcode;
	/* bytes after the header, before alignment */
	uint32_t size;
	/* the game's own use, while it writes the message (its end) */
	uint32_t reserved[2];
};

struct xgpu_configure
{
	uint32_t vsync;
	uint32_t debug_layer;
	uint32_t reserved[2];
};

struct xgpu_resize
{
	uint32_t width, height;
};

enum
{
	_xgpu_texture_2d = 1,
	_xgpu_texture_3d,
	_xgpu_texture_cube,
};

/* the texels a texture holds; the GL formats the shared texture code asks
for (xgpu_gl_textures.c) */
enum
{
	/* 32-bit BGRA, as the decoded Xbox texels are (in memory B, G, R, A) */
	_xgpu_format_bgra8 = 1,
	/* 32-bit RGBA (the high-res HUD's and text's PNGs) */
	_xgpu_format_rgba8,
	_xgpu_format_bc1,
	_xgpu_format_bc2,
	_xgpu_format_bc3,
};

/* the texture's mip levels are made by the GPU from one of them
(_xgpu_op_texture_mipmaps): it is made renderable, which a 2D texture of
_xgpu_format_bgra8 or _xgpu_format_rgba8 can be */
#define XGPU_TEXTURE_RENDERABLE 1u

/* a channel's source in a texture's component mapping: as D3D12's
D3D12_SHADER_COMPONENT_MAPPING (0 red .. 3 alpha, 4 zero, 5 one) */
struct xgpu_texture_define
{
	uint32_t texture;
	uint32_t dimension;
	uint32_t format;
	uint32_t width, height, depth;
	uint32_t levels;
	uint8_t swizzle[4];
	uint32_t flags;
	uint32_t reserved[3];
};

/* rows [y, y + height) of a level (of a face, of a slice range for 3D);
for the BC formats, rows are rows of 4x4 blocks and x, y, width and height
are in texels, multiples of 4 except at the level's edge; row_pitch is the
data's bytes a row (of blocks) */
struct xgpu_texture_data
{
	uint32_t texture;
	uint32_t face;
	uint32_t level;
	uint32_t x, y, z;
	uint32_t width, height, depth;
	uint32_t row_pitch;
	uint32_t reserved[2];
};

struct xgpu_texture_reference
{
	uint32_t texture;
	/* _xgpu_op_texture_mipmaps: the level the others are made from */
	uint32_t base_level;
	uint32_t reserved[2];
};

struct xgpu_target_define
{
	uint32_t target;
	uint32_t width, height;
	/* a depth-stencil target, else a color one (BGRA8) */
	uint32_t depth;
	/* samples a pixel: 0 or 1 none */
	uint32_t samples;
	uint32_t reserved[3];
};

struct xgpu_target_reference
{
	uint32_t target;
	uint32_t reserved[3];
};

/* _xgpu_op_target_copy: a target's pixels into a level of a texture of
the same size (the game's textures drawn one level at a time) */
struct xgpu_target_copy
{
	uint32_t target;
	uint32_t texture;
	uint32_t level;
	uint32_t reserved;
};

struct xgpu_mirror_write
{
	uint32_t segment;
	uint32_t offset;
	uint32_t reserved[2];
};

/* how a vertex attribute is read: the input layout's format, and the
HLSL's input kind (xgpu_hlsl.h) the game wrote the shader for */
enum
{
	_xgpu_vertex_none = 0,
	_xgpu_vertex_float1, _xgpu_vertex_float2, _xgpu_vertex_float3, _xgpu_vertex_float4,
	/* D3DCOLOR: BGRA bytes */
	_xgpu_vertex_color,
	_xgpu_vertex_short2, _xgpu_vertex_short4,
	_xgpu_vertex_short1,
	_xgpu_vertex_normshort1, _xgpu_vertex_normshort2, _xgpu_vertex_normshort4,
	_xgpu_vertex_pbyte1, _xgpu_vertex_pbyte2, _xgpu_vertex_pbyte4,
	/* NORMPACKED3: a 32-bit word the shader unpacks */
	_xgpu_vertex_packed,
	/* three components, read as four with the fourth replaced by 1 in the
	shader */
	_xgpu_vertex_short3, _xgpu_vertex_normshort3, _xgpu_vertex_pbyte3,
	NUMBER_OF_XGPU_VERTEX_FORMATS
};

struct xgpu_shader
{
	uint32_t shader;
	/* vertex shaders: each attribute's input kind (xgpu_hlsl.h) */
	uint8_t input_kinds[XGPU_ATTRIBUTE_COUNT];
	uint32_t reserved[3];
	/* followed by the HLSL, without its terminating zero */
};

struct xgpu_vertex_constants
{
	uint32_t first;
	uint32_t count;
	uint32_t reserved[2];
	/* followed by count registers of four floats */
};

/* a texture stage's sampling, in the Xbox's terms (D3DTEXF_*, D3DTADDRESS_*) */
struct xgpu_sampler_state
{
	uint8_t min_filter, mag_filter, mip_filter;
	uint8_t address_u, address_v, address_w;
	uint8_t max_anisotropy;
	uint8_t reserved;
	float lod_bias;
	float min_lod;
	/* D3DCOLOR */
	uint32_t border_color;
};

struct xgpu_vertex_element
{
	uint8_t reg;
	uint8_t stream;
	uint8_t format;
	uint8_t reserved;
	uint16_t offset;
	uint16_t reserved2;
};

enum
{
	_xgpu_primitive_points = 1,
	_xgpu_primitive_lines,
	_xgpu_primitive_line_strip,
	_xgpu_primitive_triangles,
	_xgpu_primitive_triangle_strip,
};

/* a texture binding: 0 none, a texture number, or a render target's with
this bit */
#define XGPU_BINDING_TARGET 0x80000000u

/* everything about a draw that the draws after it keep until it changes.
The comparison and blend functions, stencil operations and blend factors are
the Xbox's (which are OpenGL's numbers, with 0 for zero), translated by the
renderer. */
struct xgpu_draw_state
{
	uint32_t vertex_shader;
	uint32_t pixel_shader;
	uint32_t color_target;
	uint32_t depth_target;
	uint32_t textures[XGPU_TEXTURE_STAGES];
	struct xgpu_sampler_state samplers[XGPU_TEXTURE_STAGES];

	/* in the targets' pixels */
	float viewport[4];
	float depth_range[2];
	int32_t scissor[4];

	uint8_t depth_enable, depth_write, stencil_enable, blend_enable;
	uint32_t depth_function;
	uint32_t stencil_function, stencil_reference, stencil_read_mask, stencil_write_mask;
	uint32_t stencil_fail, stencil_depth_fail, stencil_pass;
	uint32_t blend_source, blend_destination, blend_operation;
	/* D3DCOLOR */
	uint32_t blend_color;
	/* bit 0 red .. 3 alpha */
	uint8_t color_write_mask;
	/* 0 none, 1 the front faces, 2 the back */
	uint8_t cull;
	/* the front faces wind counter-clockwise on the screen */
	uint8_t front_counter_clockwise;
	/* 0 solid, 1 wireframe, 2 points */
	uint8_t fill;
	uint8_t depth_bias_enable;
	uint8_t primitive;
	/* the targets' samples a pixel (1: not multisampled) */
	uint8_t samples;
	uint8_t element_count;
	float depth_bias;
	float depth_bias_slope;

	struct xgpu_vertex_element elements[XGPU_ATTRIBUTE_COUNT];
	/* the value each attribute the elements do not give reads (packed
	attributes read their first word as an integer) */
	float attribute_values[XGPU_ATTRIBUTE_COUNT][4];
	uint32_t reserved[4];
};

enum
{
	_xgpu_source_none = 0,
	/* a mirror segment's bytes */
	_xgpu_source_mirror,
	/* bytes in the draw message, after struct xgpu_draw */
	_xgpu_source_inline,
};

struct xgpu_draw_source
{
	uint8_t kind;
	uint8_t reserved[3];
	uint32_t segment;
	uint32_t offset;
	uint32_t stride;
};

/* a draw with the current draw state: count vertices, from the first, or
count 16-bit indices, each added to base_vertex */
struct xgpu_draw
{
	uint32_t count;
	uint32_t first_vertex;
	int32_t base_vertex;
	uint32_t indexed;
	struct xgpu_draw_source indices;
	struct xgpu_draw_source streams[XGPU_STREAM_COUNT];
	/* the bytes after this struct that the inline sources point into */
	uint32_t inline_size;
	uint32_t reserved[3];
};

enum
{
	_xgpu_clear_color = 1,
	_xgpu_clear_depth = 2,
	_xgpu_clear_stencil = 4,
};

#define XGPU_CLEAR_RECTANGLES 16

struct xgpu_clear
{
	uint32_t color_target;
	uint32_t depth_target;
	uint32_t flags;
	/* the channels cleared (bit 0 red .. 3 alpha) */
	uint32_t color_mask;
	float color[4];
	float depth;
	uint32_t stencil;
	/* in the targets' pixels: left, top, right, bottom */
	uint32_t rectangle_count;
	uint32_t reserved;
	int32_t rectangles[XGPU_CLEAR_RECTANGLES][4];
};

struct xgpu_visibility
{
	uint32_t slot;
	uint32_t reserved[3];
};

enum
{
	_xgpu_anti_alias_fxaa = 1,
	_xgpu_anti_alias_smaa,
};

/* SMAA's lookup textures (already sent, as RGBA8 textures of their texels:
the area's in red and green, the search's in red) */
struct xgpu_anti_alias_prepare
{
	uint32_t mode;
	uint32_t area_texture;
	uint32_t search_texture;
	uint32_t reserved;
	/* followed by SMAA.hlsl, without its terminating zero */
};

struct xgpu_anti_alias
{
	uint32_t mode;
	uint32_t target;
	/* in the target's pixels: left, top, right, bottom */
	int32_t rectangle[4];
};

struct xgpu_present
{
	uint32_t target;
	/* the frame's number, which the results of its visibility tests carry */
	uint32_t frame;
	uint32_t reserved[2];
};

enum
{
	/* a BMP (debug.screenshot_every) */
	_xgpu_screenshot_bmp = 1,
	/* a PNG (the screenshot key) */
	_xgpu_screenshot_png,
};

struct xgpu_screenshot
{
	uint32_t target;
	uint32_t format;
	uint32_t reserved[2];
	/* UTF-8, zero-terminated */
	char path[XGPU_PATH_SIZE];
};

_Static_assert(sizeof(struct xgpu_header) <= XGPU_HEADER_SIZE, "the header fits its page");
_Static_assert(sizeof(struct xgpu_results) <= XGPU_RESULTS_SIZE, "the results fit their area");
_Static_assert(sizeof(struct xgpu_message_header) == XGPU_MESSAGE_ALIGNMENT, "message header");
_Static_assert((XGPU_RING_SIZE & (XGPU_RING_SIZE - 1)) == 0, "the ring's size is a power of two");
_Static_assert(sizeof(struct xgpu_sampler_state) == 20, "sampler state");
_Static_assert(sizeof(struct xgpu_vertex_element) == 8, "vertex element");
_Static_assert(sizeof(struct xgpu_draw_source) == 16, "draw source");
_Static_assert(sizeof(struct xgpu_draw_state) % 4 == 0, "draw state");
_Static_assert(sizeof(struct xgpu_draw) % 16 == 0, "draw: its inline data follow it aligned");
_Static_assert(sizeof(struct xgpu_clear) % 16 == 0, "clear");
_Static_assert(sizeof(struct xgpu_screenshot) % 4 == 0, "screenshot");

#endif
