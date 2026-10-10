/*
RENDERER_OUTPUT.C

The renderer's own passes and what leaves it: presentation (the back
buffer's target drawn into the swap chain's buffer, letterboxed to the
window, as d3d8_gl.c's display blit does), the blit that makes mip levels,
clears of some channels only, and screenshots,
read back and written once the frame's work is done (BMP for
debug.screenshot_every, PNG through WIC for the screenshot key).

These passes share a root signature of their own: four root constants, a
texture and two static samplers (linear and point, clamped).
*/

#include "renderer.h"

#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ID3D12RootSignature *pass_signature;

/* the blit into each format, and the clear of each channel mask and
samples a pixel (made when first used) */
#define BLIT_FORMATS 2
static ID3D12PipelineState *blit_pipelines[BLIT_FORMATS];
static ID3D12PipelineState *clear_pipelines[16][4];
static ID3DBlob *fullscreen_vertex, *blit_pixel, *clear_pixel;

static const char pass_shaders[] =
	"Texture2D source : register(t0);\n"
	"SamplerState linear_clamp : register(s0);\n"
	"cbuffer pass_constants : register(b0) { float4 pass_color; };\n"
	"struct pass_varyings { float4 position : SV_Position; float2 uv : TEXCOORD0; };\n"
	/* a triangle that covers the viewport */
	"pass_varyings fullscreen(uint vertex : SV_VertexID)\n"
	"{\n"
	"\tpass_varyings output;\n"
	"\toutput.uv = float2((vertex << 1) & 2, vertex & 2);\n"
	"\toutput.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
	"\treturn output;\n"
	"}\n"
	"float4 blit(pass_varyings input) : SV_Target0\n"
	"{\n"
	"\treturn source.Sample(linear_clamp, input.uv);\n"
	"}\n"
	"float4 clear_color(pass_varyings input) : SV_Target0\n"
	"{\n"
	"\treturn pass_color;\n"
	"}\n";

static void pass_signature_create(void)
{
	D3D12_ROOT_PARAMETER parameters[2];
	D3D12_DESCRIPTOR_RANGE range;
	D3D12_STATIC_SAMPLER_DESC samplers[2];
	D3D12_ROOT_SIGNATURE_DESC description;
	ID3DBlob *blob = NULL, *errors = NULL;

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
	memset(samplers, 0, sizeof(samplers));
	samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
	samplers[0].ShaderRegister = 0;
	samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	samplers[1] = samplers[0];
	samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	samplers[1].ShaderRegister = 1;
	memset(&description, 0, sizeof(description));
	description.NumParameters = 2;
	description.pParameters = parameters;
	description.NumStaticSamplers = 2;
	description.pStaticSamplers = samplers;
	if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)))
	{
		renderer_fail("the passes' root signature does not serialize: %s",
			errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "");
	}
	RENDERER_CHECK(ID3D12Device_CreateRootSignature(renderer.device, 0, ID3D10Blob_GetBufferPointer(blob),
		ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&pass_signature));
	ID3D10Blob_Release(blob);
	if (errors)
		ID3D10Blob_Release(errors);
}

static ID3D12PipelineState *pass_pipeline(ID3DBlob *pixel, DXGI_FORMAT format, uint32_t samples, uint8_t write_mask)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC description;
	ID3D12PipelineState *pipeline;

	memset(&description, 0, sizeof(description));
	description.pRootSignature = pass_signature;
	description.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(fullscreen_vertex);
	description.VS.BytecodeLength = ID3D10Blob_GetBufferSize(fullscreen_vertex);
	description.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(pixel);
	description.PS.BytecodeLength = ID3D10Blob_GetBufferSize(pixel);
	description.BlendState.RenderTarget[0].RenderTargetWriteMask = write_mask;
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
	description.RTVFormats[0] = format;
	description.SampleDesc.Count = samples;
	RENDERER_CHECK(ID3D12Device_CreateGraphicsPipelineState(renderer.device, &description, &IID_ID3D12PipelineState,
		(void **)&pipeline));
	return pipeline;
}

void renderer_output_initialize(void)
{
	pass_signature_create();
	fullscreen_vertex = renderer_compile(pass_shaders, "fullscreen", "vs_5_0", "the passes' vertex shader");
	blit_pixel = renderer_compile(pass_shaders, "blit", "ps_5_0", "the blit");
	clear_pixel = renderer_compile(pass_shaders, "clear_color", "ps_5_0", "the clear");
	if (!fullscreen_vertex || !blit_pixel || !clear_pixel)
		renderer_fail("the renderer's own shaders do not compile");
}

