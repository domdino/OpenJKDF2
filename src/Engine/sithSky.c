// Added (remaster): volumetric cloud sky presets, lightning and cloud noise. See sithSky.h.

#include "sithSky.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include "jk.h"
#include "World/sithWorld.h"
#include "World/sithSurface.h"
#include "World/jkPlayer.h"
#include "stdPlatform.h"

// Presets, matched by the level's sky material. Colours are display-referred (what the
// renderer writes), so 1.0 is full white.
static const sithSkyPreset sithSky_aPresets[] = {
    // The storm over the Valley of the Jedi approach (Maw, Sarris, the ascent): a dark sky
    // of heavy cloud with the moon breaking through, and lightning inside the cloud deck.
    {
        "storm", {"13cnsky2", NULL},
        215.0f, 24.0f,                      // moon, in view from the Sarris rooftop start
        {0.78f, 0.84f, 1.0f}, 1.5f, 1.1f,
        {0.10f, 0.13f, 0.20f}, {0.22f, 0.25f, 0.31f},
        {0.42f, 0.46f, 0.56f}, {0.07f, 0.08f, 0.10f},
        0.78f, 6.0f, 1.2f, 3.6f, 0.025f,
        0.12f, 4.0f,
        {0.12f, 0.13f, 0.16f},
        1.45f,
    },
    // Nar Shaddaa at night: a hazy, smog-stained sky over the city, ragged high cloud lit
    // orange from below by the city, stars where it's clear.
    {
        "narshadda", {"00starry", NULL},
        210.0f, 18.0f,                      // a low, dim moon
        {0.70f, 0.74f, 0.85f}, 0.6f, 0.8f,
        {0.008f, 0.010f, 0.018f}, {0.13f, 0.075f, 0.050f},
        {0.03f, 0.035f, 0.05f}, {0.30f, 0.15f, 0.07f},
        0.62f, 2.5f, 0.8f, 1.7f, 0.012f,
        0.22f, 0.0f,
        {0.14f, 0.08f, 0.05f},
        1.5f,
    },
    // Sulon, Katarn's farm: a heavy grey overcast of dark cumulus, sun hidden.
    {
        "sulon_overcast", {"03sky09k", "03SKY01K", NULL},
        120.0f, 35.0f,
        {1.0f, 0.97f, 0.92f}, 0.8f, 0.0f,
        {0.30f, 0.33f, 0.38f}, {0.50f, 0.52f, 0.56f},
        {0.62f, 0.65f, 0.72f}, {0.18f, 0.19f, 0.22f},
        0.85f, 6.0f, 1.0f, 3.6f, 0.02f,
        0.0f, 0.0f,
        {0.34f, 0.35f, 0.39f},
        0.85f,
    },
    // Day with broken cumulus (the escape house, the cargo ship, the tower).
    {
        "day_broken", {"04sky01", "10sky2", "14sky", NULL},
        60.0f, 40.0f,
        {1.0f, 0.96f, 0.88f}, 1.5f, 0.6f,
        {0.16f, 0.30f, 0.60f}, {0.55f, 0.66f, 0.82f},
        {0.55f, 0.62f, 0.78f}, {0.24f, 0.25f, 0.29f},
        0.45f, 7.0f, 1.2f, 3.2f, 0.02f,
        0.0f, 0.0f,
        {0.50f, 0.55f, 0.62f},
        0.7f,
    },
    // Clear day, a few fair-weather clouds (Baron's Hed, the fuel station).
    {
        "day_clear", {"06sky01", "06sky02", "09skyB", NULL},
        150.0f, 55.0f,
        {1.0f, 0.97f, 0.90f}, 1.6f, 0.6f,
        {0.10f, 0.26f, 0.62f}, {0.50f, 0.64f, 0.85f},
        {0.55f, 0.63f, 0.80f}, {0.28f, 0.29f, 0.33f},
        0.30f, 7.0f, 1.3f, 2.8f, 0.02f,
        0.0f, 0.0f,
        {0.48f, 0.55f, 0.65f},
        0.7f,
    },
    // Sunset: towering pink and purple cumulus lit low from the side (the hangar's ceiling).
    {
        "sunset", {"12sky3", NULL},
        250.0f, 4.0f,
        {1.0f, 0.55f, 0.35f}, 1.7f, 1.2f,
        {0.16f, 0.09f, 0.24f}, {0.62f, 0.32f, 0.34f},
        {0.42f, 0.28f, 0.45f}, {0.22f, 0.11f, 0.15f},
        0.55f, 6.0f, 1.4f, 4.6f, 0.02f,
        0.05f, 0.0f,
        {0.30f, 0.15f, 0.20f},
        0.8f,
    },
};

