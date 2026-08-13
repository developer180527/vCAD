$input a_position, a_normal, a_color1, i_data0, i_data1, i_data2, i_data3
$output v_normal, v_color, v_ids

#include <bgfx_shader.sh>
#include "common.sh"

void main()
{
	vec4 world = vec4(instancePosition(i_data0, i_data1, i_data2, a_position), 1.0);
	gl_Position = mul(u_viewProj, world);

	v_normal = normalize(instanceDirection(i_data0, i_data1, i_data2, a_normal));

	// i_data3 is the colour+id word reinterpreted as floats by the instance stream. Unpacking
	// it here keeps the CPU-side Instance struct tight.
	v_color = vec4(i_data3.xyz, 1.0);
	v_ids = vec4(unpackU32(a_color1 * 255.0) + i_data3.w, 0.0, 0.0, 0.0);
}
