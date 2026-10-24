#version 450
// ===========================================================================
//  bloom_up.frag — 9-tap tent upsample, additively combined with the
//  corresponding downsample mip. This is the "dual filtering" bloom that keeps
//  wide, soft glows without the banding of a single Gaussian chain.
// ===========================================================================

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform sampler2D uSource;   // smaller mip
layout(set = 1, binding = 1) uniform sampler2D uCurrent;  // same-size mip

layout(push_constant) uniform BloomPC {
  vec4 texel;
  vec4 params;   // x = radius, y = intensity
} pc;

void main() {
  vec2 texel = pc.texel.xy * pc.params.x;

  // 3x3 tent filter.
  vec3 color = texture(uSource, vUV + vec2(-texel.x, -texel.y)).rgb * 0.0625;
  color += texture(uSource, vUV + vec2( 0.0, -texel.y)).rgb * 0.125;
  color += texture(uSource, vUV + vec2( texel.x, -texel.y)).rgb * 0.0625;
  color += texture(uSource, vUV + vec2(-texel.x,  0.0)).rgb * 0.125;
  color += texture(uSource, vUV).rgb * 0.25;
  color += texture(uSource, vUV + vec2( texel.x,  0.0)).rgb * 0.125;
  color += texture(uSource, vUV + vec2(-texel.x,  texel.y)).rgb * 0.0625;
  color += texture(uSource, vUV + vec2( 0.0,  texel.y)).rgb * 0.125;
  color += texture(uSource, vUV + vec2( texel.x,  texel.y)).rgb * 0.0625;

  vec3 existing = texture(uCurrent, vUV).rgb;
  outColor = vec4(existing + color * pc.params.y, 1.0);
}
