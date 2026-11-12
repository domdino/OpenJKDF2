// Added (remaster): atmosphere, the baked light volume behind the volumetric fog. See sithAtmos.h.

#include "sithAtmos.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "jk.h"
#include "World/sithWorld.h"
#include "stdPlatform.h"
#include "General/stdJSON.h"

#define BRICK 8   // points are indexed in bricks of 8x8x8 grid cells

typedef struct sithAtmosPoint
{
    int16_t i[3];
    uint16_t c[3];   // half floats
} sithAtmosPoint;

typedef struct sithAtmosLevel
{
    float spacing;
    int num;
    sithAtmosPoint* aPoints;     // sorted by brick
    int numBricks;
    int64_t* aBrickKey;          // sorted
    int* aBrickStart;            // numBricks + 1
} sithAtmosLevel;

static SithWorld* sithAtmos_pWorld = NULL;
static int sithAtmos_worldSurfaces = -1;
static char sithAtmos_worldName[32];
static int sithAtmos_bLoaded = 0;
static sithAtmosLevel sithAtmos_aLevels[2];
static sithAtmosGrid sithAtmos_aGrids[2];

// Fog volumes from jkgm/atmos/<level>.json: center:3, size:3, rot_z:1, density:1, tint:3, soft:1, shape:1.
#define VOL_STRIDE 13
#define VOL_MAX 256
static float sithAtmos_aVolumes[VOL_MAX * VOL_STRIDE];
static int sithAtmos_numVolumes = 0;

// Emitters: center:3, kind:1, rate:1, life:1, size:1, grow:1, rise:1, spread:1, tint:3, opacity:1, drift:2.
#define EMIT_STRIDE 16
#define EMIT_MAX 64
static float sithAtmos_aEmitters[EMIT_MAX * EMIT_STRIDE];
static int sithAtmos_numEmitters = 0;

static float sithAtmos_Hash(float x)
{
    float s = sinf(x * 12.9898f + 78.233f) * 43758.5453f;
    return s - floorf(s);
}

int sithAtmos_GetParticles(float t, const rdVector3* pCam, sithAtmosParticle* pOut, int max)
{
    int n = 0;
    for (int e = 0; e < sithAtmos_numEmitters && n < max; e++) {
        const float* E = &sithAtmos_aEmitters[e * EMIT_STRIDE];
        float dx = E[0] - pCam->x, dy = E[1] - pCam->y, dz = E[2] - pCam->z;
        if (dx * dx + dy * dy + dz * dz > 60.0f * 60.0f) continue;
        float rate = fmaxf(E[4], 0.1f), life = fmaxf(E[5], 0.2f);
        int count = (int)(rate * life);
        if (count < 1) count = 1;
        if (count > 24) count = 24;
        for (int i = 0; i < count && n < max; i++) {
            float cyc = t / life + (float)i / (float)count + (float)e * 0.137f;
            float phase = cyc - floorf(cyc);
            float seed = sithAtmos_Hash(floorf(cyc) * 7.31f + (float)i * 1.73f + (float)e * 11.1f);
            float s2 = sithAtmos_Hash(seed * 91.7f), s3 = sithAtmos_Hash(seed * 47.3f);
            float age = phase * life;
            float spread = E[9] * (0.3f + 0.7f * phase);
            sithAtmosParticle* p = &pOut[n++];
            p->pos[0] = E[0] + E[14] * age + (s2 - 0.5f) * 2.0f * spread;
            p->pos[1] = E[1] + E[15] * age + (s3 - 0.5f) * 2.0f * spread;
            p->pos[2] = E[2] + E[8] * age;
            p->radius = fmaxf(E[6], 0.005f) * (1.0f + (E[7] - 1.0f) * phase);
            p->age = phase;
            p->seed = seed;
            p->kind = E[3];
            p->tint[0] = E[10]; p->tint[1] = E[11]; p->tint[2] = E[12];
            p->opacity = E[13];
        }
    }
    return n;
}

