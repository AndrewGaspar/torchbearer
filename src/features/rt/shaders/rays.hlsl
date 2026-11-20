// The mod's ray-traced passes on the scene's TLAS (docs/raytracing.md sections 9 and 13), compute
// shaders with inline ray queries (cs_6_5), built at dev time by scripts/build_shaders and embedded
// as DXIL (rays_dxil.h). Root signature: trace.cpp (trace_init).
//
//   cs_gather   per update: the UVs of every alpha-tested triangle from the game's vertex / index
//               buffers into the alpha buffer (alpha.hlsli), one thread per triangle
//   cs_ao       ambient occlusion rays (cosine lobe around the G-buffer normal, first hit ends),
//               plus the receiver's TLAS instance (a short primary ray) for the motion
//               reprojection in cs_temporal
//   cs_sun      sun / moon visibility (rays inside the light's cone; prototype, [dev] rt_sun)
//   cs_val      validation: a primary ray per pixel against the G-buffer depth (histogram, image)
//   cs_temporal per-eye accumulation, history reprojected through each pixel's own instance
//               motion (current and previous TLAS transforms) and the eye's previous camera
//   cs_atrous   depth / normal-aware a-trous filter; the last pass writes the result

cbuffer C : register(b0) {
  row_major float4x4 g_c2v;   // clip -> view (this eye, jittered, as the engine's $Globals)
  row_major float4x4 g_v2w;   // view -> world relative to the TLAS anchor
  row_major float4x4 g_w2pc;  // world (anchor-relative) -> this eye's previous clip
  float4 g_p0;  // AO radius (m), normal bias (m), frame index, history length
  float4 g_p1;  // width, height, 1/width, 1/height
  uint4 g_u0;   // instance mask, samples per pixel | stage << 8, a-trous step | image, history valid
  float4 g_sun; // world direction to the light, cone half-angle (radians)
  float4 g_p2;  // AO strength (exponent), validation ray length, receiver tolerance, -
  uint4 g_u1;   // motion on, alpha on, receiver from the primary hit, gather: byte offset of the groups
};

RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> t_depth : register(t1);
Texture2D<float4> t_normal : register(t2);
Texture2D<float2> t_a : register(t3);
Texture2D<float2> t_b : register(t4);
Texture2D<float> t_c : register(t5);
Texture2D<uint> t_inst : register(t6);
RWTexture2D<float2> u_a : register(u0);
RWTexture2D<float> u_b : register(u1);
RWByteAddressBuffer u_stats : register(u2);
RWTexture2D<float4> u_vis : register(u3);
RWTexture2D<uint> u_inst : register(u4);
ByteAddressBuffer g_alpha : register(t9);
Texture2D<float4> g_alpha_tex[8192] : register(t0, space1);
ByteAddressBuffer g_motion : register(t10);
ByteAddressBuffer g_bufs[1024] : register(t0, space2);
ByteAddressBuffer g_jobs : register(t11);
RWByteAddressBuffer u_uv : register(u6);
SamplerState s_lin : register(s0);
SamplerState s_pt : register(s1);
SamplerState s_wrap : register(s2);

#define TB_ALPHA_BUF g_alpha
#define TB_ALPHA_TEX g_alpha_tex
#define TB_ALPHA_SMP s_wrap
#include "alpha.hlsli"

