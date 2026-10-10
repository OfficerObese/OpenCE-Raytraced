/*
RENDERER_POST.C

display.anti_aliasing's passes, FXAA and SMAA, over the rectangle of a
window's 3D view in the back buffer's target, before the HUD and menus are
drawn over it, as port/linux/src/xgpu_post.c draws them with OpenGL: the
target's pixels are copied first, so that no pass reads what it writes, and
only colour is written back (the game keeps values of its own in destination
alpha).

FXAA is xgpu_post.c's, in HLSL. SMAA is port/third_party/smaa's SMAA.hlsl,
which the game sends with its lookup textures when the setting is chosen
(_xgpu_op_anti_alias_prepare), compiled at its HIGH preset through its
porting macros (SMAA_CUSTOM_SL), so that its samplers have the registers the
passes' root signature gives them.

The root signature: eight constants (the target's metrics and FXAA's
bounds), a table of one texture each for t0 to t2, and two static samplers,
linear and point, clamped.
*/

#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char post_common[] =
	"cbuffer post_constants : register(b0)\n"
	"{\n"
	"\t/* 1 / width, 1 / height, width, height */\n"
	"\tfloat4 metrics;\n"
	"\t/* the rectangle's outermost texel centres, as texture coordinates */\n"
	"\tfloat4 bounds;\n"
	"};\n"
	"struct post_varyings { float4 position : SV_Position; };\n"
	/* a triangle that covers the viewport */
	"post_varyings fullscreen(uint vertex : SV_VertexID)\n"
	"{\n"
	"\tpost_varyings output;\n"
	"\tfloat2 uv = float2((vertex << 1) & 2, vertex & 2);\n"
	"\toutput.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
	"\treturn output;\n"
	"}\n";

