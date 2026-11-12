// Added (remaster): glow lights. See sithGlowLights.h.
//
// At level load (lazily, on the first gather for a new world) every level surface is
// checked for glowing texels: palette colours that stay bright at the darkest level of
// the world's light table. That's how JK marks "glows in the dark" pixels. A surface with
// enough of them gets one light at its centre, coloured by those texels and sized by its
// area. Each frame the nearest ones in visible sectors are handed to the shader.

#include "sithGlowLights.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "jk.h"
#include "World/sithWorld.h"
#include "Engine/rdCamera.h"
#include "Primitives/rdMatrix.h"
#include "Primitives/rdVector.h"
#include "stdPlatform.h"
#include "SDL2_helper.h"

typedef struct sithGlowLight
{
    rdVector3 pos;
    float color[3];
    float intensity;
    float radius;
    int sectorIdx;
} sithGlowLight;

typedef struct sithGlowMatInfo
{
    rdMaterial* pMat;
    float glowFrac;      // fraction of texels that glow
    float glowColor[3];  // average colour of glowing texels, 0..1
    float allColor[3];   // average colour of all texels, 0..1
    float satColor[3];   // average colour of clearly coloured texels (0 if none), 0..1
} sithGlowMatInfo;

static SithWorld* sithGlowLights_pWorld = NULL;
static int sithGlowLights_numLights = 0;
static sithGlowLight* sithGlowLights_aLights = NULL;
static int* sithGlowLights_aSectorStart = NULL;   // numSectors + 1 offsets into aLights
static uint8_t sithGlowLights_aIsGlowIndex[256];

#define GLOW_MAT_CACHE (1024)
static sithGlowMatInfo sithGlowLights_aMatCache[GLOW_MAT_CACHE];
static int sithGlowLights_numMatCache = 0;

static float sithGlowLights_Luma(const rdColor24* c)
{
    return (0.2126f * c->r + 0.7152f * c->g + 0.0722f * c->b) / 255.0f;
}

// Work out which palette entries glow, from the world's first colormap. Which end of the
// 64-level light table is "dark" is decided from the data: the dark end is where fewer
// colours survive.
static void sithGlowLights_BuildGlowIndices(SithWorld* pWorld)
{
    memset(sithGlowLights_aIsGlowIndex, 0, sizeof(sithGlowLights_aIsGlowIndex));
    if (!pWorld->colormaps || !pWorld->colormaps[0].lightlevel) return;
    rdColormap* pCmp = &pWorld->colormaps[0];

    int count[2] = {0, 0};
    const int levels[2] = {0, 63};
    for (int l = 0; l < 2; l++) {
        for (int i = 1; i < 256; i++) {
            float base = sithGlowLights_Luma(&pCmp->colors[i]);
            float lit = sithGlowLights_Luma(&pCmp->colors[pCmp->lightlevel[levels[l] * 256 + i]]);
            if (base > 0.15f && lit >= 0.6f * base) count[l]++;
        }
    }
    int darkLevel = (count[0] <= count[1]) ? levels[0] : levels[1];

    int numGlow = 0;
    for (int i = 1; i < 256; i++) {
        float base = sithGlowLights_Luma(&pCmp->colors[i]);
        float lit = sithGlowLights_Luma(&pCmp->colors[pCmp->lightlevel[darkLevel * 256 + i]]);
        if (base > 0.15f && lit >= 0.6f * base) {
            sithGlowLights_aIsGlowIndex[i] = 1;
            numGlow++;
        }
    }
    stdPlatform_Printf("sithGlowLights: %d glowing palette colours (dark light level %d)\n", numGlow, darkLevel);
}