static void sithAtmos_LoadEmitters(const char* path)
{
    for (int i = 0; i < EMIT_MAX; i++) {
        // Defaults: steam, 8 a second, 2.5 s, 0.04 units growing x3, rising 0.12 units/s.
        static const float def[EMIT_STRIDE] = {0, 0, 0, 0, 8, 2.5f, 0.04f, 3, 0.12f, 0.02f, 1, 1, 1, 0.6f, 0, 0};
        memcpy(&sithAtmos_aEmitters[i * EMIT_STRIDE], def, sizeof(def));
    }
    sithAtmos_numEmitters = stdJSON_ReadObjectArray(path, "emitters",
        "center:3,kind:1,rate:1,life:1,size:1,grow:1,rise:1,spread:1,tint:3,opacity:1,drift:2",
        sithAtmos_aEmitters, EMIT_MAX);
    if (sithAtmos_numEmitters)
        stdPlatform_Printf("atmos: %d smoke/steam emitters from %s\n", sithAtmos_numEmitters, path);
}

static void sithAtmos_LoadVolumes(const char* name)
{
    char path[128];
    snprintf(path, sizeof(path), "jkgm/atmos/%s.json", name);
    for (int i = 0; i < VOL_MAX; i++) {
        float* v = &sithAtmos_aVolumes[i * VOL_STRIDE];
        static const float def[VOL_STRIDE] = {0, 0, 0, 1, 1, 1, 0, 0.5f, 1, 1, 1, 0.3f, 0};
        memcpy(v, def, sizeof(def));
    }
    sithAtmos_numVolumes = stdJSON_ReadObjectArray(path, "volumes", "center:3,size:3,rot_z:1,density:1,tint:3,soft:1,shape:1",
                                                   sithAtmos_aVolumes, VOL_MAX);
    if (sithAtmos_numVolumes)
        stdPlatform_Printf("atmos: %d fog volumes from %s\n", sithAtmos_numVolumes, path);
    sithAtmos_LoadEmitters(path);
}

int sithAtmos_GetVolumes(const rdVector3* pCamPos, float* pCenter, float* pSize, float* pParams, float* pTint, int max)
{
    // Nearest first (by distance to the volume's edge), so the shader gets the ones that matter.
    int idx[VOL_MAX];
    float dist[VOL_MAX];
    int n = 0;
    for (int i = 0; i < sithAtmos_numVolumes; i++) {
        const float* v = &sithAtmos_aVolumes[i * VOL_STRIDE];
        float dx = v[0] - pCamPos->x, dy = v[1] - pCamPos->y, dz = v[2] - pCamPos->z;
        float r = 0.5f * sqrtf(v[3] * v[3] + v[4] * v[4] + v[5] * v[5]);
        float d = sqrtf(dx * dx + dy * dy + dz * dz) - r;
        if (d > 150.0f) continue;
        int j = n++;
        while (j > 0 && dist[j - 1] > d) { idx[j] = idx[j - 1]; dist[j] = dist[j - 1]; j--; }
        idx[j] = i;
        dist[j] = d;
    }
    if (n > max) n = max;
    for (int k = 0; k < n; k++) {
        const float* v = &sithAtmos_aVolumes[idx[k] * VOL_STRIDE];
        float a = v[6] * 3.14159265f / 180.0f;
        pCenter[k * 4 + 0] = v[0]; pCenter[k * 4 + 1] = v[1]; pCenter[k * 4 + 2] = v[2]; pCenter[k * 4 + 3] = v[12];
        pSize[k * 4 + 0] = fmaxf(v[3], 0.01f) * 0.5f; pSize[k * 4 + 1] = fmaxf(v[4], 0.01f) * 0.5f;
        pSize[k * 4 + 2] = fmaxf(v[5], 0.01f) * 0.5f; pSize[k * 4 + 3] = fminf(fmaxf(v[11], 0.01f), 1.0f);
        pParams[k * 4 + 0] = cosf(a); pParams[k * 4 + 1] = sinf(a); pParams[k * 4 + 2] = v[7]; pParams[k * 4 + 3] = 0.0f;
        pTint[k * 4 + 0] = v[8]; pTint[k * 4 + 1] = v[9]; pTint[k * 4 + 2] = v[10]; pTint[k * 4 + 3] = 0.0f;
    }
    return n;
}

