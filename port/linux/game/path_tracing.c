/*
PATH_TRACING.C

The game's side of display.path_tracing (port/linux/include/
halo_path_tracing.h): the structure BSP's surfaces in world space, their
shaders and those shaders' bitmaps, read when the BSP changes, and each
window's camera, handed to the platform layer, which sends them to the
Direct3D 12 renderer's path tracer (port/windows/d3d12/xgpu_scene.c).

Called from the game's sources: rasterizer_window_begin (the camera, and the
scene when it changed) and rasterizer_profile_begin and _end (the passes).
Without a path tracer (platform_path_tracing_active), nothing is done.

What is sent is the scene as the map defines it, and nothing of the light
the map's tools computed: the lightmaps' bitmaps and texture coordinates
stay behind (the path tracer finds its own light). A shader's radiosity
properties - the light a surface gives off, and the colour of light it lets
through - are what the surface is, and go with it.

So far, the BSP's surfaces whose shaders are shader_environment: the others
(water, glass, the transparent shaders) stay the game's to draw.
*/

#include "cseries.h"
#include "tag_files/tag_groups.h"
#include "scenario/scenario.h"
#include "structures/structure_bsp_definitions.h"
#include "shaders/shader_definitions.h"
#include "shaders/shaders.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmaps.h"
#include "cache/texture_cache.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_geometry_environment.h"

#include "render/render.h"

#include "halo_path_tracing.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* the platform layer's (port/linux/src) */
void platform_log(char const *format, ...);
char const *config_string(char const *name);

/* ---------- the game's layouts this file reads */

/* shaders.c's numbering of shader types */
#define PATH_TRACING_SHADER_TYPE_ENVIRONMENT 3

/* the environment shader (rasterizer_xbox_environment.c keeps its
declaration to itself) */
struct path_tracing_environment_diffuse
{
	word flags;
	short pad02;
	byte reserved04[0x18];
	struct tag_reference base_map;
	byte reserved2C[0x18];
	short detail_map_function;
	short pad46;
	real primary_detail_map_scale;
	struct tag_reference primary_detail_map;
	real secondary_detail_map_scale;
	struct tag_reference secondary_detail_map;
	byte reserved70[0x18];
	short micro_detail_map_function;
	short pad8A;
	real micro_detail_map_scale;
	struct tag_reference micro_detail_map;
	real_rgb_color material_color;
	byte reservedAC[0xC];
	real bump_map_scale;
	struct tag_reference bump_map;
	real_vector2d runtime_bump_map_scale;
	byte reservedD4[0x40];
};

struct path_tracing_environment_self_illumination
{
	word flags;
	short pad02;
	byte reserved04[0x18];
	real_rgb_color primary_on_color;
	real_rgb_color primary_off_color;
	short primary_animation_function;
	short pad36;
	real primary_animation_period;
	real primary_animation_phase;
	byte reserved40[0x18];
	real_rgb_color secondary_on_color;
	real_rgb_color secondary_off_color;
	short secondary_animation_function;
	short pad72;
	real secondary_animation_period;
	real secondary_animation_phase;
	byte reserved7C[0x18];
	real_rgb_color plasma_on_color;
	real_rgb_color plasma_off_color;
	short plasma_animation_function;
	short padAE;
	real plasma_animation_period;
	real plasma_animation_phase;
	byte reservedB8[0x18];
	real map_scale;
	struct tag_reference map;
};

struct path_tracing_environment_specular
{
	word flags;
	short type;
	long unused04[4];
	real brightness;
	long unused18[5];
	real_rgb_color view_perpendicular_color;
	real_rgb_color view_parallel_color;
	long unused44[4];
};

struct path_tracing_environment_reflection
{
	word flags;
	short type;
	real lightmap_brightness_scale;
	long unused1[7];
	real view_perpendicular_brightness;
	real view_parallel_brightness;
	long unused2[4];
	real mirror_index_of_refraction;
	real mirror_depth;
	long unused3[4];
	struct tag_reference cube_map;
	long unused4[4];
};

struct path_tracing_environment_shader
{
	struct shader shader;
	word flags;
	short type;
	real lens_flare_spacing;
	struct tag_reference lens_flare;
	long unused[11];
	struct path_tracing_environment_diffuse diffuse;
	struct path_tracing_environment_self_illumination self_illumination;
	byte reserved23C[0x18];
	struct path_tracing_environment_specular specular;
	struct path_tracing_environment_reflection reflection;
};

typedef char path_tracing_environment_base_map_offset_assert[
	offsetof(struct path_tracing_environment_shader, diffuse.base_map) == 0x88 ? 1 : -1];
