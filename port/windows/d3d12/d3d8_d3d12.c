/*
D3D8_D3D12.C

The Xbox Direct3D 8 device of the Windows build that draws with Direct3D 12
(configure.py --windows-renderer d3d12), in place of d3d8_gl.c. The drawing
itself is done by the renderer process (port/windows/renderer, 64-bit),
which this file sends messages (xgpu_protocol.h); everything that decides
what to draw is d3d8_gl.c's, kept as it is there where it does not touch
OpenGL: the screen's width and scale, the render targets found by their
memory, the vertical blank, the vertex shader objects, the mirror of the
contiguous memory, quads made triangles, visibility tests divided by their
targets' scale, the menus' pointer.

What differs from d3d8_gl.c:
- the NV2A translators' GLSL becomes HLSL (xgpu_hlsl.c), compiled by the
  renderer; a vertex shader has a variant for each way its declaration's
  attributes reach it (xgpu_hlsl.h's input kinds);
- a draw's state is gathered in struct xgpu_draw_state, sent when it
  changes; the vertex constants go as the range that changed, the other
  uniforms as one struct (xgpu_hlsl.h);
- vertex data is sent as bytes (the mirror's pages once, and again when the
  game writes them; streamed data with each draw), never read by the
  renderer from the game's memory;
- D3D12 has no fans, loops, polygons or quads: they become index lists here;
- results come back a frame or two later through the shared section: the
  visibility tests' counts, the screenshots.
Textures come from the shared texture cache (xbox_textures.c), whose GL
texture calls the renderer carries out (xgpu_gl_textures.c): a "GL texture"
there is a renderer texture here, by the same number.
*/

#include "xgpu.h"
#include "sdl_platform.h"
#include "halo_ui_pointer.h"
#include "port_config.h"
#include "main/console.h"
#include "xgpu_hlsl.h"
#include "xgpu_ring.h"
#include "xgpu_host.h"
#include "xgpu_gl_textures.h"

#include "zlib_prefixed.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void d3d8_surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);
void d3d8_surface_resize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);

/* ---------- the screen's width (as d3d8_gl.c) */

#define SCREEN_HEIGHT 480
#define SCREEN_MAXIMUM_WIDTH 1920

static long screen_width;
static float screen_scale[2] = { 1.0f, 1.0f };
static long ui_offset;
#define UI_OFFSET ((long)ui_offset)

/* ---------- anti-aliasing (as d3d8_gl.c) */

enum
{
	_anti_aliasing_off,
	_anti_aliasing_fxaa,
	_anti_aliasing_smaa,
	_anti_aliasing_ssaa,
	_anti_aliasing_msaa,
};

static const struct
{
	const char *name;
	int mode;
	int samples;
} anti_aliasing_values[] =
{
	{ "off", _anti_aliasing_off, 0 },
	{ "fxaa", _anti_aliasing_fxaa, 0 },
	{ "smaa", _anti_aliasing_smaa, 0 },
	{ "ssaa2x", _anti_aliasing_ssaa, 0 },
	{ "msaa2x", _anti_aliasing_msaa, 2 },
	{ "msaa4x", _anti_aliasing_msaa, 4 },
	{ "msaa8x", _anti_aliasing_msaa, 8 },
};

#define NUMBER_OF_ANTI_ALIASING_VALUES ((int)(sizeof(anti_aliasing_values) / sizeof(anti_aliasing_values[0])))

static int anti_aliasing_value = -1;
static int anti_aliasing_samples;
static long anti_aliasing_maximum_samples, maximum_target_size;

static void anti_aliasing_prepare(void);
void xgpu_device_shutdown(void);

static void anti_aliasing_read(void)
{
	const char *setting = config_string("display.anti_aliasing");
	int value;

	for (value = NUMBER_OF_ANTI_ALIASING_VALUES - 1;
		value > 0 && strcmp(setting, anti_aliasing_values[value].name);
		value--)
	{
	}
	if (value == anti_aliasing_value)
		return;
	anti_aliasing_value = value;
	if (strcmp(setting, anti_aliasing_values[value].name))
		platform_log("anti-aliasing: \"%s\" is unknown, so off", setting);
	else
		platform_log("anti-aliasing: %s", setting);
	anti_aliasing_prepare();
}

static int anti_aliasing(void)
{
	if (anti_aliasing_value < 0)
		anti_aliasing_read();
	return anti_aliasing_values[anti_aliasing_value].mode;
}

static void screen_mode_choose(long *width, float scale[2])
{
	long display_width, display_height;

	*width = 640;
	scale[0] = scale[1] = 1.0f;
	if (platform_screen_mode(&display_width, &display_height) && display_width > 0 && display_height > 0)
	{
		long wanted = (SCREEN_HEIGHT * display_width + display_height / 2) / display_height;

		*width = wanted < 640 ? 640 : wanted > SCREEN_MAXIMUM_WIDTH ? SCREEN_MAXIMUM_WIDTH : wanted & ~1L;
		scale[0] = (float)display_width / (float)*width;
		scale[1] = (float)display_height / (float)SCREEN_HEIGHT;
		if (*width != wanted && *width != (wanted & ~1L))
			scale[0] = scale[1] = scale[0] < scale[1] ? scale[0] : scale[1];
		if (anti_aliasing() == _anti_aliasing_ssaa && maximum_target_size > 0)
		{
			float factor = 2.0f;

			if (*width * scale[0] * factor > (float)maximum_target_size)
				factor = (float)maximum_target_size / (*width * scale[0]);
			if (SCREEN_HEIGHT * scale[1] * factor > (float)maximum_target_size)
				factor = (float)maximum_target_size / (SCREEN_HEIGHT * scale[1]);
			if (factor > 1.0f)
			{
				scale[0] *= factor;
				scale[1] *= factor;
			}
		}
	}
}

long halo_screen_width(void)
{
	if (!screen_width)
	{
		screen_mode_choose(&screen_width, screen_scale);
		platform_log("screen: %ldx%d drawn at %.0fx%.0f", screen_width, SCREEN_HEIGHT,
			screen_width * screen_scale[0], SCREEN_HEIGHT * screen_scale[1]);
	}
	return screen_width;
}

float halo_screen_pixel_scale(void)
{
	halo_screen_width();
	return screen_scale[1];
}

float halo_screen_scale(void)
{
	halo_screen_width();
	return screen_scale[0] > screen_scale[1] ? screen_scale[0] : screen_scale[1];
}

#define SHADOW_MAP_SIZE 128
#define SHADOW_MAP_MAXIMUM_SCALE 8

static long shadow_scale;
static unsigned long shadow_scale_read_at;

static long shadow_scale_choose(void)
{
	long resolution = config_integer("display.shadow_resolution");
	long scale = 1;

	while (scale < SHADOW_MAP_MAXIMUM_SCALE && SHADOW_MAP_SIZE * scale * 2 <= resolution)
		scale *= 2;
	return scale;
}

long halo_shadow_map_scale(void)
{
	if (!shadow_scale)
	{
		shadow_scale_read_at = config_changes();
		shadow_scale = shadow_scale_choose();
		platform_log("shadow maps: %ldx%ld", SHADOW_MAP_SIZE * shadow_scale, SHADOW_MAP_SIZE * shadow_scale);
	}
	return shadow_scale;
}

void halo_screen_ui_offset(unsigned char centered)
{
	ui_offset = centered ? (halo_screen_width() - 640) / 2 : 0;
}

/* ---------- state the XDK header's inline functions read and write */

DWORD D3D__RenderState[D3DRS_MAX];
DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];
WORD *D3D__IndexData;
BYTE D3D__StateBlockDirty[1024];

/* ---------- vertex shaders */

#define VERTEX_SHADER_SIGNATURE 0x76736864UL /* 'vshd' */
#define VERTEX_PROGRAM_SLOTS 136

struct vertex_element
{
	unsigned char reg;
	unsigned char stream;
	unsigned char type;
	unsigned char bytes;
	unsigned short offset;
};

/* a translation of a program, for one way of reaching its attributes */
struct vertex_variant
{
	struct vertex_variant *next;
	unsigned char immediate, lit;
	unsigned char input_kinds[XGPU_ATTRIBUTE_COUNT];
	unsigned long packed_mask;
	/* the renderer's shader, 0 if it did not translate */
	unsigned long shader;
};

struct vertex_shader_object
{
	unsigned long signature;
	unsigned long id;
	DWORD *instructions;
	unsigned long instruction_count;
	struct vertex_element elements[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long element_count;
	unsigned long packed_mask;
	struct nv2a_vertex_lighting lighting;
	BOOL lighting_failed;
	struct vertex_variant *variants;
};

/* ---------- render targets */

enum render_target_kind
{
	_render_target_plain,
	_render_target_screen,
	_render_target_shadow_map,
};

struct render_target_entry
{
	struct render_target_entry *next;
	struct render_target_entry *next_in_bucket;
	/* .texture is the renderer's target number */
	struct xgpu_render_target target;
	unsigned long last_rendered;
	BOOL screen_buffer;
	enum render_target_kind kind;
};

#define RENDER_TARGET_BUCKET_COUNT 256

static struct render_target_entry *render_target_buckets[RENDER_TARGET_BUCKET_COUNT];
static struct render_target_entry *render_targets;
static unsigned long next_target_number = 1;

static struct render_target_entry **render_target_bucket(unsigned long data)
{
	return &render_target_buckets[((data >> 12) ^ (data >> 20)) % RENDER_TARGET_BUCKET_COUNT];
}

static void render_targets_evict(void);

/* ---------- the device */

#define VISIBILITY_TEST_SLOTS XGPU_VISIBILITY_SLOTS

struct d3d12_device
{
	D3DPRESENT_PARAMETERS presentation;
	D3DSurface back_buffer;
	D3DSurface depth_buffer;
	D3DSurface *render_target;
	D3DSurface *depth_stencil;
	D3DVIEWPORT8 viewport;
	D3DMATRIX transforms[D3DTS_MAX];
	D3DBaseTexture *textures[D3DTSS_MAXSTAGES];
	D3DPalette *palettes[D3DTSS_MAXSTAGES];
	D3DSHADERCONSTANTMODE shader_constant_mode;

	struct vertex_shader_object *vertex_shader;
	struct vertex_shader_object *program_slots[VERTEX_PROGRAM_SLOTS];
	unsigned long program_address;
	float constants[XGPU_VERTEX_CONSTANT_COUNT][4];
	/* the registers changed since they were last sent */
	unsigned long constants_first, constants_last;
	float viewport_scale[4];
	float viewport_offset[4];

	struct
	{
		DWORD data;
		UINT stride;
	} streams[16];
	UINT base_vertex_index;

	float attributes[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
	BOOL immediate_active;
	D3DPRIMITIVETYPE immediate_type;
	float *immediate_vertices;
	unsigned long immediate_count;
	unsigned long immediate_capacity;

	BOOL query_pending[VISIBILITY_TEST_SLOTS];
	float query_area[VISIBILITY_TEST_SLOTS];
	BOOL visibility_test_active;

	/* the window's drawable size and vsync, as last sent */
	int sent_width, sent_height;
	int sent_vsync;

	unsigned long frame;
	unsigned long next_vertex_shader_id;
	unsigned long next_shader_number;
	BOOL ready;
	BOOL created;
};

static struct d3d12_device device;

/* the draw state and uniforms last sent */
static struct xgpu_draw_state sent_state;
static BOOL sent_state_valid;
static struct xgpu_hlsl_draw_uniforms sent_uniforms;
static BOOL sent_uniforms_valid;
static float sent_lights[XGPU_MODEL_LIGHT_COUNT][4];
static BOOL sent_lights_valid;

static struct
{
	unsigned long draws, immediate_draws, clears, presents;
	unsigned long skipped_no_program, skipped_no_target, skipped_large;
	unsigned long target_changes;
	unsigned long mirrored_bytes, streamed_bytes, state_changes;
} stats;

static D3DDevice *device_pointer(void)
{
	return (D3DDevice *)&device;
}

static float dword_to_float(DWORD value)
{
	union { DWORD d; float f; } u;

	u.d = value;
	return u.f;
}

static void color_to_vec4(D3DCOLOR color, float *out)
{
	out[0] = ((color >> 16) & 0xff) / 255.0f;
	out[1] = ((color >> 8) & 0xff) / 255.0f;
	out[2] = (color & 0xff) / 255.0f;
	out[3] = ((color >> 24) & 0xff) / 255.0f;
}

static struct
{
	const char *skip_vertex_shaders;
	const char *dump_shaders;
	BOOL statistics;
} debug_settings;

/* nothing caches GL state here: xbox_textures.c calls this after its
uploads */
void xgpu_gl_state_invalidate(void)
{
}

/* ---------- vertical blank emulation (as d3d8_gl.c) */

#define VERTICAL_BLANK_NANOSECONDS (1000000000L / 60)

static pthread_mutex_t vertical_blank_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vertical_blank_condition = PTHREAD_COND_INITIALIZER;
static D3DCALLBACK vertical_blank_callback;
static unsigned long vertical_blank_count;
static volatile unsigned int flip_count;
static unsigned long pending_flips;
static BOOL vertical_blank_thread_started = FALSE;

static void *vertical_blank_thread(void *unused)
{
	struct timespec next;

	(void)unused;
	clock_gettime(CLOCK_MONOTONIC, &next);
	for (;;)
	{
		D3DCALLBACK callback;

		next.tv_nsec += VERTICAL_BLANK_NANOSECONDS;
		if (next.tv_nsec >= 1000000000L)
		{
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

		pthread_mutex_lock(&vertical_blank_lock);
		vertical_blank_count++;
		if (pending_flips)
		{
			pending_flips--;
			flip_count++;
		}
		callback = vertical_blank_callback;
		pthread_cond_broadcast(&vertical_blank_condition);
		pthread_mutex_unlock(&vertical_blank_lock);

		if (callback)
			callback(0);
	}
	return NULL;
}

static void vertical_blank_start(void)
{
	pthread_mutex_lock(&vertical_blank_lock);
	if (!vertical_blank_thread_started)
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, vertical_blank_thread, NULL) == 0)
		{
			pthread_detach(thread);
			vertical_blank_thread_started = TRUE;
		}
		else
		{
			platform_log("cannot start the vertical blank thread");
		}
	}
	pthread_mutex_unlock(&vertical_blank_lock);
}

volatile unsigned int *d3d_find_flipcount(void)
{
	return &flip_count;
}

void WINAPI D3DDevice_SetVerticalBlankCallback(D3DCALLBACK callback)
{
	pthread_mutex_lock(&vertical_blank_lock);
	vertical_blank_callback = callback;
	pthread_mutex_unlock(&vertical_blank_lock);
	vertical_blank_start();
}

void WINAPI D3DDevice_BlockUntilVerticalBlank(void)
{
	unsigned long count;

	vertical_blank_start();
	pthread_mutex_lock(&vertical_blank_lock);
	count = vertical_blank_count;
	while (vertical_blank_count == count)
		pthread_cond_wait(&vertical_blank_condition, &vertical_blank_lock);
	pthread_mutex_unlock(&vertical_blank_lock);
}

/* ---------- render targets (as d3d8_gl.c, made by the renderer) */

