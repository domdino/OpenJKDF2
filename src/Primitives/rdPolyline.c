#include "rdPolyline.h"

#include "stdPlatform.h" // Added: *_ALLOC/*_FREE macros

#include "Engine/rdroid.h"
#include "Engine/rdCamera.h"
#include "General/stdMath.h"
#include "General/stdString.h"
#include "Raster/rdCache.h"
#include "Engine/rdColormap.h"
#include "Primitives/rdPrimit3.h"
#include "Primitives/rdDebug.h"
#include <math.h>

extern int jkSaber_rgbDrawActive;
// Added (remaster): procedural lightsaber blade (see rdPolyline_DrawBlade)
#define RD_PROC_FLAG_SABER_BLADE 0x20000000u
extern int jkSaber_bladeDrawActive;
extern float jkSaber_rgbRed, jkSaber_rgbGreen, jkSaber_rgbBlue;
extern int jkPlayer_bSaberGlow;
extern flex_t jkPlayer_saberGlowSize;
int sithGlowLights_MaterialColor(rdMaterial* pMat, float* pOutRGB);
static int rdPolyline_bBlade = 0;
static uint32_t rdPolyline_bladeColor = 0;
static rdVector2 rdPolyline_aBladeUVs[4];

// Added (remaster): swing trails. Each saber (polyline + the joint matrix it hangs from) keeps
// where its blade was over the last RDPOLYLINE_TRAIL_US; a fading ribbon is drawn through those
// positions, so fast swings smear like a bright arc. Time-based, so its length doesn't depend
// on the frame rate.
#define RDPOLYLINE_TRAIL_SABERS (16)
#define RDPOLYLINE_TRAIL_SAMPLES (12)
#define RDPOLYLINE_TRAIL_US (75000)
typedef struct rdPolylineTrail
{
    const void* pLine;
    const void* pOrient;
    int num;
    int lastFrame;
    uint64_t lastUs;
    rdVector3 aBase[RDPOLYLINE_TRAIL_SAMPLES]; // world space, newest first
    rdVector3 aTip[RDPOLYLINE_TRAIL_SAMPLES];
    uint64_t aUs[RDPOLYLINE_TRAIL_SAMPLES];
} rdPolylineTrail;
static rdPolylineTrail rdPolyline_aTrails[RDPOLYLINE_TRAIL_SABERS];
static rdVector3 polylineVerts[32]; // (tentative; defined again below with the original code)
extern int jkPlayer_bSaberTrails;

static rdPolylineTrail* rdPolyline_TrailFor(const void* pLine, const void* pOrient, uint64_t now)
{
    rdPolylineTrail* pOldest = &rdPolyline_aTrails[0];
    for (int i = 0; i < RDPOLYLINE_TRAIL_SABERS; i++) {
        rdPolylineTrail* t = &rdPolyline_aTrails[i];
        if (t->pLine == pLine && t->pOrient == pOrient) return t;
        if (t->lastUs < pOldest->lastUs) pOldest = t;
    }
    memset(pOldest, 0, sizeof(*pOldest));
    pOldest->pLine = pLine;
    pOldest->pOrient = pOrient;
    pOldest->lastFrame = -1;
    return pOldest;
}

