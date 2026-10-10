/*
RENDERER_MAIN.C

The renderer process: started by the game (win32_renderer_host.c) with the
handles it inherits,

	halo_renderer.exe <section> <data event> <space event> <frame semaphore>
		<composition surface> <width> <height>

in hexadecimal but for the window's size; it maps the shared section, makes
the device and reads the ring until the game says to quit. Each message's
size is checked against what its kind holds before anything reads it.

A crash writes halo_renderer.dmp beside the log (a minidump, for a debugger)
and the exception into the log and the shared header, which the game shows.
*/

#include "renderer.h"

#include <dbghelp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct xgpu_header *renderer_header;
struct xgpu_results *renderer_results;

static const uint8_t *ring;
static HANDLE data_event, space_event, frame_semaphore;
static FILE *log_file;
static char log_path[MAX_PATH];
/* what the device is made with, made again with after it is lost */
static HANDLE surface;
static BOOL debug_layer;

void renderer_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	fputs("halo_renderer: ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
	if (log_file)
	{
		va_start(arguments, format);
		vfprintf(log_file, format, arguments);
		fputc('\n', log_file);
		fflush(log_file);
		va_end(arguments);
	}
}

void renderer_fail(const char *format, ...)
{
	char message[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	renderer_debug_messages();
	renderer_log("%s", message);
	if (renderer_header)
	{
		memcpy(renderer_header->error, message, sizeof(message) < sizeof(renderer_header->error) ?
			sizeof(message) : sizeof(renderer_header->error));
		renderer_header->error[sizeof(renderer_header->error) - 1] = '\0';
		MemoryBarrier();
		renderer_header->renderer_state = _xgpu_renderer_failed;
	}
	/* (the game waits on this semaphore; it then finds the state) */
	if (frame_semaphore)
		ReleaseSemaphore(frame_semaphore, XGPU_FRAMES_IN_FLIGHT, NULL);
	exit(1);
}

void renderer_check(HRESULT result, const char *what)
{
	if (FAILED(result))
	{
		if (result == DXGI_ERROR_DEVICE_REMOVED && renderer.device)
			result = ID3D12Device_GetDeviceRemovedReason(renderer.device);
		if (renderer_lost_result(result) && renderer_recovery_armed)
		{
			renderer_log("%s: the GPU was lost (0x%08lX)", what, (unsigned long)result);
			renderer_device_lost(result);
		}
		renderer_fail("%s failed (0x%08lX)", what, (unsigned long)result);
	}
}

/* ---------- messages */

/* the message's payload is a struct of this size, and maybe more after it */
#define NEEDS(type) do { if (size < sizeof(type)) \
	renderer_fail("a message of kind %u is %u bytes, too short", opcode, size); } while (0)
#define EXACTLY(type) do { if (size != sizeof(type)) \
	renderer_fail("a message of kind %u is %u bytes, not %u", opcode, size, (unsigned)sizeof(type)); } while (0)

