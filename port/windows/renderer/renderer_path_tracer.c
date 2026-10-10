/*
RENDERER_PATH_TRACER.C

display.path_tracing: the 3D world drawn by a path tracer, with DXR 1.1's
inline ray tracing (RayQuery) in compute shaders of Shader Model 6.5, which
the Windows SDK's dxc.exe compiles when the renderer is built
(port/windows/renderer/shaders; tools/embed_dxil.py embeds them).

Whether the GPU can (D3D12_RAYTRACING_TIER_1_1, and the shaders built) is
told to the game in the shared header, and the GPU's ray tracing is checked
the first time the setting is on: a scene of one triangle, an acceleration
structure of it, and a ray from each texel of a small target, whose hits
and misses are read back and compared with what they must be.

So far the scene (renderer_scene.c) is traced for the debug views
(debug.path_tracing_view) only: rays from the camera, one through each
pixel of the 3D view, at the end of the pass of the environment's lightmaps
(the game's depth buffer then holds the opaque world, for the depth parity
view to compare with), drawn over the 3D view at its end. The depth parity
counts are read back once the GPU is done with them, and written to the log
every TRACE_LOG_FRAMES frames traced.
*/

#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RENDERER_PATH_TRACER_SHADERS
/* renderer_shaders.c (tools/embed_dxil.py) */
extern const unsigned char renderer_dxil_pt_self_test[];
extern const size_t renderer_dxil_pt_self_test_size;
extern const unsigned char renderer_dxil_pt_trace[];
extern const size_t renderer_dxil_pt_trace_size;
#endif

#define TRACE_LOG_FRAMES 30
#define PARITY_KINDS 6
/* the counts of pixels in front for each geometry, after the kinds' */
#define PARITY_GEOMETRIES 4096
#define PARITY_COUNTS (PARITY_KINDS + PARITY_GEOMETRIES)

static struct
{
	/* the GPU traces rays, and the shaders were built */
	BOOL available;
	/* the setting */
	BOOL wanted;
	/* the self test has run (once a process: it checks the GPU), and passed */
	BOOL checked, working;
	ID3D12Device5 *device;
	/* debug.path_tracing_view */
	uint32_t view;

	ID3D12RootSignature *trace_signature, *composite_signature;
	ID3D12PipelineState *trace_pipeline, *composite_pipeline;
	BOOL pipelines_failed;
	/* the trace's texture, of the viewport's size */
	ID3D12Resource *output;
	struct renderer_descriptor output_uav, output_srv;
	uint32_t output_width, output_height;
	/* the depth parity counts, and their copies read back */
	ID3D12Resource *counts;
	struct
	{
		ID3D12Resource *buffer;
		uint64_t fence;
	} count_readbacks[RENDERER_FRAME_COUNT];
	uint32_t count_next;
	uint64_t count_totals[PARITY_COUNTS];
	uint32_t count_frames;

	/* this window was traced: its target and viewport */
	BOOL traced;
	uint32_t traced_target;
	uint32_t traced_viewport[4];
} tracer;

BOOL renderer_path_tracing(void)
{
	return tracer.wanted && tracer.working;
}

/* ---------- buffers and acceleration structures */

/* a buffer in the GPU's memory that shaders and builds can write */
static ID3D12Resource *buffer_writable(uint64_t size, D3D12_RESOURCE_STATES state)
{
	D3D12_HEAP_PROPERTIES heap;
	D3D12_RESOURCE_DESC description;
	ID3D12Resource *resource;

	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	memset(&description, 0, sizeof(description));
	description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	description.Width = size ? size : 256;
	description.Height = 1;
	description.DepthOrArraySize = 1;
	description.MipLevels = 1;
	description.SampleDesc.Count = 1;
	description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	RENDERER_CHECK(ID3D12Device_CreateCommittedResource(renderer.device, &heap, D3D12_HEAP_FLAG_NONE, &description,
		state, NULL, &IID_ID3D12Resource, (void **)&resource));
	return resource;
}

static void barrier_uav(ID3D12Resource *resource)
{
	D3D12_RESOURCE_BARRIER barrier;

	memset(&barrier, 0, sizeof(barrier));
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = resource;
	ID3D12GraphicsCommandList_ResourceBarrier(renderer.list, 1, &barrier);
}

