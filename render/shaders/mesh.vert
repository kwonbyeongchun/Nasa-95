#version 450
// 면·선 공용. 법선이 0 이면 조명 없이 그리는 선이다.
// 푸시 상수는 보장되는 최대(128바이트)를 넘지 않는다. 클리핑 평면(RND-19)은 유니폼 버퍼로 받는다(최대 8개):
//   plane = (nx, ny, nz, d): nx·x + ny·y + nz·z + d < 0 인 쪽을 지운다. 여러 평면이면 하나라도 지우는 쪽이면 지운다.
layout(push_constant) uniform Push {
  mat4 mvp;
  mat4 view;
} pc;
layout(set = 0, binding = 0, std140) uniform Clip {
  vec4 planes[8];
  ivec4 count;  // x = 평면 수
} clip;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_color;
layout(location = 3) in uint in_id;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out uint v_id;
layout(location = 3) out float v_lit;
layout(location = 4) out vec4 v_clip0;  // 평면 0~3 까지의 부호 있는 거리(없는 평면은 큰 양수)
layout(location = 5) out vec4 v_clip1;  // 평면 4~7

void main() {
  gl_Position = pc.mvp * vec4(in_pos, 1.0);
  v_lit = dot(in_normal, in_normal) > 0.0 ? 1.0 : 0.0;
  v_normal = mat3(pc.view) * in_normal;
  v_color = in_color;
  v_id = in_id;
  // 평면마다 거리를 따로 넘긴다: 여러 평면의 최소값을 정점에서 구해 보간하면(min 은 비선형) 잘리는 자리가 틀어진다
  float d[8];
  for (int i = 0; i < 8; ++i) d[i] = i < clip.count.x ? dot(clip.planes[i].xyz, in_pos) + clip.planes[i].w : 1.0e30;
  v_clip0 = vec4(d[0], d[1], d[2], d[3]);
  v_clip1 = vec4(d[4], d[5], d[6], d[7]);
  // 선은 면과 겹칠 때 깜빡이지 않게 카메라 쪽으로 조금 당긴다(RND-13)
  if (v_lit == 0.0) gl_Position.z -= 2e-4 * gl_Position.w;
}
