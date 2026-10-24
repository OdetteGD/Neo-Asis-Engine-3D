#version 450
// ===========================================================================
//  shadow.frag — alpha-tested shadow casters only.
//  Depth-only pipelines normally use a null fragment shader; this one exists
//  so foliage cards can discard transparent texels and still cast shadows.
// ===========================================================================

layout(set = 1, binding = 0) uniform sampler2D uBindlessTextures[64];

layout(location = 0) in vec2 vUV;
layout(push_constant) uniform ShadowPC {
  mat4 lightViewProj;
  uint cascadeIndex;
  float depthBias;
  float normalBias;
} pc;

void main() {
  // Material slot 0 means "no alpha test needed" and the draw would not have
  // been dispatched to this pipeline at all.
  if (texture(uBindlessTextures[0], vUV).a < 0.5) discard;
}