static const char fxaa_source[] =
	"Texture2D color_texture : register(t0);\n"
	"SamplerState linear_sampler : register(s0);\n"
	"\n"
	"static const float EDGE_THRESHOLD = 0.125;\n"
	"static const float EDGE_THRESHOLD_MINIMUM = 0.0312;\n"
	"static const float SUBPIXEL_QUALITY = 0.75;\n"
	"static const int STEPS = 12;\n"
	"static const float STEP_LENGTHS[12] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };\n"
	"\n"
	"float3 color_at(float2 position)\n"
	"{\n"
	"\treturn color_texture.SampleLevel(linear_sampler, clamp(position, bounds.xy, bounds.zw), 0.0).rgb;\n"
	"}\n"
	"\n"
	"float luma_at(float2 position)\n"
	"{\n"
	"\treturn dot(color_at(position), float3(0.299, 0.587, 0.114));\n"
	"}\n"
	"\n"
	"float4 fxaa(post_varyings input) : SV_Target0\n"
	"{\n"
	"\tfloat2 position = input.position.xy * metrics.xy;\n"
	"\tfloat luma = luma_at(position);\n"
	"\tfloat up = luma_at(position + float2(0.0, -metrics.y));\n"
	"\tfloat down = luma_at(position + float2(0.0, metrics.y));\n"
	"\tfloat left = luma_at(position + float2(-metrics.x, 0.0));\n"
	"\tfloat right = luma_at(position + float2(metrics.x, 0.0));\n"
	"\tfloat highest = max(luma, max(max(up, down), max(left, right)));\n"
	"\tfloat range = highest - min(luma, min(min(up, down), min(left, right)));\n"
	"\n"
	"\t/* no edge: the target has the pixel already */\n"
	"\tif (range < max(EDGE_THRESHOLD_MINIMUM, highest * EDGE_THRESHOLD))\n"
	"\t\tdiscard;\n"
	"\tfloat up_left = luma_at(position + float2(-metrics.x, -metrics.y));\n"
	"\tfloat up_right = luma_at(position + float2(metrics.x, -metrics.y));\n"
	"\tfloat down_left = luma_at(position + float2(-metrics.x, metrics.y));\n"
	"\tfloat down_right = luma_at(position + float2(metrics.x, metrics.y));\n"
	"\n"
	"\tfloat average = (2.0 * (up + down + left + right) + up_left + up_right + down_left + down_right) / 12.0;\n"
	"\tfloat subpixel = smoothstep(0.0, 1.0, clamp(abs(average - luma) / range, 0.0, 1.0));\n"
	"\tsubpixel = subpixel * subpixel * SUBPIXEL_QUALITY;\n"
	"\n"
	"\tbool horizontal = abs(up_left + down_left - 2.0 * left) + 2.0 * abs(up + down - 2.0 * luma) +\n"
	"\t\tabs(up_right + down_right - 2.0 * right) >=\n"
	"\t\tabs(up_left + up_right - 2.0 * up) + 2.0 * abs(left + right - 2.0 * luma) +\n"
	"\t\tabs(down_left + down_right - 2.0 * down);\n"
	"\tfloat before = horizontal ? up : left;\n"
	"\tfloat after = horizontal ? down : right;\n"
	"\tbool before_steeper = abs(before - luma) >= abs(after - luma);\n"
	"\tfloat gradient = 0.25 * max(abs(before - luma), abs(after - luma));\n"
	"\tfloat edge_luma = 0.5 * ((before_steeper ? before : after) + luma);\n"
	"\tfloat2 across = (horizontal ? float2(0.0, metrics.y) : float2(metrics.x, 0.0)) * (before_steeper ? -1.0 : 1.0);\n"
	"\tfloat2 along = horizontal ? float2(metrics.x, 0.0) : float2(0.0, metrics.y);\n"
	"\n"
	"\tfloat2 end_before = position + 0.5 * across - along;\n"
	"\tfloat2 end_after = position + 0.5 * across + along;\n"
	"\tfloat luma_before = luma_at(end_before) - edge_luma;\n"
	"\tfloat luma_after = luma_at(end_after) - edge_luma;\n"
	"\tbool reached_before = abs(luma_before) >= gradient;\n"
	"\tbool reached_after = abs(luma_after) >= gradient;\n"
	"\t[loop]\n"
	"\tfor (int index = 1; index < STEPS && !(reached_before && reached_after); index++)\n"
	"\t{\n"
	"\t\tif (!reached_before)\n"
	"\t\t{\n"
	"\t\t\tend_before -= along * STEP_LENGTHS[index];\n"
	"\t\t\tluma_before = luma_at(end_before) - edge_luma;\n"
	"\t\t\treached_before = abs(luma_before) >= gradient;\n"
	"\t\t}\n"
	"\t\tif (!reached_after)\n"
	"\t\t{\n"
	"\t\t\tend_after += along * STEP_LENGTHS[index];\n"
	"\t\t\tluma_after = luma_at(end_after) - edge_luma;\n"
	"\t\t\treached_after = abs(luma_after) >= gradient;\n"
	"\t\t}\n"
	"\t}\n"
	"\n"
	"\tfloat distance_before = horizontal ? position.x - end_before.x : position.y - end_before.y;\n"
	"\tfloat distance_after = horizontal ? end_after.x - position.x : end_after.y - position.y;\n"
	"\tfloat pixel_offset = 0.5 - min(distance_before, distance_after) / (distance_before + distance_after);\n"
	"\tbool varies = ((distance_before < distance_after ? luma_before : luma_after) < 0.0) != (luma < edge_luma);\n"
	"\treturn float4(color_at(position + max(varies ? pixel_offset : 0.0, subpixel) * across), 1.0);\n"
	"}\n";

/* SMAA.hlsl's porting macros, with the samplers at the passes' registers */
static const char smaa_prefix[] =
	"#define SMAA_CUSTOM_SL 1\n"
	"#define SMAA_PRESET_HIGH 1\n"
	"#define SMAA_RT_METRICS metrics\n"
	"SamplerState LinearSampler : register(s0);\n"
	"SamplerState PointSampler : register(s1);\n"
	"#define SMAATexture2D(tex) Texture2D tex\n"
	"#define SMAATexturePass2D(tex) tex\n"
	"#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)\n"
	"#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)\n"
	"#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)\n"
	"#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)\n"
	"#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)\n"
	"#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)\n"
	"#define SMAA_FLATTEN [flatten]\n"
	"#define SMAA_BRANCH [branch]\n"
	"#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex\n"
	"#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)\n";