static void surface_dimensions(const D3DSurface *surface, unsigned long *width, unsigned long *height, BOOL *depth)
{
	struct xgpu_texture_description description;
	DWORD format;

	xgpu_texture_describe(surface->Format, surface->Size, &description);
	*width = description.width;
	*height = description.height;
	format = description.format;
	*depth = format == D3DFMT_D24S8 || format == D3DFMT_F24S8 || format == D3DFMT_D16 || format == D3DFMT_F16 ||
		format == D3DFMT_LIN_D24S8 || format == D3DFMT_LIN_F24S8 || format == D3DFMT_LIN_D16 || format == D3DFMT_LIN_F16;
}

static BOOL surface_is_shadow_map(const D3DSurface *surface)
{
	struct xgpu_texture_description description;

	xgpu_texture_describe(surface->Format, surface->Size, &description);
	return description.format == D3DFMT_R5G6B5 && description.width == SHADOW_MAP_SIZE &&
		description.height == SHADOW_MAP_SIZE;
}

#define SECONDARY_TARGET_WIDTH 320
#define SECONDARY_TARGET_HEIGHT 240
#define RECENT_RENDER_TARGET_COUNT 4

static struct
{
	DWORD data, format, size;
	long screen_width;
	float scale[2];
	struct render_target_entry *entry;
} recent_render_targets[RECENT_RENDER_TARGET_COUNT];
static unsigned long recent_render_target_next;

static struct render_target_entry *render_target_remember(const D3DSurface *surface, long screen,
	struct render_target_entry *entry)
{
	unsigned long slot = recent_render_target_next++ % RECENT_RENDER_TARGET_COUNT;

	recent_render_targets[slot].data = surface->Data;
	recent_render_targets[slot].format = surface->Format;
	recent_render_targets[slot].size = surface->Size;
	recent_render_targets[slot].screen_width = screen;
	recent_render_targets[slot].scale[0] = screen_scale[0];
	recent_render_targets[slot].scale[1] = screen_scale[1];
	recent_render_targets[slot].entry = entry;
	return entry;
}

/* defines the entry's target in the renderer, with its samples now */
static void render_target_send(struct render_target_entry *entry)
{
	struct xgpu_target_define definition;

	memset(&definition, 0, sizeof(definition));
	definition.target = entry->target.texture;
	definition.width = entry->target.gl_width;
	definition.height = entry->target.gl_height;
	definition.depth = entry->target.depth != FALSE;
	definition.samples = entry->target.samples > 1 ? (uint32_t)entry->target.samples : 1;
	xgpu_ring_send_data(_xgpu_op_target_define, &definition, sizeof(definition), NULL, 0);
}

static struct render_target_entry *render_target_get(const D3DSurface *surface)
{
	struct render_target_entry *entry;
	unsigned long width, height, slot;
	long screen;
	BOOL depth;
	float scale[2] = { 1.0f, 1.0f };
	enum render_target_kind kind = _render_target_plain;

	if (!surface || !surface->Data)
		return NULL;
	screen = halo_screen_width();
	for (slot = 0; slot < RECENT_RENDER_TARGET_COUNT; slot++)
	{
		if (recent_render_targets[slot].entry && recent_render_targets[slot].data == surface->Data &&
			recent_render_targets[slot].format == surface->Format && recent_render_targets[slot].size == surface->Size &&
			recent_render_targets[slot].screen_width == screen && recent_render_targets[slot].scale[0] == screen_scale[0] &&
			recent_render_targets[slot].scale[1] == screen_scale[1])
		{
			return recent_render_targets[slot].entry;
		}
	}
	surface_dimensions(surface, &width, &height, &depth);
	if ((width == (unsigned long)halo_screen_width() && height == SCREEN_HEIGHT) ||
		(!depth && width == SECONDARY_TARGET_WIDTH && height == SECONDARY_TARGET_HEIGHT))
	{
		scale[0] = screen_scale[0];
		scale[1] = screen_scale[1];
		kind = _render_target_screen;
	}
	else if (!depth && surface_is_shadow_map(surface))
	{
		scale[0] = (float)halo_shadow_map_scale();
		scale[1] = scale[0];
		kind = _render_target_shadow_map;
	}
	for (entry = *render_target_bucket(surface->Data); entry; entry = entry->next_in_bucket)
	{
		if (entry->target.data == surface->Data && entry->target.width == width &&
			entry->target.height == height && entry->target.depth == depth &&
			entry->target.scale[0] == scale[0] && entry->target.scale[1] == scale[1])
		{
			return render_target_remember(surface, screen, entry);
		}
	}
	entry = calloc(1, sizeof(*entry));
	entry->target.data = surface->Data;
	entry->target.width = width;
	entry->target.height = height;
	entry->target.depth = depth;
	entry->target.scale[0] = scale[0];
	entry->target.scale[1] = scale[1];
	entry->kind = kind;
	entry->target.gl_width = (unsigned long)(width * scale[0] + 0.5f);
	entry->target.gl_height = (unsigned long)(height * scale[1] + 0.5f);
	entry->target.texture = (GLuint)next_target_number++;
	entry->target.samples = 0;
	entry->screen_buffer = surface->Data == (depth ? device.depth_buffer.Data : device.back_buffer.Data);
	render_target_send(entry);
	entry->next = render_targets;
	render_targets = entry;
	entry->next_in_bucket = *render_target_bucket(entry->target.data);
	*render_target_bucket(entry->target.data) = entry;
	return entry;
}

struct xgpu_render_target *xgpu_render_target_find(unsigned long data)
{
	struct render_target_entry *entry, *best = NULL;

	for (entry = *render_target_bucket(data); entry; entry = entry->next_in_bucket)
	{
		if (entry->target.data == data && !entry->target.depth && (!best || entry->last_rendered > best->last_rendered))
			best = entry;
	}
	return best ? &best->target : NULL;
}

/* ---------- multisampling: the renderer keeps a target's samples, and
resolves them into its pixels when asked, before anything reads them */

static int target_samples = 1;

static void render_target_resolve(struct xgpu_render_target *target)
{
	struct xgpu_target_reference reference;

	if (!target->unresolved)
		return;
	target->unresolved = FALSE;
	memset(&reference, 0, sizeof(reference));
	reference.target = target->texture;
	xgpu_ring_send_data(_xgpu_op_target_resolve, &reference, sizeof(reference), NULL, 0);
}

/* a target with these samples a pixel (0: none): defined again, its
pixels resolved first (the new storage starts empty, as the frame clears it) */
static void render_target_multisample(struct render_target_entry *entry, int samples)
{
	if (entry->target.samples == samples || (entry->target.samples <= 1 && samples <= 1))
	{
		entry->target.samples = samples;
		return;
	}
	render_target_resolve(&entry->target);
	entry->target.samples = samples;
	render_target_send(entry);
}

static unsigned long render_target_write_serial;
static float target_scale[2] = { 1.0f, 1.0f };

static long scaled_pixel(float coordinate, float scale)
{
	float value = coordinate * scale + 0.5f;
	long pixel = (long)value;

	if ((float)pixel > value)
		pixel--;
	return pixel;
}

static long target_pixel(float coordinate, int axis)
{
	return scaled_pixel(coordinate, target_scale[axis]);
}

/* the draw's targets, and their size in pixels; FALSE if there is nothing to
draw into */
static BOOL bind_targets(struct render_target_entry **color_out, struct render_target_entry **depth_out)
{
	struct render_target_entry *color = render_target_get(device.render_target);
	struct render_target_entry *depth = render_target_get(device.depth_stencil);
	int samples;

	if (depth && !depth->target.depth)
		depth = NULL;
	*color_out = color;
	*depth_out = depth;
	if (!color && !depth)
		return FALSE;
	if (color)
	{
		color->last_rendered = device.frame + 1;
		color->target.written = ++render_target_write_serial;
	}
	target_scale[0] = color ? color->target.scale[0] : depth->target.scale[0];
	target_scale[1] = color ? color->target.scale[1] : depth->target.scale[1];
	samples = anti_aliasing() == _anti_aliasing_msaa ? anti_aliasing_samples : 0;
	if (samples && !((color && (color->screen_buffer || color->target.samples > 1)) ||
		(depth && (depth->screen_buffer || depth->target.samples > 1))))
	{
		samples = 0;
	}
	if (color)
		render_target_multisample(color, samples);
	if (depth)
		render_target_multisample(depth, samples);
	if (samples > 1)
	{
		if (color)
			color->target.unresolved = TRUE;
		if (depth)
			depth->target.unresolved = TRUE;
		target_samples = samples;
	}
	else
	{
		target_samples = 1;
	}
	return TRUE;
}

/* ---------- device creation */

static void device_initialize(void)
{
	struct xgpu_header *header = xgpu_host_header();
	int index;

	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
		device.attributes[index][3] = 1.0f;
	memory_watch_initialize();
	debug_settings.skip_vertex_shaders = config_string("debug.gpu_skip_vertex_shaders");
	debug_settings.dump_shaders = *config_string("debug.gpu_dump_shaders") ?
		config_string("debug.gpu_dump_shaders") : NULL;
	debug_settings.statistics = config_boolean("debug.gpu_stats");
	anti_aliasing_maximum_samples = header->maximum_samples;
	maximum_target_size = header->maximum_texture_size;
	device.constants_first = XGPU_VERTEX_CONSTANT_COUNT;
	device.sent_vsync = -1;
	device.next_shader_number = 1;
	device.ready = TRUE;
	/* (at exit the renderer finishes the frames sent and writes their
	screenshots: the scene tests exit just after asking for one) */
	atexit(xgpu_device_shutdown);
	platform_log("Direct3D 12 on %s (halo_renderer.exe)", header->adapter_name);
	if (anti_aliasing_value < 0)
		anti_aliasing_read();
	else
		anti_aliasing_prepare();
}

/* port/third_party/smaa, embedded by tools/embed_assets.py (as xgpu_post.c
has them): the shader's text, ending in a NUL, and the lookup textures'
bytes compressed with zlib; each size 0 where the build had no such file */
extern const unsigned int xgpu_smaa_shader[];
extern const unsigned long xgpu_smaa_shader_size;
extern const unsigned int xgpu_smaa_area_texture[];
extern const unsigned long xgpu_smaa_area_texture_size;
extern const unsigned int xgpu_smaa_search_texture[];
extern const unsigned long xgpu_smaa_search_texture_size;

/* a lookup texture of SMAA's, inflated from its zlib stream and sent as an
RGBA8 texture of its channels (the rest zero); 0 if it does not inflate */
static unsigned long smaa_lookup_send(const unsigned int *stream, unsigned long stream_size, unsigned long width,
	unsigned long height, unsigned long channels)
{
	uLongf size = (uLongf)(width * height * channels);
	unsigned char *texels = malloc(size), *rgba = malloc(width * height * 4);
	unsigned long number = 0, texel, channel;
	int result;

	if (texels && rgba)
	{
		result = uncompress(texels, &size, (const Bytef *)stream, (uLong)stream_size);
		if ((result == Z_OK || result == Z_BUF_ERROR) && size == (uLongf)(width * height * channels))
		{
			struct xgpu_texture_define definition;
			struct xgpu_texture_data data;

			memset(rgba, 0, width * height * 4);
			for (texel = 0; texel < width * height; texel++)
			{
				for (channel = 0; channel < channels; channel++)
					rgba[texel * 4 + channel] = texels[texel * channels + channel];
			}
			number = xgpu_texture_number_new();
			memset(&definition, 0, sizeof(definition));
			definition.texture = number;
			definition.dimension = _xgpu_texture_2d;
			definition.format = _xgpu_format_rgba8;
			definition.width = width;
			definition.height = height;
			definition.depth = 1;
			definition.levels = 1;
			definition.swizzle[0] = 0;
			definition.swizzle[1] = 1;
			definition.swizzle[2] = 2;
			definition.swizzle[3] = 3;
			xgpu_ring_send_data(_xgpu_op_texture_define, &definition, sizeof(definition), NULL, 0);
			memset(&data, 0, sizeof(data));
			data.texture = number;
			data.width = width;
			data.height = height;
			data.depth = 1;
			data.row_pitch = width * 4;
			xgpu_ring_send_data(_xgpu_op_texture_data, &data, sizeof(data), rgba, width * height * 4);
		}
	}
	free(texels);
	free(rgba);
	return number;
}

/* the renderer's passes for FXAA or SMAA made, as the setting is chosen
rather than in the middle of a frame (SMAA's are large) */
static void anti_aliasing_passes_send(int mode)
{
	static BOOL fxaa_sent, smaa_sent;
	struct xgpu_anti_alias_prepare prepare;

	memset(&prepare, 0, sizeof(prepare));
	if (mode == _anti_aliasing_fxaa && !fxaa_sent)
	{
		fxaa_sent = TRUE;
		prepare.mode = _xgpu_anti_alias_fxaa;
		xgpu_ring_send_data(_xgpu_op_anti_alias_prepare, &prepare, sizeof(prepare), NULL, 0);
	}
	if (mode == _anti_aliasing_smaa && !smaa_sent)
	{
		smaa_sent = TRUE;
		if (!xgpu_smaa_shader_size)
		{
			platform_log("anti-aliasing: the build has no SMAA");
			return;
		}
		prepare.mode = _xgpu_anti_alias_smaa;
		prepare.area_texture = (uint32_t)smaa_lookup_send(xgpu_smaa_area_texture, xgpu_smaa_area_texture_size,
			160, 560, 2);
		prepare.search_texture = (uint32_t)smaa_lookup_send(xgpu_smaa_search_texture, xgpu_smaa_search_texture_size,
			64, 16, 1);
		if (!prepare.area_texture || !prepare.search_texture)
		{
			platform_log("anti-aliasing: cannot inflate SMAA's lookup textures");
			return;
		}
		xgpu_ring_send_data(_xgpu_op_anti_alias_prepare, &prepare, sizeof(prepare), xgpu_smaa_shader,
			(unsigned long)strlen((const char *)xgpu_smaa_shader));
	}
}

static void anti_aliasing_prepare(void)
{
	if (!device.ready || anti_aliasing_value < 0)
		return;
	anti_aliasing_samples = anti_aliasing_values[anti_aliasing_value].samples;
	if (anti_aliasing_samples > anti_aliasing_maximum_samples)
	{
		platform_log("anti-aliasing: the GPU has at most %ld samples a pixel", anti_aliasing_maximum_samples);
		anti_aliasing_samples = anti_aliasing_maximum_samples < 2 ? 0 : (int)anti_aliasing_maximum_samples;
	}
	anti_aliasing_passes_send(anti_aliasing_values[anti_aliasing_value].mode);
}

Direct3D *WINAPI Direct3DCreate8(UINT sdk_version)
{
	(void)sdk_version;
	return (Direct3D *)1;
}

void WINAPI Direct3D_SetPushBufferSize(DWORD push_buffer_size, DWORD segment_count)
{
	(void)push_buffer_size;
	(void)segment_count;
}

static void constants_store(unsigned long first, const void *data, unsigned long count)
{
	const float (*values)[4] = data;
	unsigned long index;

	for (index = 0; index < count; index++)
	{
		if (memcmp(device.constants[first + index], values[index], sizeof(device.constants[0])))
		{
			memcpy(device.constants[first + index], values[index], sizeof(device.constants[0]));
			if (device.constants_first > first + index)
				device.constants_first = first + index;
			if (device.constants_last < first + index)
				device.constants_last = first + index;
		}
	}
}

