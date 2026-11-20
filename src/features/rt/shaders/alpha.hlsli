// Alpha-tested geometry for inline ray queries (docs/raytracing.md section 13), shared by the
// mod's ray passes (rays.hlsl) and the flashlight's spotlight shader (spotlight.hlsl).
//
// The scene (scene.cpp) marks alpha-tested geometry non-opaque and writes, per update, one
// buffer: records (16 bytes each), then the UVs of every alpha-tested triangle (3 float2, 24
// bytes), gathered on the GPU from the game's vertex buffers by cs_gather (rays.hlsl).
//   record index = CandidateInstanceID() + CandidateGeometryIndex()
//   record       = uint4(UV byte offset of triangle 0, texture index in the mod's heap,
//                        texture index in the game's heap, asuint(alpha threshold))
// The including file defines TB_ALPHA_BUF (ByteAddressBuffer), TB_ALPHA_TEX (Texture2D array)
// and TB_ALPHA_SMP (a wrap sampler), and TB_ALPHA_GAME_HEAP 1 when TB_ALPHA_TEX is the game's own
// descriptor heap (the spotlight runs inside the game's lists).

#ifndef TB_ALPHA_GAME_HEAP
#define TB_ALPHA_GAME_HEAP 0
#endif

bool tb_alpha_pass(uint rec, uint prim, float2 bary) {
  uint4 r = TB_ALPHA_BUF.Load4(rec * 16);
  if (r.x == 0xffffffffu) return true;  // no UVs for it: opaque
  uint o = r.x + prim * 24;
  float2 a = asfloat(TB_ALPHA_BUF.Load2(o));
  float2 b = asfloat(TB_ALPHA_BUF.Load2(o + 8));
  float2 c = asfloat(TB_ALPHA_BUF.Load2(o + 16));
  float2 uv = a + (b - a) * bary.x + (c - a) * bary.y;
  uint ti = TB_ALPHA_GAME_HEAP ? r.z : r.y;
  float al = TB_ALPHA_TEX[NonUniformResourceIndex(ti)].SampleLevel(TB_ALPHA_SMP, uv, 0).a;
  return al >= asfloat(r.w);
}

// Any hit (shadow / AO rays): alpha-tested candidates pass through where the texture is clear.
bool tb_occluded(RaytracingAccelerationStructure s, uint mask, RayDesc ray, bool alpha) {
  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
  q.TraceRayInline(s, alpha ? RAY_FLAG_NONE : RAY_FLAG_FORCE_OPAQUE, mask, ray);
  while (q.Proceed()) {
    if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
        tb_alpha_pass(q.CandidateInstanceID() + q.CandidateGeometryIndex(), q.CandidatePrimitiveIndex(),
                      q.CandidateTriangleBarycentrics()))
      q.CommitNonOpaqueTriangleHit();
  }
  return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// Closest hit: its distance and TLAS instance index (false on a miss).
bool tb_closest(RaytracingAccelerationStructure s, uint mask, RayDesc ray, bool alpha, out float t, out uint inst) {
  RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
  q.TraceRayInline(s, alpha ? RAY_FLAG_NONE : RAY_FLAG_FORCE_OPAQUE, mask, ray);
  while (q.Proceed()) {
    if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
        tb_alpha_pass(q.CandidateInstanceID() + q.CandidateGeometryIndex(), q.CandidatePrimitiveIndex(),
                      q.CandidateTriangleBarycentrics()))
      q.CommitNonOpaqueTriangleHit();
  }
  bool hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
  t = hit ? q.CommittedRayT() : -1.0;
  inst = hit ? q.CommittedInstanceIndex() : 0xffffffffu;
  return hit;
}
