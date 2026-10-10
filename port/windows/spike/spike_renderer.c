/*
SPIKE_RENDERER.C

The renderer process of the D3D12 spike (README.md), built 64-bit: it reads
the host's ring, presents frames with Direct3D 12 into the DirectComposition
surface the host's window shows, and copies payloads to the GPU.

	spike_renderer.exe <section> <data event> <space event> <frame semaphore>
		<composition surface> <width> <height>

The handles are the host's, inherited, in hexadecimal. Every length read from
the ring is checked before it is used.
*/

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spike_protocol.h"

#define BUFFER_COUNT 2

static struct
{
	struct spike_shared *shared;
	const unsigned char *ring;
	HANDLE data_event, space_event, frame_semaphore;
	HANDLE surface;
	unsigned long width, height;

	ID3D12Device *device;
	ID3D12CommandQueue *queue;
	ID3D12CommandAllocator *allocators[BUFFER_COUNT];
	ID3D12GraphicsCommandList *list;
	ID3D12Fence *fence;
	UINT64 fence_value;
	UINT64 frame_fence_values[BUFFER_COUNT];
	HANDLE fence_event;

	IDXGISwapChain3 *swap_chain;
	HANDLE frame_latency;
	ID3D12DescriptorHeap *rtv_heap;
	UINT rtv_increment;
	ID3D12Resource *buffers[BUFFER_COUNT];

	ID3D12CommandAllocator *copy_allocator;
	ID3D12GraphicsCommandList *copy_list;
	ID3D12Resource *upload, *destination;
	unsigned char *upload_data;
	uint32_t payload_size, payload_offset;
	LARGE_INTEGER payload_start;
} renderer;

static LARGE_INTEGER counter_frequency;

static void fail(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	if (renderer.shared)
	{
		vsnprintf(renderer.shared->error, sizeof(renderer.shared->error), format, arguments);
		MemoryBarrier();
		renderer.shared->renderer_state = _spike_renderer_failed;
	}
	va_end(arguments);
	va_start(arguments, format);
	fputs("spike_renderer: ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
	exit(1);
}

#define CHECK(call, what) do { HRESULT check_result = (call); \
	if (FAILED(check_result)) fail("%s failed (0x%08lX)", what, (unsigned long)check_result); } while (0)

static uint32_t microseconds_since(LARGE_INTEGER start)
{
	LARGE_INTEGER now;

	QueryPerformanceCounter(&now);
	return (uint32_t)((now.QuadPart - start.QuadPart) * 1000000 / counter_frequency.QuadPart);
}

/* ---------- the device */

static void gpu_wait(UINT64 value)
{
	if (ID3D12Fence_GetCompletedValue(renderer.fence) < value)
	{
		CHECK(ID3D12Fence_SetEventOnCompletion(renderer.fence, value, renderer.fence_event), "SetEventOnCompletion");
		WaitForSingleObject(renderer.fence_event, INFINITE);
	}
}

static UINT64 gpu_signal(void)
{
	renderer.fence_value++;
	CHECK(ID3D12CommandQueue_Signal(renderer.queue, renderer.fence, renderer.fence_value), "Signal");
	return renderer.fence_value;
}

static ID3D12Resource *buffer_create(D3D12_HEAP_TYPE heap_type, UINT64 size, D3D12_RESOURCE_STATES state)
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
	CHECK(ID3D12Device_CreateCommittedResource(renderer.device, &heap, D3D12_HEAP_FLAG_NONE, &description, state,
		NULL, &IID_ID3D12Resource, (void **)&resource), "CreateCommittedResource");
	return resource;
}