static void viewport_update_constants(void)
{
	float zscale = 16777215.0f;

	if (device.depth_stencil)
	{
		struct xgpu_texture_description description;

		xgpu_texture_describe(device.depth_stencil->Format, device.depth_stencil->Size, &description);
		if (description.format == D3DFMT_D16 || description.format == D3DFMT_LIN_D16 ||
			description.format == D3DFMT_F16 || description.format == D3DFMT_LIN_F16)
		{
			zscale = 65535.0f;
		}
	}
	device.viewport_scale[0] = device.viewport.Width * 0.5f;
	device.viewport_scale[1] = -(float)device.viewport.Height * 0.5f;
	device.viewport_scale[2] = zscale * (device.viewport.MaxZ - device.viewport.MinZ);
	device.viewport_scale[3] = 0.0f;
	device.viewport_offset[0] = device.viewport.X + device.viewport.Width * 0.5f;
	device.viewport_offset[1] = device.viewport.Y + device.viewport.Height * 0.5f;
	device.viewport_offset[2] = zscale * device.viewport.MinZ;
	device.viewport_offset[3] = 0.0f;
	if (!(device.shader_constant_mode & D3DSCM_NORESERVEDCONSTANTS))
	{
		constants_store(XGPU_VERTEX_CONSTANT_BIAS - 38, device.viewport_scale, 1);
		constants_store(XGPU_VERTEX_CONSTANT_BIAS - 37, device.viewport_offset, 1);
	}
}

HRESULT WINAPI Direct3D_CreateDevice(UINT adapter, D3DDEVTYPE device_type, void *unused, DWORD behavior_flags,
	D3DPRESENT_PARAMETERS *presentation_parameters, D3DDevice **returned_device)
{
	unsigned long width, height;
	int index;

	(void)adapter;
	(void)device_type;
	(void)unused;
	(void)behavior_flags;
	if (!device.created)
	{
		memset(&device, 0, sizeof(device));
		if (presentation_parameters)
			device.presentation = *presentation_parameters;
		width = device.presentation.BackBufferWidth ? device.presentation.BackBufferWidth : 640;
		height = device.presentation.BackBufferHeight ? device.presentation.BackBufferHeight : 480;
		d3d8_surface_initialize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, SCREEN_MAXIMUM_WIDTH, height);
		d3d8_surface_initialize(&device.depth_buffer, D3DFMT_LIN_D24S8, SCREEN_MAXIMUM_WIDTH, height);
		d3d8_surface_resize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, width, height);
		d3d8_surface_resize(&device.depth_buffer, D3DFMT_LIN_D24S8, width, height);
		device.render_target = &device.back_buffer;
		device.depth_stencil = &device.depth_buffer;
		for (index = 0; index < D3DTS_MAX; index++)
		{
			device.transforms[index]._11 = 1.0f;
			device.transforms[index]._22 = 1.0f;
			device.transforms[index]._33 = 1.0f;
			device.transforms[index]._44 = 1.0f;
		}
		device.viewport.Width = width;
		device.viewport.Height = height;
		device.viewport.MaxZ = 1.0f;
		device.next_vertex_shader_id = 1;
		device.constants_first = XGPU_VERTEX_CONSTANT_COUNT;
		D3D__RenderState[D3DRS_ZENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZWRITEENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
		D3D__RenderState[D3DRS_COLORWRITEENABLE] = D3DCOLORWRITEENABLE_ALL;
		D3D__RenderState[D3DRS_SRCBLEND] = D3DBLEND_ONE;
		D3D__RenderState[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
		D3D__RenderState[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
		D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_CCW;
		D3D__RenderState[D3DRS_FRONTFACE] = D3DFRONT_CW;
		D3D__RenderState[D3DRS_FILLMODE] = D3DFILL_SOLID;
		D3D__RenderState[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILWRITEMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
		for (index = 0; index < D3DTSS_MAXSTAGES; index++)
		{
			D3D__TextureState[index][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSW] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_MAGFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MINFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MAXANISOTROPY] = 1;
		}
		viewport_update_constants();

		/* (platform_video_initialize starts the renderer process with the
		window: sdl_platform.c) */
		if (!config_boolean("debug.null_renderer") && platform_video_initialize(width, height))
			device_initialize();
		else
			platform_log("Direct3D: running without a window (nothing is displayed)");
		device.created = TRUE;
	}
	*returned_device = device_pointer();
	return S_OK;
}

/* ---------- the menus' pointer (as d3d8_gl.c) */

static void ui_point_from_window_on(float window_x, float window_y, int centered, short *x, short *y)
{
	struct render_target_entry *back_buffer = render_target_get(&device.back_buffer);
	int window_width, window_height, pixel_width, pixel_height, width, height, left, top;
	float screen_x, screen_y;

	*x = *y = -1;
	if (!back_buffer)
		return;
	platform_video_window_size(&window_width, &window_height);
	platform_video_drawable_size(&pixel_width, &pixel_height);
	if (window_width <= 0 || window_height <= 0)
		return;
	width = pixel_width;
	height = (int)((long)pixel_width * back_buffer->target.gl_height / back_buffer->target.gl_width);
	if (height > pixel_height)
	{
		height = pixel_height;
		width = (int)((long)pixel_height * back_buffer->target.gl_width / back_buffer->target.gl_height);
	}
	left = (pixel_width - width) / 2;
	top = (pixel_height - height) / 2;
	screen_x = (window_x * pixel_width / window_width - left) * (float)back_buffer->target.width / (float)width;
	screen_y = (window_y * pixel_height / window_height - top) * (float)back_buffer->target.height / (float)height;
	*x = (short)floorf(screen_x - (centered ? (float)(halo_screen_width() - 640) / 2.0f : 0.0f));
	*y = (short)floorf(screen_y);
}

static void ui_point_from_window(float window_x, float window_y, short *x, short *y)
{
	ui_point_from_window_on(window_x, window_y, TRUE, x, y);
}

int halo_scoreboard_pointer_update(int offered, struct halo_ui_pointer *pointer)
{
	struct platform_ui_pointer state;

	memset(pointer, 0, sizeof(*pointer));
	if (!platform_scoreboard_pointer(offered != 0, &state) || !device.ready)
		return 0;
	ui_point_from_window_on(state.x, state.y, FALSE, &pointer->x, &pointer->y);
	ui_point_from_window_on(state.click_x, state.click_y, FALSE, &pointer->click_x, &pointer->click_y);
	pointer->moved = state.moved != FALSE;
	pointer->left_clicks = (unsigned char)(state.left_clicks < 255 ? state.left_clicks : 255);
	return 1;
}

int halo_ui_pointer_update(int menus_active, struct halo_ui_pointer *pointer)
{
	struct platform_ui_pointer state;

	platform_menus_set_active(menus_active != 0);
	platform_ui_pointer_set_active(menus_active != 0);
	if (!menus_active || !device.ready || !platform_ui_pointer_read(&state))
		return 0;
	memset(pointer, 0, sizeof(*pointer));
	ui_point_from_window(state.x, state.y, &pointer->x, &pointer->y);
	ui_point_from_window(state.click_x, state.click_y, &pointer->click_x, &pointer->click_y);
	pointer->moved = state.moved != FALSE;
	pointer->left_clicks = (unsigned char)(state.left_clicks < 255 ? state.left_clicks : 255);
	pointer->right_clicks = (unsigned char)(state.right_clicks < 255 ? state.right_clicks : 255);
	pointer->wheel_steps = (signed char)(state.wheel_steps < -8 ? -8 : state.wheel_steps > 8 ? 8 : state.wheel_steps);
	pointer->touch = state.touch != FALSE;
	pointer->downs = (unsigned char)(state.downs < 255 ? state.downs : 255);
	ui_point_from_window(state.down_x, state.down_y, &pointer->down_x, &pointer->down_y);
	return 1;
}

long halo_screen_commit(void)
{
	long width;
	float scale[2];

	anti_aliasing_read();
	if (shadow_scale && shadow_scale_read_at != config_changes())
	{
		long shadow = shadow_scale_choose();

		shadow_scale_read_at = config_changes();
		if (shadow != shadow_scale)
		{
			platform_log("shadow maps: %ldx%ld", SHADOW_MAP_SIZE * shadow, SHADOW_MAP_SIZE * shadow);
			shadow_scale = shadow;
			render_targets_evict();
		}
	}
	if (!screen_width)
		return halo_screen_width();
	screen_mode_choose(&width, scale);
	if (width != screen_width || scale[0] != screen_scale[0] || scale[1] != screen_scale[1])
	{
		platform_log("screen: %ldx%d drawn at %.0fx%.0f", width, SCREEN_HEIGHT,
			width * scale[0], SCREEN_HEIGHT * scale[1]);
		screen_width = width;
		screen_scale[0] = scale[0];
		screen_scale[1] = scale[1];
		if (device.created)
		{
			device.presentation.BackBufferWidth = (UINT)width;
			d3d8_surface_resize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, (unsigned long)width, SCREEN_HEIGHT);
			d3d8_surface_resize(&device.depth_buffer, D3DFMT_LIN_D24S8, (unsigned long)width, SCREEN_HEIGHT);
		}
		render_targets_evict();
	}
	return screen_width;
}

ULONG WINAPI D3DDevice_Release(void)
{
	return 1;
}

void WINAPI D3DDevice_GetDeviceCaps(D3DCAPS8 *caps)
{
	memset(caps, 0, sizeof(*caps));
	caps->DeviceType = D3DDEVTYPE_HAL;
	caps->MaxTextureWidth = 4096;
	caps->MaxTextureHeight = 4096;
	caps->MaxVolumeExtent = 512;
	caps->MaxTextureRepeat = 8192;
	caps->MaxTextureAspectRatio = 4096;
	caps->MaxAnisotropy = 4;
	caps->MaxTextureBlendStages = 4;
	caps->MaxSimultaneousTextures = 4;
	caps->MaxActiveLights = 8;
	caps->MaxVertexBlendMatrices = 4;
	caps->MaxPointSize = 64.0f;
	caps->MaxPrimitiveCount = 0xfffff;
	caps->MaxVertexIndex = 0xffff;
	caps->MaxStreams = 16;
	caps->MaxStreamStride = 255;
	caps->VertexShaderVersion = D3DVS_VERSION(1, 1);
	caps->MaxVertexShaderConst = 192;
	caps->PixelShaderVersion = D3DPS_VERSION(1, 1);
	caps->MaxPixelShaderValue = 1.0f;
}

void WINAPI D3DDevice_GetBackBuffer(INT back_buffer, D3DBACKBUFFER_TYPE type, D3DSurface **result)
{
	(void)back_buffer;
	(void)type;
	device.back_buffer.Common++;
	*result = &device.back_buffer;
}

HRESULT WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **result)
{
	*result = device.depth_stencil;
	if (!*result)
		return D3DERR_NOTFOUND;
	(*result)->Common++;
	return S_OK;
}

void WINAPI D3DDevice_SetRenderTarget(D3DSurface *render_target, D3DSurface *depth_stencil)
{
	stats.target_changes++;
	if (render_target)
		device.render_target = render_target;
	device.depth_stencil = depth_stencil;
	if (device.render_target)
	{
		unsigned long width, height;
		BOOL depth;

		surface_dimensions(device.render_target, &width, &height, &depth);
		device.viewport.X = 0;
		device.viewport.Y = 0;
		device.viewport.Width = width;
		device.viewport.Height = height;
		device.viewport.MinZ = 0.0f;
		device.viewport.MaxZ = 1.0f;
	}
	viewport_update_constants();
}

void WINAPI D3DDevice_SetViewport(CONST D3DVIEWPORT8 *viewport)
{
	device.viewport = *viewport;
	viewport_update_constants();
}

void WINAPI D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		device.transforms[state] = *matrix;
}

void WINAPI D3DDevice_GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		*matrix = device.transforms[state];
}

void WINAPI D3DDevice_SetFlickerFilter(DWORD filter) { (void)filter; }
void WINAPI D3DDevice_SetSoftDisplayFilter(BOOL enable) { (void)enable; }

void WINAPI D3DDevice_SetShaderConstantMode(D3DSHADERCONSTANTMODE mode)
{
	device.shader_constant_mode = mode;
	viewport_update_constants();
}

/* ---------- GPU synchronisation: everything the GPU reads is copied when
it is handed over */

BOOL WINAPI D3DDevice_IsBusy(void)
{
	return FALSE;
}

void WINAPI D3DDevice_KickPushBuffer(void)
{
	if (device.ready)
		xgpu_ring_flush();
}

void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE type, D3DCALLBACK callback, DWORD context)
{
	(void)type;
	if (callback)
		callback(context);
}

/* ---------- visibility (occlusion) tests: counts come back in the shared
section a frame or two after the test, as the latest count of the slot */

void WINAPI D3DDevice_BeginVisibilityTest(void)
{
	if (!device.ready || device.visibility_test_active)
		return;
	device.visibility_test_active = TRUE;
	xgpu_ring_send_data(_xgpu_op_visibility_begin, NULL, 0, NULL, 0);
}

HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD index)
{
	struct xgpu_visibility visibility;

	if (!device.ready || !device.visibility_test_active)
		return S_OK;
	device.visibility_test_active = FALSE;
	index %= VISIBILITY_TEST_SLOTS;
	memset(&visibility, 0, sizeof(visibility));
	visibility.slot = index;
	xgpu_ring_send_data(_xgpu_op_visibility_end, &visibility, sizeof(visibility), NULL, 0);
	/* the target's samples to a game pixel: the result is a count of the
	game's pixels (as d3d8_gl.c) */
	device.query_area[index] = target_scale[0] * target_scale[1] * (float)target_samples;
	device.query_pending[index] = TRUE;
	return S_OK;
}

HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD index, UINT *result, ULONGLONG *time_stamp)
{
	struct xgpu_results *results = xgpu_host_results();
	unsigned long samples;
	float area;

	if (time_stamp)
		*time_stamp = 0;
	index %= VISIBILITY_TEST_SLOTS;
	if (!device.ready || !device.query_pending[index] || !results)
	{
		if (result)
			*result = 0;
		return S_OK;
	}
	samples = results->visibility_samples[index];
	area = device.query_area[index];
	if (result)
		*result = area > 1.0f ? (UINT)(samples / area + 0.5f) : (UINT)samples;
	return S_OK;
}

/* ---------- render and texture stage state (as d3d8_gl.c) */

void D3DFASTCALL D3DDevice_SetRenderState_Simple(DWORD method, DWORD value)
{
	(void)method;
	(void)value;
}

void D3DFASTCALL D3DDevice_SetRenderState_Deferred(D3DRENDERSTATETYPE state, DWORD value)
{
	if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value);

void WINAPI D3DDevice_SetRenderStateNotInline(D3DRENDERSTATETYPE state, DWORD value)
{
	if (state == D3DRS_ZBIAS)
		D3DDevice_SetRenderState_ZBias(value);
	else if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value)
{
	float offset = -(float)value;
	float slope = offset * 0.25f;
	DWORD enable = value != 0;

	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZSLOPESCALE], &slope, sizeof(slope));
	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZOFFSET], &offset, sizeof(offset));
	D3D__RenderState[D3DRS_POINTOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_WIREFRAMEOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_SOLIDOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_ZBIAS] = value;
}

#define COMPLEX_RENDER_STATE(name, state) \
	void WINAPI D3DDevice_SetRenderState_##name(DWORD value) { D3D__RenderState[state] = value; }

