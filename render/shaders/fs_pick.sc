// v_wpos IS DECLARED EVEN THOUGH THIS SHADER DOES NOT USE IT, and that is not tidiness.
//
// bgfx hashes a shader's varying signature and refuses to link a vertex shader whose OUTPUT set
// differs from the fragment shader's INPUT set — not a subset, identical. This shader pairs with
// vs_shaded, which outputs v_normal, v_color, v_ids and v_wpos. Declaring three of the four made
// createProgram return an invalid handle.
//
// What that cost is the interesting part. Nothing crashed and nothing was reported: the pick pass
// simply never ran, `readIds` returned early on an invalid program, and every pick came back as
// "nothing there". Shaded rendering was unaffected, so the model was plainly on screen while every
// click and every tap missed it — which reads as a broken picker, or on a tablet as a fat finger.
//
// It arrived when the Slice feature added v_wpos to vs_shaded and fs_shaded for its per-fragment
// clip. fs_pick was not updated, because nothing links the three files except a hash computed
// inside bgfx at runtime. render/src/BgfxBackend.cpp now logs when the pick program is missing,
// and spikes/highlight measures a real pick end to end.
$input v_normal, v_color, v_ids, v_wpos

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