/* FALSE once the game has said to quit */
static BOOL message_handle(uint32_t opcode, const uint8_t *data, uint32_t size)
{
	switch (opcode)
	{
	case _xgpu_op_pad:
		break;
	case _xgpu_op_configure:
	{
		const struct xgpu_configure *configure = (const void *)data;

		EXACTLY(struct xgpu_configure);
		renderer.vsync = configure->vsync != 0;
		renderer_path_tracer_configure(configure->path_tracing != 0);
		renderer_path_tracer_view(configure->path_tracing_view);
		break;
	}
	case _xgpu_op_resize:
	{
		const struct xgpu_resize *resize = (const void *)data;

		EXACTLY(struct xgpu_resize);
		if (!resize->width || !resize->height || resize->width > 16384 || resize->height > 16384)
			renderer_fail("a window size of %ux%u", resize->width, resize->height);
		renderer_device_resize(resize->width, resize->height);
		break;
	}
	case _xgpu_op_texture_define:
		EXACTLY(struct xgpu_texture_define);
		renderer_texture_define((const void *)data);
		break;
	case _xgpu_op_texture_data:
		NEEDS(struct xgpu_texture_data);
		renderer_texture_data((const void *)data, data + sizeof(struct xgpu_texture_data),
			size - (uint32_t)sizeof(struct xgpu_texture_data));
		break;
	case _xgpu_op_texture_mipmaps:
		EXACTLY(struct xgpu_texture_reference);
		renderer_texture_mipmaps((const void *)data);
		break;
	case _xgpu_op_texture_delete:
		EXACTLY(struct xgpu_texture_reference);
		renderer_texture_delete(((const struct xgpu_texture_reference *)data)->texture);
		break;
	case _xgpu_op_target_define:
		EXACTLY(struct xgpu_target_define);
		renderer_target_define((const void *)data);
		break;
	case _xgpu_op_target_delete:
		EXACTLY(struct xgpu_target_reference);
		renderer_target_delete(((const struct xgpu_target_reference *)data)->target);
		break;
	case _xgpu_op_target_resolve:
		EXACTLY(struct xgpu_target_reference);
		renderer_target_resolve(((const struct xgpu_target_reference *)data)->target);
		break;
	case _xgpu_op_target_copy:
		EXACTLY(struct xgpu_target_copy);
		renderer_target_copy((const void *)data);
		break;
	case _xgpu_op_mirror_write:
		NEEDS(struct xgpu_mirror_write);
		renderer_mirror_write((const void *)data, data + sizeof(struct xgpu_mirror_write),
			size - (uint32_t)sizeof(struct xgpu_mirror_write));
		break;
	case _xgpu_op_vertex_shader:
	case _xgpu_op_pixel_shader:
		NEEDS(struct xgpu_shader);
		if (opcode == _xgpu_op_vertex_shader)
			renderer_vertex_shader((const void *)data, (const char *)data + sizeof(struct xgpu_shader),
				size - (uint32_t)sizeof(struct xgpu_shader));
		else
			renderer_pixel_shader((const void *)data, (const char *)data + sizeof(struct xgpu_shader),
				size - (uint32_t)sizeof(struct xgpu_shader));
		break;
	case _xgpu_op_vertex_constants:
	{
		const struct xgpu_vertex_constants *constants = (const void *)data;

		NEEDS(struct xgpu_vertex_constants);
		if (constants->first >= XGPU_CONSTANT_COUNT || constants->count > XGPU_CONSTANT_COUNT - constants->first ||
			size != sizeof(*constants) + constants->count * 16)
		{
			renderer_fail("vertex constants out of range");
		}
		renderer_vertex_constants(constants, (const float *)(data + sizeof(*constants)));
		break;
	}
	case _xgpu_op_draw_uniforms:
		EXACTLY(struct xgpu_hlsl_draw_uniforms);
		renderer_draw_uniforms((const void *)data);
		break;
	case _xgpu_op_model_lights:
		if (size != XGPU_MODEL_LIGHT_COUNT * 16)
			renderer_fail("model lights of %u bytes", size);
		renderer_model_lights((const float *)data);
		break;
	case _xgpu_op_draw_state:
		EXACTLY(struct xgpu_draw_state);
		renderer_draw_state((const void *)data);
		break;
	case _xgpu_op_draw:
	{
		const struct xgpu_draw *draw = (const void *)data;

		NEEDS(struct xgpu_draw);
		if (draw->inline_size != size - sizeof(*draw))
			renderer_fail("a draw's inline data is %u bytes, not %u", (unsigned)(size - sizeof(*draw)), draw->inline_size);
		renderer_draw(draw, data + sizeof(*draw));
		break;
	}
	case _xgpu_op_clear:
		EXACTLY(struct xgpu_clear);
		renderer_clear((const void *)data);
		break;
	case _xgpu_op_visibility_begin:
		renderer_visibility_begin();
		break;
	case _xgpu_op_visibility_end:
		EXACTLY(struct xgpu_visibility);
		renderer_visibility_end(((const struct xgpu_visibility *)data)->slot);
		break;
	case _xgpu_op_anti_alias:
		EXACTLY(struct xgpu_anti_alias);
		renderer_anti_alias((const void *)data);
		break;
	case _xgpu_op_anti_alias_prepare:
		NEEDS(struct xgpu_anti_alias_prepare);
		renderer_anti_alias_prepare((const void *)data, (const char *)data + sizeof(struct xgpu_anti_alias_prepare),
			size - (uint32_t)sizeof(struct xgpu_anti_alias_prepare));
		break;
	case _xgpu_op_present:
		EXACTLY(struct xgpu_present);
		renderer_present((const void *)data);
		/* the game may send the next frame */
		ReleaseSemaphore(frame_semaphore, 1, NULL);
		break;
	case _xgpu_op_screenshot:
		EXACTLY(struct xgpu_screenshot);
		renderer_screenshot((const void *)data);
		break;
	case _xgpu_op_quit:
		return FALSE;
	case _xgpu_op_pass:
		EXACTLY(struct xgpu_pass);
		renderer_path_tracer_pass((const void *)data);
		break;
	case _xgpu_op_scene_begin_map:
		EXACTLY(struct xgpu_scene_begin_map);
		renderer_scene_begin_map((const void *)data);
		break;
	case _xgpu_op_scene_mesh:
	case _xgpu_op_scene_materials:
		NEEDS(struct xgpu_scene_piece);
		renderer_scene_piece((const void *)data, data + sizeof(struct xgpu_scene_piece),
			size - (uint32_t)sizeof(struct xgpu_scene_piece));
		break;
	case _xgpu_op_scene_drawn:
	{
		const struct xgpu_scene_drawn *drawn = (const void *)data;

		NEEDS(struct xgpu_scene_drawn);
		if ((uint64_t)drawn->count * sizeof(uint32_t) != size - sizeof(*drawn))
			renderer_fail("%u surfaces drawn in %u bytes", drawn->count, size);
		renderer_scene_drawn((const uint32_t *)(data + sizeof(*drawn)), drawn->count);
		break;
	}
	case _xgpu_op_scene_camera:
		EXACTLY(struct xgpu_scene_camera);
		renderer_scene_camera((const void *)data);
		break;
	case _xgpu_op_debug_remove_device:
		renderer_remove_device();
		/* (found lost at once, rather than at the next wait) */
		renderer_wait(renderer_submit());
		break;
	default:
		/* the path tracer's scene messages are not taken yet */
		if (opcode >= _xgpu_op_scene_begin_map && opcode < NUMBER_OF_XGPU_OPCODES)
			break;
		renderer_fail("unknown message %u", opcode);
	}
	return TRUE;
}

