/*
WIN32_RENDERER_HOST.C

Starts the D3D12 renderer process (halo_renderer.exe, beside halo.exe) and
keeps the handles the game and it share (xgpu_host.h): the section of
xgpu_protocol.h, an event for each direction of the ring, the semaphore that
paces frames, and the DirectComposition surface the game's window shows and
the renderer presents into (win32_dcomp.cpp). The renderer runs in a job
object that ends it when the game ends, however the game ends.

As the M0 spike measured (port/windows/spike/README.md): the window keeps
its own input, there being no window of the renderer's in it.
*/

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "xgpu_host.h"

int xgpu_dcomp_attach(void *window, void **surface_handle, char *error, size_t error_size);

static struct
{
	HANDLE section;
	struct xgpu_header *header;
	HANDLE data_event, space_event, frame_semaphore;
	HANDLE job;
	HANDLE process;
	void *surface;
} host;

struct xgpu_header *xgpu_host_header(void)
{
	return host.header;
}

struct xgpu_results *xgpu_host_results(void)
{
	return host.header ? (struct xgpu_results *)((unsigned char *)host.header + XGPU_HEADER_SIZE) : NULL;
}

unsigned char *xgpu_host_ring(void)
{
	return host.header ? (unsigned char *)host.header + XGPU_HEADER_SIZE + XGPU_RESULTS_SIZE : NULL;
}

static HANDLE inheritable_event(void)
{
	SECURITY_ATTRIBUTES inherited = { sizeof(inherited), NULL, TRUE };

	return CreateEventW(&inherited, FALSE, FALSE, NULL);
}

static int start_failed(char *error, size_t error_size, const char *what)
{
	snprintf(error, error_size, "%s (%lu)", what, GetLastError());
	return 0;
}

int xgpu_host_start(void *window, unsigned long width, unsigned long height, char *error, size_t error_size)
{
	SECURITY_ATTRIBUTES inherited = { sizeof(inherited), NULL, TRUE };
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
	STARTUPINFOW startup;
	PROCESS_INFORMATION process;
	wchar_t path[MAX_PATH], command_line[600], *slash;
	ULONGLONG started;

	host.section = CreateFileMappingW(INVALID_HANDLE_VALUE, &inherited, PAGE_READWRITE, 0, XGPU_SECTION_SIZE, NULL);
	if (!host.section)
		return start_failed(error, error_size, "the renderer's shared memory cannot be made");
	host.header = MapViewOfFile(host.section, FILE_MAP_ALL_ACCESS, 0, 0, XGPU_SECTION_SIZE);
	if (!host.header)
		return start_failed(error, error_size, "the renderer's shared memory cannot be mapped");
	host.header->version = XGPU_PROTOCOL_VERSION;
	host.header->ring_size = XGPU_RING_SIZE;
	host.data_event = inheritable_event();
	host.space_event = inheritable_event();
	host.frame_semaphore = CreateSemaphoreW(&inherited, XGPU_FRAMES_IN_FLIGHT, XGPU_FRAMES_IN_FLIGHT, NULL);
	if (!host.data_event || !host.space_event || !host.frame_semaphore)
		return start_failed(error, error_size, "the renderer's events cannot be made");
	if (!xgpu_dcomp_attach(window, &host.surface, error, error_size))
		return 0;

	/* the renderer ends with the game */
	host.job = CreateJobObjectW(NULL, NULL);
	memset(&limits, 0, sizeof(limits));
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!host.job || !SetInformationJobObject(host.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
		return start_failed(error, error_size, "the renderer's job object cannot be made");

	GetModuleFileNameW(NULL, path, MAX_PATH);
	slash = wcsrchr(path, L'\\');
	wcscpy(slash ? slash + 1 : path, L"halo_renderer.exe");
	_snwprintf(command_line, sizeof(command_line) / sizeof(*command_line),
		L"\"%ls\" %lx %lx %lx %lx %lx %lu %lu", path,
		(unsigned long)(ULONG_PTR)host.section, (unsigned long)(ULONG_PTR)host.data_event,
		(unsigned long)(ULONG_PTR)host.space_event, (unsigned long)(ULONG_PTR)host.frame_semaphore,
		(unsigned long)(ULONG_PTR)host.surface, width ? width : 1, height ? height : 1);
	memset(&startup, 0, sizeof(startup));
	startup.cb = sizeof(startup);
	if (!CreateProcessW(path, command_line, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &startup, &process))
		return start_failed(error, error_size, "halo_renderer.exe (beside halo.exe) cannot be started");
	if (!AssignProcessToJobObject(host.job, process.hProcess))
	{
		TerminateProcess(process.hProcess, 1);
		return start_failed(error, error_size, "the renderer cannot be put in its job object");
	}
	ResumeThread(process.hThread);
	CloseHandle(process.hThread);
	host.process = process.hProcess;

	started = GetTickCount64();
	while (host.header->renderer_state == _xgpu_renderer_starting)
	{
		if (WaitForSingleObject(host.process, 10) == WAIT_OBJECT_0)
			break;
		if (GetTickCount64() - started > 30000)
		{
			snprintf(error, error_size, "the renderer did not start in 30 seconds");
			return 0;
		}
	}
	if (host.header->renderer_state != _xgpu_renderer_ready)
	{
		snprintf(error, error_size, "the renderer did not start: %s",
			host.header->error[0] ? host.header->error : "it exited");
		return 0;
	}
	return 1;
}

int xgpu_host_wait_space(unsigned long milliseconds)
{
	HANDLE handles[2];

	handles[0] = host.space_event;
	handles[1] = host.process;
	return WaitForMultipleObjects(2, handles, FALSE, milliseconds) != WAIT_OBJECT_0 + 1;
}

void xgpu_host_signal_data(void)
{
	SetEvent(host.data_event);
}

int xgpu_host_wait_frame(void)
{
	HANDLE handles[2];
	DWORD result;

	handles[0] = host.frame_semaphore;
	handles[1] = host.process;
	result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
	return result == WAIT_OBJECT_0 && host.header->renderer_state != _xgpu_renderer_failed;
}

long xgpu_host_exit_code(void)
{
	DWORD code;

	if (!host.process || WaitForSingleObject(host.process, 0) != WAIT_OBJECT_0)
		return -1;
	if (!GetExitCodeProcess(host.process, &code))
		return 1;
	return (long)code;
}

void xgpu_host_stop(unsigned long milliseconds)
{
	if (!host.process)
		return;
	SetEvent(host.data_event);
	WaitForSingleObject(host.process, milliseconds);
}
