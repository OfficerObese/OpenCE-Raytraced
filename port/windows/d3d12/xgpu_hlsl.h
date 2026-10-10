/*
XGPU_HLSL.H

HLSL for the D3D12 renderer from the GLSL that the NV2A translators write
(port/linux/src/nv2a_vsh.c, nv2a_psh.c), which stay as the OpenGL builds
have them: the GLSL's declarations become the HLSL's constant buffers,
textures and an entry point, and its functions are rewritten token by token
where the two languages differ (xgpu_hlsl.c).

The registers the HLSL uses, which the renderer's root signature gives:
	b0  the vertex constants, c[192]
	b1  struct xgpu_hlsl_draw_uniforms
	b2  the per-pixel model lighting's lights, model_lights[12]
	t0-t3, s0-s3  each texture stage's texture and sampler

C only, with no other header of the port's, so that the renderer and a
checker built apart (tools/test_hlsl_shaders.py) can include it.
*/

#ifndef __XGPU_HLSL_H
#define __XGPU_HLSL_H

#include <stddef.h>

#define XGPU_HLSL_ATTRIBUTE_COUNT 16
#define XGPU_HLSL_CONSTANT_COUNT 192
#define XGPU_HLSL_MODEL_LIGHT_COUNT 12

/* how a vertex attribute reaches the shader, by its declared type (the
input layout's format decides it): float as it is, or with w 1 (formats of
three components read as four); a signed integer converted to float, so
too; packed NORMPACKED3 attributes are 32-bit integers, which the GLSL
already declares as such */
enum
{
	_xgpu_hlsl_input_float = 0,
	_xgpu_hlsl_input_float_w1,
	_xgpu_hlsl_input_sint,
	_xgpu_hlsl_input_sint_w1,
	/* (a packed attribute, which the renderer's input layout reads as an
	integer: the GLSL decides this kind, and the game records it) */
	_xgpu_hlsl_input_uint,
};

/* b1: the uniforms of a draw other than the vertex constants, every member
four floats so that the C and the HLSL layouts agree */
struct xgpu_hlsl_draw_uniforms
{
	float viewport_scale[4];
	float viewport_offset[4];
	float ps_c0[8][4];
	float ps_c1[8][4];
	float ps_final_c0[4];
	float ps_final_c1[4];
	float fog_color[4];
	float fog_parameters[4];
	float bump_matrix[4][4];
	float bump_luminance[4][4];
	float texture_scale[4][4];
	float texture_lod_bias[4];
	/* x the point size, y the menus' column offset (screen_offset), z the
	alpha test's reference */
	float misc[4];
};

_Static_assert(sizeof(struct xgpu_hlsl_draw_uniforms) == 36 * 16, "the draw uniforms are 36 registers");

/* the HLSL for one of the translators' programs (a pixel shader's if pixel),
with the vertex attributes' kinds (NULL: all float); NULL, with the reason in
error, if the GLSL has something the conversion does not know. The entry
points are vs_main and ps_main. Returns a malloc'd string. */
char *xgpu_hlsl_from_glsl(const char *glsl, int pixel, const unsigned char *input_kinds, char *error,
	size_t error_size);

#endif
