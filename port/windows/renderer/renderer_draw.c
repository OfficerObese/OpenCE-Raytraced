/*
RENDERER_DRAW.C

The draws, clears and visibility tests.

A draw state (struct xgpu_draw_state) holds until the game sends another;
each draw binds what of it has changed since the draw before: the pipeline,
the targets (moved into the states drawing needs), the viewport and scissor,
the textures and samplers (a sampler descriptor is made once for each
sampler state the game uses), the constant buffers (copied into an upload
heap when they change) and the vertex and index buffers, which are the
mirror's buffers or the draw's own bytes in an upload heap.

Visibility tests are occlusion queries, read back once the frame's work is
done and written into the results area, where the game finds them a frame or
two after the test, as the OpenGL device's do (d3d8_gl.c).
*/

#include "renderer.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>

BOOL renderer_visibility_active;

static struct
{
	struct xgpu_draw_state state;
	BOOL have_state;

	float constants[XGPU_CONSTANT_COUNT][4];
	struct xgpu_hlsl_draw_uniforms uniforms;
	float lights[XGPU_MODEL_LIGHT_COUNT][4];
	/* the constant buffers' copies in upload heaps, made again when they
	change (0: to be made) */
	D3D12_GPU_VIRTUAL_ADDRESS constants_address, uniforms_address, lights_address, attributes_address;

	/* what the command list has bound (all invalid after
	renderer_draw_invalidate) */
	BOOL bound_valid;
	ID3D12PipelineState *pipeline;
	uint32_t color_target, depth_target;
	D3D12_VIEWPORT viewport;
	D3D12_RECT scissor;
	uint32_t stencil_reference;
	float blend_factor[4];
	D3D_PRIMITIVE_TOPOLOGY topology;
	D3D12_GPU_VIRTUAL_ADDRESS bound_constants[3];
	uint64_t textures[XGPU_TEXTURE_STAGES];
	uint32_t samplers[XGPU_TEXTURE_STAGES];
} draw;

void renderer_draw_invalidate(void)
{
	draw.bound_valid = FALSE;
	/* (an upload heap's copies are good only within the command list they
	were made for: after a submission the heap can be reused once that
	work is done, so the next draw copies them again) */
	draw.constants_address = draw.uniforms_address = draw.lights_address = draw.attributes_address = 0;
}

const struct xgpu_draw_state *renderer_draw_state_current(void)
{
	return draw.have_state ? &draw.state : NULL;
}

void renderer_draw_begin_frame(void)
{
	renderer_draw_invalidate();
}

void renderer_vertex_constants(const struct xgpu_vertex_constants *constants, const float *values)
{
	memcpy(draw.constants[constants->first], values, constants->count * 16);
	draw.constants_address = 0;
}

void renderer_draw_uniforms(const struct xgpu_hlsl_draw_uniforms *uniforms)
{
	draw.uniforms = *uniforms;
	draw.uniforms_address = 0;
}

void renderer_model_lights(const float *lights)
{
	memcpy(draw.lights, lights, sizeof(draw.lights));
	draw.lights_address = 0;
}

void renderer_draw_state(const struct xgpu_draw_state *state)
{
	if (state->element_count > XGPU_ATTRIBUTE_COUNT)
		renderer_fail("a draw state with %u vertex elements", state->element_count);
	draw.state = *state;
	draw.have_state = TRUE;
	draw.attributes_address = 0;
}