ID3D12Resource *renderer_acceleration_structure_build(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *inputs)
{
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes;
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build;
	ID3D12GraphicsCommandList4 *list;
	ID3D12Resource *result, *scratch;

	ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(tracer.device, inputs, &sizes);
	result = buffer_writable(sizes.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
	/* (a buffer is made in the common state, and promoted to what the build
	writes it as) */
	scratch = buffer_writable(sizes.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON);
	memset(&build, 0, sizeof(build));
	build.Inputs = *inputs;
	build.DestAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(result);
	build.ScratchAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(scratch);
	RENDERER_CHECK(ID3D12GraphicsCommandList_QueryInterface(renderer.list, &IID_ID3D12GraphicsCommandList4,
		(void **)&list));
	ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(list, &build, 0, NULL);
	ID3D12GraphicsCommandList4_Release(list);
	/* (what reads it waits for the build) */
	barrier_uav(result);
	renderer_release_later((IUnknown *)scratch);
	return result;
}

/* ---------- the self test */

#define SELF_TEST_SIZE 8

/* the test's compute pipeline, from a shader with its root signature */
static BOOL compute_pipeline(const unsigned char *code, size_t size, ID3D12RootSignature **signature,
	ID3D12PipelineState **pipeline)
{
	D3D12_COMPUTE_PIPELINE_STATE_DESC description;

	if (FAILED(ID3D12Device_CreateRootSignature(renderer.device, 0, code, size, &IID_ID3D12RootSignature,
		(void **)signature)))
	{
		return FALSE;
	}
	memset(&description, 0, sizeof(description));
	description.pRootSignature = *signature;
	description.CS.pShaderBytecode = code;
	description.CS.BytecodeLength = size;
	if (FAILED(ID3D12Device_CreateComputePipelineState(renderer.device, &description, &IID_ID3D12PipelineState,
		(void **)pipeline)))
	{
		ID3D12RootSignature_Release(*signature);
		return FALSE;
	}
	return TRUE;
}

static BOOL self_test(void)
{
#ifdef RENDERER_PATH_TRACER_SHADERS
	static const float triangle[3][3] = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 8.0f, 0.0f }, { 8.0f, 8.0f, 0.0f } };
	D3D12_RAYTRACING_GEOMETRY_DESC geometry;
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs;
	D3D12_RAYTRACING_INSTANCE_DESC *instance;
	struct renderer_upload vertices, instances;
	ID3D12Resource *bottom, *top, *results, *readback;
	ID3D12RootSignature *signature;
	ID3D12PipelineState *pipeline;
	struct renderer_descriptor view;
	D3D12_HEAP_PROPERTIES heap;
	D3D12_RESOURCE_DESC description;
	D3D12_UNORDERED_ACCESS_VIEW_DESC view_description;
	D3D12_TEXTURE_COPY_LOCATION source, destination;
	const float *texels;
	D3D12_RANGE range;
	int x, y, hits = 0, wrong = 0;

	if (!compute_pipeline(renderer_dxil_pt_self_test, renderer_dxil_pt_self_test_size, &signature, &pipeline))
	{
		renderer_log("path tracing: the self test's shader cannot be made");
		return FALSE;
	}

	/* the triangle's acceleration structure, and the scene's of it */
	vertices = renderer_upload_allocate(sizeof(triangle), 16);
	memcpy(vertices.data, triangle, sizeof(triangle));
	memset(&geometry, 0, sizeof(geometry));
	geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
	geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
	geometry.Triangles.VertexCount = 3;
	geometry.Triangles.VertexBuffer.StartAddress = vertices.address;
	geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(triangle[0]);
	memset(&inputs, 0, sizeof(inputs));
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	inputs.NumDescs = 1;
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.pGeometryDescs = &geometry;
	bottom = renderer_acceleration_structure_build(&inputs);

	instances = renderer_upload_allocate(sizeof(*instance), D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);
	instance = instances.data;
	memset(instance, 0, sizeof(*instance));
	instance->Transform[0][0] = instance->Transform[1][1] = instance->Transform[2][2] = 1.0f;
	instance->InstanceMask = 0xff;
	instance->AccelerationStructure = ID3D12Resource_GetGPUVirtualAddress(bottom);
	memset(&inputs, 0, sizeof(inputs));
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	inputs.NumDescs = 1;
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.InstanceDescs = instances.address;
	top = renderer_acceleration_structure_build(&inputs);

	/* the rays, into a small target, read back */
	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	memset(&description, 0, sizeof(description));
	description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	description.Width = SELF_TEST_SIZE;
	description.Height = SELF_TEST_SIZE;
	description.DepthOrArraySize = 1;
	description.MipLevels = 1;
	description.Format = DXGI_FORMAT_R32G32_FLOAT;
	description.SampleDesc.Count = 1;
	description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	RENDERER_CHECK(ID3D12Device_CreateCommittedResource(renderer.device, &heap, D3D12_HEAP_FLAG_NONE, &description,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&results));
	view = renderer_view_allocate();
	memset(&view_description, 0, sizeof(view_description));
	view_description.Format = DXGI_FORMAT_R32G32_FLOAT;
	view_description.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	ID3D12Device_CreateUnorderedAccessView(renderer.device, results, NULL, &view_description, view.cpu);
	readback = renderer_buffer_create(D3D12_HEAP_TYPE_READBACK, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * SELF_TEST_SIZE,
		D3D12_RESOURCE_STATE_COPY_DEST);

	ID3D12GraphicsCommandList_SetComputeRootSignature(renderer.list, signature);
	ID3D12GraphicsCommandList_SetPipelineState(renderer.list, pipeline);
	ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(renderer.list, 0, ID3D12Resource_GetGPUVirtualAddress(top));
	ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(renderer.list, 1, view.gpu);
	ID3D12GraphicsCommandList_Dispatch(renderer.list, 1, 1, 1);
	renderer_transition(results, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_COPY_SOURCE);
	memset(&source, 0, sizeof(source));
	source.pResource = results;
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	memset(&destination, 0, sizeof(destination));
	destination.pResource = readback;
	destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32_FLOAT;
	destination.PlacedFootprint.Footprint.Width = SELF_TEST_SIZE;
	destination.PlacedFootprint.Footprint.Height = SELF_TEST_SIZE;
	destination.PlacedFootprint.Footprint.Depth = 1;
	destination.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
	ID3D12GraphicsCommandList_CopyTextureRegion(renderer.list, &destination, 0, 0, 0, &source, NULL);
	/* (the draws after this set their pipeline again) */
	renderer_draw_invalidate();
	renderer_wait(renderer_submit());

	/* a texel whose ray passes x < y hits the triangle 1 unit away; x > y
	misses (x = y is on its edge: either) */
	range.Begin = 0;
	range.End = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * SELF_TEST_SIZE;
	if (SUCCEEDED(ID3D12Resource_Map(readback, 0, &range, (void **)&texels)))
	{
		for (y = 0; y < SELF_TEST_SIZE; y++)
		{
			for (x = 0; x < SELF_TEST_SIZE; x++)
			{
				const float *texel = (const float *)((const uint8_t *)texels + y * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) +
					x * 2;

				hits += texel[0] == 1.0f;
				if (x < y ? texel[0] != 1.0f || texel[1] < 0.999f || texel[1] > 1.001f :
					x > y ? texel[0] != 0.0f : FALSE)
				{
					wrong++;
				}
			}
		}
		range.End = 0;
		ID3D12Resource_Unmap(readback, 0, &range);
	}
	else
	{
		wrong = -1;
	}
	renderer_view_free(view);
	ID3D12Resource_Release(readback);
	ID3D12Resource_Release(results);
	ID3D12Resource_Release(top);
	ID3D12Resource_Release(bottom);
	ID3D12PipelineState_Release(pipeline);
	ID3D12RootSignature_Release(signature);
	if (wrong)
	{
		renderer_log("path tracing: the GPU's self test failed (%d of %d rays wrong)", wrong,
			SELF_TEST_SIZE * SELF_TEST_SIZE);
		return FALSE;
	}
	renderer_log("path tracing: the GPU's self test passed (%d of %d rays hit, as they must)", hits,
		SELF_TEST_SIZE * SELF_TEST_SIZE);
	return TRUE;
