// Added (remaster): final colour grading pass for the 3D scene (HUD is drawn after, ungraded).
//   tex    - composited scene in a half-float buffer, so bloom can go above 1.0
//   lut    - 3D colour lookup table loaded from a .cube file (Resolve, After Effects, etc.)
//   param1 - exposure multiplier
//   param2 - 1.0 = soft highlight roll-off instead of hard clipping, 0.0 = clip
//   param3 - LUT strength, 0.0 = no LUT, 1.0 = full
uniform sampler2D tex;
uniform sampler3D lut;
uniform float lutSize;
uniform vec3 lutDomainMin;
uniform vec3 lutDomainMax;
uniform vec2 iResolution;
uniform float param1;
uniform float param2;
uniform float param3;
in vec4 f_color;
in vec2 f_uv;
in vec3 f_coord;
out vec4 fragColor;

// Below the knee the image is untouched; above it values ease towards 1.0 instead of
// clipping. Continuous slope at the knee, so there's no visible band.
vec3 highlightRollOff(vec3 c)
{
    const float knee = 0.8;
    vec3 over = max(c - knee, 0.0);
    vec3 rolled = knee + (1.0 - knee) * (1.0 - exp(-over / (1.0 - knee)));
    return mix(c, rolled, step(knee, c));
}

vec3 applyLut(vec3 c)
{
    vec3 t = clamp((c - lutDomainMin) / (lutDomainMax - lutDomainMin), 0.0, 1.0);
    // Sample at texel centres so 0 and 1 hit the first and last LUT entries exactly.
    vec3 uvw = t * ((lutSize - 1.0) / lutSize) + 0.5 / lutSize;
    return texture(lut, uvw).rgb;
}

void main(void)
{
    vec3 c = texture(tex, f_uv).rgb * param1;

    if (param2 > 0.5)
        c = highlightRollOff(c);
    c = clamp(c, 0.0, 1.0);

    if (param3 > 0.0 && lutSize > 1.0)
        c = mix(c, applyLut(c), clamp(param3, 0.0, 1.0));

    // Tiny dither so smooth gradients in dark scenes don't band after the grade.
    float n = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    c += (n - 0.5) / 255.0;

    fragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
