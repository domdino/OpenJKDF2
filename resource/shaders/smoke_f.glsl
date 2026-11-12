// Added (remaster): smoke and steam (atmosphere step 5). Puffs from the emitters in
// jkgm/atmos/<level>.json, worked out on the CPU each frame (sithAtmos_GetParticles), projected
// and sorted far to near (std3D). Each is a soft, animated noise puff:
//   - lit by the baked light in the air where it is (the same light volume as the fog);
//   - fading where it meets geometry (soft particles: no hard lines through floors and walls);
//   - fading in when born and out as it dies, growing as it rises.
// Steam is white and wispy; smoke is denser and takes its tint.
// Output: premultiplied colour and coverage, blended ONE, ONE_MINUS_SRC_ALPHA.
//   tex - depth target from the world pass (z = signed depth, a = 1 where drawn)
uniform sampler2D tex;
uniform sampler2D tex2;
uniform sampler3D u_fine;
uniform sampler3D u_coarse;
uniform vec3 u_fineOrigin;
uniform vec3 u_fineSize;
uniform vec3 u_coarseOrigin;
uniform vec3 u_coarseSize;
uniform int u_numP;
uniform vec4 u_pScreen[96];   // u, v, radius (in screen heights), view depth
uniform vec4 u_pWorld[96];    // world xyz, age 0-1
uniform vec4 u_pLook[96];     // tint rgb, opacity
uniform vec4 u_pMisc[96];     // seed, kind (0 steam, 1 smoke, 2 dust)
uniform float u_aspect;       // width / height
uniform float u_time;
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

float fbm(vec3 x)
{
    return noise(x) * 0.55 + noise(x * 2.1 + 3.7) * 0.3 + noise(x * 4.3 + 9.1) * 0.15;
}

vec3 airLight(vec3 P)
{
    vec3 L = vec3(0.02);
    vec3 uf = (P - u_fineOrigin) / u_fineSize;
    vec3 uc = (P - u_coarseOrigin) / u_coarseSize;
    if (all(greaterThan(uf, vec3(0.0))) && all(lessThan(uf, vec3(1.0)))) {
        vec4 s = texture(u_fine, uf);
        if (s.a > 0.05) L = s.rgb / s.a;
        else if (all(greaterThan(uc, vec3(0.0))) && all(lessThan(uc, vec3(1.0)))) {
            vec4 c = texture(u_coarse, uc);
            if (c.a > 0.05) L = c.rgb / c.a;
        }
    } else if (all(greaterThan(uc, vec3(0.0))) && all(lessThan(uc, vec3(1.0)))) {
        vec4 c = texture(u_coarse, uc);
        if (c.a > 0.05) L = c.rgb / c.a;
    }
    L = min(L, vec3(3.0));
    return L / (1.0 + 0.6 * L);
}

void main(void)
{
    vec4 d = texture(tex, f_uv);
    float sceneDepth = (d.a > 0.5 && d.z > 0.0 && d.z < 500.0) ? d.z : 1e6;
    vec3 C = vec3(0.0);
    float A = 0.0;
    for (int i = 0; i < 96; i++) {
        if (i >= u_numP) break;
        vec4 ps = u_pScreen[i];
        vec2 q = (f_uv - ps.xy) * vec2(u_aspect, 1.0) / ps.z;
        float r = length(q);
        if (r >= 1.0) continue;
        float soft = clamp((sceneDepth - ps.w) / max(0.25 * ps.z * ps.w, 0.02), 0.0, 1.0);
        if (soft <= 0.0) continue;
        vec4 pw = u_pWorld[i];
        vec4 look = u_pLook[i];
        vec4 misc = u_pMisc[i];
        if (misc.y > 1.5) {
            // Dust mote: a tiny soft speck, only where the air is lit, twinkling slowly.
            vec3 Ld = airLight(pw.xyz);
            float lum = dot(Ld, vec3(0.2126, 0.7152, 0.0722));
            float lit = smoothstep(0.08, 0.4, lum);
            if (lit <= 0.0) continue;
            float tw = 0.55 + 0.45 * sin(u_time * (1.3 + misc.x * 2.0) + misc.x * 40.0);
            float da = clamp((1.0 - smoothstep(0.0, 1.0, r)) * lit * tw * look.a * soft * 1.6, 0.0, 1.0);
            C = C * (1.0 - da) + (Ld * 2.5 + 0.05) * da;
            A = A * (1.0 - da) + da;
            continue;
        }
        float age = pw.w;
        bool smoke = misc.y > 0.5;
        // Puff: noise that turns and billows with age, inside a soft round edge.
        float a = misc.x * 6.2831 + age * (smoke ? 0.8 : 1.6);
        vec2 qr = mat2(cos(a), -sin(a), sin(a), cos(a)) * q;
        float n = fbm(vec3(qr * (smoke ? 1.6 : 2.2), misc.x * 17.0 + age * (smoke ? 0.9 : 1.6)));
        float shape = 1.0 - smoothstep(0.25, 1.0, r);
        float dens = smoothstep(smoke ? 0.25 : 0.38, smoke ? 0.75 : 0.85, n * shape + shape * 0.35);
        float life = smoothstep(0.0, 0.12, age) * (1.0 - smoothstep(smoke ? 0.55 : 0.4, 1.0, age));
        float alpha = clamp(dens * life * look.a * soft, 0.0, 1.0);
        vec3 L = airLight(pw.xyz);
        // Steam takes the light's colour; smoke darkens it by its tint and keeps a little body.
        vec3 col = smoke ? L * look.rgb * 0.8 + look.rgb * 0.02 : L * look.rgb * 1.1 + 0.01;
        // Far to near: each puff goes over what's already there.
        C = C * (1.0 - alpha) + col * alpha;
        A = A * (1.0 - alpha) + alpha;
    }
    fragColor = vec4(C, A);
}
