$input a_position, a_texcoord0, a_texcoord1, a_color0, a_color1, a_texcoord2, a_texcoord3, a_texcoord4, a_texcoord5
$output v_local, v_uv, v_color0, v_color1, v_clip, v_params, v_half, v_world, v_seg

#include <bgfx_shader.sh>

uniform vec4 u_screen;

void main()
{
    gl_Position = vec4(a_position.x / u_screen.x * 2.0 - 1.0, 1.0 - a_position.y / u_screen.y * 2.0, 0.0, 1.0);
    v_world  = a_position;
    v_local  = a_texcoord0;
    v_uv     = a_texcoord1;
    v_color0 = a_color0;
    v_color1 = a_color1;
    v_clip   = a_texcoord2;
    v_params = a_texcoord3;
    v_half   = a_texcoord4;
    v_seg    = a_texcoord5;
}