/* ---------- a lost device */

/* recoveries since a frame was last presented: a device lost again and again
before any frame is drawn is not recovered from */
static int recoveries_without_frame;

static void device_recover(void)
{
	renderer_log("the GPU was lost (0x%08lX): making the device again", (unsigned long)renderer_lost_reason);
	if (++recoveries_without_frame > 3)
		renderer_fail("the GPU was lost again and again (0x%08lX)", (unsigned long)renderer_lost_reason);
	renderer_path_tracer_lost();
	renderer_scene_lost();
	renderer_resources_lost();
	renderer_post_lost();
	renderer_output_lost();
	renderer_draw_lost();
	renderer_pipeline_lost();
	renderer_device_release();
	renderer_device_create(surface, renderer.width, renderer.height, debug_layer);
	renderer_pipeline_restore();
	renderer_output_restore();
	renderer_post_restore();
	renderer_path_tracer_restore();
	renderer_scene_restore();
	renderer_draw_restore();
	renderer_resources_restore();
	renderer_wait(renderer_submit());
	InterlockedIncrement((volatile LONG *)&renderer_header->device_resets);
	renderer_log("the device is made again, and everything restored");
}

/* the message being handled when the device was lost is passed over (its
work is lost with the frame), a present as if it were shown */
static void message_skip(void)
{
	uint32_t read = renderer_header->read_position;
	const struct xgpu_message_header *header = (const void *)(ring + (read & (XGPU_RING_SIZE - 1)));

	read += (uint32_t)sizeof(*header) + XGPU_ALIGN(header->size);
	if (header->opcode == _xgpu_op_present)
	{
		InterlockedIncrement((volatile LONG *)&renderer_header->frames_presented);
		ReleaseSemaphore(frame_semaphore, 1, NULL);
	}
	InterlockedExchange((volatile LONG *)&renderer_header->read_position, (LONG)read);
	SetEvent(space_event);
}

static void ring_run(void)
{
	/* (a lost device comes back here, from wherever it was found) */
	if (setjmp(renderer_recovery))
	{
		device_recover();
		message_skip();
	}
	renderer_recovery_armed = TRUE;
	for (;;)
	{
		uint32_t read = renderer_header->read_position;
		uint32_t written = renderer_header->write_position;

		if (read == written)
		{
			/* (the flag first, then a look again: a message written in
			between is either seen now or signalled) */
			InterlockedExchange((volatile LONG *)&renderer_header->renderer_waiting, 1);
			if (renderer_header->write_position == read)
				WaitForSingleObject(data_event, 50);
			InterlockedExchange((volatile LONG *)&renderer_header->renderer_waiting, 0);
			continue;
		}
		MemoryBarrier();
		while (read != written)
		{
			uint32_t offset = read & (XGPU_RING_SIZE - 1);
			const struct xgpu_message_header *header = (const void *)(ring + offset);
			uint32_t opcode = header->opcode, size = header->size, total;
			BOOL running;

			/* the message lies within the ring and within what was written */
			if (size > XGPU_RING_SIZE - sizeof(*header) - offset || size > XGPU_MESSAGE_MAXIMUM + 4096)
				renderer_fail("a message runs past the ring's end");
			total = (uint32_t)sizeof(*header) + XGPU_ALIGN(size);
			if (total > written - read)
				renderer_fail("a message runs past what was written");
			renderer_submit_if_large();
			running = message_handle(opcode, (const uint8_t *)(header + 1), size);
			if (opcode == _xgpu_op_present)
				recoveries_without_frame = 0;
			read += total;
			InterlockedExchange((volatile LONG *)&renderer_header->read_position, (LONG)read);
			SetEvent(space_event);
			if (!running)
				return;
		}
	}
}

