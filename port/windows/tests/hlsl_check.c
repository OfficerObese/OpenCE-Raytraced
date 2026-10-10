/*
HLSL_CHECK.C

Converts every GLSL program in the folders given (written by the OpenGL
build's debug.gpu_dump_shaders: vs*.glsl and ps_*.glsl) with
xgpu_hlsl_from_glsl and compiles it with D3DCompile, as the D3D12 renderer
will; prints each one that fails, writing its HLSL beside the report, and a
summary. Built and run by tools/test_hlsl_shaders.py.

	hlsl_check.exe <output folder> <corpus folder>...
*/

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xgpu_hlsl.h"

static char *read_file(const char *path)
{
	FILE *file = fopen(path, "rb");
	long size;
	char *data;

	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = malloc((size_t)size + 1);
	if (data && fread(data, 1, (size_t)size, file) != (size_t)size)
	{
		free(data);
		data = NULL;
	}
	if (data)
		data[size] = '\0';
	fclose(file);
	return data;
}

static void write_file(const char *path, const char *text)
{
	FILE *file = fopen(path, "wb");

	if (file)
	{
		fputs(text, file);
		fclose(file);
	}
}

int main(int argc, char **argv)
{
	unsigned long checked = 0, failed = 0, failed_conversion = 0;
	double compile_seconds = 0.0;
	LARGE_INTEGER frequency;
	int folder;

	if (argc < 3)
	{
		fputs("hlsl_check <output folder> <corpus folder>...\n", stderr);
		return 2;
	}
	QueryPerformanceFrequency(&frequency);
	CreateDirectoryA(argv[1], NULL);
	for (folder = 2; folder < argc; folder++)
	{
		char pattern[MAX_PATH];
		WIN32_FIND_DATAA found;
		HANDLE search;

		snprintf(pattern, sizeof(pattern), "%s\\*.glsl", argv[folder]);
		search = FindFirstFileA(pattern, &found);
		if (search == INVALID_HANDLE_VALUE)
		{
			fprintf(stderr, "no GLSL in %s\n", argv[folder]);
			continue;
		}
		do
		{
			char path[MAX_PATH], output[MAX_PATH], error[512];
			int pixel = !strncmp(found.cFileName, "ps_", 3);
			char *glsl, *hlsl;
			ID3DBlob *code = NULL, *errors = NULL;
			LARGE_INTEGER start, end;
			HRESULT result;

			snprintf(path, sizeof(path), "%s\\%s", argv[folder], found.cFileName);
			glsl = read_file(path);
			if (!glsl)
				continue;
			checked++;
			hlsl = xgpu_hlsl_from_glsl(glsl, pixel, NULL, error, sizeof(error));
			free(glsl);
			if (!hlsl)
			{
				failed++;
				failed_conversion++;
				printf("%s: not converted: %s\n", path, error);
				continue;
			}
			QueryPerformanceCounter(&start);
			result = D3DCompile(hlsl, strlen(hlsl), found.cFileName, NULL, NULL, pixel ? "ps_main" : "vs_main",
				pixel ? "ps_5_0" : "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | (pixel ? 0 : D3DCOMPILE_IEEE_STRICTNESS),
				0, &code, &errors);
			QueryPerformanceCounter(&end);
			compile_seconds += (double)(end.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
			/* (HLSL_CHECK_ALL: every program's HLSL written, for reading) */
			if (getenv("HLSL_CHECK_ALL"))
			{
				snprintf(output, sizeof(output), "%s\\%s.hlsl", argv[1], found.cFileName);
				write_file(output, hlsl);
			}
			if (FAILED(result))
			{
				failed++;
				snprintf(output, sizeof(output), "%s\\%s.hlsl", argv[1], found.cFileName);
				write_file(output, hlsl);
				printf("%s: does not compile (%s):\n%s\n", path, output,
					errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "(no message)");
			}
			if (code)
				ID3D10Blob_Release(code);
			if (errors)
				ID3D10Blob_Release(errors);
			free(hlsl);
		} while (FindNextFileA(search, &found));
		FindClose(search);
	}
	printf("%lu programs: %lu failed (%lu not converted); %.1f ms compiling, %.2f ms each\n", checked, failed,
		failed_conversion, compile_seconds * 1000.0, checked ? compile_seconds * 1000.0 / checked : 0.0);
	return failed ? 1 : 0;
}
