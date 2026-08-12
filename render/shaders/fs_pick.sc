$input v_normal, v_color, v_ids

#include <bgfx_shader.sh>

// Writes the absolute element slot as an RGBA8 colour, to be read back on the CPU.
//
// An ID buffer rather than CPU ray casting against B-rep: it is far simpler, it is O(1) in
// scene complexity, and box/lasso select falls out of it for free by reading a rectangle
// instead of a pixel.
void main()
{
	float id = v_ids.x + 1.0;   // 0 is reserved for "nothing here"

	// Little-endian across the channels, matching the CPU-side decode.
	float r = mod(id, 256.0);
	float g = mod(floor(id / 256.0), 256.0);
	float b = mod(floor(id / 65536.0), 256.0);
	float a = mod(floor(id / 16777216.0), 256.0);

	gl_FragColor = vec4(r, g, b, a) / 255.0;
}
