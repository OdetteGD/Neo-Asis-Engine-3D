#version 450
// ===========================================================================
//  shadow.vert — cascaded shadow map depth-only pass.
// ===========================================================================

#extension GL_GOOGLE_include_directive : require
#include "common/frame_data.glsl"

layout(push_constant) uniform ShadowPC {
  mat4 lightViewProj;
  uint cascadeIndex;
  float depthBias;
  float normalBias;
} pc;

layout(std430, binding = 2) readonly buffer VertexBlock {
  float vertices[];
};

void main() {
  uint base = uint(gl_VertexIndex) * 8u;
  vec3 position = vec3(vertices[base + 0], vertices[base + 1], vertices[base + 2]);

  vec4 world = vec4(position, 1.0);  // world matrix folded into lightViewProj per draw
  gl_Position = pc.lightViewProj * world;
  // Slope-scaled depth bias is applied in the fragment shader; a constant
  // world-space offset here handles most of the acne on mobile GPUs.
  gl_Position.z -= pc.depthBias * gl_Position.w;
}