/* its passes' main functions, after SMAA.hlsl; its vertex functions' offsets
found for each pixel */
static const char smaa_mains[] =
	"Texture2D first_texture : register(t0);\n"
	"Texture2D second_texture : register(t1);\n"
	"Texture2D third_texture : register(t2);\n"
	"float2 smaa_edges(post_varyings input) : SV_Target0\n"
	"{\n"
	"\tfloat2 texcoord = input.position.xy * metrics.xy;\n"
	"\tfloat4 offset[3];\n"
	"\tSMAAEdgeDetectionVS(texcoord, offset);\n"
	"\treturn SMAALumaEdgeDetectionPS(texcoord, offset, first_texture);\n"
	"}\n"
	"float4 smaa_weights(post_varyings input) : SV_Target0\n"
	"{\n"
	"\tfloat2 texcoord = input.position.xy * metrics.xy;\n"
	"\tfloat2 pixcoord;\n"
	"\tfloat4 offset[3];\n"
	"\tSMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);\n"
	"\treturn SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, first_texture, second_texture,\n"
	"\t\tthird_texture, (float4)0);\n"
	"}\n"
	"float4 smaa_blend(post_varyings input) : SV_Target0\n"
	"{\n"
	"\tfloat2 texcoord = input.position.xy * metrics.xy;\n"
	"\tfloat4 offset;\n"
	"\tSMAANeighborhoodBlendingVS(texcoord, offset);\n"
	"\treturn SMAANeighborhoodBlendingPS(texcoord, offset, first_texture, second_texture);\n"
	"}\n";

enum
{
	_pass_fxaa,
	_pass_smaa_edges,
	_pass_smaa_weights,
	_pass_smaa_blend,
	NUMBER_OF_PASSES
};

/* each pass's target format, and the channels it writes (only colour into
the game's target) */
static const DXGI_FORMAT pass_formats[NUMBER_OF_PASSES] =
{
	DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
};
#define COLOR_ONLY (D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE)
static const uint8_t pass_write_masks[NUMBER_OF_PASSES] =
{
	COLOR_ONLY, D3D12_COLOR_WRITE_ENABLE_ALL, D3D12_COLOR_WRITE_ENABLE_ALL, COLOR_ONLY,
};

static struct
{
	ID3D12RootSignature *signature;
	ID3DBlob *vertex;
	/* the passes' compiled pixel shaders, kept to make their pipelines again
	on a new device */
	ID3DBlob *pixels[NUMBER_OF_PASSES];
	ID3D12PipelineState *pipelines[NUMBER_OF_PASSES];
	BOOL fxaa_failed, smaa_failed, smaa_ready;
	uint32_t area_texture, search_texture;

	/* the target's pixels before the pass, and SMAA's edges and blending
	weights, of the target's size */
	uint32_t width, height;
	ID3D12Resource *color, *edges, *weights;
	D3D12_RESOURCE_STATES color_state, edges_state, weights_state;
	struct renderer_descriptor color_srv, edges_srv, weights_srv, edges_rtv, weights_rtv;
} post;

