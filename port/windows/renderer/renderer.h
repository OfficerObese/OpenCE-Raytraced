/*
RENDERER.H

The D3D12 renderer process (halo_renderer.exe, 64-bit): what its files share.

The game (32-bit, port/windows/d3d12) starts it and feeds it the messages of
xgpu_protocol.h through a shared section; it draws them with Direct3D 12 and
presents into the DirectComposition surface the game's window shows.

	renderer_main.c       the process, the ring, the messages
	renderer_device.c     the device, frames and their command lists, the
	                      upload heaps, descriptor heaps, the swap chain
	renderer_resources.c  textures, render targets, the mirror's buffers
	renderer_pipeline.c   shader compilation, the root signature, pipelines
	renderer_draw.c       draw state, draws, clears, visibility tests
	renderer_output.c     presentation, anti-aliasing, screenshots

One thread does everything; nothing here is called from elsewhere.
*/

#ifndef __RENDERER_H
#define __RENDERER_H

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdint.h>

#include "../d3d12/xgpu_protocol.h"
#include "../d3d12/xgpu_hlsl.h"

/* frames whose command lists may be in flight on the GPU at once */
#define RENDERER_FRAME_COUNT 3
#define RENDERER_SWAP_CHAIN_BUFFERS 3

/* ---------- renderer_main.c */

/* the process's failure: written to the shared header (which the game shows)
and the log, then the process exits */
void renderer_fail(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));
void renderer_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
/* an HRESULT that is not success ends the process, naming what failed */
void renderer_check(HRESULT result, const char *what);
#define RENDERER_CHECK(call) renderer_check((call), #call)

extern struct xgpu_header *renderer_header;
extern struct xgpu_results *renderer_results;

/* ---------- renderer_device.c */

struct renderer_descriptor
{
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu;
	uint32_t index;
};

struct renderer_device
{
	ID3D12Device *device;
	ID3D12CommandQueue *queue;
	ID3D12Fence *fence;
	HANDLE fence_event;
	uint64_t fence_value;

	/* the command list being recorded, and the allocator it records into */
	ID3D12GraphicsCommandList *list;
	ID3D12CommandAllocator *allocator;

	ID3D12DescriptorHeap *view_heap;
	ID3D12DescriptorHeap *sampler_heap;
	ID3D12DescriptorHeap *rtv_heap;
	ID3D12DescriptorHeap *dsv_heap;
	UINT view_increment, sampler_increment, rtv_increment, dsv_increment;

	IDXGISwapChain3 *swap_chain;
	HANDLE frame_latency;
	ID3D12Resource *buffers[RENDERER_SWAP_CHAIN_BUFFERS];
	struct renderer_descriptor buffer_rtvs[RENDERER_SWAP_CHAIN_BUFFERS];
	uint32_t width, height;
	BOOL vsync;
	BOOL tearing;

	uint32_t maximum_samples;
};

extern struct renderer_device renderer;

void renderer_device_create(HANDLE surface, uint32_t width, uint32_t height, BOOL debug_layer);
/* the debug layer's messages since last asked, into the log (nothing without
the layer) */
void renderer_debug_messages(void);
void renderer_device_resize(uint32_t width, uint32_t height);
/* submits what has been recorded, and starts a new command list; returns the
fence value the GPU reaches once it has done the submitted work */
uint64_t renderer_submit(void);
/* waits until the GPU has reached the fence value */
void renderer_wait(uint64_t value);
uint64_t renderer_completed(void);
/* the frame's work is done being recorded: submits it, presents the swap
chain's current buffer, and recycles what finished frames used */
void renderer_present_submit(void);

/* room in an upload heap, valid until the GPU has done the work recorded
until the next submission: size bytes at an alignment (a power of two) */
struct renderer_upload
{
	void *data;
	ID3D12Resource *buffer;
	uint64_t offset;
	D3D12_GPU_VIRTUAL_ADDRESS address;
};

struct renderer_upload renderer_upload_allocate(uint64_t size, uint64_t alignment);
/* between messages: submits the frame so far if it has uploaded a lot (a
map's first frames upload hundreds of megabytes), so that its upload heaps
can be used again */
void renderer_submit_if_large(void);

/* descriptors in the shader-visible view heap, kept until freed (freed ones
are reused once the GPU is past the frame that freed them) */
struct renderer_descriptor renderer_view_allocate(void);
void renderer_view_free(struct renderer_descriptor descriptor);
struct renderer_descriptor renderer_rtv_allocate(void);
void renderer_rtv_free(struct renderer_descriptor descriptor);
struct renderer_descriptor renderer_dsv_allocate(void);
void renderer_dsv_free(struct renderer_descriptor descriptor);
D3D12_CPU_DESCRIPTOR_HANDLE renderer_sampler_cpu(uint32_t index);
D3D12_GPU_DESCRIPTOR_HANDLE renderer_sampler_gpu(uint32_t index);
#define RENDERER_SAMPLER_LIMIT 2048

/* releases an object once the GPU is past the work recorded so far */
void renderer_release_later(IUnknown *object);

void renderer_transition(ID3D12Resource *resource, UINT subresource, D3D12_RESOURCE_STATES before,
	D3D12_RESOURCE_STATES after);