static float sithAtmos_Half(uint16_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v;
    if (e == 0) v = ldexpf((float)m, -24);
    else if (e == 31) v = 65504.0f;
    else v = ldexpf((float)(m | 0x400), (int)e - 25);
    return s ? -v : v;
}

static int64_t sithAtmos_Key(int bx, int by, int bz)
{
    return ((int64_t)(bx + 4096) << 26) | ((int64_t)(by + 4096) << 13) | (int64_t)(bz + 4096);
}

static int sithAtmos_FloorDiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }

static int64_t sithAtmos_PointKey(const sithAtmosPoint* p)
{
    return sithAtmos_Key(sithAtmos_FloorDiv(p->i[0], BRICK), sithAtmos_FloorDiv(p->i[1], BRICK), sithAtmos_FloorDiv(p->i[2], BRICK));
}

static int sithAtmos_CmpPoint(const void* a, const void* b)
{
    int64_t ka = sithAtmos_PointKey((const sithAtmosPoint*)a), kb = sithAtmos_PointKey((const sithAtmosPoint*)b);
    return (ka > kb) - (ka < kb);
}

static void sithAtmos_FreeLevel(sithAtmosLevel* L)
{
    free(L->aPoints);
    free(L->aBrickKey);
    free(L->aBrickStart);
    memset(L, 0, sizeof(*L));
}

static void sithAtmos_Free(void)
{
    for (int k = 0; k < 2; k++) {
        sithAtmos_FreeLevel(&sithAtmos_aLevels[k]);
        sithAtmos_aGrids[k].origin[0] = 0x7fffffff;   // force a refill
    }
    sithAtmos_bLoaded = 0;
}

static int sithAtmos_ReadLevel(FILE* f, sithAtmosLevel* L)
{
    float sp;
    uint32_t n;
    if (fread(&sp, 4, 1, f) != 1 || fread(&n, 4, 1, f) != 1 || n > 20000000 || !(sp > 0.0f)) return 0;
    L->spacing = sp;
    L->num = (int)n;
    L->aPoints = (sithAtmosPoint*)malloc(sizeof(sithAtmosPoint) * (n ? n : 1));
    if (!L->aPoints || fread(L->aPoints, sizeof(sithAtmosPoint), n, f) != n) return 0;
    qsort(L->aPoints, n, sizeof(sithAtmosPoint), sithAtmos_CmpPoint);
    L->aBrickKey = (int64_t*)malloc(sizeof(int64_t) * (n ? n : 1));
    L->aBrickStart = (int*)malloc(sizeof(int) * (n + 1));
    if (!L->aBrickKey || !L->aBrickStart) return 0;
    int nb = 0;
    for (uint32_t i = 0; i < n; i++) {
        int64_t k = sithAtmos_PointKey(&L->aPoints[i]);
        if (!nb || L->aBrickKey[nb - 1] != k) {
            L->aBrickKey[nb] = k;
            L->aBrickStart[nb] = (int)i;
            nb++;
        }
    }
    L->aBrickStart[nb] = (int)n;
    L->numBricks = nb;
    return 1;
}