static sithGlowMatInfo* sithGlowLights_MatInfo(SithWorld* pWorld, rdMaterial* pMat)
{
    for (int i = 0; i < sithGlowLights_numMatCache; i++)
        if (sithGlowLights_aMatCache[i].pMat == pMat) return &sithGlowLights_aMatCache[i];
    if (sithGlowLights_numMatCache >= GLOW_MAT_CACHE) return NULL;

    sithGlowMatInfo* pInfo = &sithGlowLights_aMatCache[sithGlowLights_numMatCache++];
    memset(pInfo, 0, sizeof(*pInfo));
    pInfo->pMat = pMat;

    if (!pMat->textures || !pMat->num_textures || !pWorld->colormaps) return pInfo;
    tVBuffer* pBuf = pMat->textures[0].texture_struct[0];
    if (!pBuf || pBuf->format.format.is16bit || !pBuf->sdlSurface || !pBuf->sdlSurface->pixels) return pInfo;

    const uint8_t* pPix = (const uint8_t*)pBuf->sdlSurface->pixels;
    int w = pBuf->format.width, h = pBuf->format.height;
    int pitch = pBuf->sdlSurface->pitch ? pBuf->sdlSurface->pitch : w;
    if (w <= 0 || h <= 0) return pInfo;

    rdColormap* pCmp = &pWorld->colormaps[0];
    double gsum[3] = {0, 0, 0}, asum[3] = {0, 0, 0}, ssum[3] = {0, 0, 0};
    int nGlow = 0, nAll = 0, nSat = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t idx = pPix[y * pitch + x];
            const rdColor24* c = &pCmp->colors[idx];
            asum[0] += c->r; asum[1] += c->g; asum[2] += c->b; nAll++;
            int mx = c->r > c->g ? c->r : c->g; if (c->b > mx) mx = c->b;
            int mn = c->r < c->g ? c->r : c->g; if (c->b < mn) mn = c->b;
            if (mx > 40 && (mx - mn) > 0.3f * mx) { // saturated, not near-black
                ssum[0] += c->r; ssum[1] += c->g; ssum[2] += c->b; nSat++;
            }
            if (sithGlowLights_aIsGlowIndex[idx]) {
                gsum[0] += c->r; gsum[1] += c->g; gsum[2] += c->b; nGlow++;
            }
        }
    }
    pInfo->glowFrac = nAll ? (float)nGlow / (float)nAll : 0.0f;
    for (int k = 0; k < 3; k++) {
        pInfo->glowColor[k] = nGlow ? (float)(gsum[k] / nGlow / 255.0) : 0.0f;
        pInfo->allColor[k] = nAll ? (float)(asum[k] / nAll / 255.0) : 0.0f;
        pInfo->satColor[k] = nSat ? (float)(ssum[k] / nSat / 255.0) : 0.0f;
    }
    return pInfo;
}

static void sithGlowLights_Normalise(float* c)
{
    float m = c[0] > c[1] ? c[0] : c[1];
    if (c[2] > m) m = c[2];
    if (m > 0.001f) { c[0] /= m; c[1] /= m; c[2] /= m; }
    else { c[0] = c[1] = c[2] = 1.0f; }
}

