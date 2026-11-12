#ifdef GL_ARB_texture_gather
#define HAS_TEXTUREGATHER
#endif

#ifdef HAS_TEXTUREGATHER
vec4 impl_textureGather(sampler2D tex, vec2 uv)
{
    return textureGather(tex, uv);
}
#else
float modI(float a,float b) {
    float m=a-floor((a+0.5)/b)*b;
    return floor(m+0.5);
}

vec4 impl_textureGather(sampler2D tex, vec2 uv)
{
    ivec2 idims = textureSize(tex,0) - ivec2(1, 1);
    vec2 dims = vec2(idims);

    ivec2 base = ivec2(dims*uv);
    if (base.x < 0) {
        //base.x = -base.x;
    }
    if (base.y < 0) {
        //base.y = -base.y;
    }

    base.x = int(modI(float(base.x), dims.x));
    base.y = int(modI(float(base.y), dims.y));

    return vec4(texelFetch(tex,base+ivec2(0,1),0).x,
        texelFetch(tex,base+ivec2(1,1),0).x,
        texelFetch(tex,base+ivec2(1,0),0).x,
        texelFetch(tex,base+ivec2(0,0),0).x
    );
}
#endif

#define LIGHT_DIVISOR (6.0)
#define TEX_MODE_TEST 0
#define TEX_MODE_WORLDPAL 1
#define TEX_MODE_BILINEAR 2
#define TEX_MODE_16BPP 5
#define TEX_MODE_BILINEAR_16BPP 6

#define D3DBLEND_ONE             (2)
#define D3DBLEND_SRCALPHA        (5)
#define D3DBLEND_INVSRCALPHA     (6)

uniform sampler2D tex;
uniform sampler2D texEmiss;
uniform sampler2D worldPalette;
uniform sampler2D worldPaletteLights;
uniform sampler2D displacement_map;
uniform int tex_mode;
uniform int blend_mode;
uniform vec3 colorEffects_tint;
uniform vec3 colorEffects_filter;
uniform float colorEffects_fade;
uniform vec3 colorEffects_add;
uniform vec3 emissiveFactor;
uniform vec4 albedoFactor;
uniform float displacement_factor;
uniform float light_mult;
uniform vec2 iResolution;

// Added: Custom RGB lightsaber rendering.
uniform int saberRgbEnabled;
uniform vec3 saberRgbColor;

in vec4 f_color;
in float f_light;
in vec2 f_uv;
in vec3 f_coord;

// Added (remaster): per-pixel dynamic lighting (blaster bolts, sabers, explosions, headlights).
// Static, baked level light still arrives per vertex in f_color; only dynamic lights are
// computed here, plus Blinn-Phong highlights for the wet look.
noperspective in float f_invz;          // 1/(32 * camera-space depth)
uniform int u_ppl;                      // 1 = per-pixel lighting on
uniform int u_pplNumLights;
uniform vec4 u_pplLightPos[32];         // camera space xyz (x right, y forward, z up), w = radius
uniform vec4 u_pplLightCol[32];         // rgb colour, w = intensity
uniform float u_pplAtten;               // linear falloff per unit distance (as the original)
uniform vec4 u_pplProj;                 // half screen width, half screen height, focal length
uniform vec4 u_pplScreen;               // canvas width, canvas height, framebuffer width, height
uniform float u_pplSpecular;            // overall highlight strength
uniform float u_pplWetness;             // 0 = matte, 1 = soaked
uniform int u_pplNumGlow;               // glow lights: static lights at glowing surfaces
uniform vec4 u_pplGlowPos[24];          // camera space xyz, w = radius
uniform vec4 u_pplGlowCol[24];          // rgb colour, w = intensity
uniform int u_ssrWrite;                 // 1 = write reflection data
uniform vec3 u_pplUpCam;                // world up in camera space
// Added (remaster): HD material maps. ORM = glTF convention (R AO, G roughness, B metal).
uniform sampler2D texOrm;
uniform sampler2D texNormal;
uniform int u_hasOrm;
uniform int u_hasNormal;

vec2 ssr_octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 s = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return (n.z >= 0.0) ? n.xy : (1.0 - abs(n.yx)) * s;
}

// Rebuild the camera-space position from screen position and depth, inverting the
// engine's projection: x = halfW + X*f/Y, y = halfH - Z*f/Y.
vec3 ppl_viewPos()
{
    float depth = 1.0 / (32.0 * max(f_invz, 1e-7));
    vec2 c = vec2(gl_FragCoord.x * (u_pplScreen.x / u_pplScreen.z),
                  (u_pplScreen.w - gl_FragCoord.y) * (u_pplScreen.y / u_pplScreen.w));
    return vec3((c.x - u_pplProj.x) * depth / u_pplProj.z,
                depth,
                (u_pplProj.y - c.y) * depth / u_pplProj.z);
}