#else
	return FALSE;
#endif
}

/* ---------- the trace */

/* a 4x4 matrix's inverse (rows by columns); FALSE if it has none */
static BOOL matrix_inverse(const float m[4][4], float result[4][4])
{
	double a[4][8];
	int row, column, pivot, other;

	for (row = 0; row < 4; row++)
	{
		for (column = 0; column < 4; column++)
		{
			a[row][column] = m[row][column];
			a[row][column + 4] = row == column;
		}
	}
	for (column = 0; column < 4; column++)
	{
		double scale;

		pivot = column;
		for (row = column + 1; row < 4; row++)
		{
			if ((a[row][column] < 0 ? -a[row][column] : a[row][column]) >
				(a[pivot][column] < 0 ? -a[pivot][column] : a[pivot][column]))
			{
				pivot = row;
			}
		}
		if (a[pivot][column] == 0.0)
			return FALSE;
		if (pivot != column)
		{
			for (other = 0; other < 8; other++)
			{
				double swap = a[column][other];

				a[column][other] = a[pivot][other];
				a[pivot][other] = swap;
			}
		}
		scale = 1.0 / a[column][column];
		for (other = 0; other < 8; other++)
			a[column][other] *= scale;
		for (row = 0; row < 4; row++)
		{
			double factor = a[row][column];

			if (row == column || factor == 0.0)
				continue;
			for (other = 0; other < 8; other++)
				a[row][other] -= factor * a[column][other];
		}
	}
	for (row = 0; row < 4; row++)
	{
		for (column = 0; column < 4; column++)
			result[row][column] = (float)a[row][column + 4];
	}
	return TRUE;
}