typedef char path_tracing_environment_bump_map_offset_assert[
	offsetof(struct path_tracing_environment_shader, diffuse.bump_map) == 0x128 ? 1 : -1];
typedef char path_tracing_environment_size_assert[
	sizeof(struct path_tracing_environment_shader) == 0x344 ? 1 : -1];

/* rasterizer_geometry.c's vertex layouts (it keeps them to itself) */
struct path_tracing_environment_vertex_uncompressed
{
	real_point3d position;
	real_vector3d normal;
	real_vector3d binormal;
	real_vector3d tangent;
	real_point2d texcoord;
};

struct environment_vertex_compressed
{
	real_point3d position;
	unsigned long normal;
	unsigned long binormal;
	unsigned long tangent;
	real_point2d texcoord;
};

typedef char path_tracing_vertex_size_assert[
	sizeof(struct path_tracing_environment_vertex_uncompressed) == 0x38 &&
	sizeof(struct environment_vertex_compressed) == 0x20 &&
	sizeof(struct path_tracing_vertex) == 0x38 ? 1 : -1];

enum
{
	/* the environment shader's flags: alpha tested, and its bump map's
	alpha is a specular mask */
	_path_tracing_environment_alpha_tested_bit = 0,
	_path_tracing_environment_bump_map_is_specular_mask_bit
};

enum
{
	/* the environment shader's reflection is a mirror of the scene */
	_path_tracing_environment_reflection_dynamic_mirror_bit = 0
};

/* the passes the renderer acts on, by the numbers xgpu_protocol.h gives them
(XGPU_PASS_*) */
typedef char path_tracing_pass_numbers_assert[
	_rasterizer_profile_environment_lightmaps == 3 ? 1 : -1];

/* ---------- globals */

/* a bitmap tag's copy for the path tracer */
struct path_tracing_texture
{
	long tag_index;
	unsigned long texture;
};

static struct
{
	/* what the scene sent is of (NONE: none sent); a BSP's tag index is
	another map's too, so its bounds and size are its name */
	long bsp_tag_index;
	real_rectangle3d bsp_bounds;
	long bsp_surface_count;

	struct path_tracing_texture *textures;
	long texture_count;
	long texture_capacity;
} path_tracing = { NONE };

/* ---------- textures */

/* the copy of a bitmap tag's first bitmap, made the first time a shader of
the scene uses it; 0 for none */
static unsigned long path_tracing_texture_get(
	struct tag_reference const *reference)
{
	struct bitmap_data *bitmap;
	void *hardware_format;
	unsigned long texture;
	long index;

	if (reference->index == NONE)
		return 0;
	for (index = 0; index < path_tracing.texture_count; index++)
	{
		if (path_tracing.textures[index].tag_index == reference->index)
			return path_tracing.textures[index].texture;
	}
	/* (loaded now, if it is not, and copied at once: the texture cache can
	reuse its memory for the next) */
	bitmap = bitmap_group_get_bitmap_from_sequence(reference->index, 0, 0);
	hardware_format = bitmap ? _texture_cache_bitmap_get_hardware_format(bitmap, TRUE, TRUE) : NULL;
	/* (a palettized bitmap is a bump map, of the game's one palette of
	vectors, whose alpha the alpha test reads) */
	texture = hardware_format ? platform_path_tracing_texture(hardware_format,
		(const unsigned long *)global_vector_palette) : 0;
	if (path_tracing.texture_count == path_tracing.texture_capacity)
	{
		long capacity = path_tracing.texture_capacity ? path_tracing.texture_capacity * 2 : 256;
		struct path_tracing_texture *grown = realloc(path_tracing.textures, capacity * sizeof(*grown));

		if (!grown)
			return texture;
		path_tracing.textures = grown;
		path_tracing.texture_capacity = capacity;
	}
	path_tracing.textures[path_tracing.texture_count].tag_index = reference->index;
	path_tracing.textures[path_tracing.texture_count].texture = texture;
	path_tracing.texture_count++;
	return texture;
}

/* ---------- the scene */

/* the environment shader of a BSP material, NULL if it has another */
static struct path_tracing_environment_shader const *path_tracing_material_shader(
	struct structure_material const *material)
{
	struct shader const *shader;

	if (material->shader.index == NONE)
		return NULL;
	shader = tag_get(material->shader.group_tag, material->shader.index);
	if (!shader || shader->base.type != PATH_TRACING_SHADER_TYPE_ENVIRONMENT)
		return NULL;
	return (struct path_tracing_environment_shader const *)shader;
}

