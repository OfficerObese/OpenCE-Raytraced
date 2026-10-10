/*
XGPU_GL_TEXTURES.C

The OpenGL texture calls the port's shared texture code makes - the texture
cache (xbox_textures.c), the high-res HUD and text (hud_hires.c,
text_hires.c) and the menus' art (menu_files.c) - carried out by the D3D12
renderer instead, so that those files serve the Windows D3D12 build as they
are. These are the only "GL" functions this build has (gl.h's pointers):
each texture becomes a renderer texture of the same number
(xgpu_protocol.h), defined when its top level is given and filled with the
texels each call hands over, in messages of at most XGPU_MESSAGE_MAXIMUM
bytes.

What they take is what those files ask: 2D, cube and 3D textures of RGBA8
texels given as BGRA or RGBA bytes, packed (an unpack alignment of 1), and
DXT1/3/5 blocks; GL_TEXTURE_BASE_LEVEL, GL_TEXTURE_MAX_LEVEL and the
swizzle parameters; glGenerateMipmap, which the renderer does by drawing (an
uncompressed 2D texture with more than one level is made renderable for
it). A compressed texture whose top level is not a whole number of 4x4
blocks, which D3D12 does not take, is decoded to BGRA here.
*/

#include "platform.h"
#define GL_FUNCTIONS_DEFINE
#include "gl.h"
#include "xgpu_gl_textures.h"
#include "xgpu_ring.h"

#include <stdlib.h>
#include <string.h>

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83f1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83f2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83f3
#endif

struct shim_texture
{
	BOOL exists;
	GLenum target;
	GLint base_level, max_level;
	uint8_t swizzle[4];
	/* as last defined (dimension 0: not yet) */
	struct xgpu_texture_define definition;
	/* its blocks are decoded to BGRA as they come */
	BOOL decoded;
};

static struct shim_texture *textures;
static unsigned long texture_capacity;
static unsigned long next_texture = 1;
static GLuint bound_2d, bound_cube, bound_3d;

static struct shim_texture *texture_record(GLuint number)
{
	if (!number || number >= texture_capacity || !textures[number].exists)
		return NULL;
	return &textures[number];
}

static GLuint *binding_of(GLenum target)
{
	switch (target)
	{
	case GL_TEXTURE_CUBE_MAP: return &bound_cube;
	case GL_TEXTURE_3D: return &bound_3d;
	default: return &bound_2d;
	}
}

/* the texture bound to target, which may be a cube map's face */
static struct shim_texture *bound(GLenum target, unsigned long *face)
{
	*face = 0;
	if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
	{
		*face = target - GL_TEXTURE_CUBE_MAP_POSITIVE_X;
		return texture_record(bound_cube);
	}
	return texture_record(*binding_of(target));
}

unsigned long xgpu_texture_number_new(void)
{
	unsigned long number = next_texture++;

	if (number >= XGPU_TEXTURE_LIMIT)
	{
		platform_log("textures: more than %u made", XGPU_TEXTURE_LIMIT);
		abort();
	}
	if (number >= texture_capacity)
	{
		unsigned long capacity = texture_capacity ? texture_capacity * 2 : 4096;
		struct shim_texture *grown = realloc(textures, capacity * sizeof(*grown));

		if (!grown)
		{
			platform_log("textures: out of memory");
			abort();
		}
		memset(grown + texture_capacity, 0, (capacity - texture_capacity) * sizeof(*grown));
		textures = grown;
		texture_capacity = capacity;
	}
	memset(&textures[number], 0, sizeof(textures[number]));
	textures[number].exists = TRUE;
	textures[number].max_level = 1000;
	textures[number].swizzle[0] = 0;
	textures[number].swizzle[1] = 1;
	textures[number].swizzle[2] = 2;
	textures[number].swizzle[3] = 3;
	return number;
}

void xgpu_texture_number_delete(unsigned long number)
{
	struct xgpu_texture_reference reference;

	if (!texture_record((GLuint)number))
		return;
	memset(&reference, 0, sizeof(reference));
	reference.texture = (uint32_t)number;
	xgpu_ring_send_data(_xgpu_op_texture_delete, &reference, sizeof(reference), NULL, 0);
	textures[number].exists = FALSE;
}

/* ---------- definition and texels */

static unsigned long chain_length(unsigned long width, unsigned long height, unsigned long depth)
{
	unsigned long largest = width > height ? width : height, levels = 1;

	if (largest < depth)
		largest = depth;
	while (largest >> levels)
		levels++;
	return levels;
}

