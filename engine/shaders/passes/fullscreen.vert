#version 450
// ===========================================================================
//  fullscreen.vert — generates a full-screen triangle with no vertex buffer.
//  Three vertices cover the whole viewport; faster than a quad (one less
//  vertex, no diagonal seam, and better quad-shading alignment on mobile).
// ===========================================================================

layout(location = 0) out vec2 vUV;

void main() {
  // Bit trick: vertex 0/1/2 -> (-1,-1), (3,-1), (-1,3)
  vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  vUV = pos;
  gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