static void rdPolyline_DrawTrail(rdThing* pLine, rdMatrix34* pOrient, flex_t R)
{
    rdPolyline* polyline = pLine->polyline;
    uint64_t now = Linux_TimeUs();
    rdVector3 wBase = pOrient->scale, wTip, v = {0.0, polyline->length, 0.0};
    rdMatrix_TransformPoint34(&wTip, &v, pOrient);

    rdPolylineTrail* t = rdPolyline_TrailFor(pLine, pOrient, now);
    if (t->num && now - t->lastUs > 200000) t->num = 0; // not drawn for a while: start fresh
    if (t->lastFrame != rdroid_frameTrue) {             // one sample per frame
        int n = t->num < RDPOLYLINE_TRAIL_SAMPLES ? t->num + 1 : RDPOLYLINE_TRAIL_SAMPLES;
        for (int i = n - 1; i > 0; i--) { t->aBase[i] = t->aBase[i - 1]; t->aTip[i] = t->aTip[i - 1]; t->aUs[i] = t->aUs[i - 1]; }
        t->aBase[0] = wBase; t->aTip[0] = wTip; t->aUs[0] = now;
        t->num = n;
        t->lastFrame = rdroid_frameTrue;
    }
    t->lastUs = now;
    if (!jkPlayer_bSaberTrails) return;

    rdMeshinfo idxInfo;
    idxInfo.numVertices = 4;
    idxInfo.aVertices = polylineVerts;
    idxInfo.paDynamicLight = 0;
    idxInfo.intensities = 0;
    idxInfo.aTexVerticies = rdPolyline_aBladeUVs;
    rdMatrix34* cam = &rdCamera_g_pCurCamera->orient;
    for (int i = 0; i + 1 < t->num; i++) {
        uint64_t ageNew = now - t->aUs[i], ageOld = now - t->aUs[i + 1];
        if (ageNew >= RDPOLYLINE_TRAIL_US) break;
        if (ageOld > RDPOLYLINE_TRAIL_US) ageOld = RDPOLYLINE_TRAIL_US;
        rdVector3 d;
        rdVector_Sub3(&d, &t->aTip[i], &t->aTip[i + 1]);
        flex_t sweep = rdVector_Len3(&d);
        // Only real swings smear: an idle sway (a fraction of the glow width per frame) read as a
        // pale flat slab beside the blade.
        if (sweep < R * 1.2f) continue;
        // The same light spread over a wider sweep is dimmer: intensity (vertex alpha) ~ R / sweep.
        flex_t k = (R * 1.5f) / sweep;
        if (k > 1.0f) k = 1.0f;
        if (k < 0.12f) k = 0.12f;
        rdPolyline_bladeColor = (rdPolyline_bladeColor & 0x00FFFFFF) | ((uint32_t)(k * 255.0f) << 24);
        rdVector3 c[4];
        rdMatrix_TransformPoint34(&c[0], &t->aTip[i], cam);
        rdMatrix_TransformPoint34(&c[1], &t->aTip[i + 1], cam);
        rdMatrix_TransformPoint34(&c[2], &t->aBase[i + 1], cam);
        rdMatrix_TransformPoint34(&c[3], &t->aBase[i], cam);
        // u = 10 + age fraction marks a trail fragment for the shader; v = 0 at the base, 1 at the tip
        flex_t fNew = 10.0f + (flex_t)ageNew / RDPOLYLINE_TRAIL_US, fOld = 10.0f + (flex_t)ageOld / RDPOLYLINE_TRAIL_US;
        for (int pass = 0; pass < 2; pass++) { // both windings: the ribbon can face either way
            for (int k = 0; k < 4; k++) polylineVerts[k] = c[pass ? 3 - k : k];
            flex_t us[4] = {fNew, fOld, fOld, fNew}, vs[4] = {1.0f, 1.0f, 0.0f, 0.0f};
            for (int k = 0; k < 4; k++) { int src = pass ? 3 - k : k; rdPolyline_aBladeUVs[k].x = us[src]; rdPolyline_aBladeUVs[k].y = vs[src]; }
            rdPolyline_DrawFace(pLine, &polyline->face, polylineVerts, &idxInfo);
        }
    }
}

#define RD_PROC_FLAG_SABER_RGB 0x40000000u

static rdVector3 polylineVerts[32]; // idk the size on this
static rdVector3 rdPolyline_FaceVerts[32];

rdPolyline* rdPolyline_New(char *polyline_fname, char *material_fname, char *material_fname2, flex_t length, flex_t base_rad, flex_t tip_rad, int lightmode, int texmode, int sortingmethod, flex_t extraLight)
{
    rdPolyline* polyline;

    polyline = (rdPolyline *)RDROID_ALLOC(sizeof(rdPolyline));
    if (polyline)
    {
        memset(polyline, 0, sizeof(*polyline)); // Added: clear struct
        rdPolyline_NewEntry(polyline, polyline_fname, material_fname, material_fname2, length, base_rad, tip_rad, lightmode, texmode, sortingmethod, extraLight);
    }
    return polyline;
}