/* defines the texture (its top level given) if it is not already as asked */
static void define(GLuint number, struct shim_texture *texture, uint32_t dimension, uint32_t format,
	unsigned long width, unsigned long height, unsigned long depth)
{
	struct xgpu_texture_define definition;
	unsigned long levels = chain_length(width, height, depth);
	unsigned long wanted = texture->max_level >= 0 ? (unsigned long)texture->max_level + 1 : 1;

	memset(&definition, 0, sizeof(definition));
	definition.texture = number;
	definition.dimension = dimension;
	definition.format = format;
	definition.width = width;
	definition.height = height;
	definition.depth = depth;
	definition.levels = wanted < levels ? wanted : levels;
	memcpy(definition.swizzle, texture->swizzle, sizeof(definition.swizzle));
	if (dimension == _xgpu_texture_2d && definition.levels > 1 &&
		(format == _xgpu_format_bgra8 || format == _xgpu_format_rgba8))
	{
		definition.flags = XGPU_TEXTURE_RENDERABLE;
	}
	if (!memcmp(&definition, &texture->definition, sizeof(definition)))
		return;
	texture->definition = definition;
	xgpu_ring_send_data(_xgpu_op_texture_define, &definition, sizeof(definition), NULL, 0);
}

/* sends a level's texels in messages of whole rows (of 4x4 blocks: block 4)
and, for a 3D texture, whole slices */
static void send_texels(GLuint number, unsigned long face, unsigned long level, unsigned long x, unsigned long y,
	unsigned long width, unsigned long height, unsigned long depth, unsigned long row_pitch, unsigned long block,
	const unsigned char *pixels)
{
	unsigned long rows = (height + block - 1) / block;
	unsigned long slice = row_pitch * rows;

	if (slice * depth <= XGPU_MESSAGE_MAXIMUM || depth > 1)
	{
		/* (slices at a time) */
		unsigned long slices = slice ? XGPU_MESSAGE_MAXIMUM / slice : depth;
		unsigned long z;

		if (!slices)
			slices = 1;
		if (slice > XGPU_MESSAGE_MAXIMUM)
		{
			platform_log("textures: a slice of %lu bytes is larger than a message; not sent", slice);
			return;
		}
		for (z = 0; z < depth; z += slices)
		{
			struct xgpu_texture_data data;
			unsigned long count = depth - z < slices ? depth - z : slices;

			memset(&data, 0, sizeof(data));
			data.texture = number;
			data.face = face;
			data.level = level;
			data.x = x;
			data.y = y;
			data.z = z;
			data.width = width;
			data.height = height;
			data.depth = count;
			data.row_pitch = row_pitch;
			xgpu_ring_send_data(_xgpu_op_texture_data, &data, sizeof(data), pixels + z * slice, count * slice);
		}
		return;
	}
	{
		/* (rows at a time, of one slice) */
		unsigned long rows_per_message = XGPU_MESSAGE_MAXIMUM / row_pitch;
		unsigned long row;

		if (!rows_per_message)
			rows_per_message = 1;
		for (row = 0; row < rows; row += rows_per_message)
		{
			struct xgpu_texture_data data;
			unsigned long count = rows - row < rows_per_message ? rows - row : rows_per_message;

			memset(&data, 0, sizeof(data));
			data.texture = number;
			data.face = face;
			data.level = level;
			data.x = x;
			data.y = y + row * block;
			data.z = 0;
			data.width = width;
			data.height = row * block + count * block > height ? height - row * block : count * block;
			data.depth = 1;
			data.row_pitch = row_pitch;
			xgpu_ring_send_data(_xgpu_op_texture_data, &data, sizeof(data), pixels + row * row_pitch,
				count * row_pitch);
		}
	}
}

static uint32_t pixel_format(GLenum format)
{
	return format == GL_RGBA ? _xgpu_format_rgba8 : _xgpu_format_bgra8;
}

static unsigned long level_size(unsigned long size, unsigned long level)
{
	unsigned long value = size >> level;

	return value ? value : 1;
}

