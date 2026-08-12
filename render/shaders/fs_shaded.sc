$input v_normal, v_color, v_ids

#include <bgfx_shader.sh>

uniform vec4 u_shading;      // x = ambient, y = unused, z = unused, w = unused
uniform vec4 u_highlight;    // rgb = highlight tint, a = strength

void main()
{
	// Two-sided lighting. CAD surfaces are routinely viewed from their back face — inside a
	// pocket, through a section cut — and a one-sided model renders those pure black, which
	// users read as a hole in the part.
	vec3 n = normalize(v_normal);
	vec3 keyDir = normalize(vec3(0.4, 0.5, 0.75));
	float key = abs(dot(n, keyDir));

	// A second, dimmer light from below. Not physically motivated: it stops downward-facing
	// faces from going flat black, which is the single thing that makes an untextured CAD
	// model look unreadable.
	float fill = abs(dot(n, normalize(vec3(-0.3, -0.4, -0.2)))) * 0.35;

	float lit = u_shading.x + (1.0 - u_shading.x) * clamp(key + fill, 0.0, 1.0);
	vec3 base = v_color.rgb * lit;

	gl_FragColor = vec4(mix(base, u_highlight.rgb, u_highlight.a), 1.0);
}