int rdPolyline_NewEntry(rdPolyline *polyline, char *polyline_fname, char *material_side_fname, char *material_tip_fname, flex_t length, flex_t base_rad, flex_t tip_rad, rdGeoMode_t edgeGeometryMode, rdLightMode_t edgeLightingMode, rdTexMode_t edgeTextureMode, flex_t extraLight)
{

    rdMaterial *mat;
    int *vertexPosIdx;
    unsigned int numVertices;
    rdVector2 *extraUVTipMaybe;
    int *vertexUVIdx;
    rdVector2 *extraUVFaceMaybe;
    tVBuffer *v22;

    // Added: memleak mitigation
    rdPolyline_FreeEntry(polyline);

    if ( polyline_fname )
    {
        // TODO: caching?
#ifdef SITH_DEBUG_STRUCT_NAMES
        stdString_SafeStrCopy(polyline->fname, polyline_fname, 32);
#endif
    }
    polyline->length = length;
    polyline->baseRadius = base_rad;
    polyline->face.textureMode = edgeTextureMode;
    polyline->textureMode = edgeTextureMode;
    polyline->lightingMode = edgeLightingMode;
    polyline->tipRadius = tip_rad;
    polyline->face.type = 0;
    polyline->face.geometryMode = edgeGeometryMode;
    polyline->face.lightingMode = edgeLightingMode;
    polyline->geometryMode = edgeGeometryMode;
    polyline->face.extraLight = extraLight;

    polyline->face.material = rdMaterial_Load(material_side_fname, 0, 0);
    if ( !polyline->face.material )
        return 0;
    rdMaterial_EnsureDataForced(polyline->face.material); // Added: TWL
    polyline->face.numVertices = 4;
    vertexPosIdx = (int *)RDROID_ALLOC(sizeof(int) * polyline->face.numVertices);
    polyline->face.vertexPosIdx = vertexPosIdx;
    if ( !vertexPosIdx )
        return 0;
    numVertices = polyline->face.numVertices;
    for (int i = 0; i < numVertices; ++vertexPosIdx )
        *vertexPosIdx = i++;
    if ( polyline->face.geometryMode >= RD_GEOMETRY_FULL)
    {
        vertexUVIdx = (int *)RDROID_ALLOC(4 * numVertices);
        polyline->face.vertexUVIdx = vertexUVIdx;
        if ( !vertexUVIdx )
            return 0;
        for (int j = 0; j < polyline->face.numVertices; ++vertexUVIdx )
            *vertexUVIdx = j++;
        extraUVTipMaybe = (rdVector2 *)RDROID_ALLOC(sizeof(rdVector2) * polyline->face.numVertices);
        polyline->extraUVTipMaybe = extraUVTipMaybe;
        if ( !extraUVTipMaybe )
            return 0;
        // Odd quirk: This requires the material be actually loaded
        // Added: nullptr fallbacks
        v22 = NULL;
        if (polyline->face.material->texinfos[0] && polyline->face.material->texinfos[0]->texture_ptr) {
            v22 = polyline->face.material->texinfos[0]->texture_ptr->texture_struct[0];
        }
        extraUVTipMaybe[0].x = (flex_d_t)(v22 ? (unsigned int)v22->format.width : 1) - 0.01;// Added: nullptr check and fallback
        extraUVTipMaybe[0].y = 0.0;
        extraUVTipMaybe[1].x = 0.0;
        extraUVTipMaybe[1].y = 0.0;
        extraUVTipMaybe[2].x = 0.0;
        extraUVTipMaybe[2].y = (flex_d_t)(v22 ? (unsigned int)v22->format.height : 1) - 0.01;// Added: nullptr check and fallback
        extraUVTipMaybe[3].x = (flex_d_t)(v22 ? (unsigned int)v22->format.width : 1) - 0.01;// Added: nullptr check and fallback
        extraUVTipMaybe[3].y = (flex_d_t)(v22 ? (unsigned int)v22->format.height : 1) - 0.01;// Added: nullptr check and fallback
    }
    rdMaterial_OptionalFree(polyline->face.material); // Added: TWL
    polyline->tipFace.textureMode = edgeTextureMode;
    polyline->textureMode = edgeTextureMode;
    polyline->lightingMode = edgeLightingMode;
    polyline->tipFace.type = 0;
    polyline->tipFace.geometryMode = edgeGeometryMode;
    polyline->tipFace.lightingMode = edgeLightingMode;
    polyline->geometryMode = edgeGeometryMode;
    polyline->tipFace.extraLight = extraLight;
    polyline->tipFace.material = rdMaterial_Load(material_tip_fname, 0, 0);
    if ( !polyline->tipFace.material )
        return 0;
    rdMaterial_EnsureDataForced(polyline->tipFace.material); // Added: TWL
    polyline->tipFace.numVertices = 4;
    vertexPosIdx = (int *)RDROID_ALLOC(sizeof(int) * polyline->tipFace.numVertices);
    polyline->tipFace.vertexPosIdx = vertexPosIdx;
    if ( !vertexPosIdx )
        return 0;
    for (int k = 0; k < polyline->tipFace.numVertices; ++vertexPosIdx )
        *vertexPosIdx = k++;
    if ( polyline->tipFace.geometryMode >= RD_GEOMETRY_FULL)
    {
        vertexUVIdx = (int *)RDROID_ALLOC(sizeof(int) * polyline->tipFace.numVertices);
        polyline->tipFace.vertexUVIdx = vertexUVIdx;
        if ( !vertexUVIdx )
            return 0;
        for (int l = 0; l < polyline->tipFace.numVertices; ++vertexUVIdx )
            *vertexUVIdx = l++;
        extraUVFaceMaybe = (rdVector2 *)RDROID_ALLOC(sizeof(rdVector2) * polyline->tipFace.numVertices);
        polyline->extraUVFaceMaybe = extraUVFaceMaybe;
        if ( !extraUVFaceMaybe )
            return 0;
        // Odd quirk: This requires the material be actually loaded
        // Added: nullptr fallbacks
        v22 = NULL;
        if (polyline->tipFace.material->texinfos[0] && polyline->tipFace.material->texinfos[0]->texture_ptr) {
            v22 = polyline->tipFace.material->texinfos[0]->texture_ptr->texture_struct[0];
        }
        extraUVFaceMaybe[0].x = (flex_d_t)(v22 ? (unsigned int)v22->format.width : 1.0) - 0.01; // Added: nullptr check and fallback
        extraUVFaceMaybe[0].y = 0.0;
        extraUVFaceMaybe[1].x = 0.0;
        extraUVFaceMaybe[1].y = 0.0;
        extraUVFaceMaybe[2].x = 0.0;
        extraUVFaceMaybe[2].y = (flex_d_t)(v22 ? (unsigned int)v22->format.height : 1.0) - 0.01; // Added: nullptr check and fallback
        extraUVFaceMaybe[3].x = (flex_d_t)(v22 ? (unsigned int)v22->format.width : 1.0) - 0.01; // Added: nullptr check and fallback
        extraUVFaceMaybe[3].y = (flex_d_t)(v22 ? (unsigned int)v22->format.height : 1.0) - 0.01; // Added: nullptr check and fallback
    }
    rdMaterial_OptionalFree(polyline->tipFace.material); // Added: TWL
    return 1;
}