static SithWorld* sithSky_pWorld = NULL;
static char sithSky_worldName[32];
static int sithSky_worldSurfaces = -1;
static const sithSkyPreset* sithSky_pActive = NULL;

static int sithSky_MatMatches(const char* pPath, const char* pName)
{
    if (!pPath || !pName) return 0;
    const char* base = pPath;
    for (const char* c = pPath; *c; c++)
        if (*c == '/' || *c == '\\') base = c + 1;
    size_t n = strlen(pName);
    for (size_t i = 0; i < n; i++)
        if (!base[i] || tolower((unsigned char)base[i]) != tolower((unsigned char)pName[i])) return 0;
    return base[n] == '.' || base[n] == 0;
}

static const sithSkyPreset* sithSky_FindForWorld(SithWorld* pWorld)
{
    if (!pWorld || !pWorld->surfaces) return NULL;
    int best = -1, bestCount = 0;
    int counts[sizeof(sithSky_aPresets) / sizeof(sithSky_aPresets[0])] = {0};
    for (int i = 0; i < pWorld->numSurfaces; i++)
    {
        SithSurface* s = &pWorld->surfaces[i];
        if (!(s->flags & (SITH_SURFACE_CEILING_SKY | SITH_SURFACE_HORIZON_SKY))) continue;
        rdMaterial* m = s->surfaceInfo.face.material;
        if (!m) continue;
        for (size_t p = 0; p < sizeof(sithSky_aPresets) / sizeof(sithSky_aPresets[0]); p++)
            for (int k = 0; k < 4 && sithSky_aPresets[p].aMats[k]; k++)
                if (sithSky_MatMatches(m->mat_fpath, sithSky_aPresets[p].aMats[k])) counts[p]++;
    }
    for (size_t p = 0; p < sizeof(sithSky_aPresets) / sizeof(sithSky_aPresets[0]); p++)
        if (counts[p] > bestCount) { bestCount = counts[p]; best = (int)p; }
    {
        static int dbg = -1;
        if (dbg < 0) { const char* e = getenv("OPENJKDF2_SKY_DEBUG"); dbg = (e && e[0] == '1'); }
        if (dbg)
            for (int i = 0; i < pWorld->numSurfaces; i++)
                if ((pWorld->surfaces[i].flags & (SITH_SURFACE_CEILING_SKY | SITH_SURFACE_HORIZON_SKY)) && pWorld->surfaces[i].surfaceInfo.face.material)
                { stdPlatform_Printf("sithSky: %s first sky material '%s'\n", pWorld->map_jkl_fname, pWorld->surfaces[i].surfaceInfo.face.material->mat_fpath); break; }
    }
    if (best >= 0)
        stdPlatform_Printf("sithSky: preset '%s' (%d sky surfaces)\n", sithSky_aPresets[best].pName, bestCount);
    return best >= 0 ? &sithSky_aPresets[best] : NULL;
}

const sithSkyPreset* sithSky_GetActive(void)
{
    if (!jkPlayer_bVolumetricSky || !jkPlayer_bPerPixelLighting) return NULL;
    // Key on the level name and surface count too: a new level can reuse the old one's memory.
    SithWorld* w = sithWorld_g_pCurrentWorld;
    if (w != sithSky_pWorld || (w && (w->numSurfaces != sithSky_worldSurfaces || strncmp(w->map_jkl_fname, sithSky_worldName, sizeof(sithSky_worldName)))))
    {
        sithSky_pWorld = w;
        sithSky_worldSurfaces = w ? w->numSurfaces : -1;
        if (w) strncpy(sithSky_worldName, w->map_jkl_fname, sizeof(sithSky_worldName));
        sithSky_pActive = sithSky_FindForWorld(w);
    }
    return sithSky_pActive;
}