static void device_create(void)
{
	IDXGIFactory6 *factory;
	IDXGIAdapter1 *adapter;
	DXGI_ADAPTER_DESC1 adapter_description;
	D3D12_COMMAND_QUEUE_DESC queue_description;
	UINT factory_flags = 0;
	int index;

	if (GetEnvironmentVariableA("SPIKE_D3D12_DEBUG", NULL, 0))
	{
		ID3D12Debug *debug;

		if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&debug)))
		{
			ID3D12Debug_EnableDebugLayer(debug);
			ID3D12Debug_Release(debug);
			factory_flags = DXGI_CREATE_FACTORY_DEBUG;
		}
	}
	CHECK(CreateDXGIFactory2(factory_flags, &IID_IDXGIFactory6, (void **)&factory), "CreateDXGIFactory2");
	CHECK(IDXGIFactory6_EnumAdapterByGpuPreference(factory, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
		&IID_IDXGIAdapter1, (void **)&adapter), "EnumAdapterByGpuPreference");
	CHECK(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&renderer.device),
		"D3D12CreateDevice");
	IDXGIAdapter1_GetDesc1(adapter, &adapter_description);
	WideCharToMultiByte(CP_UTF8, 0, adapter_description.Description, -1, renderer.shared->adapter_name,
		sizeof(renderer.shared->adapter_name), NULL, NULL);
	IDXGIAdapter1_Release(adapter);

	memset(&queue_description, 0, sizeof(queue_description));
	queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	CHECK(ID3D12Device_CreateCommandQueue(renderer.device, &queue_description, &IID_ID3D12CommandQueue,
		(void **)&renderer.queue), "CreateCommandQueue");
	for (index = 0; index < BUFFER_COUNT; index++)
	{
		CHECK(ID3D12Device_CreateCommandAllocator(renderer.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
			&IID_ID3D12CommandAllocator, (void **)&renderer.allocators[index]), "CreateCommandAllocator");
	}
	CHECK(ID3D12Device_CreateCommandList(renderer.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, renderer.allocators[0],
		NULL, &IID_ID3D12GraphicsCommandList, (void **)&renderer.list), "CreateCommandList");
	ID3D12GraphicsCommandList_Close(renderer.list);
	CHECK(ID3D12Device_CreateCommandAllocator(renderer.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
		&IID_ID3D12CommandAllocator, (void **)&renderer.copy_allocator), "CreateCommandAllocator");
	CHECK(ID3D12Device_CreateCommandList(renderer.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, renderer.copy_allocator,
		NULL, &IID_ID3D12GraphicsCommandList, (void **)&renderer.copy_list), "CreateCommandList");
	ID3D12GraphicsCommandList_Close(renderer.copy_list);
	CHECK(ID3D12Device_CreateFence(renderer.device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
		(void **)&renderer.fence), "CreateFence");
	renderer.fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);

	/* the swap chain presents into the host's composition surface */
	{
		IDXGIFactoryMedia *media;
		IDXGISwapChain1 *swap_chain;
		DXGI_SWAP_CHAIN_DESC1 description;
		D3D12_DESCRIPTOR_HEAP_DESC heap_description;

		CHECK(IDXGIFactory6_QueryInterface(factory, &IID_IDXGIFactoryMedia, (void **)&media), "IDXGIFactoryMedia");
		memset(&description, 0, sizeof(description));
		description.Width = renderer.width;
		description.Height = renderer.height;
		description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		description.BufferCount = BUFFER_COUNT;
		description.Scaling = DXGI_SCALING_STRETCH;
		description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
		description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
		CHECK(IDXGIFactoryMedia_CreateSwapChainForCompositionSurfaceHandle(media, (IUnknown *)renderer.queue,
			renderer.surface, &description, NULL, &swap_chain), "CreateSwapChainForCompositionSurfaceHandle");
		CHECK(IDXGISwapChain1_QueryInterface(swap_chain, &IID_IDXGISwapChain3, (void **)&renderer.swap_chain),
			"IDXGISwapChain3");
		IDXGISwapChain1_Release(swap_chain);
		IDXGIFactoryMedia_Release(media);
		CHECK(IDXGISwapChain3_SetMaximumFrameLatency(renderer.swap_chain, SPIKE_FRAMES_IN_FLIGHT),
			"SetMaximumFrameLatency");
		renderer.frame_latency = IDXGISwapChain3_GetFrameLatencyWaitableObject(renderer.swap_chain);

		memset(&heap_description, 0, sizeof(heap_description));
		heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		heap_description.NumDescriptors = BUFFER_COUNT;
		CHECK(ID3D12Device_CreateDescriptorHeap(renderer.device, &heap_description, &IID_ID3D12DescriptorHeap,
			(void **)&renderer.rtv_heap), "CreateDescriptorHeap");
		renderer.rtv_increment = ID3D12Device_GetDescriptorHandleIncrementSize(renderer.device,
			D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	}
	IDXGIFactory6_Release(factory);

	renderer.upload = buffer_create(D3D12_HEAP_TYPE_UPLOAD, SPIKE_PAYLOAD_MAXIMUM, D3D12_RESOURCE_STATE_GENERIC_READ);
	renderer.destination = buffer_create(D3D12_HEAP_TYPE_DEFAULT, SPIKE_PAYLOAD_MAXIMUM, D3D12_RESOURCE_STATE_COMMON);
	CHECK(ID3D12Resource_Map(renderer.upload, 0, NULL, (void **)&renderer.upload_data), "Map");
}

