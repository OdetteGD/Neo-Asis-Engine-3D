#version 450
// ===========================================================================
//  lighting.frag — deferred PBR shading + IBL + shadows + atmosphere + clouds.
//
//  Reads the G-buffer, evaluates every light in the (clustered) list, applies
//  image-based lighting, then composites the atmospheric background where the
//  depth buffer says "nothing was drawn" and blends the volumetric cloud layer.
//
//  Cost note for mobile: the light loop is bounded to 8 lights per pixel in
//  the low tier via `push.flags.x`; the cluster grid keeps typical counts ≤ 4.
// ===========================================================================

#extension GL_GOOGLE_include_directive : require
#include "common/frame_data.glsl"
#include "common/pbr.glsl"
#include "common/atmosphere.glsl"

layout(set = 1, binding = 0) uniform sampler2D uGBufferAlbedo;
layout(set = 1, binding = 1) uniform sampler2D uGBufferNormal;
layout(set = 1, binding = 2) uniform sampler2D uGBufferEmissive;
layout(set = 1, binding = 3) uniform sampler2D uDepth;
layout(set = 1, binding = 4) uniform sampler2D uClouds;
layout(set = 1, binding = 5) uniform sampler2D uIrradiance;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform LightingPC {
  uvec4 flags;        // x = light count, y = invert Y, z = atmosphere, w = clouds
  vec4  rotation;     // surface pre-rotation
  vec4  params;       // x = render scale, y = bloom, z = cascades, w = spare
} push;

void main() {
  const vec4 albedo_ao = texture(uGBufferAlbedo, vUV);
  const vec4 normal_rough_metal = texture(uGBufferNormal, vUV);
  const vec4 emissive = texture(uGBufferEmissive, vUV);
  const float depth = texture(uDepth, vUV).r;

  const vec3 N = oct_decode(normal_rough_metal.xy);
  const float roughness = normal_rough_metal.z;
  const float metallic = normal_rough_metal.w;
  const float occlusion = albedo_ao.a;
  const vec3 albedo = albedo_ao.rgb;

  const vec3 f0 = mix(vec3(0.04), albedo, metallic);

  // --- Sky / background ---------------------------------------------------
  vec3 background = vec3(0.0);
  if (push.flags.z != 0u) {
    vec4 near_clip = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
    vec4 far_clip = vec4(vUV * 2.0 - 1.0, 1.0, 1.0);
    vec4 nw = frame.invViewProj * near_clip;
    vec4 fw = frame.invViewProj * far_clip;
    vec3 dir = normalize(fw.xyz / fw.w - nw.xyz / nw.w);

    float transmittance = 1.0;
    background = atmosphere_scattering(frame.cameraPosition.xyz, dir, 14, transmittance);
    background += sun_disk(dir) * transmittance;
  }

  // --- Nothing was rasterised here: sky + clouds only ---------------------
  if (depth <= 0.0) {
    vec3 col = background;
    if (push.flags.w != 0u) {
      vec4 clouds = texture(uClouds, vUV);
      col = col * (1.0 - clouds.a) + clouds.rgb;
    }
    outColor = vec4(col, 1.0);
    return;
  }

  // --- Surface reconstruction --------------------------------------------
  const vec3 world_pos = view_position_from_depth(vUV, depth);
  const vec3 V = normalize(frame.cameraPosition.xyz - world_pos);
  const float NoV = saturate1(dot(N, V));

  vec3 direct = vec3(0.0);
  const uint light_count = min(push.flags.x, 64u);

  for (uint i = 0u; i < light_count; ++i) {
    const GpuLight L = lights[i];
    const int type = int(L.params.x);
    vec3 Ldir;
    float attenuation = 1.0;
    float shadow = 1.0;

    if (type == NEO_LIGHT_DIRECTIONAL) {
      Ldir = L.directionAngle.xyz;
    } else {
      vec3 to_light = L.positionRange.xyz - world_pos;
      float dist2 = dot(to_light, to_light);
      float radius2 = L.positionRange.w * L.positionRange.w;
      if (dist2 > radius2) continue;

      Ldir = normalize(to_light);
      // Inverse-square with a smooth window at the falloff radius, so lights
      // never pop out at the edge of their range.
      float window = saturate1(1.0 - dist2 / radius2);
      attenuation = window * window / max(dist2, 1e-4);

      if (type == NEO_LIGHT_SPOT) {
        float cos_angle = dot(-Ldir, L.directionAngle.xyz);
        float outer = L.directionAngle.w;
        float inner = L.params.z;
        attenuation *= smoothstep(outer, max(inner, outer + 1e-3), cos_angle);
      }
    }

    float NoL = saturate1(dot(N, Ldir));
    if (NoL <= 0.0) continue;

    if (type == NEO_LIGHT_DIRECTIONAL) {
      shadow = sample_shadow(world_pos, N, NoL);
    }

    vec3 radiance = L.colorIntensity.rgb * L.colorIntensity.w * attenuation * NoL;
    vec3 H = normalize(V + Ldir);

    direct += evaluate_brdf(f0, roughness, metallic, N, V, Ldir, H, radiance) * shadow;
  }

  // --- Sun via the directional light -------------------------------------
  // The atmosphere already carries the sun colour; the explicit directional
  // term gives crisp contact shadows and specular highlights.
  {
    const vec3 Ldir = frame.sunDirection.xyz;
    const float NoL = saturate1(dot(N, Ldir));
    if (NoL > 0.0) {
      const vec3 H = normalize(V + Ldir);
      // Sun radiance drops as it approaches the horizon (longer air path).
      const float horizon_fade = smoothstep(-0.05, 0.25, Ldir.y);
      const vec3 radiance = frame.sunColor.rgb * frame.sunDirection.w * horizon_fade;
      const float shadow = sample_shadow(world_pos, N, NoL);
      direct += evaluate_brdf(f0, roughness, metallic, N, V, Ldir, H, radiance) * shadow;
    }
  }

  // --- Ambient / IBL ------------------------------------------------------
  vec3 irradiance = texture(uIrradiance, vec3(N.x, N.z, 0.0).xy * 0.5 + 0.5).rgb;
  if (length(irradiance) < 1e-4) {
    // Fallback when the baker has not run yet: hemisphere ambient.
    irradiance = mix(vec3(0.05, 0.06, 0.08), vec3(0.28, 0.36, 0.5), N.y * 0.5 + 0.5);
  }

  // Split-sum lookup uses a compact analytic fit for the BRDF LUT (r, g) with
  // no texture fetch, which is measurably cheaper on tile-based GPUs.
  float NoV4 = NoV * NoV * NoV * NoV;
  float a = roughness;
  float brdf_x = mix(1.0, 1.0 - a * 0.72, NoV4);
  float brdf_y = a * 0.28 * (1.0 - NoV4);

  vec3 ambient = evaluate_ibl(f0, roughness, metallic, N, V, irradiance, irradiance,
                              vec2(brdf_x, brdf_y));
  ambient *= occlusion;

  vec3 color = direct + ambient + emissive.rgb;

  // --- Aerial perspective -------------------------------------------------
  color = apply_aerial_perspective(color, world_pos, -V);

  // --- Volumetric clouds in front of the surface --------------------------
  if (push.flags.w != 0u) {
    // Recomposite: clouds are rendered as a translucent layer between the
    // camera and this pixel when the cloud depth is nearer than the surface.
    vec4 clouds = texture(uClouds, vUV);
    float cloud_depth = clouds.a;  // density used as a cheap ordering proxy
    color = color * (1.0 - cloud_depth * 0.85) + clouds.rgb;
  }

  outColor = vec4(color, 1.0);
}
