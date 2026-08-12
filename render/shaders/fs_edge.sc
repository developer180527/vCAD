$input v_normal, v_color, v_ids

#include <bgfx_shader.sh>

uniform vec4 u_edgeColor;

void main()
{
	gl_FragColor = vec4(u_edgeColor.rgb, 1.0);
}
