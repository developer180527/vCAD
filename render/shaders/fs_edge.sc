$input v_normal, v_color, v_ids, v_wpos

#include <bgfx_shader.sh>

uniform vec4 u_edgeColor;
uniform vec4 u_sectionPlane;

void main()
{
	// Clipped with the surfaces. Edges left unclipped hang in the air in front of the cut, which
	// reads as the model being broken rather than as a section view.
	if (dot(u_sectionPlane.xyz, u_sectionPlane.xyz) > 0.25
	    && dot(v_wpos, u_sectionPlane.xyz) > u_sectionPlane.w)
	{
		discard;
	}

	gl_FragColor = vec4(u_edgeColor.rgb, 1.0);
}
