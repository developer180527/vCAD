$input a_position, i_data0, i_data1, i_data2, i_data3
$output v_normal, v_color, v_ids

#include <bgfx_shader.sh>
#include "common.sh"

uniform vec4 u_edgeParams;   // x = depth bias in NDC

void main()
{
	mat4 model = instanceTransform(i_data0, i_data1, i_data2);
	vec4 clip = mul(u_viewProj, mul(model, vec4(a_position, 1.0)));

	// Pull edges toward the viewer in clip space. Without this they z-fight the surfaces they
	// lie exactly on, producing the stippled, broken outlines that make a viewport look cheap.
	// Scaling by w keeps the bias constant in screen terms at any distance.
	clip.z -= u_edgeParams.x * clip.w;
	gl_Position = clip;

	v_normal = vec3(0.0, 0.0, 1.0);
	v_color = vec4(i_data3.xyz, 1.0);
	v_ids = vec4(0.0, 0.0, 0.0, 0.0);
}