static void APIENTRY shim_TexImage(GLenum target, GLint level, GLsizei width, GLsizei height, GLsizei depth,
	GLenum format, const void *pixels)
{
	unsigned long face;
	struct shim_texture *texture = bound(target, &face);
	GLuint number = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? bound_cube :
		*binding_of(target);
	uint32_t dimension = target == GL_TEXTURE_3D ? _xgpu_texture_3d :
		target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? _xgpu_texture_cube :
		_xgpu_texture_2d;

	if (!texture || level < 0 || width <= 0 || height <= 0 || depth <= 0)
		return;
	if (level == 0)
	{
		texture->decoded = FALSE;
		define(number, texture, dimension, pixel_format(format), (unsigned long)width, (unsigned long)height,
			(unsigned long)depth);
	}
	if (pixels && (unsigned long)level < texture->definition.levels)
	{
		send_texels(number, face, (unsigned long)level, 0, 0, (unsigned long)width, (unsigned long)height,
			(unsigned long)depth, (unsigned long)width * 4, 1, pixels);
	}
}

static void APIENTRY shim_glTexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height,
	GLint border, GLenum format, GLenum type, const void *pixels)
{
	(void)internal_format;
	(void)border;
	(void)type;
	shim_TexImage(target, level, width, height, 1, format, pixels);
}

static void APIENTRY shim_glTexImage3D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height,
	GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels)
{
	(void)internal_format;
	(void)border;
	(void)type;
	shim_TexImage(target, level, width, height, depth, format, pixels);
}

static void APIENTRY shim_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
	GLenum format, GLenum type, const void *pixels)
{
	unsigned long face;
	struct shim_texture *texture = bound(target, &face);
	GLuint number = *binding_of(target);

	(void)format;
	(void)type;
	if (!texture || !pixels || level < 0 || (unsigned long)level >= texture->definition.levels || x < 0 || y < 0 ||
		width <= 0 || height <= 0)
	{
		return;
	}
	send_texels(number, face, (unsigned long)level, (unsigned long)x, (unsigned long)y, (unsigned long)width,
		(unsigned long)height, 1, (unsigned long)width * 4, 1, pixels);
}

/* ---------- DXT blocks, decoded where D3D12 does not take them */

static unsigned long expand5(unsigned long v) { return (v << 3) | (v >> 2); }
static unsigned long expand6(unsigned long v) { return (v << 2) | (v >> 4); }

static unsigned long color565(unsigned long value)
{
	return 0xff000000UL | (expand5(value >> 11) << 16) | (expand6((value >> 5) & 0x3f) << 8) | expand5(value & 0x1f);
}

static unsigned long blend(unsigned long a, unsigned long b, unsigned long weight_a, unsigned long weight_b,
	unsigned long divisor)
{
	unsigned long result = 0xff000000UL;
	int shift;

	for (shift = 0; shift < 24; shift += 8)
		result |= ((((a >> shift) & 0xff) * weight_a + ((b >> shift) & 0xff) * weight_b) / divisor) << shift;
	return result;
}

static void dxt_color_block(const unsigned char *block, BOOL dxt1, unsigned long colors[16])
{
	unsigned long c0 = block[0] | (block[1] << 8), c1 = block[2] | (block[3] << 8);
	unsigned long bits = block[4] | (block[5] << 8) | ((unsigned long)block[6] << 16) | ((unsigned long)block[7] << 24);
	unsigned long palette[4];
	int index;

	palette[0] = color565(c0);
	palette[1] = color565(c1);
	if (c0 > c1 || !dxt1)
	{
		palette[2] = blend(palette[0], palette[1], 2, 1, 3);
		palette[3] = blend(palette[0], palette[1], 1, 2, 3);
	}
	else
	{
		palette[2] = blend(palette[0], palette[1], 1, 1, 2);
		palette[3] = 0;
	}
	for (index = 0; index < 16; index++)
		colors[index] = palette[(bits >> (index * 2)) & 3];
}

