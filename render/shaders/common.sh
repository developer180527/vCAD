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

// The instance stream carries a 4x3 affine as three vec4 slots: slot N is basis column N in
// .xyz, with translation component N in .w. Applied here as explicit arithmetic rather than by
// building a mat4, deliberately.
//
// There WAS an instanceTransform() that assembled a mat4 and multiplied by it. bgfx_shader.sh
// defines mat4()'s argument order per backend -- column-major for GLSL, row-major for HLSL and
// Metal -- so the same constructor means two different matrices depending on who compiles it.
// Ours put the translation in the bottom row, where the multiply discards it: every instance
// drew at the origin. Because CAD placements have identity bases, the rotation and scale looked
// perfect, so the frame showed one correct-looking box no matter how many were submitted. It was
// diagnosed for months as "instancing is broken, every instance reads element 0" -- it was not,
// the instances were all there and all stacked. bgfx ships mtxFromCols for this, but it does not
// survive our HLSL path; dot products have no convention to get wrong.
vec3 instancePosition(vec4 r0, vec4 r1, vec4 r2, vec3 p)
{
	return r0.xyz * p.x + r1.xyz * p.y + r2.xyz * p.z + vec3(r0.w, r1.w, r2.w);
}

// Directions ignore the translation. Correct for the rigid placements CAD assemblies use; a
// non-uniform scale would need the inverse transpose, and we do not allow one.
vec3 instanceDirection(vec4 r0, vec4 r1, vec4 r2, vec3 d)
{
	return r0.xyz * d.x + r1.xyz * d.y + r2.xyz * d.z;
}
