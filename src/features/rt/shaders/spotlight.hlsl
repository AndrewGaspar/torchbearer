// The game's shadowed spot light pixel shader (DeferredLight, PS aa17e58e1bd8d853, "spotlightPS":
// the flashlight and the other shadow-casting spots), re-written in HLSL from its DXIL disassembly
// (docs/raytracing.md sections 11 and 14). Binding-compatible with the original: $Globals at b0
// (only the fields it reads, at their offsets), t0..t5 / s0..s5 as the original's.
//
//   TB_SHADOW_PCF  the original 10-tap Vogel-disk PCF on the shadow atlas (to verify the port)
//   TB_SHADOW_NONE unshadowed (debugging the port)
//   TB_SHADOW_RQ   [graphics] flashlight_shadows = rt: the flashlight's shadow by inline ray queries
//                  (DXR 1.1, ps_6_5) against the mod's TLAS towards a disc the size of its bulb
//                  (soft penumbra), alpha-tested foliage included; every other shadowed spot keeps
//                  the PCF on the atlas. Extra bindings, appended to every root signature at
//                  creation while the RT flashlight is on (census.cpp rs_extend): t0 space9 TLAS,
//                  b0 space9 constants, t1 space9 the alpha buffer, t0.. space10 the game's own
//                  descriptor heap (the alpha textures by heap index), s0 space9 a wrap sampler.
//
// Built at dev time with a current DXC (scripts/build_shaders), embedded as DXIL.

cbuffer Globals : register(b0) {
  row_major float4x4 g_mViewToWorld : packoffset(c4);
  float2 g_vRenderRes : packoffset(c26.z);
  float2 g_vShadowMapRes : packoffset(c29.z);
  float g_fPCFSize_d3t : packoffset(c31.y);
  row_major float4x4 g_mLightProjectionMatrix : packoffset(c90);
  float3 g_vLightPosition : packoffset(c114);
  float3 g_vLightColor : packoffset(c116);
  float4 g_vLightFalloff : packoffset(c117);
  row_major float4x4 g_mShadowProjectionMatrix : packoffset(c119);
  float g_fShadowLOD : packoffset(c123);
};

Texture2D<float4> g_sLinearDepthBufferTex : register(t0);
Texture2D<float4> g_sNormalBufferTex : register(t1);
Texture2D<float4> g_sTangentGlossBufferTex : register(t2);
Texture2D<float4> g_sExposureTextureTex : register(t3);
Texture2D<float4> g_sShadowMapTex : register(t4);
Texture2D<float4> g_sLightProjectionMapTex : register(t5);
SamplerState g_sLinearDepthBuffer : register(s0);
SamplerState g_sNormalBuffer : register(s1);
SamplerState g_sTangentGlossBuffer : register(s2);
SamplerState g_sExposureTexture : register(s3);
SamplerComparisonState g_sComparisonSampler : register(s4);
SamplerState g_sLightProjectionMap : register(s5);

#ifdef TB_SHADOW_RQ
RaytracingAccelerationStructure g_tlas : register(t0, space9);
cbuffer TbRt : register(b0, space9) {
  float4 tb_anchor;  // xyz: the TLAS origin in world space; w: instance mask
  float4 tb_p;       // x: normal bias (m), y: bulb radius (m), z: rays, w: distance kept clear at the light (m)
  float4 tb_q;       // x: alpha-tested geometry on, y: flashlight: max distance from the eye (m),
                     // z: min cos(light axis, view axis), w: 1 = every shadowed spot (dev)
};
ByteAddressBuffer g_tb_alpha : register(t1, space9);
Texture2D<float4> g_tb_heap[] : register(t0, space10);
SamplerState g_tb_wrap : register(s0, space9);
#define TB_ALPHA_BUF g_tb_alpha
#define TB_ALPHA_TEX g_tb_heap
#define TB_ALPHA_SMP g_tb_wrap
#define TB_ALPHA_GAME_HEAP 1
#include "alpha.hlsli"
#endif

static const float kInvPi = 0.31830987;

float shadow_pcf(float3 vp, float2 spos) {
  float4 sp = mul(g_mShadowProjectionMatrix, float4(vp, 1));
  float3 s = sp.xyz / sp.w;
  float ang = frac(52.9829189 * frac(dot(spos, float2(0.06711056, 0.00583715)))) * 6.2831855;
  float2 sc = g_fPCFSize_d3t * 0.5 / g_vShadowMapRes;
  float sum = 0;
  [unroll] for (int i = 0; i < 10; i++) {
    float r = sqrt((float(i) + 0.5) / 10.0);
    float a = ang + 2.4 * float(i);
    float2 o = float2(sc.x * r * cos(a), sc.y * r * sin(a));
    sum += g_sShadowMapTex.SampleCmpLevelZero(g_sComparisonSampler, s.xy + o, s.z);
  }
  return sum * 0.1;
}

#ifdef TB_SHADOW_RQ
// The player's flashlight among the shadowed spots: near the eye and aimed along the view (its
// beam follows the aim). g_mLightProjectionMatrix's w row is the light's forward axis (view space).
bool is_flashlight() {
  if (tb_q.w != 0) return true;
  float3 f = normalize(g_mLightProjectionMatrix[3].xyz);
  return length(g_vLightPosition) < tb_q.y && f.z > tb_q.z;
}