static void sithAtmos_Load(SithWorld* w)
{
    char name[64], path[128];
    int i;
    for (i = 0; i < 63 && w->map_jkl_fname[i] && w->map_jkl_fname[i] != '.'; i++)
        name[i] = (char)tolower((unsigned char)w->map_jkl_fname[i]);
    name[i] = 0;
    snprintf(path, sizeof(path), "jkgm/lightmaps/%s.jklv", name);
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char magic[4];
    uint32_t hdr[2];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "JKLV", 4) || fread(hdr, 4, 2, f) != 2 || hdr[0] != 1) {
        stdPlatform_Printf("atmos: %s is not a version 1 light volume, ignored\n", path);
        fclose(f);
        return;
    }
    if ((int)hdr[1] != w->numVertices) {
        stdPlatform_Printf("atmos: %s was baked for a different version of the level, ignored\n", path);
        fclose(f);
        return;
    }
    int ok = sithAtmos_ReadLevel(f, &sithAtmos_aLevels[0]) && sithAtmos_ReadLevel(f, &sithAtmos_aLevels[1]);
    fclose(f);
    if (!ok) {
        stdPlatform_Printf("atmos: %s is truncated, ignored\n", path);
        sithAtmos_Free();
        return;
    }
    sithAtmos_bLoaded = 1;
    stdPlatform_Printf("atmos: loaded %s (%d fine points at %.2f, %d coarse at %.2f)\n", path,
                       sithAtmos_aLevels[0].num, sithAtmos_aLevels[0].spacing, sithAtmos_aLevels[1].num, sithAtmos_aLevels[1].spacing);
}

// Copy the points around the camera into a dense grid. Refills only when the camera has moved
// a quarter of the grid from its centre.
static void sithAtmos_Fill(const sithAtmosLevel* L, sithAtmosGrid* G, int dxy, int dz, const rdVector3* pCam)
{
    if (!G->data) {
        G->data = (float*)calloc((size_t)dxy * dxy * dz * 4, sizeof(float));
        if (!G->data) return;
        G->dims[0] = G->dims[1] = dxy;
        G->dims[2] = dz;
        G->origin[0] = 0x7fffffff;
    }
    G->spacing = L->spacing;
    int c[3] = {(int)floorf(pCam->x / L->spacing), (int)floorf(pCam->y / L->spacing), (int)floorf(pCam->z / L->spacing)};
    if (G->origin[0] != 0x7fffffff
        && abs(c[0] - (G->origin[0] + dxy / 2)) < dxy / 4 && abs(c[1] - (G->origin[1] + dxy / 2)) < dxy / 4
        && abs(c[2] - (G->origin[2] + dz / 2)) < dz / 4)
        return;
    G->origin[0] = c[0] - dxy / 2;
    G->origin[1] = c[1] - dxy / 2;
    G->origin[2] = c[2] - dz / 2;
    memset(G->data, 0, sizeof(float) * 4 * (size_t)dxy * dxy * dz);
    int b0[3], b1[3];
    for (int k = 0; k < 3; k++) {
        b0[k] = sithAtmos_FloorDiv(G->origin[k], BRICK);
        b1[k] = sithAtmos_FloorDiv(G->origin[k] + G->dims[k] - 1, BRICK);
    }
    for (int bx = b0[0]; bx <= b1[0]; bx++)
    for (int by = b0[1]; by <= b1[1]; by++)
    for (int bz = b0[2]; bz <= b1[2]; bz++)
    {
        int64_t key = sithAtmos_Key(bx, by, bz);
        int lo = 0, hi = L->numBricks - 1, found = -1;
        while (lo <= hi) {
            int mid = (lo + hi) >> 1;
            if (L->aBrickKey[mid] < key) lo = mid + 1;
            else if (L->aBrickKey[mid] > key) hi = mid - 1;
            else { found = mid; break; }
        }
        if (found < 0) continue;
        for (int i = L->aBrickStart[found]; i < L->aBrickStart[found + 1]; i++) {
            const sithAtmosPoint* p = &L->aPoints[i];
            int x = p->i[0] - G->origin[0], y = p->i[1] - G->origin[1], z = p->i[2] - G->origin[2];
            if (x < 0 || y < 0 || z < 0 || x >= G->dims[0] || y >= G->dims[1] || z >= G->dims[2]) continue;
            float* d = &G->data[(((size_t)z * G->dims[1] + y) * G->dims[0] + x) * 4];
            d[0] = sithAtmos_Half(p->c[0]);
            d[1] = sithAtmos_Half(p->c[1]);
            d[2] = sithAtmos_Half(p->c[2]);
            d[3] = 1.0f;
        }
    }
    G->dirty = 1;
}