COMPLEX_RENDER_STATE(PSTextureModes, D3DRS_PSTEXTUREMODES)
COMPLEX_RENDER_STATE(VertexBlend, D3DRS_VERTEXBLEND)
COMPLEX_RENDER_STATE(FogColor, D3DRS_FOGCOLOR)
COMPLEX_RENDER_STATE(FillMode, D3DRS_FILLMODE)
COMPLEX_RENDER_STATE(BackFillMode, D3DRS_BACKFILLMODE)
COMPLEX_RENDER_STATE(TwoSidedLighting, D3DRS_TWOSIDEDLIGHTING)
COMPLEX_RENDER_STATE(NormalizeNormals, D3DRS_NORMALIZENORMALS)
COMPLEX_RENDER_STATE(ZEnable, D3DRS_ZENABLE)
COMPLEX_RENDER_STATE(StencilEnable, D3DRS_STENCILENABLE)
COMPLEX_RENDER_STATE(StencilFail, D3DRS_STENCILFAIL)
COMPLEX_RENDER_STATE(FrontFace, D3DRS_FRONTFACE)
COMPLEX_RENDER_STATE(CullMode, D3DRS_CULLMODE)
COMPLEX_RENDER_STATE(TextureFactor, D3DRS_TEXTUREFACTOR)
COMPLEX_RENDER_STATE(LogicOp, D3DRS_LOGICOP)
COMPLEX_RENDER_STATE(EdgeAntiAlias, D3DRS_EDGEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleAntiAlias, D3DRS_MULTISAMPLEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleMask, D3DRS_MULTISAMPLEMASK)
COMPLEX_RENDER_STATE(MultiSampleType, D3DRS_MULTISAMPLETYPE)
COMPLEX_RENDER_STATE(ShadowFunc, D3DRS_SHADOWFUNC)
COMPLEX_RENDER_STATE(LineWidth, D3DRS_LINEWIDTH)
COMPLEX_RENDER_STATE(Dxt1NoiseEnable, D3DRS_DXT1NOISEENABLE)
COMPLEX_RENDER_STATE(YuvEnable, D3DRS_YUVENABLE)
COMPLEX_RENDER_STATE(OcclusionCullEnable, D3DRS_OCCLUSIONCULLENABLE)
COMPLEX_RENDER_STATE(StencilCullEnable, D3DRS_STENCILCULLENABLE)
COMPLEX_RENDER_STATE(RopZCmpAlwaysRead, D3DRS_ROPZCMPALWAYSREAD)
COMPLEX_RENDER_STATE(RopZRead, D3DRS_ROPZREAD)
COMPLEX_RENDER_STATE(DoNotCullUncompressed, D3DRS_DONOTCULLUNCOMPRESSED)

void D3DFASTCALL D3DDevice_SetTextureState_Deferred(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTextureState_TexCoordIndex(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_TEXCOORDINDEX] = value;
}

void WINAPI D3DDevice_SetTextureState_BorderColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_BORDERCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_ColorKeyColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_COLORKEYCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_BumpEnv(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTexture(DWORD stage, D3DBaseTexture *texture)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.textures[stage] = texture;
}

void WINAPI D3DDevice_SetPalette(DWORD stage, D3DPalette *palette)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.palettes[stage] = palette;
}

void WINAPI D3DDevice_SetPixelShaderProgram(D3DPIXELSHADERDEF *definition)
{
	if (!definition)
		return;
	memcpy(&D3D__RenderState[D3DRS_PSALPHAINPUTS0], definition->PSAlphaInputs, sizeof(definition->PSAlphaInputs));
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSABCD] = definition->PSFinalCombinerInputsABCD;
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSEFG] = definition->PSFinalCombinerInputsEFG;
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT0_0], definition->PSConstant0, sizeof(definition->PSConstant0));
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT1_0], definition->PSConstant1, sizeof(definition->PSConstant1));
	memcpy(&D3D__RenderState[D3DRS_PSALPHAOUTPUTS0], definition->PSAlphaOutputs, sizeof(definition->PSAlphaOutputs));
	memcpy(&D3D__RenderState[D3DRS_PSRGBINPUTS0], definition->PSRGBInputs, sizeof(definition->PSRGBInputs));
	D3D__RenderState[D3DRS_PSCOMPAREMODE] = definition->PSCompareMode;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0] = definition->PSFinalCombinerConstant0;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1] = definition->PSFinalCombinerConstant1;
	memcpy(&D3D__RenderState[D3DRS_PSRGBOUTPUTS0], definition->PSRGBOutputs, sizeof(definition->PSRGBOutputs));
	D3D__RenderState[D3DRS_PSCOMBINERCOUNT] = definition->PSCombinerCount;
	D3D__RenderState[D3DRS_PSTEXTUREMODES] = definition->PSTextureModes;
	D3D__RenderState[D3DRS_PSDOTMAPPING] = definition->PSDotMapping;
	D3D__RenderState[D3DRS_PSINPUTTEXTURE] = definition->PSInputTexture;
}

/* ---------- vertex shaders (as d3d8_gl.c) */

static unsigned long vertex_type_bytes(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return 4;
	case D3DVSDT_FLOAT2: return 8;
	case D3DVSDT_FLOAT3: return 12;
	case D3DVSDT_FLOAT4: return 16;
	case D3DVSDT_D3DCOLOR: return 4;
	case D3DVSDT_SHORT1: return 2;
	case D3DVSDT_SHORT2: return 4;
	case D3DVSDT_SHORT3: return 6;
	case D3DVSDT_SHORT4: return 8;
	case D3DVSDT_NORMSHORT1: return 2;
	case D3DVSDT_NORMSHORT2: return 4;
	case D3DVSDT_NORMSHORT3: return 6;
	case D3DVSDT_NORMSHORT4: return 8;
	case D3DVSDT_NORMPACKED3: return 4;
	case D3DVSDT_PBYTE1: return 1;
	case D3DVSDT_PBYTE2: return 2;
	case D3DVSDT_PBYTE3: return 3;
	case D3DVSDT_PBYTE4: return 4;
	case D3DVSDT_FLOAT2H: return 12;
	default: return 0;
	}
}

static void parse_declaration(struct vertex_shader_object *object, const DWORD *declaration)
{
	unsigned long stream = 0;
	unsigned long offsets[16] = { 0 };

	for (; declaration && *declaration != D3DVSD_END(); declaration++)
	{
		DWORD token = *declaration;
		unsigned long token_type = (token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;

		switch (token_type)
		{
		case D3DVSD_TOKEN_STREAM:
			stream = token & D3DVSD_STREAMNUMBERMASK;
			break;
		case D3DVSD_TOKEN_STREAMDATA:
			if (token & D3DVSD_DATALOADTYPEMASK)
			{
				unsigned long count = (token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT;

				offsets[stream] += (token & 0x08000000) ? count : count * 4;
			}
			else if (object->element_count < XGPU_VERTEX_ATTRIBUTE_COUNT)
			{
				struct vertex_element *element = &object->elements[object->element_count++];

				element->reg = (unsigned char)(token & D3DVSD_VERTEXREGMASK);
				element->stream = (unsigned char)stream;
				element->type = (unsigned char)((token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT);
				element->bytes = (unsigned char)vertex_type_bytes(element->type);
				element->offset = (unsigned short)offsets[stream];
				offsets[stream] += element->bytes;
				if (element->type == D3DVSDT_NORMPACKED3)
					object->packed_mask |= 1UL << element->reg;
			}
			break;
		case D3DVSD_TOKEN_CONSTMEM:
			declaration += ((token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT) * 4;
			break;
		case D3DVSD_TOKEN_EXT:
			declaration += (token & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT;
			break;
		default:
			break;
		}
	}
}

HRESULT WINAPI D3DDevice_CreateVertexShader(CONST DWORD *declaration, CONST DWORD *function, DWORD *handle, DWORD usage)
{
	struct vertex_shader_object *object = calloc(1, sizeof(*object));

	(void)usage;
	if (!object)
		return E_OUTOFMEMORY;
	object->signature = VERTEX_SHADER_SIGNATURE;
	object->id = device.next_vertex_shader_id++;
	if (function)
	{
		object->instruction_count = function[0] >> 16;
		object->instructions = malloc(object->instruction_count * 4 * sizeof(DWORD));
		memcpy(object->instructions, function + 1, object->instruction_count * 4 * sizeof(DWORD));
	}
	parse_declaration(object, declaration);
	*handle = (DWORD)object;
	return S_OK;
}

static struct vertex_shader_object *vertex_shader_from_handle(DWORD handle)
{
	struct vertex_shader_object *object = (struct vertex_shader_object *)handle;

	if (!handle || (handle & 1) || object->signature != VERTEX_SHADER_SIGNATURE)
		return NULL;
	return object;
}

void halo_vertex_shader_lighting(unsigned long handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle((DWORD)handle);

	if (!object || !object->instructions)
		return;
	if (!nv2a_vertex_shader_lighting(object->instructions, object->instruction_count, &object->lighting))
	{
		memset(&object->lighting, 0, sizeof(object->lighting));
		platform_log("GPU: vertex shader %lu is not lit as the pixel shader would light it: lit for each vertex",
			object->id);
	}
}

void WINAPI D3DDevice_DeleteVertexShader(DWORD handle)
{
	(void)handle;
}

void WINAPI D3DDevice_SetVertexShader(DWORD handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
	{
		device.vertex_shader = object;
		device.program_address = 0;
		device.program_slots[0] = object;
	}
}

void WINAPI D3DDevice_LoadVertexShader(DWORD handle, DWORD address)
{
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_slots[address] = vertex_shader_from_handle(handle);
}

void WINAPI D3DDevice_SelectVertexShader(DWORD handle, DWORD address)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
		device.vertex_shader = object;
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_address = address;
}

void WINAPI D3DDevice_GetVertexShaderSize(DWORD handle, UINT *size)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	*size = object ? object->instruction_count : 0;
}

void WINAPI D3DDevice_SetVertexShaderConstant(INT reg, CONST void *constant_data, DWORD constant_count)
{
	long first = reg + XGPU_VERTEX_CONSTANT_BIAS;

	if (first < 0 || first >= XGPU_VERTEX_CONSTANT_COUNT)
		return;
	if (first + (long)constant_count > XGPU_VERTEX_CONSTANT_COUNT)
		constant_count = XGPU_VERTEX_CONSTANT_COUNT - first;
	constants_store((unsigned long)first, constant_data, constant_count);
}

static struct vertex_shader_object *current_program(void)
{
	struct vertex_shader_object *program = device.program_slots[device.program_address];

	return program ? program : device.vertex_shader;
}

/* ---------- shaders, as HLSL to the renderer */

static void shader_dump(const char *name, const char *text)
{
	char path[512];
	FILE *file;

	if (!debug_settings.dump_shaders)
		return;
	snprintf(path, sizeof(path), "%s/%s", debug_settings.dump_shaders, name);
	if ((file = fopen(path, "w")) != NULL)
	{
		fputs(text, file);
		fclose(file);
	}
}

/* sends a shader's HLSL; returns its number, 0 if it did not convert */
static unsigned long shader_send(unsigned long opcode, const char *glsl, BOOL pixel, const unsigned char *input_kinds,
	const char *name)
{
	char error[256];
	char *hlsl = xgpu_hlsl_from_glsl(glsl, pixel, input_kinds, error, sizeof(error));
	struct xgpu_shader shader;
	unsigned long length;

	if (!hlsl)
	{
		platform_log("GPU: the %s shader %s does not become HLSL: %s", pixel ? "pixel" : "vertex", name, error);
		return 0;
	}
	length = (unsigned long)strlen(hlsl);
	if (length > XGPU_MESSAGE_MAXIMUM)
	{
		platform_log("GPU: the %s shader %s is too long (%lu bytes)", pixel ? "pixel" : "vertex", name, length);
		free(hlsl);
		return 0;
	}
	memset(&shader, 0, sizeof(shader));
	shader.shader = device.next_shader_number++;
	if (input_kinds)
		memcpy(shader.input_kinds, input_kinds, sizeof(shader.input_kinds));
	xgpu_ring_send_data(opcode, &shader, sizeof(shader), hlsl, length);
	if (debug_settings.dump_shaders)
	{
		char file[64];

		snprintf(file, sizeof(file), "%s.hlsl", name);
		shader_dump(file, hlsl);
	}
	free(hlsl);
	return shader.shader;
}

/* how each attribute of the declaration reaches the shader (xgpu_hlsl.h) */
static void input_kinds_of(const struct vertex_shader_object *declaration, BOOL immediate, unsigned char *kinds)
{
	unsigned long index;

	memset(kinds, _xgpu_hlsl_input_float, XGPU_ATTRIBUTE_COUNT);
	if (immediate || !declaration)
		return;
	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];

		if (element->reg >= XGPU_ATTRIBUTE_COUNT || !device.streams[element->stream].data)
			continue;
		switch (element->type)
		{
		case D3DVSDT_SHORT1: case D3DVSDT_SHORT2: case D3DVSDT_SHORT4:
			kinds[element->reg] = _xgpu_hlsl_input_sint;
			break;
		case D3DVSDT_SHORT3:
			kinds[element->reg] = _xgpu_hlsl_input_sint_w1;
			break;
		case D3DVSDT_NORMSHORT3: case D3DVSDT_PBYTE3:
			kinds[element->reg] = _xgpu_hlsl_input_float_w1;
			break;
		default:
			break;
		}
	}
	for (index = 0; index < XGPU_ATTRIBUTE_COUNT; index++)
	{
		if (declaration->packed_mask & (1UL << index))
			kinds[index] = _xgpu_hlsl_input_uint;
	}
}

static unsigned long vertex_shader_get(struct vertex_shader_object *program, BOOL immediate, BOOL lit)
{
	unsigned char kinds[XGPU_ATTRIBUTE_COUNT];
	unsigned long packed_mask = immediate ? 0 : device.vertex_shader->packed_mask;
	struct vertex_variant *variant;

	input_kinds_of(immediate ? NULL : device.vertex_shader, immediate, kinds);
	for (variant = program->variants; variant; variant = variant->next)
	{
		if (variant->immediate == (immediate != 0) && variant->lit == (lit != 0) && variant->packed_mask == packed_mask &&
			!memcmp(variant->input_kinds, kinds, sizeof(kinds)))
		{
			return variant->shader;
		}
	}
	variant = calloc(1, sizeof(*variant));
	variant->immediate = immediate != 0;
	variant->lit = lit != 0;
	variant->packed_mask = packed_mask;
	memcpy(variant->input_kinds, kinds, sizeof(kinds));
	{
		char *glsl = nv2a_vertex_shader_to_glsl(program->instructions, program->instruction_count, packed_mask,
			lit ? &program->lighting : NULL);
		char name[64];

		snprintf(name, sizeof(name), "vs%03lu_%d%s_%lx", program->id, immediate ? 1 : 0, lit ? "_lit" : "",
			(unsigned long)(size_t)variant & 0xffff);
		variant->shader = shader_send(_xgpu_op_vertex_shader, glsl, FALSE, kinds, name);
		free(glsl);
	}
	variant->next = program->variants;
	program->variants = variant;
	return variant->shader;
}

static unsigned long hash_words(const void *data, unsigned long size)
{
	const DWORD *words = data;
	unsigned long hash = 2166136261UL;

	for (size /= 4; size; size--)
		hash = (hash ^ *words++) * 16777619UL;
	return hash;
}

struct fragment_entry
{
	struct fragment_entry *next;
	unsigned long hash;
	struct nv2a_pixel_shader_key key;
	unsigned long shader;
};

#define FRAGMENT_BUCKETS 1024

static struct fragment_entry *fragment_buckets[FRAGMENT_BUCKETS];

typedef char pixel_shader_key_size_assert[sizeof(struct nv2a_pixel_shader_key) % 4 == 0 ? 1 : -1];

static unsigned long pixel_shader_get(const struct nv2a_pixel_shader_key *key)
{
#define RECENT_FRAGMENT_COUNT 4
	static struct fragment_entry *recent[RECENT_FRAGMENT_COUNT];
	static unsigned long recent_next;
	unsigned long hash, index;
	struct fragment_entry **bucket;
	struct fragment_entry *entry;

	for (index = 0; index < RECENT_FRAGMENT_COUNT; index++)
	{
		if (recent[index] && !memcmp(&recent[index]->key, key, sizeof(*key)))
			return recent[index]->shader;
	}
	hash = hash_words(key, sizeof(*key));
	bucket = &fragment_buckets[hash % FRAGMENT_BUCKETS];
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->hash == hash && !memcmp(&entry->key, key, sizeof(*key)))
		{
			recent[recent_next++ % RECENT_FRAGMENT_COUNT] = entry;
			return entry->shader;
		}
	}
	entry = calloc(1, sizeof(*entry));
	entry->hash = hash;
	entry->key = *key;
	{
		char *glsl = nv2a_pixel_shader_to_glsl(key);
		char name[32];

		snprintf(name, sizeof(name), "ps_%08lx", hash);
		entry->shader = shader_send(_xgpu_op_pixel_shader, glsl, TRUE, NULL, name);
		free(glsl);
	}
	entry->next = *bucket;
	*bucket = entry;
	recent[recent_next++ % RECENT_FRAGMENT_COUNT] = entry;
	return entry->shader;
}

