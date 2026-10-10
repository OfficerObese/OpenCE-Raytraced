/*
XGPU_SCENE.C

display.path_tracing, the platform layer's side (halo_path_tracing.h): the
scene the game's side hands over (port/linux/game/path_tracing.c), sent to
the renderer's path tracer (renderer_scene.c) as the scene messages of
xgpu_protocol.h, in pieces of at most XGPU_MESSAGE_MAXIMUM bytes; the
copies of the scene's textures, which this file owns until the next scene;
the windows' cameras and the game's passes.

Path tracing is active while the setting is on and the renderer said the
GPU can trace rays (its header's path_tracing_available); d3d8_d3d12.c
tells this file the setting each frame.
*/

#include "xgpu.h"
#include "port_config.h"
#include "halo_path_tracing.h"
#include "xgpu_host.h"
#include "xgpu_ring.h"
#include "xgpu_scene.h"

#include <stdlib.h>
#include <string.h>

static struct
{
	int active;
	/* the scene's texture copies (xgpu_texture_copy), deleted with it */
	GLuint *textures;
	unsigned long texture_count;
	unsigned long texture_capacity;
} scene;

void xgpu_scene_setting(int on)
{
	struct xgpu_header *header = xgpu_host_header();

	scene.active = on && header && header->path_tracing_available;
}

int platform_path_tracing_active(void)
{
	return scene.active;
}

void platform_path_tracing_begin_map(void)
{
	if (scene.texture_count)
		glDeleteTextures((GLsizei)scene.texture_count, scene.textures);
	scene.texture_count = 0;
}

unsigned long platform_path_tracing_texture(const void *hardware_format, const unsigned long *palette)
{
	GLenum target;
	GLuint texture;

	if (!scene.active || !hardware_format)
		return 0;
	if (scene.texture_count == scene.texture_capacity)
	{
		unsigned long capacity = scene.texture_capacity ? scene.texture_capacity * 2 : 256;
		GLuint *grown = realloc(scene.textures, capacity * sizeof(*grown));

		if (!grown)
			return 0;
		scene.textures = grown;
		scene.texture_capacity = capacity;
	}
	texture = xgpu_texture_copy((const DWORD *)hardware_format, (const D3DCOLOR *)palette, &target);
	if (!texture)
		return 0;
	scene.textures[scene.texture_count++] = texture;
	/* (only 2D textures are sampled so far) */
	return target == GL_TEXTURE_2D ? texture : 0;
}

/* records of a piece's kind, sent in messages that fit */
static void pieces_send(uint32_t opcode, uint32_t kind, const void *records, unsigned long count,
	unsigned long record_size)
{
	unsigned long per_message = XGPU_MESSAGE_MAXIMUM / record_size, first;

	for (first = 0; first < count; first += per_message)
	{
		struct xgpu_scene_piece piece;
		unsigned long this_count = count - first < per_message ? count - first : per_message;

		memset(&piece, 0, sizeof(piece));
		piece.kind = kind;
		piece.first = (uint32_t)first;
		piece.count = (uint32_t)this_count;
		xgpu_ring_send_data(opcode, &piece, sizeof(piece), (const unsigned char *)records + first * record_size,
			this_count * record_size);
	}
}

