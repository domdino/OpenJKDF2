// Added (remaster): baked light probes for models. See sithLightProbes.h.

#include "sithLightProbes.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jk.h"
#include "World/sithWorld.h"
#include "World/jkPlayer.h"
#include "Engine/sithCamera.h"
#include "Primitives/rdVector.h"
#include "stdPlatform.h"

typedef struct sithLightProbe
{
    float pos[3];
    int32_t sector;
    int32_t layer;       // 0 = just above the floor, 1 = chest height
    float cube[6][3];
} sithLightProbe;

SithSector* sithLightProbes_pThingSector = NULL;

static SithWorld* sithLightProbes_pWorld = NULL;
static int sithLightProbes_worldSurfaces = -1;
static char sithLightProbes_worldName[32];
static sithLightProbe* sithLightProbes_aProbes = NULL;   // sorted by sector
static int sithLightProbes_num = 0;
static int* sithLightProbes_aSectorStart = NULL;         // numSectors + 1 offsets

// Last lookup (a model is several meshes drawn one after another at the same spot).
static rdVector3 sithLightProbes_lastPos;
static SithSector* sithLightProbes_lastSector = NULL;
static int sithLightProbes_lastOk = 0;
static sithLightProbeSample sithLightProbes_last;
static unsigned sithLightProbes_lastGen = 0, sithLightProbes_gen = 1;

static int sithLightProbes_CmpSector(const void* a, const void* b)
{
    return ((const sithLightProbe*)a)->sector - ((const sithLightProbe*)b)->sector;
}

static void sithLightProbes_Free(void)
{
    free(sithLightProbes_aProbes);
    free(sithLightProbes_aSectorStart);
    sithLightProbes_aProbes = NULL;
    sithLightProbes_aSectorStart = NULL;
    sithLightProbes_num = 0;
    sithLightProbes_gen++;
}

static void sithLightProbes_Load(SithWorld* w)
{
    char name[64], path[128];
    int i;
    for (i = 0; i < 63 && w->map_jkl_fname[i] && w->map_jkl_fname[i] != '.'; i++)
        name[i] = (char)tolower((unsigned char)w->map_jkl_fname[i]);
    name[i] = 0;
    snprintf(path, sizeof(path), "jkgm/lightmaps/%s.jklp", name);
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char magic[4];
    uint32_t hdr[3];
    float spacing;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "JKLP", 4) || fread(hdr, 4, 3, f) != 3
        || fread(&spacing, 4, 1, f) != 1 || hdr[0] != 2) {
        stdPlatform_Printf("lightprobes: %s is not a version 2 probe file, ignored\n", path);
        fclose(f);
        return;
    }
    if ((int)hdr[1] != w->numVertices || hdr[2] == 0 || hdr[2] > 1000000) {
        stdPlatform_Printf("lightprobes: %s was baked for a different version of the level, ignored\n", path);
        fclose(f);
        return;
    }
    int n = (int)hdr[2];
    sithLightProbe* p = (sithLightProbe*)malloc(sizeof(sithLightProbe) * n);
    int ok = p && fread(p, sizeof(sithLightProbe), n, f) == (size_t)n;
    fclose(f);
    if (!ok) {
        stdPlatform_Printf("lightprobes: %s is truncated, ignored\n", path);
        free(p);
        return;
    }
    // Index by sector (probes with no valid sector are dropped).
    int kept = 0;
    for (i = 0; i < n; i++)
        if (p[i].sector >= 0 && p[i].sector < w->numSectors && (p[i].layer == 0 || p[i].layer == 1))
        {
            // Probes right above a floor strip see thousands of units of light; a model there
            // can't show more than full brightness anyway.
            for (int f = 0; f < 6; f++)
                for (int c = 0; c < 3; c++)
                    if (p[i].cube[f][c] > 4.0f) p[i].cube[f][c] = 4.0f;
            p[kept++] = p[i];
        }
    qsort(p, kept, sizeof(sithLightProbe), sithLightProbes_CmpSector);
    int* start = (int*)calloc(w->numSectors + 1, sizeof(int));
    if (!start) { free(p); return; }
    int k = 0;
    for (int s = 0; s <= w->numSectors; s++) {
        while (k < kept && p[k].sector < s) k++;
        start[s] = k;
    }
    sithLightProbes_aProbes = p;
    sithLightProbes_aSectorStart = start;
    sithLightProbes_num = kept;
    sithLightProbes_gen++;
    stdPlatform_Printf("lightprobes: loaded %s (%d probes)\n", path, kept);
}

static int sithLightProbes_Ready(void)
{
    SithWorld* w = sithWorld_g_pCurrentWorld;
    if (!w) return 0;
    if (w != sithLightProbes_pWorld || w->numSurfaces != sithLightProbes_worldSurfaces
        || strncmp(w->map_jkl_fname, sithLightProbes_worldName, sizeof(sithLightProbes_worldName)))
    {
        sithLightProbes_pWorld = w;
        sithLightProbes_worldSurfaces = w->numSurfaces;
        strncpy(sithLightProbes_worldName, w->map_jkl_fname, sizeof(sithLightProbes_worldName));
        sithLightProbes_Free();
        sithLightProbes_Load(w);
    }
    return sithLightProbes_num > 0;
}