void sithSky_GetSunDir(const sithSkyPreset* p, float* pOut3)
{
    float az = (jkPlayer_skySunAzimuth >= -360.0f) ? (float)jkPlayer_skySunAzimuth : p->sunAzimuth;
    float el = (jkPlayer_skySunElevation >= -90.0f) ? (float)jkPlayer_skySunElevation : p->sunElevation;
    float a = az * (float)M_PI / 180.0f, e = el * (float)M_PI / 180.0f;
    pOut3[0] = cosf(e) * cosf(a);
    pOut3[1] = cosf(e) * sinf(a);
    pOut3[2] = sinf(e);
}

float sithSky_GetCoverage(const sithSkyPreset* p)
{
    return (jkPlayer_skyCoverage >= 0.0f) ? (float)jkPlayer_skyCoverage : p->coverage;
}

static uint32_t sithSky_Hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static float sithSky_Hash01(uint32_t x) { return (sithSky_Hash(x) & 0xFFFFFF) / (float)0x1000000; }

float sithSky_GetFlash(const sithSkyPreset* p, double t, float* pDir3)
{
    pDir3[0] = 1.0f; pDir3[1] = 0.0f; pDir3[2] = 0.3f;
    if (!p || p->lightning <= 0.0f) return 0.0f;
    double window = 60.0 / p->lightning;
    // Check this window and the previous one (a flash near the end can spill over).
    float best = 0.0f;
    for (int w = 0; w < 2; w++)
    {
        int64_t k = (int64_t)floor(t / window) - w;
        uint32_t h = (uint32_t)(k * 2654435761u);
        if (sithSky_Hash01(h ^ 0x51ed27u) < 0.25f) continue; // some windows stay quiet
        double t0 = k * window + sithSky_Hash01(h) * window * 0.85;
        double dt = t - t0;
        if (dt < 0.0 || dt > 1.2) continue;
        // Main strike with a decaying tail, then one or two re-strikes (the flicker).
        float f = expf(-(float)dt * 7.0f);
        f += 0.7f * expf(-powf(((float)dt - 0.16f) * 22.0f, 2.0f));
        if (sithSky_Hash01(h ^ 0x9e37u) > 0.5f) f += 0.5f * expf(-powf(((float)dt - 0.38f) * 18.0f, 2.0f));
        if (f > best)
        {
            best = f;
            float az = sithSky_Hash01(h ^ 0x1234u) * 6.2831853f;
            float el = (6.0f + sithSky_Hash01(h ^ 0x4321u) * 30.0f) * 0.0174533f;
            pDir3[0] = cosf(el) * cosf(az);
            pDir3[1] = cosf(el) * sinf(az);
            pDir3[2] = sinf(el);
        }
    }
    return best > 1.0f ? 1.0f : best;
}

// ---- Tileable noise -------------------------------------------------------------------

static float sithSky_Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

static float sithSky_Grad(int ix, int iy, int iz, int period, float x, float y, float z)
{
    ix = ((ix % period) + period) % period;
    iy = ((iy % period) + period) % period;
    iz = ((iz % period) + period) % period;
    uint32_t h = sithSky_Hash((uint32_t)(ix * 73856093) ^ (uint32_t)(iy * 19349663) ^ (uint32_t)(iz * 83492791) ^ (uint32_t)period);
    switch (h % 12) {
        case 0: return x + y;  case 1: return -x + y; case 2: return x - y;  case 3: return -x - y;
        case 4: return x + z;  case 5: return -x + z; case 6: return x - z;  case 7: return -x - z;
        case 8: return y + z;  case 9: return -y + z; case 10: return y - z; default: return -y - z;
    }
}