// Height fog per level (ground = the level's low floors, from tools: area-weighted floor heights).
// Outdoor levels only: it sits in pits, canyons and city depths and hides the far terrain.
typedef struct sithAtmosHeightFog { const char* level; float density, groundZ, falloff; } sithAtmosHeightFog;
static const sithAtmosHeightFog sithAtmos_aHeightFog[] = {
    {"01narshadda",   0.25f, -11.2f, 3.0f},   // the city's depths
    {"02narshadda",   0.25f,  -9.7f, 3.0f},
    {"04escapehouse", 0.20f, -15.4f, 2.5f},   // the ravine below the house
    {"06abarons",     0.20f,  -0.7f, 0.8f},   // mist on the water
    {"06bbarons",     0.20f,   2.1f, 2.0f},
    {"09fuelstation", 0.30f, -20.8f, 4.0f},   // the canyon floor
    {"10cargo",       0.10f,  -4.7f, 1.5f},
    {"14tower",       0.30f,   0.2f, 2.0f},   // sea mist round the rock towers
    {"15maw",         0.35f, -30.6f, 6.0f},   // the pit
    {"18ascend",      0.25f,  -8.8f, 4.0f},
};

void sithAtmos_GetHeightFog(float* pDensity, float* pGroundZ, float* pFalloff)
{
    *pDensity = 0.0f; *pGroundZ = 0.0f; *pFalloff = 1.0f;
    for (size_t i = 0; i < sizeof(sithAtmos_aHeightFog) / sizeof(sithAtmos_aHeightFog[0]); i++)
    {
        size_t n = strlen(sithAtmos_aHeightFog[i].level);
        if (!strncasecmp(sithAtmos_worldName, sithAtmos_aHeightFog[i].level, n))
        {
            *pDensity = sithAtmos_aHeightFog[i].density;
            *pGroundZ = sithAtmos_aHeightFog[i].groundZ;
            *pFalloff = sithAtmos_aHeightFog[i].falloff;
            return;
        }
    }
}

int sithAtmos_Update(const rdVector3* pCamPos, sithAtmosGrid** ppFine, sithAtmosGrid** ppCoarse)
{
    SithWorld* w = sithWorld_g_pCurrentWorld;
    if (!w || !pCamPos) return 0;
    if (w != sithAtmos_pWorld || w->numSurfaces != sithAtmos_worldSurfaces
        || strncmp(w->map_jkl_fname, sithAtmos_worldName, sizeof(sithAtmos_worldName)))
    {
        sithAtmos_pWorld = w;
        sithAtmos_worldSurfaces = w->numSurfaces;
        strncpy(sithAtmos_worldName, w->map_jkl_fname, sizeof(sithAtmos_worldName));
        sithAtmos_Free();
        sithAtmos_Load(w);
        char name[64];
        int i;
        for (i = 0; i < 63 && w->map_jkl_fname[i] && w->map_jkl_fname[i] != '.'; i++)
            name[i] = (char)tolower((unsigned char)w->map_jkl_fname[i]);
        name[i] = 0;
        sithAtmos_LoadVolumes(name);
    }
    if (!sithAtmos_bLoaded) return 0;
    sithAtmos_Fill(&sithAtmos_aLevels[0], &sithAtmos_aGrids[0], SITHATMOS_FINE_XY, SITHATMOS_FINE_Z, pCamPos);
    sithAtmos_Fill(&sithAtmos_aLevels[1], &sithAtmos_aGrids[1], SITHATMOS_COARSE_XY, SITHATMOS_COARSE_Z, pCamPos);
    *ppFine = &sithAtmos_aGrids[0];
    *ppCoarse = &sithAtmos_aGrids[1];
    return 1;
}
