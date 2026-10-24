#version 450
// ===========================================================================
//  gbuffer.frag — writes the deferred G-buffer.
//
//  Targets:
//    0: albedo.rgb + ambient occlusion (a)
//    1: octahedral normal.xy + roughness (b) + metallic (a)
//    2: emissive (B10G11R11 packed — 32 bits of HDR for free)
//
//  Texture sampling goes through the bindless array in descriptor set 1. Until
//  a material has a texture assigned, index 0 resolves to a 1x1 white default,
//  so the multiply is always safe.
// ===========================================================================

#extension GL_GOOGLE_include_directive : require
#include "common/frame_data.glsl"

layout(set = 1, binding = 0) uniform sampler2D uBindlessTextures[64];

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec3 vTangent;
layout(location = 4) in float vViewDepth;

layout(push_constant) uniform DrawPC {
  mat4 model;
  mat4 normalMatrix;
  vec4 baseColorFactor;
  vec4 materialParams;   // x = metallic, y = roughness, z = emissive, w = occlusion
  vec4 uvScaleOffset;
  uvec4 textureIndices;
  vec4 flags;            // x = alpha test, y = double sided
} obj;

layout(location = 0) out vec4 outAlbedoAO;
layout(location = 1) out vec4 outNormalRoughMetal;
layout(location = 2) out vec4 outEmissive;

void main() {
  vec4 texel = vec4(1.0);
  if (obj.textureIndices.x != 0u) {
    texel = texture(uBindlessTextures[obj.textureIndices.x], vUV);
  }

  // Alpha-tested foliage/fences: discard before any writes.
  if (obj.flags.x > 0.5 && texel.a < 0.5) discard;

  vec3 albedo = obj.baseColorFactor.rgb * texel.rgb;

  vec3 N = normalize(vNormal);
  if (!gl_FrontFacing) N = -N;

  float roughness = clamp(obj.materialParams.y, 0.04, 1.0);
  float metallic = clamp(obj.materialParams.x, 0.0, 1.0);
  float occlusion = obj.materialParams.w;

  // Geometric specular anti-aliasing: clamp roughness to the normal variance
  // across the pixel footprint so specular highlights stop shimmering.
  vec3 dNdx = dFdx(N);
  vec3 dNdy = dFdy(N);
  float variance = 0.25 * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
  roughness = max(roughness, sqrt(clamp(variance, 0.0, 1.0)));

  outAlbedoAO = vec4(albedo, occlusion);
  outNormalRoughMetal = vec4(oct_encode(N), roughness, metallic);
  outEmissive = vec4(albedo * obj.materialParams.z, 1.0);
}