void rdPolyline_Free(rdPolyline *pPolyline)
{
    if ( pPolyline )
    {
        rdPolyline_FreeEntry(pPolyline);
        RDROID_FREE(pPolyline);
    }
}

void rdPolyline_FreeEntry(rdPolyline *pPolyline)
{
    if ( pPolyline->extraUVFaceMaybe )
    {
        RDROID_FREE(pPolyline->extraUVFaceMaybe);
        pPolyline->extraUVFaceMaybe = 0;
    }
    if ( pPolyline->extraUVTipMaybe )
    {
        RDROID_FREE(pPolyline->extraUVTipMaybe);
        pPolyline->extraUVTipMaybe = 0;
    }
    if ( pPolyline->tipFace.vertexPosIdx )
    {
        RDROID_FREE(pPolyline->tipFace.vertexPosIdx);
        pPolyline->tipFace.vertexPosIdx = 0;
    }
    if ( pPolyline->tipFace.vertexUVIdx )
    {
        RDROID_FREE(pPolyline->tipFace.vertexUVIdx);
        pPolyline->tipFace.vertexUVIdx = 0;
    }
    if ( pPolyline->face.vertexPosIdx )
    {
        RDROID_FREE(pPolyline->face.vertexPosIdx);
        pPolyline->face.vertexPosIdx = 0;
    }
    if ( pPolyline->face.vertexUVIdx )
    {
        RDROID_FREE(pPolyline->face.vertexUVIdx);
        pPolyline->face.vertexUVIdx = 0;
    }
}

