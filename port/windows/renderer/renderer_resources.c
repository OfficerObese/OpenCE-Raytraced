/*
RENDERER_RESOURCES.C

Textures, render targets and the mirror of the Xbox's contiguous memory, by
the numbers the game gives them (xgpu_protocol.h).

A texture is created when it is defined and again when it is defined with
another shape or format (the game's texture objects are reused for other
texels); its texels arrive in pieces through an upload heap. A texture
rests as a pixel shader resource, a render target's pixels as one too
(between draws into it), and each is moved to what a command needs and back,
by the state it is known to be in.
*/

#include "renderer.h"

#include <stdlib.h>
#include <string.h>

/* ---------- tables by number */

static struct renderer_texture **textures;
static uint32_t texture_capacity;
static struct renderer_target **targets;
static uint32_t target_capacity;

static void *table_slot(void ***table, uint32_t *capacity, uint32_t number, uint32_t limit, size_t size)
{
	if (!number || number >= limit)
		renderer_fail("a resource number %u out of range", number);
	if (number >= *capacity)
	{
		uint32_t grown = *capacity ? *capacity : 1024;
		void **resized;

		while (grown <= number)
			grown *= 2;
		if (grown > limit)
			grown = limit;
		resized = realloc(*table, grown * sizeof(void *));
		if (!resized)
			renderer_fail("out of memory");
		memset(resized + *capacity, 0, (grown - *capacity) * sizeof(void *));
		*table = resized;
		*capacity = grown;
	}
	if (!(*table)[number])
	{
		(*table)[number] = calloc(1, size);
		if (!(*table)[number])
			renderer_fail("out of memory");
	}
	return (*table)[number];
}

struct renderer_texture *renderer_texture_get(uint32_t texture)
{
	if (!texture || texture >= texture_capacity || !textures[texture] || !textures[texture]->resource)
		return NULL;
	return textures[texture];
}

struct renderer_target *renderer_target_get(uint32_t target)
{
	if (!target || target >= target_capacity || !targets[target] || !targets[target]->resource)
		return NULL;
	return targets[target];
}

/* ---------- null views */

static struct renderer_descriptor null_srvs[4];

struct renderer_descriptor renderer_null_srv(uint32_t dimension)
{
	return null_srvs[dimension < 4 ? dimension : _xgpu_texture_2d];
}

void renderer_resources_initialize(void)
{
	static const D3D12_SRV_DIMENSION dimensions[4] =
	{
		D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURE3D,
		D3D12_SRV_DIMENSION_TEXTURECUBE,
	};
	int index;

	for (index = 0; index < 4; index++)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC view;

		memset(&view, 0, sizeof(view));
		view.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		view.ViewDimension = dimensions[index];
		view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		view.Texture2D.MipLevels = 1;
		null_srvs[index] = renderer_view_allocate();
		ID3D12Device_CreateShaderResourceView(renderer.device, NULL, &view, null_srvs[index].cpu);
	}
}

/* ---------- textures */