void platform_path_tracing_scene(const struct path_tracing_vertex *vertices, unsigned long vertex_count,
	const unsigned short *indices, unsigned long index_count, const struct path_tracing_geometry *geometries,
	unsigned long geometry_count, const struct path_tracing_material *materials, unsigned long material_count,
	unsigned long surface_count)
{
	struct xgpu_scene_begin_map begin;
	struct xgpu_scene_geometry *sent_geometries;
	struct xgpu_scene_material *sent_materials;
	uint32_t *sent_indices;
	unsigned long index;

	if (!scene.active)
		return;
	if (vertex_count > XGPU_SCENE_VERTEX_LIMIT || index_count > XGPU_SCENE_INDEX_LIMIT ||
		geometry_count > XGPU_SCENE_GEOMETRY_LIMIT || material_count > XGPU_SCENE_MATERIAL_LIMIT)
	{
		platform_log("path tracing: a structure BSP of %lu vertices and %lu triangles is too large to trace",
			vertex_count, index_count / 3);
		vertex_count = index_count = geometry_count = material_count = 0;
	}
	sent_indices = malloc((index_count ? index_count : 1) * sizeof(*sent_indices));
	sent_geometries = calloc(geometry_count ? geometry_count : 1, sizeof(*sent_geometries));
	sent_materials = calloc(material_count ? material_count : 1, sizeof(*sent_materials));
	if (!sent_indices || !sent_geometries || !sent_materials)
	{
		platform_log("path tracing: no memory to send the scene");
		vertex_count = index_count = geometry_count = material_count = 0;
	}
	for (index = 0; index < index_count; index++)
		sent_indices[index] = indices[index];
	for (index = 0; index < geometry_count; index++)
	{
		sent_geometries[index].material = (uint32_t)geometries[index].material;
		sent_geometries[index].first_surface = (uint32_t)geometries[index].first_surface;
		sent_geometries[index].first_vertex = (uint32_t)geometries[index].first_vertex;
		sent_geometries[index].vertex_count = (uint32_t)geometries[index].vertex_count;
		sent_geometries[index].first_index = (uint32_t)geometries[index].first_index;
		sent_geometries[index].index_count = (uint32_t)geometries[index].index_count;
	}
	for (index = 0; index < material_count; index++)
	{
		const struct path_tracing_material *from = &materials[index];
		struct xgpu_scene_material *to = &sent_materials[index];

		to->type = (uint32_t)from->type;
		to->flags = (uint32_t)from->flags;
		to->base_map = (uint32_t)from->base_map;
		to->primary_detail_map = (uint32_t)from->primary_detail_map;
		to->secondary_detail_map = (uint32_t)from->secondary_detail_map;
		to->micro_detail_map = (uint32_t)from->micro_detail_map;
		to->bump_map = (uint32_t)from->bump_map;
		to->detail_function = (uint32_t)from->detail_function;
		to->micro_detail_function = (uint32_t)from->micro_detail_function;
		to->environment_type = (uint32_t)from->environment_type;
		to->primary_detail_scale = from->primary_detail_scale;
		to->secondary_detail_scale = from->secondary_detail_scale;
		to->micro_detail_scale = from->micro_detail_scale;
		to->bump_scale[0] = from->bump_scale[0];
		to->bump_scale[1] = from->bump_scale[1];
		memcpy(to->emission, from->emission, sizeof(to->emission));
		memcpy(to->tint, from->tint, sizeof(to->tint));
	}

	memset(&begin, 0, sizeof(begin));
	begin.vertex_count = (uint32_t)vertex_count;
	begin.index_count = (uint32_t)index_count;
	begin.geometry_count = (uint32_t)geometry_count;
	begin.material_count = (uint32_t)material_count;
	begin.surface_count = (uint32_t)(surface_count < XGPU_SCENE_SURFACE_LIMIT ? surface_count : 0);
	xgpu_ring_send_data(_xgpu_op_scene_begin_map, &begin, sizeof(begin), NULL, 0);
	/* (struct path_tracing_vertex is struct xgpu_scene_vertex's layout) */
	pieces_send(_xgpu_op_scene_mesh, _xgpu_scene_piece_vertices, vertices, vertex_count,
		sizeof(struct xgpu_scene_vertex));
	pieces_send(_xgpu_op_scene_mesh, _xgpu_scene_piece_indices, sent_indices, index_count, sizeof(uint32_t));
	pieces_send(_xgpu_op_scene_mesh, _xgpu_scene_piece_geometries, sent_geometries, geometry_count,
		sizeof(struct xgpu_scene_geometry));
	pieces_send(_xgpu_op_scene_materials, _xgpu_scene_piece_materials, sent_materials, material_count,
		sizeof(struct xgpu_scene_material));
	free(sent_indices);
	free(sent_geometries);
	free(sent_materials);
}

void platform_path_tracing_camera(const float world_to_clip[4][4], const float position[3],
	const float viewport[4], int primary)
{
	struct xgpu_scene_camera camera;

	if (!scene.active)
		return;
	memset(&camera, 0, sizeof(camera));
	memcpy(camera.world_to_clip, world_to_clip, sizeof(camera.world_to_clip));
	memcpy(camera.position, position, sizeof(camera.position));
	memcpy(camera.game_viewport, viewport, sizeof(camera.game_viewport));
	camera.primary = primary ? 1 : 0;
	xgpu_ring_send_data(_xgpu_op_scene_camera, &camera, sizeof(camera), NULL, 0);
}

static void pass_send(uint32_t pass, int begin)
{
	struct xgpu_pass message;

	message.pass = pass;
	message.begin = begin ? 1 : 0;
	xgpu_ring_send_data(_xgpu_op_pass, &message, sizeof(message), NULL, 0);
}

void platform_path_tracing_pass(long pass, int begin)
{
	if (scene.active && pass >= 0 && pass < XGPU_PASS_VIEW_END)
		pass_send((uint32_t)pass, begin);
}

void platform_path_tracing_drawn(unsigned long const *surfaces, unsigned long count)
{
	struct xgpu_scene_drawn drawn;

	if (!scene.active)
		return;
	if (count > XGPU_MESSAGE_MAXIMUM / sizeof(uint32_t))
		count = XGPU_MESSAGE_MAXIMUM / sizeof(uint32_t);
	memset(&drawn, 0, sizeof(drawn));
	drawn.count = (uint32_t)count;
	/* (a long is 32 bits here: the indices go as they are) */
	xgpu_ring_send_data(_xgpu_op_scene_drawn, &drawn, sizeof(drawn), surfaces, count * sizeof(uint32_t));
}

void xgpu_scene_view_end(void)
{
	if (scene.active)
		pass_send(XGPU_PASS_VIEW_END, 1);
}
