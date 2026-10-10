/*
RENDERER_SCENE.C

The path tracer's scene (display.path_tracing; renderer_path_tracer.c): the
structure BSP's surfaces, in world space, and their materials, as the game
sends them when its BSP changes (the scene messages of xgpu_protocol.h,
port/windows/d3d12/xgpu_scene.c), and each window's camera.

The scene is kept here as it came, so that a new device can have it again;
the GPU's copies - the vertices, the indices, the table of geometries and
the acceleration structures, a bottom level of the BSP's triangles (one
geometry for each of its materials) in a top level of one instance - are
made from it when it is first traced. Every record is checked against what
was sent before it is used: map data is not trusted.
*/

#include "renderer.h"

#include <stdlib.h>
#include <string.h>

/* a geometry as the trace reads it (shaders/pt_common.hlsli): the GPU's
geometry index is its place in this table, of the geometries traced */
struct gpu_geometry
{
	uint32_t first_vertex;
	uint32_t first_index;
	uint32_t material;
	uint32_t first_surface;
};

static struct
{
	/* as sent */
	struct xgpu_scene_vertex *vertices;
	uint32_t *indices;
	struct xgpu_scene_geometry *geometries;
	struct xgpu_scene_material *materials;
	struct xgpu_scene_begin_map counts;
	/* the GPU's copies are to be made */
	BOOL dirty;

	ID3D12Resource *vertex_buffer, *index_buffer, *geometry_buffer;
	ID3D12Resource *bottom, *top;
	uint32_t traced_geometry_count;
	/* each traced geometry's material */
	uint32_t *traced_materials;

	struct xgpu_scene_camera camera;
	BOOL have_camera;

	/* the surfaces the game draws, a bit each */
	uint32_t *drawn;
} scene;

static void gpu_release(void)
{
	renderer_release_later((IUnknown *)scene.vertex_buffer);
	renderer_release_later((IUnknown *)scene.index_buffer);
	renderer_release_later((IUnknown *)scene.geometry_buffer);
	renderer_release_later((IUnknown *)scene.bottom);
	renderer_release_later((IUnknown *)scene.top);
	scene.vertex_buffer = scene.index_buffer = scene.geometry_buffer = NULL;
	scene.bottom = scene.top = NULL;
	scene.traced_geometry_count = 0;
}

/* ---------- messages */

void renderer_scene_begin_map(const struct xgpu_scene_begin_map *begin)
{
	if (begin->vertex_count > XGPU_SCENE_VERTEX_LIMIT || begin->index_count > XGPU_SCENE_INDEX_LIMIT ||
		begin->geometry_count > XGPU_SCENE_GEOMETRY_LIMIT || begin->material_count > XGPU_SCENE_MATERIAL_LIMIT)
	{
		renderer_fail("a scene of %u vertices, %u indices, %u geometries, %u materials", begin->vertex_count,
			begin->index_count, begin->geometry_count, begin->material_count);
	}
	gpu_release();
	free(scene.vertices);
	free(scene.indices);
	free(scene.geometries);
	free(scene.materials);
	scene.counts = *begin;
	/* (zeroed: what a message never fills is nothing) */
	scene.vertices = calloc(begin->vertex_count ? begin->vertex_count : 1, sizeof(*scene.vertices));
	scene.indices = calloc(begin->index_count ? begin->index_count : 1, sizeof(*scene.indices));
	scene.geometries = calloc(begin->geometry_count ? begin->geometry_count : 1, sizeof(*scene.geometries));
	scene.materials = calloc(begin->material_count ? begin->material_count : 1, sizeof(*scene.materials));
	if (begin->surface_count > XGPU_SCENE_SURFACE_LIMIT)
		renderer_fail("a scene of %u surfaces", begin->surface_count);
	free(scene.drawn);
	scene.drawn = calloc(begin->surface_count / 32 + 1, sizeof(*scene.drawn));
	if (!scene.vertices || !scene.indices || !scene.geometries || !scene.materials || !scene.drawn)
		renderer_fail("out of memory for a scene of %u vertices", begin->vertex_count);
	scene.dirty = TRUE;
}

