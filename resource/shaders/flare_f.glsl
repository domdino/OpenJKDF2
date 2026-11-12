// Added (remaster): lens flares from everything that blooms (light fixtures, glowing strips,
// sabers, the sky's sun). Reads the blurred bloom source and adds, like a film lens would:
//   - an anamorphic streak: a long thin horizontal smear of each bright source, cool blue-white
//     (the widescreen-lens look of the Star Wars films);
//   - ghosts: faint coloured copies of bright sources mirrored through the screen centre;
//   - a halo: a soft ring around the centre lit by sources near the edges.
// Output is added on top of the scene (blend ONE, ONE).
//   tex    - bloom source, quarter resolution, blurred
//   param1 - strength (r_flareStrength)
//   param2 - aspect (width / height)
uniform sampler2D tex;
uniform sampler2D tex2;
uniform vec2 iResolution;
uniform float param1;
uniform float param2;
uniform float param3;
in vec4 f_color;
in vec2 f_uv;
in vec3 f_coord;
out vec4 fragColor;

vec3 bright(vec2 uv, float threshold)
{
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return vec3(0.0);
    vec3 c = texture(tex, uv).rgb;
    float l = max(c.r, max(c.g, c.b));
    return c * max(l - threshold, 0.0) / max(l, 1e-4);
}

void main(void)
{
    vec2 uv = f_uv;
    vec3 sum = vec3(0.0);

    // Anamorphic streak: horizontal only, long, falling off with distance.
    vec3 streak = vec3(0.0);
    float wsum = 0.0;
    for (int i = -24; i <= 24; i++) {
        float x = float(i) / 24.0;
        float w = exp(-abs(x) * 3.0);
        streak += bright(uv + vec2(x * 0.45, 0.0), 0.35) * w;
        wsum += w;
    }
    streak /= wsum;
    sum += streak * vec3(0.55, 0.75, 1.25) * 2.2;

    // Ghosts through the centre, each a little smaller and differently tinted.
    vec2 toC = vec2(0.5) - uv;
    const vec3 tints[4] = vec3[4](vec3(1.0, 0.6, 0.3), vec3(0.4, 0.8, 1.0), vec3(0.7, 1.0, 0.5), vec3(1.0, 0.5, 0.9));
    for (int i = 1; i <= 4; i++) {
        vec2 g = uv + toC * (float(i) * 0.42);
        float fall = 1.0 - smoothstep(0.0, 0.75, length(vec2(0.5) - g) * 1.4);
        sum += bright(g, 0.6) * tints[i - 1] * fall * 0.18;
    }

    // Halo: a ring around the centre.
    vec2 hv = normalize(toC * vec2(param2, 1.0) + 1e-5) / vec2(param2, 1.0) * 0.38;
    float hfall = pow(1.0 - min(abs(length((uv - vec2(0.5)) * vec2(param2, 1.0)) - 0.38) * 6.0, 1.0), 3.0);
    sum += bright(uv + hv, 0.6) * vec3(0.8, 0.9, 1.0) * hfall * 0.15;

    fragColor = vec4(sum * param1, 1.0);
}