/* ---------- per-draw state */

static unsigned long stage_texture_mode(int stage)
{
	return (D3D__RenderState[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f;
}

static void sampler_state(int stage, BOOL mipmapped, BOOL hires, struct xgpu_sampler_state *sampler)
{
	DWORD *state = D3D__TextureState[stage];

	memset(sampler, 0, sizeof(*sampler));
	sampler->min_filter = (uint8_t)(hires ? D3DTEXF_LINEAR : state[D3DTSS_MINFILTER]);
	sampler->mip_filter = (uint8_t)(hires ? D3DTEXF_LINEAR : mipmapped ? state[D3DTSS_MIPFILTER] : D3DTEXF_NONE);
	sampler->mag_filter = (uint8_t)(hires ? D3DTEXF_LINEAR : state[D3DTSS_MAGFILTER]);
	sampler->address_u = (uint8_t)state[D3DTSS_ADDRESSU];
	sampler->address_v = (uint8_t)state[D3DTSS_ADDRESSV];
	sampler->address_w = (uint8_t)state[D3DTSS_ADDRESSW];
	sampler->max_anisotropy = (uint8_t)(state[D3DTSS_MAXANISOTROPY] > 16 ? 16 : state[D3DTSS_MAXANISOTROPY]);
	sampler->lod_bias = hires ? 0.0f : dword_to_float(state[D3DTSS_MIPMAPLODBIAS]);
	sampler->min_lod = hires ? 0.0f : (float)state[D3DTSS_MAXMIPLEVEL];
	sampler->border_color = state[D3DTSS_BORDERCOLOR];
}

/* ---------- render targets sampled with their mip chain (as d3d8_gl.c:
a texture of the levels the game drew, copied from their targets, and the
others made from them) */

#define MIP_COMPOSITE_LEVELS 16

struct mip_composite
{
	struct mip_composite *next;
	unsigned long data, width, height, levels;
	unsigned long texture;
	unsigned long rendered_levels;
	unsigned long level_sources[MIP_COMPOSITE_LEVELS];
	unsigned long level_written[MIP_COMPOSITE_LEVELS];
};

static struct mip_composite *mip_composites;

static BOOL render_target_stale(const struct render_target_entry *entry)
{
	switch (entry->kind)
	{
	case _render_target_screen:
		return entry->target.scale[0] != screen_scale[0] || entry->target.scale[1] != screen_scale[1] ||
			(entry->target.height == SCREEN_HEIGHT && entry->target.width != (unsigned long)halo_screen_width());
	case _render_target_shadow_map:
		return entry->target.scale[0] != (float)halo_shadow_map_scale();
	default:
		return FALSE;
	}
}

static void render_target_delete(struct render_target_entry *entry)
{
	struct render_target_entry **link;
	struct mip_composite *composite;
	struct xgpu_target_reference reference;

	for (link = render_target_bucket(entry->target.data); *link != entry; link = &(*link)->next_in_bucket)
		;
	*link = entry->next_in_bucket;
	for (composite = mip_composites; composite; composite = composite->next)
	{
		unsigned long level;

		for (level = 0; level < composite->rendered_levels; level++)
		{
			if (composite->level_sources[level] == entry->target.texture)
			{
				composite->rendered_levels = 0;
				break;
			}
		}
	}
	memset(&reference, 0, sizeof(reference));
	reference.target = entry->target.texture;
	xgpu_ring_send_data(_xgpu_op_target_delete, &reference, sizeof(reference), NULL, 0);
	free(entry);
}

static void render_targets_evict(void)
{
	struct render_target_entry *entry, **link = &render_targets;
	BOOL any = FALSE;

	while ((entry = *link) != NULL)
	{
		if (render_target_stale(entry))
		{
			*link = entry->next;
			render_target_delete(entry);
			any = TRUE;
		}
		else
		{
			link = &entry->next;
		}
	}
	if (any)
		memset(recent_render_targets, 0, sizeof(recent_render_targets));
}

static unsigned long mip_composite_get(const struct xgpu_texture_description *description, unsigned long data)
{
	struct mip_composite *composite;
	struct xgpu_render_target *targets[MIP_COMPOSITE_LEVELS];
	unsigned long level, rendered_levels = 0;
	BOOL changed;

	for (composite = mip_composites; composite; composite = composite->next)
	{
		if (composite->data == data && composite->width == description->width &&
			composite->height == description->height && composite->levels == description->levels)
		{
			break;
		}
	}
	if (!composite)
	{
		struct xgpu_texture_define definition;

		composite = calloc(1, sizeof(*composite));
		composite->data = data;
		composite->width = description->width;
		composite->height = description->height;
		composite->levels = description->levels;
		composite->texture = xgpu_texture_number_new();
		memset(&definition, 0, sizeof(definition));
		definition.texture = composite->texture;
		definition.dimension = _xgpu_texture_2d;
		definition.format = _xgpu_format_bgra8;
		definition.width = description->width;
		definition.height = description->height;
		definition.depth = 1;
		definition.levels = description->levels;
		definition.swizzle[0] = 0;
		definition.swizzle[1] = 1;
		definition.swizzle[2] = 2;
		definition.swizzle[3] = 3;
		definition.flags = XGPU_TEXTURE_RENDERABLE;
		xgpu_ring_send_data(_xgpu_op_texture_define, &definition, sizeof(definition), NULL, 0);
		composite->rendered_levels = ~0UL;
		composite->next = mip_composites;
		mip_composites = composite;
	}
	for (level = 0; level < description->levels && level < MIP_COMPOSITE_LEVELS; level++)
	{
		unsigned long width = description->width >> level ? description->width >> level : 1;
		unsigned long height = description->height >> level ? description->height >> level : 1;
		struct xgpu_render_target *target =
			xgpu_render_target_find(data + xgpu_texture_level_offset(description, level));

		if (!target || target->width != width || target->height != height ||
			target->gl_width != width || target->gl_height != height)
			break;
		targets[level] = target;
		rendered_levels++;
	}
	changed = rendered_levels != composite->rendered_levels;
	for (level = 0; level < rendered_levels && !changed; level++)
	{
		changed = targets[level]->texture != composite->level_sources[level] ||
			targets[level]->written != composite->level_written[level];
	}
	if (!changed)
		return composite->texture;
	composite->rendered_levels = rendered_levels;
	for (level = 0; level < rendered_levels; level++)
	{
		struct xgpu_target_copy copy;

		composite->level_sources[level] = targets[level]->texture;
		composite->level_written[level] = targets[level]->written;
		render_target_resolve(targets[level]);
		memset(&copy, 0, sizeof(copy));
		copy.target = targets[level]->texture;
		copy.texture = composite->texture;
		copy.level = level;
		xgpu_ring_send_data(_xgpu_op_target_copy, &copy, sizeof(copy), NULL, 0);
	}
	if (rendered_levels < description->levels)
	{
		struct xgpu_texture_reference reference;

		memset(&reference, 0, sizeof(reference));
		reference.texture = composite->texture;
		reference.base_level = rendered_levels ? rendered_levels - 1 : 0;
		xgpu_ring_send_data(_xgpu_op_texture_mipmaps, &reference, sizeof(reference), NULL, 0);
	}
	return composite->texture;
}

/* each stage's texture (a renderer texture, or a target with
XGPU_BINDING_TARGET) and sampler into the state, as d3d8_gl.c's
bind_textures chooses them */
static void bind_textures(struct nv2a_pixel_shader_key *key, float texture_scale[4][4], struct xgpu_draw_state *state)
{
	int stage;

	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		D3DBaseTexture *texture = device.textures[stage];
		unsigned long mode = stage_texture_mode(stage);
		struct xgpu_texture_description description;
		uint32_t binding;
		GLenum gl_target = GL_TEXTURE_2D;

		texture_scale[stage][0] = texture_scale[stage][1] = 1.0f;
		texture_scale[stage][2] = texture_scale[stage][3] = 1.0f;
		sampler_state(stage, FALSE, FALSE, &state->samplers[stage]);
		if (!texture || !texture->Data || mode == 0 || mode == 0x04 || mode == 0x05 || mode == 0x11)
		{
			state->textures[stage] = 0;
			key->sampler_type[stage] = mode == 0x11 ? _xgpu_sampler_2d : _xgpu_sampler_none;
			continue;
		}
		{
			struct xgpu_render_target *target = xgpu_render_target_find(texture->Data);

			if (target)
			{
				render_target_resolve(target);
				xgpu_texture_describe(texture->Format, texture->Size, &description);
				binding = target->texture | XGPU_BINDING_TARGET;
				if (description.linear)
				{
					texture_scale[stage][0] = 1.0f / (float)target->width;
					texture_scale[stage][1] = 1.0f / (float)target->height;
				}
				if (!description.linear && !description.cube_map && description.levels > 1 &&
					target->width == description.width && target->height == description.height)
					binding = (uint32_t)mip_composite_get(&description, texture->Data);
				else
					description.levels = 1;
			}
			else
			{
				const D3DCOLOR *palette = device.palettes[stage] && device.palettes[stage]->Data ?
					(const D3DCOLOR *)PLATFORM_PHYSICAL_TO_VIRTUAL(device.palettes[stage]->Data) : NULL;

				binding = (uint32_t)xgpu_texture_get((const DWORD *)texture, palette, &gl_target, &description);
				if (description.linear)
				{
					texture_scale[stage][0] = 1.0f / (float)description.width;
					texture_scale[stage][1] = 1.0f / (float)description.height;
				}
			}
			state->textures[stage] = binding;
			sampler_state(stage, description.levels > 1, description.hires, &state->samplers[stage]);
			if (stage == 0)
			{
				key->coverage_alpha = description.hires_coverage != FALSE;
				key->point_threshold = description.hires_point_threshold != FALSE;
			}
			key->sampler_type[stage] = gl_target == GL_TEXTURE_CUBE_MAP ? _xgpu_sampler_cube :
				gl_target == GL_TEXTURE_3D ? _xgpu_sampler_3d : _xgpu_sampler_2d;
		}
	}
}

/* the Xbox's blend operations, as OpenGL's equations (the renderer's terms) */
static uint32_t blend_equation(DWORD operation)
{
	switch (operation)
	{
	case D3DBLENDOP_SUBTRACT: return 0x800a;
	case D3DBLENDOP_REVSUBTRACT:
	case D3DBLENDOP_REVSUBTRACTSIGNED: return 0x800b;
	case D3DBLENDOP_MIN: return 0x8007;
	case D3DBLENDOP_MAX: return 0x8008;
	default: return 0x8006;
	}
}

/* the raster state, as d3d8_gl.c's apply_raster_state sets it in GL */
static void raster_state(struct render_target_entry *color, struct render_target_entry *depth,
	struct xgpu_draw_state *state)
{
	DWORD *rs = D3D__RenderState;
	DWORD write = rs[D3DRS_COLORWRITEENABLE];
	BOOL has_depth = depth != NULL;
	long x0 = target_pixel((float)device.viewport.X, 0);
	long y0 = target_pixel((float)device.viewport.Y, 1);
	long x1 = target_pixel((float)(device.viewport.X + device.viewport.Width), 0);
	long y1 = target_pixel((float)(device.viewport.Y + device.viewport.Height), 1);
	struct render_target_entry *target = color ? color : depth;

	state->color_target = color ? color->target.texture : 0;
	state->depth_target = depth ? depth->target.texture : 0;
	state->viewport[0] = (float)x0;
	state->viewport[1] = (float)y0;
	state->viewport[2] = (float)(x1 - x0);
	state->viewport[3] = (float)(y1 - y0);
	state->depth_range[0] = device.viewport.MinZ;
	state->depth_range[1] = device.viewport.MaxZ;
	/* the NV2A's scissor follows the viewport (as d3d8_gl.c); one of no
	size is the whole target */
	if (x1 > x0 && y1 > y0)
	{
		state->scissor[0] = x0 < 0 ? 0 : x0;
		state->scissor[1] = y0 < 0 ? 0 : y0;
		state->scissor[2] = x1;
		state->scissor[3] = y1;
	}
	else
	{
		state->scissor[0] = state->scissor[1] = 0;
		state->scissor[2] = (int32_t)target->target.gl_width;
		state->scissor[3] = (int32_t)target->target.gl_height;
	}

	state->depth_enable = has_depth && rs[D3DRS_ZENABLE];
	state->depth_function = state->depth_enable ? (rs[D3DRS_ZFUNC] ? rs[D3DRS_ZFUNC] : 0x200) : 0;
	state->depth_write = state->depth_enable && rs[D3DRS_ZWRITEENABLE];
	state->stencil_enable = has_depth && rs[D3DRS_STENCILENABLE];
	if (state->stencil_enable)
	{
		state->stencil_function = rs[D3DRS_STENCILFUNC] ? rs[D3DRS_STENCILFUNC] : 0x200;
		state->stencil_reference = rs[D3DRS_STENCILREF];
		state->stencil_read_mask = rs[D3DRS_STENCILMASK];
		state->stencil_write_mask = rs[D3DRS_STENCILWRITEMASK];
		state->stencil_fail = rs[D3DRS_STENCILFAIL];
		state->stencil_depth_fail = rs[D3DRS_STENCILZFAIL];
		state->stencil_pass = rs[D3DRS_STENCILPASS];
	}
	state->blend_enable = rs[D3DRS_ALPHABLENDENABLE] != 0;
	if (state->blend_enable)
	{
		state->blend_source = rs[D3DRS_SRCBLEND];
		state->blend_destination = rs[D3DRS_DESTBLEND];
		state->blend_operation = blend_equation(rs[D3DRS_BLENDOP]);
		state->blend_color = rs[D3DRS_BLENDCOLOR];
	}
	state->color_write_mask = (uint8_t)(((write & D3DCOLORWRITEENABLE_RED) ? 1 : 0) |
		((write & D3DCOLORWRITEENABLE_GREEN) ? 2 : 0) | ((write & D3DCOLORWRITEENABLE_BLUE) ? 4 : 0) |
		((write & D3DCOLORWRITEENABLE_ALPHA) ? 8 : 0));
	/* the cull mode names the winding to discard; FRONTFACE the front's */
	if (rs[D3DRS_CULLMODE] != D3DCULL_NONE)
	{
		state->front_counter_clockwise = rs[D3DRS_FRONTFACE] == D3DFRONT_CCW;
		state->cull = rs[D3DRS_CULLMODE] == rs[D3DRS_FRONTFACE] ? 1 : 2;
	}
	state->fill = rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME ? 1 : rs[D3DRS_FILLMODE] == D3DFILL_POINT ? 2 : 0;
	state->depth_bias_enable = rs[D3DRS_SOLIDOFFSETENABLE] != 0;
	if (state->depth_bias_enable)
	{
		state->depth_bias_slope = dword_to_float(rs[D3DRS_POLYGONOFFSETZSLOPESCALE]);
		state->depth_bias = dword_to_float(rs[D3DRS_POLYGONOFFSETZOFFSET]);
	}
	state->samples = (uint8_t)(target_samples > 1 ? target_samples : 1);
}

static BOOL per_pixel_lighting(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static BOOL enabled;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		enabled = config_boolean("display.per_pixel_lighting") != 0;
	}
	return enabled;
}