/* a material's vertices, into vertices (its count of them); FALSE if it
has none the scene can use */
static boolean path_tracing_material_vertices(
	struct structure_material const *material,
	struct path_tracing_vertex *vertices)
{
	long count = material->vertices.count;
	long index;

	if (material->uncompressed_vertex_data.address &&
		material->uncompressed_vertex_data.size >= count * (long)sizeof(struct path_tracing_environment_vertex_uncompressed))
	{
		/* (the same layout: positions, the three vectors, coordinates) */
		if (vertices)
			memcpy(vertices, material->uncompressed_vertex_data.address, count * sizeof(*vertices));
		return TRUE;
	}
	if (material->compressed_vertex_data.address &&
		material->compressed_vertex_data.size >= count * (long)sizeof(struct environment_vertex_compressed))
	{
		struct environment_vertex_compressed const *compressed = material->compressed_vertex_data.address;

		for (index = 0; vertices && index < count; index++)
		{
			union real_point3d point;
			union real_vector3d vector;
			union real_point2d texcoord;

			environment_vertex_compressed_get_point(&compressed[index], &point);
			vertices[index].position[0] = point.x;
			vertices[index].position[1] = point.y;
			vertices[index].position[2] = point.z;
			environment_vertex_compressed_get_normal(&compressed[index], &vector);
			vertices[index].normal[0] = vector.i;
			vertices[index].normal[1] = vector.j;
			vertices[index].normal[2] = vector.k;
			vector = uncompress_int32_to_real_vector3d(compressed[index].binormal);
			vertices[index].binormal[0] = vector.i;
			vertices[index].binormal[1] = vector.j;
			vertices[index].binormal[2] = vector.k;
			vector = uncompress_int32_to_real_vector3d(compressed[index].tangent);
			vertices[index].tangent[0] = vector.i;
			vertices[index].tangent[1] = vector.j;
			vertices[index].tangent[2] = vector.k;
			environment_vertex_compressed_get_texcoord(&compressed[index], &texcoord);
			vertices[index].texcoord[0] = texcoord.x;
			vertices[index].texcoord[1] = texcoord.y;
		}
		return TRUE;
	}
	return FALSE;
}

static void path_tracing_material_make(
	struct path_tracing_environment_shader const *shader,
	struct path_tracing_material *material)
{
	struct shader_radiosity_properties const *radiosity = &shader->shader.base.radiosity;

	memset(material, 0, sizeof(*material));
	material->type = _path_tracing_material_environment;
	/* (the game tests the bump map's alpha, which a shader whose bump map's
	alpha is a specular mask has none of: rasterizer_xbox_environment.c's
	lightmap pass) */
	if (TEST_FLAG(shader->flags, _path_tracing_environment_alpha_tested_bit) &&
		!TEST_FLAG(shader->flags, _path_tracing_environment_bump_map_is_specular_mask_bit) &&
		shader->diffuse.bump_map.index != NONE)
	{
		SET_FLAG(material->flags, _path_tracing_material_alpha_tested_bit, TRUE);
	}
	if (TEST_FLAG(shader->reflection.flags, _path_tracing_environment_reflection_dynamic_mirror_bit))
		SET_FLAG(material->flags, _path_tracing_material_mirror_bit, TRUE);
	material->environment_type = shader->type;
	material->base_map = path_tracing_texture_get(&shader->diffuse.base_map);
	material->primary_detail_map = path_tracing_texture_get(&shader->diffuse.primary_detail_map);
	material->secondary_detail_map = path_tracing_texture_get(&shader->diffuse.secondary_detail_map);
	material->micro_detail_map = path_tracing_texture_get(&shader->diffuse.micro_detail_map);
	material->bump_map = path_tracing_texture_get(&shader->diffuse.bump_map);
	material->detail_function = shader->diffuse.detail_map_function;
	material->micro_detail_function = shader->diffuse.micro_detail_map_function;
	material->primary_detail_scale = shader->diffuse.primary_detail_map_scale;
	material->secondary_detail_scale = shader->diffuse.secondary_detail_map_scale;
	material->micro_detail_scale = shader->diffuse.micro_detail_map_scale;
	material->bump_scale[0] = shader->diffuse.runtime_bump_map_scale.i;
	material->bump_scale[1] = shader->diffuse.runtime_bump_map_scale.j;
	material->emission[0] = radiosity->power * radiosity->color_of_emitted_light.red;
	material->emission[1] = radiosity->power * radiosity->color_of_emitted_light.green;
	material->emission[2] = radiosity->power * radiosity->color_of_emitted_light.blue;
	material->tint[0] = radiosity->tint_color.red;
	material->tint[1] = radiosity->tint_color.green;
	material->tint[2] = radiosity->tint_color.blue;
}

