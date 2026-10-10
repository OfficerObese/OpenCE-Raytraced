/*
SPIKE_DCOMP.CPP

Presentation option B of the spike: the host's window shows a DirectComposition
surface whose handle the renderer process presents its swap chain into
(IDXGIFactoryMedia::CreateSwapChainForCompositionSurfaceHandle). The window
has no child window of the renderer's, so the two processes' input queues stay
apart. C++ because dcomp.h declares its interfaces for C++ only.
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dcomp.h>
#include <stdio.h>

static IDCompositionDesktopDevice *composition_device;
static IDCompositionTarget *composition_target;
static IDCompositionVisual2 *composition_visual;
static IUnknown *composition_surface;

extern "C" int spike_dcomp_attach(HWND window, HANDLE *surface_handle, char *error, size_t error_size)
{
	HANDLE surface = NULL;
	HRESULT result;

	/* (it refuses security attributes that ask for an inheritable handle) */
	result = DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, NULL, &surface);
	if (FAILED(result))
	{
		snprintf(error, error_size, "DCompositionCreateSurfaceHandle failed (0x%08lX)", (unsigned long)result);
		return 0;
	}
	if (!SetHandleInformation(surface, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
	{
		snprintf(error, error_size, "the composition surface handle cannot be inherited (%lu)", GetLastError());
		return 0;
	}
	result = DCompositionCreateDevice2(NULL, __uuidof(IDCompositionDesktopDevice), (void **)&composition_device);
	if (FAILED(result))
	{
		snprintf(error, error_size, "DCompositionCreateDevice2 failed (0x%08lX)", (unsigned long)result);
		return 0;
	}
	result = composition_device->CreateTargetForHwnd(window, TRUE, &composition_target);
	if (SUCCEEDED(result))
		result = composition_device->CreateVisual(&composition_visual);
	if (SUCCEEDED(result))
		result = composition_device->CreateSurfaceFromHandle(surface, &composition_surface);
	if (SUCCEEDED(result))
		result = composition_visual->SetContent(composition_surface);
	if (SUCCEEDED(result))
		result = composition_target->SetRoot(composition_visual);
	if (SUCCEEDED(result))
		result = composition_device->Commit();
	if (FAILED(result))
	{
		snprintf(error, error_size, "the composition tree could not be made (0x%08lX)", (unsigned long)result);
		return 0;
	}
	*surface_handle = surface;
	return 1;
}
