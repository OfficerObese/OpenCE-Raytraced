/*
RENDERER_PIPELINE.C

Shaders, the root signature every draw uses, and the pipeline state objects
made from draw states.

The game sends each shader's HLSL once (xgpu_hlsl.c wrote it from the NV2A
translators' GLSL), which is compiled here with D3DCompile, in this process
rather than the game's. A draw state becomes a pipeline the first time it is
used: its shaders, its blend, depth-stencil and rasterizer states, its
vertex layout, its targets' formats and samples; the comparison and blend
functions it has in the Xbox's terms (OpenGL's numbers) are translated here.

The root signature:
	0, 1, 2   constant buffers b0 (the vertex constants), b1 (the draw's
	          other uniforms), b2 (the model lights)
	3-6       a table of one texture each, t0-t3
	7-10      a table of one sampler each, s0-s3
*/

#include "renderer.h"

#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- shaders */

struct shader_entry
{
	ID3DBlob *code;
	uint8_t input_kinds[XGPU_ATTRIBUTE_COUNT];
	BOOL failed;
};

static struct shader_entry *vertex_shaders, *pixel_shaders;

ID3DBlob *renderer_compile(const char *hlsl, const char *entry, const char *target, const char *what)
{
	ID3DBlob *code = NULL, *errors = NULL;
	UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
	HRESULT result;

	/* (a vertex position computed the same way in two programs must come out
	the same: the game draws a surface in several passes, depth equal) */
	if (target[0] == 'v')
		flags |= D3DCOMPILE_IEEE_STRICTNESS;
	result = D3DCompile(hlsl, strlen(hlsl), what, NULL, NULL, entry, target, flags, 0, &code, &errors);
	if (FAILED(result))
	{
		renderer_log("cannot compile %s:\n%s\n%s", what, errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) :
			"(no message)", hlsl);
		if (code)
			ID3D10Blob_Release(code);
		code = NULL;
	}
	if (errors)
		ID3D10Blob_Release(errors);
	return code;
}

static void shader_store(struct shader_entry **table, const struct xgpu_shader *shader, const char *hlsl,
	uint32_t length, BOOL vertex)
{
	struct shader_entry *entry;
	char name[32], *text;

	if (!shader->shader || shader->shader >= XGPU_SHADER_LIMIT)
		renderer_fail("shader number %u", shader->shader);
	if (!*table)
	{
		*table = calloc(XGPU_SHADER_LIMIT, sizeof(**table));
		if (!*table)
			renderer_fail("out of memory");
	}
	entry = &(*table)[shader->shader];
	if (entry->code)
	{
		renderer_release_later((IUnknown *)entry->code);
		entry->code = NULL;
	}
	memcpy(entry->input_kinds, shader->input_kinds, sizeof(entry->input_kinds));
	text = malloc((size_t)length + 1);
	if (!text)
		renderer_fail("out of memory");
	memcpy(text, hlsl, length);
	text[length] = '\0';
	snprintf(name, sizeof(name), "%s%u", vertex ? "vs" : "ps", shader->shader);
	entry->code = renderer_compile(text, vertex ? "vs_main" : "ps_main", vertex ? "vs_5_0" : "ps_5_0", name);
	entry->failed = !entry->code;
	free(text);
}

void renderer_vertex_shader(const struct xgpu_shader *shader, const char *hlsl, uint32_t length)
{
	shader_store(&vertex_shaders, shader, hlsl, length, TRUE);
}

void renderer_pixel_shader(const struct xgpu_shader *shader, const char *hlsl, uint32_t length)
{
	shader_store(&pixel_shaders, shader, hlsl, length, FALSE);
}

/* ---------- the root signature */

static ID3D12RootSignature *root_signature;

ID3D12RootSignature *renderer_root_signature(void)
{
	return root_signature;
}