static const char composite_shaders[] =
	"Texture2D<float4> traced : register(t0);\n"
	"cbuffer composite_constants : register(b0) { float4 viewport; };\n"
	/* a triangle that covers the viewport */
	"float4 composite_vertex(uint vertex : SV_VertexID) : SV_Position\n"
	"{\n"
	"\tfloat2 uv = float2((vertex << 1) & 2, vertex & 2);\n"
	"\treturn float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
	"}\n"
	/* the trace's pixel where it has one */
	"float4 composite_pixel(float4 position : SV_Position) : SV_Target0\n"
	"{\n"
	"\tfloat4 color = traced.Load(int3(position.xy - viewport.xy, 0));\n"
	"\tif (color.a == 0.0)\n"
	"\t\tdiscard;\n"
	"\treturn float4(color.rgb, 1.0);\n"
	"}\n";

static BOOL pipelines_make(void)
{
#ifdef RENDERER_PATH_TRACER_SHADERS
	D3D12_ROOT_PARAMETER parameters[2];
	D3D12_DESCRIPTOR_RANGE range;
	D3D12_ROOT_SIGNATURE_DESC description;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline;
	ID3DBlob *blob = NULL, *errors = NULL, *vertex, *pixel;
	BOOL made;

	if (tracer.trace_pipeline || tracer.pipelines_failed)
		return !tracer.pipelines_failed;
	tracer.pipelines_failed = TRUE;
	if (!compute_pipeline(renderer_dxil_pt_trace, renderer_dxil_pt_trace_size, &tracer.trace_signature,
		&tracer.trace_pipeline))
	{
		renderer_log("path tracing: the trace's shader cannot be made");
		return FALSE;
	}

	memset(parameters, 0, sizeof(parameters));
	parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[0].Constants.Num32BitValues = 4;
	parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	memset(&range, 0, sizeof(range));
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = 1;
	parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameters[1].DescriptorTable.NumDescriptorRanges = 1;
	parameters[1].DescriptorTable.pDescriptorRanges = &range;
	parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	memset(&description, 0, sizeof(description));
	description.NumParameters = 2;
	description.pParameters = parameters;
	if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)))
		renderer_fail("the path tracer's composite root signature does not serialize");
	RENDERER_CHECK(ID3D12Device_CreateRootSignature(renderer.device, 0, ID3D10Blob_GetBufferPointer(blob),
		ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&tracer.composite_signature));
	ID3D10Blob_Release(blob);
	if (errors)
		ID3D10Blob_Release(errors);
	vertex = renderer_compile(composite_shaders, "composite_vertex", "vs_5_0", "the path tracer's composite");
	pixel = renderer_compile(composite_shaders, "composite_pixel", "ps_5_0", "the path tracer's composite");
	made = FALSE;
	if (vertex && pixel)
	{
		memset(&pipeline, 0, sizeof(pipeline));
		pipeline.pRootSignature = tracer.composite_signature;
		pipeline.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vertex);
		pipeline.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vertex);
		pipeline.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(pixel);
		pipeline.PS.BytecodeLength = ID3D10Blob_GetBufferSize(pixel);
		/* (colour only: the game keeps its own values in alpha) */
		pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask =
			D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
		pipeline.BlendState.RenderTarget[0].SrcBlend = pipeline.BlendState.RenderTarget[0].SrcBlendAlpha =
			D3D12_BLEND_ONE;
		pipeline.BlendState.RenderTarget[0].DestBlend = pipeline.BlendState.RenderTarget[0].DestBlendAlpha =
			D3D12_BLEND_ZERO;
		pipeline.BlendState.RenderTarget[0].BlendOp = pipeline.BlendState.RenderTarget[0].BlendOpAlpha =
			D3D12_BLEND_OP_ADD;
		pipeline.SampleMask = 0xffffffffu;
		pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pipeline.RasterizerState.DepthClipEnable = TRUE;
		pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pipeline.NumRenderTargets = 1;
		pipeline.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
		pipeline.SampleDesc.Count = 1;
		made = SUCCEEDED(ID3D12Device_CreateGraphicsPipelineState(renderer.device, &pipeline, &IID_ID3D12PipelineState,
			(void **)&tracer.composite_pipeline));
	}
	if (vertex)
		ID3D10Blob_Release(vertex);
	if (pixel)
		ID3D10Blob_Release(pixel);
	if (!made)
	{
		renderer_log("path tracing: the composite's pipeline cannot be made");
		return FALSE;
	}

	tracer.counts = buffer_writable(PARITY_COUNTS * sizeof(uint32_t), D3D12_RESOURCE_STATE_COMMON);
	tracer.pipelines_failed = FALSE;
	return TRUE;