void ppl_compute(vec3 P, vec3 N, float rough, float metal, out vec3 diff, out vec3 spec)
{
    diff = vec3(0.0);
    spec = vec3(0.0);
    vec3 V = normalize(-P);
    if (dot(N, V) < 0.0) N = -N;
    // Roughness drives highlight sharpness and strength (Blinn-Phong exponent from roughness).
    float r4 = max(rough * rough * rough * rough, 1e-4);
    float shininess = clamp(2.0 / r4 - 2.0, 2.0, 512.0);
    float specScale = u_pplSpecular * (1.0 - rough) * (1.0 - rough) * 2.0;

    for (int i = 0; i < u_pplNumLights; i++)
    {
        vec3 Lv = u_pplLightPos[i].xyz - P;
        float d = length(Lv);
        if (d >= u_pplLightPos[i].w) continue;
        float att = u_pplLightCol[i].w - d * u_pplAtten;   // same falloff as the original
        if (att <= 0.0) continue;
        vec3 L = Lv / max(d, 1e-5);
        float ndl = dot(N, L);
        // Slightly wrapped diffuse: the original applied dynamic light to level geometry
        // with no angle term at all, so faces turned away shouldn't go fully black.
        diff += u_pplLightCol[i].rgb * att * clamp((ndl + 0.3) / 1.3, 0.0, 1.0);
        if (ndl > 0.0)
        {
            vec3 H = normalize(L + V);
            spec += u_pplLightCol[i].rgb * att * pow(max(dot(N, H), 0.0), shininess) * specScale;
        }
    }

    // Glow lights. Their glow is already in the baked light, so they add mostly
    // highlights (the wet look) and only a touch of extra light. Smooth falloff to zero
    // at the radius, since they're broad, soft sources.
    for (int i = 0; i < u_pplNumGlow; i++)
    {
        vec3 Lv = u_pplGlowPos[i].xyz - P;
        float d = length(Lv);
        float r = u_pplGlowPos[i].w;
        if (d >= r) continue;
        float f = 1.0 - d / r;
        float att = u_pplGlowCol[i].w * f * f;
        vec3 L = Lv / max(d, 1e-5);
        float ndl = dot(N, L);
        if (ndl <= 0.0) continue;
        diff += u_pplGlowCol[i].rgb * att * ndl * 0.15;
        vec3 H = normalize(L + V);
        // Slightly broader and stronger highlight than point lights: these are bright
        // surfaces, not points, and they're what makes a static scene read as wet.
        spec += u_pplGlowCol[i].rgb * att * pow(max(dot(N, H), 0.0), shininess * 0.5) * specScale * 2.0;
    }
}

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 fragColorEmiss;
layout(location = 2) out vec4 fragColorPos;
layout(location = 3) out vec4 fragColorNormal;
// Added (remaster): reflection data for the screen-space reflection pass:
// octahedral camera-space normal (xy), depth (z, negative = this surface doesn't reflect),
// alpha 1 on solid surfaces so blending writes it as-is, all zero on see-through ones so
// blending leaves it alone.
layout(location = 4) out vec4 fragColorSSR;
// Added (remaster): material for the reflection pass (R = roughness, G = metalness, A = 1 on solid).
layout(location = 5) out vec4 fragColorMat;

// Added (remaster): procedural lightsaber blade. The blade arrives as one camera-facing capsule
// quad (rdPolyline_DrawBlade): f_uv is blade space in glow radii (x across, y along from the
// base), f_color is the blade colour with (blade length / glow radius / 32) in alpha. The
// distance to the core segment makes a white-hot core, a saturated sheath and a soft halo;
// two layers of noise drift along the blade with a hum flicker, so the plasma looks alive.
uniform int saberBlade;
uniform float saberStrength;
uniform float u_time;

float saberHash(float n) { return fract(sin(n * 12.9898) * 43758.5453); }
float saberNoise(float x)
{
    float i = floor(x);
    float f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(saberHash(i), saberHash(i + 1.0), f);
}

// Swing trail ribbon: u = 10 + age (0 = now, 1 = oldest), v = 0 at the base, 1 at the tip.
void saberTrailMain()
{
    vec3 col = f_color.rgb;
    float age = clamp(f_uv.x - 10.0, 0.0, 1.0);
    float along = clamp(f_uv.y, 0.0, 1.0);
    float fade = pow(1.0 - age, 1.6) * smoothstep(0.0, 0.25, along) * (1.0 - smoothstep(0.92, 1.0, along) * 0.5);
    fade *= f_color.a; // sweep-width falloff from rdPolyline_DrawTrail
    vec3 c = mix(vec3(1.0), col, 0.4 + 0.6 * age) * fade * 0.4 * saberStrength;
    fragColor = vec4(c, 0.0);
    fragColorEmiss = vec4(col * fade * 0.3 * saberStrength, 0.0);
    fragColorPos = vec4(0.0);
    fragColorNormal = vec4(0.0);
    fragColorSSR = vec4(0.0);
    fragColorMat = vec4(0.0);
}