static DXGI_FORMAT texture_format(uint32_t format)
{
	switch (format)
	{
	case _xgpu_format_bgra8: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case _xgpu_format_rgba8: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case _xgpu_format_bc1: return DXGI_FORMAT_BC1_UNORM;
	case _xgpu_format_bc2: return DXGI_FORMAT_BC2_UNORM;
	case _xgpu_format_bc3: return DXGI_FORMAT_BC3_UNORM;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

static BOOL format_compressed(uint32_t format)
{
	return format == _xgpu_format_bc1 || format == _xgpu_format_bc2 || format == _xgpu_format_bc3;
}

/* bytes of a texel, or of a 4x4 block */
static uint32_t format_bytes(uint32_t format)
{
	return format == _xgpu_format_bc1 ? 8 : format_compressed(format) ? 16 : 4;
}

static uint32_t level_size(uint32_t size, uint32_t level)
{
	uint32_t value = size >> level;

	return value ? value : 1;
}

void renderer_texture_state(struct renderer_texture *texture, D3D12_RESOURCE_STATES state)
{
	renderer_transition(texture->resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, texture->state, state);
	texture->state = state;
}

static void texture_view(struct renderer_texture *texture)
{
	const struct xgpu_texture_define *definition = &texture->definition;
	D3D12_SHADER_RESOURCE_VIEW_DESC view;

	if (texture->srv.cpu.ptr)
		renderer_view_free(texture->srv);
	texture->srv = renderer_view_allocate();
	memset(&view, 0, sizeof(view));
	view.Format = texture->format;
	view.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(definition->swizzle[0],
		definition->swizzle[1], definition->swizzle[2], definition->swizzle[3]);
	switch (definition->dimension)
	{
	case _xgpu_texture_3d:
		view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
		view.Texture3D.MipLevels = definition->levels;
		break;
	case _xgpu_texture_cube:
		view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
		view.TextureCube.MipLevels = definition->levels;
		break;
	default:
		view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		view.Texture2D.MipLevels = definition->levels;
		break;
	}
	ID3D12Device_CreateShaderResourceView(renderer.device, texture->resource, &view, texture->srv.cpu);
}

static void texture_release(struct renderer_texture *texture)
{
	if (texture->resource)
		renderer_release_later((IUnknown *)texture->resource);
	if (texture->srv.cpu.ptr)
		renderer_view_free(texture->srv);
	texture->resource = NULL;
	memset(&texture->srv, 0, sizeof(texture->srv));
}

/* ---------- what is kept of a texture's texels: each subresource as sent,
packed (its rows, of texels or of 4x4 blocks, one after another) */

static uint32_t subresource_count(const struct xgpu_texture_define *definition)
{
	return definition->levels * (definition->dimension == _xgpu_texture_cube ? 6 : 1);
}

/* a level's packed bytes: its rows' bytes, rows and slices */
static void level_layout(const struct xgpu_texture_define *definition, uint32_t level, uint32_t *row_bytes,
	uint32_t *rows, uint32_t *slices)
{
	uint32_t block = format_compressed(definition->format) ? 4 : 1;

	*row_bytes = (level_size(definition->width, level) + block - 1) / block * format_bytes(definition->format);
	*rows = (level_size(definition->height, level) + block - 1) / block;
	*slices = definition->dimension == _xgpu_texture_3d ? level_size(definition->depth, level) : 1;
}

static void shadows_free(struct renderer_texture *texture)
{
	uint32_t index;

	for (index = 0; index < texture->shadow_count; index++)
		free(texture->shadows[index]);
	free(texture->shadows);
	texture->shadows = NULL;
	texture->shadow_count = 0;
	texture->mipmap_base = -1;
}

static void shadows_allocate(struct renderer_texture *texture)
{
	texture->shadow_count = subresource_count(&texture->definition);
	texture->shadows = calloc(texture->shadow_count, sizeof(*texture->shadows));
	if (!texture->shadows)
		renderer_fail("out of memory");
	texture->mipmap_base = -1;
}

/* the texture's resource and view, made for its definition */
static void texture_create(struct renderer_texture *texture)
{
	const struct xgpu_texture_define *definition = &texture->definition;
	D3D12_HEAP_PROPERTIES heap;
	D3D12_RESOURCE_DESC description;

	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	memset(&description, 0, sizeof(description));
	description.Dimension = definition->dimension == _xgpu_texture_3d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D :
		D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	description.Width = definition->width;
	description.Height = definition->height;
	description.DepthOrArraySize = (UINT16)(definition->dimension == _xgpu_texture_3d ? definition->depth :
		definition->dimension == _xgpu_texture_cube ? 6 : 1);
	description.MipLevels = (UINT16)definition->levels;
	description.Format = texture->format;
	description.SampleDesc.Count = 1;
	description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	description.Flags = (definition->flags & XGPU_TEXTURE_RENDERABLE) ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET :
		D3D12_RESOURCE_FLAG_NONE;
	RENDERER_CHECK(ID3D12Device_CreateCommittedResource(renderer.device, &heap, D3D12_HEAP_FLAG_NONE, &description,
		D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&texture->resource));
	texture->state = D3D12_RESOURCE_STATE_COMMON;
	texture_view(texture);
}

void renderer_texture_define(const struct xgpu_texture_define *definition)
{
	struct renderer_texture *texture = table_slot((void ***)&textures, &texture_capacity, definition->texture,
		XGPU_TEXTURE_LIMIT, sizeof(struct renderer_texture));
	DXGI_FORMAT format = texture_format(definition->format);
	uint32_t largest = definition->width > definition->height ? definition->width : definition->height;
	uint32_t full_chain = 1, channel;

	if (largest < definition->depth)
		largest = definition->depth;
	while (largest >> full_chain)
		full_chain++;
	if (format == DXGI_FORMAT_UNKNOWN || definition->dimension < _xgpu_texture_2d ||
		definition->dimension > _xgpu_texture_cube || !definition->width || !definition->height || !definition->depth ||
		definition->width > 16384 || definition->height > 16384 || definition->depth > 2048 ||
		(definition->dimension != _xgpu_texture_3d && definition->depth != 1) ||
		(definition->dimension == _xgpu_texture_cube && definition->width != definition->height) ||
		!definition->levels || definition->levels > full_chain)
	{
		renderer_fail("texture %u is defined as %u of format %u, %ux%ux%u, %u levels", definition->texture,
			definition->dimension, definition->format, definition->width, definition->height, definition->depth,
			definition->levels);
	}
	for (channel = 0; channel < 4; channel++)
	{
		if (definition->swizzle[channel] > 5)
			renderer_fail("texture %u has a channel from %u", definition->texture, definition->swizzle[channel]);
	}
	/* (the same shape and format: only its channels change, in a new view;
	the old one may still be read by work in flight) */
	if (texture->resource && texture->definition.dimension == definition->dimension &&
		texture->definition.format == definition->format && texture->definition.width == definition->width &&
		texture->definition.height == definition->height && texture->definition.depth == definition->depth &&
		texture->definition.levels == definition->levels && texture->definition.flags == definition->flags)
	{
		if (memcmp(texture->definition.swizzle, definition->swizzle, sizeof(definition->swizzle)))
		{
			texture->definition = *definition;
			texture_view(texture);
		}
		return;
	}
	texture_release(texture);
	shadows_free(texture);
	texture->definition = *definition;
	texture->format = format;
	/* (D3D12 makes a BC texture's top level a whole number of blocks: the
	game's that are not are decoded before they are sent: xgpu_gl_textures.c) */
	if (format_compressed(definition->format) && (definition->width % 4 || definition->height % 4))
	{
		renderer_log("texture %u: a compressed %ux%u texture, which D3D12 does not take; not drawn",
			definition->texture, definition->width, definition->height);
		return;
	}
	if ((definition->flags & XGPU_TEXTURE_RENDERABLE) && (definition->dimension != _xgpu_texture_2d ||
		format_compressed(definition->format)))
	{
		renderer_fail("texture %u is renderable but not a 2D texture of 32-bit texels", definition->texture);
	}
	/* (kept first: a device lost while the texture is made makes it again) */
	shadows_allocate(texture);
	texture_create(texture);
}

/* a checked region's texels copied into the texture */
static void texels_upload(struct renderer_texture *texture, const struct xgpu_texture_data *data, const uint8_t *bytes)
{
	const struct xgpu_texture_define *definition = &texture->definition;
	uint32_t block = format_compressed(definition->format) ? 4 : 1;
	uint32_t rows = (data->height + block - 1) / block;
	uint32_t row_bytes = (data->width + block - 1) / block * format_bytes(definition->format);
	uint32_t pitch, z, row;
	struct renderer_upload upload;
	D3D12_TEXTURE_COPY_LOCATION source, destination;
	D3D12_BOX box;

	pitch = (row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	upload = renderer_upload_allocate((uint64_t)pitch * rows * data->depth, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
	for (z = 0; z < data->depth; z++)
	{
		for (row = 0; row < rows; row++)
		{
			memcpy((uint8_t *)upload.data + ((uint64_t)z * rows + row) * pitch,
				bytes + ((uint64_t)z * rows + row) * data->row_pitch, row_bytes);
		}
	}
	memset(&source, 0, sizeof(source));
	source.pResource = upload.buffer;
	source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	source.PlacedFootprint.Offset = upload.offset;
	source.PlacedFootprint.Footprint.Format = texture->format;
	/* (a block-compressed footprint is whole blocks) */
	source.PlacedFootprint.Footprint.Width = (data->width + block - 1) / block * block;
	source.PlacedFootprint.Footprint.Height = rows * block;
	source.PlacedFootprint.Footprint.Depth = data->depth;
	source.PlacedFootprint.Footprint.RowPitch = pitch;
	memset(&destination, 0, sizeof(destination));
	destination.pResource = texture->resource;
	destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destination.SubresourceIndex = data->level + data->face * definition->levels;
	/* (whole blocks of the footprint: a level of a BC texture smaller than a
	block takes the block whole) */
	box.left = 0;
	box.top = 0;
	box.front = 0;
	box.right = source.PlacedFootprint.Footprint.Width;
	box.bottom = source.PlacedFootprint.Footprint.Height;
	box.back = data->depth;
	renderer_texture_state(texture, D3D12_RESOURCE_STATE_COPY_DEST);
	ID3D12GraphicsCommandList_CopyTextureRegion(renderer.list, &destination, data->x, data->y, data->z, &source, &box);
	renderer_texture_state(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

/* a checked region's texels into what is kept of its subresource */
static void texels_keep(struct renderer_texture *texture, const struct xgpu_texture_data *data, const uint8_t *bytes)
{
	const struct xgpu_texture_define *definition = &texture->definition;
	uint32_t block = format_compressed(definition->format) ? 4 : 1;
	uint32_t subresource = data->level + data->face * definition->levels;
	uint32_t level_row_bytes, level_rows, slices, rows, row_bytes, x_bytes, z, row;
	uint8_t *shadow;

	if (!texture->shadows || subresource >= texture->shadow_count)
		return;
	level_layout(definition, data->level, &level_row_bytes, &level_rows, &slices);
	if (!texture->shadows[subresource])
	{
		texture->shadows[subresource] = calloc((size_t)level_row_bytes * level_rows, slices);
		if (!texture->shadows[subresource])
			renderer_fail("out of memory");
	}
	shadow = texture->shadows[subresource];
	rows = (data->height + block - 1) / block;
	row_bytes = (data->width + block - 1) / block * format_bytes(definition->format);
	x_bytes = data->x / block * format_bytes(definition->format);
	for (z = 0; z < data->depth; z++)
	{
		for (row = 0; row < rows; row++)
		{
			memcpy(shadow + (((size_t)data->z + z) * level_rows + data->y / block + row) * level_row_bytes + x_bytes,
				bytes + ((size_t)z * rows + row) * data->row_pitch, row_bytes);
		}
	}
}

void renderer_texture_data(const struct xgpu_texture_data *data, const uint8_t *bytes, uint32_t size)
{
	struct renderer_texture *texture = renderer_texture_get(data->texture);
	const struct xgpu_texture_define *definition;
	uint32_t level_width, level_height, level_depth, block, rows, row_bytes;

	if (!texture)
	{
		/* (a texture never defined, or one D3D12 does not take) */
		if (data->texture && data->texture < XGPU_TEXTURE_LIMIT)
			return;
		renderer_fail("texels for texture %u, which is not defined", data->texture);
	}
	definition = &texture->definition;
	block = format_compressed(definition->format) ? 4 : 1;
	if (data->level >= definition->levels ||
		data->face >= (definition->dimension == _xgpu_texture_cube ? 6u : 1u))
	{
		renderer_fail("texels for level %u, face %u of texture %u", data->level, data->face, data->texture);
	}
	level_width = level_size(definition->width, data->level);
	level_height = level_size(definition->height, data->level);
	level_depth = definition->dimension == _xgpu_texture_3d ? level_size(definition->depth, data->level) : 1;
	if (!data->width || !data->height || !data->depth || data->x > level_width || data->width > level_width - data->x ||
		data->y > level_height || data->height > level_height - data->y || data->z > level_depth ||
		data->depth > level_depth - data->z || data->x % block || data->y % block ||
		((data->x + data->width) % block && data->x + data->width != level_width) ||
		((data->y + data->height) % block && data->y + data->height != level_height))
	{
		renderer_fail("texels outside level %u of texture %u", data->level, data->texture);
	}
	rows = (data->height + block - 1) / block;
	row_bytes = (data->width + block - 1) / block * format_bytes(definition->format);
	if (data->row_pitch < row_bytes || (uint64_t)data->row_pitch * rows * data->depth > size)
		renderer_fail("texels for texture %u run past their message", data->texture);
	texels_keep(texture, data, bytes);
	texels_upload(texture, data, bytes);
}

void renderer_texture_mipmaps(const struct xgpu_texture_reference *reference)
{
	struct renderer_texture *texture = renderer_texture_get(reference->texture);
	const struct xgpu_texture_define *definition;
	uint32_t level;

	if (!texture)
		return;
	definition = &texture->definition;
	if (!(definition->flags & XGPU_TEXTURE_RENDERABLE) || reference->base_level >= definition->levels)
		renderer_fail("mip levels made for texture %u, which cannot have them", reference->texture);
	texture->mipmap_base = (int32_t)reference->base_level;
	for (level = reference->base_level + 1; level < definition->levels; level++)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC view;
		D3D12_RENDER_TARGET_VIEW_DESC target_view;
		struct renderer_descriptor source = renderer_view_allocate(), target = renderer_rtv_allocate();

		memset(&view, 0, sizeof(view));
		view.Format = texture->format;
		view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		view.Texture2D.MostDetailedMip = level - 1;
		view.Texture2D.MipLevels = 1;
		ID3D12Device_CreateShaderResourceView(renderer.device, texture->resource, &view, source.cpu);
		memset(&target_view, 0, sizeof(target_view));
		target_view.Format = texture->format;
		target_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		target_view.Texture2D.MipSlice = level;
		ID3D12Device_CreateRenderTargetView(renderer.device, texture->resource, &target_view, target.cpu);
		/* (the level read, then the one drawn: each level its own state) */
		renderer_transition(texture->resource, level, texture->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
		if (texture->state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
			renderer_transition(texture->resource, level - 1, texture->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		renderer_blit(source.gpu, target.cpu, texture->format, 0.0f, 0.0f,
			(float)level_size(definition->width, level), (float)level_size(definition->height, level));
		renderer_transition(texture->resource, level, D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		if (texture->state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
			renderer_transition(texture->resource, level - 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, texture->state);
		renderer_view_free(source);
		renderer_rtv_free(target);
		renderer_draw_invalidate();
	}
	/* (every level is a shader resource now) */
	if (texture->state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
	{
		renderer_transition(texture->resource, reference->base_level, texture->state,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		for (level = 0; level < reference->base_level; level++)
			renderer_transition(texture->resource, level, texture->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		texture->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	}
}

void renderer_texture_delete(uint32_t number)
{
	if (!number || number >= XGPU_TEXTURE_LIMIT)
		renderer_fail("texture %u deleted", number);
	if (number < texture_capacity && textures[number])
	{
		texture_release(textures[number]);
		shadows_free(textures[number]);
		free(textures[number]);
		textures[number] = NULL;
	}
}

/* ---------- render targets */

void renderer_target_state(struct renderer_target *target, D3D12_RESOURCE_STATES state)
{
	renderer_transition(target->resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, target->state, state);
	target->state = state;
}

/* the resource draws go to: the multisampled one, else the target's own */
void renderer_target_draw_state(struct renderer_target *target, D3D12_RESOURCE_STATES state)
{
	if (target->multisampled)
	{
		renderer_transition(target->multisampled, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, target->multisampled_state,
			state);
		target->multisampled_state = state;
	}
	else
	{
		renderer_target_state(target, state);
	}
}

static void target_release(struct renderer_target *target)
{
	renderer_release_later((IUnknown *)target->resource);
	renderer_release_later((IUnknown *)target->multisampled);
	if (target->view.cpu.ptr)
	{
		if (target->definition.depth)
			renderer_dsv_free(target->view);
		else
			renderer_rtv_free(target->view);
	}
	if (target->draw_view.cpu.ptr && target->draw_view.cpu.ptr != target->view.cpu.ptr)
	{
		if (target->definition.depth)
			renderer_dsv_free(target->draw_view);
		else
			renderer_rtv_free(target->draw_view);
	}
	if (target->srv.cpu.ptr)
		renderer_view_free(target->srv);
	memset(target, 0, sizeof(*target));
}

static ID3D12Resource *target_resource(const struct xgpu_target_define *definition, uint32_t samples,
	D3D12_RESOURCE_STATES state)
{
	D3D12_HEAP_PROPERTIES heap;
	D3D12_RESOURCE_DESC description;
	D3D12_CLEAR_VALUE clear;
	ID3D12Resource *resource;

	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	memset(&description, 0, sizeof(description));
	description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	description.Width = definition->width;
	description.Height = definition->height;
	description.DepthOrArraySize = 1;
	description.MipLevels = 1;
	description.Format = definition->depth ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_B8G8R8A8_UNORM;
	description.SampleDesc.Count = samples;
	description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	description.Flags = definition->depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	memset(&clear, 0, sizeof(clear));
	clear.Format = definition->depth ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_B8G8R8A8_UNORM;
	clear.DepthStencil.Depth = 1.0f;
	RENDERER_CHECK(ID3D12Device_CreateCommittedResource(renderer.device, &heap, D3D12_HEAP_FLAG_NONE, &description,
		state, &clear, &IID_ID3D12Resource, (void **)&resource));
	return resource;
}

static struct renderer_descriptor target_view(ID3D12Resource *resource, BOOL depth, BOOL multisampled)
{
	struct renderer_descriptor view;

	if (depth)
	{
		D3D12_DEPTH_STENCIL_VIEW_DESC description;

		memset(&description, 0, sizeof(description));
		description.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		description.ViewDimension = multisampled ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
		view = renderer_dsv_allocate();
		ID3D12Device_CreateDepthStencilView(renderer.device, resource, &description, view.cpu);
	}
	else
	{
		view = renderer_rtv_allocate();
		ID3D12Device_CreateRenderTargetView(renderer.device, resource, NULL, view.cpu);
	}
	return view;
}

void renderer_target_define(const struct xgpu_target_define *definition)
{
	struct renderer_target *target = table_slot((void ***)&targets, &target_capacity, definition->target,
		XGPU_TARGET_LIMIT, sizeof(struct renderer_target));
	uint32_t samples = definition->samples > 1 ? definition->samples : 1;
	D3D12_SHADER_RESOURCE_VIEW_DESC view;

	if (!definition->width || !definition->height || definition->width > 16384 || definition->height > 16384 ||
		(samples != 1 && samples != 2 && samples != 4 && samples != 8) || samples > renderer.maximum_samples)
	{
		renderer_fail("target %u is defined as %ux%u, %u samples", definition->target, definition->width,
			definition->height, samples);
	}
	if (target->resource)
		target_release(target);
	target->definition = *definition;
	target->definition.samples = samples;
	target->state = definition->depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	target->resource = target_resource(definition, 1, target->state);
	target->view = target_view(target->resource, definition->depth != 0, FALSE);
	target->draw_view = target->view;
	if (samples > 1)
	{
		target->multisampled_state = definition->depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE :
			D3D12_RESOURCE_STATE_RENDER_TARGET;
		target->multisampled = target_resource(definition, samples, target->multisampled_state);
		target->draw_view = target_view(target->multisampled, definition->depth != 0, TRUE);
	}
	target->srv = renderer_view_allocate();
	memset(&view, 0, sizeof(view));
	view.Format = definition->depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_B8G8R8A8_UNORM;
	view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	view.Texture2D.MipLevels = 1;
	ID3D12Device_CreateShaderResourceView(renderer.device, target->resource, &view, target->srv.cpu);
}

void renderer_target_delete(uint32_t number)
{
	if (!number || number >= XGPU_TARGET_LIMIT)
		renderer_fail("target %u deleted", number);
	if (number < target_capacity && targets[number])
	{
		if (targets[number]->resource)
			target_release(targets[number]);
		free(targets[number]);
		targets[number] = NULL;
	}
}

void renderer_target_resolve(uint32_t number)
{
	struct renderer_target *target = renderer_target_get(number);

	if (!target)
		renderer_fail("target %u resolved, which is not defined", number);
	/* (a depth target's samples are not resolved: nothing samples depth) */
	if (!target->multisampled || target->definition.depth)
		return;
	renderer_target_draw_state(target, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
	renderer_target_state(target, D3D12_RESOURCE_STATE_RESOLVE_DEST);
	ID3D12GraphicsCommandList_ResolveSubresource(renderer.list, target->resource, 0, target->multisampled, 0,
		DXGI_FORMAT_B8G8R8A8_UNORM);
	renderer_target_state(target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void renderer_target_copy(const struct xgpu_target_copy *copy)
{
	struct renderer_target *target = renderer_target_get(copy->target);
	struct renderer_texture *texture = renderer_texture_get(copy->texture);
	D3D12_TEXTURE_COPY_LOCATION source, destination;

	if (!target || !texture)
		return;
	if (target->definition.depth || copy->level >= texture->definition.levels ||
		texture->definition.dimension != _xgpu_texture_2d || texture->format != DXGI_FORMAT_B8G8R8A8_UNORM ||
		level_size(texture->definition.width, copy->level) != target->definition.width ||
		level_size(texture->definition.height, copy->level) != target->definition.height)
	{
		renderer_fail("target %u copied into level %u of texture %u, which do not match", copy->target, copy->level,
			copy->texture);
	}
	memset(&source, 0, sizeof(source));
	source.pResource = target->resource;
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	memset(&destination, 0, sizeof(destination));
	destination.pResource = texture->resource;
	destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destination.SubresourceIndex = copy->level;
	renderer_target_state(target, D3D12_RESOURCE_STATE_COPY_SOURCE);
	renderer_texture_state(texture, D3D12_RESOURCE_STATE_COPY_DEST);
	ID3D12GraphicsCommandList_CopyTextureRegion(renderer.list, &destination, 0, 0, 0, &source, NULL);
	renderer_texture_state(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	renderer_target_state(target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

/* ---------- the mirror */

#define MIRROR_READ_STATE (D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER)

static ID3D12Resource *mirror_buffers[XGPU_MIRROR_SEGMENT_LIMIT];
/* (a buffer is made in the common state, and is read in MIRROR_READ_STATE
once written) */
static D3D12_RESOURCE_STATES mirror_states[XGPU_MIRROR_SEGMENT_LIMIT];
/* each segment's bytes, kept to fill its buffer again on a new device */
static uint8_t *mirror_shadows[XGPU_MIRROR_SEGMENT_LIMIT];

static void mirror_upload(uint32_t segment, uint32_t offset, const uint8_t *bytes, uint32_t size)
{
	struct renderer_upload upload;

	if (!mirror_buffers[segment])
	{
		mirror_buffers[segment] = renderer_buffer_create(D3D12_HEAP_TYPE_DEFAULT, XGPU_MIRROR_SEGMENT_SIZE,
			D3D12_RESOURCE_STATE_COMMON);
		mirror_states[segment] = D3D12_RESOURCE_STATE_COMMON;
	}
	upload = renderer_upload_allocate(size, 16);
	memcpy(upload.data, bytes, size);
	renderer_transition(mirror_buffers[segment], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, mirror_states[segment],
		D3D12_RESOURCE_STATE_COPY_DEST);
	mirror_states[segment] = MIRROR_READ_STATE;
	ID3D12GraphicsCommandList_CopyBufferRegion(renderer.list, mirror_buffers[segment], offset, upload.buffer,
		upload.offset, size);
	renderer_transition(mirror_buffers[segment], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_COPY_DEST, MIRROR_READ_STATE);
}

void renderer_mirror_write(const struct xgpu_mirror_write *write, const uint8_t *bytes, uint32_t size)
{
	if (write->segment >= XGPU_MIRROR_SEGMENT_LIMIT || write->offset > XGPU_MIRROR_SEGMENT_SIZE ||
		size > XGPU_MIRROR_SEGMENT_SIZE - write->offset || !size)
	{
		renderer_fail("a mirror write of %u bytes at %u in segment %u", size, write->offset, write->segment);
	}
	if (!mirror_shadows[write->segment])
	{
		mirror_shadows[write->segment] = calloc(1, XGPU_MIRROR_SEGMENT_SIZE);
		if (!mirror_shadows[write->segment])
			renderer_fail("out of memory");
	}
	memcpy(mirror_shadows[write->segment] + write->offset, bytes, size);
	mirror_upload(write->segment, write->offset, bytes, size);
}

ID3D12Resource *renderer_mirror_buffer(uint32_t segment)
{
	return segment < XGPU_MIRROR_SEGMENT_LIMIT ? mirror_buffers[segment] : NULL;
}

/* ---------- a lost device */

void renderer_resources_lost(void)
{
	uint32_t index;

	/* (the objects are released at once: the device they belong to is gone,
	and its descriptor heaps with every view) */
	for (index = 0; index < texture_capacity; index++)
	{
		struct renderer_texture *texture = textures[index];

		if (!texture)
			continue;
		if (texture->resource)
			ID3D12Resource_Release(texture->resource);
		texture->resource = NULL;
		memset(&texture->srv, 0, sizeof(texture->srv));
	}
	for (index = 0; index < target_capacity; index++)
	{
		struct renderer_target *target = targets[index];

		if (!target)
			continue;
		if (target->resource)
			ID3D12Resource_Release(target->resource);
		if (target->multisampled)
			ID3D12Resource_Release(target->multisampled);
		target->resource = NULL;
		target->multisampled = NULL;
		memset(&target->view, 0, sizeof(target->view));
		memset(&target->draw_view, 0, sizeof(target->draw_view));
		memset(&target->srv, 0, sizeof(target->srv));
	}
	for (index = 0; index < XGPU_MIRROR_SEGMENT_LIMIT; index++)
	{
		if (mirror_buffers[index])
			ID3D12Resource_Release(mirror_buffers[index]);
		mirror_buffers[index] = NULL;
	}
	memset(null_srvs, 0, sizeof(null_srvs));
}

/* everything made again on the new device: the targets (their pixels are the
game's to draw again), the textures from their kept texels, the mirror from
its kept bytes */
void renderer_resources_restore(void)
{
	uint32_t index;

	renderer_resources_initialize();
	for (index = 0; index < target_capacity; index++)
	{
		struct renderer_target *target = targets[index];

		if (target && target->definition.width)
		{
			struct xgpu_target_define definition = target->definition;

			renderer_target_define(&definition);
		}
	}
	for (index = 0; index < texture_capacity; index++)
	{
		struct renderer_texture *texture = textures[index];
		uint32_t subresource;

		/* (one with kept texels was made; the others D3D12 does not take) */
		if (!texture || !texture->shadows)
			continue;
		texture_create(texture);
		for (subresource = 0; subresource < texture->shadow_count; subresource++)
		{
			struct xgpu_texture_data data;
			uint32_t row_bytes, rows, slices;

			if (!texture->shadows[subresource])
				continue;
			memset(&data, 0, sizeof(data));
			data.texture = texture->definition.texture;
			data.level = subresource % texture->definition.levels;
			data.face = subresource / texture->definition.levels;
			level_layout(&texture->definition, data.level, &row_bytes, &rows, &slices);
			data.width = level_size(texture->definition.width, data.level);
			data.height = level_size(texture->definition.height, data.level);
			data.depth = slices;
			data.row_pitch = row_bytes;
			texels_upload(texture, &data, texture->shadows[subresource]);
		}
		if (texture->mipmap_base >= 0)
		{
			struct xgpu_texture_reference reference;

			memset(&reference, 0, sizeof(reference));
			reference.texture = texture->definition.texture;
			reference.base_level = (uint32_t)texture->mipmap_base;
			renderer_texture_mipmaps(&reference);
		}
		renderer_submit_if_large();
	}
	for (index = 0; index < XGPU_MIRROR_SEGMENT_LIMIT; index++)
	{
		if (mirror_shadows[index])
		{
			mirror_upload(index, 0, mirror_shadows[index], XGPU_MIRROR_SEGMENT_SIZE);
			renderer_submit_if_large();
		}
	}
}