static void signature_create(void)
{
	D3D12_ROOT_PARAMETER parameters[4];
	D3D12_DESCRIPTOR_RANGE ranges[3];
	D3D12_STATIC_SAMPLER_DESC samplers[2];
	D3D12_ROOT_SIGNATURE_DESC description;
	ID3DBlob *blob = NULL, *errors = NULL;
	int index;

	memset(parameters, 0, sizeof(parameters));
	memset(ranges, 0, sizeof(ranges));
	parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[0].Constants.Num32BitValues = 8;
	parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	for (index = 0; index < 3; index++)
	{
		ranges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[index].NumDescriptors = 1;
		ranges[index].BaseShaderRegister = (UINT)index;
		parameters[1 + index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[1 + index].DescriptorTable.NumDescriptorRanges = 1;
		parameters[1 + index].DescriptorTable.pDescriptorRanges = &ranges[index];
		parameters[1 + index].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}
	memset(samplers, 0, sizeof(samplers));
	samplers[0].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
	samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	samplers[1] = samplers[0];
	samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	samplers[1].ShaderRegister = 1;
	memset(&description, 0, sizeof(description));
	description.NumParameters = 4;
	description.pParameters = parameters;
	description.NumStaticSamplers = 2;
	description.pStaticSamplers = samplers;
	if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)))
	{
		renderer_fail("the anti-aliasing root signature does not serialize: %s",
			errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "");
	}
	RENDERER_CHECK(ID3D12Device_CreateRootSignature(renderer.device, 0, ID3D10Blob_GetBufferPointer(blob),
		ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&post.signature));
	ID3D10Blob_Release(blob);
	if (errors)
		ID3D10Blob_Release(errors);
}

/* a pass's pipeline from its pixel shader; NULL if it does not build */
static ID3D12PipelineState *pass_pipeline(int which)
{
	ID3DBlob *pixel = post.pixels[which];
	D3D12_GRAPHICS_PIPELINE_STATE_DESC description;
	ID3D12PipelineState *pipeline = NULL;

	memset(&description, 0, sizeof(description));
	description.pRootSignature = post.signature;
	description.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(post.vertex);
	description.VS.BytecodeLength = ID3D10Blob_GetBufferSize(post.vertex);
	description.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(pixel);
	description.PS.BytecodeLength = ID3D10Blob_GetBufferSize(pixel);
	description.BlendState.RenderTarget[0].RenderTargetWriteMask = pass_write_masks[which];
	description.BlendState.RenderTarget[0].SrcBlend = description.BlendState.RenderTarget[0].SrcBlendAlpha =
		D3D12_BLEND_ONE;
	description.BlendState.RenderTarget[0].DestBlend = description.BlendState.RenderTarget[0].DestBlendAlpha =
		D3D12_BLEND_ZERO;
	description.BlendState.RenderTarget[0].BlendOp = description.BlendState.RenderTarget[0].BlendOpAlpha =
		D3D12_BLEND_OP_ADD;
	description.SampleMask = 0xffffffffu;
	description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	description.RasterizerState.DepthClipEnable = TRUE;
	description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	description.NumRenderTargets = 1;
	description.RTVFormats[0] = pass_formats[which];
	description.SampleDesc.Count = 1;
	if (FAILED(ID3D12Device_CreateGraphicsPipelineState(renderer.device, &description, &IID_ID3D12PipelineState,
		(void **)&pipeline)))
	{
		return NULL;
	}
	return pipeline;
}

/* the common text, then more, as one string (malloc'd) */
static char *joined(const char *const *parts, int count)
{
	size_t length = 1;
	char *text;
	int index;

	for (index = 0; index < count; index++)
		length += strlen(parts[index]);
	text = malloc(length);
	if (!text)
		renderer_fail("out of memory");
	text[0] = '\0';
	for (index = 0; index < count; index++)
		strcat(text, parts[index]);
	return text;
}

static void post_initialize(void)
{
	if (post.signature)
		return;
	signature_create();
	post.vertex = renderer_compile(post_common, "fullscreen", "vs_5_0", "the anti-aliasing passes' vertex shader");
	if (!post.vertex)
		renderer_fail("the anti-aliasing passes' vertex shader does not compile");
}