void saberBladeMain()
{
    if (f_uv.x >= 9.0) { saberTrailMain(); return; }
    vec3 col = f_color.rgb;
    float lr = f_color.a * 32.0;
    float along = clamp(f_uv.y, 0.0, lr);
    float d = length(vec2(f_uv.x, f_uv.y - along));
    float t = u_time;
    float n = saberNoise(along * 2.3 - t * 9.0) * 0.6 + saberNoise(along * 5.7 + t * 14.0) * 0.4;
    float hum = 0.92 + 0.08 * sin(t * 71.0) * sin(t * 29.0 + 1.7);
    float coreR = 0.06 * (0.9 + 0.25 * n);
    float core = 1.0 - smoothstep(coreR * 0.5, coreR * 1.2, d); // (reversed smoothstep edges are undefined in GLSL)
    float sheath = exp(-pow(d / (0.12 + 0.03 * n), 2.0));
    float halo = exp(-d * 5.0) * 0.16;
    float edge = 1.0 - smoothstep(0.8, 1.0, d);
    float s = saberStrength * edge;
    s *= exp(-max(-f_uv.y, 0.0) * 5.0); // below the emitter the glow dies quickly instead of washing over the hilt
    vec3 sat = col * col;                     // deeper colour for the sheath, so it doesn't wash to white
    float ripple = 0.8 + 0.4 * saberNoise(along * 9.0 - t * 23.0); // fast plasma ripples in the sheath
    vec3 c = ((sat * 0.75 + col * 0.15) * sheath * hum * ripple + col * halo + vec3(1.0) * core * 0.95) * s;
    fragColor = vec4(c, 0.0); // premultiplied with alpha 0: purely additive light
    fragColorEmiss = vec4((col * (sheath * 0.45 + halo * 1.3) + vec3(core) * 0.2) * s, 0.0);
    fragColorPos = vec4(0.0);
    fragColorNormal = vec4(0.0);
    fragColorSSR = vec4(0.0);
    fragColorMat = vec4(0.0);
}

// Added (remaster): volumetric sky. Sky surfaces (sithSky preset levels) show the cloud
// panorama (sky_f.glsl) by view direction, over a clear-sky gradient with the moon or sun
// disc, stars, and lightning lighting the clouds from inside. Writes a sky marker
// (depth -1000) to the reflection target so haze and light shafts can tell sky from wall.
uniform int u_sky;
uniform sampler2D u_skyPano;
uniform vec3 u_skyWX;          // world axes in camera space
uniform vec3 u_skyWY;
uniform vec3 u_skyWZ;
uniform vec3 u_skySunDir;      // world
uniform vec3 u_skySunCol;
uniform vec4 u_skyZenith;      // rgb, w = stars
uniform vec4 u_skyHorizon;     // rgb, w = sun/moon disc radius (radians)
uniform vec4 u_skyBelowExp;   // below-horizon colour, w = exposure
uniform vec4 u_skyFlash;       // world dir, intensity