static unsigned long model_light_register(int light)
{
	return (unsigned long)(XGPU_VERTEX_CONSTANT_BIAS + (light ? -80 + light : -82));
}

/* the vertex constants that changed, the other uniforms, the model lights */
static void uniforms_send(const float texture_scale[4][4], BOOL lit)
{
	struct xgpu_hlsl_draw_uniforms uniforms;
	int stage;

	if (device.constants_first <= device.constants_last)
	{
		struct xgpu_vertex_constants constants;

		memset(&constants, 0, sizeof(constants));
		constants.first = device.constants_first;
		constants.count = device.constants_last - device.constants_first + 1;
		xgpu_ring_send_data(_xgpu_op_vertex_constants, &constants, sizeof(constants),
			device.constants[device.constants_first], constants.count * 16);
		device.constants_first = XGPU_VERTEX_CONSTANT_COUNT;
		device.constants_last = 0;
	}
	memset(&uniforms, 0, sizeof(uniforms));
	memcpy(uniforms.viewport_scale, device.viewport_scale, sizeof(uniforms.viewport_scale));
	memcpy(uniforms.viewport_offset, device.viewport_offset, sizeof(uniforms.viewport_offset));
	memcpy(uniforms.texture_scale, texture_scale, sizeof(uniforms.texture_scale));
	for (stage = 0; stage < 8; stage++)
	{
		color_to_vec4(D3D__RenderState[D3DRS_PSCONSTANT0_0 + stage], uniforms.ps_c0[stage]);
		color_to_vec4(D3D__RenderState[D3DRS_PSCONSTANT1_0 + stage], uniforms.ps_c1[stage]);
	}
	color_to_vec4(D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0], uniforms.ps_final_c0);
	color_to_vec4(D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1], uniforms.ps_final_c1);
	color_to_vec4(D3D__RenderState[D3DRS_FOGCOLOR], uniforms.fog_color);
	uniforms.fog_parameters[0] = dword_to_float(D3D__RenderState[D3DRS_FOGSTART]);
	uniforms.fog_parameters[1] = dword_to_float(D3D__RenderState[D3DRS_FOGEND]);
	uniforms.fog_parameters[2] = dword_to_float(D3D__RenderState[D3DRS_FOGDENSITY]);
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		DWORD *state = D3D__TextureState[stage];

		uniforms.bump_matrix[stage][0] = dword_to_float(state[D3DTSS_BUMPENVMAT00]);
		uniforms.bump_matrix[stage][1] = dword_to_float(state[D3DTSS_BUMPENVMAT01]);
		uniforms.bump_matrix[stage][2] = dword_to_float(state[D3DTSS_BUMPENVMAT10]);
		uniforms.bump_matrix[stage][3] = dword_to_float(state[D3DTSS_BUMPENVMAT11]);
		uniforms.bump_luminance[stage][0] = dword_to_float(state[D3DTSS_BUMPENVLSCALE]);
		uniforms.bump_luminance[stage][1] = dword_to_float(state[D3DTSS_BUMPENVLOFFSET]);
		uniforms.texture_lod_bias[stage] = dword_to_float(state[D3DTSS_MIPMAPLODBIAS]);
	}
	uniforms.misc[0] = D3D__RenderState[D3DRS_POINTSIZE] ? dword_to_float(D3D__RenderState[D3DRS_POINTSIZE]) : 1.0f;
	uniforms.misc[1] = (float)UI_OFFSET;
	uniforms.misc[2] = (float)(D3D__RenderState[D3DRS_ALPHAREF] & 0xff);
	if (!sent_uniforms_valid || memcmp(&uniforms, &sent_uniforms, sizeof(uniforms)))
	{
		sent_uniforms = uniforms;
		sent_uniforms_valid = TRUE;
		xgpu_ring_send_data(_xgpu_op_draw_uniforms, &uniforms, sizeof(uniforms), NULL, 0);
	}
	if (lit)
	{
		float lights[XGPU_MODEL_LIGHT_COUNT][4];
		int light;

		for (light = 0; light < XGPU_MODEL_LIGHT_COUNT; light++)
			memcpy(lights[light], device.constants[model_light_register(light)], sizeof(lights[0]));
		if (!sent_lights_valid || memcmp(lights, sent_lights, sizeof(lights)))
		{
			memcpy(sent_lights, lights, sizeof(lights));
			sent_lights_valid = TRUE;
			xgpu_ring_send_data(_xgpu_op_model_lights, lights, sizeof(lights), NULL, 0);
		}
	}
}

/* the state of a draw (its vertex layout and topology to be filled by the
caller) gathered and its uniforms sent; FALSE if it cannot be drawn */
static BOOL prepare_draw(BOOL immediate, struct xgpu_draw_state *state)
{
	struct vertex_shader_object *program = current_program();
	struct nv2a_pixel_shader_key key;
	struct render_target_entry *color, *depth;
	float texture_scale[4][4];
	int stage;
	BOOL lit = FALSE;

	if (!device.ready || !program || !device.vertex_shader || !program->instructions)
	{
		stats.skipped_no_program++;
		return FALSE;
	}
	{
		const char *skip = debug_settings.skip_vertex_shaders;

		while (skip && *skip)
		{
			if ((unsigned long)atol(skip) == program->id)
				return FALSE;
			skip = strchr(skip, ',');
			if (skip)
				skip++;
		}
	}
	memset(state, 0, sizeof(*state));
	memset(&key, 0, sizeof(key));
	memcpy(key.combiner_state, D3D__RenderState, sizeof(key.combiner_state));
	memset(&key.combiner_state[D3DRS_PSCONSTANT0_0], 0, 16 * sizeof(DWORD));
	key.combiner_state[D3DRS_PSFINALCOMBINERCONSTANT0] = 0;
	key.combiner_state[D3DRS_PSFINALCOMBINERCONSTANT1] = 0;
	key.texture_modes = D3D__RenderState[D3DRS_PSTEXTUREMODES];
	bind_textures(&key, texture_scale, state);
	if (!bind_targets(&color, &depth))
	{
		stats.skipped_no_target++;
		return FALSE;
	}
	raster_state(color, depth, state);
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		key.alpha_kill[stage] = D3D__TextureState[stage][D3DTSS_ALPHAKILL] == D3DTALPHAKILL_ENABLE;
		key.color_sign[stage] = (unsigned char)((D3D__TextureState[stage][D3DTSS_COLORSIGN] >> 28) & 0xf);
	}
	key.coverage_alpha = key.coverage_alpha && D3D__RenderState[D3DRS_ALPHABLENDENABLE] &&
		D3D__RenderState[D3DRS_SRCBLEND] == D3DBLEND_CONSTANTCOLOR &&
		D3D__RenderState[D3DRS_DESTBLEND] == D3DBLEND_SRCALPHA;
	key.point_threshold = key.point_threshold && key.coverage_alpha;
	key.alpha_test_function = D3D__RenderState[D3DRS_ALPHATESTENABLE] ? D3D__RenderState[D3DRS_ALPHAFUNC] : 0;
	if (target_samples > 1 && key.alpha_test_function && !D3D__RenderState[D3DRS_ALPHABLENDENABLE])
		key.alpha_test_samples = (unsigned char)target_samples;
	key.fog_enable = D3D__RenderState[D3DRS_FOGENABLE] != 0;
	key.fog_table_mode = (unsigned char)D3D__RenderState[D3DRS_FOGTABLEMODE];

	if (program->lighting.lights && !program->lighting_failed && per_pixel_lighting())
	{
		key.per_pixel_lighting = (unsigned char)program->lighting.lights;
		state->vertex_shader = vertex_shader_get(program, immediate, TRUE);
		state->pixel_shader = pixel_shader_get(&key);
		if (!state->vertex_shader || !state->pixel_shader)
		{
			platform_log("GPU: vertex shader %lu cannot be lit for each pixel here: lit for each vertex", program->id);
			program->lighting_failed = TRUE;
			key.per_pixel_lighting = 0;
		}
		else
		{
			lit = TRUE;
		}
	}
	if (!lit)
	{
		state->vertex_shader = vertex_shader_get(program, immediate, FALSE);
		state->pixel_shader = pixel_shader_get(&key);
	}
	if (!state->vertex_shader || !state->pixel_shader)
	{
		stats.skipped_no_program++;
		return FALSE;
	}
	uniforms_send((const float (*)[4])texture_scale, lit);
	return TRUE;
}

/* sends the state if it differs from the one sent last */
static void state_send(const struct xgpu_draw_state *state)
{
	if (sent_state_valid && !memcmp(state, &sent_state, sizeof(*state)))
		return;
	sent_state = *state;
	sent_state_valid = TRUE;
	stats.state_changes++;
	xgpu_ring_send_data(_xgpu_op_draw_state, state, sizeof(*state), NULL, 0);
}

/* ---------- the contiguous window, mirrored in the renderer (as d3d8_gl.c's
mirror, its pages sent as messages rather than uploaded to GL buffers) */

#define MIRROR_SEGMENT_SIZE XGPU_MIRROR_SEGMENT_SIZE
#define MIRROR_SEGMENT_COUNT (PLATFORM_CONTIGUOUS_SIZE / MIRROR_SEGMENT_SIZE)
#define MIRROR_PAGE_SIZE 0x1000UL
#define MIRROR_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / MIRROR_PAGE_SIZE)
#define MIRROR_REWRITE_FRAMES 2
#define MIRROR_VOLATILE_REWRITES 4
#define MIRROR_VOLATILE_FRAMES 600

typedef char mirror_segment_count_assert[MIRROR_SEGMENT_COUNT <= XGPU_MIRROR_SEGMENT_LIMIT ? 1 : -1];

enum
{
	_mirror_page_absent,
	_mirror_page_present,
	_mirror_page_volatile
};

static struct
{
	unsigned char state[MIRROR_PAGE_COUNT];
	unsigned char rewrites[MIRROR_PAGE_COUNT];
	unsigned long generation[MIRROR_PAGE_COUNT];
	unsigned long rewritten_frame[MIRROR_PAGE_COUNT];
} mirror;

/* bytes of the contiguous window to the renderer's segment, in messages */
static void mirror_send(unsigned long address, unsigned long size)
{
	while (size)
	{
		unsigned long start = address - PLATFORM_CONTIGUOUS_BASE;
		unsigned long segment = start / MIRROR_SEGMENT_SIZE;
		unsigned long offset = start - segment * MIRROR_SEGMENT_SIZE;
		unsigned long piece = size < XGPU_MESSAGE_MAXIMUM ? size : XGPU_MESSAGE_MAXIMUM;
		struct xgpu_mirror_write write;

		if (offset + piece > MIRROR_SEGMENT_SIZE)
			piece = MIRROR_SEGMENT_SIZE - offset;
		memset(&write, 0, sizeof(write));
		write.segment = segment;
		write.offset = offset;
		xgpu_ring_send_data(_xgpu_op_mirror_write, &write, sizeof(write), (const void *)address, piece);
		address += piece;
		size -= piece;
	}
}

static BOOL mirror_refresh(unsigned long first, unsigned long last)
{
	unsigned long page, run;
	BOOL volatile_page = FALSE;
	unsigned char stale[256];
	unsigned long count = last - first;

	if (count > sizeof(stale))
	{
		for (page = first; page < last; page += sizeof(stale))
		{
			if (!mirror_refresh(page, page + sizeof(stale) < last ? page + sizeof(stale) : last))
				return FALSE;
		}
		return TRUE;
	}
	for (page = first; page < last; page++)
	{
		BOOL written = mirror.state[page] == _mirror_page_present &&
			memory_watch_generation(PLATFORM_CONTIGUOUS_BASE + page * MIRROR_PAGE_SIZE, MIRROR_PAGE_SIZE) >
			mirror.generation[page];

		stale[page - first] = mirror.state[page] != _mirror_page_present || written;
		if (written)
		{
			if (device.frame - mirror.rewritten_frame[page] <= MIRROR_REWRITE_FRAMES)
				mirror.rewrites[page]++;
			else
				mirror.rewrites[page] = 1;
			mirror.rewritten_frame[page] = device.frame;
			if (mirror.rewrites[page] >= MIRROR_VOLATILE_REWRITES)
			{
				mirror.state[page] = _mirror_page_volatile;
				volatile_page = TRUE;
			}
		}
	}
	if (volatile_page)
		return FALSE;
	for (page = first; page < last; page = run)
	{
		unsigned long address, size;

		if (!stale[page - first])
		{
			run = page + 1;
			continue;
		}
		for (run = page; run < last && stale[run - first]; run++)
		{
			if (mirror.state[run] != _mirror_page_present)
			{
				mirror.rewrites[run] = 0;
				mirror.rewritten_frame[run] = device.frame;
			}
		}
		address = PLATFORM_CONTIGUOUS_BASE + page * MIRROR_PAGE_SIZE;
		size = (run - page) * MIRROR_PAGE_SIZE;
		/* protect first, so a write racing with the copy is noticed */
		memory_watch_protect(address, size);
		for (; page < run; page++)
		{
			mirror.generation[page] = memory_watch_generation(PLATFORM_CONTIGUOUS_BASE + page * MIRROR_PAGE_SIZE,
				MIRROR_PAGE_SIZE);
			mirror.state[page] = _mirror_page_present;
		}
		mirror_send(address, size);
	}
	return TRUE;
}

