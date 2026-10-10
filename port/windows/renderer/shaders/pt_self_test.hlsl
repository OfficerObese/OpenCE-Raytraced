/*
PT_SELF_TEST.HLSL

The path tracer's check of the GPU, run once when path tracing is first
turned on (renderer_path_tracer.c): one ray from each texel of a small
target straight along +z, against a scene of one triangle that covers the
texels with x below their y. A texel holds 1 where its ray hit the triangle,
and the distance to it; 0 where it missed.
*/

#define SELF_TEST_SIGNATURE "SRV(t0), DescriptorTable(UAV(u0))"

RaytracingAccelerationStructure scene : register(t0);
RWTexture2D<float2> results : register(u0);

[RootSignature(SELF_TEST_SIGNATURE)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
	RayDesc ray;

	ray.Origin = float3(id.x + 0.5, id.y + 0.5, -1.0);
	ray.Direction = float3(0.0, 0.0, 1.0);
	ray.TMin = 0.0;
	ray.TMax = 100.0;
	query.TraceRayInline(scene, RAY_FLAG_NONE, 0xff, ray);
	while (query.Proceed())
	{
	}
	results[id.xy] = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ?
		float2(1.0, query.CommittedRayT()) : float2(0.0, 0.0);
}
