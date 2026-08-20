$input v_normal, v_color, v_ids, v_wpos

#include <bgfx_shader.sh>

uniform vec4 u_edgeColor;
uniform vec4 u_edgeParams;   // w = 1 on the pass that draws ONLY highlighted edges
uniform vec4 u_sectionPlane;

// The SAME highlight table the shaded pass reads. Its absence here is why a selected edge never
// changed colour: this shader wrote u_edgeColor unconditionally, so every edge in the scene was
// the same near-black line whatever the model thought was selected.
//
// xy = lookup size in texels, z = 1 when bound.
uniform vec4 u_highlight;
SAMPLER2D(s_highlight, 0);   // R8, one texel per element slot; 0 none, 1 hovered, 2 selected, 3 error

// The same tints as fs_shaded, and deliberately not a shared include: a shader include would have
// to be compiled into both anyway, and the two differ in how strongly they apply it. An edge is a
// thin line — a 55% mix that reads as "unmistakable" across a face is barely visible on one — so an
// edge takes the tint outright.
vec3 highlightTint(float kind)
{
	if (kind > 2.5) return vec3(0.85, 0.24, 0.18);   // error
	if (kind > 1.5) return vec3(0.10, 0.45, 0.91);   // selected
	return vec3(0.36, 0.72, 0.98);                   // hovered
}

void main()
{
	// Clipped with the surfaces. Edges left unclipped hang in the air in front of the cut, which
	// reads as the model being broken rather than as a section view.
	if (dot(u_sectionPlane.xyz, u_sectionPlane.xyz) > 0.25
	    && dot(v_wpos, u_sectionPlane.xyz) > u_sectionPlane.w)
	{
		discard;
	}

	float kind = 0.0;
	if (u_highlight.z > 0.5)
	{
		// Rounded, not truncated: the slot arrives through a float interpolator, and floor() on a
		// value that interpolated to 5.9999 would highlight element 5 instead of 6.
		float slot = floor(v_ids.x + 0.5);
		float row = floor(slot / u_highlight.x);
		vec2 uv = vec2((slot - row * u_highlight.x + 0.5) / u_highlight.x,
		               (row + 0.5) / max(u_highlight.y, 1.0));
		kind = floor(texture2D(s_highlight, uv).x * 255.0 + 0.5);
	}

	// The thickening pass draws the whole edge stream offset by a pixel and keeps only the
	// highlighted fragments. Without this discard it would smear every edge in the scene.
	if (u_edgeParams.w > 0.5 && kind < 0.5)
	{
		discard;
	}

	vec3 colour = kind < 0.5 ? u_edgeColor.rgb : highlightTint(kind);
	gl_FragColor = vec4(colour, 1.0);
}