static void sithGlowLights_Build(SithWorld* pWorld)
{
    free(sithGlowLights_aLights);
    free(sithGlowLights_aSectorStart);
    sithGlowLights_aLights = NULL;
    sithGlowLights_aSectorStart = NULL;
    sithGlowLights_numLights = 0;
    sithGlowLights_numMatCache = 0;
    sithGlowLights_pWorld = pWorld;
    if (!pWorld || !pWorld->surfaces || !pWorld->aSectors || pWorld->numSectors <= 0) return;

    sithGlowLights_BuildGlowIndices(pWorld);

    sithGlowLight* aTmp = (sithGlowLight*)malloc(sizeof(sithGlowLight) * (pWorld->numSurfaces + 1));
    if (!aTmp) return;
    int n = 0;

    for (int s = 0; s < pWorld->numSurfaces; s++)
    {
        SithSurface* pSurf = &pWorld->surfaces[s];
        rdFace* pFace = &pSurf->surfaceInfo.face;
        if (!pFace->geometryMode || !pFace->material || pFace->numVertices < 3 || !pSurf->pSector) continue;
        if (pSurf->flags & (SITH_SURFACE_CEILING_SKY | SITH_SURFACE_HORIZON_SKY)) continue;

        sithGlowMatInfo* pInfo = sithGlowLights_MatInfo(pWorld, pFace->material);
        if (!pInfo) continue;

        // Glowing texels, or a surface the level marks as fully lit (light panels).
        float strength = 0.0f;
        float col[3];
        if (pInfo->glowFrac >= 0.08f) {
            strength = pInfo->glowFrac;
            memcpy(col, pInfo->glowColor, sizeof(col));
        }
        else if (pFace->extraLight >= 0.99f && pFace->lightingMode != RD_LIGHTMODE_NOTLIT) {
            strength = 0.6f;
            memcpy(col, pInfo->allColor, sizeof(col));
        }
        else {
            continue;
        }

        // Centre and area (triangle fan).
        rdVector3 centre = {0, 0, 0};
        for (uint32_t v = 0; v < pFace->numVertices; v++)
            rdVector_Add3Acc(&centre, &pWorld->aVertices[pFace->vertexPosIdx[v]]);
        rdVector_Scale3Acc(&centre, 1.0f / (float)pFace->numVertices);

        float area = 0.0f;
        rdVector3* p0 = &pWorld->aVertices[pFace->vertexPosIdx[0]];
        for (uint32_t v = 1; v + 1 < pFace->numVertices; v++) {
            rdVector3 a, b, c;
            rdVector_Sub3(&a, &pWorld->aVertices[pFace->vertexPosIdx[v]], p0);
            rdVector_Sub3(&b, &pWorld->aVertices[pFace->vertexPosIdx[v + 1]], p0);
            rdVector_Cross3(&c, &a, &b);
            area += 0.5f * rdVector_Len3(&c);
        }
        if (area <= 0.0f) continue;

        float size = sqrtf(area);
        sithGlowLight* pL = &aTmp[n++];
        // Hover a little in front of the surface. Flush against it, the light would only
        // graze its own plane, so a glowing floor strip could never put a glint on the floor
        // around it.
        const float offset = 0.12f;
        pL->pos.x = centre.x + pFace->normal.x * offset;
        pL->pos.y = centre.y + pFace->normal.y * offset;
        pL->pos.z = centre.z + pFace->normal.z * offset;
        sithGlowLights_Normalise(col);
        memcpy(pL->color, col, sizeof(col));
        // Brightness doesn't depend on how much of the texture glows (a thin strip on a big
        // floor texture is still a bright light), only a little on it.
        pL->intensity = fminf(0.5f + strength, 1.0f);
        pL->radius = fminf(fmaxf(0.6f + size * 2.5f, 0.6f), 3.0f);
        pL->sectorIdx = (int)(pSurf->pSector - pWorld->aSectors);
    }

    // Bucket by sector so gathering only touches visible sectors.
    sithGlowLights_aSectorStart = (int*)calloc(pWorld->numSectors + 1, sizeof(int));
    sithGlowLights_aLights = (sithGlowLight*)malloc(sizeof(sithGlowLight) * (n ? n : 1));
    if (!sithGlowLights_aSectorStart || !sithGlowLights_aLights) { free(aTmp); return; }
    for (int i = 0; i < n; i++) {
        int sIdx = aTmp[i].sectorIdx;
        if (sIdx >= 0 && sIdx < pWorld->numSectors) sithGlowLights_aSectorStart[sIdx + 1]++;
    }
    for (int s = 0; s < pWorld->numSectors; s++)
        sithGlowLights_aSectorStart[s + 1] += sithGlowLights_aSectorStart[s];
    int* aFill = (int*)calloc(pWorld->numSectors, sizeof(int));
    for (int i = 0; i < n && aFill; i++) {
        int sIdx = aTmp[i].sectorIdx;
        if (sIdx < 0 || sIdx >= pWorld->numSectors) continue;
        sithGlowLights_aLights[sithGlowLights_aSectorStart[sIdx] + aFill[sIdx]++] = aTmp[i];
    }
    free(aFill);
    free(aTmp);
    sithGlowLights_numLights = n;
    stdPlatform_Printf("sithGlowLights: %d glow lights from %d surfaces\n", n, pWorld->numSurfaces);
}

int sithGlowLights_MaterialColor(rdMaterial* pMat, float* pOutRGB)
{
    SithWorld* pWorld = sithWorld_g_pCurrentWorld;
    if (!pWorld || !pMat || !pOutRGB) return 0;
    if (pWorld != sithGlowLights_pWorld) sithGlowLights_Build(pWorld);
    sithGlowMatInfo* pInfo = sithGlowLights_MatInfo(pWorld, pMat);
    if (!pInfo) return 0;
    // Prefer clearly coloured texels: a saber blade is a white core with coloured edges,
    // and averaging everything gives near-white.
    const float* src = (pInfo->satColor[0] + pInfo->satColor[1] + pInfo->satColor[2] > 0.0f) ? pInfo->satColor
                     : (pInfo->glowFrac >= 0.05f) ? pInfo->glowColor : pInfo->allColor;
    if (src[0] + src[1] + src[2] <= 0.0f) return 0;
    memcpy(pOutRGB, src, sizeof(float) * 3);
    sithGlowLights_Normalise(pOutRGB);
    return 1;
}

float sithGlowLights_Strongest(const rdVector3* pPos, rdVector3* pOutDir)
{
    SithWorld* pWorld = sithWorld_g_pCurrentWorld;
    if (!pWorld || !pPos) return 0.0f;
    if (pWorld != sithGlowLights_pWorld) sithGlowLights_Build(pWorld);
    if (!sithGlowLights_numLights || !sithGlowLights_aSectorStart) return 0.0f;
    float best = 0.0f, total = 0.0f;
    const sithGlowLight* pBest = NULL;
    for (int k = 0; k < sithRender_g_numVisibleSectors; k++)
    {
        SithSector* pSector = sithRender_aVisibleSectors[k];
        if (!pSector) continue;
        int sIdx = (int)(pSector - pWorld->aSectors);
        if (sIdx < 0 || sIdx >= pWorld->numSectors) continue;
        for (int i = sithGlowLights_aSectorStart[sIdx]; i < sithGlowLights_aSectorStart[sIdx + 1]; i++)
        {
            const sithGlowLight* pL = &sithGlowLights_aLights[i];
            float d2 = rdVector_DistSquared3(&pL->pos, pPos);
            float score = pL->intensity / (d2 + 0.02f);
            total += score;
            if (score > best) { best = score; pBest = pL; }
        }
    }
    if (!pBest || total <= 0.0f) return 0.0f;
    rdVector_Sub3(pOutDir, &pBest->pos, pPos);
    rdVector_Normalize3Acc(pOutDir);
    // Dominance: how much of the nearby glow comes from this one light, faded out with distance.
    float dom = best / total;
    float near = best / (best + 4.0f);
    return dom * near;
}

