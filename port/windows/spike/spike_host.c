/*
SPIKE_HOST.C

The game's side of the D3D12 renderer spike (README.md), built 32-bit as the
game is: a window that keeps its own input, the shared ring, and the 64-bit
renderer process it starts and feeds.

	spike_host.exe           runs until the window is closed; T sends 64 MB
	                         of payloads through the ring
	spike_host.exe --test    runs a fixed check and exits: 0 if it passed
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellscalingapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "spike_protocol.h"

int spike_dcomp_attach(HWND window, HANDLE *surface_handle, char *error, size_t error_size);

static struct
{
	HWND window;
	BOOL quit;
	unsigned long clicks, keys;
	BOOL payload_requested;
	BOOL resized;
	unsigned long width, height;

	HANDLE section;
	struct spike_shared *shared;
	unsigned char *ring;
	HANDLE data_event, space_event, frame_semaphore;
	HANDLE surface;
	HANDLE job;
	HANDLE renderer;
} host;

static LARGE_INTEGER counter_frequency;

static double seconds_now(void)
{
	LARGE_INTEGER now;

	QueryPerformanceCounter(&now);
	return (double)now.QuadPart / (double)counter_frequency.QuadPart;
}

static void fail(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	fputs("spike_host: ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
	if (host.shared && host.shared->renderer_state == _spike_renderer_failed)
		fprintf(stderr, "spike_host: the renderer reported: %s\n", host.shared->error);
	exit(1);
}

/* ---------- the window */

static LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message)
	{
	case WM_CLOSE:
		host.quit = TRUE;
		return 0;
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
		host.clicks++;
		break;
	case WM_KEYDOWN:
		host.keys++;
		if (wparam == 'T')
			host.payload_requested = TRUE;
		if (wparam == VK_ESCAPE)
			host.quit = TRUE;
		break;
	case WM_SIZE:
		if (LOWORD(lparam) && HIWORD(lparam))
		{
			host.width = LOWORD(lparam);
			host.height = HIWORD(lparam);
			host.resized = TRUE;
		}
		break;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

static void window_create(BOOL topmost)
{
	WNDCLASSW window_class;
	RECT bounds = { 0, 0, 1280, 720 };

	memset(&window_class, 0, sizeof(window_class));
	window_class.lpfnWndProc = window_procedure;
	window_class.hInstance = GetModuleHandleW(NULL);
	window_class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	window_class.lpszClassName = L"spike_host";
	RegisterClassW(&window_class);
	AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0);
	host.window = CreateWindowExW(topmost ? WS_EX_TOPMOST : 0, L"spike_host", L"D3D12 renderer spike",
		WS_OVERLAPPEDWINDOW, 100, 100, bounds.right - bounds.left, bounds.bottom - bounds.top,
		NULL, NULL, window_class.hInstance, NULL);
	if (!host.window)
		fail("CreateWindowExW failed (%lu)", GetLastError());
	GetClientRect(host.window, &bounds);
	host.width = bounds.right;
	host.height = bounds.bottom;
	host.resized = FALSE;
	ShowWindow(host.window, SW_SHOW);
	SetForegroundWindow(host.window);
}

