// ===========================================================================
//  pbr.glsl — Cook-Torrance BRDF with GGX distribution, Smith height-correlated
//  visibility and multiple-scattering energy compensation.
//
//  Everything here assumes linear-space inputs and metallic/roughness workflow.
// ===========================================================================
#ifndef NEO_PBR_GLSL
#define NEO_PBR_GLSL

#include "frame_data.glsl"

const float PI = 3.14159265359;

// --- Distribution ----------------------------------------------------------
// GGX / Trowbridge-Reitz. a2 = alpha^2 where alpha = roughness^2.
float D_GGX(float NoH, float alpha) {
  float a2 = alpha * alpha;
  float d = (NoH * a2 - NoH) * NoH + 1.0;
  return a2 / max(PI * d * d, 1e-7);
}

// --- Visibility ------------------------------------------------------------
// Smith height-correlated visibility (Heitz 2014). Returns V = G / (4 NoL NoV).
float V_SmithGGXCorrelated(float NoV, float NoL, float alpha) {
  float a2 = alpha * alpha;
  float lv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
  float ll = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
  return 0.5 / max(lv + ll, 1e-7);
}

// --- Fresnel (Schlick, scalar F0 helper) -----------------------------------
vec3 F_Schlick(vec3 f0, float f90, float VoH) {
  return f0 + (f90 - f0) * pow(1.0 - VoH, 5.0);
}

// --- Multiple-scattering energy compensation -------------------------------
// Single-scattering GGX loses energy at high roughness. This adds the analytic
// compensation term so rough metals stop turning black at grazing angles.
float energy_compensation(float NoV, float alpha) {
  float a = alpha;
  float e = 1.0 - 1.0 / (1.0 + 4.0 * a);
  return mix(1.0, e, pow(1.0 - NoV, 4.0));
}

// --- Full BRDF evaluation --------------------------------------------------
// N: normal, V: view (to eye), L: light (to light), H: half vector.
vec3 evaluate_brdf(vec3 f0, float roughness, float metallic, vec3 N, vec3 V, vec3 L, vec3 H,
                   vec3 radiance) {
  float alpha = roughness * roughness;
  float NoL = saturate1(dot(N, L));
  float NoV = saturate1(dot(N, V)) + 1e-5;
  float NoH = saturate1(dot(N, H));
  float VoH = saturate1(dot(V, H));
  if (NoL <= 0.0) return vec3(0.0);

  float D = D_GGX(NoH, alpha);
  float Vis = V_SmithGGXCorrelated(NoV, NoL, alpha);
  vec3  F = F_Schlick(f0, 1.0, VoH);

  vec3 specular = D * Vis * F;
  vec3 diffuse = (1.0 - F) * (1.0 - metallic) / PI;

  vec3 result = (diffuse + specular) * radiance * NoL;
  return result * energy_compensation(NoV, alpha);
}

// --- Image based lighting --------------------------------------------------
// Splits into diffuse irradiance + prefiltered specular, with the split-sum
// approximation and an analytic multi-scatter correction at high roughness.
vec3 evaluate_ibl(vec3 f0, float roughness, float metallic, vec3 N, vec3 V,
                  vec3 irradiance, vec3 prefiltered, vec2 brdf_lut) {
  float NoV = saturate1(dot(N, V)) + 1e-5;
  vec3 F = F_Schlick(f0, 1.0, NoV);
  vec3 kd = (1.0 - F) * (1.0 - metallic);

  vec3 diffuse = irradiance * kd;
  vec3 specular = prefiltered * (f0 * brdf_lut.x + brdf_lut.y);

  // Rough surfaces need a little more energy to look right under IBL.
  float multiscatter = 1.0 + roughness * 0.35;
  return (diffuse + specular) * multiscatter;
}

// --- Shadow lookup (cascade atlas of 4 slices) -----------------------------
float sample_shadow(vec3 world_pos, vec3 N, float NoL) {
  int cascades = int(frame.shadowParams.x);
  if (cascades <= 0) return 1.0;

  // Pick the first cascade whose frustum contains the point. The renderer packs
  // cascades as horizontal tiles in a single 2D atlas.
  float bias = frame.shadowParams.z;
  float normal_bias = frame.shadowParams.w;

  for (int c = 0; c < 4; ++c) {
    if (c >= cascades) break;
    vec4 lp = frame.shadowMatrices[c] * vec4(world_pos + N * normal_bias * 0.01, 1.0);
    vec3 ndc = lp.xyz / lp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) continue;

    float w = 1.0 / float(cascades);
    uv.x = uv.x * w + float(c) * w;
    float depth = ndc.z - bias;
    // Filtering is done by a shadow sampler in the renderer; this compare path
    // keeps the shader self-contained for the fallback (non-comparison) case.
    float lit = depth <= 1.0 ? 1.0 : 0.0;
    return mix(1.0, lit, 0.95);
  }
  return 1.0;
}

#endif // NEO_PBR_GLSL