/* makes [address, address + size) current in the renderer's mirror, giving
its segment and offset there and the newest generation of its pages; FALSE
if the range is outside the window, spans two segments or is volatile */
static BOOL mirror_range(unsigned long address, unsigned long size, unsigned long *segment_out, unsigned long *offset,
	unsigned long *generation)
{
	unsigned long start = address - PLATFORM_CONTIGUOUS_BASE;
	unsigned long segment, first, last, page, oldest = ~0UL, newest = 0;
	BOOL present = TRUE;

	if (!size || address < PLATFORM_CONTIGUOUS_BASE || start + size > PLATFORM_CONTIGUOUS_SIZE)
		return FALSE;
	segment = start / MIRROR_SEGMENT_SIZE;
	if ((start + size - 1) / MIRROR_SEGMENT_SIZE != segment)
		return FALSE;
	first = start / MIRROR_PAGE_SIZE;
	last = (start + size - 1) / MIRROR_PAGE_SIZE + 1;
	for (page = first; page < last; page++)
	{
		if (mirror.state[page] == _mirror_page_volatile)
		{
			if (device.frame - mirror.rewritten_frame[page] < MIRROR_VOLATILE_FRAMES)
				return FALSE;
			mirror.state[page] = _mirror_page_absent;
		}
		if (mirror.state[page] != _mirror_page_present)
		{
			present = FALSE;
		}
		else
		{
			if (mirror.generation[page] < oldest)
				oldest = mirror.generation[page];
			if (mirror.generation[page] > newest)
				newest = mirror.generation[page];
		}
	}
	if (!present || memory_watch_generation(address, size) > oldest)
	{
		if (!mirror_refresh(first, last))
			return FALSE;
		for (newest = 0, page = first; page < last; page++)
		{
			if (mirror.generation[page] > newest)
				newest = mirror.generation[page];
		}
	}
	*segment_out = segment;
	*offset = start - segment * MIRROR_SEGMENT_SIZE;
	if (generation)
		*generation = newest;
	stats.mirrored_bytes += size;
	return TRUE;
}

#define INDEX_RANGE_SLOTS 4096

static struct
{
	unsigned long address;
	unsigned long count;
	unsigned long generation;
	WORD minimum;
	WORD maximum;
} index_ranges[INDEX_RANGE_SLOTS];

static void index_extent(const WORD *indices, unsigned long count, unsigned long generation, BOOL cached,
	unsigned long *minimum, unsigned long *maximum)
{
	unsigned long slot = (((unsigned long)indices >> 1) ^ (count * 2654435761UL)) % INDEX_RANGE_SLOTS;
	unsigned long index, low = 0xffff, high = 0;

	if (cached && index_ranges[slot].address == (unsigned long)indices && index_ranges[slot].count == count &&
		index_ranges[slot].generation == generation)
	{
		*minimum = index_ranges[slot].minimum;
		*maximum = index_ranges[slot].maximum;
		return;
	}
	for (index = 0; index < count; index++)
	{
		if (indices[index] < low)
			low = indices[index];
		if (indices[index] > high)
			high = indices[index];
	}
	if (cached)
	{
		index_ranges[slot].address = (unsigned long)indices;
		index_ranges[slot].count = count;
		index_ranges[slot].generation = generation;
		index_ranges[slot].minimum = (WORD)low;
		index_ranges[slot].maximum = (WORD)high;
	}
	*minimum = low;
	*maximum = high;
}

/* ---------- a draw's message: struct xgpu_draw, then its own bytes */

static struct
{
	unsigned char *bytes;
	unsigned long size, capacity;
	BOOL overflowed;
} draw_inline;

static unsigned long inline_append(const void *data, unsigned long size)
{
	unsigned long offset = draw_inline.size;
	unsigned long room = (size + 15) & ~15UL;

	if (draw_inline.size + room > XGPU_MESSAGE_MAXIMUM)
	{
		draw_inline.overflowed = TRUE;
		return 0;
	}
	if (draw_inline.size + room > draw_inline.capacity)
	{
		unsigned long capacity = draw_inline.capacity ? draw_inline.capacity : 65536;
		unsigned char *grown;

		while (capacity < draw_inline.size + room)
			capacity *= 2;
		grown = realloc(draw_inline.bytes, capacity);
		if (!grown)
		{
			draw_inline.overflowed = TRUE;
			return 0;
		}
		draw_inline.bytes = grown;
		draw_inline.capacity = capacity;
	}
	memcpy(draw_inline.bytes + offset, data, size);
	memset(draw_inline.bytes + offset + size, 0, room - size);
	draw_inline.size += room;
	stats.streamed_bytes += size;
	return offset;
}

static void draw_send(struct xgpu_draw *draw)
{
	if (draw_inline.overflowed)
	{
		static BOOL logged;

		stats.skipped_large++;
		if (!logged)
		{
			logged = TRUE;
			platform_log("GPU: a draw's data is larger than a message; such draws are not drawn");
		}
		return;
	}
	draw->inline_size = draw_inline.size;
	xgpu_ring_send_data(_xgpu_op_draw, draw, sizeof(*draw), draw_inline.bytes, draw_inline.size);
}

static void draw_begin(struct xgpu_draw *draw)
{
	memset(draw, 0, sizeof(*draw));
	draw_inline.size = 0;
	draw_inline.overflowed = FALSE;
}

/* ---------- vertex data */

static uint8_t vertex_format(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return _xgpu_vertex_float1;
	case D3DVSDT_FLOAT2: return _xgpu_vertex_float2;
	case D3DVSDT_FLOAT3: case D3DVSDT_FLOAT2H: return _xgpu_vertex_float3;
	case D3DVSDT_FLOAT4: return _xgpu_vertex_float4;
	case D3DVSDT_D3DCOLOR: return _xgpu_vertex_color;
	case D3DVSDT_SHORT1: return _xgpu_vertex_short1;
	case D3DVSDT_SHORT2: return _xgpu_vertex_short2;
	case D3DVSDT_SHORT3: return _xgpu_vertex_short3;
	case D3DVSDT_SHORT4: return _xgpu_vertex_short4;
	case D3DVSDT_NORMSHORT1: return _xgpu_vertex_normshort1;
	case D3DVSDT_NORMSHORT2: return _xgpu_vertex_normshort2;
	case D3DVSDT_NORMSHORT3: return _xgpu_vertex_normshort3;
	case D3DVSDT_NORMSHORT4: return _xgpu_vertex_normshort4;
	case D3DVSDT_NORMPACKED3: return _xgpu_vertex_packed;
	case D3DVSDT_PBYTE1: return _xgpu_vertex_pbyte1;
	case D3DVSDT_PBYTE2: return _xgpu_vertex_pbyte2;
	case D3DVSDT_PBYTE3: return _xgpu_vertex_pbyte3;
	case D3DVSDT_PBYTE4: return _xgpu_vertex_pbyte4;
	default: return _xgpu_vertex_none;
	}
}

/* an element of the declaration that the draw reads */
static BOOL element_used(const struct vertex_element *element)
{
	return device.streams[element->stream].data && element->type != D3DVSDT_NONE &&
		vertex_format(element->type) != _xgpu_vertex_none && element->reg < XGPU_ATTRIBUTE_COUNT;
}

/* the bytes a repacked element takes: what D3D12 reads of it (three
16-bit components are read as four), a multiple of 4 */
static unsigned long element_room(const struct vertex_element *element)
{
	if (element->type == D3DVSDT_SHORT3 || element->type == D3DVSDT_NORMSHORT3)
		return 8;
	return (element->bytes + 3) & ~3UL;
}

/* D3D12 takes an element only at an offset of a multiple of 4 (the Xbox's
declarations put 16-bit and byte components anywhere): a stream with one
elsewhere, or a stride that is not such a multiple, is copied into the
draw's own bytes with each of its elements at such an offset; returns the
stride of the copy and gives each element's offset there */
static unsigned long stream_repack(const struct vertex_shader_object *declaration, unsigned long stream,
	unsigned long first, unsigned long count, struct xgpu_draw_source *source, unsigned short *offsets)
{
	const unsigned char *base = PLATFORM_PHYSICAL_TO_VIRTUAL(device.streams[stream].data);
	unsigned long stride = device.streams[stream].stride, packed_stride = 0, index, vertex;
	unsigned char *packed;

	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];

		if (element->stream == stream && element_used(element))
		{
			offsets[index] = (unsigned short)packed_stride;
			packed_stride += element_room(element);
		}
	}
	packed = calloc(count ? count : 1, packed_stride ? packed_stride : 4);
	if (!packed)
	{
		draw_inline.overflowed = TRUE;
		return packed_stride;
	}
	for (vertex = 0; vertex < count; vertex++)
	{
		for (index = 0; index < declaration->element_count; index++)
		{
			const struct vertex_element *element = &declaration->elements[index];

			if (element->stream == stream && element_used(element))
			{
				memcpy(packed + vertex * packed_stride + offsets[index], base + (first + vertex) * stride + element->offset,
					element->bytes);
			}
		}
	}
	source->kind = _xgpu_source_inline;
	source->stride = packed_stride;
	source->offset = inline_append(packed, count * packed_stride);
	free(packed);
	return packed_stride;
}

/* the declaration's streams, from vertex first on, for count vertices: from
the mirror where it holds them, else in the draw's own bytes; and the layout
and the attributes no stream gives, into the state */
static void setup_streams(unsigned long first, unsigned long count, struct xgpu_draw_state *state,
	struct xgpu_draw *draw)
{
	struct vertex_shader_object *declaration = device.vertex_shader;
	BOOL placed[16] = { FALSE };
	BOOL repacked[16] = { FALSE };
	BOOL enabled[XGPU_VERTEX_ATTRIBUTE_COUNT] = { FALSE };
	unsigned short offsets[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long index;

	/* the streams D3D12 cannot read as they are */
	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];

		offsets[index] = element->offset;
		if (element_used(element) && (element->offset % 4 || device.streams[element->stream].stride % 4))
			repacked[element->stream] = TRUE;
	}
	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];
		unsigned long stream = element->stream;
		unsigned long stride = device.streams[stream].stride;
		unsigned long bytes = stride ? stride * count : 64;
		struct xgpu_vertex_element *out;

		if (!element_used(element))
			continue;
		if (!placed[stream])
		{
			unsigned long base = (unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(device.streams[stream].data) + first * stride;
			unsigned long segment, offset;

			placed[stream] = TRUE;
			draw->streams[stream].stride = stride;
			if (repacked[stream])
			{
				stream_repack(declaration, stream, first, stride ? count : 1, &draw->streams[stream], offsets);
				if (!stride)
					draw->streams[stream].stride = 0;
			}
			else if (mirror_range(base, bytes, &segment, &offset, NULL))
			{
				draw->streams[stream].kind = _xgpu_source_mirror;
				draw->streams[stream].segment = segment;
				draw->streams[stream].offset = offset;
			}
			else
			{
				draw->streams[stream].kind = _xgpu_source_inline;
				draw->streams[stream].offset = inline_append((const void *)base, bytes);
			}
		}
		out = &state->elements[state->element_count++];
		out->reg = element->reg;
		out->stream = (uint8_t)stream;
		out->format = vertex_format(element->type);
		out->offset = offsets[index];
		enabled[element->reg] = TRUE;
	}
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		/* (a packed attribute no stream gives reads the integer zero) */
		if (!enabled[index] && !(declaration->packed_mask & (1UL << index)))
			memcpy(state->attribute_values[index], device.attributes[index], sizeof(state->attribute_values[index]));
	}
}

/* ---------- primitives D3D12 has (fans, loops, polygons and quads become
lists) */

static uint8_t primitive_of(D3DPRIMITIVETYPE type)
{
	switch (type)
	{
	case D3DPT_POINTLIST: return _xgpu_primitive_points;
	case D3DPT_LINELIST: return _xgpu_primitive_lines;
	case D3DPT_LINESTRIP: case D3DPT_LINELOOP: return _xgpu_primitive_line_strip;
	case D3DPT_TRIANGLESTRIP: case D3DPT_QUADSTRIP: return _xgpu_primitive_triangle_strip;
	default: return _xgpu_primitive_triangles;
	}
}

/* the indices a primitive type that D3D12 lacks is drawn by, from count
indices (or vertices, with indices NULL); NULL if the type needs none */
static WORD *converted_indices(D3DPRIMITIVETYPE type, const WORD *indices, unsigned long count, unsigned long *out_count)
{
	WORD *result = NULL;
	unsigned long index;

#define SOURCE(i) (indices ? indices[i] : (WORD)(i))
	switch (type)
	{
	case D3DPT_QUADLIST:
	{
		unsigned long quads = count / 4, quad;

		result = malloc(quads * 6 * sizeof(WORD) + 2);
		for (quad = 0; quad < quads; quad++)
		{
			result[quad * 6 + 0] = SOURCE(quad * 4);
			result[quad * 6 + 1] = SOURCE(quad * 4 + 1);
			result[quad * 6 + 2] = SOURCE(quad * 4 + 2);
			result[quad * 6 + 3] = SOURCE(quad * 4);
			result[quad * 6 + 4] = SOURCE(quad * 4 + 2);
			result[quad * 6 + 5] = SOURCE(quad * 4 + 3);
		}
		*out_count = quads * 6;
		break;
	}
	case D3DPT_TRIANGLEFAN:
	case D3DPT_POLYGON:
		if (count < 3)
		{
			*out_count = 0;
			return malloc(2);
		}
		result = malloc((count - 2) * 3 * sizeof(WORD) + 2);
		for (index = 0; index + 2 < count; index++)
		{
			result[index * 3] = SOURCE(0);
			result[index * 3 + 1] = SOURCE(index + 1);
			result[index * 3 + 2] = SOURCE(index + 2);
		}
		*out_count = (count - 2) * 3;
		break;
	case D3DPT_LINELOOP:
		result = malloc((count + 1) * sizeof(WORD) + 2);
		for (index = 0; index < count; index++)
			result[index] = SOURCE(index);
		result[count] = SOURCE(0);
		*out_count = count + 1;
		break;
	default:
		break;
	}
#undef SOURCE
	return result;
}

void WINAPI D3DDevice_SetStreamSource(UINT stream_number, D3DVertexBuffer *stream_data, UINT stride)
{
	if (stream_number >= 16)
		return;
	device.streams[stream_number].data = stream_data ? stream_data->Data : 0;
	device.streams[stream_number].stride = stride;
}

void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *index_data, UINT base_vertex_index)
{
	device.base_vertex_index = base_vertex_index;
	D3D__IndexData = index_data ? (WORD *)index_data->Data : NULL;
}

void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE primitive_type, UINT start_vertex, UINT vertex_count)
{
	struct xgpu_draw_state state;
	struct xgpu_draw draw;
	unsigned long count;
	WORD *indices;

	if (!vertex_count || !prepare_draw(FALSE, &state))
		return;
	draw_begin(&draw);
	setup_streams(start_vertex, vertex_count, &state, &draw);
	state.primitive = primitive_of(primitive_type);
	state_send(&state);
	indices = converted_indices(primitive_type, NULL, vertex_count, &count);
	if (indices)
	{
		draw.indexed = TRUE;
		draw.count = count;
		draw.indices.kind = _xgpu_source_inline;
		draw.indices.offset = inline_append(indices, count * sizeof(WORD));
		free(indices);
	}
	else
	{
		draw.count = vertex_count;
	}
	stats.draws++;
	if (draw.count)
		draw_send(&draw);
}

void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE primitive_type, UINT vertex_count, CONST WORD *index_data)
{
	struct xgpu_draw_state state;
	struct xgpu_draw draw;
	unsigned long minimum, maximum, count, generation = 0, segment = 0, index_offset = 0;
	WORD *indices;
	BOOL mirrored;

	if (!vertex_count || !index_data || !prepare_draw(FALSE, &state))
		return;
	indices = converted_indices(primitive_type, index_data, vertex_count, &count);
	mirrored = !indices && mirror_range((unsigned long)index_data, vertex_count * sizeof(WORD), &segment, &index_offset,
		&generation);
	index_extent(index_data, vertex_count, generation, mirrored, &minimum, &maximum);
	draw_begin(&draw);
	/* (the streams from the base vertex on: index i is vertex base + i, and
	the streams start at vertex minimum) */
	setup_streams(device.base_vertex_index + minimum, maximum - minimum + 1, &state, &draw);
	state.primitive = primitive_of(primitive_type);
	state_send(&state);
	draw.indexed = TRUE;
	draw.base_vertex = -(int32_t)minimum;
	if (mirrored)
	{
		draw.count = vertex_count;
		draw.indices.kind = _xgpu_source_mirror;
		draw.indices.segment = segment;
		draw.indices.offset = index_offset;
	}
	else
	{
		draw.count = indices ? count : vertex_count;
		draw.indices.kind = _xgpu_source_inline;
		draw.indices.offset = inline_append(indices ? indices : index_data, draw.count * sizeof(WORD));
	}
	free(indices);
	stats.draws++;
	if (draw.count)
		draw_send(&draw);
}