static void root_signature_create(void)
{
	D3D12_ROOT_PARAMETER parameters[11];
	D3D12_DESCRIPTOR_RANGE ranges[8];
	D3D12_ROOT_SIGNATURE_DESC description;
	ID3DBlob *blob = NULL, *errors = NULL;
	int index;

	memset(parameters, 0, sizeof(parameters));
	memset(ranges, 0, sizeof(ranges));
	for (index = 0; index < 3; index++)
	{
		parameters[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		parameters[index].Descriptor.ShaderRegister = (UINT)index;
		parameters[index].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	}
	for (index = 0; index < XGPU_TEXTURE_STAGES; index++)
	{
		ranges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[index].NumDescriptors = 1;
		ranges[index].BaseShaderRegister = (UINT)index;
		parameters[3 + index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[3 + index].DescriptorTable.NumDescriptorRanges = 1;
		parameters[3 + index].DescriptorTable.pDescriptorRanges = &ranges[index];
		parameters[3 + index].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		ranges[4 + index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
		ranges[4 + index].NumDescriptors = 1;
		ranges[4 + index].BaseShaderRegister = (UINT)index;
		parameters[7 + index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[7 + index].DescriptorTable.NumDescriptorRanges = 1;
		parameters[7 + index].DescriptorTable.pDescriptorRanges = &ranges[4 + index];
		parameters[7 + index].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}
	memset(&description, 0, sizeof(description));
	description.NumParameters = 11;
	description.pParameters = parameters;
	description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)))
	{
		renderer_fail("the root signature does not serialize: %s",
			errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "");
	}
	RENDERER_CHECK(ID3D12Device_CreateRootSignature(renderer.device, 0, ID3D10Blob_GetBufferPointer(blob),
		ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&root_signature));
	ID3D10Blob_Release(blob);
	if (errors)
		ID3D10Blob_Release(errors);
}

/* ---------- translations of the Xbox's (OpenGL's) numbers */

static D3D12_BLEND blend_factor(uint32_t factor, BOOL alpha)
{
	switch (factor)
	{
	case 0: return D3D12_BLEND_ZERO;
	case 1: return D3D12_BLEND_ONE;
	case 0x300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
	case 0x301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
	case 0x302: return D3D12_BLEND_SRC_ALPHA;
	case 0x303: return D3D12_BLEND_INV_SRC_ALPHA;
	case 0x304: return D3D12_BLEND_DEST_ALPHA;
	case 0x305: return D3D12_BLEND_INV_DEST_ALPHA;
	case 0x306: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
	case 0x307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
	case 0x308: return D3D12_BLEND_SRC_ALPHA_SAT;
	/* constant color, and constant alpha (whose factor the draw makes the
	blend color's alpha in every channel: renderer_draw.c) */
	case 0x8001: case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
	case 0x8002: case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
	default: return D3D12_BLEND_ONE;
	}
}

static D3D12_BLEND_OP blend_operation(uint32_t operation)
{
	switch (operation)
	{
	case 0x800a: return D3D12_BLEND_OP_SUBTRACT;
	case 0x800b: return D3D12_BLEND_OP_REV_SUBTRACT;
	case 0x8007: return D3D12_BLEND_OP_MIN;
	case 0x8008: return D3D12_BLEND_OP_MAX;
	default: return D3D12_BLEND_OP_ADD;
	}
}

static D3D12_COMPARISON_FUNC comparison(uint32_t function)
{
	if (function >= 0x200 && function <= 0x207)
		return (D3D12_COMPARISON_FUNC)(D3D12_COMPARISON_FUNC_NEVER + (function - 0x200));
	return D3D12_COMPARISON_FUNC_NEVER;
}

static D3D12_STENCIL_OP stencil_operation(uint32_t operation)
{
	switch (operation)
	{
	case 0: return D3D12_STENCIL_OP_ZERO;
	case 0x1e01: return D3D12_STENCIL_OP_REPLACE;
	case 0x1e02: return D3D12_STENCIL_OP_INCR_SAT;
	case 0x1e03: return D3D12_STENCIL_OP_DECR_SAT;
	case 0x150a: return D3D12_STENCIL_OP_INVERT;
	case 0x8507: return D3D12_STENCIL_OP_INCR;
	case 0x8508: return D3D12_STENCIL_OP_DECR;
	default: return D3D12_STENCIL_OP_KEEP;
	}
}

static DXGI_FORMAT vertex_format(uint8_t format)
{
	switch (format)
	{
	case _xgpu_vertex_float1: return DXGI_FORMAT_R32_FLOAT;
	case _xgpu_vertex_float2: return DXGI_FORMAT_R32G32_FLOAT;
	case _xgpu_vertex_float3: return DXGI_FORMAT_R32G32B32_FLOAT;
	case _xgpu_vertex_float4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
	case _xgpu_vertex_color: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case _xgpu_vertex_short1: return DXGI_FORMAT_R16_SINT;
	case _xgpu_vertex_short2: return DXGI_FORMAT_R16G16_SINT;
	case _xgpu_vertex_short3: case _xgpu_vertex_short4: return DXGI_FORMAT_R16G16B16A16_SINT;
	case _xgpu_vertex_normshort1: return DXGI_FORMAT_R16_SNORM;
	case _xgpu_vertex_normshort2: return DXGI_FORMAT_R16G16_SNORM;
	case _xgpu_vertex_normshort3: case _xgpu_vertex_normshort4: return DXGI_FORMAT_R16G16B16A16_SNORM;
	case _xgpu_vertex_pbyte1: return DXGI_FORMAT_R8_UNORM;
	case _xgpu_vertex_pbyte2: return DXGI_FORMAT_R8G8_UNORM;
	case _xgpu_vertex_pbyte3: case _xgpu_vertex_pbyte4: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case _xgpu_vertex_packed: return DXGI_FORMAT_R32_UINT;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

/* ---------- pipelines */

/* everything a pipeline is made of (zeroed, then filled: compared whole) */
struct pipeline_key
{
	uint32_t vertex_shader, pixel_shader;
	uint8_t color, depth, samples, topology;
	uint8_t blend_enable, color_write_mask, depth_enable, depth_write;
	uint32_t blend_source, blend_destination, blend_operation;
	uint32_t depth_function;
	uint8_t stencil_enable, cull, front_counter_clockwise, fill;
	uint32_t stencil_function, stencil_read_mask, stencil_write_mask;
	uint32_t stencil_fail, stencil_depth_fail, stencil_pass;
	uint8_t depth_bias_enable, element_count, reserved[2];
	float depth_bias, depth_bias_slope;
	struct xgpu_vertex_element elements[XGPU_ATTRIBUTE_COUNT];
};

struct pipeline_entry
{
	struct pipeline_entry *next;
	uint32_t hash;
	struct pipeline_key key;
	/* NULL: its shaders did not compile, or the pipeline did not build */
	ID3D12PipelineState *pipeline;
};

#define PIPELINE_BUCKETS 4096

static struct pipeline_entry *pipeline_buckets[PIPELINE_BUCKETS];
/* the pipeline found last (draws often repeat it) */
static struct pipeline_entry *pipeline_last;

static void pipeline_key_make(const struct xgpu_draw_state *state, BOOL color, BOOL depth, struct pipeline_key *key)
{
	memset(key, 0, sizeof(*key));
	key->vertex_shader = state->vertex_shader;
	key->pixel_shader = state->pixel_shader;
	key->color = (uint8_t)(color != 0);
	key->depth = (uint8_t)(depth != 0);
	key->samples = state->samples > 1 ? state->samples : 1;
	key->topology = state->primitive;
	key->color_write_mask = color ? state->color_write_mask & 0xf : 0;
	key->blend_enable = color && state->blend_enable;
	if (key->blend_enable)
	{
		key->blend_source = state->blend_source;
		key->blend_destination = state->blend_destination;
		key->blend_operation = state->blend_operation;
	}
	key->depth_enable = depth && state->depth_enable;
	if (key->depth_enable)
	{
		key->depth_write = state->depth_write;
		key->depth_function = state->depth_function;
	}
	key->stencil_enable = depth && state->stencil_enable;
	if (key->stencil_enable)
	{
		key->stencil_function = state->stencil_function;
		key->stencil_read_mask = state->stencil_read_mask & 0xff;
		key->stencil_write_mask = state->stencil_write_mask & 0xff;
		key->stencil_fail = state->stencil_fail;
		key->stencil_depth_fail = state->stencil_depth_fail;
		key->stencil_pass = state->stencil_pass;
	}
	key->cull = state->cull;
	key->front_counter_clockwise = state->front_counter_clockwise;
	key->fill = state->fill;
	key->depth_bias_enable = state->depth_bias_enable;
	if (key->depth_bias_enable)
	{
		key->depth_bias = state->depth_bias;
		key->depth_bias_slope = state->depth_bias_slope;
	}
	key->element_count = state->element_count;
	memcpy(key->elements, state->elements, state->element_count * sizeof(state->elements[0]));
}

static ID3D12PipelineState *pipeline_create(const struct pipeline_key *key)
{
	const struct shader_entry *vertex = vertex_shaders ? &vertex_shaders[key->vertex_shader] : NULL;
	const struct shader_entry *pixel = pixel_shaders ? &pixel_shaders[key->pixel_shader] : NULL;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC description;
	D3D12_INPUT_ELEMENT_DESC inputs[XGPU_ATTRIBUTE_COUNT];
	D3D12_RENDER_TARGET_BLEND_DESC *blend;
	ID3D12PipelineState *pipeline;
	unsigned long attribute;
	HRESULT result;

	if (!vertex || !pixel || !vertex->code || !pixel->code)
		return NULL;
	memset(&description, 0, sizeof(description));
	description.pRootSignature = root_signature;
	description.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vertex->code);
	description.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vertex->code);
	description.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(pixel->code);
	description.PS.BytecodeLength = ID3D10Blob_GetBufferSize(pixel->code);

	blend = &description.BlendState.RenderTarget[0];
	blend->RenderTargetWriteMask = key->color_write_mask;
	blend->BlendEnable = key->blend_enable;
	blend->SrcBlend = blend_factor(key->blend_source, FALSE);
	blend->DestBlend = blend_factor(key->blend_destination, FALSE);
	blend->BlendOp = blend_operation(key->blend_operation);
	blend->SrcBlendAlpha = blend_factor(key->blend_source, TRUE);
	blend->DestBlendAlpha = blend_factor(key->blend_destination, TRUE);
	blend->BlendOpAlpha = blend->BlendOp;
	blend->LogicOp = D3D12_LOGIC_OP_NOOP;
	/* (minimum and maximum take no factors) */
	if (blend->BlendOp == D3D12_BLEND_OP_MIN || blend->BlendOp == D3D12_BLEND_OP_MAX)
		blend->SrcBlend = blend->SrcBlendAlpha = blend->DestBlend = blend->DestBlendAlpha = D3D12_BLEND_ONE;
	if (!key->blend_enable)
	{
		blend->SrcBlend = blend->SrcBlendAlpha = D3D12_BLEND_ONE;
		blend->DestBlend = blend->DestBlendAlpha = D3D12_BLEND_ZERO;
		blend->BlendOp = blend->BlendOpAlpha = D3D12_BLEND_OP_ADD;
	}
	description.SampleMask = 0xffffffffu;

	description.RasterizerState.FillMode = key->fill == 1 ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
	description.RasterizerState.CullMode = key->cull == 1 ? D3D12_CULL_MODE_FRONT :
		key->cull == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
	description.RasterizerState.FrontCounterClockwise = key->front_counter_clockwise;
	if (key->depth_bias_enable)
	{
		description.RasterizerState.DepthBias = (INT)key->depth_bias;
		description.RasterizerState.SlopeScaledDepthBias = key->depth_bias_slope;
	}
	description.RasterizerState.DepthClipEnable = TRUE;
	description.RasterizerState.MultisampleEnable = key->samples > 1;

	description.DepthStencilState.DepthEnable = key->depth_enable;
	description.DepthStencilState.DepthWriteMask = key->depth_write ? D3D12_DEPTH_WRITE_MASK_ALL :
		D3D12_DEPTH_WRITE_MASK_ZERO;
	description.DepthStencilState.DepthFunc = key->depth_enable ? comparison(key->depth_function) :
		D3D12_COMPARISON_FUNC_ALWAYS;
	description.DepthStencilState.StencilEnable = key->stencil_enable;
	description.DepthStencilState.StencilReadMask = (UINT8)key->stencil_read_mask;
	description.DepthStencilState.StencilWriteMask = (UINT8)key->stencil_write_mask;
	description.DepthStencilState.FrontFace.StencilFailOp = stencil_operation(key->stencil_fail);
	description.DepthStencilState.FrontFace.StencilDepthFailOp = stencil_operation(key->stencil_depth_fail);
	description.DepthStencilState.FrontFace.StencilPassOp = stencil_operation(key->stencil_pass);
	description.DepthStencilState.FrontFace.StencilFunc = key->stencil_enable ? comparison(key->stencil_function) :
		D3D12_COMPARISON_FUNC_ALWAYS;
	description.DepthStencilState.BackFace = description.DepthStencilState.FrontFace;

	/* every attribute the shader reads: from its stream, or else the
	constant value in input slot 16 (a buffer read with no stride) */
	for (attribute = 0; attribute < XGPU_ATTRIBUTE_COUNT; attribute++)
	{
		D3D12_INPUT_ELEMENT_DESC *input = &inputs[attribute];
		int element;

		memset(input, 0, sizeof(*input));
		input->SemanticName = "ATTRIB";
		input->SemanticIndex = (UINT)attribute;
		input->InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
		for (element = 0; element < key->element_count; element++)
		{
			if (key->elements[element].reg == attribute)
				break;
		}
		if (element < key->element_count && vertex_format(key->elements[element].format) != DXGI_FORMAT_UNKNOWN)
		{
			input->Format = vertex_format(key->elements[element].format);
			input->InputSlot = key->elements[element].stream;
			input->AlignedByteOffset = key->elements[element].offset;
		}
		else
		{
			input->Format = vertex->input_kinds[attribute] == _xgpu_hlsl_input_uint ? DXGI_FORMAT_R32_UINT :
				vertex->input_kinds[attribute] == _xgpu_hlsl_input_sint ||
				vertex->input_kinds[attribute] == _xgpu_hlsl_input_sint_w1 ? DXGI_FORMAT_R32G32B32A32_SINT :
				DXGI_FORMAT_R32G32B32A32_FLOAT;
			input->InputSlot = XGPU_STREAM_COUNT;
			input->AlignedByteOffset = (UINT)attribute * 16;
		}
	}
	description.InputLayout.pInputElementDescs = inputs;
	description.InputLayout.NumElements = XGPU_ATTRIBUTE_COUNT;
	description.PrimitiveTopologyType = key->topology == _xgpu_primitive_points ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT :
		key->topology == _xgpu_primitive_lines || key->topology == _xgpu_primitive_line_strip ?
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	description.NumRenderTargets = key->color ? 1 : 0;
	description.RTVFormats[0] = key->color ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_UNKNOWN;
	description.DSVFormat = key->depth ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_UNKNOWN;
	description.SampleDesc.Count = key->samples;

	result = ID3D12Device_CreateGraphicsPipelineState(renderer.device, &description, &IID_ID3D12PipelineState,
		(void **)&pipeline);
	if (FAILED(result))
	{
		renderer_log("cannot make the pipeline of vertex shader %u and pixel shader %u (0x%08lX)", key->vertex_shader,
			key->pixel_shader, (unsigned long)result);
		return NULL;
	}
	return pipeline;
}

ID3D12PipelineState *renderer_pipeline_get(const struct xgpu_draw_state *state, BOOL color, BOOL depth)
{
	struct pipeline_key key;
	struct pipeline_entry **bucket, *entry;
	const uint8_t *bytes;
	uint32_t hash = 2166136261u;
	size_t index;

	if (!state->vertex_shader || state->vertex_shader >= XGPU_SHADER_LIMIT || !state->pixel_shader ||
		state->pixel_shader >= XGPU_SHADER_LIMIT || state->element_count > XGPU_ATTRIBUTE_COUNT)
	{
		return NULL;
	}
	pipeline_key_make(state, color, depth, &key);
	if (pipeline_last && !memcmp(&pipeline_last->key, &key, sizeof(key)))
		return pipeline_last->pipeline;
	bytes = (const uint8_t *)&key;
	for (index = 0; index < sizeof(key); index++)
		hash = (hash ^ bytes[index]) * 16777619u;
	bucket = &pipeline_buckets[hash % PIPELINE_BUCKETS];
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->hash == hash && !memcmp(&entry->key, &key, sizeof(key)))
		{
			pipeline_last = entry;
			return entry->pipeline;
		}
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		renderer_fail("out of memory");
	entry->hash = hash;
	entry->key = key;
	entry->pipeline = pipeline_create(&key);
	entry->next = *bucket;
	*bucket = entry;
	pipeline_last = entry;
	return entry->pipeline;
}

void renderer_pipeline_initialize(void)
{
	root_signature_create();
}

/* ---------- a lost device: the compiled shaders are kept (their code is the
device's to translate), the pipelines made again as draws use them */

void renderer_pipeline_lost(void)
{
	int index;

	for (index = 0; index < PIPELINE_BUCKETS; index++)
	{
		struct pipeline_entry *entry = pipeline_buckets[index];

		while (entry)
		{
			struct pipeline_entry *next = entry->next;

			if (entry->pipeline)
				ID3D12PipelineState_Release(entry->pipeline);
			free(entry);
			entry = next;
		}
		pipeline_buckets[index] = NULL;
	}
	pipeline_last = NULL;
	if (root_signature)
		ID3D12RootSignature_Release(root_signature);
	root_signature = NULL;
}

void renderer_pipeline_restore(void)
{
	root_signature_create();
}
