// Added (remaster): volumetric fog (atmosphere step 2). Each pixel marches its view ray from the
// camera to the surface (or u_fog.z units), and at every step takes the baked light in the air
// there (the light volume around the camera, fine and coarse) and a density (base haze with
// slowly drifting noise). Light scattered towards the camera adds up, and the scene behind is
// dimmed by what's in front: air glows where light is and stays dark where it isn't.
// Output: rgb = scattered light, a = how much of the scene shows through (blend ONE, SRC_ALPHA).
//   tex  - depth target from the world pass (z = signed depth, a = 1 where drawn)
uniform sampler2D tex;
uniform sampler2D tex2;
uniform sampler3D u_fine;     // light in the air near the camera (rgb; a = 1 where baked)
uniform sampler3D u_coarse;   // the same, coarser, further out
uniform vec3 u_fineOrigin;    // world position of the fine grid's corner
uniform vec3 u_fineSize;      // world size of the fine grid
uniform vec3 u_coarseOrigin;
uniform vec3 u_coarseSize;
uniform vec4 u_proj;          // half screen width, half screen height, focal length
uniform vec4 u_screen;        // canvas width, canvas height
uniform vec3 u_WX;            // world axes in camera space
uniform vec3 u_WY;
uniform vec3 u_WZ;
uniform vec3 u_camPos;        // camera position in the world
uniform vec4 u_fog;           // density per unit, light gain, max distance, time (s)
uniform vec4 u_fog2;          // noise amount, noise scale, light cap, steps
uniform vec2 iResolution;
uniform float param1;
uniform float param2;
uniform float param3;
in vec4 f_color;
in vec2 f_uv;
in vec3 f_coord;
out vec4 fragColor;

float hash(vec3 p)
{
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float noise(vec3 x)
{
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash(i + vec3(0, 0, 0)), hash(i + vec3(1, 0, 0)), f.x),
                   mix(hash(i + vec3(0, 1, 0)), hash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash(i + vec3(0, 0, 1)), hash(i + vec3(1, 0, 1)), f.x),
                   mix(hash(i + vec3(0, 1, 1)), hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

// Light in the air at a world position: fine grid where it has data, else coarse, else a dim
// default. Cells with no data (inside walls) have alpha 0, so dividing by the filtered alpha
// keeps walls from darkening the air next to them.
vec3 airLight(vec3 P)
{
    vec3 uf = (P - u_fineOrigin) / u_fineSize;
    if (all(greaterThan(uf, vec3(0.0))) && all(lessThan(uf, vec3(1.0)))) {
        vec4 s = texture(u_fine, uf);
        if (s.a > 0.05) return min(s.rgb / s.a, vec3(u_fog2.z));
    }
    vec3 uc = (P - u_coarseOrigin) / u_coarseSize;
    if (all(greaterThan(uc, vec3(0.0))) && all(lessThan(uc, vec3(1.0)))) {
        vec4 s = texture(u_coarse, uc);
        if (s.a > 0.05) return min(s.rgb / s.a, vec3(u_fog2.z));
    }
    return vec3(0.01);
}

void main(void)
{
    vec4 d = texture(tex, f_uv);
    float maxDist = u_fog.z;
    // Distance along the view ray to the surface (camera-space depth is y).
    float depth = (d.a > 0.5 && d.z > 0.0 && d.z < 500.0) ? d.z : maxDist;
    vec2 c = vec2(f_uv.x * u_screen.x, (1.0 - f_uv.y) * u_screen.y);
    vec3 dirC = vec3((c.x - u_proj.x) / u_proj.z, 1.0, (u_proj.y - c.y) / u_proj.z);  // depth 1
    float far = min(depth, maxDist);
    vec3 endC = dirC * far;
    vec3 endW = u_camPos + vec3(dot(endC, u_WX), dot(endC, u_WY), dot(endC, u_WZ));
    vec3 seg = endW - u_camPos;
    float len = length(seg);
    int steps = int(u_fog2.w);
    float ds = len / float(steps);
    float jitter = hash(vec3(gl_FragCoord.xy, fract(u_fog.w * 7.0) * 61.0));
    vec3 scat = vec3(0.0);
    float T = 1.0;
    vec3 drift = vec3(0.13, 0.07, 0.02) * u_fog.w;
    for (int i = 0; i < 64; i++) {
        if (i >= steps) break;
        vec3 P = u_camPos + seg * ((float(i) + jitter) / float(steps));
        float n = noise(P * u_fog2.y + drift) * 0.65 + noise(P * u_fog2.y * 2.7 - drift * 1.7) * 0.35;
        float dens = u_fog.x * max(0.0, 1.0 + u_fog2.x * (n * 2.0 - 1.0));
        float tr = exp(-dens * ds);
        scat += T * airLight(P) * (1.0 - tr);
        T *= tr;
    }
    fragColor = vec4(scat * u_fog.y, T);
}