/* a level of DXT blocks into BGRA texels, width by height by depth */
static void dxt_decode(GLenum format, const unsigned char *source, unsigned long width, unsigned long height,
	unsigned long depth, unsigned long *destination)
{
	unsigned long blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
	unsigned long block_bytes = format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ? 8 : 16;
	unsigned long z, bx, by, x, y;

	for (z = 0; z < depth; z++)
	{
		for (by = 0; by < blocks_y; by++)
		{
			for (bx = 0; bx < blocks_x; bx++)
			{
				const unsigned char *block = source + ((z * blocks_y + by) * blocks_x + bx) * block_bytes;
				unsigned long colors[16], alpha[16];
				int index;

				if (format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT)
				{
					dxt_color_block(block, TRUE, colors);
					for (index = 0; index < 16; index++)
						alpha[index] = colors[index] >> 24;
				}
				else
				{
					dxt_color_block(block + 8, FALSE, colors);
					if (format == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT)
					{
						for (index = 0; index < 16; index++)
							alpha[index] = ((block[index / 2] >> ((index & 1) * 4)) & 0xf) * 0x11;
					}
					else
					{
						unsigned long a0 = block[0], a1 = block[1], values[8];
						unsigned long long bits = 0;
						int bit;

						for (bit = 0; bit < 6; bit++)
							bits |= (unsigned long long)block[2 + bit] << (bit * 8);
						values[0] = a0;
						values[1] = a1;
						if (a0 > a1)
						{
							for (index = 2; index < 8; index++)
								values[index] = ((8 - index) * a0 + (index - 1) * a1) / 7;
						}
						else
						{
							for (index = 2; index < 6; index++)
								values[index] = ((6 - index) * a0 + (index - 1) * a1) / 5;
							values[6] = 0;
							values[7] = 255;
						}
						for (index = 0; index < 16; index++)
							alpha[index] = values[(bits >> (index * 3)) & 7];
					}
				}
				for (y = 0; y < 4; y++)
				{
					for (x = 0; x < 4; x++)
					{
						unsigned long px = bx * 4 + x, py = by * 4 + y;

						if (px < width && py < height)
						{
							destination[(z * height + py) * width + px] =
								(colors[y * 4 + x] & 0x00ffffffUL) | (alpha[y * 4 + x] << 24);
						}
					}
				}
			}
		}
	}
}

static void compressed_image(GLenum target, GLint level, GLenum format, GLsizei width, GLsizei height, GLsizei depth,
	GLsizei size, const void *data)
{
	unsigned long face;
	struct shim_texture *texture = bound(target, &face);
	GLuint number = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? bound_cube :
		*binding_of(target);
	uint32_t dimension = target == GL_TEXTURE_3D ? _xgpu_texture_3d :
		target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? _xgpu_texture_cube :
		_xgpu_texture_2d;
	uint32_t blocks = format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ? _xgpu_format_bc1 :
		format == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT ? _xgpu_format_bc2 : _xgpu_format_bc3;
	unsigned long block_bytes = format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ? 8 : 16;
	unsigned long row_pitch = ((unsigned long)width + 3) / 4 * block_bytes;

	if (!texture || level < 0 || width <= 0 || height <= 0 || depth <= 0 || !data ||
		(unsigned long)size < row_pitch * (((unsigned long)height + 3) / 4) * (unsigned long)depth)
	{
		return;
	}
	if (level == 0)
	{
		/* (D3D12 takes blocks for a top level of whole blocks only) */
		texture->decoded = width % 4 || height % 4;
		define(number, texture, dimension, texture->decoded ? _xgpu_format_bgra8 : blocks, (unsigned long)width,
			(unsigned long)height, (unsigned long)depth);
	}
	if ((unsigned long)level >= texture->definition.levels)
		return;
	if (texture->decoded)
	{
		unsigned long *texels = malloc((size_t)width * height * depth * 4);

		if (!texels)
			return;
		dxt_decode(format, data, (unsigned long)width, (unsigned long)height, (unsigned long)depth, texels);
		send_texels(number, face, (unsigned long)level, 0, 0, (unsigned long)width, (unsigned long)height,
			(unsigned long)depth, (unsigned long)width * 4, 1, (const unsigned char *)texels);
		free(texels);
		return;
	}
	send_texels(number, face, (unsigned long)level, 0, 0, (unsigned long)width, (unsigned long)height,
		(unsigned long)depth, row_pitch, 4, data);
}

static void APIENTRY shim_glCompressedTexImage2D(GLenum target, GLint level, GLenum internal_format, GLsizei width,
	GLsizei height, GLint border, GLsizei size, const void *data)
{
	(void)border;
	compressed_image(target, level, internal_format, width, height, 1, size, data);
}

static void APIENTRY shim_glCompressedTexImage3D(GLenum target, GLint level, GLenum internal_format, GLsizei width,
	GLsizei height, GLsizei depth, GLint border, GLsizei size, const void *data)
{
	(void)border;
	compressed_image(target, level, internal_format, width, height, depth, size, data);
}

/* ---------- objects and parameters */