void renderer_scene_piece(const struct xgpu_scene_piece *piece, const uint8_t *data, uint32_t size)
{
	void *records;
	uint32_t total, record_size;

	switch (piece->kind)
	{
	case _xgpu_scene_piece_vertices:
		records = scene.vertices;
		total = scene.counts.vertex_count;
		record_size = sizeof(struct xgpu_scene_vertex);
		break;
	case _xgpu_scene_piece_indices:
		records = scene.indices;
		total = scene.counts.index_count;
		record_size = sizeof(uint32_t);
		break;
	case _xgpu_scene_piece_geometries:
		records = scene.geometries;
		total = scene.counts.geometry_count;
		record_size = sizeof(struct xgpu_scene_geometry);
		break;
	case _xgpu_scene_piece_materials:
		records = scene.materials;
		total = scene.counts.material_count;
		record_size = sizeof(struct xgpu_scene_material);
		break;
	default:
		renderer_fail("a scene piece of kind %u", piece->kind);
	}
	if (!records || piece->first > total || piece->count > total - piece->first ||
		(uint64_t)piece->count * record_size != size)
	{
		renderer_fail("a scene piece of kind %u: %u records from %u, of %u, in %u bytes", piece->kind, piece->count,
			piece->first, total, size);
	}
	memcpy((uint8_t *)records + (size_t)piece->first * record_size, data, size);
	scene.dirty = TRUE;
}

void renderer_scene_camera(const struct xgpu_scene_camera *camera)
{
	scene.camera = *camera;
	scene.have_camera = TRUE;
}

void renderer_scene_drawn(const uint32_t *surfaces, uint32_t count)
{
	uint32_t index;

	if (!scene.drawn)
		return;
	memset(scene.drawn, 0, (scene.counts.surface_count / 32 + 1) * sizeof(*scene.drawn));
	for (index = 0; index < count; index++)
	{
		if (surfaces[index] < scene.counts.surface_count)
			scene.drawn[surfaces[index] / 32] |= 1u << (surfaces[index] % 32);
	}
}

D3D12_GPU_VIRTUAL_ADDRESS renderer_scene_drawn_bits(void)
{
	uint32_t words = scene.counts.surface_count / 32 + 1;
	struct renderer_upload upload = renderer_upload_allocate((uint64_t)words * sizeof(uint32_t), 16);

	if (scene.drawn)
		memcpy(upload.data, scene.drawn, (size_t)words * sizeof(uint32_t));
	else
		memset(upload.data, 0, (size_t)words * sizeof(uint32_t));
	return upload.address;
}

const struct xgpu_scene_camera *renderer_scene_camera_get(void)
{
	return scene.have_camera ? &scene.camera : NULL;
}

/* ---------- the GPU's copies */