// Perlin noise with the lattice wrapping every `period` cells; p in cells. Roughly -1..1.
static float sithSky_Perlin(float x, float y, float z, int period)
{
    int ix = (int)floorf(x), iy = (int)floorf(y), iz = (int)floorf(z);
    float fx = x - ix, fy = y - iy, fz = z - iz;
    float u = sithSky_Fade(fx), v = sithSky_Fade(fy), w = sithSky_Fade(fz);
    float n000 = sithSky_Grad(ix, iy, iz, period, fx, fy, fz);
    float n100 = sithSky_Grad(ix + 1, iy, iz, period, fx - 1, fy, fz);
    float n010 = sithSky_Grad(ix, iy + 1, iz, period, fx, fy - 1, fz);
    float n110 = sithSky_Grad(ix + 1, iy + 1, iz, period, fx - 1, fy - 1, fz);
    float n001 = sithSky_Grad(ix, iy, iz + 1, period, fx, fy, fz - 1);
    float n101 = sithSky_Grad(ix + 1, iy, iz + 1, period, fx - 1, fy, fz - 1);
    float n011 = sithSky_Grad(ix, iy + 1, iz + 1, period, fx, fy - 1, fz - 1);
    float n111 = sithSky_Grad(ix + 1, iy + 1, iz + 1, period, fx - 1, fy - 1, fz - 1);
    float x00 = n000 + u * (n100 - n000), x10 = n010 + u * (n110 - n010);
    float x01 = n001 + u * (n101 - n001), x11 = n011 + u * (n111 - n011);
    float y0 = x00 + v * (x10 - x00), y1 = x01 + v * (x11 - x01);
    return y0 + w * (y1 - y0);
}

// Worley (cellular) noise, wrapping every `cells` cells; p in 0..1 of the tile.
// Returns 1 - distance to the nearest feature point (bright blobs), 0..1.
static float sithSky_Worley(float x, float y, float z, int cells)
{
    float px = x * cells, py = y * cells, pz = z * cells;
    int cx = (int)floorf(px), cy = (int)floorf(py), cz = (int)floorf(pz);
    float best = 9.0f;
    for (int dz = -1; dz <= 1; dz++)
    for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++)
    {
        int nx = cx + dx, ny = cy + dy, nz = cz + dz;
        int wx = ((nx % cells) + cells) % cells, wy = ((ny % cells) + cells) % cells, wz = ((nz % cells) + cells) % cells;
        uint32_t h = sithSky_Hash((uint32_t)(wx * 1597334677u) ^ (uint32_t)(wy * 3812015801u) ^ (uint32_t)(wz * 2798796415u) ^ (uint32_t)cells);
        float fx = nx + sithSky_Hash01(h), fy = ny + sithSky_Hash01(h ^ 0xA5A5u), fz = nz + sithSky_Hash01(h ^ 0x5A5Au);
        float d = (fx - px) * (fx - px) + (fy - py) * (fy - py) + (fz - pz) * (fz - pz);
        if (d < best) best = d;
    }
    float d = sqrtf(best);
    return d > 1.0f ? 0.0f : 1.0f - d;
}

static float sithSky_WorleyFbm(float x, float y, float z, int cells)
{
    return sithSky_Worley(x, y, z, cells) * 0.625f + sithSky_Worley(x, y, z, cells * 2) * 0.25f
         + sithSky_Worley(x, y, z, cells * 4) * 0.125f;
}

static float sithSky_Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static float sithSky_Remap(float v, float lo, float hi, float nlo, float nhi)
{
    return nlo + (v - lo) / (hi - lo) * (nhi - nlo);
}

#define SITHSKY_SHAPE_SIZE 128
#define SITHSKY_DETAIL_SIZE 32
static uint8_t* sithSky_pShape = NULL;
static uint8_t* sithSky_pDetail = NULL;

