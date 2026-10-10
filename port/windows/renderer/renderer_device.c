/*
RENDERER_DEVICE.C

The device and what the rest of the renderer records with: one command list
at a time, submitted at each presentation (and in the middle of a frame that
uploads a lot, so that its upload heaps can be reused), the upload heaps that
data reach the GPU through, the descriptor heaps, and the swap chain, which
presents into the game's DirectComposition surface (as the spike in
port/windows/spike proved). Whatever the GPU may still use when it is freed
(an upload heap, a descriptor, a resource) waits for the fence value of the
last submission that could use it.
*/

#include "renderer.h"

#include <stdlib.h>
#include <string.h>

struct renderer_device renderer;

/* with the debug layer, its messages, written to the log as they come */
static ID3D12InfoQueue *info_queue;

void renderer_debug_messages(void)
{
	UINT64 count, index;

	if (!info_queue)
		return;
	count = ID3D12InfoQueue_GetNumStoredMessages(info_queue);
	for (index = 0; index < count; index++)
	{
		SIZE_T length = 0;
		D3D12_MESSAGE *message;

		if (FAILED(ID3D12InfoQueue_GetMessage(info_queue, index, NULL, &length)) || !length)
			continue;
		message = malloc(length);
		if (message && SUCCEEDED(ID3D12InfoQueue_GetMessage(info_queue, index, message, &length)) &&
			message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
		{
			renderer_log("D3D12 %s: %.*s", message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR ? "error" : "warning",
				(int)message->DescriptionByteLength, message->pDescription);
		}
		free(message);
	}
	ID3D12InfoQueue_ClearStoredMessages(info_queue);
}

/* ---------- objects released once the GPU is past them */

struct deferred_release
{
	IUnknown *object;
	uint64_t fence;
};

static struct deferred_release *releases;
static size_t release_count, release_capacity;

void renderer_release_later(IUnknown *object)
{
	if (!object)
		return;
	if (release_count == release_capacity)
	{
		size_t capacity = release_capacity ? release_capacity * 2 : 256;
		struct deferred_release *grown = realloc(releases, capacity * sizeof(*grown));

		if (!grown)
			renderer_fail("out of memory");
		releases = grown;
		release_capacity = capacity;
	}
	releases[release_count].object = object;
	releases[release_count].fence = renderer.fence_value + 1;
	release_count++;
}

static void releases_collect(uint64_t completed)
{
	size_t index = 0;

	while (index < release_count)
	{
		if (releases[index].fence <= completed)
		{
			IUnknown_Release(releases[index].object);
			releases[index] = releases[--release_count];
		}
		else
		{
			index++;
		}
	}
}

/* ---------- descriptor heaps */

#define VIEW_DESCRIPTOR_COUNT 300000u
#define RTV_DESCRIPTOR_COUNT (XGPU_TARGET_LIMIT * 2 + 64)
#define DSV_DESCRIPTOR_COUNT (XGPU_TARGET_LIMIT + 64)

/* a heap's free indices, and the ones freed until the GPU is past them */
struct descriptor_pool
{
	ID3D12DescriptorHeap **heap;
	UINT *increment;
	BOOL shader_visible;
	uint32_t capacity;
	uint32_t next;
	uint32_t *free;
	uint32_t free_count;
	struct
	{
		uint32_t index;
		uint64_t fence;
	} *pending;
	uint32_t pending_count;
};

static struct descriptor_pool view_pool = { &renderer.view_heap, &renderer.view_increment, TRUE, VIEW_DESCRIPTOR_COUNT };
static struct descriptor_pool rtv_pool = { &renderer.rtv_heap, &renderer.rtv_increment, FALSE, RTV_DESCRIPTOR_COUNT };
static struct descriptor_pool dsv_pool = { &renderer.dsv_heap, &renderer.dsv_increment, FALSE, DSV_DESCRIPTOR_COUNT };

static struct renderer_descriptor descriptor_at(struct descriptor_pool *pool, uint32_t index)
{
	struct renderer_descriptor descriptor;

	ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(*pool->heap, &descriptor.cpu);
	descriptor.cpu.ptr += (SIZE_T)index * *pool->increment;
	descriptor.gpu.ptr = 0;
	if (pool->shader_visible)
	{
		ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(*pool->heap, &descriptor.gpu);
		descriptor.gpu.ptr += (UINT64)index * *pool->increment;
	}
	descriptor.index = index;
	return descriptor;
}

static struct renderer_descriptor descriptor_allocate(struct descriptor_pool *pool)
{
	uint32_t index;

	if (pool->free_count)
		index = pool->free[--pool->free_count];
	else if (pool->next < pool->capacity)
		index = pool->next++;
	else
		renderer_fail("out of descriptors (%u)", pool->capacity);
	return descriptor_at(pool, index);
}

static void descriptor_free(struct descriptor_pool *pool, struct renderer_descriptor descriptor)
{
	if (!pool->pending)
	{
		pool->pending = malloc(pool->capacity * sizeof(*pool->pending));
		pool->free = malloc(pool->capacity * sizeof(*pool->free));
		if (!pool->pending || !pool->free)
			renderer_fail("out of memory");
	}
	if (pool->pending_count >= pool->capacity)
		return;
	pool->pending[pool->pending_count].index = descriptor.index;
	pool->pending[pool->pending_count].fence = renderer.fence_value + 1;
	pool->pending_count++;
}

static void descriptors_collect(struct descriptor_pool *pool, uint64_t completed)
{
	uint32_t index = 0;

	while (index < pool->pending_count)
	{
		if (pool->pending[index].fence <= completed)
		{
			pool->free[pool->free_count++] = pool->pending[index].index;
			pool->pending[index] = pool->pending[--pool->pending_count];
		}
		else
		{
			index++;
		}
	}
}

struct renderer_descriptor renderer_view_allocate(void) { return descriptor_allocate(&view_pool); }
void renderer_view_free(struct renderer_descriptor descriptor) { descriptor_free(&view_pool, descriptor); }
struct renderer_descriptor renderer_rtv_allocate(void) { return descriptor_allocate(&rtv_pool); }
void renderer_rtv_free(struct renderer_descriptor descriptor) { descriptor_free(&rtv_pool, descriptor); }
struct renderer_descriptor renderer_dsv_allocate(void) { return descriptor_allocate(&dsv_pool); }
void renderer_dsv_free(struct renderer_descriptor descriptor) { descriptor_free(&dsv_pool, descriptor); }

D3D12_CPU_DESCRIPTOR_HANDLE renderer_sampler_cpu(uint32_t index)
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle;

	ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(renderer.sampler_heap, &handle);
	handle.ptr += (SIZE_T)index * renderer.sampler_increment;
	return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE renderer_sampler_gpu(uint32_t index)
{
	D3D12_GPU_DESCRIPTOR_HANDLE handle;

	ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(renderer.sampler_heap, &handle);
	handle.ptr += (UINT64)index * renderer.sampler_increment;
	return handle;
}

static ID3D12DescriptorHeap *descriptor_heap_create(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, BOOL shader_visible)
{
	D3D12_DESCRIPTOR_HEAP_DESC description;
	ID3D12DescriptorHeap *heap;

	memset(&description, 0, sizeof(description));
	description.Type = type;
	description.NumDescriptors = count;
	description.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	RENDERER_CHECK(ID3D12Device_CreateDescriptorHeap(renderer.device, &description, &IID_ID3D12DescriptorHeap,
		(void **)&heap));
	return heap;
}

/* ---------- upload heaps */

#define UPLOAD_CHUNK_SIZE (64ull << 20)
/* a frame that has uploaded this much is submitted, so that its chunks can
come back (a map's first frames upload hundreds of megabytes) */
#define UPLOAD_SUBMIT_THRESHOLD (256ull << 20)

struct upload_chunk
{
	ID3D12Resource *buffer;
	uint8_t *data;
	D3D12_GPU_VIRTUAL_ADDRESS address;
	uint64_t size;
	uint64_t used;
	/* the fence value of the last submission that used it */
	uint64_t fence;
};

#define UPLOAD_CHUNK_LIMIT 64

static struct upload_chunk upload_chunks[UPLOAD_CHUNK_LIMIT];
static int upload_chunk_count;
static int upload_current = -1;
static uint64_t uploaded_since_submit;

ID3D12Resource *renderer_buffer_create(D3D12_HEAP_TYPE heap_type, uint64_t size, D3D12_RESOURCE_STATES state)
{
	D3D12_HEAP_PROPERTIES heap;
	D3D12_RESOURCE_DESC description;
	ID3D12Resource *resource;

	memset(&heap, 0, sizeof(heap));
	heap.Type = heap_type;
	memset(&description, 0, sizeof(description));
	description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	description.Width = size;
	description.Height = 1;
	description.DepthOrArraySize = 1;
	description.MipLevels = 1;
	description.SampleDesc.Count = 1;
	description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	RENDERER_CHECK(ID3D12Device_CreateCommittedResource(renderer.device, &heap, D3D12_HEAP_FLAG_NONE, &description,
		state, NULL, &IID_ID3D12Resource, (void **)&resource));
	return resource;
}

static int upload_chunk_take(uint64_t size)
{
	uint64_t completed = renderer_completed();
	int index;

	for (index = 0; index < upload_chunk_count; index++)
	{
		if (index != upload_current && upload_chunks[index].fence <= completed && upload_chunks[index].size >= size)
		{
			upload_chunks[index].used = 0;
			return index;
		}
	}
	if (upload_chunk_count == UPLOAD_CHUNK_LIMIT)
	{
		/* every chunk is in flight: wait for the oldest */
		uint64_t oldest = ~0ull;
		int found = -1;

		for (index = 0; index < upload_chunk_count; index++)
		{
			if (index != upload_current && upload_chunks[index].fence < oldest && upload_chunks[index].size >= size)
			{
				oldest = upload_chunks[index].fence;
				found = index;
			}
		}
		if (found < 0)
			renderer_fail("no upload heap can hold %llu bytes", (unsigned long long)size);
		if (oldest > renderer.fence_value)
			renderer_submit();
		renderer_wait(oldest);
		upload_chunks[found].used = 0;
		return found;
	}
	index = upload_chunk_count++;
	upload_chunks[index].size = size > UPLOAD_CHUNK_SIZE ? size : UPLOAD_CHUNK_SIZE;
	upload_chunks[index].buffer = renderer_buffer_create(D3D12_HEAP_TYPE_UPLOAD, upload_chunks[index].size,
		D3D12_RESOURCE_STATE_GENERIC_READ);
	RENDERER_CHECK(ID3D12Resource_Map(upload_chunks[index].buffer, 0, NULL, (void **)&upload_chunks[index].data));
	upload_chunks[index].address = ID3D12Resource_GetGPUVirtualAddress(upload_chunks[index].buffer);
	upload_chunks[index].used = 0;
	upload_chunks[index].fence = 0;
	return index;
}

struct renderer_upload renderer_upload_allocate(uint64_t size, uint64_t alignment)
{
	struct renderer_upload upload;
	struct upload_chunk *chunk;
	uint64_t offset;

	offset = upload_current >= 0 ? (upload_chunks[upload_current].used + alignment - 1) & ~(alignment - 1) : 0;
	if (upload_current < 0 || offset + size > upload_chunks[upload_current].size)
	{
		upload_current = upload_chunk_take(size);
		offset = 0;
	}
	chunk = &upload_chunks[upload_current];
	chunk->used = offset + size;
	chunk->fence = renderer.fence_value + 1;
	uploaded_since_submit += size;
	upload.data = chunk->data + offset;
	upload.buffer = chunk->buffer;
	upload.offset = offset;
	upload.address = chunk->address + offset;
	return upload;
}

void renderer_submit_if_large(void)
{
	if (uploaded_since_submit > UPLOAD_SUBMIT_THRESHOLD && !renderer_visibility_active)
		renderer_submit();
}

/* ---------- command lists */

struct allocator_entry
{
	ID3D12CommandAllocator *allocator;
	uint64_t fence;
};

#define ALLOCATOR_LIMIT 32

static struct allocator_entry allocators[ALLOCATOR_LIMIT];
static int allocator_count;

static ID3D12CommandAllocator *allocator_take(void)
{
	uint64_t completed = renderer_completed();
	int index;

	for (index = 0; index < allocator_count; index++)
	{
		if (allocators[index].fence <= completed)
		{
			ID3D12CommandAllocator *allocator = allocators[index].allocator;

			allocators[index] = allocators[--allocator_count];
			RENDERER_CHECK(ID3D12CommandAllocator_Reset(allocator));
			return allocator;
		}
	}
	if (allocator_count == ALLOCATOR_LIMIT)
	{
		/* wait for the oldest */
		uint64_t oldest = ~0ull;
		int found = 0;

		for (index = 0; index < allocator_count; index++)
		{
			if (allocators[index].fence < oldest)
			{
				oldest = allocators[index].fence;
				found = index;
			}
		}
		renderer_wait(oldest);
		{
			ID3D12CommandAllocator *allocator = allocators[found].allocator;

			allocators[found] = allocators[--allocator_count];
			RENDERER_CHECK(ID3D12CommandAllocator_Reset(allocator));
			return allocator;
		}
	}
	{
		ID3D12CommandAllocator *allocator;

		RENDERER_CHECK(ID3D12Device_CreateCommandAllocator(renderer.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
			&IID_ID3D12CommandAllocator, (void **)&allocator));
		return allocator;
	}
}

static void list_begin(void)
{
	ID3D12DescriptorHeap *heaps[2];

	renderer.allocator = allocator_take();
	RENDERER_CHECK(ID3D12GraphicsCommandList_Reset(renderer.list, renderer.allocator, NULL));
	heaps[0] = renderer.view_heap;
	heaps[1] = renderer.sampler_heap;
	ID3D12GraphicsCommandList_SetDescriptorHeaps(renderer.list, 2, heaps);
	renderer_draw_invalidate();
}

uint64_t renderer_completed(void)
{
	return ID3D12Fence_GetCompletedValue(renderer.fence);
}

void renderer_wait(uint64_t value)
{
	if (renderer_completed() < value)
	{
		RENDERER_CHECK(ID3D12Fence_SetEventOnCompletion(renderer.fence, value, renderer.fence_event));
		if (WaitForSingleObject(renderer.fence_event, 10000) != WAIT_OBJECT_0)
		{
			HRESULT reason = ID3D12Device_GetDeviceRemovedReason(renderer.device);

			renderer_fail("the GPU did not finish its work in 10 seconds (device: 0x%08lX)", (unsigned long)reason);
		}
	}
}

uint64_t renderer_submit(void)
{
	ID3D12CommandList *lists[1];
	uint64_t completed;

	RENDERER_CHECK(ID3D12GraphicsCommandList_Close(renderer.list));
	lists[0] = (ID3D12CommandList *)renderer.list;
	ID3D12CommandQueue_ExecuteCommandLists(renderer.queue, 1, lists);
	renderer.fence_value++;
	RENDERER_CHECK(ID3D12CommandQueue_Signal(renderer.queue, renderer.fence, renderer.fence_value));
	if (allocator_count < ALLOCATOR_LIMIT)
	{
		allocators[allocator_count].allocator = renderer.allocator;
		allocators[allocator_count].fence = renderer.fence_value;
		allocator_count++;
	}
	else
	{
		renderer_wait(renderer.fence_value);
		ID3D12CommandAllocator_Release(renderer.allocator);
	}
	uploaded_since_submit = 0;
	renderer_debug_messages();
	completed = renderer_completed();
	releases_collect(completed);
	descriptors_collect(&view_pool, completed);
	descriptors_collect(&rtv_pool, completed);
	descriptors_collect(&dsv_pool, completed);
	list_begin();
	return renderer.fence_value;
}

void renderer_transition(ID3D12Resource *resource, UINT subresource, D3D12_RESOURCE_STATES before,
	D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER barrier;

	if (before == after)
		return;
	memset(&barrier, 0, sizeof(barrier));
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = subresource;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	ID3D12GraphicsCommandList_ResourceBarrier(renderer.list, 1, &barrier);
}

/* ---------- the swap chain */

static void buffers_get(void)
{
	int index;

	for (index = 0; index < RENDERER_SWAP_CHAIN_BUFFERS; index++)
	{
		RENDERER_CHECK(IDXGISwapChain3_GetBuffer(renderer.swap_chain, index, &IID_ID3D12Resource,
			(void **)&renderer.buffers[index]));
		if (!renderer.buffer_rtvs[index].cpu.ptr)
			renderer.buffer_rtvs[index] = renderer_rtv_allocate();
		ID3D12Device_CreateRenderTargetView(renderer.device, renderer.buffers[index], NULL, renderer.buffer_rtvs[index].cpu);
	}
}

void renderer_device_resize(uint32_t width, uint32_t height)
{
	int index;

	if (width == renderer.width && height == renderer.height)
		return;
	renderer_wait(renderer_submit());
	for (index = 0; index < RENDERER_SWAP_CHAIN_BUFFERS; index++)
	{
		ID3D12Resource_Release(renderer.buffers[index]);
		renderer.buffers[index] = NULL;
	}
	RENDERER_CHECK(IDXGISwapChain3_ResizeBuffers(renderer.swap_chain, 0, width, height, DXGI_FORMAT_UNKNOWN,
		DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
		(renderer.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)));
	renderer.width = width;
	renderer.height = height;
	buffers_get();
}

void renderer_present_submit(void)
{
	UINT index = IDXGISwapChain3_GetCurrentBackBufferIndex(renderer.swap_chain);
	HRESULT result;

	renderer_transition(renderer.buffers[index], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
	renderer_submit();
	result = IDXGISwapChain3_Present(renderer.swap_chain, renderer.vsync ? 1 : 0,
		!renderer.vsync && renderer.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
	/* (occluded or minimized: nothing is shown, which is not a failure) */
	if (result != DXGI_STATUS_OCCLUDED)
		renderer_check(result, "Present");
	/* at most the frames in flight behind the display */
	WaitForSingleObject(renderer.frame_latency, 1000);
}

/* ---------- the device */

static uint32_t samples_supported(DXGI_FORMAT format)
{
	uint32_t samples;

	for (samples = 8; samples >= 2; samples /= 2)
	{
		D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels;

		memset(&levels, 0, sizeof(levels));
		levels.Format = format;
		levels.SampleCount = samples;
		if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(renderer.device, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
			&levels, sizeof(levels))) && levels.NumQualityLevels > 0)
		{
			return samples;
		}
	}
	return 1;
}

void renderer_device_create(HANDLE surface, uint32_t width, uint32_t height, BOOL debug_layer)
{
	IDXGIFactory6 *factory;
	IDXGIAdapter1 *adapter;
	DXGI_ADAPTER_DESC1 adapter_description;
	D3D12_COMMAND_QUEUE_DESC queue_description;
	UINT factory_flags = 0;

	if (debug_layer)
	{
		ID3D12Debug *debug;

		if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&debug)))
		{
			ID3D12Debug_EnableDebugLayer(debug);
			ID3D12Debug_Release(debug);
			factory_flags = DXGI_CREATE_FACTORY_DEBUG;
			renderer_log("the D3D12 debug layer is on");
		}
	}
	RENDERER_CHECK(CreateDXGIFactory2(factory_flags, &IID_IDXGIFactory6, (void **)&factory));
	RENDERER_CHECK(IDXGIFactory6_EnumAdapterByGpuPreference(factory, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
		&IID_IDXGIAdapter1, (void **)&adapter));
	if (FAILED(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
		(void **)&renderer.device)))
	{
		renderer_fail("the GPU cannot run Direct3D 12 (feature level 11_0)");
	}
	IDXGIAdapter1_GetDesc1(adapter, &adapter_description);
	WideCharToMultiByte(CP_UTF8, 0, adapter_description.Description, -1, renderer_header->adapter_name,
		sizeof(renderer_header->adapter_name), NULL, NULL);
	IDXGIAdapter1_Release(adapter);
	if (debug_layer && SUCCEEDED(ID3D12Device_QueryInterface(renderer.device, &IID_ID3D12InfoQueue,
		(void **)&info_queue)))
	{
		/* (the game clears its targets to many colors: clears slower than
		the fastest are expected) */
		D3D12_MESSAGE_ID denied[] =
		{
			D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
			D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
		};
		D3D12_INFO_QUEUE_FILTER filter;

		memset(&filter, 0, sizeof(filter));
		filter.DenyList.NumIDs = sizeof(denied) / sizeof(denied[0]);
		filter.DenyList.pIDList = denied;
		ID3D12InfoQueue_AddStorageFilterEntries(info_queue, &filter);
		ID3D12InfoQueue_SetBreakOnSeverity(info_queue, D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
		ID3D12InfoQueue_SetMessageCountLimit(info_queue, 4096);
	}

	memset(&queue_description, 0, sizeof(queue_description));
	queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	RENDERER_CHECK(ID3D12Device_CreateCommandQueue(renderer.device, &queue_description, &IID_ID3D12CommandQueue,
		(void **)&renderer.queue));
	RENDERER_CHECK(ID3D12Device_CreateFence(renderer.device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
		(void **)&renderer.fence));
	renderer.fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);

	renderer.view_heap = descriptor_heap_create(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, VIEW_DESCRIPTOR_COUNT, TRUE);
	renderer.sampler_heap = descriptor_heap_create(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, RENDERER_SAMPLER_LIMIT, TRUE);
	renderer.rtv_heap = descriptor_heap_create(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_DESCRIPTOR_COUNT, FALSE);
	renderer.dsv_heap = descriptor_heap_create(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_DESCRIPTOR_COUNT, FALSE);
	renderer.view_increment = ID3D12Device_GetDescriptorHandleIncrementSize(renderer.device,
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	renderer.sampler_increment = ID3D12Device_GetDescriptorHandleIncrementSize(renderer.device,
		D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	renderer.rtv_increment = ID3D12Device_GetDescriptorHandleIncrementSize(renderer.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	renderer.dsv_increment = ID3D12Device_GetDescriptorHandleIncrementSize(renderer.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	{
		uint32_t color = samples_supported(DXGI_FORMAT_B8G8R8A8_UNORM);
		uint32_t depth = samples_supported(DXGI_FORMAT_D24_UNORM_S8_UINT);

		renderer.maximum_samples = color < depth ? color : depth;
	}

	/* the swap chain presents into the game's composition surface */
	{
		IDXGIFactoryMedia *media;
		IDXGISwapChain1 *swap_chain;
		DXGI_SWAP_CHAIN_DESC1 description;
		BOOL tearing = FALSE;

		if (FAILED(IDXGIFactory6_CheckFeatureSupport(factory, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
			sizeof(tearing))))
		{
			tearing = FALSE;
		}
		renderer.tearing = tearing;
		RENDERER_CHECK(IDXGIFactory6_QueryInterface(factory, &IID_IDXGIFactoryMedia, (void **)&media));
		memset(&description, 0, sizeof(description));
		description.Width = width;
		description.Height = height;
		description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		description.BufferCount = RENDERER_SWAP_CHAIN_BUFFERS;
		description.Scaling = DXGI_SCALING_STRETCH;
		description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
		description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
			(tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
		RENDERER_CHECK(IDXGIFactoryMedia_CreateSwapChainForCompositionSurfaceHandle(media, (IUnknown *)renderer.queue,
			surface, &description, NULL, &swap_chain));
		RENDERER_CHECK(IDXGISwapChain1_QueryInterface(swap_chain, &IID_IDXGISwapChain3, (void **)&renderer.swap_chain));
		IDXGISwapChain1_Release(swap_chain);
		IDXGIFactoryMedia_Release(media);
		RENDERER_CHECK(IDXGISwapChain3_SetMaximumFrameLatency(renderer.swap_chain, XGPU_FRAMES_IN_FLIGHT));
		renderer.frame_latency = IDXGISwapChain3_GetFrameLatencyWaitableObject(renderer.swap_chain);
	}
	IDXGIFactory6_Release(factory);
	renderer.width = width;
	renderer.height = height;
	renderer.vsync = TRUE;
	buffers_get();

	renderer.allocator = allocator_take();
	RENDERER_CHECK(ID3D12Device_CreateCommandList(renderer.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		renderer.allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&renderer.list));
	{
		ID3D12DescriptorHeap *heaps[2];

		heaps[0] = renderer.view_heap;
		heaps[1] = renderer.sampler_heap;
		ID3D12GraphicsCommandList_SetDescriptorHeaps(renderer.list, 2, heaps);
	}
}