// Added (remaster): one camera-facing capsule quad around the whole blade, glow radius R wide.
// UVs are blade-space in units of R (u across: -1..1, v along: -1 at the base cap to L/R+1 past
// the tip), the vertex colour is the blade colour with L/R/32 in alpha; the shader turns the
// distance to the core segment into a white-hot core, coloured glow, halo and moving plasma.
static int rdPolyline_DrawBlade(rdThing *pLine, rdMatrix34 *pOrient)
{
    rdPolyline* polyline = pLine->polyline;
    rdMatrix34 out;
    rdVector3 tip, base;
    rdMeshinfo idxInfo;
    rdMatrix_Multiply34(&out, &rdCamera_g_pCurCamera->orient, pOrient);
    rdVector3 v = {0.0, polyline->length, 0.0};
    rdMatrix_TransformPoint34(&tip, &v, &out);
    base = out.scale;

    flex_t core = polyline->baseRadius > polyline->tipRadius ? polyline->baseRadius : polyline->tipRadius;
    flex_t R = core * 7.0f * (jkPlayer_saberGlowSize > 0.05f ? jkPlayer_saberGlowSize : 0.05f);
    if (R <= 0.0f) R = 0.01f;
    flex_t dx = tip.x - base.x, dz = tip.z - base.z;
    flex_t mag = stdMath_Sqrt(dx * dx + dz * dz);
    flex_t ux = 0.0f, uz = 1.0f;
    if (mag > 0.000001f) { ux = dx / mag; uz = dz / mag; }
    flex_t nx = uz, nz = -ux; // the original blade quad's "+" side
    flex_t lr = mag / R;

    // The ends are extended along the blade's real 3D direction (screen-plane part normalised to
    // 1), so the quad's centre line runs exactly through the emitter and the tip and the
    // blade-space UVs line up with them in perspective. (Extending flat in the screen plane put
    // the core's start off the emitter whenever the blade runs towards or away from the camera.)
    flex_t uy = 0.0f;
    if (mag > 0.000001f) {
        uy = (tip.y - base.y) / mag;
        if (uy > 3.0f) uy = 3.0f;   // blade almost pointing at the camera: keep the caps sane
        if (uy < -3.0f) uy = -3.0f;
    }
    // Same corner order (and winding) as the original blade quad: tip+, tip-, base-, base+.
    polylineVerts[0].x = tip.x + (ux + nx) * R;  polylineVerts[0].z = tip.z + (uz + nz) * R;  polylineVerts[0].y = tip.y + uy * R;
    polylineVerts[1].x = tip.x + (ux - nx) * R;  polylineVerts[1].z = tip.z + (uz - nz) * R;  polylineVerts[1].y = tip.y + uy * R;
    polylineVerts[2].x = base.x - (ux + nx) * R; polylineVerts[2].z = base.z - (uz + nz) * R; polylineVerts[2].y = base.y - uy * R;
    polylineVerts[3].x = base.x - (ux - nx) * R; polylineVerts[3].z = base.z - (uz - nz) * R; polylineVerts[3].y = base.y - uy * R;
    rdPolyline_aBladeUVs[0].x = 1.0f;  rdPolyline_aBladeUVs[0].y = lr + 1.0f;
    rdPolyline_aBladeUVs[1].x = -1.0f; rdPolyline_aBladeUVs[1].y = lr + 1.0f;
    rdPolyline_aBladeUVs[2].x = -1.0f; rdPolyline_aBladeUVs[2].y = -1.0f;
    rdPolyline_aBladeUVs[3].x = 1.0f;  rdPolyline_aBladeUVs[3].y = -1.0f;

    // Colour: the player's custom RGB if set, else the blade texture's saturated colour.
    float rgb[3] = {0.6f, 0.8f, 1.0f};
    if (jkSaber_rgbDrawActive) { rgb[0] = jkSaber_rgbRed; rgb[1] = jkSaber_rgbGreen; rgb[2] = jkSaber_rgbBlue; }
    else sithGlowLights_MaterialColor(polyline->face.material, rgb);
    int a = (int)(lr / 32.0f * 255.0f + 0.5f); if (a > 255) a = 255; if (a < 0) a = 0;
    int r = (int)(rgb[0] * 255.0f), g = (int)(rgb[1] * 255.0f), b = (int)(rgb[2] * 255.0f);
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    rdPolyline_bladeColor = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;

    rdPolyline_bBlade = 1;
    uint32_t bladeColor = rdPolyline_bladeColor;
    rdPolyline_DrawTrail(pLine, pOrient, R); // uses the blade colour above; overwrites the shared verts/UVs
    rdPolyline_bladeColor = bladeColor;      // (the trail reuses the alpha byte for its intensity)

    // (re)build the blade quad: the trail reused the shared vertex/UV arrays
    polylineVerts[0].x = tip.x + (ux + nx) * R;  polylineVerts[0].z = tip.z + (uz + nz) * R;  polylineVerts[0].y = tip.y + uy * R;
    polylineVerts[1].x = tip.x + (ux - nx) * R;  polylineVerts[1].z = tip.z + (uz - nz) * R;  polylineVerts[1].y = tip.y + uy * R;
    polylineVerts[2].x = base.x - (ux + nx) * R; polylineVerts[2].z = base.z - (uz + nz) * R; polylineVerts[2].y = base.y - uy * R;
    polylineVerts[3].x = base.x - (ux - nx) * R; polylineVerts[3].z = base.z - (uz - nz) * R; polylineVerts[3].y = base.y - uy * R;
    rdPolyline_aBladeUVs[0].x = 1.0f;  rdPolyline_aBladeUVs[0].y = lr + 1.0f;
    rdPolyline_aBladeUVs[1].x = -1.0f; rdPolyline_aBladeUVs[1].y = lr + 1.0f;
    rdPolyline_aBladeUVs[2].x = -1.0f; rdPolyline_aBladeUVs[2].y = -1.0f;
    rdPolyline_aBladeUVs[3].x = 1.0f;  rdPolyline_aBladeUVs[3].y = -1.0f;
    idxInfo.numVertices = 4;
    idxInfo.aVertices = polylineVerts;
    idxInfo.paDynamicLight = 0;
    idxInfo.intensities = 0;
    idxInfo.aTexVerticies = rdPolyline_aBladeUVs;
    rdPolyline_DrawFace(pLine, &polyline->face, polylineVerts, &idxInfo);
    rdPolyline_bBlade = 0;
    return 1;
}