const uint8_t* sithSky_GetShapeNoise(int* pSize)
{
    *pSize = SITHSKY_SHAPE_SIZE;
    if (sithSky_pShape) return sithSky_pShape;
    const int N = SITHSKY_SHAPE_SIZE;
    uint8_t* out = (uint8_t*)malloc((size_t)N * N * N * 4);
    if (!out) return NULL;
    uint32_t t0 = stdPlatform_GetTimeMsec();
    for (int z = 0; z < N; z++)
    for (int y = 0; y < N; y++)
    for (int x = 0; x < N; x++)
    {
        float fx = (x + 0.5f) / N, fy = (y + 0.5f) / N, fz = (z + 0.5f) / N;
        // Perlin fbm, 3 octaves, base period 4 cells across the tile.
        float pn = 0.0f, amp = 1.0f, tot = 0.0f;
        for (int o = 0; o < 3; o++)
        {
            int per = 4 << o;
            pn += sithSky_Perlin(fx * per, fy * per, fz * per, per) * amp;
            tot += amp;
            amp *= 0.5f;
        }
        pn = sithSky_Clamp01(pn / tot * 0.7f + 0.5f);
        float w1 = sithSky_WorleyFbm(fx, fy, fz, 4);
        float w2 = sithSky_WorleyFbm(fx, fy, fz, 8);
        float w3 = sithSky_WorleyFbm(fx, fy, fz, 16);
        // Perlin-Worley: billowy Perlin, carved into cells by Worley.
        float pw = sithSky_Clamp01(sithSky_Remap(pn, w1 - 1.0f, 1.0f, 0.0f, 1.0f));
        uint8_t* o = &out[((size_t)z * N * N + (size_t)y * N + x) * 4];
        o[0] = (uint8_t)(pw * 255.0f + 0.5f);
        o[1] = (uint8_t)(sithSky_Clamp01(w1) * 255.0f + 0.5f);
        o[2] = (uint8_t)(sithSky_Clamp01(w2) * 255.0f + 0.5f);
        o[3] = (uint8_t)(sithSky_Clamp01(w3) * 255.0f + 0.5f);
    }
    // Equalise each channel (histogram -> uniform), so coverage reads as a fraction of the
    // sky: coverage 0.4 leaves roughly 60 % of the noise volume clear.
    for (int c = 0; c < 4; c++)
    {
        uint32_t hist[256] = {0};
        size_t total = (size_t)N * N * N;
        for (size_t i = 0; i < total; i++) hist[out[i * 4 + c]]++;
        uint8_t map[256];
        uint64_t acc = 0;
        for (int v = 0; v < 256; v++) { acc += hist[v]; map[v] = (uint8_t)((acc * 255) / total); }
        for (size_t i = 0; i < total; i++) out[i * 4 + c] = map[out[i * 4 + c]];
    }
    stdPlatform_Printf("sithSky: built %d^3 cloud shape noise in %u ms\n", N, stdPlatform_GetTimeMsec() - t0);
    sithSky_pShape = out;
    return out;
}

const uint8_t* sithSky_GetDetailNoise(int* pSize)
{
    *pSize = SITHSKY_DETAIL_SIZE;
    if (sithSky_pDetail) return sithSky_pDetail;
    const int N = SITHSKY_DETAIL_SIZE;
    uint8_t* out = (uint8_t*)malloc((size_t)N * N * N * 4);
    if (!out) return NULL;
    for (int z = 0; z < N; z++)
    for (int y = 0; y < N; y++)
    for (int x = 0; x < N; x++)
    {
        float fx = (x + 0.5f) / N, fy = (y + 0.5f) / N, fz = (z + 0.5f) / N;
        uint8_t* o = &out[((size_t)z * N * N + (size_t)y * N + x) * 4];
        o[0] = (uint8_t)(sithSky_Worley(fx, fy, fz, 2) * 255.0f + 0.5f);
        o[1] = (uint8_t)(sithSky_Worley(fx, fy, fz, 4) * 255.0f + 0.5f);
        o[2] = (uint8_t)(sithSky_Worley(fx, fy, fz, 8) * 255.0f + 0.5f);
        o[3] = 255;
    }
    sithSky_pDetail = out;
    return out;
}