static void pass_begin(ID3D12PipelineState *pipeline, D3D12_CPU_DESCRIPTOR_HANDLE target, float x, float y,
	float width, float height)
{
	D3D12_VIEWPORT viewport;
	D3D12_RECT scissor;

	viewport.TopLeftX = x;
	viewport.TopLeftY = y;
	viewport.Width = width;
	viewport.Height = height;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	scissor.left = (LONG)x;
	scissor.top = (LONG)y;
	scissor.right = (LONG)(x + width + 0.5f);
	scissor.bottom = (LONG)(y + height + 0.5f);
	ID3D12GraphicsCommandList_SetGraphicsRootSignature(renderer.list, pass_signature);
	ID3D12GraphicsCommandList_SetPipelineState(renderer.list, pipeline);
	ID3D12GraphicsCommandList_OMSetRenderTargets(renderer.list, 1, &target, FALSE, NULL);
	ID3D12GraphicsCommandList_RSSetViewports(renderer.list, 1, &viewport);
	ID3D12GraphicsCommandList_RSSetScissorRects(renderer.list, 1, &scissor);
	ID3D12GraphicsCommandList_IASetPrimitiveTopology(renderer.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	/* (the draws after this set all of theirs again) */
	renderer_draw_invalidate();
}

void renderer_blit(D3D12_GPU_DESCRIPTOR_HANDLE source, D3D12_CPU_DESCRIPTOR_HANDLE target, DXGI_FORMAT format,
	float x, float y, float width, float height)
{
	int which = format == DXGI_FORMAT_R8G8B8A8_UNORM ? 1 : 0;

	if (!blit_pipelines[which])
		blit_pipelines[which] = pass_pipeline(blit_pixel, which ? DXGI_FORMAT_R8G8B8A8_UNORM :
			DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D12_COLOR_WRITE_ENABLE_ALL);
	pass_begin(blit_pipelines[which], target, x, y, width, height);
	ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(renderer.list, 1, source);
	ID3D12GraphicsCommandList_DrawInstanced(renderer.list, 3, 1, 0, 0);
}

void renderer_clear_channels(D3D12_CPU_DESCRIPTOR_HANDLE target, uint32_t samples, uint32_t width, uint32_t height,
	const D3D12_RECT *rectangles, uint32_t count, const float color[4], uint32_t mask)
{
	int sample_index = samples >= 8 ? 3 : samples >= 4 ? 2 : samples >= 2 ? 1 : 0;
	uint32_t index;

	mask &= 0xf;
	if (!clear_pipelines[mask][sample_index])
		clear_pipelines[mask][sample_index] = pass_pipeline(clear_pixel, DXGI_FORMAT_B8G8R8A8_UNORM, 1u << sample_index,
			(uint8_t)mask);
	pass_begin(clear_pipelines[mask][sample_index], target, 0.0f, 0.0f, (float)width, (float)height);
	ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(renderer.list, 0, 4, color, 0);
	for (index = 0; index < count; index++)
	{
		ID3D12GraphicsCommandList_RSSetScissorRects(renderer.list, 1, &rectangles[index]);
		ID3D12GraphicsCommandList_DrawInstanced(renderer.list, 3, 1, 0, 0);
	}
}

/* ---------- presentation */

void renderer_present(const struct xgpu_present *present)
{
	struct renderer_target *target = renderer_target_get(present->target);
	UINT index = IDXGISwapChain3_GetCurrentBackBufferIndex(renderer.swap_chain);
	static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

	renderer_transition(renderer.buffers[index], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	ID3D12GraphicsCommandList_ClearRenderTargetView(renderer.list, renderer.buffer_rtvs[index].cpu, black, 0, NULL);
	if (target && !target->definition.depth)
	{
		/* letterboxed to the target's shape */
		float width = (float)renderer.width;
		float height = (float)renderer.width * target->definition.height / target->definition.width;

		if (height > (float)renderer.height)
		{
			height = (float)renderer.height;
			width = (float)renderer.height * target->definition.width / target->definition.height;
		}
		renderer_target_state(target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		renderer_blit(target->srv.gpu, renderer.buffer_rtvs[index].cpu, DXGI_FORMAT_B8G8R8A8_UNORM,
			(float)(int)((renderer.width - width) / 2.0f), (float)(int)((renderer.height - height) / 2.0f), width, height);
	}
	renderer_visibility_resolve(present->frame);
	renderer_present_submit();
	InterlockedIncrement((volatile LONG *)&renderer_header->frames_presented);
	renderer_visibility_collect();
	renderer_screenshots_collect();
	renderer_header->frames_completed = (uint32_t)renderer_completed();
	renderer_draw_begin_frame();
}

/* ---------- screenshots */

struct screenshot
{
	ID3D12Resource *readback;
	uint32_t width, height, pitch;
	uint32_t format;
	char path[XGPU_PATH_SIZE];
	uint64_t fence;
};

#define SCREENSHOT_LIMIT 8

static struct screenshot screenshots[SCREENSHOT_LIMIT];

void renderer_screenshot(const struct xgpu_screenshot *request)
{
	struct renderer_target *target = renderer_target_get(request->target);
	struct screenshot *screenshot = NULL;
	D3D12_TEXTURE_COPY_LOCATION source, destination;
	int index;

	if (!target || target->definition.depth || !memchr(request->path, 0, sizeof(request->path)) ||
		(request->format != _xgpu_screenshot_bmp && request->format != _xgpu_screenshot_png))
	{
		renderer_log("a screenshot of target %u cannot be taken", request->target);
		return;
	}
	for (index = 0; index < SCREENSHOT_LIMIT && !screenshot; index++)
	{
		if (!screenshots[index].readback)
			screenshot = &screenshots[index];
	}
	if (!screenshot)
	{
		renderer_log("too many screenshots at once: %s is not written", request->path);
		return;
	}
	screenshot->width = target->definition.width;
	screenshot->height = target->definition.height;
	screenshot->pitch = (screenshot->width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
		~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	screenshot->format = request->format;
	memcpy(screenshot->path, request->path, sizeof(screenshot->path));
	screenshot->readback = renderer_buffer_create(D3D12_HEAP_TYPE_READBACK,
		(uint64_t)screenshot->pitch * screenshot->height, D3D12_RESOURCE_STATE_COPY_DEST);

	memset(&source, 0, sizeof(source));
	source.pResource = target->resource;
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	memset(&destination, 0, sizeof(destination));
	destination.pResource = screenshot->readback;
	destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	destination.PlacedFootprint.Footprint.Width = screenshot->width;
	destination.PlacedFootprint.Footprint.Height = screenshot->height;
	destination.PlacedFootprint.Footprint.Depth = 1;
	destination.PlacedFootprint.Footprint.RowPitch = screenshot->pitch;
	renderer_target_state(target, D3D12_RESOURCE_STATE_COPY_SOURCE);
	ID3D12GraphicsCommandList_CopyTextureRegion(renderer.list, &destination, 0, 0, 0, &source, NULL);
	renderer_target_state(target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	screenshot->fence = renderer.fence_value + 1;
}

static BOOL write_bmp(const char *path, const uint8_t *pixels, uint32_t width, uint32_t height)
{
	unsigned char header[54] = { 'B', 'M' };
	uint32_t image_size = width * height * 4, row;
	FILE *file = fopen(path, "wb");

	if (!file)
		return FALSE;
	*(uint32_t *)(header + 2) = 54 + image_size;
	*(uint32_t *)(header + 10) = 54;
	*(uint32_t *)(header + 14) = 40;
	*(int32_t *)(header + 18) = (int32_t)width;
	/* rows from the top */
	*(int32_t *)(header + 22) = -(int32_t)height;
	*(uint16_t *)(header + 26) = 1;
	*(uint16_t *)(header + 28) = 32;
	*(uint32_t *)(header + 34) = image_size;
	fwrite(header, 1, sizeof(header), file);
	for (row = 0; row < height; row++)
		fwrite(pixels + (size_t)row * width * 4, 1, width * 4, file);
	fclose(file);
	return TRUE;
}

static BOOL write_png(const char *path, uint8_t *pixels, uint32_t width, uint32_t height)
{
	static IWICImagingFactory *factory;
	IWICStream *stream = NULL;
	IWICBitmapEncoder *encoder = NULL;
	IWICBitmapFrameEncode *frame = NULL;
	WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
	wchar_t wide[XGPU_PATH_SIZE];
	BOOL written = FALSE;

	if (!factory)
	{
		CoInitializeEx(NULL, COINIT_MULTITHREADED);
		if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
			(void **)&factory)))
		{
			return FALSE;
		}
	}
	if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, XGPU_PATH_SIZE))
		return FALSE;
	if (SUCCEEDED(IWICImagingFactory_CreateStream(factory, &stream)) &&
		SUCCEEDED(IWICStream_InitializeFromFilename(stream, wide, GENERIC_WRITE)) &&
		SUCCEEDED(IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder)) &&
		SUCCEEDED(IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache)) &&
		SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(encoder, &frame, NULL)) &&
		SUCCEEDED(IWICBitmapFrameEncode_Initialize(frame, NULL)) &&
		SUCCEEDED(IWICBitmapFrameEncode_SetSize(frame, width, height)) &&
		SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(frame, &format)) &&
		IsEqualGUID(&format, &GUID_WICPixelFormat32bppBGRA) &&
		SUCCEEDED(IWICBitmapFrameEncode_WritePixels(frame, height, width * 4, width * height * 4, pixels)) &&
		SUCCEEDED(IWICBitmapFrameEncode_Commit(frame)) &&
		SUCCEEDED(IWICBitmapEncoder_Commit(encoder)))
	{
		written = TRUE;
	}
	if (frame)
		IWICBitmapFrameEncode_Release(frame);
	if (encoder)
		IWICBitmapEncoder_Release(encoder);
	if (stream)
		IWICStream_Release(stream);
	return written;
}

