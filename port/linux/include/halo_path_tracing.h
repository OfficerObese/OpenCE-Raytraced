/*
HALO_PATH_TRACING.H

display.path_tracing: the game's side of the path tracer (Windows, Direct3D
12; port/windows/README.md). The game's units (port/linux/game/
path_tracing.c) read the scene in the game's terms - the structure BSP, its
shaders and their bitmaps, the camera - and hand it to the platform layer
through these functions, in plain types, which sends it to the renderer
(port/windows/d3d12/xgpu_scene.c). A build without a path tracer has them
do nothing (port/linux/src/d3d8_gl.c), and platform_path_tracing_active
says no, so the game's side does no work.

No lighting the game pre-computed is handed over: not the lightmaps, nor
their texture coordinates, nor the light the game samples from them for
objects.
*/

#ifndef __HALO_PATH_TRACING_H
#define __HALO_PATH_TRACING_H

/* a vertex of the structure BSP, in world space */
struct path_tracing_vertex
{
	float position[3];
	float normal[3];
	float binormal[3];
	float tangent[3];
	float texcoord[2];
};

/* triangles of one material: index_count indices from first_index, of
vertex_count vertices from first_vertex */
struct path_tracing_geometry
{
	unsigned long material;
	/* the structure BSP's surface its first triangle is (the others follow) */
	unsigned long first_surface;
	unsigned long first_vertex;
	unsigned long vertex_count;
	unsigned long first_index;
	unsigned long index_count;
};

enum
{
	_path_tracing_material_environment = 1,
};

enum
{
	/* the bump map's texels whose alpha is below half are holes */
	_path_tracing_material_alpha_tested_bit = 0,
	/* the game draws the scene mirrored in it */
	_path_tracing_material_mirror_bit,
};

/* a shader's surface; textures are from platform_path_tracing_texture */
struct path_tracing_material
{
	unsigned long type;
	unsigned long flags;
	unsigned long base_map;
	unsigned long primary_detail_map;
	unsigned long secondary_detail_map;
	unsigned long micro_detail_map;
	unsigned long bump_map;
	unsigned long detail_function;
	unsigned long micro_detail_function;
	unsigned long environment_type;
	float primary_detail_scale;
	float secondary_detail_scale;
	float micro_detail_scale;
	/* the bump map's texture coordinates' scale, each way */
	float bump_scale[2];
	float emission[3];
	float tint[3];
};

/* ---------- the platform layer's */

/* the setting is on and the renderer can trace rays */
int platform_path_tracing_active(void);
/* a new scene: the textures of the old one are released */
void platform_path_tracing_begin_map(void);
/* the scene's geometry and materials (indices are unsigned shorts, three a
triangle, of their geometry's own vertices) */
void platform_path_tracing_scene(const struct path_tracing_vertex *vertices, unsigned long vertex_count,
	const unsigned short *indices, unsigned long index_count, const struct path_tracing_geometry *geometries,
	unsigned long geometry_count, const struct path_tracing_material *materials, unsigned long material_count,
	unsigned long surface_count);
/* a copy of a loaded texture (a D3DBaseTexture; a palettized one's colours
from palette, 256 of them: the bump maps' vector palette), kept for the
scene; 0 if it cannot be (cube or volume: not yet) */
unsigned long platform_path_tracing_texture(const void *hardware_format, const unsigned long *palette);
/* the camera of the window being drawn: world to clip space, as the
game's vertex shaders take it (rows are clip coordinates), its point, the
window's 3D view in the game's pixels (left, top, right, bottom), and
whether it is drawn to the screen (not a mirror's reflection, drawn into a
texture of its own) */
void platform_path_tracing_camera(const float world_to_clip[4][4], const float position[3],
	const float viewport[4], int primary);
/* one of the game's passes begins or ends (its rasterizer profile) */
void platform_path_tracing_pass(long pass, int begin);
/* the structure BSP's surfaces the game draws in the window being drawn
(for the depth parity view: debug.path_tracing_view) */
void platform_path_tracing_drawn(unsigned long const *surfaces, unsigned long count);

/* ---------- the game's (port/linux/game/path_tracing.c), called by the
game's sources */

struct rasterizer_window_begin_parameters;

/* the window's camera, after rasterizer_window_begin set its frustum; the
scene is sent again when the structure BSP has changed */
void path_tracing_window_begin(struct rasterizer_window_begin_parameters const *parameters);
/* rasterizer_profile_begin and rasterizer_profile_end */
void path_tracing_pass(short profile, int begin);

#endif
