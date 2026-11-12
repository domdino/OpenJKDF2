// Added (remaster): volumetric cloud panorama. Renders the cloud layer as seen from the level
// into a panorama (x = azimuth 0..360, y = elevation from just below the horizon to straight
// up), a slice of columns per frame. The sky surfaces then sample it by view direction
// (default_f.glsl, skyMain), so the cost doesn't depend on screen size.
//   rgb = light scattered towards the viewer by the clouds (premultiplied)
//   a   = transmittance (1 = clear sky shows through)
// Units: km, viewer at height 0, world Z up. Raymarched through a slab of Perlin-Worley
// noise with a short march towards the sun/moon per sample for self-shadowing (Beer's law,
// a dual-lobe phase function for the silver lining, and a powder term for dark crevices).
uniform sampler2D tex;
uniform sampler2D tex2;
uniform sampler2D tex3;
uniform vec2 iResolution;
uniform float param1;   // time (s)
uniform float param2;
uniform float param3;
uniform sampler3D u_shape;
uniform sampler3D u_detail;
uniform vec3 u_sunDir;
uniform vec3 u_sunCol;     // colour * intensity
uniform vec3 u_ambTop;
uniform vec3 u_ambBottom;
uniform vec3 u_horizon;
uniform vec4 u_layer;      // base km, top km, coverage, density (extinction per km)
uniform vec2 u_wind;       // km offset of the cloud field
in vec4 f_color;
in vec2 f_uv;
in vec3 f_coord;
out vec4 fragColor;

const float ELEV_MIN = 0.12;   // radians below the horizon covered by the panorama
const float PI = 3.14159265;
const int STEPS = 56;
const int LIGHT_STEPS = 5;
const float MAX_DIST = 70.0;

float remap(float v, float lo, float hi, float nlo, float nhi)
{
    return nlo + (v - lo) / max(hi - lo, 1e-4) * (nhi - nlo);
}

float hg(float c, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

// Cloud density at p (km); h = 0..1 through the layer. Detail erosion only when asked
// (the light march uses the cheap version).
float cloudDensity(vec3 p, float h, bool detail)
{
    if (h <= 0.0 || h >= 1.0) return 0.0;
    vec3 q = p + vec3(u_wind, 0.0);
    // Large-scale weather: where the deck is thick and where it breaks.
    float weather = texture(u_shape, vec3(q.xy / 30.0 + vec2(0.37, 0.71), 0.37)).r;
    float cov = clamp(u_layer.z + (weather - 0.5) * 0.45, 0.0, 1.0);
    vec4 s = texture(u_shape, q / 9.0);
    vec4 s2 = texture(u_shape, q / 2.7 + vec3(0.31, 0.17, 0.0));
    float base = s.r * 0.7 + s2.r * 0.3;  // big masses plus smaller billows
    // Height profile: rounded base, tops thinning out.
    float profile = smoothstep(0.0, 0.1, h) * (1.0 - smoothstep(0.6, 1.0, h));
    // The noise is equalised (uniform 0..1), so coverage is the cloudy fraction.
    // Fairly hard edges: dense within a short band above the coverage threshold.
    float c = clamp(remap(base * mix(0.75, 1.0, profile), 1.0 - cov, 1.0 - cov + 0.22, 0.0, 1.0), 0.0, 1.0) * profile;
    if (c <= 0.0) return 0.0;
    if (detail)
    {
        vec3 d = texture(u_detail, q / 0.45 + vec3(0.0, 0.0, param1 * 0.004)).rgb;
        float dfbm = d.r * 0.625 + d.g * 0.25 + d.b * 0.125;
        float m = mix(dfbm, 1.0 - dfbm, clamp(h * 4.0, 0.0, 1.0));
        c = remap(c, m * 0.45, 1.0, 0.0, 1.0);
    }
    return clamp(c, 0.0, 1.0);
}

void main(void)
{
    float az = f_uv.x * 2.0 * PI;
    // Below the horizon the view looks into the same deck as at the horizon (the shader
    // drawing the sky fades it into murk); marching it separately only made streaks.
    float el = max(mix(-ELEV_MIN, 0.5 * PI, f_uv.y), 0.01);
    vec3 dir = vec3(cos(el) * cos(az), cos(el) * sin(az), sin(el));

    float dz = max(dir.z, 0.025);   // below the horizon: look into the base of the deck
    float t0 = u_layer.x / dz;
    float t1 = min(u_layer.y / dz, MAX_DIST);
    if (t0 >= MAX_DIST) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }

    float sigma = u_layer.w;
    float dt = (t1 - t0) / float(STEPS);
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float cosT = dot(dir, u_sunDir);
    float phase = min(mix(hg(cosT, 0.5), hg(cosT, -0.2), 0.3) * 4.0 * PI, 1.7);
    float lightStep = (u_layer.y - u_layer.x) * 0.18;

    vec3 L = vec3(0.0);
    float T = 1.0;
    float t = t0 + dt * jitter;
    for (int i = 0; i < STEPS; i++)
    {
        vec3 p = dir * t;
        p.z = dz * t;
        float h = (p.z - u_layer.x) / (u_layer.y - u_layer.x);
        float d = cloudDensity(p, h, true);
        if (d > 0.002)
        {
            // Optical depth towards the sun/moon.
            float od = 0.0;
            for (int j = 0; j < LIGHT_STEPS; j++)
            {
                vec3 pl = p + u_sunDir * lightStep * (float(j) + 0.5);
                float hl = (pl.z - u_layer.x) / (u_layer.y - u_layer.x);
                od += cloudDensity(pl, hl, false) * lightStep;
            }
            float Tl = exp(-od * sigma);
            float powder = 1.0 - exp(-od * sigma * 2.0);
            vec3 sun = u_sunCol * Tl * phase * mix(0.6, 1.0, powder);
            // Sky light from above, dimmed by the cloud overhead: thick parts of the deck go
            // dark underneath, thin parts glow. Two cheap samples straight up.
            float odUp = 0.0;
            float upStep = (u_layer.y - p.z) * 0.5;
            for (int j = 0; j < 2; j++)
            {
                vec3 pu = p + vec3(0.0, 0.0, upStep * (float(j) + 0.5));
                odUp += cloudDensity(pu, (pu.z - u_layer.x) / (u_layer.y - u_layer.x), false) * upStep;
            }
            vec3 amb = u_ambBottom + u_ambTop * exp(-odUp * sigma * 0.35);
            vec3 S = sun + amb;
            float ext = d * sigma;
            float Ts = exp(-ext * dt);
            L += T * S * (1.0 - Ts);
            T *= Ts;
            if (T < 0.01) break;
        }
        t += dt;
    }
    // Aerial perspective: distant cloud melts into the horizon colour.
    float fade = exp(-max(t0 - 6.0, 0.0) / 30.0);
    L = L * fade + u_horizon * (1.0 - T) * (1.0 - fade);
    fragColor = vec4(L, T);
}