static void APIENTRY shim_glGenTextures(GLsizei count, GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
		names[index] = (GLuint)xgpu_texture_number_new();
}

static void APIENTRY shim_glDeleteTextures(GLsizei count, const GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		if (bound_2d == names[index])
			bound_2d = 0;
		if (bound_cube == names[index])
			bound_cube = 0;
		if (bound_3d == names[index])
			bound_3d = 0;
		xgpu_texture_number_delete(names[index]);
	}
}

static void APIENTRY shim_glBindTexture(GLenum target, GLuint texture)
{
	struct shim_texture *record = texture_record(texture);

	*binding_of(target) = texture;
	if (record)
		record->target = target;
}

static uint8_t swizzle_source(GLint value)
{
	switch (value)
	{
	case GL_RED: return 0;
	case GL_GREEN: return 1;
	case GL_BLUE: return 2;
	case GL_ALPHA: return 3;
	case GL_ZERO: return 4;
	default: return 5;
	}
}

static void APIENTRY shim_glTexParameteri(GLenum target, GLenum name, GLint value)
{
	unsigned long face;
	struct shim_texture *texture = bound(target, &face);

	if (!texture)
		return;
	switch (name)
	{
	case GL_TEXTURE_BASE_LEVEL: texture->base_level = value; break;
	case GL_TEXTURE_MAX_LEVEL: texture->max_level = value; break;
	case GL_TEXTURE_SWIZZLE_R: texture->swizzle[0] = swizzle_source(value); break;
	case GL_TEXTURE_SWIZZLE_G: texture->swizzle[1] = swizzle_source(value); break;
	case GL_TEXTURE_SWIZZLE_B: texture->swizzle[2] = swizzle_source(value); break;
	case GL_TEXTURE_SWIZZLE_A: texture->swizzle[3] = swizzle_source(value); break;
	default: break;
	}
}

static void APIENTRY shim_glPixelStorei(GLenum name, GLint value)
{
	/* (the texels given are packed, an alignment of 1, as the callers set) */
	(void)name;
	(void)value;
}

static void APIENTRY shim_glGenerateMipmap(GLenum target)
{
	unsigned long face;
	struct shim_texture *texture = bound(target, &face);
	struct xgpu_texture_reference reference;

	if (!texture || !(texture->definition.flags & XGPU_TEXTURE_RENDERABLE))
		return;
	memset(&reference, 0, sizeof(reference));
	reference.texture = *binding_of(target);
	reference.base_level = (uint32_t)(texture->base_level > 0 ? texture->base_level : 0);
	if (reference.base_level + 1 >= texture->definition.levels)
		return;
	xgpu_ring_send_data(_xgpu_op_texture_mipmaps, &reference, sizeof(reference), NULL, 0);
}

static void APIENTRY shim_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void *pixels)
{
	static BOOL logged;

	/* (debug.texture_dump_directory reads textures back: not here) */
	(void)target;
	(void)level;
	(void)format;
	(void)type;
	(void)pixels;
	if (!logged)
	{
		logged = TRUE;
		platform_log("textures: debug.texture_dump_directory is not available with the D3D12 renderer");
	}
}

/* the entry points the shared files call through (gl.h) */
__typeof__(&glGenTextures) halo_glGenTextures = shim_glGenTextures;
__typeof__(&glDeleteTextures) halo_glDeleteTextures = shim_glDeleteTextures;
__typeof__(&glBindTexture) halo_glBindTexture = shim_glBindTexture;
__typeof__(&glTexImage2D) halo_glTexImage2D = shim_glTexImage2D;
__typeof__(&glTexImage3D) halo_glTexImage3D = shim_glTexImage3D;
__typeof__(&glTexSubImage2D) halo_glTexSubImage2D = shim_glTexSubImage2D;
__typeof__(&glCompressedTexImage2D) halo_glCompressedTexImage2D = shim_glCompressedTexImage2D;
__typeof__(&glCompressedTexImage3D) halo_glCompressedTexImage3D = shim_glCompressedTexImage3D;
__typeof__(&glTexParameteri) halo_glTexParameteri = shim_glTexParameteri;
__typeof__(&glPixelStorei) halo_glPixelStorei = shim_glPixelStorei;
__typeof__(&glGenerateMipmap) halo_glGenerateMipmap = shim_glGenerateMipmap;
__typeof__(&glGetTexImage) halo_glGetTexImage = shim_glGetTexImage;
