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
	renderer_output.c     presentation, its own passes, screenshots
	renderer_post.c       anti-aliasing (FXAA, SMAA)
	renderer_path_tracer.c  display.path_tracing (its shaders: the shaders folder)
	renderer_scene.c      the path tracer's scene

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
#include <setjmp.h>
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

/* A lost device (removed, reset, hung: renderer_lost_result) goes back to
the ring's loop through renderer_recovery (renderer_main.c), which releases
every module's objects (the *_lost functions), makes the device again and
restores them (the *_restore functions) from what the renderer keeps: the
textures' texels, the mirror's bytes, the shaders' code, the targets'
definitions. */
extern jmp_buf renderer_recovery;
extern BOOL renderer_recovery_armed;
extern HRESULT renderer_lost_reason;
BOOL renderer_lost_result(HRESULT result);
void renderer_device_lost(HRESULT reason) __attribute__((noreturn));
void renderer_device_release(void);
/* (the game's test of the recovery: debug.scene_test_device_reset) */
void renderer_remove_device(void);
void renderer_resources_lost(void);
void renderer_resources_restore(void);
void renderer_pipeline_lost(void);
void renderer_pipeline_restore(void);
void renderer_draw_lost(void);
void renderer_draw_restore(void);
void renderer_output_lost(void);
void renderer_output_restore(void);
void renderer_post_lost(void);
void renderer_post_restore(void);
void renderer_path_tracer_lost(void);
void renderer_path_tracer_restore(void);
void renderer_scene_lost(void);
void renderer_scene_restore(void);
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
/* the view heap's first descriptor, for tables of every texture */
D3D12_GPU_DESCRIPTOR_HANDLE renderer_view_gpu_start(void);
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
	/* the texels sent for each subresource (a level of a face), packed, kept
	to fill the texture again on a new device; NULL where none came */
	uint8_t **shadows;
	uint32_t shadow_count;
	/* the level its others were last made from by the GPU (-1: none) */
	int32_t mipmap_base;
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
/* the draw state the game sent last (NULL before any) */
const struct xgpu_draw_state *renderer_draw_state_current(void);
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
void renderer_anti_alias_prepare(const struct xgpu_anti_alias_prepare *prepare, const char *smaa, uint32_t length);
void renderer_screenshot(const struct xgpu_screenshot *screenshot);
/* screenshots of finished frames, written */
void renderer_screenshots_collect(void);

/* ---------- renderer_path_tracer.c */

/* whether the GPU can trace rays, into the shared header */
void renderer_path_tracer_initialize(void);
/* display.path_tracing, as the game sends it */
void renderer_path_tracer_configure(BOOL wanted);
/* the setting is on and the GPU's ray tracing works */
BOOL renderer_path_tracing(void);
/* debug.path_tracing_view */
void renderer_path_tracer_view(uint32_t view);
/* one of the game's passes begins or ends */
void renderer_path_tracer_pass(const struct xgpu_pass *pass);
/* an acceleration structure built from inputs into the command list (its
scratch released once the GPU is past it); read after a UAV barrier, which
it records */
ID3D12Resource *renderer_acceleration_structure_build(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *inputs);

/* ---------- renderer_scene.c */

/* a material as the trace reads it (shaders/pt_common.hlsli) */
#define RENDERER_SCENE_NO_TEXTURE 0xffffffffu
struct renderer_scene_gpu_material
{
	/* the view heap's descriptors of the base, primary detail, secondary
	detail, micro detail and bump maps */
	uint32_t maps[5];
	uint32_t flags;
	/* detail function, micro detail function, environment type: a byte each */
	uint32_t functions;
	uint32_t reserved;
	/* primary, secondary and micro detail scales, the bump map's scale across */
	float scales[4];
	float emission[3];
	/* the bump map's scale down */
	float bump_scale_v;
};

void renderer_scene_begin_map(const struct xgpu_scene_begin_map *begin);
void renderer_scene_piece(const struct xgpu_scene_piece *piece, const uint8_t *data, uint32_t size);
void renderer_scene_camera(const struct xgpu_scene_camera *camera);
/* the surfaces the game draws in the window */
void renderer_scene_drawn(const uint32_t *surfaces, uint32_t count);
/* their bits, by surface, for a trace (in an upload heap) */
D3D12_GPU_VIRTUAL_ADDRESS renderer_scene_drawn_bits(void);
/* the camera of the window being drawn (NULL before any) */
const struct xgpu_scene_camera *renderer_scene_camera_get(void);
/* the GPU's copies of the scene made, if they are to be; FALSE if there is
nothing to trace */
BOOL renderer_scene_prepare(void);
/* the material of a geometry the GPU traces (by its index there) */
uint32_t renderer_scene_geometry_material(uint32_t geometry);
/* the material table for a trace (in an upload heap) */
D3D12_GPU_VIRTUAL_ADDRESS renderer_scene_materials(void);
void renderer_scene_buffers(D3D12_GPU_VIRTUAL_ADDRESS *top, D3D12_GPU_VIRTUAL_ADDRESS *vertices,
	D3D12_GPU_VIRTUAL_ADDRESS *indices, D3D12_GPU_VIRTUAL_ADDRESS *geometries);

#endif