/* calls proc for each of the BSP's materials the scene takes */
typedef void (*path_tracing_material_proc)(void *context, struct structure_bsp *bsp,
	struct structure_material const *material, struct path_tracing_environment_shader const *shader);

static void path_tracing_materials_iterate(
	struct structure_bsp *bsp,
	path_tracing_material_proc proc,
	void *context)
{
	long lightmap_index;

	for (lightmap_index = 0; lightmap_index < bsp->lightmaps.count; lightmap_index++)
	{
		struct structure_lightmap const *lightmap =
			TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, lightmap_index, struct structure_lightmap);
		long material_index;

		for (material_index = 0; material_index < lightmap->materials.count; material_index++)
		{
			struct structure_material const *material =
				TAG_BLOCK_GET_ELEMENT(&lightmap->materials, material_index, struct structure_material);
			struct path_tracing_environment_shader const *shader = path_tracing_material_shader(material);

			/* (a map is not trusted: its surfaces must be the BSP's) */
			if (shader && material->vertices.count > 0 && material->vertices.count <= 0x10000 &&
				material->surface_count > 0 && material->first_surface_index >= 0 &&
				material->first_surface_index <= bsp->surfaces.count - material->surface_count &&
				path_tracing_material_vertices(material, NULL))
			{
				proc(context, bsp, material, shader);
			}
		}
	}
}

struct path_tracing_scene_build
{
	struct path_tracing_vertex *vertices;
	unsigned short *indices;
	struct path_tracing_geometry *geometries;
	struct path_tracing_material *materials;
	unsigned long vertex_count;
	unsigned long index_count;
	unsigned long geometry_count;
};

static void path_tracing_scene_count(
	void *context,
	struct structure_bsp *bsp,
	struct structure_material const *material,
	struct path_tracing_environment_shader const *shader)
{
	struct path_tracing_scene_build *build = context;

	build->vertex_count += material->vertices.count;
	build->index_count += material->surface_count * NUMBER_OF_VERTICES_PER_TRIANGLE;
	build->geometry_count++;
}

static void path_tracing_scene_fill(
	void *context,
	struct structure_bsp *bsp,
	struct structure_material const *material,
	struct path_tracing_environment_shader const *shader)
{
	struct path_tracing_scene_build *build = context;
	struct path_tracing_geometry *geometry = &build->geometries[build->geometry_count];
	long surface_index;

	geometry->material = build->geometry_count;
	geometry->first_surface = material->first_surface_index;
	geometry->first_vertex = build->vertex_count;
	geometry->vertex_count = material->vertices.count;
	geometry->first_index = build->index_count;
	path_tracing_material_vertices(material, &build->vertices[build->vertex_count]);
	for (surface_index = 0; surface_index < material->surface_count; surface_index++)
	{
		struct structure_surface const *surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces,
			material->first_surface_index + surface_index, struct structure_surface);
		short vertex;

		/* (a triangle of vertices the material does not have is left out) */
		for (vertex = 0; vertex < NUMBER_OF_VERTICES_PER_TRIANGLE; vertex++)
		{
			if (surface->vertex_indices[vertex] >= material->vertices.count)
				break;
		}
		if (vertex < NUMBER_OF_VERTICES_PER_TRIANGLE)
			continue;
		for (vertex = 0; vertex < NUMBER_OF_VERTICES_PER_TRIANGLE; vertex++)
			build->indices[build->index_count++] = surface->vertex_indices[vertex];
	}
	geometry->index_count = build->index_count - geometry->first_index;
	path_tracing_material_make(shader, &build->materials[build->geometry_count]);
	/* (the depth parity view's log names the materials by these numbers) */
	if (!strcmp(config_string("debug.path_tracing_view"), "depth_parity"))
	{
		platform_log("path tracing: material %lu: %s%s", build->geometry_count,
			material->shader.name ? material->shader.name : "(unnamed)",
			TEST_FLAG(build->materials[build->geometry_count].flags, _path_tracing_material_alpha_tested_bit) ?
			" (alpha tested)" : "");
	}
	build->vertex_count += material->vertices.count;
	build->geometry_count++;
}

static void path_tracing_scene_send(
	struct structure_bsp *bsp)
{
	struct path_tracing_scene_build build;
	unsigned long vertex_total, index_total, geometry_total;

