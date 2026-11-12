// Added (remaster): distance haze. Fog thickens with camera-space depth and takes its colour
// from a heavily blurred copy of the scene, so it glows near lights (a saber's colour, red
// near red strips) instead of being a flat grey. Output is alpha-blended over the scene.
//   tex    - blurred scene colour
//   tex2   - depth target from the world pass (z = signed depth, a = 1 where drawn)
//   param1 - density per world unit
//   param2 - strength multiplier
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

void main(void)
{
    vec4 d = texture(tex2, f_uv);
    // The volumetric sky (z = -1000 marker) has its own aerial perspective: only a light veil.
    if (d.a > 0.5 && d.z < -500.0) {
        fragColor = vec4(vec3(0.020, 0.021, 0.024) + texture(tex, f_uv).rgb * 1.6, 0.12 * param2);
        return;
    }
    // Nothing drawn here (open void): treat as far away.
    float depth = (d.a > 0.5) ? abs(d.z) : 40.0;

    float fog = 1.0 - exp(-param1 * depth);
    fog = clamp(fog * param2, 0.0, 0.75);

    // A little base haze so dark areas still read as air, plus scattered light.
    vec3 lit = texture(tex, f_uv).rgb;
    vec3 col = vec3(0.020, 0.021, 0.024) + lit * 1.6;

    fragColor = vec4(col, fog);
}