static D3D12_GPU_VIRTUAL_ADDRESS constant_copy(const void *data, uint32_t size)
{
	struct renderer_upload upload = renderer_upload_allocate((size + 255) & ~255u,
		D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

	memcpy(upload.data, data, size);
	return upload.address;
}

/* ---------- samplers */

#define SAMPLER_BUCKETS 512

static struct
{
	struct xgpu_sampler_state state;
	uint32_t index;
	BOOL used;
} sampler_table[SAMPLER_BUCKETS * 4];
static uint32_t sampler_count;

static D3D12_TEXTURE_ADDRESS_MODE address_mode(uint8_t mode)
{
	switch (mode)
	{
	case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
	case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	}
}

/* D3DTEXF_POINT is 1; LINEAR and the filters beyond it filter */
static D3D12_FILTER_TYPE filter_type(uint8_t filter)
{
	return filter <= 1 ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR;
}

static uint32_t sampler_get(const struct xgpu_sampler_state *state)
{
	uint32_t hash = 2166136261u, slot, probe;
	const uint8_t *bytes = (const uint8_t *)state;
	size_t index;
	D3D12_SAMPLER_DESC description;

	for (index = 0; index < sizeof(*state); index++)
		hash = (hash ^ bytes[index]) * 16777619u;
	for (probe = 0; probe < SAMPLER_BUCKETS * 4; probe++)
	{
		slot = (hash + probe) % (SAMPLER_BUCKETS * 4);
		if (!sampler_table[slot].used)
			break;
		if (!memcmp(&sampler_table[slot].state, state, sizeof(*state)))
			return sampler_table[slot].index;
	}
	if (probe == SAMPLER_BUCKETS * 4 || sampler_count >= RENDERER_SAMPLER_LIMIT)
	{
		/* (never seen: the game uses a few dozen) */
		return 0;
	}
	memset(&description, 0, sizeof(description));
	if (state->min_filter == 3 && state->max_anisotropy > 1)
	{
		description.Filter = D3D12_FILTER_ANISOTROPIC;
		description.MaxAnisotropy = state->max_anisotropy > 16 ? 16 : state->max_anisotropy;
	}
	else
	{
		description.Filter = D3D12_ENCODE_BASIC_FILTER(filter_type(state->min_filter), filter_type(state->mag_filter),
			state->mip_filter == 2 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
			D3D12_FILTER_REDUCTION_TYPE_STANDARD);
		description.MaxAnisotropy = 1;
	}
	description.AddressU = address_mode(state->address_u);
	description.AddressV = address_mode(state->address_v);
	description.AddressW = address_mode(state->address_w);
	description.MipLODBias = state->lod_bias < -16.0f ? -16.0f : state->lod_bias > 15.99f ? 15.99f : state->lod_bias;
	description.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	description.BorderColor[0] = ((state->border_color >> 16) & 0xff) / 255.0f;
	description.BorderColor[1] = ((state->border_color >> 8) & 0xff) / 255.0f;
	description.BorderColor[2] = (state->border_color & 0xff) / 255.0f;
	description.BorderColor[3] = ((state->border_color >> 24) & 0xff) / 255.0f;
	/* without mipmapping (D3DTEXF_NONE), the top level only, as OpenGL's
	minification without mipmaps */
	description.MinLOD = state->mip_filter ? state->min_lod : 0.0f;
	description.MaxLOD = state->mip_filter ? D3D12_FLOAT32_MAX : 0.0f;
	ID3D12Device_CreateSampler(renderer.device, &description, renderer_sampler_cpu(sampler_count));
	sampler_table[slot].state = *state;
	sampler_table[slot].index = sampler_count;
	sampler_table[slot].used = TRUE;
	return sampler_count++;
}

/* ---------- textures that are the draw's own target */

/* a copy of a target the draw both samples and draws into (D3D12 cannot have
a resource in both states at once): the target as it was before the draw */
static struct
{
	ID3D12Resource *resource;
	struct renderer_descriptor srv;
	uint32_t width, height;
} feedback;

static D3D12_GPU_DESCRIPTOR_HANDLE feedback_copy(struct renderer_target *target)
{
	if (!feedback.resource || feedback.width != target->definition.width || feedback.height != target->definition.height)
	{
		D3D12_HEAP_PROPERTIES heap;
		D3D12_RESOURCE_DESC description;

		if (feedback.resource)
			renderer_release_later((IUnknown *)feedback.resource);
		memset(&heap, 0, sizeof(heap));
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		memset(&description, 0, sizeof(description));
		description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		description.Width = target->definition.width;
		description.Height = target->definition.height;
		description.DepthOrArraySize = 1;
		description.MipLevels = 1;
		description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		description.SampleDesc.Count = 1;
		RENDERER_CHECK(ID3D12Device_CreateCommittedResource(renderer.device, &heap, D3D12_HEAP_FLAG_NONE,
			&description, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource,
			(void **)&feedback.resource));
		if (!feedback.srv.cpu.ptr)
			feedback.srv = renderer_view_allocate();
		ID3D12Device_CreateShaderResourceView(renderer.device, feedback.resource, NULL, feedback.srv.cpu);
		feedback.width = target->definition.width;
		feedback.height = target->definition.height;
	}
	renderer_target_state(target, D3D12_RESOURCE_STATE_COPY_SOURCE);
	renderer_transition(feedback.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
	ID3D12GraphicsCommandList_CopyResource(renderer.list, feedback.resource, target->resource);
	renderer_transition(feedback.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	return feedback.srv.gpu;
}

/* ---------- the targets */

/* the draw's targets in the states drawing needs, and bound; FALSE if the
draw has none */
static BOOL targets_bind(uint32_t color_number, uint32_t depth_number, struct renderer_target **color_out,
	struct renderer_target **depth_out)
{
	struct renderer_target *color = renderer_target_get(color_number);
	struct renderer_target *depth = renderer_target_get(depth_number);

	if (color && color->definition.depth)
		color = NULL;
	if (depth && !depth->definition.depth)
		depth = NULL;
	*color_out = color;
	*depth_out = depth;
	if (!color && !depth)
		return FALSE;
	if (color)
		renderer_target_draw_state(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
	if (depth)
		renderer_target_draw_state(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	if (!draw.bound_valid || draw.color_target != color_number || draw.depth_target != depth_number)
	{
		ID3D12GraphicsCommandList_OMSetRenderTargets(renderer.list, color ? 1 : 0, color ? &color->draw_view.cpu : NULL,
			FALSE, depth ? &depth->draw_view.cpu : NULL);
		draw.color_target = color_number;
		draw.depth_target = depth_number;
	}
	return TRUE;
}

/* ---------- draws */

static D3D_PRIMITIVE_TOPOLOGY topology(uint8_t primitive)
{
	switch (primitive)
	{
	case _xgpu_primitive_points: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
	case _xgpu_primitive_lines: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
	case _xgpu_primitive_line_strip: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
	case _xgpu_primitive_triangle_strip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
	default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	}
}

/* a vertex or index source's buffer address and the bytes it holds from
there; FALSE if it is not one */
static BOOL source_address(const struct xgpu_draw_source *source, D3D12_GPU_VIRTUAL_ADDRESS inline_address,
	uint32_t inline_size, D3D12_GPU_VIRTUAL_ADDRESS *address, uint32_t *size)
{
	switch (source->kind)
	{
	case _xgpu_source_mirror:
	{
		ID3D12Resource *buffer = renderer_mirror_buffer(source->segment);

		if (!buffer || source->offset >= XGPU_MIRROR_SEGMENT_SIZE)
			return FALSE;
		*address = ID3D12Resource_GetGPUVirtualAddress(buffer) + source->offset;
		*size = XGPU_MIRROR_SEGMENT_SIZE - source->offset;
		return TRUE;
	}
	case _xgpu_source_inline:
		if (source->offset >= inline_size)
			return FALSE;
		*address = inline_address + source->offset;
		*size = inline_size - source->offset;
		return TRUE;
	default:
		return FALSE;
	}
}

void renderer_draw(const struct xgpu_draw *command, const uint8_t *inline_data)
{
	const struct xgpu_draw_state *state = &draw.state;
	struct renderer_target *color, *depth;
	ID3D12PipelineState *pipeline;
	D3D12_GPU_VIRTUAL_ADDRESS inline_address = 0;
	D3D12_VERTEX_BUFFER_VIEW views[XGPU_STREAM_COUNT + 1];
	int stage;
	uint32_t index;

	if (!draw.have_state || !command->count)
		return;
	if (!targets_bind(state->color_target, state->depth_target, &color, &depth))
		return;
	pipeline = renderer_pipeline_get(state, color != NULL, depth != NULL);
	if (!pipeline)
		return;

	if (!draw.bound_valid)
	{
		ID3D12GraphicsCommandList_SetGraphicsRootSignature(renderer.list, renderer_root_signature());
		memset(draw.bound_constants, 0, sizeof(draw.bound_constants));
		memset(draw.textures, 0xff, sizeof(draw.textures));
		memset(draw.samplers, 0xff, sizeof(draw.samplers));
		draw.pipeline = NULL;
		draw.topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
		memset(&draw.viewport, 0xff, sizeof(draw.viewport));
		memset(&draw.scissor, 0xff, sizeof(draw.scissor));
		draw.stencil_reference = ~0u;
		memset(draw.blend_factor, 0xff, sizeof(draw.blend_factor));
		/* (targets_bind set the targets just now, from an invalid state) */
		draw.bound_valid = TRUE;
	}
	if (draw.pipeline != pipeline)
	{
		ID3D12GraphicsCommandList_SetPipelineState(renderer.list, pipeline);
		draw.pipeline = pipeline;
	}
	{
		D3D12_VIEWPORT viewport;
		D3D12_RECT scissor;

		viewport.TopLeftX = state->viewport[0];
		viewport.TopLeftY = state->viewport[1];
		viewport.Width = state->viewport[2];
		viewport.Height = state->viewport[3];
		viewport.MinDepth = state->depth_range[0];
		viewport.MaxDepth = state->depth_range[1];
		scissor.left = state->scissor[0];
		scissor.top = state->scissor[1];
		scissor.right = state->scissor[2];
		scissor.bottom = state->scissor[3];
		if (memcmp(&viewport, &draw.viewport, sizeof(viewport)))
		{
			ID3D12GraphicsCommandList_RSSetViewports(renderer.list, 1, &viewport);
			draw.viewport = viewport;
		}
		if (memcmp(&scissor, &draw.scissor, sizeof(scissor)))
		{
			ID3D12GraphicsCommandList_RSSetScissorRects(renderer.list, 1, &scissor);
			draw.scissor = scissor;
		}
	}
	if (state->stencil_enable && draw.stencil_reference != (state->stencil_reference & 0xff))
	{
		draw.stencil_reference = state->stencil_reference & 0xff;
		ID3D12GraphicsCommandList_OMSetStencilRef(renderer.list, draw.stencil_reference);
	}
	if (state->blend_enable)
	{
		float factor[4];
		uint32_t blend_color = state->blend_color;

		factor[0] = ((blend_color >> 16) & 0xff) / 255.0f;
		factor[1] = ((blend_color >> 8) & 0xff) / 255.0f;
		factor[2] = (blend_color & 0xff) / 255.0f;
		factor[3] = ((blend_color >> 24) & 0xff) / 255.0f;
		/* (constant alpha: the alpha in every channel) */
		if (state->blend_source == 0x8003 || state->blend_source == 0x8004 ||
			state->blend_destination == 0x8003 || state->blend_destination == 0x8004)
		{
			factor[0] = factor[1] = factor[2] = factor[3];
		}
		if (memcmp(factor, draw.blend_factor, sizeof(factor)))
		{
			ID3D12GraphicsCommandList_OMSetBlendFactor(renderer.list, factor);
			memcpy(draw.blend_factor, factor, sizeof(factor));
		}
	}
	if (draw.topology != topology(state->primitive))
	{
		draw.topology = topology(state->primitive);
		ID3D12GraphicsCommandList_IASetPrimitiveTopology(renderer.list, draw.topology);
	}

	/* the constant buffers, copied where they changed */
	if (!draw.constants_address)
		draw.constants_address = constant_copy(draw.constants, sizeof(draw.constants));
	if (!draw.uniforms_address)
		draw.uniforms_address = constant_copy(&draw.uniforms, sizeof(draw.uniforms));
	if (!draw.lights_address)
		draw.lights_address = constant_copy(draw.lights, sizeof(draw.lights));
	{
		D3D12_GPU_VIRTUAL_ADDRESS addresses[3];

		addresses[0] = draw.constants_address;
		addresses[1] = draw.uniforms_address;
		addresses[2] = draw.lights_address;
		for (index = 0; index < 3; index++)
		{
			if (draw.bound_constants[index] != addresses[index])
			{
				ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(renderer.list, index, addresses[index]);
				draw.bound_constants[index] = addresses[index];
			}
		}
	}

	/* the textures and samplers */
	for (stage = 0; stage < XGPU_TEXTURE_STAGES; stage++)
	{
		uint32_t binding = state->textures[stage];
		D3D12_GPU_DESCRIPTOR_HANDLE view = renderer_null_srv(_xgpu_texture_2d).gpu;
		uint32_t sampler;

		if (binding & XGPU_BINDING_TARGET)
		{
			struct renderer_target *target = renderer_target_get(binding & ~XGPU_BINDING_TARGET);

			if (target && !target->definition.depth)
			{
				if (target == color)
				{
					view = feedback_copy(target);
					/* (back to drawing into it) */
					renderer_target_draw_state(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
				}
				else
				{
					renderer_target_state(target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
					view = target->srv.gpu;
				}
			}
		}
		else if (binding)
		{
			struct renderer_texture *texture = renderer_texture_get(binding);

			if (texture)
			{
				renderer_texture_state(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				view = texture->srv.gpu;
			}
		}
		if (draw.textures[stage] != view.ptr)
		{
			ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(renderer.list, 3 + stage, view);
			draw.textures[stage] = view.ptr;
		}
		sampler = sampler_get(&state->samplers[stage]);
		if (draw.samplers[stage] != sampler)
		{
			ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(renderer.list, 7 + stage,
				renderer_sampler_gpu(sampler));
			draw.samplers[stage] = sampler;
		}
	}

	/* the vertex and index data */
	if (command->inline_size)
	{
		struct renderer_upload upload = renderer_upload_allocate(command->inline_size, 16);

		memcpy(upload.data, inline_data, command->inline_size);
		inline_address = upload.address;
	}
	memset(views, 0, sizeof(views));
	for (index = 0; index < XGPU_STREAM_COUNT; index++)
	{
		D3D12_GPU_VIRTUAL_ADDRESS address;
		uint32_t size;

		if (source_address(&command->streams[index], inline_address, command->inline_size, &address, &size))
		{
			views[index].BufferLocation = address;
			views[index].SizeInBytes = size;
			views[index].StrideInBytes = command->streams[index].stride;
		}
	}
	if (!draw.attributes_address)
		draw.attributes_address = constant_copy(state->attribute_values, sizeof(state->attribute_values));
	views[XGPU_STREAM_COUNT].BufferLocation = draw.attributes_address;
	views[XGPU_STREAM_COUNT].SizeInBytes = sizeof(state->attribute_values);
	views[XGPU_STREAM_COUNT].StrideInBytes = 0;
	ID3D12GraphicsCommandList_IASetVertexBuffers(renderer.list, 0, XGPU_STREAM_COUNT + 1, views);
	if (command->indexed)
	{
		D3D12_INDEX_BUFFER_VIEW indices;
		D3D12_GPU_VIRTUAL_ADDRESS address;
		uint32_t size;

		if (!source_address(&command->indices, inline_address, command->inline_size, &address, &size) ||
			(uint64_t)command->count * 2 > size)
		{
			return;
		}
		indices.BufferLocation = address;
		indices.SizeInBytes = command->count * 2;
		indices.Format = DXGI_FORMAT_R16_UINT;
		ID3D12GraphicsCommandList_IASetIndexBuffer(renderer.list, &indices);
		ID3D12GraphicsCommandList_DrawIndexedInstanced(renderer.list, command->count, 1, 0, command->base_vertex, 0);
	}
	else
	{
		ID3D12GraphicsCommandList_DrawInstanced(renderer.list, command->count, 1, command->first_vertex, 0);
	}
}

/* ---------- clears */

void renderer_clear(const struct xgpu_clear *clear)
{
	struct renderer_target *color = renderer_target_get(clear->color_target);
	struct renderer_target *depth = renderer_target_get(clear->depth_target);
	D3D12_RECT rectangles[XGPU_CLEAR_RECTANGLES];
	uint32_t count = clear->rectangle_count, index;

	if (count > XGPU_CLEAR_RECTANGLES)
		renderer_fail("a clear of %u rectangles", count);
	if (color && color->definition.depth)
		color = NULL;
	if (depth && !depth->definition.depth)
		depth = NULL;
	for (index = 0; index < count; index++)
	{
		rectangles[index].left = clear->rectangles[index][0];
		rectangles[index].top = clear->rectangles[index][1];
		rectangles[index].right = clear->rectangles[index][2];
		rectangles[index].bottom = clear->rectangles[index][3];
	}
	if (color && (clear->flags & _xgpu_clear_color) && (clear->color_mask & 0xf))
	{
		uint32_t width = color->definition.width, height = color->definition.height;
		uint32_t limited = 0;

		/* (each rectangle within the target) */
		for (index = 0; index < count; index++)
		{
			D3D12_RECT *r = &rectangles[limited];

			*r = rectangles[index];
			if (r->left < 0) r->left = 0;
			if (r->top < 0) r->top = 0;
			if (r->right > (LONG)width) r->right = (LONG)width;
			if (r->bottom > (LONG)height) r->bottom = (LONG)height;
			if (r->left < r->right && r->top < r->bottom)
				limited++;
		}
		renderer_target_draw_state(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
		if ((clear->color_mask & 0xf) == 0xf)
		{
			if (!count || limited)
				ID3D12GraphicsCommandList_ClearRenderTargetView(renderer.list, color->draw_view.cpu, clear->color,
					count ? limited : 0, count ? rectangles : NULL);
		}
		else if (!count || limited)
		{
			D3D12_RECT whole = { 0, 0, (LONG)width, (LONG)height };

			renderer_clear_channels(color->draw_view.cpu, color->definition.samples, width, height,
				count ? rectangles : &whole, count ? limited : 1, clear->color, clear->color_mask);
			renderer_draw_invalidate();
		}
		/* (restored for the clears of depth below) */
		for (index = 0; index < count; index++)
		{
			rectangles[index].left = clear->rectangles[index][0];
			rectangles[index].top = clear->rectangles[index][1];
			rectangles[index].right = clear->rectangles[index][2];
			rectangles[index].bottom = clear->rectangles[index][3];
		}
	}
	if (depth && (clear->flags & (_xgpu_clear_depth | _xgpu_clear_stencil)))
	{
		D3D12_CLEAR_FLAGS flags = (D3D12_CLEAR_FLAGS)(((clear->flags & _xgpu_clear_depth) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
			((clear->flags & _xgpu_clear_stencil) ? D3D12_CLEAR_FLAG_STENCIL : 0));
		float value = clear->depth < 0.0f ? 0.0f : clear->depth > 1.0f ? 1.0f : clear->depth;

		renderer_target_draw_state(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		ID3D12GraphicsCommandList_ClearDepthStencilView(renderer.list, depth->draw_view.cpu, flags, value,
			(UINT8)clear->stencil, count, count ? rectangles : NULL);
	}
}

/* ---------- visibility tests */

/* a frame's tests: its queries, each test's slot, and where they are read */
struct visibility_batch
{
	uint32_t slots[XGPU_VISIBILITY_SLOTS];
	uint32_t count;
	uint32_t frame;
	ID3D12Resource *readback;
	/* the fence value of the submission that copied them out (0: none) */
	uint64_t fence;
};

static ID3D12QueryHeap *query_heap;
static struct visibility_batch batches[RENDERER_FRAME_COUNT];
static uint32_t batch_current;

void renderer_visibility_begin(void)
{
	struct visibility_batch *batch = &batches[batch_current];

	if (renderer_visibility_active || batch->count >= XGPU_VISIBILITY_SLOTS)
		return;
	ID3D12GraphicsCommandList_BeginQuery(renderer.list, query_heap, D3D12_QUERY_TYPE_OCCLUSION,
		batch_current * XGPU_VISIBILITY_SLOTS + batch->count);
	renderer_visibility_active = TRUE;
}

void renderer_visibility_end(uint32_t slot)
{
	struct visibility_batch *batch = &batches[batch_current];

	if (!renderer_visibility_active)
		return;
	ID3D12GraphicsCommandList_EndQuery(renderer.list, query_heap, D3D12_QUERY_TYPE_OCCLUSION,
		batch_current * XGPU_VISIBILITY_SLOTS + batch->count);
	renderer_visibility_active = FALSE;
	batch->slots[batch->count++] = slot % XGPU_VISIBILITY_SLOTS;
}

void renderer_visibility_resolve(uint32_t frame)
{
	struct visibility_batch *batch = &batches[batch_current];

	if (renderer_visibility_active)
	{
		/* (a test still running at the frame's end: ended without a slot) */
		ID3D12GraphicsCommandList_EndQuery(renderer.list, query_heap, D3D12_QUERY_TYPE_OCCLUSION,
			batch_current * XGPU_VISIBILITY_SLOTS + batch->count);
		renderer_visibility_active = FALSE;
	}
	batch->frame = frame;
	if (batch->count)
	{
		ID3D12GraphicsCommandList_ResolveQueryData(renderer.list, query_heap, D3D12_QUERY_TYPE_OCCLUSION,
			batch_current * XGPU_VISIBILITY_SLOTS, batch->count, batch->readback, 0);
		batch->fence = renderer.fence_value + 1;
	}
	batch_current = (batch_current + 1) % RENDERER_FRAME_COUNT;
	/* (the batch about to be filled: its counts read, or waited for) */
	if (batches[batch_current].fence)
	{
		renderer_visibility_collect();
		if (batches[batch_current].fence)
		{
			renderer_wait(batches[batch_current].fence);
			renderer_visibility_collect();
		}
	}
}

void renderer_visibility_collect(void)
{
	uint64_t completed = renderer_completed();
	uint32_t index;

	for (index = 0; index < RENDERER_FRAME_COUNT; index++)
	{
		struct visibility_batch *batch = &batches[index];
		const uint64_t *counts;
		D3D12_RANGE range;
		uint32_t test;

		if (!batch->fence || batch->fence > completed)
			continue;
		range.Begin = 0;
		range.End = batch->count * sizeof(uint64_t);
		if (SUCCEEDED(ID3D12Resource_Map(batch->readback, 0, &range, (void **)&counts)))
		{
			for (test = 0; test < batch->count; test++)
			{
				renderer_results->visibility_samples[batch->slots[test]] =
					counts[test] > 0xffffffffull ? 0xffffffffu : (uint32_t)counts[test];
				renderer_results->visibility_frame[batch->slots[test]] = batch->frame;
			}
			range.End = 0;
			ID3D12Resource_Unmap(batch->readback, 0, &range);
		}
		batch->fence = 0;
		batch->count = 0;
	}
}

void renderer_draw_initialize(void)
{
	D3D12_QUERY_HEAP_DESC description;
	uint32_t index;

	memset(&description, 0, sizeof(description));
	description.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
	description.Count = XGPU_VISIBILITY_SLOTS * RENDERER_FRAME_COUNT;
	RENDERER_CHECK(ID3D12Device_CreateQueryHeap(renderer.device, &description, &IID_ID3D12QueryHeap,
		(void **)&query_heap));
	for (index = 0; index < RENDERER_FRAME_COUNT; index++)
	{
		batches[index].readback = renderer_buffer_create(D3D12_HEAP_TYPE_READBACK,
			XGPU_VISIBILITY_SLOTS * sizeof(uint64_t), D3D12_RESOURCE_STATE_COPY_DEST);
	}
	/* (sampler 0: the defaults, for any state past the table's room) */
	{
		struct xgpu_sampler_state defaults;

		memset(&defaults, 0, sizeof(defaults));
		defaults.min_filter = defaults.mag_filter = 2;
		defaults.address_u = defaults.address_v = defaults.address_w = 1;
		sampler_get(&defaults);
	}
}

/* ---------- a lost device: the draw state is kept (the game sends only what
changes), the device's objects made again; visibility tests in flight are
lost, and the game reads their last counts */

void renderer_draw_lost(void)
{
	uint32_t index;

	if (query_heap)
		ID3D12QueryHeap_Release(query_heap);
	query_heap = NULL;
	for (index = 0; index < RENDERER_FRAME_COUNT; index++)
	{
		if (batches[index].readback)
			ID3D12Resource_Release(batches[index].readback);
	}
	memset(batches, 0, sizeof(batches));
	batch_current = 0;
	renderer_visibility_active = FALSE;
	if (feedback.resource)
		ID3D12Resource_Release(feedback.resource);
	memset(&feedback, 0, sizeof(feedback));
	memset(sampler_table, 0, sizeof(sampler_table));
	sampler_count = 0;
	renderer_draw_invalidate();
}

void renderer_draw_restore(void)
{
	renderer_draw_initialize();
	renderer_draw_invalidate();
}
