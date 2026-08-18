$input a_position, a_normal, a_color1, i_data0, i_data1, i_data2, i_data3
$output v_normal, v_color, v_ids, v_wpos

#include <bgfx_shader.sh>
#include "common.sh"

void main()
{
	vec4 world = vec4(instancePosition(i_data0, i_data1, i_data2, a_position), 1.0);
	gl_Position = mul(u_viewProj, world);

	// World position, for the section test in the fragment shader. The clip has to be per
	// FRAGMENT: testing per vertex would cut whole triangles and leave a ragged staircase
	// wherever the plane crosses one.
	v_wpos = world.xyz;

	v_normal = normalize(instanceDirection(i_data0, i_data1, i_data2, a_normal));

	// i_data3 is the colour+id word reinterpreted as floats by the instance stream. Unpacking
	// it here keeps the CPU-side Instance struct tight.
	v_color = vec4(i_data3.xyz, 1.0);
	// NOT a_color1 * 255.0. The attribute is declared UNNORMALISED (see BgfxBackend's vertex
	// layout), so bgfx hands the four bytes over as floats already in 0..255 -- which is exactly
	// what unpackU32 expects. Scaling by 255 first made every element id 255 times too large.
	//
	// Nothing on screen showed it. Highlighting ignored the table entirely, and picking is the other
	// consumer -- so GPU picks resolved to slots that do not exist and silently found nothing, while
	// every test drove the scripted null picker instead of a GPU.
	v_ids = vec4(unpackU32(a_color1) + i_data3.w, 0.0, 0.0, 0.0);
}