ID3D12Resource *renderer_buffer_create(D3D12_HEAP_TYPE heap, uint64_t size, D3D12_RESOURCE_STATES state);

/* ---------- renderer_resources.c */

struct renderer_texture
{
	ID3D12Resource *resource;
	struct renderer_descriptor srv;
	struct xgpu_texture_define definition;
	DXGI_FORMAT format;
	/* the resource's state (every subresource alike) */
	D3D12_RESOURCE_STATES state;
};

struct renderer_target
{
	struct xgpu_target_define definition;
	/* the pixels sampled, copied and presented */
	ID3D12Resource *resource;
	/* with samples, the multisampled pixels drawn into */
	ID3D12Resource *multisampled;
	D3D12_RESOURCE_STATES state, multisampled_state;
	struct renderer_descriptor view;
	struct renderer_descriptor srv;
	/* the views draws use: the multisampled resource's, with samples */
	struct renderer_descriptor draw_view;
};

void renderer_resources_initialize(void);
struct renderer_texture *renderer_texture_get(uint32_t texture);
struct renderer_target *renderer_target_get(uint32_t target);
void renderer_texture_define(const struct xgpu_texture_define *definition);
void renderer_texture_data(const struct xgpu_texture_data *data, const uint8_t *bytes, uint32_t size);
void renderer_texture_mipmaps(const struct xgpu_texture_reference *reference);
void renderer_texture_delete(uint32_t texture);
void renderer_target_define(const struct xgpu_target_define *definition);
void renderer_target_delete(uint32_t target);
void renderer_target_resolve(uint32_t target);
void renderer_target_copy(const struct xgpu_target_copy *copy);
void renderer_target_state(struct renderer_target *target, D3D12_RESOURCE_STATES state);
void renderer_target_draw_state(struct renderer_target *target, D3D12_RESOURCE_STATES state);
void renderer_texture_state(struct renderer_texture *texture, D3D12_RESOURCE_STATES state);
void renderer_mirror_write(const struct xgpu_mirror_write *write, const uint8_t *bytes, uint32_t size);
/* the buffer of a mirror segment, in a state draws read (NULL if never written) */
ID3D12Resource *renderer_mirror_buffer(uint32_t segment);
/* SRVs of no texture, for stages that have none */
struct renderer_descriptor renderer_null_srv(uint32_t dimension);

/* ---------- renderer_pipeline.c */

void renderer_pipeline_initialize(void);
void renderer_vertex_shader(const struct xgpu_shader *shader, const char *hlsl, uint32_t length);
void renderer_pixel_shader(const struct xgpu_shader *shader, const char *hlsl, uint32_t length);
ID3D12RootSignature *renderer_root_signature(void);
/* the pipeline of a draw state, made the first time; NULL if its shaders did
not compile */
ID3D12PipelineState *renderer_pipeline_get(const struct xgpu_draw_state *state, BOOL color, BOOL depth);
/* compiles HLSL for the renderer's own passes; NULL with the log written */
ID3DBlob *renderer_compile(const char *hlsl, const char *entry, const char *target, const char *what);

/* ---------- renderer_draw.c */

void renderer_draw_initialize(void);
void renderer_draw_begin_frame(void);
void renderer_vertex_constants(const struct xgpu_vertex_constants *constants, const float *values);
void renderer_draw_uniforms(const struct xgpu_hlsl_draw_uniforms *uniforms);
void renderer_model_lights(const float *lights);
void renderer_draw_state(const struct xgpu_draw_state *state);
void renderer_draw(const struct xgpu_draw *draw, const uint8_t *inline_data);
void renderer_clear(const struct xgpu_clear *clear);
void renderer_visibility_begin(void);
void renderer_visibility_end(uint32_t slot);
/* the frame's visibility counts copied out, at its end */
void renderer_visibility_resolve(uint32_t frame);
/* the counts of frames the GPU has finished, into the results area */
void renderer_visibility_collect(void);
/* the bound state is unknown: the next draw sets all of it again */
void renderer_draw_invalidate(void);
/* a visibility test is running: a query begins and ends in one command list,
so nothing submits until it ends */
extern BOOL renderer_visibility_active;

/* ---------- renderer_output.c */

void renderer_output_initialize(void);
/* draws a texture (its whole, through an SRV of the levels to read) into a
viewport of a render target of format, filtered; the target is a render
target already */
void renderer_blit(D3D12_GPU_DESCRIPTOR_HANDLE source, D3D12_CPU_DESCRIPTOR_HANDLE target, DXGI_FORMAT format,
	float x, float y, float width, float height);
/* clears only some channels (mask: bit 0 red .. 3 alpha) of rectangles of a
color target (a BGRA8 one, of samples a pixel, width by height), by drawing:
D3D12's clears write every channel */
void renderer_clear_channels(D3D12_CPU_DESCRIPTOR_HANDLE target, uint32_t samples, uint32_t width, uint32_t height,
	const D3D12_RECT *rectangles, uint32_t count, const float color[4], uint32_t mask);
void renderer_present(const struct xgpu_present *present);
void renderer_anti_alias(const struct xgpu_anti_alias *anti_alias);
void renderer_screenshot(const struct xgpu_screenshot *screenshot);
/* screenshots of finished frames, written */
void renderer_screenshots_collect(void);

#endif
