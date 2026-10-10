/*
PT_TRACE.HLSL

The path tracer's rays from the camera, one through each pixel of the 3D
view's viewport (renderer_path_tracer.c): what each sees, for the debug
views (debug.path_tracing_view), written to a texture of the viewport's
size, whose alpha is 0 where nothing is drawn over the game's pixel.

The depth parity view also counts, against the game's depth buffer, where
the rays' surfaces agree with the game's, are behind them (something the
scene does not have yet is in front), in front of them, where the rays hit
nothing but the game drew, and the reverse.
*/

#include "pt_common.hlsli"

/* (the textures and the game's depth are drawn into and uploaded to while
this signature is still set for compute: their data are volatile) */
#define TRACE_SIGNATURE \
	"CBV(b0), SRV(t0), SRV(t1), SRV(t2), SRV(t3), SRV(t4), " \
	"DescriptorTable(SRV(t0, space = 1, numDescriptors = unbounded, flags = DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
	"DescriptorTable(UAV(u0)), UAV(u1), DescriptorTable(SRV(t5, flags = DATA_VOLATILE)), SRV(t6), " \
	"StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_WRAP, " \
	"addressV = TEXTURE_ADDRESS_WRAP, addressW = TEXTURE_ADDRESS_WRAP)"

cbuffer trace_constants : register(b0)
{
	row_major float4x4 clip_to_world;
	float4 camera_position;
	/* x, y, width, height, in the target's pixels */
	uint4 viewport;
	/* the view (an _xgpu_path_tracing_view), the game has depth */
	uint4 options;
	/* where the game's image lies from Direct3D 12's pixel centres, in
	pixels (half of one of the game's: Direct3D 8 centres them on whole
	pixels) */
	float4 pixel_offset;
};

RWTexture2D<float4> output : register(u0);
/* the depth parity counts: agree, behind, in front, missed, extra, not
drawn by the game; then the pixels in front for each geometry
(PARITY_GEOMETRIES of them) */
RWByteAddressBuffer counts : register(u1);
#define PARITY_KINDS 6
#define PARITY_GEOMETRIES 4096
Texture2D<float> game_depth : register(t5);
/* the surfaces the game drew, a bit each */
ByteAddressBuffer drawn : register(t6);

/* whether the game drew the surface a ray hit, and it is not a mirror (the
game's depth there is of what the mirror shows) */
bool game_drew_surface(uint geometry_index, uint primitive)
{
	scene_geometry geometry = geometries[geometry_index];
	uint surface = geometry.first_surface + primitive;

	return !(materials[geometry.material].flags & MATERIAL_MIRROR) &&
		(drawn.Load((surface / 32) * 4) & (1u << (surface % 32))) != 0;
}

#define VIEW_ALBEDO 1
#define VIEW_NORMALS 2
#define VIEW_DEPTH_PARITY 3

float3 unproject(float2 ndc, float z)
{
	float4 position = mul(clip_to_world, float4(ndc, z, 1.0));

	return position.xyz / position.w;
}

[RootSignature(TRACE_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	float2 pixel, ndc;
	float3 origin, direction;
	RayDesc ray;
	RayQuery<RAY_FLAG_NONE> query;
	float4 result = float4(0.0, 0.0, 0.0, 0.0);
	bool hit;

	if (any(id.xy >= viewport.zw))
		return;
	pixel = float2(viewport.xy + id.xy) + 0.5 - pixel_offset.xy;
	ndc = float2((pixel.x - viewport.x) / viewport.z * 2.0 - 1.0, 1.0 - (pixel.y - viewport.y) / viewport.w * 2.0);
	/* (from the near plane, as the game's clips) */
	origin = unproject(ndc, 0.0);
	direction = normalize(unproject(ndc, 1.0) - origin);

	ray.Origin = origin;
	ray.Direction = direction;
	ray.TMin = 0.0;
	ray.TMax = 1.0e6;
	query.TraceRayInline(scene, RAY_FLAG_NONE, 0xff, ray);
	while (query.Proceed())
	{
		/* (an alpha-tested surface: a hit where its texel is not a hole) */
		if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
			!surface_hole(query.CandidateGeometryIndex(), query.CandidatePrimitiveIndex(),
			query.CandidateTriangleBarycentrics()))
		{
			query.CommitNonOpaqueTriangleHit();
		}
	}
	hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;

	if (options.x == VIEW_DEPTH_PARITY)
	{
		float depth = options.y ? game_depth.Load(int3(viewport.xy + id.xy, 0)) : 1.0;
		bool game_drew = depth < 1.0;
		uint kind = 6;

		if (hit && !game_drew_surface(query.CommittedGeometryIndex(), query.CommittedPrimitiveIndex()))
		{
			kind = 5;
		}
		else if (hit && game_drew)
		{
			float game_distance = length(unproject(ndc, depth) - origin);
			float distance = query.CommittedRayT();
			float tolerance = 0.01 * game_distance + 0.01;

			kind = abs(distance - game_distance) <= tolerance ? 0 : distance > game_distance ? 1 : 2;
		}
		else
		{
			kind = game_drew ? 3 : hit ? 4 : 5;
		}
		if (kind < 6)
			counts.InterlockedAdd(kind * 4, 1);
		if (kind == 2)
			counts.InterlockedAdd((PARITY_KINDS + min(query.CommittedGeometryIndex(), PARITY_GEOMETRIES - 1)) * 4, 1);
		result = kind == 0 ? float4(0.0, 0.6, 0.0, 1.0) : kind == 1 ? float4(0.0, 0.2, 0.8, 1.0) :
			kind == 2 ? float4(0.9, 0.0, 0.0, 1.0) : kind == 3 ? float4(0.8, 0.0, 0.8, 1.0) :
			kind == 4 ? float4(0.9, 0.9, 0.0, 1.0) : kind == 5 ? float4(0.0, 0.7, 0.7, 1.0) : float4(0.0, 0.0, 0.0, 0.0);
	}
	else if (hit)
	{
		surface surface_hit = surface_at(query.CommittedGeometryIndex(), query.CommittedPrimitiveIndex(),
			query.CommittedTriangleBarycentrics());

		if (options.x == VIEW_NORMALS)
			result = float4(surface_hit.normal * 0.5 + 0.5, 1.0);
		else
			result = float4(surface_albedo(surface_hit, 0.0), 1.0);
	}
	output[id.xy] = result;
}