static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(int index)
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle;

	ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(renderer.rtv_heap, &handle);
	handle.ptr += (SIZE_T)index * renderer.rtv_increment;
	return handle;
}

static void buffers_get(void)
{
	int index;

	for (index = 0; index < BUFFER_COUNT; index++)
	{
		CHECK(IDXGISwapChain3_GetBuffer(renderer.swap_chain, index, &IID_ID3D12Resource,
			(void **)&renderer.buffers[index]), "GetBuffer");
		ID3D12Device_CreateRenderTargetView(renderer.device, renderer.buffers[index], NULL, rtv_handle(index));
	}
}

static void buffers_resize(unsigned long width, unsigned long height)
{
	int index;

	gpu_wait(gpu_signal());
	for (index = 0; index < BUFFER_COUNT; index++)
	{
		ID3D12Resource_Release(renderer.buffers[index]);
		renderer.buffers[index] = NULL;
	}
	CHECK(IDXGISwapChain3_ResizeBuffers(renderer.swap_chain, 0, width, height, DXGI_FORMAT_UNKNOWN,
		DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT), "ResizeBuffers");
	renderer.width = width;
	renderer.height = height;
	buffers_get();
}

static void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
	D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER barrier;

	memset(&barrier, 0, sizeof(barrier));
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
}

/* ---------- messages */