bool is_geo(float z) { return z > 0.0 && z < 1.0; }
float3 view_pos(int2 px, float z) {
  float2 ndc = (float2(px) + 0.5) * g_p1.zw * float2(2, -2) + float2(-1, 1);
  float4 v = mul(g_c2v, float4(ndc, z, 1));
  return v.xyz / v.w;
}
int2 clampi(int2 q) { return clamp(q, int2(0, 0), int2(g_p1.xy) - 1); }
float3 view_at(int2 q) { q = clampi(q); return view_pos(q, t_depth[q]); }
// Geometric normal from the depth buffer (the side with the smaller depth step), facing the eye.
float3 geo_normal(int2 px, float3 p) {
  float3 r = view_at(px + int2(1, 0)), l = view_at(px - int2(1, 0));
  float3 d = view_at(px + int2(0, 1)), u = view_at(px - int2(0, 1));
  float3 dx = abs(r.z - p.z) < abs(p.z - l.z) ? r - p : p - l;
  float3 dy = abs(d.z - p.z) < abs(p.z - u.z) ? d - p : p - u;
  float3 n = normalize(cross(dy, dx));
  return dot(n, -p) < 0 ? -n : n;
}
float3 shading_normal(int2 px, float3 ng) {
  float3 n = t_normal[px].xyz * 2 - 1;
  float l = length(n);
  n = l > 0.1 ? n / l : ng;
  return dot(n, ng) < 0.1 ? ng : n;
}
uint hash(uint x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
float2 rnd2(int2 px, uint i) {
  uint h = hash(uint(px.x) * 1973u + uint(px.y) * 9277u + 26699u);
  float2 b = float2(h & 0xffff, h >> 16) / 65536.0;
  return frac(b + float2(0.7548776662, 0.5698402910) * float(i % 4096u));
}
float3 cos_dir(float3 n, float2 u) {
  float s = n.z >= 0 ? 1.0 : -1.0;
  float a = -1.0 / (s + n.z);
  float b = n.x * n.y * a;
  float3 t = float3(1 + s * n.x * n.x * a, s * b, -s * n.x);
  float3 bt = float3(b, s + n.y * n.y * a, -n.y);
  float r = sqrt(u.x), ph = 6.2831853 * u.y;
  return normalize(t * (r * cos(ph)) + bt * (r * sin(ph)) + n * sqrt(max(0.0, 1.0 - u.x)));
}
float3 cam_pos() { return float3(g_v2w[0].w, g_v2w[1].w, g_v2w[2].w); }

// ---- per-update UV gather ------------------------------------------------------------------------
// Job (64 bytes) per alpha-tested mesh:
//   uint4 a: index buffer slot, index byte offset, triangles, output byte offset
//   uint4 b: vertex buffer slot, byte offset of the base vertex in the UV stream, stride,
//            mode | format A << 8 | format B << 16 | component of A << 24
//   uint4 c: attribute A offset | attribute B offset << 16, asuint(scale), constant buffer slot,
//            byte offset of the atlas table's first row
//   uint4 d: rows per atlas entry, index size (2 or 4), -, -
// Modes (the game's vertex shaders, static): 1 UV = A.xy * scale (R16G16_SINT / 4095), 2 UV = A.xy,
// 3 atlas (the grass cards): e = int(A.c * 255 + 0.1), UV = B.xy * cb[e].zw + cb[e].xy.
// Groups (uint2 job, first triangle) from byte offset g_u1.w.
float attr(uint slot, uint addr, uint fmt, uint c) {
  uint w;
  switch (fmt) {
    case 38: {  // R16G16_SINT
      w = g_bufs[NonUniformResourceIndex(slot)].Load(addr);
      int v = c ? int(w) >> 16 : int(w << 16) >> 16;
      return float(v);
    }
    case 37: {  // R16G16_SNORM
      w = g_bufs[NonUniformResourceIndex(slot)].Load(addr);
      int v = c ? int(w) >> 16 : int(w << 16) >> 16;
      return max(float(v) / 32767.0, -1.0);
    }
    case 35:  // R16G16_UNORM
      w = g_bufs[NonUniformResourceIndex(slot)].Load(addr);
      return float(c ? w >> 16 : w & 0xffff) / 65535.0;
    case 34:  // R16G16_FLOAT
      w = g_bufs[NonUniformResourceIndex(slot)].Load(addr);
      return f16tof32(c ? w >> 16 : w & 0xffff);
    case 87:  // B8G8R8A8_UNORM: x = R (byte 2), y = G (1), z = B (0), w = A (3)
      w = g_bufs[NonUniformResourceIndex(slot)].Load(addr);
      return float((w >> (c == 0 ? 16 : c == 1 ? 8 : c == 2 ? 0 : 24)) & 0xff) / 255.0;
    case 28:  // R8G8B8A8_UNORM
      w = g_bufs[NonUniformResourceIndex(slot)].Load(addr);
      return float((w >> (8 * c)) & 0xff) / 255.0;
    case 16:  // R32G32_FLOAT
    default:
      return asfloat(g_bufs[NonUniformResourceIndex(slot)].Load(addr + 4 * c));
  }
}

[numthreads(64, 1, 1)] void cs_gather(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
  uint2 g = g_jobs.Load2(g_u1.w + gid.x * 8);
  uint4 a = g_jobs.Load4(g.x * 64), b = g_jobs.Load4(g.x * 64 + 16), c = g_jobs.Load4(g.x * 64 + 32),
        d = g_jobs.Load4(g.x * 64 + 48);
  uint tri = g.y + gi;
  if (tri >= a.z) return;
  uint mode = b.w & 0xff, fa = (b.w >> 8) & 0xff, fb = (b.w >> 16) & 0xff, ca = b.w >> 24;
  float scale = asfloat(c.y);
  float2 uv[3];
  for (uint k = 0; k < 3; k++) {
    uint io = a.y + (tri * 3 + k) * d.y;
    uint iw = g_bufs[NonUniformResourceIndex(a.x)].Load(io & ~3u);
    uint idx = d.y == 4 ? iw : (io & 2) ? iw >> 16 : iw & 0xffff;
    uint va = b.y + idx * b.z;
    if (mode == 3) {
      uint e = uint(int(attr(b.x, va + (c.x & 0xffff), fa, ca) * 255.0 + 0.1));
      float4 r = asfloat(g_bufs[NonUniformResourceIndex(c.z)].Load4(c.w + e * d.x * 16));
      uint ob = va + (c.x >> 16);
      uv[k] = float2(attr(b.x, ob, fb, 0), attr(b.x, ob, fb, 1)) * r.zw + r.xy;
    } else {
      uint oa = va + (c.x & 0xffff);
      uv[k] = float2(attr(b.x, oa, fa, 0), attr(b.x, oa, fa, 1)) * (mode == 1 ? scale : 1.0);
    }
  }
  uint o = a.w + tri * 24;
  u_uv.Store2(o, asuint(uv[0]));
  u_uv.Store2(o + 8, asuint(uv[1]));
  u_uv.Store2(o + 16, asuint(uv[2]));
}

// ---- rays ----------------------------------------------------------------------------------------

[numthreads(8, 8, 1)] void cs_ao(uint3 id : SV_DispatchThreadID) {
  int2 px = int2(id.xy);
  if (any(px >= int2(g_p1.xy))) return;
  float z = t_depth[px];
  if (!is_geo(z)) { u_a[px] = float2(1, 1e4); u_inst[px] = 0xffffffffu; return; }
  bool alpha = g_u1.y != 0;
  float3 p = view_pos(px, z);
  float3 ng = geo_normal(px, p);
  float3 ns = shading_normal(px, ng);
  float3 wp = mul(g_v2w, float4(p, 1)).xyz;
  float3 wn = normalize(mul((float3x3)g_v2w, ns));
  float3 wg = normalize(mul((float3x3)g_v2w, ng));
  float d = length(p);
  // The receiver on our geometry: its instance (motion) and, optionally, its surface as the origin.
  float3 cam = cam_pos();
  RayDesc pr;
  pr.Origin = cam;
  pr.Direction = (wp - cam) / max(d, 1e-4);
  pr.TMin = max(d * (1.0 - g_p2.z) - 0.02, 0.0);
  pr.TMax = d * (1.0 + g_p2.z) + 0.05;
  float th;
  uint inst;
  bool hit = tb_closest(Scene, g_u0.x & 0xff, pr, alpha, th, inst);
  u_inst[px] = inst;
  float3 base = hit && g_u1.z != 0 ? cam + pr.Direction * th : wp;
  float3 o = base + wg * (g_p0.y + d * (hit && g_u1.z != 0 ? 0.0005 : 0.0015));
  uint spp = max(g_u0.y & 0xff, 1u);
  float vis = 0;
  for (uint s = 0; s < spp; s++) {
    float3 dir = cos_dir(wn, rnd2(px, uint(g_p0.z) * spp + s));
    float dg = dot(dir, wg);
    if (dg < 0.02) dir = normalize(dir - wg * (dg - 0.02) * 2.0);
    RayDesc r;
    r.Origin = o;
    r.Direction = dir;
    r.TMin = hit && g_u1.z != 0 ? 0.005 + d * 0.0005 : 0.01 + d * 0.002;  // past the receiver's own surface
    r.TMax = g_p0.x;
    vis += tb_occluded(Scene, g_u0.x & 0xff, r, alpha) ? 0.0 : 1.0;
  }
  u_a[px] = float2(vis / float(spp), p.z);
}

// Sun / moon visibility (prototype): rays towards the light inside its cone.
[numthreads(8, 8, 1)] void cs_sun(uint3 id : SV_DispatchThreadID) {
  int2 px = int2(id.xy);
  if (any(px >= int2(g_p1.xy))) return;
  float z = t_depth[px];
  if (!is_geo(z)) { u_a[px] = float2(1, 1e4); return; }
  float3 p = view_pos(px, z);
  float3 ng = geo_normal(px, p);
  float3 wp = mul(g_v2w, float4(p, 1)).xyz;
  float3 wg = normalize(mul((float3x3)g_v2w, ng));
  float3 L = normalize(g_sun.xyz);
  float vis = 0;
  uint spp = max(g_u0.y & 0xff, 1u);
  float3 t = normalize(abs(L.y) < 0.99 ? cross(L, float3(0, 1, 0)) : cross(L, float3(1, 0, 0)));
  float3 b = cross(L, t);
  for (uint s = 0; s < spp; s++) {
    float2 u = rnd2(px, uint(g_p0.z) * spp + s);
    float ct = 1.0 - u.x * (1.0 - cos(g_sun.w)), st = sqrt(max(0.0, 1.0 - ct * ct)), ph = 6.2831853 * u.y;
    float3 dir = normalize(L * ct + t * (st * cos(ph)) + b * (st * sin(ph)));
    if (dot(dir, wg) <= 0) continue;
    RayDesc r;
    r.Origin = wp + wg * (g_p0.y + length(p) * 0.0015);
    r.Direction = dir;
    r.TMin = 0.0;
    r.TMax = 1000.0;
    vis += tb_occluded(Scene, g_u0.x & 0xff, r, g_u1.y != 0) ? 0.0 : 1.0;
  }
  u_a[px] = float2(vis / float(spp), p.z);
}

// Validation: a primary ray per pixel, its hit distance against the G-buffer's. Bins (x2: pixels
// whose normal-buffer alpha is 1, the foliage material term, in bins 8..15): 0 sky + miss, 1 sky +
// hit, 2 geometry + miss, 3 |err| < 0.2 %, 4 < 1 %, 5 < 5 %, 6 ray nearer by > 5 %, 7 farther.
[numthreads(8, 8, 1)] void cs_val(uint3 id : SV_DispatchThreadID) {
  int2 px = int2(id.xy);
  if (any(px >= int2(g_p1.xy))) return;
  float z = t_depth[px];
  bool sky = !is_geo(z);
  float3 p = view_pos(px, sky ? 0.99999 : z);
  RayDesc r;
  r.Origin = cam_pos();
  r.Direction = normalize(mul((float3x3)g_v2w, p));
  r.TMin = 0.0;
  r.TMax = g_p2.y;
  float t;
  uint inst;
  bool hit = tb_closest(Scene, g_u0.x & 0xff, r, g_u1.y != 0, t, inst);
  float d = length(p);
  uint bin;
  float4 c;
  if (sky) {
    bin = hit ? 1 : 0;
    c = hit ? float4(0.5, 0, 0.5, 1) : float4(0, 0, 0, 1);
  } else if (!hit) {
    bin = 2;
    c = float4(1, 0, 1, 1);
  } else {
    float e = (t - d) / d, ae = abs(e);
    if (ae < 0.002) { bin = 3; c = float4(0, 0.8, 0, 1); }
    else if (ae < 0.01) { bin = 4; c = float4(0.5, 1, 0.5, 1); }
    else if (ae < 0.05) { bin = 5; c = float4(1, 1, 0, 1); }
    else if (e < 0) { bin = 6; c = float4(1, 0, 0, 1); }
    else { bin = 7; c = float4(0, 0.3, 1, 1); }
  }
  if (!sky && t_normal[px].a > 0.99) bin += 8;
  uint dummy;
  u_stats.InterlockedAdd(bin * 4, 1, dummy);
  if (g_u0.z) u_vis[px] = c;
}

// ---- denoiser ------------------------------------------------------------------------------------

[numthreads(8, 8, 1)] void cs_temporal(uint3 id : SV_DispatchThreadID) {
  int2 px = int2(id.xy);
  if (any(px >= int2(g_p1.xy))) return;
  float2 raw = t_a[px];
  float z = t_depth[px];
  if (!is_geo(z)) { u_a[px] = float2(1, 1); u_b[px] = 1e4; return; }
  float3 p = view_pos(px, z);
  float n = 1, ao = raw.x;
  if (g_u0.w != 0) {
    float3 wp = mul(g_v2w, float4(p, 1)).xyz;
    // A pixel on a moving instance (the ferry, a car, a door): where its surface point was at the
    // previous update, through that instance's motion (previous transform * inverse current).
    uint inst = t_inst[px];
    if (g_u1.x != 0 && inst != 0xffffffffu) {
      float4 r0 = asfloat(g_motion.Load4(inst * 48)), r1 = asfloat(g_motion.Load4(inst * 48 + 16)),
             r2 = asfloat(g_motion.Load4(inst * 48 + 32));
      float4 w4 = float4(wp, 1);
      wp = float3(dot(r0, w4), dot(r1, w4), dot(r2, w4));
    }
    float4 pc = mul(g_w2pc, float4(wp, 1));
    if (pc.w > 0.01) {
      float2 uv = pc.xy / pc.w * float2(0.5, -0.5) + 0.5;
      if (all(uv > 0) && all(uv < 1)) {
        float pz = t_c.SampleLevel(s_pt, uv, 0);
        if (abs(pz - pc.w) < 0.04 * pc.w + 0.03) {
          float2 h = t_b.SampleLevel(s_lin, uv, 0);
          n = min(h.y + 1, g_p0.w);
          ao = lerp(h.x, raw.x, 1.0 / n);
        }
      }
    }
  }
  u_a[px] = float2(ao, n);
  u_b[px] = p.z;
}

static const float kW[3] = {0.375, 0.25, 0.0625};
[numthreads(8, 8, 1)] void cs_atrous(uint3 id : SV_DispatchThreadID) {
  int2 px = int2(id.xy);
  if (any(px >= int2(g_p1.xy))) return;
  float2 c = t_a[px];
  uint stage = g_u0.y >> 8;  // 1 intermediate, 2 final AO (u0, R16G16), 3 final sun (u3, BGRA8)
  if (!is_geo(t_depth[px])) {
    if (stage == 3) u_vis[px] = float4(1, 1, 1, 1);
    else u_a[px] = stage == 2 ? float2(1, 1) : c;
    return;
  }
  float zc = t_c[px];
  float3 nc = t_normal[px].xyz * 2 - 1;
  int step = int(g_u0.z);
  float sigz = 0.02 * zc + 0.02;
  float sum = 0, ws = 0;
  for (int dy = -2; dy <= 2; dy++)
    for (int dx = -2; dx <= 2; dx++) {
      int2 q = clampi(px + int2(dx, dy) * step);
      float zq = t_c[q];
      float3 nq = t_normal[q].xyz * 2 - 1;
      float w = kW[abs(dx)] * kW[abs(dy)] * exp(-abs(zq - zc) / sigz) * pow(saturate(dot(nc, nq)), 8.0);
      sum += t_a[q].x * w;
      ws += w;
    }
  float ao = ws > 1e-5 ? sum / ws : c.x;
  if (stage == 3) {
    u_vis[px] = float4(saturate(ao), 1, 1, 1);
  } else if (stage == 2) {
    ao = pow(saturate(ao), g_p2.x);
    u_a[px] = float2(ao, ao);
  } else {
    u_a[px] = float2(ao, c.y);
  }
}