int rdPolyline_Draw(rdThing *pLine, rdMatrix34 *pOrient)
{
    if (jkSaber_bladeDrawActive && jkPlayer_bSaberGlow) // Added (remaster)
        return rdPolyline_DrawBlade(pLine, pOrient);

    rdPolyline *polyline;
    flex_t length;
    flex_d_t tip_left;
    flex_d_t tip_bottom;
    flex_d_t tip_right;
    flex_d_t tip_top;
    flex_t ang;
    flex_t angSin;
    flex_t angCos;
    rdVector3 vertex_out;
    rdMatrix34 out;
    rdVector3 vertex;
    rdMeshinfo idxInfo;

    polyline = pLine->polyline;
    
    // This is slightly different than IDA?
    idxInfo.numVertices = 4;
    idxInfo.aVertices = polylineVerts;
    idxInfo.paDynamicLight = 0;
    idxInfo.intensities = 0;

    rdMatrix_Multiply34(&out, &rdCamera_g_pCurCamera->orient, pOrient);
    vertex.x = 0.0;
    vertex.y = polyline->length;
    vertex.z = 0.0;
    rdMatrix_TransformPoint34(&vertex_out, &vertex, &out);
    tip_left = vertex_out.x - polyline->tipRadius;
    tip_bottom = vertex_out.z - polyline->tipRadius;
    tip_right = vertex_out.x + polyline->tipRadius;
    tip_top = vertex_out.z + polyline->tipRadius;

    flex_t epislon = -0.001;

    // Tip
    {
        polylineVerts[0].x = tip_left;
        polylineVerts[0].y = vertex_out.y - epislon;
        polylineVerts[0].z = tip_bottom;
        polylineVerts[1].x = tip_right;
        polylineVerts[1].y = vertex_out.y - epislon;
        polylineVerts[1].z = tip_bottom;
        polylineVerts[2].x = tip_right;
        polylineVerts[2].y = vertex_out.y - epislon;
        polylineVerts[2].z = tip_top;
        polylineVerts[3].x = tip_left;
        polylineVerts[3].y = vertex_out.y - epislon;
        polylineVerts[3].z = tip_top;
        idxInfo.aTexVerticies = polyline->extraUVFaceMaybe;
        rdPolyline_DrawFace(pLine, &polyline->tipFace, polylineVerts, &idxInfo);
    }

    // Base
    {
        polylineVerts[0].x = out.scale.x - polyline->baseRadius;
        polylineVerts[0].y = out.scale.y - epislon;
        polylineVerts[0].z = out.scale.z - polyline->baseRadius;
        polylineVerts[1].x = out.scale.x + polyline->baseRadius;
        polylineVerts[1].y = out.scale.y - epislon;
        polylineVerts[1].z = out.scale.z - polyline->baseRadius;
        polylineVerts[2].x = out.scale.x + polyline->baseRadius;
        polylineVerts[2].y = out.scale.y - epislon;
        polylineVerts[2].z = out.scale.z + polyline->baseRadius;
        polylineVerts[3].x = out.scale.x - polyline->baseRadius;
        polylineVerts[3].y = out.scale.y - epislon;
        polylineVerts[3].z = out.scale.z + polyline->baseRadius;
        idxInfo.aTexVerticies = polyline->extraUVFaceMaybe;
        rdPolyline_DrawFace(pLine, &polyline->tipFace, polylineVerts, &idxInfo);
    }
    

    // Blade
    {
        flex_t zdist = vertex_out.z - out.scale.z;
        flex_t xdist = vertex_out.x - out.scale.x;
        flex_t mag = stdMath_Sqrt(xdist * xdist + zdist * zdist);

        // Added: prevent div 0
        if (mag == 0)
        {
            mag = 0.000001f;
        }

        ang = stdMath_ArcSin3((-xdist) / mag);
        if ( zdist < 0.0 )
        {
            if ( xdist <= 0.0 )
                ang = -(ang - -180.0);
            else
                ang = 180.0 - ang;
        }
        stdMath_SinCos(ang, &angSin, &angCos);
        polylineVerts[0].x = (polyline->tipRadius * angCos) - (mag * angSin) + out.scale.x;
        polylineVerts[0].y = vertex_out.y;
        polylineVerts[0].z = (polyline->tipRadius * angSin) + (mag * angCos) + out.scale.z;
        
        polylineVerts[1].x = (-polyline->tipRadius * angCos) - (mag * angSin) + out.scale.x;
        polylineVerts[1].y = vertex_out.y;
        polylineVerts[1].z = (-polyline->tipRadius * angSin) + (mag * angCos) + out.scale.z;
        
        polylineVerts[2].x = (-polyline->baseRadius * angCos) - (flex_t)0.0 + out.scale.x;
        polylineVerts[2].y = out.scale.y;
        polylineVerts[2].z = (-polyline->baseRadius * angSin) + (flex_t)0.0 + out.scale.z;
        
        polylineVerts[3].x = (polyline->baseRadius * angCos) - (flex_t)0.0 + out.scale.x;
        polylineVerts[3].y = out.scale.y;
        polylineVerts[3].z = (polyline->baseRadius * angSin) + (flex_t)0.0 + out.scale.z;
        idxInfo.aTexVerticies = polyline->extraUVTipMaybe;
        rdPolyline_DrawFace(pLine, &polyline->face, polylineVerts, &idxInfo);
    }
    return 1;
}