#else
	return FALSE;
#endif
}

/* the trace's texture, of the viewport's size */
static void output_fit(uint32_t width, uint32_t height)
{
	D3D12_HEAP_PROPERTIES heap;
	D3D12_RESOURCE_DESC description;

	if (tracer.output && tracer.output_width == width && tracer.output_height == height)
		return;
	renderer_release_later((IUnknown *)tracer.output);
	/* (new views: the old ones may be read by work in flight) */
	if (tracer.output_uav.cpu.ptr)
		renderer_view_free(tracer.output_uav);
	if (tracer.output_srv.cpu.ptr)
		renderer_view_free(tracer.output_srv);
	memset(&tracer.output_uav, 0, sizeof(tracer.output_uav));
	memset(&tracer.output_srv, 0, sizeof(tracer.output_srv));
	memset(&heap, 0, sizeof(heap));
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	memset(&description, 0, sizeof(description));
	description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	description.Width = width;
	description.Height = height;
	description.DepthOrArraySize = 1;
	description.MipLevels = 1;
	description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	description.SampleDesc.Count = 1;
	description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	RENDERER_CHECK(ID3D12Device_CreateCommittedResource(renderer.device, &heap, D3D12_HEAP_FLAG_NONE, &description,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&tracer.output));
	if (!tracer.output_uav.cpu.ptr)
		tracer.output_uav = renderer_view_allocate();
	if (!tracer.output_srv.cpu.ptr)
		tracer.output_srv = renderer_view_allocate();
	ID3D12Device_CreateUnorderedAccessView(renderer.device, tracer.output, NULL, NULL, tracer.output_uav.cpu);
	ID3D12Device_CreateShaderResourceView(renderer.device, tracer.output, NULL, tracer.output_srv.cpu);
	tracer.output_width = width;
	tracer.output_height = height;
}

/* the trace's constants (cbuffer trace_constants, shaders/pt_trace.hlsl) */
struct trace_constants
{
	float clip_to_world[4][4];
	float camera_position[4];
	uint32_t viewport[4];
	uint32_t options[4];
	float pixel_offset[4];
};

