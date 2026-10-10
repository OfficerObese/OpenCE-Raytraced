/*
PT_COMMON.HLSLI

What the path tracer's shaders share: the scene's records as
renderer_scene.c lays them out (struct xgpu_scene_vertex, struct
gpu_geometry, struct renderer_scene_gpu_material), and reading a surface
at a ray's hit.
*/

#ifndef PT_COMMON_HLSLI
#define PT_COMMON_HLSLI

#define NO_TEXTURE 0xffffffffu

/* struct xgpu_scene_vertex */
struct scene_vertex
{
	float3 position;
	float3 normal;
	float3 binormal;
	float3 tangent;
	float2 texcoord;
};

/* renderer_scene.c's struct gpu_geometry */
struct scene_geometry
{
	uint first_vertex;
	uint first_index;
	uint material;
	/* the structure BSP's surface its first triangle is */
	uint first_surface;
};

/* struct renderer_scene_gpu_material */
struct scene_material
{
	/* the view heap's descriptors of the base, primary detail, secondary
	detail, micro detail and bump maps (NO_TEXTURE: none) */
	uint maps[5];
	uint flags;
	/* detail function, micro detail function, environment type: a byte each */
	uint functions;
	uint reserved;
	/* primary, secondary and micro detail scales, the bump map's scale
	across */
	float4 scales;
	float3 emission;
	/* the bump map's scale down */
	float bump_scale_v;
};

#define MATERIAL_ALPHA_TESTED 1u
#define MATERIAL_MIRROR 2u

RaytracingAccelerationStructure scene : register(t0);
StructuredBuffer<scene_vertex> vertices : register(t1);
StructuredBuffer<uint> indices : register(t2);
StructuredBuffer<scene_geometry> geometries : register(t3);
StructuredBuffer<scene_material> materials : register(t4);
/* every texture, by its descriptor in the view heap */
Texture2D<float4> textures[] : register(t0, space1);
SamplerState linear_wrap : register(s0);

/* a surface where a ray hit it, its vertices' attributes interpolated */
struct surface
{
	float3 normal;
	float2 texcoord;
	uint material;
};

surface surface_at(uint geometry_index, uint primitive, float2 barycentrics)
{
	scene_geometry geometry = geometries[geometry_index];
	uint first = geometry.first_index + primitive * 3;
	scene_vertex a = vertices[geometry.first_vertex + indices[first]];
	scene_vertex b = vertices[geometry.first_vertex + indices[first + 1]];
	scene_vertex c = vertices[geometry.first_vertex + indices[first + 2]];
	float3 weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
	surface result;

	result.normal = normalize(a.normal * weights.x + b.normal * weights.y + c.normal * weights.z);
	result.texcoord = a.texcoord * weights.x + b.texcoord * weights.y + c.texcoord * weights.z;
	result.material = geometry.material;
	return result;
}

float4 texture_sample(uint map, float2 texcoord, float level)
{
	if (map == NO_TEXTURE)
		return float4(1.0, 1.0, 1.0, 1.0);
	return textures[NonUniformResourceIndex(map)].SampleLevel(linear_wrap, texcoord, level);
}

/* a detail map's texel combined with the colour under it, as the game's
detail functions do: double/biased multiply, multiply, double/biased add */
float3 detail_combine(float3 color, float3 detail, uint function)
{
	if (function == 1)
		return color * detail;
	if (function == 2)
		return saturate(color + 2.0 * detail - 1.0);
	return saturate(color * detail * 2.0);
}

/* the surface's colour, unlit: the base map with its detail maps (the
secondary where the base map's alpha is low, for a normal shader) */
float3 surface_albedo(surface hit, float level)
{
	scene_material material = materials[hit.material];
	float4 base = texture_sample(material.maps[0], hit.texcoord, level);
	uint detail_function = material.functions & 0xff;
	float3 color = base.rgb;

	if (material.maps[1] != NO_TEXTURE)
	{
		float3 detail = texture_sample(material.maps[1], hit.texcoord * material.scales.x, level).rgb;

		if (material.maps[2] != NO_TEXTURE)
		{
			float3 secondary = texture_sample(material.maps[2], hit.texcoord * material.scales.y, level).rgb;

			detail = lerp(secondary, detail, base.a);
		}
		color = detail_combine(color, detail, detail_function);
	}
	return color;
}

/* an alpha-tested surface's hole: where its bump map's alpha is below half,
as the game tests it */
bool surface_hole(uint geometry_index, uint primitive, float2 barycentrics)
{
	surface hit = surface_at(geometry_index, primitive, barycentrics);
	scene_material material = materials[hit.material];

	return (material.flags & MATERIAL_ALPHA_TESTED) &&
		texture_sample(material.maps[4], hit.texcoord * float2(material.scales.w, material.bump_scale_v), 0.0).a < 0.5;
}

#endif
