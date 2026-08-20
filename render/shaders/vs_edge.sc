$input a_position, a_color1, i_data0, i_data1, i_data2, i_data3
$output v_normal, v_color, v_ids, v_wpos

#include <bgfx_shader.sh>
#include "common.sh"

uniform vec4 u_edgeParams;   // x = depth bias in NDC, yz = screen offset in pixels, w = highlight-only

void main()
{
	vec4 world = vec4(instancePosition(i_data0, i_data1, i_data2, a_position), 1.0);
	vec4 clip = mul(u_viewProj, world);
	v_wpos = world.xyz;

	// Pull edges toward the viewer in clip space. Without this they z-fight the surfaces they
	// lie exactly on, producing the stippled, broken outlines that make a viewport look cheap.
	// Scaling by w keeps the bias constant in screen terms at any distance.
	clip.z -= u_edgeParams.x * clip.w;

	// Screen-space offset, for the pass that thickens highlighted edges. Zero on the normal pass.
	// Scaled by w so the offset stays a fixed number of PIXELS at any distance, and by the view
	// rect so it is the same size on a Retina display as anywhere else.
	if (u_viewRect.z > 0.0 && u_viewRect.w > 0.0)
	{
		clip.x += u_edgeParams.y * 2.0 / u_viewRect.z * clip.w;
		clip.y += u_edgeParams.z * 2.0 / u_viewRect.w * clip.w;
	}
	gl_Position = clip;

	v_normal = vec3(0.0, 0.0, 1.0);
	v_color = vec4(i_data3.xyz, 1.0);

	// The element slot, exactly as vs_shaded computes it: the mesh's own element plus the
	// instance's base, because one mesh is shared by every placement of it.
	//
	// This was a hard-coded zero, and the two consequences were both reported as bugs: the pick
	// buffer got id 0 for every edge, so no edge could be selected; and the highlight lookup read
	// slot 0 for every edge, so no edge could glow. NOT a_color1 * 255 — the attribute is
	// unnormalised, so bgfx delivers the four bytes already in 0..255.
	v_ids = vec4(unpackU32(a_color1) + i_data3.w, 0.0, 0.0, 0.0);
}