/* ---------- immediate mode */

void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE primitive_type)
{
	device.immediate_active = TRUE;
	device.immediate_type = primitive_type;
	device.immediate_count = 0;
}

static void immediate_emit(void)
{
	unsigned long floats = XGPU_VERTEX_ATTRIBUTE_COUNT * 4;

	if (device.immediate_count == device.immediate_capacity)
	{
		unsigned long capacity = device.immediate_capacity ? device.immediate_capacity * 2 : 256;
		float *vertices = realloc(device.immediate_vertices, capacity * floats * sizeof(float));

		if (!vertices)
			return;
		device.immediate_vertices = vertices;
		device.immediate_capacity = capacity;
	}
	memcpy(device.immediate_vertices + device.immediate_count * floats, device.attributes, floats * sizeof(float));
	device.immediate_count++;
}

void WINAPI D3DDevice_End(void)
{
	unsigned long stride = XGPU_VERTEX_ATTRIBUTE_COUNT * 4 * sizeof(float);
	unsigned long index, count = device.immediate_count, index_count;
	D3DPRIMITIVETYPE type = device.immediate_type;
	struct xgpu_draw_state state;
	struct xgpu_draw draw;
	WORD *indices;

	device.immediate_active = FALSE;
	if (!count || !prepare_draw(TRUE, &state))
		return;
	draw_begin(&draw);
	/* every attribute four floats, one after another, in stream 0 */
	draw.streams[0].kind = _xgpu_source_inline;
	draw.streams[0].stride = stride;
	draw.streams[0].offset = inline_append(device.immediate_vertices, count * stride);
	state.element_count = XGPU_VERTEX_ATTRIBUTE_COUNT;
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		state.elements[index].reg = (uint8_t)index;
		state.elements[index].stream = 0;
		state.elements[index].format = _xgpu_vertex_float4;
		state.elements[index].offset = (uint16_t)(index * 4 * sizeof(float));
	}
	state.primitive = primitive_of(type);
	state_send(&state);
	indices = converted_indices(type, NULL, count, &index_count);
	if (indices)
	{
		draw.indexed = TRUE;
		draw.count = index_count;
		draw.indices.kind = _xgpu_source_inline;
		draw.indices.offset = inline_append(indices, index_count * sizeof(WORD));
		free(indices);
	}
	else
	{
		draw.count = count;
	}
	stats.immediate_draws++;
	if (draw.count)
		draw_send(&draw);
}

static void set_attribute(INT reg, float a, float b, float c, float d)
{
	BOOL emit = FALSE;

	if (reg == D3DVSDE_VERTEX)
	{
		reg = 0;
		emit = TRUE;
	}
	if (reg < 0 || reg >= XGPU_VERTEX_ATTRIBUTE_COUNT)
		return;
	device.attributes[reg][0] = a;
	device.attributes[reg][1] = b;
	device.attributes[reg][2] = c;
	device.attributes[reg][3] = d;
	if (device.immediate_active && (emit || reg == 0))
		immediate_emit();
}

void WINAPI D3DDevice_SetVertexData2f(INT reg, FLOAT a, FLOAT b)
{
	set_attribute(reg, a, b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4f(INT reg, FLOAT a, FLOAT b, FLOAT c, FLOAT d)
{
	set_attribute(reg, a, b, c, d);
}

void WINAPI D3DDevice_SetVertexData2s(INT reg, SHORT a, SHORT b)
{
	set_attribute(reg, (float)a, (float)b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4ub(INT reg, BYTE a, BYTE b, BYTE c, BYTE d)
{
	set_attribute(reg, a / 255.0f, b / 255.0f, c / 255.0f, d / 255.0f);
}

void WINAPI D3DDevice_SetVertexDataColor(INT reg, D3DCOLOR color)
{
	float value[4];

	color_to_vec4(color, value);
	set_attribute(reg, value[0], value[1], value[2], value[3]);
}

/* ---------- clearing */

void WINAPI D3DDevice_Clear(DWORD count, CONST D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	struct render_target_entry *color_target, *depth_target;
	struct xgpu_clear clear;
	DWORD index;

	if (!device.ready || !bind_targets(&color_target, &depth_target))
		return;
	stats.clears++;
	memset(&clear, 0, sizeof(clear));
	clear.color_target = color_target ? color_target->target.texture : 0;
	clear.depth_target = depth_target ? depth_target->target.texture : 0;
	color_to_vec4(color, clear.color);
	if (flags & D3DCLEAR_TARGET)
	{
		/* the Xbox clears the channels named: the fog screen clears only
		alpha, leaving the picture under the fog */
		clear.flags |= _xgpu_clear_color;
		clear.color_mask = ((flags & D3DCLEAR_TARGET_R) ? 1 : 0) | ((flags & D3DCLEAR_TARGET_G) ? 2 : 0) |
			((flags & D3DCLEAR_TARGET_B) ? 4 : 0) | ((flags & D3DCLEAR_TARGET_A) ? 8 : 0);
	}
	if (depth_target && (flags & D3DCLEAR_ZBUFFER))
		clear.flags |= _xgpu_clear_depth;
	if (depth_target && (flags & D3DCLEAR_STENCIL))
		clear.flags |= _xgpu_clear_stencil;
	clear.depth = z;
	clear.stencil = stencil & 0xff;
	if (!clear.flags)
		return;
	if (!count || !rectangles)
	{
		/* the NV2A clips a clear without rectangles to the viewport */
		clear.rectangle_count = 1;
		clear.rectangles[0][0] = target_pixel((float)device.viewport.X, 0);
		clear.rectangles[0][1] = target_pixel((float)device.viewport.Y, 1);
		clear.rectangles[0][2] = target_pixel((float)(device.viewport.X + device.viewport.Width), 0);
		clear.rectangles[0][3] = target_pixel((float)(device.viewport.Y + device.viewport.Height), 1);
		if (clear.rectangles[0][0] < 0)
			clear.rectangles[0][0] = 0;
		if (clear.rectangles[0][1] < 0)
			clear.rectangles[0][1] = 0;
		if (clear.rectangles[0][2] <= clear.rectangles[0][0] || clear.rectangles[0][3] <= clear.rectangles[0][1])
			return;
	}
	else
	{
		for (index = 0; index < count && clear.rectangle_count < XGPU_CLEAR_RECTANGLES; index++)
		{
			INT left = rectangles[index].x1 > device.viewport.X ? rectangles[index].x1 : device.viewport.X;
			INT top = rectangles[index].y1 > device.viewport.Y ? rectangles[index].y1 : device.viewport.Y;
			INT right = rectangles[index].x2 < device.viewport.X + device.viewport.Width ?
				rectangles[index].x2 : device.viewport.X + device.viewport.Width;
			INT bottom = rectangles[index].y2 < device.viewport.Y + device.viewport.Height ?
				rectangles[index].y2 : device.viewport.Y + device.viewport.Height;
			int32_t *rectangle = clear.rectangles[clear.rectangle_count];

			if (left >= right || top >= bottom)
				continue;
			rectangle[0] = target_pixel((float)(left + UI_OFFSET), 0);
			rectangle[1] = target_pixel((float)top, 1);
			rectangle[2] = target_pixel((float)(right + UI_OFFSET), 0);
			rectangle[3] = target_pixel((float)bottom, 1);
			if (rectangle[0] < 0)
				rectangle[0] = 0;
			if (rectangle[1] < 0)
				rectangle[1] = 0;
			if (rectangle[2] > rectangle[0] && rectangle[3] > rectangle[1])
				clear.rectangle_count++;
		}
		if (!clear.rectangle_count)
			return;
	}
	xgpu_ring_send_data(_xgpu_op_clear, &clear, sizeof(clear), NULL, 0);
}

/* ---------- the anti-aliasing passes */

void halo_screen_anti_alias(short x0, short y0, short x1, short y1)
{
	struct render_target_entry *target;
	struct xgpu_anti_alias anti_alias;
	int mode = anti_aliasing();

	if (!device.ready || (mode != _anti_aliasing_fxaa && mode != _anti_aliasing_smaa))
		return;
	target = render_target_get(&device.back_buffer);
	if (!target)
		return;
	render_target_multisample(target, 0);
	memset(&anti_alias, 0, sizeof(anti_alias));
	anti_alias.mode = mode == _anti_aliasing_smaa ? _xgpu_anti_alias_smaa : _xgpu_anti_alias_fxaa;
	anti_alias.target = target->target.texture;
	anti_alias.rectangle[0] = scaled_pixel(x0, target->target.scale[0]);
	anti_alias.rectangle[1] = scaled_pixel(y0, target->target.scale[1]);
	anti_alias.rectangle[2] = scaled_pixel(x1, target->target.scale[0]);
	anti_alias.rectangle[3] = scaled_pixel(y1, target->target.scale[1]);
	xgpu_ring_send_data(_xgpu_op_anti_alias, &anti_alias, sizeof(anti_alias), NULL, 0);
}

/* ---------- presentation */

static void screenshot_send(struct render_target_entry *target, uint32_t format, const char *path)
{
	struct xgpu_screenshot screenshot;

	memset(&screenshot, 0, sizeof(screenshot));
	screenshot.target = target->target.texture;
	screenshot.format = format;
	snprintf(screenshot.path, sizeof(screenshot.path), "%s", path);
	xgpu_ring_send_data(_xgpu_op_screenshot, &screenshot, sizeof(screenshot), NULL, 0);
}

/* the screenshot key's PNG, in the data root's screenshots folder */
static void key_screenshot(struct render_target_entry *target)
{
	char directory[512], path[640], timestamp[32], filename[64];
	SDL_Time now;
	SDL_DateTime date;
	unsigned int collision;

	if (snprintf(directory, sizeof(directory), "%s/screenshots", platform_data_root()) >= (int)sizeof(directory) ||
		!SDL_CreateDirectory(directory) || !SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &date, true))
	{
		console_printf(FALSE, "Screenshot failed: %.180s", SDL_GetError());
		return;
	}
	snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d_%02d.%02d.%02d",
		date.year, date.month, date.day, date.hour, date.minute, date.second);
	for (collision = 0; collision < 1000; collision++)
	{
		if (collision)
			snprintf(filename, sizeof(filename), "%s_%u.png", timestamp, collision + 1);
		else
			snprintf(filename, sizeof(filename), "%s.png", timestamp);
		snprintf(path, sizeof(path), "%s/%s", directory, filename);
		if (!SDL_GetPathInfo(path, NULL))
			break;
	}
	if (collision == 1000)
	{
		console_printf(FALSE, "Screenshot failed: too many screenshots with the same timestamp");
		return;
	}
	screenshot_send(target, _xgpu_screenshot_png, path);
	console_printf(FALSE, "Saved screenshot as %s", filename);
}

/* the window's size and vsync, sent when they change */
static void window_send(void)
{
	int width, height, vsync = config_boolean("display.vsync") ? 1 : 0;

	platform_video_drawable_size(&width, &height);
	if (width > 0 && height > 0 && (width != device.sent_width || height != device.sent_height))
	{
		struct xgpu_resize resize;

		resize.width = (uint32_t)width;
		resize.height = (uint32_t)height;
		xgpu_ring_send_data(_xgpu_op_resize, &resize, sizeof(resize), NULL, 0);
		device.sent_width = width;
		device.sent_height = height;
	}
	if (vsync != device.sent_vsync)
	{
		struct xgpu_configure configure;

		memset(&configure, 0, sizeof(configure));
		configure.vsync = (uint32_t)vsync;
		xgpu_ring_send_data(_xgpu_op_configure, &configure, sizeof(configure), NULL, 0);
		device.sent_vsync = vsync;
	}
}

void WINAPI D3DDevice_Present(CONST RECT *source_rectangle, CONST RECT *destination_rectangle,
	void *unused, void *unused2)
{
	static long screenshot_every = -1;

	(void)source_rectangle;
	(void)destination_rectangle;
	(void)unused;
	(void)unused2;
	if (screenshot_every < 0)
		screenshot_every = config_integer("debug.screenshot_every");

	if (device.ready)
	{
		struct render_target_entry *back_buffer = render_target_get(&device.back_buffer);
		struct xgpu_present present;

		render_target_resolve(&back_buffer->target);
		if (screenshot_every > 0 && device.frame % (unsigned long)screenshot_every == 0 &&
			*config_string("debug.screenshot_directory"))
		{
			char path[512];

			snprintf(path, sizeof(path), "%s/frame%05lu.bmp", config_string("debug.screenshot_directory"), device.frame);
			screenshot_send(back_buffer, _xgpu_screenshot_bmp, path);
		}
		if (platform_screenshot_take_request())
			key_screenshot(back_buffer);
		window_send();
		memset(&present, 0, sizeof(present));
		present.target = back_buffer->target.texture;
		present.frame = (uint32_t)device.frame + 1;
		/* (paced as the renderer presents: at most XGPU_FRAMES_IN_FLIGHT
		behind it, which waits for vsync) */
		if (!xgpu_host_wait_frame())
		{
			xgpu_ring_flush();
			platform_log("the renderer has stopped");
			exit(EXIT_FAILURE);
		}
		xgpu_ring_send_data(_xgpu_op_present, &present, sizeof(present), NULL, 0);
		xgpu_ring_flush();
		platform_video_swap();
		memory_watch_begin_frame();
		xgpu_texture_cache_begin_frame();
	}
	device.frame++;
	stats.presents++;
	if (debug_settings.statistics && device.frame % 60 == 0)
	{
		platform_log("frame %lu: %lu draws, %lu immediate, %lu clears, %lu target changes, %lu state changes; skipped "
			"%lu no program, %lu no target, %lu too large; %lu KB mirrored, %lu KB streamed",
			device.frame, stats.draws / stats.presents, stats.immediate_draws / stats.presents,
			stats.clears / stats.presents, stats.target_changes / stats.presents, stats.state_changes / stats.presents,
			stats.skipped_no_program, stats.skipped_no_target, stats.skipped_large,
			stats.mirrored_bytes / stats.presents / 1024, stats.streamed_bytes / stats.presents / 1024);
		memset(&stats, 0, sizeof(stats));
	}
	platform_pump_events();

	pthread_mutex_lock(&vertical_blank_lock);
	if (halo_interpolation_enabled())
	{
		flip_count++;
	}
	else
	{
		while (pending_flips >= 2)
			pthread_cond_wait(&vertical_blank_condition, &vertical_blank_lock);
		pending_flips++;
	}
	pthread_mutex_unlock(&vertical_blank_lock);
}

HRESULT WINAPI D3DDevice_PersistDisplay(void)
{
	return S_OK;
}

/* the renderer told to finish (its screenshots written), at exit */
void xgpu_device_shutdown(void)
{
	if (!device.ready)
		return;
	device.ready = FALSE;
	xgpu_ring_send_data(_xgpu_op_quit, NULL, 0, NULL, 0);
	xgpu_ring_flush();
	xgpu_host_stop(3000);
}