// Rays from the surface towards points on the bulb's disc (radius tb_p.y) around the light's
// position: an area light, so the penumbra widens with the occluder's distance from the receiver
// (crisp contact shadows near, soft far). Interleaved-gradient rotation per pixel, as the game's
// PCF; its TAA resolves the noise.
float shadow_rq(float3 vp, float3 n, float2 spos) {
  float3 wp = mul(g_mViewToWorld, float4(vp, 1)).xyz - tb_anchor.xyz;
  float3 wn = normalize(mul((float3x3)g_mViewToWorld, n));
  float3 wl = mul(g_mViewToWorld, float4(g_vLightPosition, 1)).xyz - tb_anchor.xyz;
  float ang = frac(52.9829189 * frac(dot(spos, float2(0.06711056, 0.00583715)))) * 6.2831855;
  uint rays = max(uint(tb_p.z), 1u);
  float3 axis = normalize(wl - wp);
  float3 t = normalize(abs(axis.y) < 0.99 ? cross(axis, float3(0, 1, 0)) : cross(axis, float3(1, 0, 0)));
  float3 b = cross(axis, t);
  float3 o = wp + wn * (tb_p.x + length(vp) * 0.001);
  float lit = 0;
  for (uint i = 0; i < rays; i++) {
    float r = tb_p.y * sqrt((float(i) + 0.5) / float(rays));
    float a = ang + 2.4 * float(i);
    float3 d = wl + (t * cos(a) + b * sin(a)) * r - o;
    float len = length(d);
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d / len;
    ray.TMin = 0.0;
    ray.TMax = max(len - tb_p.w, 0.0);
    lit += tb_occluded(g_tlas, uint(tb_anchor.w), ray, tb_q.x != 0) ? 0.0 : 1.0;
  }
  return lit / float(rays);
}
#endif

float4 main(float4 pos : SV_Position, float3 ray : TEXCOORD0) : SV_Target {
  float2 uv = pos.xy / g_vRenderRes;
  float3 n = g_sNormalBufferTex.Sample(g_sNormalBuffer, uv).xyz * 2 - 1;
  float4 tg = g_sTangentGlossBufferTex.Sample(g_sTangentGlossBuffer, uv);
  float depth = g_sLinearDepthBufferTex.Sample(g_sLinearDepthBuffer, uv).x;
  float3 vp = ray * depth / ray.z;
  float3 N = normalize(n);
  float3 toL = g_vLightPosition - vp;
  float3 L = normalize(toL);
  float att = saturate(exp2(length(toL) * -1.442695 * g_vLightFalloff.x) - g_vLightFalloff.y);
  float3 col = att * g_vLightColor;
  float4 lp = mul(g_mLightProjectionMatrix, float4(vp, 1));
  float2 luv = float2(lp.x / lp.w * 0.5 + 0.5, 0.5 - lp.y / lp.w * 0.5);
  col *= g_sLightProjectionMapTex.Sample(g_sLightProjectionMap, luv).xyz;
#ifdef TB_SHADOW_RQ
  float sh = is_flashlight() ? shadow_rq(vp, N, pos.xy) : shadow_pcf(vp, pos.xy);
#elif defined(TB_SHADOW_NONE)
  float sh = 1;
#else
  float sh = shadow_pcf(vp, pos.xy);
#endif
  float3 lit = col * (sh * g_fShadowLOD + (1 - g_fShadowLOD));
  float3 V = normalize(-vp);
  float NdotL = dot(N, L);
  float diffuse, spec;
  float gloss = tg.w;
  if (any(tg.xyz != 0)) {  // anisotropic (Ward)
    float3 B = tg.xyz;
    float3 Bv = normalize(float3(N.z * B.y - N.y * B.z, N.x * B.z - N.z * B.x, N.y * B.x - N.x * B.y));
    float NdotV = dot(N, V);
    spec = 0;
    if (NdotL >= 0 && NdotV >= 0) {
      float ax = gloss * 255.0;
      float3 H = normalize(V + L);
      float NdotH = dot(N, H);
      float BdotH = dot(Bv, H);
      float TdotH = dot(tg.xyz, H);
      float k = sqrt(NdotL / max(NdotV, 0.02)) / (gloss * 640.8835);
      float e = (pow(abs(TdotH / ax), 2.0) + pow(abs(BdotH * 5.0), 2.0)) / (NdotH + 1.0);
      spec = k * kInvPi * exp2(e * -2.88539);
    }
    diffuse = saturate(NdotL);
  } else {  // Phong-like
    float has = gloss != 0 ? 1.0 : 0.0;
    float ex = (max(5.0, exp2(log2(saturate(gloss)) * 3.0) * 3000.0) - 40.0) * has + 40.0;
    float3 H = normalize(V + L);
    spec = exp2(log2(max(dot(N, H), 0.0)) * ex) * (has * kInvPi);
    diffuse = saturate(NdotL);
  }
  float3 dif = diffuse * kInvPi * lit;
  float sl = dot(lit, float3(0.2126, 0.7152, 0.0722)) * spec;
  float ex = g_sExposureTextureTex.Sample(g_sExposureTexture, float2(0, 0)).x;
  return min(float4(dif * ex, sl * ex), 32.0);
}