static void trace(void)
{
	const struct xgpu_draw_state *state = renderer_draw_state_current();
	const struct xgpu_scene_camera *camera = renderer_scene_camera_get();
	struct renderer_target *color, *depth;
	struct trace_constants *constants;
	struct renderer_upload upload;
	D3D12_GPU_VIRTUAL_ADDRESS top, vertices, indices, geometries;
	int32_t x, y, width, height;

	if (!state || !camera || !camera->primary || !pipelines_make() || !renderer_scene_prepare())
		return;
	color = renderer_target_get(state->color_target);
	depth = renderer_target_get(state->depth_target);
	if (!color || color->definition.depth)
		return;
	if (depth && (!depth->definition.depth || depth->multisampled))
		depth = NULL;
	/* (the viewport, within the target) */
	x = (int32_t)state->viewport[0];
	y = (int32_t)state->viewport[1];
	width = (int32_t)state->viewport[2];
	height = (int32_t)state->viewport[3];
	if (x < 0)
	{
		width += x;
		x = 0;
	}
	if (y < 0)
	{
		height += y;
		y = 0;
	}
	if (x + width > (int32_t)color->definition.width)
		width = (int32_t)color->definition.width - x;
	if (y + height > (int32_t)color->definition.height)
		height = (int32_t)color->definition.height - y;
	if (width <= 0 || height <= 0)
		return;
	output_fit((uint32_t)width, (uint32_t)height);

	upload = renderer_upload_allocate(sizeof(*constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
	constants = upload.data;
	memset(constants, 0, sizeof(*constants));
	if (!matrix_inverse(camera->world_to_clip, constants->clip_to_world))
		return;
	memcpy(constants->camera_position, camera->position, sizeof(camera->position));
	constants->viewport[0] = (uint32_t)x;
	constants->viewport[1] = (uint32_t)y;
	constants->viewport[2] = (uint32_t)width;
	constants->viewport[3] = (uint32_t)height;
	constants->options[0] = tracer.view;
	constants->options[1] = depth != NULL;
	/* (the game's pixels are centred on whole pixels, as Direct3D 8 has
	them: its image lies half of one of its pixels to the right of and below
	where Direct3D 12's centres would put it) */
	if (camera->game_viewport[2] > camera->game_viewport[0] && camera->game_viewport[3] > camera->game_viewport[1])
	{
		constants->pixel_offset[0] = 0.5f * (float)width / (camera->game_viewport[2] - camera->game_viewport[0]);
		constants->pixel_offset[1] = 0.5f * (float)height / (camera->game_viewport[3] - camera->game_viewport[1]);
	}

	/* (the counts start at zero) */
	{
		struct renderer_upload zeros = renderer_upload_allocate(PARITY_COUNTS * sizeof(uint32_t), 16);

		memset(zeros.data, 0, PARITY_COUNTS * sizeof(uint32_t));
		ID3D12GraphicsCommandList_CopyBufferRegion(renderer.list, tracer.counts, 0, zeros.buffer, zeros.offset,
			PARITY_COUNTS * sizeof(uint32_t));
		renderer_transition(tracer.counts, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
	if (depth)
		renderer_target_state(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	renderer_scene_buffers(&top, &vertices, &indices, &geometries);
	ID3D12GraphicsCommandList_SetComputeRootSignature(renderer.list, tracer.trace_signature);
	ID3D12GraphicsCommandList_SetPipelineState(renderer.list, tracer.trace_pipeline);
	ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(renderer.list, 0, upload.address);
	ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(renderer.list, 1, top);
	ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(renderer.list, 2, vertices);
	ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(renderer.list, 3, indices);
	ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(renderer.list, 4, geometries);
	ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(renderer.list, 5, renderer_scene_materials());
	ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(renderer.list, 6, renderer_view_gpu_start());
	ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(renderer.list, 7, tracer.output_uav.gpu);
	ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(renderer.list, 8,
		ID3D12Resource_GetGPUVirtualAddress(tracer.counts));
	ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(renderer.list, 9,
		depth ? depth->srv.gpu : renderer_null_srv(_xgpu_texture_2d).gpu);
	ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(renderer.list, 10, renderer_scene_drawn_bits());
	ID3D12GraphicsCommandList_Dispatch(renderer.list, ((uint32_t)width + 7) / 8, ((uint32_t)height + 7) / 8, 1);
	barrier_uav(tracer.output);

	/* the counts, copied out to be read once the GPU is done */
	renderer_transition(tracer.counts, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_COPY_SOURCE);
	if (tracer.view == _xgpu_path_tracing_view_depth_parity && !tracer.count_readbacks[tracer.count_next].fence)
	{
		uint32_t slot = tracer.count_next;

		if (!tracer.count_readbacks[slot].buffer)
			tracer.count_readbacks[slot].buffer = renderer_buffer_create(D3D12_HEAP_TYPE_READBACK,
				PARITY_COUNTS * sizeof(uint32_t), D3D12_RESOURCE_STATE_COPY_DEST);
		ID3D12GraphicsCommandList_CopyBufferRegion(renderer.list, tracer.count_readbacks[slot].buffer, 0,
			tracer.counts, 0, PARITY_COUNTS * sizeof(uint32_t));
		tracer.count_readbacks[slot].fence = renderer.fence_value + 1;
		tracer.count_next = (slot + 1) % RENDERER_FRAME_COUNT;
	}
	/* (as a buffer is made: common, which the next copy promotes from) */
	renderer_transition(tracer.counts, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE,
		D3D12_RESOURCE_STATE_COMMON);
	renderer_draw_invalidate();

	tracer.traced = TRUE;
	tracer.traced_target = state->color_target;
	tracer.traced_viewport[0] = (uint32_t)x;
	tracer.traced_viewport[1] = (uint32_t)y;
	tracer.traced_viewport[2] = (uint32_t)width;
	tracer.traced_viewport[3] = (uint32_t)height;
}

/* the trace drawn over the 3D view, where it has pixels */
static void composite(void)
{
	struct renderer_target *color = renderer_target_get(tracer.traced_target);
	D3D12_VIEWPORT viewport;
	D3D12_RECT scissor;
	float constants[4];

	tracer.traced = FALSE;
	if (!color || color->definition.depth || color->multisampled || !tracer.composite_pipeline)
		return;
	renderer_target_state(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderer_transition(tracer.output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	viewport.TopLeftX = (float)tracer.traced_viewport[0];
	viewport.TopLeftY = (float)tracer.traced_viewport[1];
	viewport.Width = (float)tracer.traced_viewport[2];
	viewport.Height = (float)tracer.traced_viewport[3];
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	scissor.left = (LONG)tracer.traced_viewport[0];
	scissor.top = (LONG)tracer.traced_viewport[1];
	scissor.right = (LONG)(tracer.traced_viewport[0] + tracer.traced_viewport[2]);
	scissor.bottom = (LONG)(tracer.traced_viewport[1] + tracer.traced_viewport[3]);
	constants[0] = viewport.TopLeftX;
	constants[1] = viewport.TopLeftY;
	constants[2] = constants[3] = 0.0f;
	ID3D12GraphicsCommandList_SetGraphicsRootSignature(renderer.list, tracer.composite_signature);
	ID3D12GraphicsCommandList_SetPipelineState(renderer.list, tracer.composite_pipeline);
	ID3D12GraphicsCommandList_OMSetRenderTargets(renderer.list, 1, &color->view.cpu, FALSE, NULL);
	ID3D12GraphicsCommandList_RSSetViewports(renderer.list, 1, &viewport);
	ID3D12GraphicsCommandList_RSSetScissorRects(renderer.list, 1, &scissor);
	ID3D12GraphicsCommandList_IASetPrimitiveTopology(renderer.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(renderer.list, 0, 4, constants, 0);
	ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(renderer.list, 1, tracer.output_srv.gpu);
	ID3D12GraphicsCommandList_DrawInstanced(renderer.list, 3, 1, 0, 0);
	renderer_transition(tracer.output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	renderer_draw_invalidate();
}

/* the depth parity counts of traces the GPU has done, added up, and logged
every TRACE_LOG_FRAMES of them */
static void counts_collect(void)
{
	static const char *const names[PARITY_KINDS] =
	{
		"agree", "behind", "in front", "missed", "extra", "not drawn by the game",
	};
	uint64_t completed = renderer_completed();
	uint32_t slot;

	for (slot = 0; slot < RENDERER_FRAME_COUNT; slot++)
	{
		const uint32_t *counts;
		D3D12_RANGE range;
		int kind;

		if (!tracer.count_readbacks[slot].fence || tracer.count_readbacks[slot].fence > completed)
			continue;
		tracer.count_readbacks[slot].fence = 0;
		range.Begin = 0;
		range.End = PARITY_COUNTS * sizeof(uint32_t);
		if (FAILED(ID3D12Resource_Map(tracer.count_readbacks[slot].buffer, 0, &range, (void **)&counts)))
			continue;
		for (kind = 0; kind < PARITY_COUNTS; kind++)
			tracer.count_totals[kind] += counts[kind];
		range.End = 0;
		ID3D12Resource_Unmap(tracer.count_readbacks[slot].buffer, 0, &range);
		if (++tracer.count_frames == TRACE_LOG_FRAMES)
		{
			uint64_t total = 0, judged;
			char line[256];
			int length;

			for (kind = 0; kind < PARITY_KINDS; kind++)
				total += tracer.count_totals[kind];
			/* (what is behind the game's surfaces is behind its objects, which
			the scene does not have yet, and what the game did not draw - its
			visibility leaves out some surfaces rays reach, and draws mirrors'
			reflections in their place - cannot be compared: not judged) */
			judged = total - tracer.count_totals[1] - tracer.count_totals[5];
			length = snprintf(line, sizeof(line), "path tracing: depth parity over %u frames: %.2f%% agree of the "
				"pixels judged (", tracer.count_frames, judged ? 100.0 * tracer.count_totals[0] / judged : 0.0);
			for (kind = 0; kind < PARITY_KINDS && length > 0 && length < (int)sizeof(line); kind++)
			{
				length += snprintf(line + length, sizeof(line) - length, "%s%s %.2f%%", kind ? ", " : "", names[kind],
					total ? 100.0 * tracer.count_totals[kind] / total : 0.0);
			}
			renderer_log("%s)", line);
			/* (the scene's materials most in front, by the game's numbers: its
			log names them) */
			if (tracer.count_totals[2])
			{
				int rank;

				for (rank = 0; rank < 3; rank++)
				{
					uint32_t geometry, worst = 0;

					for (geometry = 1; geometry < PARITY_GEOMETRIES; geometry++)
					{
						if (tracer.count_totals[PARITY_KINDS + geometry] > tracer.count_totals[PARITY_KINDS + worst])
							worst = geometry;
					}
					if (!tracer.count_totals[PARITY_KINDS + worst])
						break;
					renderer_log("path tracing: in front on material %u: %.2f%% of the pixels",
						renderer_scene_geometry_material(worst),
						100.0 * tracer.count_totals[PARITY_KINDS + worst] / total);
					tracer.count_totals[PARITY_KINDS + worst] = 0;
				}
			}
			memset(tracer.count_totals, 0, sizeof(tracer.count_totals));
			tracer.count_frames = 0;
		}
	}
}

void renderer_path_tracer_view(uint32_t view)
{
	tracer.view = view < NUMBER_OF_XGPU_PATH_TRACING_VIEWS ? view : _xgpu_path_tracing_view_none;
}

void renderer_path_tracer_pass(const struct xgpu_pass *pass)
{
	if (!renderer_path_tracing() || tracer.view == _xgpu_path_tracing_view_none)
		return;
	if (pass->pass == XGPU_PASS_ENVIRONMENT_LIGHTMAPS && !pass->begin)
	{
		trace();
	}
	else if (pass->pass == XGPU_PASS_VIEW_END)
	{
		if (tracer.traced)
			composite();
		counts_collect();
	}
}

/* ---------- the renderer's */

void renderer_path_tracer_initialize(void)
{
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 options;

	if (!tracer.device)
		ID3D12Device_QueryInterface(renderer.device, &IID_ID3D12Device5, (void **)&tracer.device);
	memset(&options, 0, sizeof(options));
	if (tracer.device && SUCCEEDED(ID3D12Device_CheckFeatureSupport(renderer.device, D3D12_FEATURE_D3D12_OPTIONS5,
		&options, sizeof(options))))
	{
		tracer.available = options.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
	}
#ifndef RENDERER_PATH_TRACER_SHADERS
	tracer.available = FALSE;
#endif
	renderer_header->path_tracing_available = tracer.available;
}

void renderer_path_tracer_configure(BOOL wanted)
{
	if (wanted == tracer.wanted)
		return;
	tracer.wanted = wanted;
	if (!wanted)
	{
		renderer_log("path tracing: off");
		return;
	}
	if (!tracer.available)
	{
#ifdef RENDERER_PATH_TRACER_SHADERS
		renderer_log("path tracing: this GPU cannot trace rays (DXR 1.1), so the game is drawn as before");
#else
		renderer_log("path tracing: the renderer was built without its shaders (no dxc.exe), so the game is drawn "
			"as before");
#endif
		return;
	}
	if (!tracer.checked)
	{
		tracer.checked = TRUE;
		tracer.working = self_test();
	}
	renderer_log(tracer.working ? "path tracing: on" : "path tracing: not working, so the game is drawn as before");
}

/* ---------- a lost device */

void renderer_path_tracer_lost(void)
{
	uint32_t slot;

	if (tracer.device)
		ID3D12Device5_Release(tracer.device);
	tracer.device = NULL;
	if (tracer.trace_signature)
		ID3D12RootSignature_Release(tracer.trace_signature);
	if (tracer.trace_pipeline)
		ID3D12PipelineState_Release(tracer.trace_pipeline);
	if (tracer.composite_signature)
		ID3D12RootSignature_Release(tracer.composite_signature);
	if (tracer.composite_pipeline)
		ID3D12PipelineState_Release(tracer.composite_pipeline);
	if (tracer.output)
		ID3D12Resource_Release(tracer.output);
	if (tracer.counts)
		ID3D12Resource_Release(tracer.counts);
	for (slot = 0; slot < RENDERER_FRAME_COUNT; slot++)
	{
		if (tracer.count_readbacks[slot].buffer)
			ID3D12Resource_Release(tracer.count_readbacks[slot].buffer);
	}
	tracer.trace_signature = tracer.composite_signature = NULL;
	tracer.trace_pipeline = tracer.composite_pipeline = NULL;
	tracer.output = tracer.counts = NULL;
	memset(&tracer.output_uav, 0, sizeof(tracer.output_uav));
	memset(&tracer.output_srv, 0, sizeof(tracer.output_srv));
	tracer.output_width = tracer.output_height = 0;
	memset(tracer.count_readbacks, 0, sizeof(tracer.count_readbacks));
	tracer.traced = FALSE;
	tracer.pipelines_failed = FALSE;
}

void renderer_path_tracer_restore(void)
{
	renderer_path_tracer_initialize();
}