static BOOL fxaa_prepare(void)
{
	const char *parts[2];
	char *text;

	post_initialize();
	if (post.pipelines[_pass_fxaa] || post.fxaa_failed)
		return !post.fxaa_failed;
	parts[0] = post_common;
	parts[1] = fxaa_source;
	text = joined(parts, 2);
	post.pixels[_pass_fxaa] = renderer_compile(text, "fxaa", "ps_5_0", "FXAA");
	free(text);
	if (post.pixels[_pass_fxaa])
		post.pipelines[_pass_fxaa] = pass_pipeline(_pass_fxaa);
	post.fxaa_failed = !post.pipelines[_pass_fxaa];
	if (post.fxaa_failed)
		renderer_log("anti-aliasing: FXAA does not build, so the 3D view is not antialiased");
	return !post.fxaa_failed;
}

void renderer_anti_alias_prepare(const struct xgpu_anti_alias_prepare *prepare, const char *smaa, uint32_t length)
{
	static const char *const entries[3] = { "smaa_edges", "smaa_weights", "smaa_blend" };
	char *source, *text;
	const char *parts[4];
	int pass;

	if (prepare->mode == _xgpu_anti_alias_fxaa)
	{
		fxaa_prepare();
		return;
	}
	post_initialize();
	post.area_texture = prepare->area_texture;
	post.search_texture = prepare->search_texture;
	if (post.smaa_ready || post.smaa_failed)
		return;
	source = malloc((size_t)length + 1);
	if (!source)
		renderer_fail("out of memory");
	memcpy(source, smaa, length);
	source[length] = '\0';
	parts[0] = post_common;
	parts[1] = smaa_prefix;
	parts[2] = source;
	parts[3] = smaa_mains;
	text = joined(parts, 4);
	free(source);
	for (pass = 0; pass < 3; pass++)
	{
		post.pixels[_pass_smaa_edges + pass] = renderer_compile(text, entries[pass], "ps_5_0", entries[pass]);
		if (post.pixels[_pass_smaa_edges + pass])
			post.pipelines[_pass_smaa_edges + pass] = pass_pipeline(_pass_smaa_edges + pass);
		if (!post.pipelines[_pass_smaa_edges + pass])
		{
			post.smaa_failed = TRUE;
			renderer_log("anti-aliasing: SMAA does not build, so the 3D view is not antialiased");
			break;
		}
	}
	free(text);
	post.smaa_ready = !post.smaa_failed;
}

/* the passes' textures, of the target's size */
static ID3D12Resource *intermediate(DXGI_FORMAT format, BOOL renderable, D3D12_RESOURCE_STATES state,
	struct renderer_descriptor *srv, struct renderer_descriptor *rtv)
{
	D3D12_HEAP_PROPERTIES heap;
	D3D12_RESOURCE_DESC description;
	D3D12_CLEAR_VALUE clear;
	ID3D12Resource *resource;

	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	memset(&description, 0, sizeof(description));
	description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	description.Width = post.width;
	description.Height = post.height;
	description.DepthOrArraySize = 1;
	description.MipLevels = 1;
	description.Format = format;
	description.SampleDesc.Count = 1;
	description.Flags = renderable ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
	memset(&clear, 0, sizeof(clear));
	clear.Format = format;
	RENDERER_CHECK(ID3D12Device_CreateCommittedResource(renderer.device, &heap, D3D12_HEAP_FLAG_NONE, &description,
		state, renderable ? &clear : NULL, &IID_ID3D12Resource, (void **)&resource));
	if (!srv->cpu.ptr)
		*srv = renderer_view_allocate();
	ID3D12Device_CreateShaderResourceView(renderer.device, resource, NULL, srv->cpu);
	if (rtv)
	{
		if (!rtv->cpu.ptr)
			*rtv = renderer_rtv_allocate();
		ID3D12Device_CreateRenderTargetView(renderer.device, resource, NULL, rtv->cpu);
	}
	return resource;
}