	platform_path_tracing_begin_map();
	path_tracing.texture_count = 0;
	memset(&build, 0, sizeof(build));
	path_tracing_materials_iterate(bsp, path_tracing_scene_count, &build);
	vertex_total = build.vertex_count;
	index_total = build.index_count;
	geometry_total = build.geometry_count;
	build.vertices = malloc((vertex_total ? vertex_total : 1) * sizeof(*build.vertices));
	build.indices = malloc((index_total ? index_total : 1) * sizeof(*build.indices));
	build.geometries = malloc((geometry_total ? geometry_total : 1) * sizeof(*build.geometries));
	build.materials = malloc((geometry_total ? geometry_total : 1) * sizeof(*build.materials));
	if (build.vertices && build.indices && build.geometries && build.materials)
	{
		build.vertex_count = build.index_count = build.geometry_count = 0;
		path_tracing_materials_iterate(bsp, path_tracing_scene_fill, &build);
		platform_path_tracing_scene(build.vertices, build.vertex_count, build.indices, build.index_count,
			build.geometries, build.geometry_count, build.materials, build.geometry_count, bsp->surfaces.count);
		platform_log("path tracing: the structure BSP: %lu vertices, %lu triangles, %lu materials, %ld textures",
			build.vertex_count, build.index_count / NUMBER_OF_VERTICES_PER_TRIANGLE, build.geometry_count,
			path_tracing.texture_count);
	}
	else
	{
		platform_log("path tracing: no memory for the structure BSP's %lu vertices", vertex_total);
	}
	free(build.vertices);
	free(build.indices);
	free(build.geometries);
	free(build.materials);
}

/* ---------- the game's */

void path_tracing_window_begin(
	struct rasterizer_window_begin_parameters const *parameters)
{
	struct structure_bsp *bsp;
	long bsp_tag_index;
	float world_to_clip[4][4];
	float position[3];
	float viewport[4];
	short column;
	short row;

	if (!platform_path_tracing_active())
	{
		/* (sent again when it is turned on) */
		path_tracing.bsp_tag_index = NONE;
		return;
	}

	/* (no structure BSP while a map loads) */
	bsp = NULL;
	bsp_tag_index = NONE;
	if (global_scenario_try_and_get() && global_structure_bsp_index != NONE)
	{
		bsp = global_structure_bsp_get();
		bsp_tag_index = global_structure_bsp_tag_index_get();
	}
	if (bsp && bsp_tag_index != NONE &&
		(bsp_tag_index != path_tracing.bsp_tag_index || bsp->surfaces.count != path_tracing.bsp_surface_count ||
		memcmp(&bsp->world_bounds, &path_tracing.bsp_bounds, sizeof(bsp->world_bounds))))
	{
		path_tracing.bsp_tag_index = bsp_tag_index;
		path_tracing.bsp_surface_count = bsp->surfaces.count;
		path_tracing.bsp_bounds = bsp->world_bounds;
		path_tracing_scene_send(bsp);
	}

	/* the vertex shaders' world to clip transform, as
	rasterizer_set_frustum_z makes it */
	for (column = 0; column < 4; column++)
	{
		for (row = 0; row < 4; row++)
		{
			const real *n = parameters->frustum.world_to_view.n[row];

			world_to_clip[column][row] =
				n[0] * parameters->frustum.projection_matrix[0][column] +
				n[1] * parameters->frustum.projection_matrix[1][column] +
				n[2] * parameters->frustum.projection_matrix[2][column];
		}
		world_to_clip[column][3] += parameters->frustum.projection_matrix[3][column];
	}
	position[0] = parameters->camera.position.x;
	position[1] = parameters->camera.position.y;
	position[2] = parameters->camera.position.z;
	viewport[0] = parameters->camera.viewport_bounds.x0;
	viewport[1] = parameters->camera.viewport_bounds.y0;
	viewport[2] = parameters->camera.viewport_bounds.x1;
	viewport[3] = parameters->camera.viewport_bounds.y1;
	/* (the primary render target is 0, as rasterizer_xbox.c numbers them) */
	platform_path_tracing_camera(world_to_clip, position, viewport, parameters->rasterizer_target == 0);

	/* (the depth parity view judges only the surfaces the game draws: its
	visibility leaves out some that rays reach) */
	if (bsp && !strcmp(config_string("debug.path_tracing_view"), "depth_parity"))
	{
		platform_path_tracing_drawn((unsigned long const *)render.environment_surface_indices,
			render.environment_surface_count);
	}
}

void path_tracing_pass(
	short profile,
	int begin)
{
	if (platform_path_tracing_active())
		platform_path_tracing_pass(profile, begin);
}