void rdPolyline_DrawFace(rdThing *pLine, rdFace *pFace, rdVector3 *aVertices, rdMeshinfo *aUVs)
{
    rdProcEntry *procEntry;
    rdMeshinfo mesh_out;
    flex_t staticLight;

    procEntry = rdCache_GetProcEntry();
    if (!procEntry)
        return;

    mesh_out.aVertices = rdPolyline_FaceVerts;
    mesh_out.verticesOrig = procEntry->aVertices;
    mesh_out.aTexVerticies = procEntry->aTexVerticies;
    mesh_out.paDynamicLight = procEntry->vertexIntensities;
    
    aUVs->numVertices = pFace->numVertices;
    aUVs->vertexPosIdx = pFace->vertexPosIdx;
    aUVs->vertexUVIdx = pFace->vertexUVIdx;
    
    rdGeoMode_t curGeometryMode_ = rdroid_g_curGeometryMode;
    rdLightMode_t curLightingMode_ = rdroid_g_curLightingMode;
    rdTexMode_t curTextureMode_ = rdroid_curTextureMode;

    if ( curGeometryMode_ >= pFace->geometryMode )
        curGeometryMode_ = pFace->geometryMode;
    if ( curGeometryMode_ >= pLine->curGeoMode )
    {
        procEntry->geometryMode = pLine->curGeoMode;
    }    
    else if ( rdroid_g_curGeometryMode >= pFace->geometryMode )
    {
        procEntry->geometryMode = pFace->geometryMode;
    }
    else
    {
        procEntry->geometryMode = rdroid_g_curGeometryMode;
    }
    
    procEntry->geometryMode = procEntry->geometryMode;
    if ( rdroid_g_curRenderOptions & 2 && rdCamera_g_pCurCamera->ambientLight >= 1.0 )
    {
        procEntry->lightingMode = RD_LIGHTMODE_FULLYLIT;
    }
    else
    {
        if ( curLightingMode_ >= pFace->lightingMode )
            curLightingMode_ = pFace->lightingMode;
        if ( curLightingMode_ >= pLine->curLightMode )
        {
            pFace->lightingMode = pLine->curLightMode;
        }
        else if ( rdroid_g_curLightingMode < pFace->lightingMode )
        {
            pFace->lightingMode = rdroid_g_curLightingMode;
        }
        procEntry->lightingMode = pFace->lightingMode;
    }

    if ( curTextureMode_ >= pFace->textureMode )
        curTextureMode_ = pFace->textureMode;
    
    procEntry->textureMode = pLine->curTexMode;
    if ( curTextureMode_ < procEntry->textureMode )
    {
        if ( curTextureMode_ >= pFace->textureMode )
            procEntry->textureMode = pFace->textureMode;
        else
            procEntry->textureMode = rdroid_curTextureMode;
    }

    // Added (remaster): the procedural blade's UVs are blade space, so skip the 1997 shimmer offset
    // (jkSaber_PolylineRand scrolls texVertOffset every frame).
    rdVector2 zeroOffset = {0.0, 0.0};
    rdPrimit3_ClipFace(rdCamera_g_pCurCamera->pClipFrustum, procEntry->geometryMode, procEntry->lightingMode, procEntry->textureMode, aUVs, &mesh_out,
                       rdPolyline_bBlade ? &zeroOffset : &pFace->texVertOffset);
    if ( mesh_out.numVertices < 3 )
        return;

    rdCamera_g_pCurCamera->pfProjectList(mesh_out.verticesOrig, mesh_out.aVertices, mesh_out.numVertices);

    if ( rdroid_g_curRenderOptions & 2 )
        procEntry->ambientLight = rdCamera_g_pCurCamera->ambientLight;
    else
        procEntry->ambientLight = 0.0;

    // Software renderer optimizations, skip
#ifndef TARGET_TWL
    if ( procEntry->lightingMode )
    {
        if ( procEntry->ambientLight < 1.0 )
        {
            if ( procEntry->lightingMode == 2 )
            {
                if ( procEntry->light_level_static < 1.0 || rdColormap_pCurMap != rdColormap_pIdentityMap )
                {
                    if ( procEntry->light_level_static <= 0.0 )
                        procEntry->lightingMode = 1;
                }
                else
                {
                    procEntry->lightingMode = 0;
                }
            }
            else if ( procEntry->lightingMode == 3 )
            {
                int i;
                staticLight = *procEntry->vertexIntensities;
                for (i = 1; i < mesh_out.numVertices; i++)
                {
                    if ( procEntry->vertexIntensities[i] != staticLight )
                        break;
                }
                if ( i == mesh_out.numVertices )
                {
                    if ( staticLight == 1.0 )
                    {
                        if ( rdColormap_pCurMap == rdColormap_pIdentityMap )
                        {
                            procEntry->lightingMode = 0;
                        }
                        else
                        {
                            procEntry->lightingMode = 2;
                            procEntry->light_level_static = 1.0;
                        }
                    }
                    else if ( staticLight == 0.0 )
                    {
                        procEntry->lightingMode = 1;
                        procEntry->light_level_static = 0.0;
                    }
                    else
                    {
                        procEntry->lightingMode = 2;
                        procEntry->light_level_static = staticLight;
                    }
                }
            }
        }
        else if ( rdColormap_pCurMap == rdColormap_pIdentityMap )
        {
            procEntry->lightingMode = 0;
        }
        else
        {
            procEntry->lightingMode = 2;
            procEntry->light_level_static = 1.0;
        }
    }
#else
    if ( procEntry->lightingMode == 3 )
    {
        procEntry->light_level_static = *procEntry->vertexIntensities;
    }
#endif
    
    int procFaceFlags = 1;
    if ( procEntry->geometryMode >= 4 )
        procFaceFlags = 3;
    if ( procEntry->lightingMode >= 3 )
        procFaceFlags |= 4u;

    procEntry->light_flags = jkSaber_rgbDrawActive
        ? RD_PROC_FLAG_SABER_RGB
        : 0;
    if (rdPolyline_bBlade) { // Added (remaster)
        procEntry->light_flags = RD_PROC_FLAG_SABER_BLADE;
        procEntry->bladeColor = rdPolyline_bladeColor;
    }
    procEntry->wallCel = pLine->wallCel;
    procEntry->type = pFace->type;
    procEntry->extralight = pFace->extraLight;
    procEntry->material = pFace->material;

    // Added: Polylines should always be drawn
    rdMaterial_EnsureDataForced(procEntry->material);

    rdCache_AddProcFace(0, mesh_out.numVertices, procFaceFlags);
}