/* ---------- a crash */

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *exception)
{
	static LONG entered;
	char dump_path[MAX_PATH + 8];
	char *slash;
	HANDLE file;

	if (InterlockedExchange(&entered, 1))
		return EXCEPTION_CONTINUE_SEARCH;
	/* (the dump beside the log) */
	strcpy(dump_path, log_path);
	slash = strrchr(dump_path, '\\');
	strcpy(slash ? slash + 1 : dump_path, "halo_renderer.dmp");
	file = CreateFileA(dump_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file != INVALID_HANDLE_VALUE)
	{
		MINIDUMP_EXCEPTION_INFORMATION information;

		information.ThreadId = GetCurrentThreadId();
		information.ExceptionPointers = exception;
		information.ClientPointers = FALSE;
		MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
			(MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo), &information, NULL, NULL);
		CloseHandle(file);
	}
	renderer_fail("the renderer crashed: exception 0x%08lX at %p (%s)",
		(unsigned long)exception->ExceptionRecord->ExceptionCode, exception->ExceptionRecord->ExceptionAddress,
		file != INVALID_HANDLE_VALUE ? dump_path : "no minidump written");
}

static HANDLE handle_argument(const char *argument)
{
	return (HANDLE)(ULONG_PTR)strtoull(argument, NULL, 16);
}

int main(int argc, char **argv)
{
	HANDLE section;
	uint32_t width, height;

	if (argc != 8)
	{
		fputs("halo_renderer is started by the game (halo.exe)\n", stderr);
		return 2;
	}
	section = handle_argument(argv[1]);
	data_event = handle_argument(argv[2]);
	space_event = handle_argument(argv[3]);
	frame_semaphore = handle_argument(argv[4]);
	surface = handle_argument(argv[5]);
	width = (uint32_t)strtoul(argv[6], NULL, 10);
	height = (uint32_t)strtoul(argv[7], NULL, 10);

	/* (halo_renderer.log beside the executable, as halo.log, or where
	HALO_RENDERER_LOG names: several copies of the game at once) */
	if (GetEnvironmentVariableA("HALO_RENDERER_LOG", log_path, sizeof(log_path)) &&
		strlen(log_path) < sizeof(log_path) - 1)
	{
		log_file = fopen(log_path, "w");
	}
	else if (GetModuleFileNameA(NULL, log_path, sizeof(log_path)))
	{
		char *dot = strrchr(log_path, '.');

		if (dot)
		{
			strcpy(dot, ".log");
			log_file = fopen(log_path, "w");
		}
	}

	if (!log_file)
		strcpy(log_path, "halo_renderer.log");
	SetUnhandledExceptionFilter(crash_filter);

	renderer_header = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, XGPU_SECTION_SIZE);
	if (!renderer_header)
	{
		renderer_log("the shared section cannot be mapped (%lu)", GetLastError());
		return 1;
	}
	if (renderer_header->version != XGPU_PROTOCOL_VERSION || renderer_header->ring_size != XGPU_RING_SIZE)
		renderer_fail("the game speaks protocol %u, the renderer %u", renderer_header->version, XGPU_PROTOCOL_VERSION);
	renderer_results = (struct xgpu_results *)((uint8_t *)renderer_header + XGPU_HEADER_SIZE);
	ring = (const uint8_t *)renderer_header + XGPU_HEADER_SIZE + XGPU_RESULTS_SIZE;
	if (!width || !height || width > 16384 || height > 16384)
		renderer_fail("the window size %ux%u is not usable", width, height);

	debug_layer = GetEnvironmentVariableA("HALO_D3D12_DEBUG_LAYER", NULL, 0) != 0;
	renderer_device_create(surface, width, height, debug_layer);
	renderer_resources_initialize();
	renderer_pipeline_initialize();
	renderer_draw_initialize();
	renderer_output_initialize();
	renderer_path_tracer_initialize();
	renderer_header->maximum_samples = renderer.maximum_samples;
	renderer_header->maximum_texture_size = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
	MemoryBarrier();
	renderer_header->renderer_state = _xgpu_renderer_ready;
	renderer_log("ready on %s", renderer_header->adapter_name);

	ring_run();
	renderer_recovery_armed = FALSE;
	renderer_wait(renderer_submit());
	renderer_screenshots_collect();
	renderer_log("the game said to quit");
	return 0;
}