void renderer_screenshots_collect(void)
{
	uint64_t completed = renderer_completed();
	int index;

	for (index = 0; index < SCREENSHOT_LIMIT; index++)
	{
		struct screenshot *screenshot = &screenshots[index];
		const uint8_t *mapped;
		uint8_t *pixels;
		uint32_t row, column;
		D3D12_RANGE range;

		if (!screenshot->readback || screenshot->fence > completed)
			continue;
		pixels = malloc((size_t)screenshot->width * screenshot->height * 4);
		range.Begin = 0;
		range.End = (SIZE_T)screenshot->pitch * screenshot->height;
		if (pixels && SUCCEEDED(ID3D12Resource_Map(screenshot->readback, 0, &range, (void **)&mapped)))
		{
			/* tightly packed; the game keeps its own values in destination
			alpha, which image viewers would show as transparency */
			for (row = 0; row < screenshot->height; row++)
			{
				uint8_t *destination = pixels + (size_t)row * screenshot->width * 4;

				memcpy(destination, mapped + (size_t)row * screenshot->pitch, screenshot->width * 4);
				for (column = 0; column < screenshot->width; column++)
					destination[column * 4 + 3] = 0xff;
			}
			range.End = 0;
			ID3D12Resource_Unmap(screenshot->readback, 0, &range);
			if (screenshot->format == _xgpu_screenshot_bmp ?
				write_bmp(screenshot->path, pixels, screenshot->width, screenshot->height) :
				write_png(screenshot->path, pixels, screenshot->width, screenshot->height))
			{
				InterlockedIncrement((volatile LONG *)&renderer_header->screenshots_written);
			}
			else
			{
				renderer_log("cannot write the screenshot %s", screenshot->path);
			}
		}
		free(pixels);
		ID3D12Resource_Release(screenshot->readback);
		screenshot->readback = NULL;
	}
}

/* ---------- a lost device: the passes' shaders are kept, their signature
and pipelines made again; screenshots not yet read back are lost */

void renderer_output_lost(void)
{
	int index, sample_index;

	if (pass_signature)
		ID3D12RootSignature_Release(pass_signature);
	pass_signature = NULL;
	for (index = 0; index < BLIT_FORMATS; index++)
	{
		if (blit_pipelines[index])
			ID3D12PipelineState_Release(blit_pipelines[index]);
		blit_pipelines[index] = NULL;
	}
	for (index = 0; index < 16; index++)
	{
		for (sample_index = 0; sample_index < 4; sample_index++)
		{
			if (clear_pipelines[index][sample_index])
				ID3D12PipelineState_Release(clear_pipelines[index][sample_index]);
			clear_pipelines[index][sample_index] = NULL;
		}
	}
	for (index = 0; index < SCREENSHOT_LIMIT; index++)
	{
		if (screenshots[index].readback)
		{
			renderer_log("the screenshot %s is lost with the device", screenshots[index].path);
			ID3D12Resource_Release(screenshots[index].readback);
			screenshots[index].readback = NULL;
		}
	}
}

void renderer_output_restore(void)
{
	pass_signature_create();
}
