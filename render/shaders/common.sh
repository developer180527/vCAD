// Shared helpers, included by every shader here.
//
// NOTE: comments are fine in THIS file. They are NOT fine at the top of varying.def.sc --
// shaderc silently drops the first declaration there. See README.md.

// bgfx has no integer vertex attributes, so the element index travels as four unnormalised
// uint8 channels and is reassembled here. This is the standard bgfx idiom for passing an
// integer id through a vertex stream.
float unpackU32(vec4 packed)
{
	return packed.x + packed.y * 256.0 + packed.z * 65536.0 + packed.w * 16777216.0;
}

// Instance rows are the 4x3 affine we upload; the fourth row is always (0,0,0,1) and is
// reconstructed rather than transmitted.
mat4 instanceTransform(vec4 r0, vec4 r1, vec4 r2)
{
	return mat4(
		r0.x, r0.y, r0.z, 0.0,
		r1.x, r1.y, r1.z, 0.0,
		r2.x, r2.y, r2.z, 0.0,
		r0.w, r1.w, r2.w, 1.0);
}
