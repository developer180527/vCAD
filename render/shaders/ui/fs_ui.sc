$input v_local, v_uv, v_color0, v_color1, v_clip, v_params, v_half, v_world, v_seg

#include <bgfx_shader.sh>

uniform vec4 u_screen;
SAMPLER2D(s_tex, 0);

float sdSegment(vec2 p, vec2 a, vec2 b)
{
    vec2 pa = p - a;
    vec2 ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
    return length(pa - ba * h);
}

float sdRoundRect(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b + vec2(r, r);
    return length(max(q, vec2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - r;
}

vec4 premul(vec4 c) { return vec4(c.rgb * c.a, c.a); }

void main()
{
    if (v_world.x < v_clip.x || v_world.y < v_clip.y || v_world.x > v_clip.z || v_world.y > v_clip.w)
        discard;
    float kind = floor(v_params.w + 0.5);
    float aa = 1.0 / u_screen.z;

    if (kind == 3.0) {
        float d = sdSegment(v_world, v_seg.xy, v_seg.zw) - v_params.x;
        gl_FragColor = premul(v_color0) * clamp(0.5 - d / aa, 0.0, 1.0);
        return;
    }
    if (kind == 2.0) {
        float r = min(v_params.x, min(v_half.x, v_half.y));
        float d = sdRoundRect(v_local, v_half, r);
        float m = clamp(0.5 - d / aa, 0.0, 1.0);
        // params.y is the image's alpha mode (libgui render contract 3): 0 ignores the texture's
        // alpha (a 3D view), 1 reads it as premultiplied, 2 as straight.
        //
        // bgfx filters in hardware, so straight alpha cannot be premultiplied per texel before
        // filtering the way libgui's reference shader does; it is premultiplied after, which
        // haloes edges. libgui's header says such a renderer should premultiply on upload and draw
        // with mode 1 -- vCAD's icon atlas does. Mode 2 is honoured, not exact.
        float mode = floor(v_params.y + 0.5);
        vec4 texel = texture2D(s_tex, v_uv);
        if (mode < 0.5) {
            gl_FragColor = premul(vec4(texel.rgb * v_color0.rgb, v_color0.a)) * m;
            return;
        }
        if (mode > 1.5) texel = vec4(texel.rgb * texel.a, texel.a);
        gl_FragColor = vec4(texel.rgb * v_color0.rgb * v_color0.a, texel.a * v_color0.a) * m;
        return;
    }
    if (kind == 1.0) {
        gl_FragColor = premul(v_color0) * texture2D(s_tex, v_uv).r;
        return;
    }
    float r = min(v_params.x, min(v_half.x, v_half.y));
    float soft = v_params.z + aa;
    float d = sdRoundRect(v_local, v_half, r);
    float fill = 1.0 - smoothstep(-soft * 0.5, soft * 0.5, d);
    vec4 col = premul(v_color0);
    float bw = v_params.y;
    if (bw > 0.0) {
        float inner = sdRoundRect(v_local, v_half - vec2(bw, bw), max(r - bw, 0.0));
        col = mix(col, premul(v_color1), smoothstep(-aa * 0.5, aa * 0.5, inner));
    }
    gl_FragColor = col * fill;
}
