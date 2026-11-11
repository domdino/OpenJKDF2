// Added (remaster): screen-space reflections. For each wet pixel, march its reflection ray
// across the screen using the depth the world pass wrote, and add the scene colour where it
// hits. Floors reflect most (wet ground), walls a little, everything more at grazing angles.
//   tex  - composited scene colour (the world pass's colour target)
//   tex2 - reflection data: octahedral camera-space normal (xy), signed depth (z), alpha 1
uniform sampler2D tex;
uniform sampler2D tex2;
uniform sampler2D tex3;     // blurred scene colour, for glossy (rough) reflections
uniform float u_roughness;  // r_reflectionRoughness: 0 = mirror, 1 = very blurry
uniform vec2 iResolution;
uniform vec4 u_proj;       // half screen width, half screen height, focal length
uniform vec4 u_screen;     // canvas width, canvas height, scene framebuffer width, height
uniform vec3 u_upCam;      // world up in camera space
uniform float u_wetness;   // r_wetness
uniform float u_strength;  // r_reflectionStrength
uniform float param1;
uniform float param2;
uniform float param3;
in vec4 f_color;
in vec2 f_uv;
in vec3 f_coord;
out vec4 fragColor;

vec3 octDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        vec2 s = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        n.xy = (1.0 - abs(n.yx)) * s;
    }
    return normalize(n);
}

// Camera space: x right, y forward (depth), z up. Same projection as the engine:
// screen x = halfW + X*f/Y, screen y = halfH - Z*f/Y (canvas pixels, y down).
vec3 viewPosAt(vec2 uv, float depth)
{
    vec2 c = vec2(uv.x * u_screen.x, (1.0 - uv.y) * u_screen.y);
    return vec3((c.x - u_proj.x) * depth / u_proj.z, depth, (u_proj.y - c.y) * depth / u_proj.z);
}

vec2 projectUV(vec3 P)
{
    vec2 c = vec2(u_proj.x + P.x * u_proj.z / P.y, u_proj.y - P.z * u_proj.z / P.y);
    return vec2(c.x / u_screen.x, 1.0 - c.y / u_screen.y);
}

void main(void)
{
    // Debug view (r_reflectionStrength < 0): green = hit, red = reflective but missed,
    // blue = solid but not reflective, black = no data.
    bool dbg = u_strength < 0.0;
    fragColor = vec4(0.0);
    vec4 d = texture(tex2, f_uv);
    if (d.a < 0.5) return;
    if (d.z <= 0.0) { if (dbg) fragColor = vec4(0.0, 0.0, 0.5, 0.0); return; }

    vec3 N = octDecode(d.xy);
    vec3 P = viewPosAt(f_uv, d.z);
    vec3 V = normalize(P);                           // camera to surface
    vec3 R = normalize(reflect(V, N));
    if (R.y < 0.02) { if (dbg) fragColor = vec4(0.5, 0.0, 0.5, 0.0); return; } // heading back at the camera

    // How much this surface reflects: wet floors most, walls a little, more at grazing angles.
    float floorness = smoothstep(0.55, 0.95, dot(N, u_upCam));
    float refl = clamp(u_wetness, 0.0, 1.0) * mix(0.2, 1.0, floorness);
    float fresnel = mix(0.2, 1.0, pow(1.0 - clamp(dot(-V, N), 0.0, 1.0), 4.0));
    // Roughness: floors use the base value, walls and slopes are rougher. Rough surfaces
    // reflect a blurred scene and a bit less of it, which reads as a wet sheen, not a mirror.
    float rough = clamp(u_roughness + (1.0 - floorness) * 0.3, 0.0, 1.0);
    float amount = refl * fresnel * abs(u_strength) * mix(1.0, 0.6, rough);
    if (amount < 0.01) { if (dbg) fragColor = vec4(0.0, 0.5, 0.5, 0.0); return; } // cyan: amount ~0

    // March in camera space with growing steps, then refine the hit with a binary search.
    float stepLen = max(0.004, d.z * 0.015);
    vec3 Q = P + N * 0.002;
    vec3 prevQ = Q;
    vec2 hitUV = vec2(-1.0);
    float travel = 0.0;
    for (int i = 0; i < 32; i++) // was 48; steps grow faster to cover the same distance
    {
        travel = float(i) / 32.0;
        prevQ = Q;
        Q += R * stepLen;
        stepLen *= 1.15;
        if (Q.y <= 0.001) break;
        vec2 uv = projectUV(Q);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;
        vec4 s = texture(tex2, uv);
        if (s.a < 0.5) continue;
        float sceneDepth = abs(s.z);
        float behind = Q.y - sceneDepth;
        // Tight tolerance, and only surfaces facing the ray count as hits. A ray skimming
        // past a thin light strip used to "hit" it from behind, and every floor pixel along
        // that line grabbed the same strip end: a diagonal smear instead of a reflection.
        if (behind > 0.0 && behind < max(0.01, stepLen * 1.0)
            && dot(octDecode(s.xy), R) < -0.05)
        {
            vec3 a = prevQ, b = Q;
            for (int j = 0; j < 4; j++)
            {
                vec3 m = (a + b) * 0.5;
                vec2 muv = projectUV(m);
                float md = abs(texture(tex2, muv).z);
                if (m.y > md) b = m; else a = m;
            }
            hitUV = projectUV(b);
            break;
        }
    }
    if (hitUV.x < 0.0) { if (dbg) fragColor = vec4(0.6, 0.0, 0.0, 0.0); return; }
    if (dbg) { fragColor = vec4(0.0, 0.7, 0.0, 0.0); return; }

    // Fade near screen edges, where the reflected thing would be off-screen.
    vec2 edge = min(hitUV, 1.0 - hitUV);
    float edgeFade = smoothstep(0.0, 0.08, min(edge.x, edge.y));

    // Long rays are less reliable and, on real surfaces, blurrier: fade and soften them.
    float travelFade = 1.0 - smoothstep(0.5, 1.0, travel);
    float blurAmt = clamp(rough + travel * 0.3, 0.0, 1.0);
    vec3 col = mix(texture(tex, hitUV).rgb, texture(tex3, hitUV).rgb, blurAmt);
    fragColor = vec4(col * amount * edgeFade * travelFade, 0.0);
}
