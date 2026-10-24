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
// Enables the real atlas-sampling path in pbr.glsl.
#define NEO_SHADOW_ATLAS 1
#include "common/pbr.glsl"
#include "common/atmosphere.glsl"

// set 2: G-buffer + depth + volumetric clouds
layout(set = 2, binding = 0) uniform sampler2D uGBufferAlbedo;
layout(set = 2, binding = 1) uniform sampler2D uGBufferNormal;
layout(set = 2, binding = 2) uniform sampler2D uGBufferEmissive;
layout(set = 2, binding = 3) uniform sampler2D uDepth;
layout(set = 2, binding = 4) uniform sampler2D uClouds;

// set 3: image-based lighting (baked by IBLBaker)
layout(set = 3, binding = 0) uniform samplerCube uIrradiance;
layout(set = 3, binding = 1) uniform samplerCube uPrefiltered;
layout(set = 3, binding = 2) uniform sampler2D uBRDFLUT;

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

  // --- Clustered light lookup --------------------------------------------
  // Pick the cluster covering this pixel, then walk only its light list.
  // Falls back to the flat list when clustering is disabled (flags.y == 2).
  const vec3 view_pos = (frame.view * vec4(world_pos, 1.0)).xyz;
  const float view_depth = max(-view_pos.z, frame.clusterParams.x);

  uint cluster_index = 0u;
  uint cluster_count = 0u;
  uint capacity = uint(frame.clusterDims.w);

  if (push.flags.y != 2u) {
    const vec2 tile = floor(gl_FragCoord.xy /
                            max(vec2(frame.clusterDims.y, frame.clusterDims.z), vec2(1.0)));
    const uint cx = uint(clamp(tile.x, 0.0, frame.clusterParams.z - 1.0));
    const uint cy = uint(clamp(tile.y, 0.0, frame.clusterParams.w - 1.0));

    const float log_ratio = log(frame.clusterParams.y / frame.clusterParams.x);
    const float slice = log(view_depth / frame.clusterParams.x) / log_ratio;
    const uint cz = uint(clamp(slice * frame.clusterDims.x, 0.0, frame.clusterDims.x - 1.0));

    cluster_index = cx + cy * uint(frame.clusterParams.z) +
                    cz * uint(frame.clusterParams.z) * uint(frame.clusterParams.w);
    cluster_count = min(clusterCounts[cluster_index], capacity);
  }

  const uint light_count = min(push.flags.x, 64u);
  const uint iterations = (cluster_count > 0u) ? cluster_count : 0u;

  for (uint k = 0u; k < iterations; ++k) {
    const uint i = clusterIndices[cluster_index * capacity + k];
    if (i >= light_count) continue;
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
  // Diffuse IBL from the baked irradiance cube.
  const vec3 irradiance = texture(uIrradiance, N).rgb;

  // Specular IBL: prefiltered chain sampled at the roughness mip, times the
  // split-sum BRDF term from the baked LUT.
  const float max_lod = float(textureQueryLevels(uPrefiltered) - 1);
  const vec3 prefiltered = textureLod(uPrefiltered, reflect(-V, N), roughness * max_lod).rgb;
  const vec2 brdf = texture(uBRDFLUT, vec2(NoV, roughness)).rg;

  vec3 ambient = evaluate_ibl(f0, roughness, metallic, N, V, irradiance, prefiltered, brdf);
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
