#version 450
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in uint v_id;
layout(location = 3) in float v_lit;
layout(location = 4) in vec4 v_clip0;
layout(location = 5) in vec4 v_clip1;

layout(location = 0) out vec4 o_color;
layout(location = 1) out uint o_id;  // ID 버퍼(RND-48): 픽셀마다 그 자리에 그려진 객체의 번호

void main() {
  if (any(lessThan(v_clip0, vec4(0.0))) || any(lessThan(v_clip1, vec4(0.0)))) discard;  // 클리핑 평면(RND-19): 하나라도 지우는 쪽이면
  vec3 c = v_color.rgb;
  if (v_lit > 0.5) {
    // 카메라에 붙은 조명(RND-16): 시선 방향과 법선의 각으로 밝기를 정한다
    vec3 n = normalize(v_normal);
    c *= 0.35 + 0.65 * abs(n.z);
    // 뒷면은 다른 색을 섞어 구분한다(RND-12, 쉘 법선 확인용)
    if (!gl_FrontFacing) c = mix(c, vec3(0.60, 0.25, 0.25), 0.5);
  }
  o_color = vec4(c, v_color.a);
  o_id = v_id;
}