static void frame_draw(const struct spike_frame *frame)
{
	UINT index;
	ID3D12CommandList *lists[1];
	float color[4];
	static const float bar_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	D3D12_RECT bar;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv;

	WaitForSingleObject(renderer.frame_latency, 1000);
	index = IDXGISwapChain3_GetCurrentBackBufferIndex(renderer.swap_chain);
	gpu_wait(renderer.frame_fence_values[index]);
	CHECK(ID3D12CommandAllocator_Reset(renderer.allocators[index]), "Reset");
	CHECK(ID3D12GraphicsCommandList_Reset(renderer.list, renderer.allocators[index], NULL), "Reset");

	rtv = rtv_handle(index);
	transition(renderer.list, renderer.buffers[index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	color[0] = frame->red;
	color[1] = frame->green;
	color[2] = frame->blue;
	color[3] = 1.0f;
	ID3D12GraphicsCommandList_ClearRenderTargetView(renderer.list, rtv, color, 0, NULL);
	/* a bar moving across the top, to see the frames advance */
	bar.left = (LONG)((frame->frame * 8) % (renderer.width > 64 ? renderer.width - 64 : 1));
	bar.right = bar.left + 64;
	bar.top = 0;
	bar.bottom = (LONG)(renderer.height / 8);
	ID3D12GraphicsCommandList_ClearRenderTargetView(renderer.list, rtv, bar_color, 1, &bar);
	transition(renderer.list, renderer.buffers[index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
	CHECK(ID3D12GraphicsCommandList_Close(renderer.list), "Close");

	lists[0] = (ID3D12CommandList *)renderer.list;
	ID3D12CommandQueue_ExecuteCommandLists(renderer.queue, 1, lists);
	CHECK(IDXGISwapChain3_Present(renderer.swap_chain, 1, 0), "Present");
	renderer.frame_fence_values[index] = gpu_signal();
	InterlockedIncrement((volatile LONG *)&renderer.shared->frames_presented);
	ReleaseSemaphore(renderer.frame_semaphore, 1, NULL);
}

static void payload_end(void)
{
	ID3D12CommandList *lists[1];
	LARGE_INTEGER copy_start;

	QueryPerformanceCounter(&copy_start);
	CHECK(ID3D12CommandAllocator_Reset(renderer.copy_allocator), "Reset");
	CHECK(ID3D12GraphicsCommandList_Reset(renderer.copy_list, renderer.copy_allocator, NULL), "Reset");
	ID3D12GraphicsCommandList_CopyBufferRegion(renderer.copy_list, renderer.destination, 0, renderer.upload, 0,
		renderer.payload_size);
	CHECK(ID3D12GraphicsCommandList_Close(renderer.copy_list), "Close");
	lists[0] = (ID3D12CommandList *)renderer.copy_list;
	ID3D12CommandQueue_ExecuteCommandLists(renderer.queue, 1, lists);
	/* the upload buffer is written again by the next payload */
	gpu_wait(gpu_signal());
	renderer.shared->gpu_copy_microseconds = microseconds_since(copy_start);
	renderer.shared->payload_microseconds = microseconds_since(renderer.payload_start);
	InterlockedIncrement((volatile LONG *)&renderer.shared->payloads_completed);
}

/* FALSE when the host has asked the renderer to exit */
static BOOL message_handle(const struct spike_message_header *header, const void *data)
{
	switch (header->opcode)
	{
	case _spike_opcode_pad:
		break;
	case _spike_opcode_resize:
	{
		const struct spike_resize *resize = data;

		if (header->size != sizeof(*resize) || !resize->width || !resize->height ||
			resize->width > 16384 || resize->height > 16384)
		{
			fail("a resize message is malformed");
		}
		buffers_resize(resize->width, resize->height);
		break;
	}
	case _spike_opcode_frame:
		if (header->size != sizeof(struct spike_frame))
			fail("a frame message is malformed");
		frame_draw(data);
		break;
	case _spike_opcode_payload_begin:
	{
		const struct spike_payload_begin *begin = data;

		if (header->size != sizeof(*begin) || begin->total_size > SPIKE_PAYLOAD_MAXIMUM)
			fail("a payload's size is malformed");
		renderer.payload_size = begin->total_size;
		renderer.payload_offset = 0;
		QueryPerformanceCounter(&renderer.payload_start);
		break;
	}
	case _spike_opcode_payload_chunk:
		if (header->size > renderer.payload_size - renderer.payload_offset)
			fail("a payload chunk runs past its payload");
		memcpy(renderer.upload_data + renderer.payload_offset, data, header->size);
		renderer.payload_offset += header->size;
		break;
	case _spike_opcode_payload_end:
		if (renderer.payload_offset != renderer.payload_size)
			fail("a payload ended before all of it arrived");
		payload_end();
		break;
	case _spike_opcode_quit:
		return FALSE;
	default:
		fail("unknown message %u", header->opcode);
	}
	return TRUE;
}

static void ring_run(void)
{
	for (;;)
	{
		uint32_t read = renderer.shared->read_position;
		uint32_t written = renderer.shared->write_position;

		if (read == written)
		{
			WaitForSingleObject(renderer.data_event, 100);
			continue;
		}
		MemoryBarrier();
		while (read != written)
		{
			uint32_t offset = read & (SPIKE_RING_SIZE - 1);
			const struct spike_message_header *header = (const struct spike_message_header *)(renderer.ring + offset);
			uint32_t size = header->size;
			uint32_t total;
			BOOL running;

			/* the message lies within what was written and within the ring */
			if (size > SPIKE_RING_SIZE - sizeof(*header) - offset)
				fail("a message runs past the ring's end");
			total = (uint32_t)sizeof(*header) + SPIKE_ALIGN(size);
			if (total > written - read)
				fail("a message runs past what was written");
			running = message_handle(header, header + 1);
			read += total;
			InterlockedExchange((volatile LONG *)&renderer.shared->read_position, (LONG)read);
			SetEvent(renderer.space_event);
			if (!running)
				return;
		}
	}
}

static HANDLE handle_argument(const char *argument)
{
	return (HANDLE)(ULONG_PTR)strtoul(argument, NULL, 16);
}

int main(int argc, char **argv)
{
	HANDLE section;

	QueryPerformanceFrequency(&counter_frequency);
	if (argc != 8)
	{
		fputs("spike_renderer is started by spike_host\n", stderr);
		return 2;
	}
	section = handle_argument(argv[1]);
	renderer.data_event = handle_argument(argv[2]);
	renderer.space_event = handle_argument(argv[3]);
	renderer.frame_semaphore = handle_argument(argv[4]);
	renderer.surface = handle_argument(argv[5]);
	renderer.width = strtoul(argv[6], NULL, 10);
	renderer.height = strtoul(argv[7], NULL, 10);

	renderer.shared = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, SPIKE_SECTION_SIZE);
	if (!renderer.shared)
	{
		fprintf(stderr, "spike_renderer: the shared section could not be mapped (%lu)\n", GetLastError());
		return 1;
	}
	if (renderer.shared->version != SPIKE_PROTOCOL_VERSION || renderer.shared->ring_size != SPIKE_RING_SIZE)
		fail("the host speaks protocol %u, the renderer %u", renderer.shared->version, SPIKE_PROTOCOL_VERSION);
	renderer.ring = (const unsigned char *)renderer.shared + SPIKE_HEADER_SIZE;
	if (!renderer.width || !renderer.height || renderer.width > 16384 || renderer.height > 16384)
		fail("the window size %lux%lu is not usable", renderer.width, renderer.height);

	device_create();
	buffers_get();
	MemoryBarrier();
	renderer.shared->renderer_state = _spike_renderer_ready;

	ring_run();
	gpu_wait(gpu_signal());
	return 0;
}