int sithGlowLights_Count(void)
{
    return sithGlowLights_numLights;
}

int sithGlowLights_Gather(rdCamera* pCamera, float* aPos, float* aCol, int maxLights)
{
    SithWorld* pWorld = sithWorld_g_pCurrentWorld;
    if (!pWorld || !pCamera) return 0;
    if (pWorld != sithGlowLights_pWorld) sithGlowLights_Build(pWorld);
    if (!sithGlowLights_numLights || !sithGlowLights_aSectorStart) return 0;
    if (maxLights > SITHGLOWLIGHTS_MAX_GATHER) maxLights = SITHGLOWLIGHTS_MAX_GATHER;

    // Nearest-first selection among visible sectors.
    const sithGlowLight* aBest[SITHGLOWLIGHTS_MAX_GATHER];
    float aBestDist[SITHGLOWLIGHTS_MAX_GATHER];
    int numBest = 0;
    rdVector3 camPos = rdCamera_g_camMatrix.scale;

    for (int k = 0; k < sithRender_g_numVisibleSectors; k++)
    {
        SithSector* pSector = sithRender_aVisibleSectors[k];
        if (!pSector) continue;
        int sIdx = (int)(pSector - pWorld->aSectors);
        if (sIdx < 0 || sIdx >= pWorld->numSectors) continue;
        for (int i = sithGlowLights_aSectorStart[sIdx]; i < sithGlowLights_aSectorStart[sIdx + 1]; i++)
        {
            const sithGlowLight* pL = &sithGlowLights_aLights[i];
            float d = rdVector_Dist3(&pL->pos, &camPos) - pL->radius;
            if (numBest < maxLights) {
                aBest[numBest] = pL; aBestDist[numBest] = d; numBest++;
            }
            else {
                int worst = 0;
                for (int j = 1; j < numBest; j++) if (aBestDist[j] > aBestDist[worst]) worst = j;
                if (d < aBestDist[worst]) { aBest[worst] = pL; aBestDist[worst] = d; }
            }
        }
    }

    // Debug: OPENJKDF2_GLOW_DEBUG=1 prints what was gathered every 120 frames.
    static int dbg = -1, dbgCount = 0;
    if (dbg < 0) { const char* e = getenv("OPENJKDF2_GLOW_DEBUG"); dbg = (e && e[0] == '1'); }
    if (dbg && (dbgCount++ % 120) == 0) {
        float nearest = 1e9f;
        for (int i = 0; i < numBest; i++) if (aBestDist[i] < nearest) nearest = aBestDist[i];
        stdPlatform_Printf("sithGlowLights: gathered %d (visible sectors %d), nearest edge %.3f, cam %.2f %.2f %.2f\n",
                           numBest, sithRender_g_numVisibleSectors, nearest, camPos.x, camPos.y, camPos.z);
        for (int i = 0; i < numBest && i < 4; i++)
            stdPlatform_Printf("  light %.2f %.2f %.2f r=%.2f i=%.2f col=%.2f %.2f %.2f\n",
                               aBest[i]->pos.x, aBest[i]->pos.y, aBest[i]->pos.z, aBest[i]->radius, aBest[i]->intensity,
                               aBest[i]->color[0], aBest[i]->color[1], aBest[i]->color[2]);
    }

    for (int i = 0; i < numBest; i++)
    {
        rdVector3 cp;
        rdMatrix_TransformPoint34(&cp, &aBest[i]->pos, &pCamera->orient);
        aPos[i*4+0] = cp.x; aPos[i*4+1] = cp.y; aPos[i*4+2] = cp.z; aPos[i*4+3] = aBest[i]->radius;
        aCol[i*4+0] = aBest[i]->color[0]; aCol[i*4+1] = aBest[i]->color[1]; aCol[i*4+2] = aBest[i]->color[2];
        aCol[i*4+3] = aBest[i]->intensity;
    }
    return numBest;
}
