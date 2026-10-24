#version 450
// ===========================================================================
//  gbuffer.vert — geometry into the deferred G-buffer.
//
//  The vertex stream is a storage buffer (bindless-friendly) so the same
//  pipeline serves every mesh without rebinding vertex buffers. Per-object data
//  arrives as push constants.
// ===========================================================================

#extension GL_GOOGLE_include_directive : require
#include "common/frame_data.glsl"

// 112 bytes — must match neo::DrawConstants. Mali caps push constants at 128.
layout(push_constant) uniform DrawPC {
  mat4  model;
  vec4  baseColorFactor;
  vec4  materialParams;   // x = metallic, y = roughness, z = emissive, w = occlusion
  uvec4 textureIndices;   // x = albedo, y = normal, z = alpha test, w = double sided
} obj;

// Packed vertex: position (3xf32), normal (oct-encoded 2xf32), uv (2xf32)
layout(std430, binding = 2) readonly buffer VertexBlock {
  float vertices[];
};

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec3 vTangent;
layout(location = 4) out float vViewDepth;

void main() {
  // 8 floats per vertex.
  uint base = uint(gl_VertexIndex) * 8u;
  vec3 position = vec3(vertices[base + 0], vertices[base + 1], vertices[base + 2]);
  vec2 nrm_oct = vec2(vertices[base + 3], vertices[base + 4]);
  vec2 uv = vec2(vertices[base + 5], vertices[base + 6]);

  vec4 world = obj.model * vec4(position, 1.0);
  vWorldPos = world.xyz;

  // The normal matrix is derived here instead of being shipped in push
  // constants; that is what keeps the block inside the 128-byte budget.
  mat3 normal_matrix = transpose(inverse(mat3(obj.model)));

  vec3 normal = oct_decode(nrm_oct);
  vNormal = normalize(normal_matrix * normal);
  vTangent = normalize(normal_matrix * normalize(cross(normal, vec3(0, 1, 0))));

  vUV = uv;

  vec4 view_pos = frame.view * world;
  vViewDepth = -view_pos.z;

  gl_Position = frame.viewProj * world;
}
