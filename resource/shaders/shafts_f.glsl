// Added (remaster): light shafts (screen-space light scattering). For each chosen glow light,
// sample the glow buffer along the line from this pixel towards the light and accumulate it
// with decay: bright glowing areas between the pixel and the light smear into rays.
// Additive over the scene.
//   tex    - glow (emissive) buffer from the world pass
//   tex2   - depth target: the sun's rays only come from open sky, and a glow light hidden
//            behind a wall (checked around its screen position) casts no rays
//   tex3   - scene colour: its bright parts (light strips, sky) also feed the rays
//   param1 - strength
uniform sampler2D tex;
uniform sampler2D tex2;
uniform sampler2D tex3;
uniform vec2 iResolution;
uniform float param1;
uniform float param2;
uniform float param3;
uniform int u_numShafts;
uniform vec4 u_shaftPos[4];   // light uv xy, intensity, w = view depth of a glow light (< 0: the sky's sun/moon)
uniform vec4 u_shaftCol[4];   // rgb
in vec4 f_color;
in vec2 f_uv;
in vec3 f_coord;
out vec4 fragColor;

const int SAMPLES = 16; // was 32; the jitter hides the coarser steps

void main(void)
{
    vec3 total = vec3(0.0);
    for (int i = 0; i < u_numShafts; i++)
    {
        vec2 toLight = u_shaftPos[i].xy - f_uv;
        float dist = length(toLight);
        // Rays fade with distance; skip lights too far away to contribute.
        bool skySource = u_shaftPos[i].w < 0.0;
        float falloff = exp(-dist * (skySource ? 1.2 : 2.5));
        if (falloff * u_shaftPos[i].z < 0.02) continue;
        // Added (remaster): occlusion. A glow light sits on its own surface, so where it's in
        // view the depth around its screen position matches its depth; a wall or a character in
        // front is nearer. Off screen there's nothing to test, so it counts as visible.
        float vis = 1.0;
        vec2 luv = u_shaftPos[i].xy;
        if (!skySource && luv.x > 0.0 && luv.x < 1.0 && luv.y > 0.0 && luv.y < 1.0)
        {
            float lightDepth = u_shaftPos[i].w;
            float seen = 0.0;
            for (int ox = -1; ox <= 1; ox++)
                for (int oy = -1; oy <= 1; oy++) {
                    vec4 dd = texture(tex2, luv + vec2(ox, oy) * 0.006);
                    float sd = (dd.a > 0.5 && dd.z > 0.0 && dd.z < 500.0) ? dd.z : 1e6;
                    seen += sd >= lightDepth * 0.93 - 0.03 ? 1.0 : 0.0;
                }
            vis = seen / 9.0;
            if (vis <= 0.0) continue;
        }
        vec2 stepUV = toLight * (0.9 / float(SAMPLES));
        // Per-pixel jitter of the start point turns the 32-step banding into fine noise.
        float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        vec2 uv = f_uv + stepUV * jitter;
        float decay = 1.0;
        float accum = 0.0;
        // The sun/moon of the volumetric sky: only open sky feeds its rays (walls and cloud
        // cores block them), so they stream through gaps in the clouds and the level.
        for (int s = 0; s < SAMPLES; s++)
        {
            uv += stepUV;
            if (skySource)
            {
                float isSky = texture(tex2, uv).z < -500.0 ? 1.0 : 0.0;
                float lum = dot(texture(tex3, uv).rgb, vec3(0.2126, 0.7152, 0.0722));
                accum += isSky * max(lum - 0.45, 0.0) * 0.35 * decay;
                decay *= 0.94;
                continue;
            }
            float glow = dot(texture(tex, uv).rgb, vec3(0.2126, 0.7152, 0.0722));
            float bright = max(dot(texture(tex3, uv).rgb, vec3(0.2126, 0.7152, 0.0722)) - 0.7, 0.0) * 2.0;
            accum += (glow + bright) * decay;
            decay *= 0.91; // stronger per step, to cover the same distance in 16 steps
        }
        accum /= float(SAMPLES);
        total += u_shaftCol[i].rgb * accum * falloff * u_shaftPos[i].z * vis;
    }
    fragColor = vec4(total * param1 * 1.5, 0.0);
}
