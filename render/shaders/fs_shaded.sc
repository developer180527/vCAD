$input v_normal, v_color, v_ids, v_wpos

#include <bgfx_shader.sh>

uniform vec4 u_shading;      // x = ambient, y = unused, z = unused, w = unused

// xy = highlight lookup size in texels, z = 1 when the lookup is bound, w = unused.
//
// This uniform used to be "rgb = tint, a = strength", and that is precisely why nothing was ever
// highlighted: a uniform is per DRAW CALL, and one draw call covers a whole mesh across every
// placement of it, while selection is per element. There was no value that could mean "this face and
// not the other five", so the backend set strength to zero permanently and the highlight table the
// scene layer had been maintaining all along went nowhere.
uniform vec4 u_highlight;
SAMPLER2D(s_highlight, 0);   // R8, one texel per element slot; 0 none, 1 hovered, 2 selected, 3 error

// The tint per kind lives HERE, not on the CPU, because it has to vary per fragment. Mixed rather
// than replaced so the shading survives -- a flat fill reads as a hole in the part, not a selection.
vec3 highlightTint(float kind)
{
	if (kind > 2.5) return vec3(0.85, 0.24, 0.18);   // error
	if (kind > 1.5) return vec3(0.10, 0.45, 0.91);   // selected
	return vec3(0.36, 0.72, 0.98);                   // hovered
}

float highlightStrength(float kind)
{
	if (kind < 0.5) return 0.0;
	if (kind > 1.5) return 0.55;   // selected: unmistakable
	return 0.30;                   // hovered: a hint, not a commitment
}

// Section plane: xyz is the normal, w the offset along it. Disabled when the normal is zero,
// which is what an unset uniform already is -- so a backend that never sets it clips nothing.
uniform vec4 u_sectionPlane;

void main()
{
	// Everything on the near side of the section plane is cut away. This is Slice: sketching on a
	// face buried inside a part is otherwise done blind, because the material in front of the
	// sketch plane hides both the face and whatever is being drawn on it.
	if (dot(u_sectionPlane.xyz, u_sectionPlane.xyz) > 0.25
	    && dot(v_wpos, u_sectionPlane.xyz) > u_sectionPlane.w)
	{
		discard;
	}

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

	// The element slot travels in v_ids.x for picking; the same number indexes the highlight table.
	// Rounded, not truncated: it arrives through a float interpolator, and floor() on a value that
	// interpolated to 5.9999 would highlight element 5 instead of 6.
	float kind = 0.0;
	if (u_highlight.z > 0.5)
	{
		float slot = floor(v_ids.x + 0.5);
		float row = floor(slot / u_highlight.x);
		vec2 uv = vec2((slot - row * u_highlight.x + 0.5) / u_highlight.x,
		               (row + 0.5) / max(u_highlight.y, 1.0));
		kind = floor(texture2D(s_highlight, uv).x * 255.0 + 0.5);
	}

	gl_FragColor = vec4(mix(base, highlightTint(kind), highlightStrength(kind)), 1.0);
}