float skyHash(vec3 p)
{
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

void skyMain()
{
    vec2 c = vec2(gl_FragCoord.x * (u_pplScreen.x / u_pplScreen.z),
                  (u_pplScreen.w - gl_FragCoord.y) * (u_pplScreen.y / u_pplScreen.w));
    vec3 dc = normalize(vec3((c.x - u_pplProj.x) / u_pplProj.z, 1.0, (u_pplProj.y - c.y) / u_pplProj.z));
    vec3 dw = normalize(vec3(dot(dc, u_skyWX), dot(dc, u_skyWY), dot(dc, u_skyWZ)));

    // Clear sky behind the clouds.
    float up = clamp(dw.z, 0.0, 1.0);
    vec3 bg = mix(u_skyHorizon.rgb, u_skyZenith.rgb, pow(up, 0.45));
    float cosA = dot(dw, u_skySunDir);
    bg += normalize(u_skySunCol + 1e-4) * (pow(max(cosA, 0.0), 400.0) * 0.35 + pow(max(cosA, 0.0), 40.0) * 0.05);
    float ang = acos(clamp(cosA, -1.0, 1.0));
    float disc = 1.0 - smoothstep(u_skyHorizon.w * 0.85, u_skyHorizon.w, ang);
    // The disc at a fixed, believable brightness (the light it casts on clouds is separate).
    vec3 discCol = normalize(u_skySunCol + 1e-4) * 1.6 * (0.85 + 0.15 * skyHash(floor(dw * 900.0)));
    bg += discCol * disc;
    // Stars: sparse points on a fine direction grid.
    if (u_skyZenith.w > 0.0)
    {
        vec3 g = floor(dw * 420.0);
        float h = skyHash(g);
        float star = smoothstep(0.9965, 1.0, h) * u_skyZenith.w * smoothstep(0.02, 0.25, dw.z);
        bg += vec3(0.8, 0.85, 1.0) * star * (0.5 + 1.5 * skyHash(g + 7.0));
    }

    // Clouds from the panorama (same mapping as sky_f.glsl).
    const float ELEV_MIN = 0.12;
    float az = atan(dw.y, dw.x) / 6.2831853;
    float el = asin(clamp(dw.z, -1.0, 1.0));
    vec2 puv = vec2(fract(az), clamp((el + ELEV_MIN) / (1.5707963 + ELEV_MIN), 0.0, 1.0));
    vec4 cl = textureLod(u_skyPano, puv, 0.0);
    vec3 col = (bg * cl.a + cl.rgb) * u_skyBelowExp.w;

    // Lightning: lights the cloud it's in, flickering, instantly (not through the panorama).
    if (u_skyFlash.w > 0.0)
    {
        float fa = acos(clamp(dot(dw, u_skyFlash.xyz), -1.0, 1.0));
        float glow = exp(-fa * 3.5) + 0.25 * exp(-fa * 0.8);
        col += vec3(0.75, 0.8, 1.0) * u_skyFlash.w * glow * (1.0 - cl.a) * 2.5;
    }

    // Below the horizon: the murk the deck sits on.
    float below = smoothstep(0.0, 1.0, clamp(-dw.z / 0.1, 0.0, 1.0));
    col = mix(col, u_skyBelowExp.rgb * u_skyBelowExp.w, below);

    float bright = max(max(col.r, col.g), col.b);
    // Glow: the whole sky feeds bloom a little, so it wraps softly over the edges of
    // buildings and rock in front of it (how a bright sky reads on camera); the brightest
    // breaks feed it more. Bloom also adds this back onto the sky itself (over a
    // large area, measured ~10x), so the base colour is reduced to match and the sky keeps its brightness.
    const float SKY_GLOW = 0.25;
    vec3 emiss = col * (SKY_GLOW + smoothstep(0.6, 1.4, bright) * 0.6);
    fragColor = vec4(col / (1.0 + 10.4 * SKY_GLOW), 1.0);
    fragColorEmiss = vec4(emiss, 1.0);
    // Very far for ambient occlusion (zero depth made it treat the sky as the nearest surface
    // and darken it), facing the camera.
    fragColorPos = vec4(gl_FragCoord.x / iResolution.x, gl_FragCoord.y / iResolution.y, 1.0e4, 1.0);
    fragColorNormal = vec4(0.0, 0.0, 1.0, 1.0);
    fragColorSSR = vec4(0.0, 0.0, -1000.0, 1.0);
    fragColorMat = vec4(1.0, 0.0, 0.0, 1.0);
}

float luminance(vec3 c_rgb)
{
    const vec3 W = vec3(0.2125, 0.7154, 0.0721);
    return dot(c_rgb, W);
}

vec3 normals(vec3 pos) {
    vec3 fdx = dFdx(pos);
    vec3 fdy = dFdy(pos);
    return normalize(cross(fdx, fdy));
}

mat3 construct_tbn(vec3 vp_normal, vec3 adjusted_coords)
{
    vec3 n = normalize(vp_normal);

    vec3 dp1 = dFdx(adjusted_coords);
    vec3 dp2 = dFdy(adjusted_coords);
    vec2 duv1 = dFdx(f_uv);
    vec2 duv2 = dFdy(f_uv);

    vec3 dp2perp = cross(dp2, n);
    vec3 dp1perp = cross(n, dp1);

    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;

    float invmax = inversesqrt(max(dot(t, t), dot(b, b)));
    return mat3(t * invmax, b * invmax, n);
}

vec2 parallax_mapping(vec2 tc, vec3 vp_normal, vec3 adjusted_coords)
{
    /*if (f_coord.x < 0.5) {
        return tc;
    }*/

    // The injector world space view position is always considered (0, 0, 0):
    vec3 view_dir = -normalize(transpose(construct_tbn(vp_normal, adjusted_coords)) * adjusted_coords);

    const float min_layers = 32.0;
    const float max_layers = 128.0;
    float num_layers = mix(max_layers, min_layers, abs(dot(vec3(0.0, 0.0, 1.0), view_dir)));

    float layer_depth = 1.0 / num_layers;
    float current_layer_depth = 0.0;
    vec2 shift_per_layer = (view_dir.xy / view_dir.z) * displacement_factor;
    vec2 d_tc = shift_per_layer / num_layers;

    vec2 current_tc = tc;
    float current_sample = texture(displacement_map, current_tc).r;

    while(current_layer_depth < current_sample) {
        current_tc -= d_tc;
        current_sample = texture(displacement_map, current_tc).r;
        current_layer_depth += layer_depth;
    }

    vec2 prev_tc = current_tc + d_tc;

    float after_col_depth = current_sample - current_layer_depth;
    float before_col_depth = texture(displacement_map, prev_tc).r - current_layer_depth + layer_depth;

    float a = after_col_depth / (after_col_depth - before_col_depth);
    vec2 adj_tc = mix(current_tc, prev_tc, a);

    return adj_tc;
}

#ifdef CAN_BILINEAR_FILTER
vec4 bilinear_paletted()
{
    // Get texture size in pixels:
    vec2 colorTextureSize = vec2(textureSize(tex, 0));

    // Convert UV coordinates to pixel coordinates and get pixel index of top left pixel (assuming UVs are relative to top left corner of texture)
    vec2 pixCoord = f_uv * colorTextureSize - 0.5f;    // First pixel goes from -0.5 to +0.4999 (0.0 is center) last pixel goes from (size - 1.5) to (size - 0.5000001)
    vec2 originPixCoord = floor(pixCoord);              // Pixel index coordinates of bottom left pixel of set of 4 we will be blending

    // For Gather we want UV coordinates of bottom right corner of top left pixel
    vec2 gUV = (originPixCoord + 1.0f) / colorTextureSize;

    vec4 gIndex   = impl_textureGather(tex, gUV);

    vec4 c00   = texture(worldPalette, vec2(gIndex.w, 0.5));
    vec4 c01 = texture(worldPalette, vec2(gIndex.x, 0.5));
    vec4 c11  = texture(worldPalette, vec2(gIndex.y, 0.5));
    vec4 c10 = texture(worldPalette, vec2(gIndex.z, 0.5));

    if (blend_mode == D3DBLEND_SRCALPHA || blend_mode == D3DBLEND_INVSRCALPHA) {
        if (gIndex.x == 0.0) {
            c01.a = 0.0;
        }
        if (gIndex.y == 0.0) {
            c11.a = 0.0;
        }
        if (gIndex.z == 0.0) {
            c10.a = 0.0;
        }
        if (gIndex.w == 0.0) {
            c00.a = 0.0;
        }
    }

    vec2 filterWeight = pixCoord - originPixCoord;
 
    // Bi-linear mixing:
    vec4 temp0 = mix(c01, c11, filterWeight.x);
    vec4 temp1 = mix(c00, c10, filterWeight.x);
    vec4 blendColor = mix(temp1, temp0, filterWeight.y);

    return vec4(blendColor.r, blendColor.g, blendColor.b, blendColor.a);
}

vec4 bilinear_paletted_light(float index)
{
    // Makes sure light is in a sane range
    float light = clamp(f_light, 0.0, 1.0);

    // Special case for lightsabers
    //if (index * 255.0 >= 16.0 && index * 255.0 < 17.0)
    //    light = 0.0;

    // Take the fragment light, and divide by 4.0 to select for colors
    // which glow in the dark
    float light_idx = light / LIGHT_DIVISOR;

    // Get texture size in pixels:
    vec2 colorTextureSize = vec2(textureSize(tex, 0));

    // Convert UV coordinates to pixel coordinates and get pixel index of top left pixel (assuming UVs are relative to top left corner of texture)
    vec2 pixCoord = f_uv * colorTextureSize - 0.5f;    // First pixel goes from -0.5 to +0.4999 (0.0 is center) last pixel goes from (size - 1.5) to (size - 0.5000001)
    vec2 originPixCoord = floor(pixCoord);              // Pixel index coordinates of bottom left pixel of set of 4 we will be blending

    // For Gather we want UV coordinates of bottom right corner of top left pixel
    vec2 gUV = (originPixCoord + 1.0f) / colorTextureSize;

    vec4 gIndex   = impl_textureGather(tex, gUV);

    vec4 c00   = texture(worldPalette, vec2(texture(worldPaletteLights, vec2(gIndex.w, light_idx)).r, 0.5));
    vec4 c01 = texture(worldPalette, vec2(texture(worldPaletteLights, vec2(gIndex.x, light_idx)).r, 0.5));
    vec4 c11  = texture(worldPalette, vec2(texture(worldPaletteLights, vec2(gIndex.y, light_idx)).r, 0.5));
    vec4 c10 = texture(worldPalette, vec2(texture(worldPaletteLights, vec2(gIndex.z, light_idx)).r, 0.5));

    /*if (blend_mode == D3DBLEND_SRCALPHA || blend_mode == D3DBLEND_INVSRCALPHA) {
        if (gIndex.x == 0.0) {
            c01.a = 0.0;
        }
        if (gIndex.y == 0.0) {
            c11.a = 0.0;
        }
        if (gIndex.z == 0.0) {
            c10.a = 0.0;
        }
        if (gIndex.w == 0.0) {
            c00.a = 0.0;
        }
    }*/

    vec2 filterWeight = pixCoord - originPixCoord;
 
    // Bi-linear mixing:
    vec4 temp0 = mix(c01, c11, filterWeight.x);
    vec4 temp1 = mix(c00, c10, filterWeight.x);
    vec4 blendColor = mix(temp1, temp0, filterWeight.y);
    vec4 light_mult_quad = vec4(light_mult, light_mult, light_mult, 1.0);

    return vec4(blendColor.r, blendColor.g, blendColor.b, 1.0) * light_mult_quad ;//* (1.0 - light) * light_mult;
}
#endif

void main(void)
{
    if (saberBlade != 0) { saberBladeMain(); return; } // Added (remaster)
    if (u_sky != 0) { skyMain(); return; } // Added (remaster)
    float originalZ = gl_FragCoord.z / gl_FragCoord.w;
    vec3 adjusted_coords = vec3(f_coord.x/iResolution.x, f_coord.y/iResolution.y, originalZ);
    vec3 adjusted_coords_norms = vec3(gl_FragCoord.x/iResolution.x, gl_FragCoord.y/iResolution.y, 1.0/gl_FragCoord.z);
    vec3 adjusted_coords_parallax = vec3(adjusted_coords_norms.x - 0.5, adjusted_coords_norms.y - 0.5, gl_FragCoord.z);
    vec3 face_normals = normals(adjusted_coords_norms);
    vec3 face_normals_parallax = normals(adjusted_coords_parallax);

    // Added (remaster): position and flat face normal for per-pixel lighting. Derivatives
    // must be taken here, outside any branch.
    vec3 ppl_P = ppl_viewPos();
    vec3 ppl_N = normalize(cross(dFdx(ppl_P), dFdy(ppl_P)));
    vec3 ppl_spec = vec3(0.0);
    if (dot(ppl_N, -ppl_P) < 0.0) ppl_N = -ppl_N;

    // Added (remaster): material. Without an ORM map, surfaces default to fairly rough,
    // non-metal and unoccluded, so rock and plaster don't read as polished.
    vec4 mat_orm = (u_hasOrm != 0) ? texture(texOrm, f_uv) : vec4(1.0, 0.75, 0.0, 1.0);
    float mat_ao = mat_orm.r;
    float mat_metal = mat_orm.b;
    // r_wetness makes the whole world somewhat smoother (wet), on top of the material.
    float mat_rough = clamp(mat_orm.g * (1.0 - 0.6 * clamp(u_pplWetness, 0.0, 1.0)), 0.04, 1.0);
    // Normal map: bend the face normal with a tangent frame built from screen derivatives
    // of position and UV (no tangents in the vertex data).
    if (u_hasNormal != 0)
    {
        vec3 tn = texture(texNormal, f_uv).xyz * 2.0 - 1.0;
        vec3 dp1 = dFdx(ppl_P), dp2 = dFdy(ppl_P);
        vec2 duv1 = dFdx(f_uv), duv2 = dFdy(f_uv);
        vec3 dp2perp = cross(dp2, ppl_N), dp1perp = cross(ppl_N, dp1);
        vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
        vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
        float invmax = inversesqrt(max(dot(T, T), dot(B, B)) + 1e-12);
        ppl_N = normalize(mat3(T * invmax, B * invmax, ppl_N) * tn);
    }

    vec2 adj_texcoords = f_uv;
    if(displacement_factor != 0.0) {
        adj_texcoords = parallax_mapping(f_uv, face_normals_parallax, adjusted_coords_parallax);
    }

    vec4 sampled = texture(tex, adj_texcoords);
    vec4 sampledEmiss = texture(texEmiss, adj_texcoords);
    vec4 sampled_color = vec4(1.0, 1.0, 1.0, 1.0);
    vec4 vertex_color = f_color;
    float index = sampled.r;
    vec4 palval = texture(worldPalette, vec2(index, 0.5));
    vec4 color_add = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 color_add_emiss = vec4(0.0, 0.0, 0.0, 0.0);

    if (tex_mode == TEX_MODE_TEST) {
        sampled_color = vec4(1.0, 1.0, 1.0, 1.0);
    }
    else if (tex_mode == TEX_MODE_16BPP
    || tex_mode == TEX_MODE_BILINEAR_16BPP
    )
    {
        sampled_color = vec4(sampled.b, sampled.g, sampled.r, sampled.a);
    }
    else if (tex_mode == TEX_MODE_WORLDPAL
#ifndef CAN_BILINEAR_FILTER
    || tex_mode == TEX_MODE_BILINEAR
#endif
    )

    {
        if (index == 0.0 && (blend_mode == D3DBLEND_SRCALPHA || blend_mode == D3DBLEND_INVSRCALPHA))
            discard;

        // Makes sure light is in a sane range
        float light = clamp(f_light, 0.0, 1.0);

        // Special case for lightsabers
        //if (index * 255.0 >= 16.0 && index * 255.0 < 17.0)
        //    light = 0.0;

        // Take the fragment light, and divide by 4.0 to select for colors
        // which glow in the dark
        float light_idx = light / LIGHT_DIVISOR;

        // Get the shaded palette index
        float light_worldpalidx = texture(worldPaletteLights, vec2(index, light_idx)).r;

        // Now take our index and look up the corresponding palette value
        vec4 lightPalval = texture(worldPalette, vec2(light_worldpalidx, 0.5));

        // Add more of the emissive color depending on the darkness of the fragment
        color_add = (lightPalval  * light_mult); // * (1.0 - light)
        sampled_color = palval;
    }
#ifdef CAN_BILINEAR_FILTER
    else if (tex_mode == TEX_MODE_BILINEAR)
    {
        sampled_color = bilinear_paletted();
        color_add = bilinear_paletted_light(index);

        if (sampled_color.a < 0.01) {
            discard;
        }
    }
#endif

    vec4 albedoFactor_copy = albedoFactor;

    if (blend_mode == D3DBLEND_INVSRCALPHA)
    {
        if (vertex_color.a < 0.01) {
            discard;
        }
        //albedoFactor_copy.a = (1.0 - albedoFactor_copy.a);
        //vertex_color.a = (1.0 - vertex_color.a);
        //sampled_color.a = (1.0 - sampled_color.a);
    }

    if (blend_mode != D3DBLEND_SRCALPHA && blend_mode != D3DBLEND_INVSRCALPHA && vertex_color.a != 0.0)
    {
        vertex_color.a = 1.0;
    }

    vec4 main_color = (sampled_color * vertex_color);
    vec4 effectAdd_color = vec4(colorEffects_add.r, colorEffects_add.g, colorEffects_add.b, 0.0);
    
    main_color *= albedoFactor_copy;

    // Added (remaster): add dynamic lights per pixel on top of the baked vertex light.
    if (u_ppl != 0 && f_invz > 0.0 && tex_mode != TEX_MODE_TEST)
    {
        vec3 ppl_diff;
        ppl_compute(ppl_P, ppl_N, mat_rough, mat_metal, ppl_diff, ppl_spec);
        // Metals have little diffuse; ambient occlusion darkens crevices.
        ppl_diff *= (1.0 - 0.8 * mat_metal);
        vec3 lit = min(vertex_color.rgb + ppl_diff, vec3(1.0)) * mix(1.0, mat_ao, 0.8);
        main_color.rgb = sampled_color.rgb * lit * albedoFactor_copy.rgb;

        // No highlights on see-through or additive faces (sprites, bolts, glass). (Full vertex
        // light doesn't mean a light source: brightly lit floors are at full light too.)
        // (D3DBLEND_ONE is what solid surfaces use; only SRCALPHA/INVSRCALPHA are see-through.)
        bool bNoSpec = (blend_mode == D3DBLEND_SRCALPHA || blend_mode == D3DBLEND_INVSRCALPHA);
        if (bNoSpec) ppl_spec = vec3(0.0);
        // Highlights take the surface colour on metals, stay mostly white otherwise.
        main_color.rgb += ppl_spec * mix(vec3(1.0), sampled_color.rgb, mix(0.35, 1.0, mat_metal));
    }
    float should_write_normals = 1.0;
    float orig_alpha = main_color.a;

    if (main_color.a < 0.01 && sampledEmiss.r == 0.0 && sampledEmiss.g == 0.0 && sampledEmiss.b == 0.0) {
        discard;
    }
    
    // Translucent faces (water, glass): the original D3D renderer drew them with
    // SRCBLEND=SRCALPHA, DESTBLEND=INVSRCALPHA and MODULATE, so the face's vertex
    // alpha (90/255) is how much of the face shows, about 35%. The blend here is
    // GL_ONE, GL_ONE_MINUS_SRC_ALPHA, so the color is premultiplied. Using
    // 1 - alpha instead draws water about 65% solid.
    if (blend_mode == D3DBLEND_INVSRCALPHA)
    {
        main_color.rgb *= main_color.a;
    }

    //if (blend_mode == D3DBLEND_SRCALPHA || blend_mode == D3DBLEND_INVSRCALPHA)
    {
        should_write_normals = main_color.a;
    }

    //if (sampledEmiss.r != 0.0 || sampledEmiss.g != 0.0 || sampledEmiss.b != 0.0)
    {
        color_add.rgb += sampledEmiss.rgb * emissiveFactor * 0.1;
    }

    if (sampledEmiss.r != 0.0 || sampledEmiss.g != 0.0 || sampledEmiss.b != 0.0)
    {
        color_add_emiss.rgb += sampledEmiss.rgb * 0.1;
    }

    // Added: Custom RGB saber color with preserved white core.
if (saberRgbEnabled != 0)
{
    vec3 originalColor = main_color.rgb;

    float brightness = max(
        originalColor.r,
        max(originalColor.g, originalColor.b)
    );

    float lowestChannel = min(
        originalColor.r,
        min(originalColor.g, originalColor.b)
    );

    // White and nearly white pixels have very little saturation.
    float saturation = (brightness > 0.001)
        ? (brightness - lowestChannel) / brightness
        : 0.0;

    // Preserve nearly white pixels and recolor saturated pixels.
    float recolorAmount = smoothstep(0.08, 0.40, saturation);

    vec3 customColor = saberRgbColor * brightness;

    main_color.rgb = mix(
        originalColor,
        customColor,
        recolorAmount
    );
}

    fragColor = main_color + effectAdd_color;// + color_add;

    color_add.a = orig_alpha;

    float luma = luminance(color_add.rgb) * 0.5;// * 4.0;

    if (emissiveFactor.r != 0.0 || emissiveFactor.g != 0.0 || emissiveFactor.b != 0.0)
    {
        //color_add = vec4(1.0, 1.0, 1.0, 1.0);
        luma = 1.0;
    }
    else
    {
        // The emissive maps also include slight amounts of darkly-rendered geometry,
        // so we want to ramp the amount that gets added based on luminance/brightness.
        

        color_add.r *= luma;
        color_add.g *= luma;
        color_add.b *= luma;
    }

    vec3 tint = normalize(colorEffects_tint + 1.0) * sqrt(3.0);

    color_add.r *= tint.r;
    color_add.g *= tint.g;
    color_add.b *= tint.b;

    color_add.r *= colorEffects_fade;
    color_add.g *= colorEffects_fade;
    color_add.b *= colorEffects_fade;

    color_add.r *= colorEffects_filter.r;
    color_add.g *= colorEffects_filter.g;
    color_add.b *= colorEffects_filter.b;

    //color_add = vec4(0.0, 0.0, 0.0, 1.0);

    // Dont include any windows or transparent objects in emissivity output
    if (luma < 0.01 && orig_alpha < 0.5 && (blend_mode == D3DBLEND_SRCALPHA || blend_mode == 6))
    {
        color_add = vec4(0.0, 0.0, 0.0, 0.0);
    }

    if (blend_mode == D3DBLEND_INVSRCALPHA) {
        //color_add.a = (1.0 - color_add.a);
        //should_write_normals = 1.0 - should_write_normals;
    }

    // Added: Apply the same RGB override to the saber's glow.
vec4 finalEmiss = color_add_emiss + color_add;

if (saberRgbEnabled != 0)
{
    float emissBrightness = max(
        finalEmiss.r,
        max(finalEmiss.g, finalEmiss.b)
    );

    finalEmiss.rgb = saberRgbColor * emissBrightness;
}

finalEmiss.rgb += ppl_spec * 0.5; // Added (remaster): wet glints feed the bloom

    // Added (remaster): reflection data
    fragColorSSR = vec4(0.0);
    fragColorMat = vec4(0.0);
    if (u_ssrWrite != 0 && f_invz > 0.0)
    {
        bool bSolid = (blend_mode != D3DBLEND_SRCALPHA && blend_mode != D3DBLEND_INVSRCALPHA
                       && orig_alpha > 0.99);
        if (bSolid)
        {
            vec3 n = ppl_N;
            if (dot(n, -ppl_P) < 0.0) n = -n;
            // Test-textured surfaces don't reflect. (Full vertex light can't be used to spot
            // light sources: brightly lit floors are at full light too.)
            bool bReflects = tex_mode != TEX_MODE_TEST;
            fragColorSSR = vec4(ssr_octEncode(n), bReflects ? ppl_P.y : -ppl_P.y, 1.0);
            fragColorMat = vec4(mat_rough, mat_metal, 0.0, 1.0);
        }
    }
fragColorEmiss = finalEmiss;

    //fragColor = vec4(face_normals_parallax.x, face_normals_parallax.y, face_normals_parallax.z, 1.0);
    //fragColor = vec4(face_normals*0.5 + 0.5,1.0);
    //vec4 test_norms = (main_color + effectAdd_color);
    //test_norms.xyz *= dot(vec3(1.0, 0.0, -0.7), face_normals);
    //fragColor = test_norms;

    fragColorPos = vec4(adjusted_coords.x, adjusted_coords.y, adjusted_coords.z, should_write_normals);
    fragColorNormal = vec4(face_normals, should_write_normals);
    // No gl_FragDepth here: it only wrote back gl_FragCoord.z, the depth the GPU already
    // had, and writing depth from a shader turns off the early depth test, so every
    // hidden pixel ran this whole shader.
}