#define PROBE_K 6

typedef struct sithLightProbeNear
{
    const sithLightProbe* p[PROBE_K];
    float d2[PROBE_K];
    int n;
} sithLightProbeNear;

// Keep the PROBE_K nearest probes of each layer of one sector, sorted by distance.
static void sithLightProbes_Scan(int sIdx, const rdVector3* pPos, sithLightProbeNear* nearL)
{
    for (int i = sithLightProbes_aSectorStart[sIdx]; i < sithLightProbes_aSectorStart[sIdx + 1]; i++)
    {
        const sithLightProbe* p = &sithLightProbes_aProbes[i];
        sithLightProbeNear* L = &nearL[p->layer];
        float dx = p->pos[0] - pPos->x, dy = p->pos[1] - pPos->y, dz = (p->pos[2] - pPos->z) * 1.5f;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (L->n == PROBE_K && d2 >= L->d2[PROBE_K - 1]) continue;
        int j = (L->n < PROBE_K) ? L->n++ : PROBE_K - 1;
        while (j > 0 && L->d2[j - 1] > d2) { L->p[j] = L->p[j - 1]; L->d2[j] = L->d2[j - 1]; j--; }
        L->p[j] = p;
        L->d2[j] = d2;
    }
}

// Inverse square distance blend (probes 1.5 m apart, so a soft blend between them).
static void sithLightProbes_Blend(const sithLightProbeNear* L, float aCube[6][3], float* pZ)
{
    float wsum = 0.0f, z = 0.0f;
    memset(aCube, 0, sizeof(float) * 18);
    for (int i = 0; i < L->n; i++)
    {
        float wt = 1.0f / (L->d2[i] + 0.004f);
        wsum += wt;
        z += L->p[i]->pos[2] * wt;
        for (int f = 0; f < 6; f++)
            for (int c = 0; c < 3; c++)
                aCube[f][c] += L->p[i]->cube[f][c] * wt;
    }
    for (int f = 0; f < 6; f++)
        for (int c = 0; c < 3; c++)
            aCube[f][c] /= wsum;
    *pZ = z / wsum;
}

int sithLightProbes_Sample(const rdVector3* pPos, SithSector* pSector, sithLightProbeSample* pOut)
{
    if (!jkPlayer_bLightProbes || !pPos || !sithLightProbes_Ready()) return 0;
    SithWorld* w = sithWorld_g_pCurrentWorld;
    if (!pSector && sithCamera_g_pCurCamera) pSector = sithCamera_g_pCurCamera->sector;
    if (!pSector) return 0;
    if (sithLightProbes_lastGen == sithLightProbes_gen && pSector == sithLightProbes_lastSector
        && rdVector_DistSquared3(pPos, &sithLightProbes_lastPos) < 1e-8f)
    {
        if (sithLightProbes_lastOk) *pOut = sithLightProbes_last;
        return sithLightProbes_lastOk;
    }
    int sIdx = (int)(pSector - w->aSectors);
    if (sIdx < 0 || sIdx >= w->numSectors) return 0;
    sithLightProbeNear nearL[2];
    nearL[0].n = nearL[1].n = 0;
    // The model's sector and the sectors it opens into, so the light doesn't jump at a doorway.
    sithLightProbes_Scan(sIdx, pPos, nearL);
    for (uint32_t s = 0; s < pSector->numSurfaces; s++)
    {
        SithSurfaceAdjoin* a = pSector->surfaces[s].pAdjoin;
        if (!a || !a->sector) continue;
        int aIdx = (int)(a->sector - w->aSectors);
        if (aIdx >= 0 && aIdx < w->numSectors && aIdx != sIdx)
            sithLightProbes_Scan(aIdx, pPos, nearL);
    }
    int ok = nearL[0].n || nearL[1].n;
    if (ok)
    {
        const sithLightProbeNear* lo = nearL[0].n ? &nearL[0] : &nearL[1];
        const sithLightProbeNear* hi = nearL[1].n ? &nearL[1] : &nearL[0];
        sithLightProbes_Blend(lo, pOut->low, &pOut->zLow);
        sithLightProbes_Blend(hi, pOut->high, &pOut->zHigh);
        if (pOut->zHigh < pOut->zLow + 0.01f) pOut->zHigh = pOut->zLow + 0.01f;
    }
    sithLightProbes_lastGen = sithLightProbes_gen;
    sithLightProbes_lastSector = pSector;
    sithLightProbes_lastPos = *pPos;
    sithLightProbes_lastOk = ok;
    if (ok) sithLightProbes_last = *pOut;
    return ok;
}

void sithLightProbes_Eval(const float aCube[6][3], const rdVector3* n, float out[3])
{
    float xx = n->x * n->x, yy = n->y * n->y, zz = n->z * n->z;
    const float* fx = aCube[n->x >= 0 ? 0 : 1];
    const float* fy = aCube[n->y >= 0 ? 2 : 3];
    const float* fz = aCube[n->z >= 0 ? 4 : 5];
    for (int c = 0; c < 3; c++)
        out[c] = xx * fx[c] + yy * fy[c] + zz * fz[c];
}