static void intermediates_fit(BOOL smaa, uint32_t width, uint32_t height)
{
	if (post.width != width || post.height != height)
	{
		renderer_release_later((IUnknown *)post.color);
		renderer_release_later((IUnknown *)post.edges);
		renderer_release_later((IUnknown *)post.weights);
		post.color = post.edges = post.weights = NULL;
		post.width = width;
		post.height = height;
		post.color_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		post.color = intermediate(DXGI_FORMAT_B8G8R8A8_UNORM, FALSE, post.color_state, &post.color_srv, NULL);
	}
	if (smaa && !post.edges)
	{
		post.edges_state = post.weights_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		post.edges = intermediate(DXGI_FORMAT_R8G8_UNORM, TRUE, post.edges_state, &post.edges_srv, &post.edges_rtv);
		post.weights = intermediate(DXGI_FORMAT_R8G8B8A8_UNORM, TRUE, post.weights_state, &post.weights_srv,
			&post.weights_rtv);
	}
}

static void move(ID3D12Resource *resource, D3D12_RESOURCE_STATES *state, D3D12_RESOURCE_STATES wanted)
{
	renderer_transition(resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, *state, wanted);
	*state = wanted;
}

/* a pass's triangle over the rectangle of target, reading the textures */
static void pass_draw(int which, D3D12_CPU_DESCRIPTOR_HANDLE target, const int32_t rectangle[4],
	D3D12_GPU_DESCRIPTOR_HANDLE first, D3D12_GPU_DESCRIPTOR_HANDLE second, D3D12_GPU_DESCRIPTOR_HANDLE third)
{
	float constants[8];
	D3D12_VIEWPORT viewport;
	D3D12_RECT scissor;

	constants[0] = 1.0f / (float)post.width;
	constants[1] = 1.0f / (float)post.height;
	constants[2] = (float)post.width;
	constants[3] = (float)post.height;
	/* (FXAA's bounds: no texel of another split-screen window is read) */
	constants[4] = ((float)rectangle[0] + 0.5f) / (float)post.width;
	constants[5] = ((float)rectangle[1] + 0.5f) / (float)post.height;
	constants[6] = ((float)rectangle[2] - 0.5f) / (float)post.width;
	constants[7] = ((float)rectangle[3] - 0.5f) / (float)post.height;
	viewport.TopLeftX = (float)rectangle[0];
	viewport.TopLeftY = (float)rectangle[1];
	viewport.Width = (float)(rectangle[2] - rectangle[0]);
	viewport.Height = (float)(rectangle[3] - rectangle[1]);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	scissor.left = rectangle[0];
	scissor.top = rectangle[1];
	scissor.right = rectangle[2];
	scissor.bottom = rectangle[3];
	ID3D12GraphicsCommandList_SetGraphicsRootSignature(renderer.list, post.signature);
	ID3D12GraphicsCommandList_SetPipelineState(renderer.list, post.pipelines[which]);
	ID3D12GraphicsCommandList_OMSetRenderTargets(renderer.list, 1, &target, FALSE, NULL);
	ID3D12GraphicsCommandList_RSSetViewports(renderer.list, 1, &viewport);
	ID3D12GraphicsCommandList_RSSetScissorRects(renderer.list, 1, &scissor);
	ID3D12GraphicsCommandList_IASetPrimitiveTopology(renderer.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(renderer.list, 0, 8, constants, 0);
	ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(renderer.list, 1, first);
	ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(renderer.list, 2, second);
	ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(renderer.list, 3, third);
	ID3D12GraphicsCommandList_DrawInstanced(renderer.list, 3, 1, 0, 0);
}

void renderer_anti_alias(const struct xgpu_anti_alias *anti_alias)
{
	struct renderer_target *target = renderer_target_get(anti_alias->target);
	D3D12_GPU_DESCRIPTOR_HANDLE none = renderer_null_srv(_xgpu_texture_2d).gpu;
	BOOL smaa = anti_alias->mode == _xgpu_anti_alias_smaa;
	int32_t rectangle[4];
	int index;

	if (!target || target->definition.depth || target->multisampled)
		return;
	if (smaa ? !post.smaa_ready : !fxaa_prepare())
		return;
	for (index = 0; index < 4; index++)
		rectangle[index] = anti_alias->rectangle[index];
	/* (within the target, and something left) */
	if (rectangle[0] < 0) rectangle[0] = 0;
	if (rectangle[1] < 0) rectangle[1] = 0;
	if (rectangle[2] > (int32_t)target->definition.width) rectangle[2] = (int32_t)target->definition.width;
	if (rectangle[3] > (int32_t)target->definition.height) rectangle[3] = (int32_t)target->definition.height;
	if (rectangle[0] >= rectangle[2] || rectangle[1] >= rectangle[3])
		return;
	intermediates_fit(smaa, target->definition.width, target->definition.height);

	/* the target's pixels, which the passes read while it is drawn into */
	renderer_target_state(target, D3D12_RESOURCE_STATE_COPY_SOURCE);
	move(post.color, &post.color_state, D3D12_RESOURCE_STATE_COPY_DEST);
	ID3D12GraphicsCommandList_CopyResource(renderer.list, post.color, target->resource);
	move(post.color, &post.color_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	if (!smaa)
	{
		renderer_target_state(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
		pass_draw(_pass_fxaa, target->view.cpu, rectangle, post.color_srv.gpu, none, none);
	}
	else
	{
		static const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		struct renderer_texture *area = renderer_texture_get(post.area_texture);
		struct renderer_texture *search = renderer_texture_get(post.search_texture);

		if (!area || !search)
			return;
		renderer_texture_state(area, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		renderer_texture_state(search, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		/* (emptied whole, so that no edge of another window's pass is found:
		the passes write only where there is an edge) */
		move(post.edges, &post.edges_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
		ID3D12GraphicsCommandList_ClearRenderTargetView(renderer.list, post.edges_rtv.cpu, clear, 0, NULL);
		pass_draw(_pass_smaa_edges, post.edges_rtv.cpu, rectangle, post.color_srv.gpu, none, none);
		move(post.edges, &post.edges_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		move(post.weights, &post.weights_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
		ID3D12GraphicsCommandList_ClearRenderTargetView(renderer.list, post.weights_rtv.cpu, clear, 0, NULL);
		pass_draw(_pass_smaa_weights, post.weights_rtv.cpu, rectangle, post.edges_srv.gpu, area->srv.gpu,
			search->srv.gpu);
		move(post.weights, &post.weights_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		renderer_target_state(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
		pass_draw(_pass_smaa_blend, target->view.cpu, rectangle, post.color_srv.gpu, post.weights_srv.gpu, none);
	}
	renderer_draw_invalidate();
}

/* ---------- a lost device: the compiled shaders are kept, the signature,
pipelines and textures made again (the textures as the next pass needs them) */

void renderer_post_lost(void)
{
	int which;

	if (post.signature)
		ID3D12RootSignature_Release(post.signature);
	post.signature = NULL;
	for (which = 0; which < NUMBER_OF_PASSES; which++)
	{
		if (post.pipelines[which])
			ID3D12PipelineState_Release(post.pipelines[which]);
		post.pipelines[which] = NULL;
	}
	if (post.color)
		ID3D12Resource_Release(post.color);
	if (post.edges)
		ID3D12Resource_Release(post.edges);
	if (post.weights)
		ID3D12Resource_Release(post.weights);
	post.color = post.edges = post.weights = NULL;
	post.width = post.height = 0;
	memset(&post.color_srv, 0, sizeof(post.color_srv));
	memset(&post.edges_srv, 0, sizeof(post.edges_srv));
	memset(&post.weights_srv, 0, sizeof(post.weights_srv));
	memset(&post.edges_rtv, 0, sizeof(post.edges_rtv));
	memset(&post.weights_rtv, 0, sizeof(post.weights_rtv));
}

void renderer_post_restore(void)
{
	int which;

	/* (nothing made yet: the passes were never used) */
	if (!post.vertex)
		return;
	signature_create();
	for (which = 0; which < NUMBER_OF_PASSES; which++)
	{
		if (post.pixels[which])
			post.pipelines[which] = pass_pipeline(which);
	}
}