/* a buffer in the GPU's memory, filled from bytes (through an upload heap) */
static ID3D12Resource *buffer_filled(const void *bytes, uint64_t size)
{
	ID3D12Resource *buffer = renderer_buffer_create(D3D12_HEAP_TYPE_DEFAULT, size ? size : 16,
		D3D12_RESOURCE_STATE_COMMON);
	uint64_t offset;

	/* (in pieces an upload heap can hold) */
	for (offset = 0; offset < size; offset += 32ull << 20)
	{
		uint64_t piece = size - offset < (32ull << 20) ? size - offset : 32ull << 20;
		struct renderer_upload upload = renderer_upload_allocate(piece, 16);

		memcpy(upload.data, (const uint8_t *)bytes + offset, piece);
		ID3D12GraphicsCommandList_CopyBufferRegion(renderer.list, buffer, offset, upload.buffer, upload.offset, piece);
	}
	renderer_transition(buffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	return buffer;
}

/* whether a geometry's records are within the scene, and its indices
within its vertices */
static BOOL geometry_valid(const struct xgpu_scene_geometry *geometry)
{
	uint32_t index;

	if (geometry->material >= scene.counts.material_count ||
		geometry->first_vertex > scene.counts.vertex_count ||
		geometry->vertex_count > scene.counts.vertex_count - geometry->first_vertex ||
		geometry->first_index > scene.counts.index_count ||
		geometry->index_count > scene.counts.index_count - geometry->first_index ||
		!geometry->index_count || geometry->index_count % 3 || !geometry->vertex_count)
	{
		return FALSE;
	}
	for (index = 0; index < geometry->index_count; index++)
	{
		if (scene.indices[geometry->first_index + index] >= geometry->vertex_count)
			return FALSE;
	}
	return TRUE;
}

static BOOL material_alpha_tested(uint32_t material)
{
	return (scene.materials[material].flags & (1u << _xgpu_scene_material_alpha_tested_bit)) != 0;
}

BOOL renderer_scene_prepare(void)
{
	D3D12_RAYTRACING_GEOMETRY_DESC *descriptions;
	struct gpu_geometry *table;
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs;
	D3D12_RAYTRACING_INSTANCE_DESC *instance;
	struct renderer_upload instances;
	D3D12_GPU_VIRTUAL_ADDRESS vertices, indices;
	uint32_t index, traced = 0, invalid = 0;

	if (!scene.dirty)
		return scene.top != NULL;
	scene.dirty = FALSE;
	gpu_release();
	if (!scene.counts.geometry_count)
		return FALSE;
	descriptions = calloc(scene.counts.geometry_count, sizeof(*descriptions));
	table = calloc(scene.counts.geometry_count, sizeof(*table));
	free(scene.traced_materials);
	scene.traced_materials = calloc(scene.counts.geometry_count, sizeof(*scene.traced_materials));
	if (!descriptions || !table || !scene.traced_materials)
		renderer_fail("out of memory");
	scene.vertex_buffer = buffer_filled(scene.vertices, (uint64_t)scene.counts.vertex_count * sizeof(*scene.vertices));
	scene.index_buffer = buffer_filled(scene.indices, (uint64_t)scene.counts.index_count * sizeof(*scene.indices));
	vertices = ID3D12Resource_GetGPUVirtualAddress(scene.vertex_buffer);
	indices = ID3D12Resource_GetGPUVirtualAddress(scene.index_buffer);
	for (index = 0; index < scene.counts.geometry_count; index++)
	{
		const struct xgpu_scene_geometry *geometry = &scene.geometries[index];
		D3D12_RAYTRACING_GEOMETRY_DESC *description = &descriptions[traced];

		if (!geometry_valid(geometry))
		{
			invalid++;
			continue;
		}
		description->Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		/* (an alpha-tested one's texels decide each hit: the trace's
		candidate loop) */
		description->Flags = material_alpha_tested(geometry->material) ?
			D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		description->Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		description->Triangles.VertexCount = geometry->vertex_count;
		description->Triangles.VertexBuffer.StartAddress = vertices +
			(uint64_t)geometry->first_vertex * sizeof(struct xgpu_scene_vertex);
		description->Triangles.VertexBuffer.StrideInBytes = sizeof(struct xgpu_scene_vertex);
		description->Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
		description->Triangles.IndexCount = geometry->index_count;
		description->Triangles.IndexBuffer = indices + (uint64_t)geometry->first_index * sizeof(uint32_t);
		table[traced].first_vertex = geometry->first_vertex;
		table[traced].first_index = geometry->first_index;
		table[traced].material = geometry->material;
		table[traced].first_surface = geometry->first_surface;
		scene.traced_materials[traced] = geometry->material;
		traced++;
	}
	if (invalid)
		renderer_log("path tracing: %u of the scene's %u geometries are not within it, and are not traced", invalid,
			scene.counts.geometry_count);
	scene.traced_geometry_count = traced;
	if (traced)
	{
		scene.geometry_buffer = buffer_filled(table, (uint64_t)traced * sizeof(*table));
		memset(&inputs, 0, sizeof(inputs));
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		inputs.NumDescs = traced;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.pGeometryDescs = descriptions;
		scene.bottom = renderer_acceleration_structure_build(&inputs);

		instances = renderer_upload_allocate(sizeof(*instance), D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);
		instance = instances.data;
		memset(instance, 0, sizeof(*instance));
		instance->Transform[0][0] = instance->Transform[1][1] = instance->Transform[2][2] = 1.0f;
		instance->InstanceMask = 0xff;
		instance->AccelerationStructure = ID3D12Resource_GetGPUVirtualAddress(scene.bottom);
		memset(&inputs, 0, sizeof(inputs));
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		inputs.NumDescs = 1;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.InstanceDescs = instances.address;
		scene.top = renderer_acceleration_structure_build(&inputs);
		renderer_log("path tracing: the scene: %u vertices, %u triangles in %u geometries", scene.counts.vertex_count,
			scene.counts.index_count / 3, traced);
	}
	free(descriptions);
	free(table);
	return scene.top != NULL;
}

uint32_t renderer_scene_geometry_material(uint32_t geometry)
{
	return geometry < scene.traced_geometry_count ? scene.traced_materials[geometry] : 0;
}

/* the material table, with each texture's descriptor in the view heap (as
textures are now: their views can be made again), for one trace */
D3D12_GPU_VIRTUAL_ADDRESS renderer_scene_materials(void)
{
	struct renderer_upload upload;
	struct renderer_scene_gpu_material *materials;
	uint32_t index, count = scene.counts.material_count ? scene.counts.material_count : 1;

	upload = renderer_upload_allocate((uint64_t)count * sizeof(*materials), 16);
	materials = upload.data;
	memset(materials, 0, (size_t)count * sizeof(*materials));
	for (index = 0; index < scene.counts.material_count; index++)
	{
		const struct xgpu_scene_material *from = &scene.materials[index];
		struct renderer_scene_gpu_material *to = &materials[index];
		const uint32_t maps[5] = { from->base_map, from->primary_detail_map, from->secondary_detail_map,
			from->micro_detail_map, from->bump_map };
		int map;

		for (map = 0; map < 5; map++)
		{
			struct renderer_texture *texture = renderer_texture_get(maps[map]);

			to->maps[map] = texture && texture->definition.dimension == _xgpu_texture_2d ? texture->srv.index :
				RENDERER_SCENE_NO_TEXTURE;
		}
		to->flags = from->flags;
		to->functions = (from->detail_function & 0xff) | (from->micro_detail_function & 0xff) << 8 |
			(from->environment_type & 0xff) << 16;
		to->scales[0] = from->primary_detail_scale;
		to->scales[1] = from->secondary_detail_scale;
		to->scales[2] = from->micro_detail_scale;
		to->scales[3] = from->bump_scale[0];
		to->bump_scale_v = from->bump_scale[1];
		memcpy(to->emission, from->emission, sizeof(from->emission));
	}
	return upload.address;
}

void renderer_scene_buffers(D3D12_GPU_VIRTUAL_ADDRESS *top, D3D12_GPU_VIRTUAL_ADDRESS *vertices,
	D3D12_GPU_VIRTUAL_ADDRESS *indices, D3D12_GPU_VIRTUAL_ADDRESS *geometries)
{
	*top = ID3D12Resource_GetGPUVirtualAddress(scene.top);
	*vertices = ID3D12Resource_GetGPUVirtualAddress(scene.vertex_buffer);
	*indices = ID3D12Resource_GetGPUVirtualAddress(scene.index_buffer);
	*geometries = ID3D12Resource_GetGPUVirtualAddress(scene.geometry_buffer);
}

/* ---------- a lost device: the scene is kept, its GPU copies made again */

void renderer_scene_lost(void)
{
	if (scene.vertex_buffer)
		ID3D12Resource_Release(scene.vertex_buffer);
	if (scene.index_buffer)
		ID3D12Resource_Release(scene.index_buffer);
	if (scene.geometry_buffer)
		ID3D12Resource_Release(scene.geometry_buffer);
	if (scene.bottom)
		ID3D12Resource_Release(scene.bottom);
	if (scene.top)
		ID3D12Resource_Release(scene.top);
	scene.vertex_buffer = scene.index_buffer = scene.geometry_buffer = NULL;
	scene.bottom = scene.top = NULL;
}

void renderer_scene_restore(void)
{
	scene.dirty = scene.counts.geometry_count != 0;
}