static void window_pump(void)
{
	MSG message;

	while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
	{
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
}

/* ---------- the ring */

static void wait_for_renderer(HANDLE event, const char *what)
{
	HANDLE handles[2];
	DWORD result;

	handles[0] = event;
	handles[1] = host.renderer;
	result = WaitForMultipleObjects(2, handles, FALSE, 5000);
	if (result == WAIT_OBJECT_0 + 1)
		fail("the renderer exited while the host waited for %s", what);
	if (result == WAIT_TIMEOUT && event == host.frame_semaphore)
		fail("the renderer presented nothing for 5 seconds");
}

/* room for a message of size bytes after its header, contiguous; the
message is sent by ring_commit */
static void *ring_reserve(uint32_t opcode, uint32_t size)
{
	uint32_t total = sizeof(struct spike_message_header) + SPIKE_ALIGN(size);
	uint32_t position = host.shared->write_position;
	uint32_t offset = position & (SPIKE_RING_SIZE - 1);
	uint32_t pad = offset + total > SPIKE_RING_SIZE ? SPIKE_RING_SIZE - offset : 0;
	struct spike_message_header *header;

	while (SPIKE_RING_SIZE - (position - host.shared->read_position) < pad + total)
		wait_for_renderer(host.space_event, "room in the ring");
	if (pad)
	{
		header = (struct spike_message_header *)(host.ring + offset);
		header->opcode = _spike_opcode_pad;
		header->size = pad - sizeof(*header);
		position += pad;
		offset = 0;
	}
	header = (struct spike_message_header *)(host.ring + offset);
	header->opcode = opcode;
	header->size = size;
	header->reserved[0] = header->reserved[1] = position + total;
	return header + 1;
}

static void ring_commit(void *data)
{
	struct spike_message_header *header = (struct spike_message_header *)data - 1;

	MemoryBarrier();
	InterlockedExchange((volatile LONG *)&host.shared->write_position, (LONG)header->reserved[0]);
	SetEvent(host.data_event);
}

static void send_resize(void)
{
	struct spike_resize *resize = ring_reserve(_spike_opcode_resize, sizeof(*resize));

	resize->width = host.width;
	resize->height = host.height;
	ring_commit(resize);
}

static void send_frame(uint32_t frame, float red, float green, float blue)
{
	struct spike_frame *message;

	wait_for_renderer(host.frame_semaphore, "a frame");
	message = ring_reserve(_spike_opcode_frame, sizeof(*message));
	message->frame = frame;
	message->red = red;
	message->green = green;
	message->blue = blue;
	ring_commit(message);
}

/* sends count payloads of SPIKE_PAYLOAD_MAXIMUM bytes and waits for the
renderer to have copied the last to the GPU; returns the seconds taken */
static double send_payloads(int count, const unsigned char *source)
{
	uint32_t completed = host.shared->payloads_completed;
	double start = seconds_now();
	int payload;

	for (payload = 0; payload < count; payload++)
	{
		struct spike_payload_begin *begin = ring_reserve(_spike_opcode_payload_begin, sizeof(*begin));
		uint32_t offset;

		begin->total_size = SPIKE_PAYLOAD_MAXIMUM;
		ring_commit(begin);
		for (offset = 0; offset < SPIKE_PAYLOAD_MAXIMUM; offset += SPIKE_CHUNK_SIZE)
		{
			void *chunk = ring_reserve(_spike_opcode_payload_chunk, SPIKE_CHUNK_SIZE);

			memcpy(chunk, source + offset, SPIKE_CHUNK_SIZE);
			ring_commit(chunk);
		}
		ring_commit(ring_reserve(_spike_opcode_payload_end, 0));
	}
	while (host.shared->payloads_completed - completed < (uint32_t)count)
	{
		if (WaitForSingleObject(host.renderer, 1) == WAIT_OBJECT_0)
			fail("the renderer exited during the payload test");
	}
	return seconds_now() - start;
}

/* ---------- the renderer process */

static HANDLE inheritable_event(BOOL manual_reset)
{
	SECURITY_ATTRIBUTES inherited = { sizeof(inherited), NULL, TRUE };
	HANDLE event = CreateEventW(&inherited, manual_reset, FALSE, NULL);

	if (!event)
		fail("CreateEventW failed (%lu)", GetLastError());
	return event;
}

static void renderer_start(void)
{
	SECURITY_ATTRIBUTES inherited = { sizeof(inherited), NULL, TRUE };
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
	STARTUPINFOW startup;
	PROCESS_INFORMATION process;
	wchar_t path[MAX_PATH], command_line[512], *slash;
	char error[256];
	double start;

	host.section = CreateFileMappingW(INVALID_HANDLE_VALUE, &inherited, PAGE_READWRITE, 0, SPIKE_SECTION_SIZE, NULL);
	if (!host.section)
		fail("CreateFileMappingW failed (%lu)", GetLastError());
	host.shared = MapViewOfFile(host.section, FILE_MAP_ALL_ACCESS, 0, 0, SPIKE_SECTION_SIZE);
	if (!host.shared)
		fail("MapViewOfFile failed (%lu)", GetLastError());
	host.ring = (unsigned char *)host.shared + SPIKE_HEADER_SIZE;
	host.shared->version = SPIKE_PROTOCOL_VERSION;
	host.shared->ring_size = SPIKE_RING_SIZE;

	host.data_event = inheritable_event(FALSE);
	host.space_event = inheritable_event(FALSE);
	host.frame_semaphore = CreateSemaphoreW(&inherited, SPIKE_FRAMES_IN_FLIGHT, SPIKE_FRAMES_IN_FLIGHT, NULL);
	if (!host.frame_semaphore)
		fail("CreateSemaphoreW failed (%lu)", GetLastError());
	if (!spike_dcomp_attach(host.window, &host.surface, error, sizeof(error)))
		fail("%s", error);

	/* the renderer ends with the host, however the host ends */
	host.job = CreateJobObjectW(NULL, NULL);
	memset(&limits, 0, sizeof(limits));
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!host.job || !SetInformationJobObject(host.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
		fail("the job object could not be made (%lu)", GetLastError());

	GetModuleFileNameW(NULL, path, MAX_PATH);
	slash = wcsrchr(path, L'\\');
	wcscpy(slash ? slash + 1 : path, L"spike_renderer.exe");
	_snwprintf(command_line, sizeof(command_line) / sizeof(*command_line),
		L"\"%ls\" %lx %lx %lx %lx %lx %lu %lu", path,
		(unsigned long)(ULONG_PTR)host.section, (unsigned long)(ULONG_PTR)host.data_event,
		(unsigned long)(ULONG_PTR)host.space_event, (unsigned long)(ULONG_PTR)host.frame_semaphore,
		(unsigned long)(ULONG_PTR)host.surface, host.width, host.height);
	memset(&startup, 0, sizeof(startup));
	startup.cb = sizeof(startup);
	if (!CreateProcessW(path, command_line, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &startup, &process))
		fail("%ls could not be started (%lu)", path, GetLastError());
	if (!AssignProcessToJobObject(host.job, process.hProcess))
		fail("AssignProcessToJobObject failed (%lu)", GetLastError());
	ResumeThread(process.hThread);
	CloseHandle(process.hThread);
	host.renderer = process.hProcess;

	start = seconds_now();
	while (host.shared->renderer_state == _spike_renderer_starting)
	{
		window_pump();
		if (WaitForSingleObject(host.renderer, 10) == WAIT_OBJECT_0)
			fail("the renderer exited while starting");
		if (seconds_now() - start > 10.0)
			fail("the renderer did not start in 10 seconds");
	}
	if (host.shared->renderer_state != _spike_renderer_ready)
		fail("the renderer did not start");
}

static void renderer_stop(void)
{
	ring_commit(ring_reserve(_spike_opcode_quit, 0));
	if (WaitForSingleObject(host.renderer, 3000) != WAIT_OBJECT_0)
		fprintf(stderr, "spike_host: the renderer did not exit; the job ends it\n");
	CloseHandle(host.job);
}

/* ---------- the check */

/* the color the screen shows at the window's client centre */
static COLORREF screen_color_at_centre(void)
{
	POINT centre;
	HDC screen;
	COLORREF color;

	centre.x = (LONG)host.width / 2;
	/* below the moving bar the renderer draws at the top */
	centre.y = (LONG)host.height * 3 / 4;
	ClientToScreen(host.window, &centre);
	screen = GetDC(NULL);
	color = GetPixel(screen, centre.x, centre.y);
	ReleaseDC(NULL, screen);
	return color;
}

static int color_close(COLORREF color, int red, int green, int blue)
{
	return abs((int)GetRValue(color) - red) <= 3 && abs((int)GetGValue(color) - green) <= 3 &&
		abs((int)GetBValue(color) - blue) <= 3;
}

static int run_test(unsigned char *payload)
{
	static const float colors[2][3] = { { 0.2f, 0.6f, 0.9f }, { 0.9f, 0.3f, 0.1f } };
	uint32_t frame = 0;
	int failures = 0, pass;
	double start, seconds;

	for (pass = 0; pass < 2; pass++)
	{
		COLORREF color;
		int red = (int)(colors[pass][0] * 255.0f + 0.5f);
		int green = (int)(colors[pass][1] * 255.0f + 0.5f);
		int blue = (int)(colors[pass][2] * 255.0f + 0.5f);

		start = seconds_now();
		while (seconds_now() - start < 1.0)
		{
			window_pump();
			send_frame(frame++, colors[pass][0], colors[pass][1], colors[pass][2]);
		}
		/* the compositor shows a frame after it is presented */
		Sleep(100);
		window_pump();
		color = screen_color_at_centre();
		printf("present %d: wanted %d,%d,%d, the screen shows %d,%d,%d\n", pass, red, green, blue,
			GetRValue(color), GetGValue(color), GetBValue(color));
		if (!color_close(color, red, green, blue))
			failures++;
	}

	start = seconds_now();
	seconds = (double)host.shared->frames_presented;
	while (seconds_now() - start < 2.0)
	{
		window_pump();
		send_frame(frame++, 0.1f, 0.1f, 0.1f);
	}
	printf("frames: %.1f a second presented (Present waits for vsync: this is the display's refresh rate)\n",
		((double)host.shared->frames_presented - seconds) / (seconds_now() - start));

	/* a resize to half size, then back */
	SetWindowPos(host.window, NULL, 0, 0, 660, 400, SWP_NOMOVE | SWP_NOZORDER);
	window_pump();
	if (host.resized)
	{
		host.resized = FALSE;
		send_resize();
	}
	start = seconds_now();
	while (seconds_now() - start < 0.5)
	{
		window_pump();
		send_frame(frame++, colors[0][0], colors[0][1], colors[0][2]);
	}
	Sleep(100);
	if (!color_close(screen_color_at_centre(), 51, 153, 230))
	{
		printf("resize: the screen does not show the frame after the window was resized\n");
		failures++;
	}
	else
	{
		printf("resize: %lux%lu presented\n", host.width, host.height);
	}

	seconds = send_payloads(4, payload);
	printf("ring: 4 payloads of 16 MB in %.1f ms (%.0f MB/s), the last %u us from first byte to GPU copy done, "
		"%u us of it the GPU copy\n", seconds * 1000.0, 64.0 / seconds, host.shared->payload_microseconds,
		host.shared->gpu_copy_microseconds);

	/* input reaches the host's window while the renderer presents into it */
	{
		INPUT inputs[2];

		memset(inputs, 0, sizeof(inputs));
		inputs[0].type = inputs[1].type = INPUT_KEYBOARD;
		inputs[0].ki.wVk = inputs[1].ki.wVk = VK_F13;
		inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
		SetForegroundWindow(host.window);
		SendInput(2, inputs, sizeof(INPUT));
		start = seconds_now();
		while (seconds_now() - start < 0.5)
		{
			window_pump();
			send_frame(frame++, 0.1f, 0.1f, 0.1f);
		}
		if (host.keys)
		{
			printf("input: the window received the key\n");
		}
		else
		{
			printf("input: the window received no key (it may not have been the foreground window)\n");
			failures++;
		}
	}
	return failures;
}

int main(int argc, char **argv)
{
	BOOL test = argc > 1 && !strcmp(argv[1], "--test");
	unsigned char *payload;
	uint32_t frame = 0;
	double title_time;
	uint32_t title_frames;
	int failures = 0;

	QueryPerformanceFrequency(&counter_frequency);
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	payload = malloc(SPIKE_PAYLOAD_MAXIMUM);
	if (!payload)
		fail("out of memory");
	{
		uint32_t index;

		for (index = 0; index < SPIKE_PAYLOAD_MAXIMUM / 4; index++)
			((uint32_t *)payload)[index] = index * 2654435761u;
	}

	window_create(test);
	renderer_start();
	printf("renderer: started on %s\n", host.shared->adapter_name);

	if (test)
	{
		failures = run_test(payload);
		renderer_stop();
		printf(failures ? "FAILED: %d checks\n" : "passed\n", failures);
		return failures ? 1 : 0;
	}

	title_time = seconds_now();
	title_frames = host.shared->frames_presented;
	while (!host.quit)
	{
		double now;
		float phase;

		window_pump();
		if (host.resized)
		{
			host.resized = FALSE;
			send_resize();
		}
		if (host.payload_requested)
		{
			double seconds;

			host.payload_requested = FALSE;
			seconds = send_payloads(4, payload);
			printf("ring: 64 MB in %.1f ms (%.0f MB/s)\n", seconds * 1000.0, 64.0 / seconds);
		}
		phase = (float)(frame % 600) / 600.0f * 6.2831853f;
		send_frame(frame++, 0.5f + 0.4f * sinf(phase), 0.5f + 0.4f * sinf(phase + 2.1f), 0.5f + 0.4f * sinf(phase + 4.2f));

		now = seconds_now();
		if (now - title_time >= 1.0)
		{
			wchar_t title[160];

			_snwprintf(title, sizeof(title) / sizeof(*title),
				L"D3D12 renderer spike - %.0f fps, %lu clicks, %lu keys (T: ring test)",
				(double)(host.shared->frames_presented - title_frames) / (now - title_time), host.clicks, host.keys);
			SetWindowTextW(host.window, title);
			title_time = now;
			title_frames = host.shared->frames_presented;
		}
	}
	renderer_stop();
	return 0;
}
