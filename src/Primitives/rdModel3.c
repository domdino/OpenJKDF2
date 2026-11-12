#include "rdModel3.h"
#include "World/jkPlayer.h" // Added (remaster): r_perPixelLighting

// Added: on TWL, the big cold model payload arrays (aVertices/UVs/normals/faces/
// index pools -- word-safe writes only, parsed once) go to the slot-2 extram
// heap. Not enabled on DC: these are read in per-frame transform loops and DC
// VRAM CPU reads are uncached.
#ifdef TARGET_TWL
#define RDMODEL3_EXTRAM_SUGGEST() int _prevSuggest = rdroid_g_pHS->suggestHeap(HEAP_WORD_ADDRESSABLE)
#define RDMODEL3_EXTRAM_RESTORE() rdroid_g_pHS->suggestHeap(_prevSuggest)
#else
#define RDMODEL3_EXTRAM_SUGGEST() do {} while (0)
#define RDMODEL3_EXTRAM_RESTORE() do {} while (0)
#endif

#include "Engine/rdroid.h"
#include "General/stdConffile.h"
#include "General/stdString.h"
#include "stdPlatform.h"
#include "Primitives/rdVector.h"
#include "Primitives/rdMatrix.h"
#include "Primitives/rdMath.h"
#include "Engine/rdPuppet.h"
#include "Engine/rdClip.h"
#include "Win95/std.h"
#include "Raster/rdCache.h"
#include "Engine/rdColormap.h"
#include "Primitives/rdPrimit3.h"
#include "Primitives/rdDebug.h"
void rdModel3_CinematicLight(rdMesh* pMesh, rdMatrix34* pMatInv, rdMatrix34* pOrient); // Added (remaster)
#include "Engine/sithGlowLights.h"

model3Loader_t rdModel3_RegisterLoader(model3Loader_t pfFunc)
{
    model3Loader_t result = pModel3Loader;
    pModel3Loader = pfFunc;
    return result;
}

model3Unloader_t rdModel3_RegisterUnloader(model3Unloader_t pfFunc)
{
    model3Unloader_t result = pModel3Unloader;
    pModel3Unloader = pfFunc;
    return result;
}

void rdModel3_ClearFrameCounters()
{
    rdModel3_numDrawnModels = 0;
}

int rdModel3_NewEntry(rdModel3 *pModel3)
{
    stdPlatform_Memzero32(pModel3, sizeof(rdModel3)); // Added: word-safe (aModels array may be in extram)
    stdString_SafeStrCopy(pModel3->filename, "UNKNOWN", 32);
    pModel3->geosetSelect = 0;
    return 0;
}

rdModel3* rdModel3_Load(char *pName)
{
    rdModel3 *model;

    if ( pModel3Loader )
        return (rdModel3 *)pModel3Loader(pName, 0);
    model = (rdModel3 *)RDROID_ALLOC(sizeof(rdModel3));
    if ( model )
    {
        if ( rdModel3_LoadEntry(pName, model) )
            return model;
        rdModel3_Free(model);
    }
    return 0;
}

#define rdModel3_HelpDebug(s, ...) (s)

// MOTS altered (RGB aLights?)
// ---------------------------------------------------------------- Added (remaster): skinning
//
// <model>.skn, next to the .3do (e.g. 3do\ky.skn), text:
//   SKIN 1
//   MESH <geoset> <mesh> <numVertices>
//   <vertex>: <count> <node> <weight> [<node> <weight> ...]     (one line per vertex, up to 4)
//   ... more MESH blocks ...
// Weights are normalised on load. Meshes not listed stay rigid.

static void rdModel3_FreeSkin(rdMesh* pMesh)
{
    rdSkin* pSkin = pMesh->pSkin;
    if (!pSkin) return;
    if (pSkin->aNodes) RDROID_FREE(pSkin->aNodes);
    if (pSkin->aWeights) RDROID_FREE(pSkin->aWeights);
    if (pSkin->aRestFaceNormals) RDROID_FREE(pSkin->aRestFaceNormals);
    if (pSkin->aSkinned) RDROID_FREE(pSkin->aSkinned);
    if (pSkin->aSkinnedNormals) RDROID_FREE(pSkin->aSkinnedNormals);
    if (pSkin->aBindToNode) RDROID_FREE(pSkin->aBindToNode);
    RDROID_FREE(pSkin);
    pMesh->pSkin = NULL;
}

static void rdModel3_LoadSkin(const char* pModelPath, rdModel3* pModel3)
{
    char path[128];
    stdString_SafeStrCopy(path, pModelPath, sizeof(path) - 4);
    char* dot = strrchr(path, '.');
    if (!dot) return;
    strcpy(dot, ".skn");

    stdFile_t fd = rdroid_g_pHS->fileOpen(path, "r");
    if (!fd) return;
    rdroid_g_pHS->fileClose(fd);
    if (!stdConffile_Open(path)) return;

    int numSkinned = 0, version = 0;
    if (!stdConffile_ReadLine() || _sscanf(stdConffile_g_aLine, " skin %d", &version) != 1) {
        stdPlatform_Printf("rdModel3: %s: bad header\n", path);
        stdConffile_Close();
        return;
    }
    while (stdConffile_ReadLine())
    {
        int geo, meshIdx, numVerts;
        if (_sscanf(stdConffile_g_aLine, " mesh %d %d %d", &geo, &meshIdx, &numVerts) != 3) continue;
        if (geo < 0 || geo >= (int)pModel3->numGeos || meshIdx < 0 || meshIdx >= (int)pModel3->aGeos[geo].numMeshes) {
            stdPlatform_Printf("rdModel3: %s: no mesh %d in geoset %d\n", path, meshIdx, geo);
            break;
        }
        rdMesh* pMesh = &pModel3->aGeos[geo].aMeshes[meshIdx];
        if (numVerts != pMesh->numVertices) {
            stdPlatform_Printf("rdModel3: %s: mesh %s has %d vertices, skin has %d\n", path, pMesh->name, pMesh->numVertices, numVerts);
            break;
        }
        rdModel3_FreeSkin(pMesh);
        rdSkin* pSkin = (rdSkin*)RDROID_ALLOC(sizeof(rdSkin));
        memset(pSkin, 0, sizeof(*pSkin));
        pSkin->numVertices = numVerts;
        pSkin->numFaces = pMesh->numFaces;
        pSkin->meshNode = -1;
        for (int n = 0; n < (int)pModel3->numHNodes; n++) {
            if (pModel3->aHierarchyNodes[n].meshIdx == meshIdx) { pSkin->meshNode = n; break; }
        }
        pSkin->numNodes = pModel3->numHNodes;
        pSkin->aNodes = (int*)RDROID_ALLOC(sizeof(int) * numVerts * RDSKIN_MAX_INFLUENCES);
        pSkin->aWeights = (flex_t*)RDROID_ALLOC(sizeof(flex_t) * numVerts * RDSKIN_MAX_INFLUENCES);
        pSkin->aRestFaceNormals = (rdVector3*)RDROID_ALLOC(sizeof(rdVector3) * (pMesh->numFaces ? pMesh->numFaces : 1));
        pSkin->aSkinned = (rdVector3*)RDROID_ALLOC(sizeof(rdVector3) * numVerts);
        pSkin->aSkinnedNormals = (rdVector3*)RDROID_ALLOC(sizeof(rdVector3) * numVerts);
        pSkin->aBindToNode = (rdMatrix34*)RDROID_ALLOC(sizeof(rdMatrix34) * pSkin->numNodes);
        for (int i = 0; i < numVerts * RDSKIN_MAX_INFLUENCES; i++) { pSkin->aNodes[i] = -1; pSkin->aWeights[i] = 0; }

        int ok = 1;
        for (int v = 0; v < numVerts; v++)
        {
            if (!stdConffile_ReadLine()) { ok = 0; break; }
            int idx, cnt, consumed = 0;
            const char* p = stdConffile_g_aLine;
            if (_sscanf(p, " %d: %d%n", &idx, &cnt, &consumed) != 2 || idx != v) { ok = 0; break; }
            p += consumed;
            flex_t total = 0;
            for (int k = 0; k < cnt; k++) {
                int node; float w;
                if (_sscanf(p, " %d %f%n", &node, &w, &consumed) != 2) { ok = 0; break; }
                p += consumed;
                if (k >= RDSKIN_MAX_INFLUENCES || node < 0 || node >= pSkin->numNodes || w <= 0) continue;
                pSkin->aNodes[v * RDSKIN_MAX_INFLUENCES + k] = node;
                pSkin->aWeights[v * RDSKIN_MAX_INFLUENCES + k] = w;
                total += w;
            }
            if (!ok) break;
            if (total <= 0) {
                pSkin->aNodes[v * RDSKIN_MAX_INFLUENCES] = pSkin->meshNode;
                pSkin->aWeights[v * RDSKIN_MAX_INFLUENCES] = 1;
                total = 1;
            }
            for (int k = 0; k < RDSKIN_MAX_INFLUENCES; k++) pSkin->aWeights[v * RDSKIN_MAX_INFLUENCES + k] /= total;
        }
        if (!ok || pSkin->meshNode < 0) {
            stdPlatform_Printf("rdModel3: %s: bad weights for mesh %s\n", path, pMesh->name);
            pMesh->pSkin = pSkin;
            rdModel3_FreeSkin(pMesh);
            break;
        }
        pMesh->pSkin = pSkin;
        // A skinned mesh moves away from its node, so cull with a generous radius.
        pMesh->radius *= 3.0f;
        numSkinned++;
    }
    stdConffile_Close();
    if (numSkinned)
        stdPlatform_Printf("rdModel3: %s: %d skinned mesh(es)\n", path, numSkinned);
}

static void rdModel3_SkinAccumulateRest(rdModel3* pModel, rdHierarchyNode* pNode, const rdMatrix34* pParent, rdMatrix34* aOut)
{
    rdMatrix34 matrix;
    rdVector3 negPivot;
    rdMatrix_BuildTranslate34(&matrix, &pNode->pivot);
    rdMatrix_PostMultiply34(&matrix, &pNode->posRotMatrix);
    if (pNode->parent) {
        rdVector_Neg3(&negPivot, &pNode->parent->pivot);
        rdMatrix_PostTranslate34(&matrix, &negPivot);
    }
    rdMatrix_Multiply34(&aOut[pNode->idx], pParent, &matrix);
    rdHierarchyNode* child = pNode->child;
    for (int i = 0; i < pNode->numChildren && child; i++) {
        rdModel3_SkinAccumulateRest(pModel, child, &aOut[pNode->idx], aOut);
        child = child->nextSibling;
    }
}

// Same maths as rdThing_AccumulateMatrices, at the rest pose with no placement.
static void rdModel3_SkinBuildBind(rdModel3* pModel, rdSkin* pSkin)
{
    rdMatrix34* aRest = (rdMatrix34*)RDROID_ALLOC(sizeof(rdMatrix34) * pSkin->numNodes);
    rdMatrix34 ident, inv;
    rdMatrix_Identity34(&ident);
    for (int i = 0; i < pSkin->numNodes; i++) rdMatrix_Identity34(&aRest[i]);
    rdModel3_SkinAccumulateRest(pModel, pModel->aHierarchyNodes, &ident, aRest);
    for (int j = 0; j < pSkin->numNodes; j++) {
        rdMatrix_InvertOrtho34(&inv, &aRest[j]);
        rdMatrix_Multiply34(&pSkin->aBindToNode[j], &inv, &aRest[pSkin->meshNode]);
    }
    RDROID_FREE(aRest);
    pSkin->bBindReady = 1;
}

// Bends the mesh into its current pose, in the space of its own node (so everything after
// this in rdModel3_DrawMesh is unchanged), and swaps the bent arrays in until the draw ends.
static void rdModel3_SkinBegin(rdMesh* pMesh, const rdMatrix34* pOrient)
{
    rdSkin* pSkin = pMesh->pSkin;
    rdModel3* pModel = pCurThing->model3;
    if (!pSkin->bBindReady) rdModel3_SkinBuildBind(pModel, pSkin);

    rdMatrix34 invOrient, tmp;
    rdMatrix34 aK[RDPUPPET_MAX_NODES];
    rdMatrix_InvertOrtho34(&invOrient, pOrient);
    int numNodes = pSkin->numNodes < RDPUPPET_MAX_NODES ? pSkin->numNodes : RDPUPPET_MAX_NODES;
    for (int j = 0; j < numNodes; j++) {
        rdMatrix_Multiply34(&tmp, &pCurThing->paJointMatrices[j], &pSkin->aBindToNode[j]);
        rdMatrix_Multiply34(&aK[j], &invOrient, &tmp);
    }

    pSkin->aRestVertices = pMesh->aVertices;
    pSkin->aRestNormals = pMesh->vertexNormals;
    for (int v = 0; v < pSkin->numVertices; v++) {
        rdVector3 p = {0, 0, 0}, n = {0, 0, 0}, t;
        for (int k = 0; k < RDSKIN_MAX_INFLUENCES; k++) {
            int node = pSkin->aNodes[v * RDSKIN_MAX_INFLUENCES + k];
            if (node < 0 || node >= numNodes) continue;
            flex_t w = pSkin->aWeights[v * RDSKIN_MAX_INFLUENCES + k];
            rdMatrix_TransformPoint34(&t, &pSkin->aRestVertices[v], &aK[node]);
            p.x += t.x * w; p.y += t.y * w; p.z += t.z * w;
            if (pSkin->aRestNormals) {
                rdMatrix_TransformVector34(&t, &pSkin->aRestNormals[v], &aK[node]);
                n.x += t.x * w; n.y += t.y * w; n.z += t.z * w;
            }
        }
        pSkin->aSkinned[v] = p;
        rdVector_Normalize3Acc(&n);
        pSkin->aSkinnedNormals[v] = n;
    }
    pMesh->aVertices = pSkin->aSkinned;
    if (pSkin->aRestNormals) pMesh->vertexNormals = pSkin->aSkinnedNormals;

    for (int f = 0; f < pMesh->numFaces; f++) {
        rdFace* face = &pMesh->faces[f];
        pSkin->aRestFaceNormals[f] = face->normal;
        if (face->numVertices < 3) continue;
        // New normal from the bent triangle, oriented like the rest normal carried along by the
        // first vertex's strongest node (the winding convention then doesn't matter).
        rdVector3 a, b, nrm, ref;
        int v0 = face->vertexPosIdx[0];
        rdVector_Sub3(&a, &pMesh->aVertices[face->vertexPosIdx[1]], &pMesh->aVertices[v0]);
        rdVector_Sub3(&b, &pMesh->aVertices[face->vertexPosIdx[2]], &pMesh->aVertices[v0]);
        rdVector_Cross3(&nrm, &a, &b);
        int node = pSkin->aNodes[v0 * RDSKIN_MAX_INFLUENCES];
        if (node < 0 || node >= numNodes) node = pSkin->meshNode;
        rdMatrix_TransformVector34(&ref, &pSkin->aRestFaceNormals[f], &aK[node]);
        if (rdVector_Normalize3Acc(&nrm) > 0) {
            if (nrm.x * ref.x + nrm.y * ref.y + nrm.z * ref.z < 0) rdVector_Neg3Acc(&nrm);
            face->normal = nrm;
        }
        else face->normal = ref;
    }
}

static void rdModel3_SkinEnd(rdMesh* pMesh)
{
    rdSkin* pSkin = pMesh->pSkin;
    pMesh->aVertices = pSkin->aRestVertices;
    pMesh->vertexNormals = pSkin->aRestNormals;
    for (int f = 0; f < pMesh->numFaces; f++) pMesh->faces[f].normal = pSkin->aRestFaceNormals[f];
}
// ---------------------------------------------------------------- end skinning

int rdModel3_LoadEntry(char *pFilename, rdModel3 *pModel3)
{
    rdMesh *mesh; // ebx
    int vertex_num; // edi
    int v25; // edi
    int v29; // edi
    rdVector3 *vertex_normal; // eax
    char *tmpTxt; // eax
    int v36; // eax
    char *to_num_verts; // eax
    unsigned int v49; // ebp
    unsigned int v52; // ebp
    int v55; // edi
    unsigned int idx; // edi
    rdHierarchyNode *node; // esi
    flex32_t v_z; // [esp+14h] [ebp-80h]
    flex32_t v_y; // [esp+18h] [ebp-7Ch]
    flex32_t v_x; // [esp+1Ch] [ebp-78h]
    rdFace *face; // [esp+34h] [ebp-60h]
    int v78; // [esp+50h] [ebp-44h]
    int sibling; // [esp+54h] [ebp-40h]
    flex32_t pitch; // [esp+58h] [ebp-3Ch]
    flex32_t v_i; // [esp+5Ch] [ebp-38h]
    flex32_t yaw; // [esp+60h] [ebp-34h]
    flex32_t v_v; // [esp+64h] [ebp-30h]
    flex32_t roll; // [esp+68h] [ebp-2Ch]
    int parent; // [esp+6Ch] [ebp-28h]
    flex32_t pivot_x; // [esp+70h] [ebp-24h]
    flex32_t radius; // [esp+74h] [ebp-20h]
    flex32_t pivot_y; // [esp+78h] [ebp-1Ch]
    flex32_t extralight; // [esp+7Ch] [ebp-18h]
    flex32_t pivot_z; // [esp+80h] [ebp-14h]
    flex32_t v_u; // [esp+84h] [ebp-10h]
    int child; // [esp+88h] [ebp-Ch]
    int version_minor; // [esp+8Ch] [ebp-8h]
    int version_major; // [esp+90h] [ebp-4h]
    int geoset_num;

    rdModel3_NewEntry(pModel3);
    stdString_SafeStrCopy(pModel3->filename, stdFileFromPath(pFilename), 32);

    //rdModel3_HelpDebug("OpenJKDF2: %s -> `%s`\n", __func__, model_fpath); // Added

    if ( !stdConffile_Open(pFilename) ) {
        rdModel3_HelpDebug("OpenJKDF2: %s: Failed to open file `%s`\n", __func__, pFilename); // Added
        return 0;
    }

    if (!stdConffile_ReadLine())
        return 0;

    if ( _sscanf(stdConffile_g_aLine, " section: %s", std_g_genBuffer) != 1 ) {
        rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse section line\n", __func__); // Added
        return 0;
    }

    if (!stdConffile_ReadLine()) {
        rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse line after section line\n", __func__); // Added
        return 0;
    }

    _sscanf(stdConffile_g_aLine, " 3do %d.%d", &version_major, &version_minor);
    if (!stdConffile_ReadLine()) {
        rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse 3do version\n", __func__); // Added
        return 0;
    }

    if ( _sscanf(stdConffile_g_aLine, " section: %s", std_g_genBuffer) != 1
      || !stdConffile_ReadLine()
      || _sscanf(stdConffile_g_aLine, " materials %d", &pModel3->sizeMaterials) != 1 ) {

        rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse section or materials\n", __func__); // Added
        return 0;
    }

    if ( pModel3->sizeMaterials)
    {
        pModel3->aMaterials = (rdMaterial **)RDROID_ALLOC(sizeof(rdMaterial*) * pModel3->sizeMaterials);
        if (!pModel3->aMaterials) {
            rdModel3_HelpDebug("OpenJKDF2: %s: Failed to allocate materials\n", __func__); // Added
            return 0;
        }
    }
    for (int i = 0; i < pModel3->sizeMaterials; i++)
    {
        if (!stdConffile_ReadLine())
            goto fail;

        if ( _sscanf(stdConffile_g_aLine, " %d: %s", &geoset_num, std_g_genBuffer) != 2 )
            goto fail;

        pModel3->aMaterials[i] = rdMaterial_Load(std_g_genBuffer, 0, 0);

        if ( !pModel3->aMaterials[i] ) {
            rdModel3_HelpDebug("OpenJKDF2: %s: Failed to load material %s\n", __func__, std_g_genBuffer); // Added
            goto fail;
        }
    }

    if (!stdConffile_ReadLine())
        goto fail;

    if ( _sscanf(stdConffile_g_aLine, " section: %s", std_g_genBuffer) != 1 ) {
        rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse section line %s\n", __func__, stdConffile_g_aLine); // Added
        goto fail;
    }

    if (!stdConffile_ReadLine()) {
        rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse section ln %s\n", __func__, stdConffile_g_aLine); // Added
        goto fail;
    }

    if ( _sscanf(stdConffile_g_aLine, " radius %f", &radius) != 1 ) {
        rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse radius %s\n", __func__, stdConffile_g_aLine); // Added
        goto fail;
    }

    pModel3->radius = radius; // FLEXTODO
    if (!stdConffile_ReadLine()) {
        rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse radius ln %s\n", __func__, stdConffile_g_aLine); // Added
        goto fail;
    }

    if ( _sscanf(stdConffile_g_aLine, " insert offset %f %f %f", &v_x, &v_y, &v_z) != 3 ) {
        rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse insert offset %s\n", __func__, stdConffile_g_aLine); // Added
        goto fail;
    }

    pModel3->insertOffset.x = v_x; // FLEXTODO
    pModel3->insertOffset.y = v_y; // FLEXTODO
    pModel3->insertOffset.z = v_z; // FLEXTODO
    if (!stdConffile_ReadLine()) {
        rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse insertOffset ln %s\n", __func__, stdConffile_g_aLine); // Added
        goto fail;
    }

    if ( _sscanf(stdConffile_g_aLine, " geosets %d", &pModel3->numGeos) != 1 ) {
        rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse geosets %s\n", __func__, stdConffile_g_aLine); // Added
        goto fail;
    }
    for (v78 = 0; v78 < pModel3->numGeos; v78++)
    {
        if (!stdConffile_ReadLine()) {
            rdModel3_HelpDebug("OpenJKDF2: %s: Failed to geosets ln %s\n", __func__, stdConffile_g_aLine); // Added
            goto fail;
        }
            
        if ( _sscanf(stdConffile_g_aLine, " geoset %d", &geoset_num) != 1 ) {
            rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse geoset %s\n", __func__, stdConffile_g_aLine); // Added
            goto fail;
        }
            
        if ( !stdConffile_ReadLine() )
            goto fail;
            
        if ( _sscanf(stdConffile_g_aLine, " meshes %d", &pModel3->aGeos[v78].numMeshes) != 1 ) {
            rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse meshes %s\n", __func__, stdConffile_g_aLine); // Added
            goto fail;
        }

        pModel3->aGeos[v78].aMeshes = (rdMesh *)RDROID_ALLOC(sizeof(rdMesh) * pModel3->aGeos[v78].numMeshes);
        if ( !pModel3->aGeos[v78].aMeshes )
            goto fail;
        
        for (int i = 0; i < pModel3->aGeos[v78].numMeshes; i++)
        {
            mesh = &pModel3->aGeos[v78].aMeshes[i];
            mesh->mesh_num = i;
            if ( !stdConffile_ReadLine() )
                goto fail;
            if ( _sscanf(stdConffile_g_aLine, " mesh %d", std_g_genBuffer) != 1 ) {
                rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse mesh %s\n", __func__, stdConffile_g_aLine); // Added
                goto fail;
            }
            if ( !stdConffile_ReadLine() )
                goto fail;
            if ( _sscanf(stdConffile_g_aLine, " name %s", std_g_genBuffer) != 1 ) {
                rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse name %s\n", __func__, stdConffile_g_aLine); // Added
                goto fail;
            }

            stdString_SafeStrCopy(mesh->name, std_g_genBuffer, 32);

            if ( !stdConffile_ReadLine()
              || _sscanf(stdConffile_g_aLine, " radius %f", &radius) != 1
              || !stdConffile_ReadLine()
              || _sscanf(stdConffile_g_aLine, " geometrymode %d", &mesh->geometryMode) != 1
              || !stdConffile_ReadLine()
              || _sscanf(stdConffile_g_aLine, " lightingmode %d", &mesh->lightingMode) != 1
              || !stdConffile_ReadLine()
              || _sscanf(stdConffile_g_aLine, " texturemode %d", &mesh->textureMode) != 1
              || !stdConffile_ReadLine()
              || _sscanf(stdConffile_g_aLine, " vertices %d", &mesh->numVertices) != 1
              || mesh->numVertices > RDMODEL3_MAX_MESH_VERTICES ) // Added (remaster): was 0x200
            {
                rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse radius, render modes, vertices %s\n", __func__, stdConffile_g_aLine); // Added
                goto fail;
            }
            mesh->radius = radius; // FLEXTODO
            
#ifdef STDPLATFORM_HEAP_SUGGESTIONS
            pSithHS->suggestHeap(HEAP_FAST);
#endif

            mesh->aVertices = 0;
            mesh->vertices_i = 0;
            mesh->vertices_unk = 0;
            if ( mesh->numVertices)
            {
                { RDMODEL3_EXTRAM_SUGGEST();
                mesh->aVertices = (rdVector3 *)RDROID_ALLOC(sizeof(rdVector3) * mesh->numVertices);
                RDMODEL3_EXTRAM_RESTORE(); }
                if ( !mesh->aVertices ){
                    rdModel3_HelpDebug("OpenJKDF2: %s: Failed to allocate vertices\n", __func__); // Added
                    goto fail;
                }
                { RDMODEL3_EXTRAM_SUGGEST();
                mesh->vertices_i = (flex_t *)RDROID_ALLOC(sizeof(flex_t) * mesh->numVertices);
                RDMODEL3_EXTRAM_RESTORE(); }
                if ( !mesh->vertices_i ) {
                    rdModel3_HelpDebug("OpenJKDF2: %s: Failed to allocate vertex lights\n", __func__); // Added
                    goto fail;
                }
                { RDMODEL3_EXTRAM_SUGGEST();
                mesh->vertices_unk  = (flex_t *)RDROID_ALLOC(sizeof(flex_t) * mesh->numVertices);
                RDMODEL3_EXTRAM_RESTORE(); }
                if ( !mesh->vertices_unk  ) {
                    rdModel3_HelpDebug("OpenJKDF2: %s: Failed to allocate vertex unk\n", __func__); // Added
                    goto fail;
                }
                stdPlatform_Memzero32(mesh->vertices_unk, mesh->numVertices); // bug? // Added: word-safe
            }
            for (vertex_num = 0; vertex_num < mesh->numVertices; vertex_num++)
            {
                if ( !stdConffile_ReadLine()
                     || _sscanf(
                            stdConffile_g_aLine,
                            " %d: %f %f %f %f",
                            &geoset_num,
                            &v_x,
                            &v_y,
                            &v_z,
                            &v_i) != 5 ) {
                    rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse vertex %s\n", __func__, stdConffile_g_aLine); // Added
                    goto fail;
                }

                mesh->aVertices[vertex_num].x = v_x; // FLEXTODO
                mesh->aVertices[vertex_num].y = v_y; // FLEXTODO
                mesh->aVertices[vertex_num].z = v_z; // FLEXTODO
                mesh->vertices_i[vertex_num] = v_i; // FLEXTODO
            }

            if ( !stdConffile_ReadLine()
              || _sscanf(stdConffile_g_aLine, " texture vertices %d", &mesh->numUVs) != 1
              || mesh->numUVs > RDMODEL3_MAX_MESH_UVS ) // Added (remaster): was 0x300
            {
                rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse texture vertices %s\n", __func__, stdConffile_g_aLine); // Added
                goto fail;
            }
            
            mesh->aTexVerticies = 0;
            if ( mesh->numUVs )
            {
                { RDMODEL3_EXTRAM_SUGGEST();
                mesh->aTexVerticies = (rdVector2 *)RDROID_ALLOC(sizeof(rdVector2) * mesh->numUVs);
                RDMODEL3_EXTRAM_RESTORE(); }
                if ( !mesh->aTexVerticies ) {
                    rdModel3_HelpDebug("OpenJKDF2: %s: Failed to allocate vertex UVs\n", __func__); // Added
                    goto fail;
                }
                for (v25 = 0; v25 < mesh->numUVs; v25++)
                {
                    if ( !stdConffile_ReadLine()
                         || _sscanf(stdConffile_g_aLine, " %d: %f %f", &geoset_num, &v_u, &v_v) != 3 ) {
                            rdModel3_HelpDebug("OpenJKDF2: %s: Failed to parse texture vertex %s\n", __func__, stdConffile_g_aLine); // Added
                            goto fail;
                        }

                        mesh->aTexVerticies[v25].x = v_u;
                        mesh->aTexVerticies[v25].y = v_v;
                }
            }

            if ( !stdConffile_ReadLine() )
                goto fail;
            mesh->vertexNormals = 0;
            if ( mesh->numVertices)
            {
                { RDMODEL3_EXTRAM_SUGGEST();
                mesh->vertexNormals = (rdVector3 *)RDROID_ALLOC(sizeof(rdVector3) * mesh->numVertices);
                RDMODEL3_EXTRAM_RESTORE(); }
                if ( !mesh->vertexNormals ) {
                    rdModel3_HelpDebug("OpenJKDF2: %s: Failed to allocate vertex normals\n", __func__); // Added
                    goto fail;
                }
            }

#ifdef STDPLATFORM_HEAP_SUGGESTIONS
            pSithHS->suggestHeap(HEAP_ANY);
#endif
            for (v29 = 0; v29 < mesh->numVertices; v29++ )
            {
                
                if ( !stdConffile_ReadLine()
                     || _sscanf(
                            stdConffile_g_aLine,
                            " %d: %f %f %f",
                            &geoset_num,
                            &v_x,
                            &v_y,
                            &v_z) != 4 )
                    goto fail;

                vertex_normal = &mesh->vertexNormals[v29];
                vertex_normal->x = v_x; // FLEXTODO
                vertex_normal->y = v_y; // FLEXTODO
                vertex_normal->z = v_z; // FLEXTODO
            }

            if ( !stdConffile_ReadLine()
              || _sscanf(stdConffile_g_aLine, " faces %d", &mesh->numFaces) != 1
              || mesh->numFaces > RDMODEL3_MAX_MESH_FACES ) // Added (remaster): was 0x200
            {
                goto fail;
            }
            mesh->faces = 0;
#ifdef RDMODEL3_POOLED_FACE_INDICES
            int poolUsed = 0, poolCap = 0;   // Added: this mesh's face index pool
            int* pIdxPool = NULL;
#endif
            if ( mesh->numFaces)
            {
                { RDMODEL3_EXTRAM_SUGGEST();
                mesh->faces = (rdFace *)RDROID_ALLOC(sizeof(rdFace) * mesh->numFaces);
                RDMODEL3_EXTRAM_RESTORE(); }
                if ( !mesh->faces ) {
                    rdModel3_HelpDebug("OpenJKDF2: %s: Failed to allocate faces\n", __func__); // Added
                    goto fail;
                }
            }
            
            for (int j = 0; j < mesh->numFaces; j++)
            {
                if (!stdConffile_ReadLine())
                    goto fail;

                face = &mesh->faces[j];
                rdFace_NewEntry(face);
                _strtok(stdConffile_g_aLine, " \t");
                tmpTxt = _strtok(0, " \t");
                v36 = _atoi(tmpTxt);
                face->num = j;
                // Added: model->sizeMaterials bounds
                if (v36 > pModel3->sizeMaterials) {
                    v36 = pModel3->sizeMaterials-1;
                }
                else if (v36 < 0 && v36 != -1) {
                    v36 = 0;
                }
                face->material = (v36 == -1 || !pModel3->sizeMaterials) ? 0 : pModel3->aMaterials[v36]; // Added: model->sizeMaterials check
                rdMaterial_EnsureMetadata(face->material); // Added: we don't need VBuffers yet
                tmpTxt = _strtok(0, " \t");
                if ( _sscanf(tmpTxt, "%x", &face->type) != 1 )
                    goto fail;
                tmpTxt = _strtok(0, " \t");
                if ( _sscanf(tmpTxt, "%d", &face->geometryMode) != 1 )
                    goto fail;
                tmpTxt = _strtok(0, " \t");
                if ( _sscanf(tmpTxt, "%d", &face->lightingMode) != 1 )
                    goto fail;
                tmpTxt = _strtok(0, " \t");
                if ( _sscanf(tmpTxt, "%d", &face->textureMode) != 1 )
                    goto fail;
                tmpTxt = _strtok(0, " \t");
                if ( _sscanf(tmpTxt, "%f", &extralight) != 1 )
                    goto fail;
                face->extraLight = extralight; // FLEXTODO
                to_num_verts = _strtok(0, " \t");
                face->numVertices = _atoi(to_num_verts);
                if ( !face->numVertices )
                    goto fail;
                if ( face->numVertices > 24 )
                    goto fail;
                int bHasUV = (face->material && (face->material->tex_type & 2)) ? 1 : 0; // Added: hoisted
                int* pPosIdx;
                int* pUVIdx = NULL;
#ifdef RDMODEL3_POOLED_FACE_INDICES
                // Added: grab index storage from the mesh pool; the face fields hold
                // offset+1 until the fixup after this loop (the pool moves on growth).
                {
                    int need = face->numVertices * (bHasUV ? 2 : 1);
                    if (poolUsed + need > poolCap) {
                        int newCap = poolCap ? poolCap * 2 : 256;
                        while (newCap < poolUsed + need)
                            newCap *= 2;
                        int* pNewPool = (int*)RDROID_REALLOC(pIdxPool, sizeof(int) * newCap);
                        if (!pNewPool) {
                            rdModel3_HelpDebug("OpenJKDF2: %s: Failed to grow face index pool\n", __func__); // Added
                            goto fail;
                        }
                        pIdxPool = pNewPool;
                        poolCap = newCap;
                        mesh->paFaceIdxPool = pIdxPool; // keep current for the fail path
                    }
                    pPosIdx = pIdxPool + poolUsed;
                    face->vertexPosIdx = (int*)(intptr_t)(poolUsed + 1);
                    poolUsed += face->numVertices;
                    if (bHasUV) {
                        pUVIdx = pIdxPool + poolUsed;
                        face->vertexUVIdx = (int*)(intptr_t)(poolUsed + 1);
                        poolUsed += face->numVertices;
                    }
                }
#else
                face->vertexPosIdx = (int*)RDROID_ALLOC(sizeof(int) * face->numVertices);
                if ( !face->vertexPosIdx ) {
                    rdModel3_HelpDebug("OpenJKDF2: %s: Failed to allocate vertexPosIdx\n", __func__); // Added
                    goto fail;
                }
                pPosIdx = face->vertexPosIdx;
                if ( bHasUV )
                {
                    face->vertexUVIdx = (int*)RDROID_ALLOC(sizeof(int) * face->numVertices);
                    if ( !face->vertexUVIdx ) {
                        rdModel3_HelpDebug("OpenJKDF2: %s: Failed to allocate mesh vertex UVs\n", __func__); // Added
                        goto fail;
                    }
                    pUVIdx = face->vertexUVIdx;
                }
#endif
                if ( bHasUV )
                {
                    for (v49 = 0; v49 < face->numVertices; v49++)
                    {
                        tmpTxt = _strtok(0, " \t,");
                        pPosIdx[v49] = _atoi(tmpTxt);
                        
                        tmpTxt = _strtok(0, " \t,");
                        pUVIdx[v49] = _atoi(tmpTxt);
                    }
                }
                else
                {
                    
                    for (v52 = 0; v52 < face->numVertices; v52++)
                    {
                        tmpTxt = _strtok(0, " \t,");
                        pPosIdx[v52] = _atoi(tmpTxt);
                        _strtok(0, " \t,");
                    }
                }
                rdMaterial_OptionalFree(face->material); // Added
                face++;
            }
#ifdef RDMODEL3_POOLED_FACE_INDICES
            // Added: trim the pool to what was used and resolve offsets to pointers.
            if (pIdxPool && poolUsed && poolUsed < poolCap) {
                int* pTrim = (int*)RDROID_REALLOC(pIdxPool, sizeof(int) * poolUsed);
                if (pTrim)
                    pIdxPool = pTrim;
            }
#ifdef TARGET_TWL
            // Added: relocate the finished pool into extram (TWL realloc cannot
            // migrate heaps, so growth happened in sysram; one word-safe move).
            if (pIdxPool && poolUsed) {
                RDMODEL3_EXTRAM_SUGGEST();
                int* pMoved = (int*)RDROID_ALLOC(sizeof(int) * poolUsed);
                RDMODEL3_EXTRAM_RESTORE();
                if (pMoved) {
                    stdPlatform_Memcpy32(pMoved, pIdxPool, sizeof(int) * poolUsed);
                    RDROID_FREE(pIdxPool);
                    pIdxPool = pMoved;
                }
            }
#endif
            mesh->paFaceIdxPool = pIdxPool;
            for (int j2 = 0; j2 < mesh->numFaces; j2++)
            {
                rdFace* pFixFace = &mesh->faces[j2];
                pFixFace->vertexPosIdx = pIdxPool + ((intptr_t)pFixFace->vertexPosIdx - 1);
                if (pFixFace->vertexUVIdx)
                    pFixFace->vertexUVIdx = pIdxPool + ((intptr_t)pFixFace->vertexUVIdx - 1);
            }
#endif

            if ( !stdConffile_ReadLine() )
                goto fail;
            for (v55 = 0; v55 < mesh->numFaces; v55++)
            {
                if ( !stdConffile_ReadLine()
                     || _sscanf(
                            stdConffile_g_aLine,
                            " %d: %f %f %f",
                            &geoset_num,
                            &v_x,
                            &v_y,
                            &v_z) != 4 )
                    goto fail;


                mesh->faces[v55].normal.x = v_x; // FLEXTODO
                mesh->faces[v55].normal.y = v_y; // FLEXTODO
                mesh->faces[v55].normal.z = v_z; // FLEXTODO
            }
        }
    }

    if (!stdConffile_ReadLine() )
        goto fail;

    if ( _sscanf(stdConffile_g_aLine, " section: %s", std_g_genBuffer) != 1 )
        goto fail;

    if (!stdConffile_ReadLine())
        goto fail;

    if ( _sscanf(stdConffile_g_aLine, " hierarchy nodes %d", &pModel3->numHNodes) != 1 )
        goto fail;

    pModel3->aHierarchyNodes = (rdHierarchyNode *)RDROID_ALLOC(sizeof(rdHierarchyNode) * pModel3->numHNodes);
    if (!pModel3->aHierarchyNodes) {
        rdModel3_HelpDebug("OpenJKDF2: %s: Failed to allocate hierarchyNodes\n", __func__); // Added
        goto fail;
    }

    for (idx = 0; idx < pModel3->numHNodes; idx++)
    {
        node = &pModel3->aHierarchyNodes[idx];
        node->idx = idx;
        if ( !stdConffile_ReadLine()
          || _sscanf(
                 stdConffile_g_aLine,
                 " %d: %x %x %d %d %d %d %d %f %f %f %f %f %f %f %f %f %s",
                 &geoset_num,
                 &node->flags,
                 &node->type,
                 &node->meshIdx,
                 &parent,
                 &child,
                 &sibling,
                 &node->numChildren,
                 &v_x,
                 &v_y,
                 &v_z,
                 &pitch,
                 &yaw,
                 &roll,
                 &pivot_x,
                 &pivot_y,
                 &pivot_z,
                 node) != 18 )
        {
            goto fail;
        }
        
        if ( parent == -1 )
        {
            node->parent = 0;
        }
        else
        {
            node->parent = &pModel3->aHierarchyNodes[parent];
        }

        if ( child == -1 )
            node->child = 0;
        else
            node->child = &pModel3->aHierarchyNodes[child];

        if ( sibling == -1 )
            node->nextSibling = 0;
        else
            node->nextSibling = &pModel3->aHierarchyNodes[sibling];

        node->pos.x = v_x; // FLEXTODO
        node->pos.y = v_y; // FLEXTODO
        node->pos.z = v_z; // FLEXTODO
        node->rot.x = pitch; // FLEXTODO
        node->rot.y = yaw; // FLEXTODO
        node->rot.z = roll; // FLEXTODO
        node->pivot.x = pivot_x; // FLEXTODO
        node->pivot.y = pivot_y; // FLEXTODO
        node->pivot.z = pivot_z; // FLEXTODO
    }

    rdModel3_CalcNumParents(pModel3); // MOTS added

    stdConffile_Close();
    rdModel3_LoadSkin(pFilename, pModel3); // Added (remaster): optional <model>.skn
    return 1;

fail:
#ifdef STDPLATFORM_HEAP_SUGGESTIONS
    pSithHS->suggestHeap(HEAP_ANY);
#endif
    stdConffile_Close();
    return 0;
}

// from editor?
void rdModel3_LoadPostProcess(rdModel3 *pModel3)
{
    rdModel3_CalcRadii(pModel3);
    rdModel3_CalcFaceNormals(pModel3);
    rdModel3_CalcVertexNormals(pModel3);
    RD_ASSERTREL(rdModel3_Validate(pModel3)); // Added: J3D assert
    rdModel3_CalcNumParents(pModel3); // MOTS added
}

// MOTS added
void rdModel3_CalcNumParents(rdModel3* pModel)
{
#ifdef JKM_BONES
    for (int nodeNum = 0; nodeNum < pModel->numHNodes; nodeNum++ )
    {
        rdHierarchyNode* node = &pModel->aHierarchyNodes[nodeNum];

        node->numParents = 0;
        rdHierarchyNode* parent = node->parent;
        while (parent)
        {
            parent = parent->parent;
            node->numParents++;
        }

    }
#endif
}

// from editors?
int rdModel3_Write(char *pFilename, rdModel3 *pModel, char *pCratedName)
{
    rdGeoset* geoset;
    int siblingIdx;
    int parentIdx;
    int childIdx;
    stdFile_t fd;

    fd = rdroid_g_pHS->fileOpen(pFilename, "wt+");
    if (!fd)
        return 0;

    rdroid_g_pHS->filePrintf(fd, "# MODEL '%s' created from '%s'\n\n", pModel->filename, pCratedName);
    rdroid_g_pHS->filePrintf(fd, "###############\n");
    rdroid_g_pHS->filePrintf(fd, "SECTION: HEADER\n\n");
    rdroid_g_pHS->filePrintf(fd, "3DO %d.%d\n\n", 2, 1);
    rdroid_g_pHS->filePrintf(fd, "###############\n");
    rdroid_g_pHS->filePrintf(fd, "SECTION: MODELRESOURCE\n\n");
    rdroid_g_pHS->filePrintf(fd, "# Materials list\n");
    rdroid_g_pHS->filePrintf(fd, "MATERIALS %d\n\n", pModel->sizeMaterials);
    for (int i = 0; i < pModel->sizeMaterials; i++)
    {
            rdroid_g_pHS->filePrintf(fd, "%10d:%15s\n", i, pModel->aMaterials[i]->mat_fpath);
    }
    rdroid_g_pHS->filePrintf(fd, "\n\n");
    rdroid_g_pHS->filePrintf(fd, "###############\n");
    rdroid_g_pHS->filePrintf(fd, "SECTION: GEOMETRYDEF\n\n");
    rdroid_g_pHS->filePrintf(fd, "# Object radius\n");
    rdroid_g_pHS->filePrintf(fd, "RADIUS %10.6f\n\n", pModel->radius);
    rdroid_g_pHS->filePrintf(fd, "# Insertion offset\n");
    rdroid_g_pHS->filePrintf(fd, "INSERT OFFSET %10.6f %10.6f %10.6f\n\n", pModel->insertOffset.x, pModel->insertOffset.y, pModel->insertOffset.z);
    rdroid_g_pHS->filePrintf(fd, "# Number of Geometry Sets\n");
    rdroid_g_pHS->filePrintf(fd, "GEOSETS %d\n\n", pModel->numGeos);

    geoset = pModel->aGeos;
    for (int geosetNum = 0; geosetNum < pModel->numGeos; geosetNum++)
    {
        rdroid_g_pHS->filePrintf(fd, "# Geometry Set definition\n");
        rdroid_g_pHS->filePrintf(fd, "GEOSET %d\n\n", geosetNum);
        rdroid_g_pHS->filePrintf(fd, "# Number of Meshes\n");
        rdroid_g_pHS->filePrintf(fd, "MESHES %d\n\n\n", geoset->numMeshes);

        for (int meshNum = 0; meshNum < geoset->numMeshes; meshNum++)
        {
            rdroid_g_pHS->filePrintf(fd, "# Mesh definition\n");
            rdroid_g_pHS->filePrintf(fd, "MESH %d\n\n", meshNum);
            rdroid_g_pHS->filePrintf(fd, "NAME %s\n\n", geoset->aMeshes[meshNum].name);
            rdroid_g_pHS->filePrintf(fd, "RADIUS %10.6f\n\n", geoset->aMeshes[meshNum].radius);
            rdroid_g_pHS->filePrintf(fd, "GEOMETRYMODE\t%d\n", geoset->aMeshes[meshNum].geometryMode);
            rdroid_g_pHS->filePrintf(fd, "LIGHTINGMODE\t%d\n", geoset->aMeshes[meshNum].lightingMode);
            rdroid_g_pHS->filePrintf(fd, "TEXTUREMODE\t%d\n", geoset->aMeshes[meshNum].textureMode);
            rdroid_g_pHS->filePrintf(fd, "\n\n");
            rdroid_g_pHS->filePrintf(fd, "VERTICES %d\n\n", geoset->aMeshes[meshNum].numVertices);
            rdroid_g_pHS->filePrintf(fd, "# num:     x:         y:         z:         i: \n");
            for (int vertexNum = 0; vertexNum < geoset->aMeshes[meshNum].numVertices; vertexNum++)
            {
                rdVector3* vertex = &geoset->aMeshes[meshNum].aVertices[vertexNum];
                rdroid_g_pHS->filePrintf(
                    fd,
                    "  %3d: %10.6f %10.6f %10.6f %10.6f\n",
                    vertexNum,
                    vertex->x,
                    vertex->y,
                    vertex->z,
                    geoset->aMeshes[meshNum].vertices_i[vertexNum]);
            }
            rdroid_g_pHS->filePrintf(fd, "\n\n");
            rdroid_g_pHS->filePrintf(fd, "TEXTURE VERTICES %d\n\n", geoset->aMeshes[meshNum].numVertices);
            for (int vertexNum = 0; vertexNum < geoset->aMeshes[meshNum].numUVs; vertexNum++)
            {
                rdVector2* uv = &geoset->aMeshes[meshNum].aTexVerticies[vertexNum];
                rdroid_g_pHS->filePrintf(fd, "  %3d: %10.6f %10.6f\n", vertexNum, uv->x, uv->y);
            }
            rdroid_g_pHS->filePrintf(fd, "\n\n");
            rdroid_g_pHS->filePrintf(fd, "VERTEX NORMALS\n\n");
            rdroid_g_pHS->filePrintf(fd, "# num:     x:         y:         z:\n");
            for (int vertexNum = 0; vertexNum < geoset->aMeshes[meshNum].numVertices; vertexNum++)
            {
                rdVector3* norm = &geoset->aMeshes[meshNum].vertexNormals[vertexNum];
                rdroid_g_pHS->filePrintf(
                    fd,
                    "  %3d: %10.6f %10.6f %10.6f\n",
                    vertexNum,
                    norm->x,
                    norm->y,
                    norm->z);
            }
            rdroid_g_pHS->filePrintf(fd, "\n\n");
            rdroid_g_pHS->filePrintf(fd, "FACES %d\n\n", geoset->aMeshes[meshNum].numFaces);
            rdroid_g_pHS->filePrintf(fd, "#  num:  material:   type:  geo:  light:   tex:  extralight:  verts:\n");
            rdFace* face = geoset->aMeshes[meshNum].faces;
            for (int faceNum = 0; faceNum < geoset->aMeshes[meshNum].numFaces; faceNum++)
            {
                int materialIdx = -1;
                for (int j = 0; j < pModel->sizeMaterials; j++)
                {
                    if (!face->material)
                        break;

                    if ( face->material == pModel->aMaterials[j] )
                        materialIdx = j;
                }

                if ( face->material )
                    RD_ASSERTREL(materialIdx != -1); // Added: J3D assert

                rdroid_g_pHS->filePrintf(
                    fd,
                    "   %3d: %9d  0x%04x  %4d %7d %6d %12.4f %7d  ",
                    faceNum,
                    materialIdx,
                    face->type,
                    face->geometryMode,
                    face->lightingMode,
                    face->textureMode,
                    face->extraLight,
                    face->numVertices);

                if (face->material && face->material->tex_type & 2)
                {
                    for (int j = 0; j < face->numVertices; j++)
                    {
                        rdroid_g_pHS->filePrintf(
                            fd,
                            "%3d,%3d ",
                            face->vertexPosIdx[j],
                            face->vertexUVIdx[j]);
                    }
                }
                else
                {
                    for (int j = 0; j < face->numVertices; j++)
                    {
                        rdroid_g_pHS->filePrintf(fd, "%3d, 0 ", face->vertexPosIdx[j]);
                    }
                }
                rdroid_g_pHS->filePrintf(fd, "\n");
            }
            rdroid_g_pHS->filePrintf(fd, "\n\n");
            rdroid_g_pHS->filePrintf(fd, "FACE NORMALS\n\n");
            rdroid_g_pHS->filePrintf(fd, "# num:     x:         y:         z:\n");
            for (int j = 0; j < geoset->aMeshes[meshNum].numFaces; j++)
            {
                rdVector3* norm = &geoset->aMeshes[meshNum].faces[j].normal;
                rdroid_g_pHS->filePrintf(fd, "  %3d: %10.6f %10.6f %10.6f\n", j, norm->x, norm->y, norm->z);
            }
            rdroid_g_pHS->filePrintf(fd, "\n\n");
        }
        ++geoset;
    }
    rdroid_g_pHS->filePrintf(fd, "###############\n");
    rdroid_g_pHS->filePrintf(fd, "SECTION: HIERARCHYDEF\n\n");
    rdroid_g_pHS->filePrintf(fd, "# Hierarchy node list\n");
    rdroid_g_pHS->filePrintf(fd, "HIERARCHY NODES %d\n\n", pModel->numHNodes);
    rdroid_g_pHS->filePrintf(fd, 
        "#  num:   flags:   type:    mesh:  parent:  child:  sibling:  numChildren:        x:         y:         z:     pitch:       yaw:      roll:    pivot"
        "x:    pivoty:    pivotz:  hnodename:\n");

    for (int nodeNum = 0; nodeNum < pModel->numHNodes; nodeNum++ )
    {
        rdHierarchyNode* node = &pModel->aHierarchyNodes[nodeNum];

        if ( node->parent )
            parentIdx = node->parent->idx;
        else
            parentIdx = -1;

        if ( node->child )
            childIdx = node->child->idx;
        else
            childIdx = -1;

        if ( node->nextSibling )
            siblingIdx = node->nextSibling->idx;
        else
            siblingIdx = -1;

        rdroid_g_pHS->filePrintf(
            fd,
            "   %3d:  0x%04x 0x%05X %8d %8d %7d %9d %13d %10.6f %10.6f %10.6f %10.6f %10.6f %10.6f %10.6f %10.6f %10.6f  %s\n",
            nodeNum,
            node->flags,
            node->type,
            node->meshIdx,
            parentIdx,
            childIdx,
            siblingIdx,
            node->numChildren,
            node->pos.x,
            node->pos.y,
            node->pos.z,
            node->rot.x,
            node->rot.y,
            node->rot.z,
            node->pivot.x,
            node->pivot.y,
            node->pivot.z,
            node->name);
    }
    rdroid_g_pHS->fileClose(fd);
    return 1;
}

void rdModel3_Free(rdModel3 *pModel3)
{
    if ( pModel3 )
    {
        if ( pModel3Unloader )
        {
            pModel3Unloader(pModel3);
        }
        else
        {
            rdModel3_FreeEntry(pModel3);
            RDROID_FREE(pModel3);
        }
    }
}

void rdModel3_FreeEntry(rdModel3 *pModel3)
{
    if (!pModel3)
        return;

    rdGeoset* geoset = pModel3->aGeos;
    for (int geosetNum = 0; geosetNum < pModel3->numGeos; geosetNum++ )
    {
        for (int meshNum = 0; meshNum < geoset->numMeshes; meshNum++)
        {
            rdMesh* mesh = &geoset->aMeshes[meshNum];
            
            if (mesh->aVertices)
                RDROID_FREE(mesh->aVertices);
            
            if (mesh->aTexVerticies)
                RDROID_FREE(mesh->aTexVerticies);
            
            if ( mesh->faces )
            {
#ifdef RDMODEL3_POOLED_FACE_INDICES
                // Added: pooled index storage frees as one block (per-face pointers
                // alias into it; a partially-parsed mesh may hold offsets instead)
                if (mesh->paFaceIdxPool) {
                    RDROID_FREE(mesh->paFaceIdxPool);
                    mesh->paFaceIdxPool = NULL;
                } else
#endif
                {
                    for (int faceIdx = 0; faceIdx < mesh->numFaces; faceIdx++)
                    {
                        rdFace_FreeEntry(&mesh->faces[faceIdx]);
                    }
                }
                RDROID_FREE(mesh->faces);
            }
            if (mesh->vertices_i)
                RDROID_FREE(mesh->vertices_i);
            if (mesh->vertices_unk)
                RDROID_FREE(mesh->vertices_unk);
            if (mesh->vertexNormals)
                RDROID_FREE(mesh->vertexNormals);
            rdModel3_FreeSkin(mesh); // Added (remaster)
        }
        if ( geoset->aMeshes )
            RDROID_FREE(geoset->aMeshes);
        ++geoset;
    }

    if ( pModel3->aHierarchyNodes )
        RDROID_FREE(pModel3->aHierarchyNodes);

    if ( pModel3->sizeMaterials )
    {
        for (int i = 0; i < pModel3->sizeMaterials; i++)
        {
            rdMaterial_Free(pModel3->aMaterials[i]);
        }
    }
    if (pModel3->aMaterials )
        RDROID_FREE(pModel3->aMaterials);
}

void rdModel3_FreeEntryGeometryOnly(rdModel3 *pModel3)
{
    if (!pModel3)
        return;

    rdGeoset* geoset = pModel3->aGeos;
    for (int geosetNum = 0; geosetNum < pModel3->numGeos; geosetNum++ )
    {
        for (int meshNum = 0; meshNum < geoset->numMeshes; meshNum++)
        {
            rdMesh* mesh = &geoset->aMeshes[meshNum];
            
            if (mesh->aVertices)
                RDROID_FREE(mesh->aVertices);
            
            if (mesh->aTexVerticies)
                RDROID_FREE(mesh->aTexVerticies);
            
            if ( mesh->faces )
            {
#ifdef RDMODEL3_POOLED_FACE_INDICES
                // Added: pooled index storage frees as one block (per-face pointers
                // alias into it; a partially-parsed mesh may hold offsets instead)
                if (mesh->paFaceIdxPool) {
                    RDROID_FREE(mesh->paFaceIdxPool);
                    mesh->paFaceIdxPool = NULL;
                } else
#endif
                {
                    for (int faceIdx = 0; faceIdx < mesh->numFaces; faceIdx++)
                    {
                        rdFace_FreeEntry(&mesh->faces[faceIdx]);
                    }
                }
                RDROID_FREE(mesh->faces);
            }
            if (mesh->vertices_i)
                RDROID_FREE(mesh->vertices_i);
            if (mesh->vertices_unk)
                RDROID_FREE(mesh->vertices_unk);
            if (mesh->vertexNormals)
                RDROID_FREE(mesh->vertexNormals);
        }
        if ( geoset->aMeshes )
            RDROID_FREE(geoset->aMeshes);
        ++geoset;
    }

    if ( pModel3->aHierarchyNodes )
        RDROID_FREE(pModel3->aHierarchyNodes);

    if (pModel3->aMaterials )
        RDROID_FREE(pModel3->aMaterials);
}

#if 0
int __cdecl rdModel3_Validate(rdModel3 *pModel3)
{
    int result; // eax
    unsigned int v2; // edx
    rdGeoset *v3; // ecx
    int geoset; // ebp
    unsigned int v5; // ebx
    int *v6; // edi
    unsigned int v7; // edx
    void *v8; // eax
    _BYTE *v9; // ecx
    unsigned int v10; // [esp+10h] [ebp-8h]
    unsigned int v11; // [esp+14h] [ebp-4h]
    rdGeoset *modela; // [esp+1Ch] [ebp+4h]

    result = (int)pModel3;
    v2 = pModel3->numGeos;
    v3 = pModel3->aGeos;
    modela = pModel3->aGeos;
    v10 = 0;
    v11 = v2;
    if ( v2 )
    {
        do
        {
            geoset = v3->numMeshes;
            v5 = 0;
            if ( v3->numMeshes )
            {
                v6 = &v3->aMeshes->numFaces;
                do
                {
                    v7 = 0;
                    if ( *v6 )
                    {
                        v8 = (void *)(*(v6 - 4) + 8);// faces+8
                        do
                        {
                            v9 = (_BYTE *)*((_DWORD *)v8 + 6);
                            if ( v9 )
                            {
                                if ( !(*v9 & 2) && *(_DWORD *)v8 > 3 )
                                    return 0;
                            }
                            else if ( *(_DWORD *)v8 > 0 )
                            {
                                return 0;
                            }
                            v8 = (char *)v8 + 64;
                            ++v7;
                        }
                        while ( v7 < *v6 );
                    }
                    v6 += 28;
                    ++v5;
                }
                while ( v5 < geoset );
                v3 = modela;
                v2 = v11;
            }
            ++v3;
            result = v10 + 1;
            modela = v3;
            ++v10;
        }
        while ( v10 < v2 );
    }
    return result;
}
#endif

rdModel3* rdModel3_Validate(rdModel3 *model)
{
    return model;
}

void rdModel3_CalcRadii(rdModel3 *pModel3)
{
    flex_t maxDist;

    for (int i = 0; i < pModel3->aGeos[0].numMeshes; i++)
    {
        rdMesh* mesh = &pModel3->aGeos[0].aMeshes[i];
        maxDist = 0.0;
        for (int j = 0; j < mesh->numVertices; j++)
        {
            rdVector3* vtx = &mesh->aVertices[j];
            flex_t dist = rdVector_Len3(vtx);
            if ( dist > maxDist )
            {
                maxDist = dist;
            }
        }
        mesh->field_64 = (maxDist * 0.1) + maxDist;
    }
    rdModel3_fRadius = 0.0;
    rdModel3_BuildExpandedRadius(pModel3, pModel3->aHierarchyNodes, &rdroid_identMatrix34);
    pModel3->radius = rdModel3_fRadius * 0.1 + rdModel3_fRadius;
}

void rdModel3_BuildExpandedRadius(rdModel3 *pModel, rdHierarchyNode *pNode, const rdMatrix34 *orient)
{
    rdVector3 vertex_out;
    rdVector3 vecTmp;
    rdMatrix34 matPivotTranslate;
    rdMatrix34 out;
    rdMatrix34 matTmp;

    rdMatrix_Build34(&matTmp, &pNode->rot, &pNode->pos);
    rdMatrix_BuildTranslate34(&matPivotTranslate, &pNode->pivot);
    rdMatrix_PostMultiply34(&matPivotTranslate, &matTmp);
    
    if (pNode->parent)
    {
        rdVector_Neg3(&vecTmp, &pNode->parent->pivot);
        rdMatrix_PostTranslate34(&matPivotTranslate, &vecTmp);
    }
    
    rdMatrix_Multiply34(&out, orient, &matPivotTranslate);
    
    if ( pNode->meshIdx != -1 )
    {
        rdMesh* mesh = &pModel->aGeos[0].aMeshes[pNode->meshIdx];
        for (int i = 0; i < mesh->numVertices; i++)
        {
            rdVector3* vtx = &mesh->aVertices[i];
            rdMatrix_TransformPoint34(&vertex_out, vtx, &out);
            flex_t dist = rdVector_Len3(&vertex_out);
            if ( dist > rdModel3_fRadius )
                rdModel3_fRadius = dist;
        }
    }
    
    if (pNode->numChildren)
    {
        rdHierarchyNode* childIter = pNode->child;
        for (int i = 0; i < pNode->numChildren; i++)
        {
            rdModel3_BuildExpandedRadius(pModel, childIter, &out);
            childIter = childIter->nextSibling;
        }
    }
}

// from editors?
void rdModel3_CalcFaceNormals(rdModel3 *pModel3)
{
    for (int geosetIdx = 0; geosetIdx < pModel3->numGeos; geosetIdx++)
    {
        rdGeoset* geoset = &pModel3->aGeos[geosetIdx];

        for (int meshIdx = 0; meshIdx < geoset->numMeshes; meshIdx++)
        {
            rdMesh* mesh = &geoset->aMeshes[meshIdx];
            for (int faceIdx = 0; faceIdx < mesh->numFaces; faceIdx++)
            {
                int idx1, idx2, idx3;
                rdFace* face = &mesh->faces[faceIdx];
                RD_ASSERTREL(face->numVertices > 2); // Added: J3D assert
                for (idx1 = 0; idx1 < face->numVertices; idx1++)
                {
                    idx2 = idx1 - 1;
                    if ( idx1 - 1 < 0 )
                        idx2 = face->numVertices - 1;
                    idx3 = ((idx1 + 1) % face->numVertices);
                    if ( !rdMath_PointsCollinear(
                              &mesh->aVertices[face->vertexPosIdx[idx1]],
                              &mesh->aVertices[face->vertexPosIdx[idx3]],
                              &mesh->aVertices[face->vertexPosIdx[idx2]]))
                        break;
                }
                if ( idx1 < face->numVertices )
                    rdMath_CalcSurfaceNormal(
                        &face->normal,
                        &mesh->aVertices[face->vertexPosIdx[idx1]],
                        &mesh->aVertices[face->vertexPosIdx[idx3]],
                        &mesh->aVertices[face->vertexPosIdx[idx2]]);
            }
        }
    } 
}

void rdModel3_CalcVertexNormals(rdModel3 *pModel)
{
    flex_d_t v10; // st7
    flex_d_t v11; // st6
    flex_d_t v12; // st5
    flex_d_t v13; // st4
    unsigned int v15; // eax
    rdVector3 *v19; // ecx
    int v22; // edx

    for (int geosetNum = 0; geosetNum < pModel->numGeos; geosetNum++)
    {
        rdGeoset* geoset = &pModel->aGeos[geosetNum];
        for (int meshNum = 0; meshNum < geoset->numMeshes; meshNum++)
        {
            rdMesh* mesh = &geoset->aMeshes[meshNum];

            for (int vtxNum = 0; vtxNum < mesh->numVertices; vtxNum++)
            {
                rdFace* faceRoot = mesh->faces;
                v10 = 0.0;
                v11 = 0.0;
                v12 = 0.0;
                v13 = 0.0;
                for (int faceNum = 0; faceNum < mesh->numFaces; faceNum++)
                {
                    rdFace* face = &mesh->faces[faceNum];
                    v15 = 0;
                    for (int i = 0; i < face->numVertices; i++)
                    {
                        if ( face->vertexPosIdx[v15] != vtxNum )
                        {
                            ++v15;
                            if ( v15 >= face->numVertices )
                                break;
                            continue;
                        }
                        v10 = v10 + face->normal.x;
                        v11 = v11 + face->normal.y;
                        v12 = v12 + face->normal.z;
                        v13 = v13 - -1.0;
                    }
                }
                if ( v13 == 0.0 )
                {
                    mesh->vertexNormals[vtxNum].x = 1.0;
                    mesh->vertexNormals[vtxNum].y = 0.0;
                    mesh->vertexNormals[vtxNum].z = 0.0;
                    RDLOG_ERROR("Warning: Unused vertex found while calculating vert normals.\n"); // Added: J3D log
                }
                else
                {
                    if ( v13 == 1.0 )
                    {
                        v19 = &mesh->aVertices[faceRoot->vertexPosIdx[faceRoot->numVertices - 1]];
                        v22 = faceRoot->vertexPosIdx[1 % faceRoot->numVertices];
                        mesh->vertexNormals[vtxNum].y = (mesh->aVertices->y - mesh->aVertices[v22].y) + (mesh->aVertices->y - v19->y);
                        mesh->vertexNormals[vtxNum].z = (mesh->aVertices->z - mesh->aVertices[v22].z) + (mesh->aVertices->z - v19->z);
                        mesh->vertexNormals[vtxNum].x = (mesh->aVertices->x - mesh->aVertices[v22].x) + (mesh->aVertices->x - v19->x);
                    }
                    else
                    {
                        mesh->vertexNormals[vtxNum].x = v10 / v13;
                        mesh->vertexNormals[vtxNum].y = v11 / v13;
                        mesh->vertexNormals[vtxNum].z = v12 / v13;
                    }
                    rdVector_Normalize3Acc(&mesh->vertexNormals[vtxNum]);
                }
            }
        }
    }
}

//vertexnormals, technically unused, from editors?

rdHierarchyNode* rdModel3_FindNamedNode(char *pName, rdModel3 *pModel3)
{
    uint32_t i = 0;
    rdHierarchyNode* nodeIter = pModel3->aHierarchyNodes;

    if ( !pModel3->numHNodes )
        return 0;

    while (_strcmp(nodeIter->name, pName))
    {
        ++nodeIter;
        if ( ++i >= pModel3->numHNodes )
            return 0;
    }

    return nodeIter;
}

int rdModel3_GetMeshMatrix(rdThing *pThing, rdMatrix34 *orient, uint32_t nodeNum, rdMatrix34 *meshOrient)
{
    RD_ASSERTREL(pThing && meshOrient); // Added: J3D assert
    RD_ASSERTREL(pThing->type == RD_THING_MODEL3); // Added: J3D assert
    RD_ASSERTREL(pThing->model3); // Added: J3D assert
    if ( nodeNum >= pThing->model3->numHNodes )
        return 0;

    if ( pThing->rdFrameNum != rdroid_frameTrue )
        rdPuppet_BuildJointMatrices(pThing, orient);

    _memcpy(meshOrient, &pThing->paJointMatrices[nodeNum], sizeof(rdMatrix34));
    return 1;
}

int rdModel3_ReplaceMesh(rdModel3 *pModel, int geosetNum, int meshNum, rdMesh *pSrcMesh)
{
    _memcpy(&pModel->aGeos[geosetNum].aMeshes[meshNum], pSrcMesh, sizeof(pModel->aGeos[geosetNum].aMeshes[meshNum]));
    return 1;
}

// MOTS altered (RGB aLights)
int rdModel3_Draw(rdThing *pThing, rdMatrix34 *pPlacement)
{
    int frustumCull;
    int geosetNum;
    rdGeoset *geoset;
    rdLight **pGeoLight;
    rdLight **lightIter;
    rdHierarchyNode *rootNode;
    int meshIdx;
    rdHierarchyNode *node;
    
    RD_ASSERTREL(pThing != NULL); // Added: J3D assert
    RD_ASSERTREL(pThing->model3 != NULL); // Added: J3D assert

    pCurThing = pThing;
    pCurModel3 = pThing->model3;

    if (rdroid_curCullFlags & 2) {
        rdVector3 vertex_out;
        rdClipFrustum* pThingFrustum = rdCamera_g_pCurCamera->pClipFrustum;

        // Moved this in here, it's not used elsewhere
        rdMatrix_TransformPoint34(&vertex_out, &pPlacement->scale, &rdCamera_g_pCurCamera->orient);
        frustumCull = rdClip_SphereInFrustrum(pThingFrustum, &vertex_out, pCurModel3->radius);
#ifdef SITHRENDER_SPHERE_TEST_SURFACES
        extern rdClipFrustum sithRender_absoluteMaxFrustum;

        if (frustumCull == SPHERE_CLIPPING_EDGE) {
            frustumCull = rdClip_SphereInFrustrum(&sithRender_absoluteMaxFrustum, &vertex_out, pCurModel3->radius);
        }
#endif
    }
    else {
        frustumCull = pThing->clippingIdk;
    }
    thingFrustumCull = frustumCull;
    if (frustumCull == SPHERE_FULLY_OUTSIDE) {
        return 0;
    }

    // LEC HACK: Amputated joints can't do frustum culling?
#ifndef TARGET_TWL
    thingFrustumCull = SPHERE_CLIPPING_EDGE;
#endif

    if ( pThing->geosetSelect == -1 )
    {
        geosetNum = pCurModel3->geosetSelect;
    }
    else
    {
        geosetNum = pCurThing->geosetSelect;
    }
    geoset = &pCurModel3->aGeos[geosetNum];
    rdModel3_pCurGeoset = geoset;
    if ( pCurThing->rdFrameNum != rdroid_frameTrue )
    {
        rdPuppet_BuildJointMatrices(pCurThing, pPlacement);
    }

    curGeometryMode = pCurThing->curGeoMode;
    if ( curGeometryMode >= rdroid_g_curGeometryMode )
        curGeometryMode = rdroid_g_curGeometryMode;

    if ((rdroid_g_curRenderOptions & 2) && rdCamera_g_pCurCamera->ambientLight >= 1.0 )
    {
        curLightingMode = RD_LIGHTMODE_FULLYLIT;
    }
    else
    {
        curLightingMode = pCurThing->curLightMode;
        if ( curLightingMode >= rdroid_g_curLightingMode )
            curLightingMode = rdroid_g_curLightingMode;
    }

    curTextureMode = pCurThing->curTexMode;
    if ( curTextureMode >= rdroid_curTextureMode )
        curTextureMode = rdroid_curTextureMode;

    if ( curLightingMode > RD_LIGHTMODE_NOTLIT)
    {
        rdModel3_numGeoLights = 0;
        pGeoLight = apGeoLights;
        // Added (remaster): with per-pixel lighting the shader adds dynamic lights instead.
        int numCameraLights = jkPlayer_bPerPixelLighting ? 0 : rdCamera_g_pCurCamera->numLights;
        for (int lNum = 0; lNum < numCameraLights; lNum++)
        {
            rdVector3* lightPos = &rdCamera_g_pCurCamera->aLightPositions[lNum];
            rdLight* lightIter = rdCamera_g_pCurCamera->aLights[lNum];

            if ( lightIter->minRadius + pCurModel3->radius > rdVector_Dist3(lightPos, &pPlacement->scale))
            {
                *pGeoLight = lightIter;
                ++pGeoLight;
                ++rdModel3_numGeoLights;
            }
        }
    }
    
    // JKDF2 inlined
    rdModel3_DrawHNode(pCurModel3->aHierarchyNodes);
#if 0
    rdDebug_DrawBoundingBox(pPlacement, pCurModel3->radius, 0xFF0000FF);
#endif
    ++rdModel3_numDrawnModels;
    return 1;
}

// MOTS altered (RGB aLights)
void rdModel3_DrawHNode(rdHierarchyNode *pNode)
{
    rdHierarchyNode *iter;

    if ( pNode->meshIdx != -1 ) {

        // MOTS added:
        if (pNode->flags & 2) {
            rdHierarchyNode* pParent = pNode->parent;
            while (pParent && pParent->flags & 2) {  // Added: nullptr check
                pParent = pParent->parent;
            }
            rdModel3_pCurGeoset->aMeshes[pNode->meshIdx].lightingMode = RD_LIGHTMODE_6_UNK;
            if (pParent) // Added: nullptr check
                rdModel3_pCurGeoset->aMeshes[pNode->meshIdx].radius = rdModel3_pCurGeoset->aMeshes[pParent->meshIdx].radius;
        }

#ifdef TARGET_TWL
        // Added: HACK: Force enemy weapons to not have textures
        int geoMode = curGeometryMode;
        if (!strcmp(pNode->name, "weapon")) {
            curGeometryMode = RD_GEOMETRY_SOLID;
        }
#endif
        rdModel3_DrawMesh(&rdModel3_pCurGeoset->aMeshes[pNode->meshIdx], &pCurThing->paJointMatrices[pNode->idx]);

#ifdef TARGET_TWL
        curGeometryMode = geoMode;
#endif
    }

    iter = pNode->child;
    for (int i = 0; i < pNode->numChildren; i++)
    {
        if ( !pCurThing->paJointAmputationFlags[iter->idx] )
            rdModel3_DrawHNode(iter);
        iter = iter->nextSibling;
    }
}

// MOTS altered (RGB aLights)
void rdModel3_DrawMesh(rdMesh *pMesh, rdMatrix34 *orient)
{
    rdLight **pGeoLight;
    rdVector3 vertex;
    rdMatrix34 matInv;
    rdMatrix34 out;

    RD_ASSERTREL(rdCamera_g_pCurCamera != NULL); // Added: J3D assert
    RD_ASSERTREL(pMesh != NULL); // Added: J3D assert

    pCurMesh = pMesh;
    if ( !pMesh->geometryMode )
        return;
    
    if (thingFrustumCull != SPHERE_FULLY_INSIDE) {
        rdVector3 vertex_out;
        rdClipFrustum* pMeshFrustum = rdCamera_g_pCurCamera->pClipFrustum;

        // Moved this in here, it's not used elsewhere
        rdMatrix_TransformPoint34(&vertex_out, &orient->scale, &rdCamera_g_pCurCamera->orient);
        meshFrustumCull = (rdroid_curCullFlags & 1) ? rdClip_SphereInFrustrum(pMeshFrustum, &vertex_out, pCurMesh->radius) : SPHERE_CLIPPING_EDGE;
#ifdef SITHRENDER_SPHERE_TEST_SURFACES
        extern rdClipFrustum sithRender_absoluteMaxFrustum;

        if (meshFrustumCull == SPHERE_CLIPPING_EDGE) {
            meshFrustumCull = rdClip_SphereInFrustrum(&sithRender_absoluteMaxFrustum, &vertex_out, pCurMesh->radius);
        }
#endif
    }
    else {
        meshFrustumCull = SPHERE_FULLY_INSIDE;
    }

    if (meshFrustumCull == SPHERE_FULLY_OUTSIDE) {
        return;
    }

    if (pMesh->pSkin && pCurThing && pCurThing->paJointMatrices) // Added (remaster): bend skinned meshes
        rdModel3_SkinBegin(pMesh, orient);
    rdMatrix_Multiply34(&out, &rdCamera_g_pCurCamera->orient, orient);
    rdMatrix_TransformPointList34(&out, pCurMesh->aVertices, aView, pCurMesh->numVertices);
    rdMatrix_InvertOrtho34(&matInv, orient);
    
    rdModel3_geometryMode = pCurMesh->geometryMode;
    if ( rdModel3_geometryMode >= curGeometryMode )
        rdModel3_geometryMode = curGeometryMode;

    rdModel3_lightingMode = pCurMesh->lightingMode;
    if (rdModel3_lightingMode == RD_LIGHTMODE_6_UNK) // MOTS added
    {
        rdModel3_lightingMode = RD_LIGHTMODE_6_UNK;
    }
    else if ( rdModel3_lightingMode >= curLightingMode ) {
        rdModel3_lightingMode = curLightingMode;
    }

// This function = 1ms or so
#ifdef TARGET_TWL
    //thingFrustumCull = 0;
    //meshFrustumCull = 0;
    // TODO: Check if it's really that expensive
    rdModel3_lightingMode = RD_LIGHTMODE_DIFFUSE;
#endif

    rdModel3_textureMode = pCurMesh->textureMode;
    if ( rdModel3_textureMode >= curTextureMode )
        rdModel3_textureMode = curTextureMode;

    vertexSrc.paDynamicLight = pCurMesh->vertices_unk;
    vertexSrc.aVertices = aView;
    vertexSrc.aTexVerticies = pCurMesh->aTexVerticies;
    vertexSrc.intensities = 0;
    vertexDst.aVertices = aFaceVerts;

    if (rdModel3_lightingMode == RD_LIGHTMODE_FULLYLIT)
    {
    }
    else if (rdModel3_lightingMode == RD_LIGHTMODE_NOTLIT)
    {
    }
    else if (rdModel3_lightingMode == RD_LIGHTMODE_DIFFUSE)
    {
        rdModel3_numMeshLights = 0;
        pGeoLight = apGeoLights;
        for (int i = 0; i < rdModel3_numGeoLights; i++)
        {
            int lightIdx = (*pGeoLight)->id;

            // Added: dist -> dist squared
            flex_t dist = (*pGeoLight)->minRadius + pCurMesh->radius;
            if ( dist*dist > rdVector_DistSquared3(&rdCamera_g_pCurCamera->aLightPositions[lightIdx], &orient->scale) )
            {
                apMeshLights[rdModel3_numMeshLights] = *pGeoLight;
                rdMatrix_TransformPoint34(&rdModel3_aLocalLightPos[rdModel3_numMeshLights], &rdCamera_g_pCurCamera->aLightPositions[lightIdx], &matInv);
                
                // MOTS added
                if ((*pGeoLight)->type == 3) {
                    flex_t tmpZ = orient->scale.z;
                    rdVector_Zero3(&orient->scale);

                    rdVector3 tmpDir;
                    rdVector_Neg3(&tmpDir, &(*pGeoLight)->direction);
                    rdMatrix_TransformPoint34(&rdModel3_aLocalLightDir[rdModel3_numMeshLights], &tmpDir, &matInv);
                    orient->scale.z = tmpZ;
                }

                ++rdModel3_numMeshLights;
            }
            ++pGeoLight;
        }
    }
    else if (rdModel3_lightingMode == RD_LIGHTMODE_GOURAUD)
    {
        rdModel3_numMeshLights = 0;
        pGeoLight = apGeoLights;
        for (int i = 0; i < rdModel3_numGeoLights; i++)
        {
            int lightIdx = (*pGeoLight)->id;

            // Added: dist -> dist squared
            flex_t dist = (*pGeoLight)->minRadius + pCurMesh->radius;
            if ( dist*dist > rdVector_DistSquared3(&rdCamera_g_pCurCamera->aLightPositions[lightIdx], &orient->scale) )
            {
                apMeshLights[rdModel3_numMeshLights] = *pGeoLight;
                rdMatrix_TransformPoint34(&rdModel3_aLocalLightPos[rdModel3_numMeshLights], &rdCamera_g_pCurCamera->aLightPositions[lightIdx], &matInv);
                
                // MOTS added
                if ((*pGeoLight)->type == 3) {
                    flex_t tmpZ = orient->scale.z;
                    rdVector_Zero3(&orient->scale);

                    rdVector3 tmpDir;
                    rdVector_Neg3(&tmpDir, &(*pGeoLight)->direction);
                    rdMatrix_TransformPoint34(&rdModel3_aLocalLightDir[rdModel3_numMeshLights], &tmpDir, &matInv);
                    orient->scale.z = tmpZ;
                }

                ++rdModel3_numMeshLights;
            }
            ++pGeoLight;
        }

        // MOTS added assignment
        pMesh->extraLight = rdLight_CalcVertexIntensities(
            apMeshLights,
            rdModel3_aLocalLightPos,
#ifdef JKM_LIGHTING
            rdModel3_aLocalLightDir,
#endif
            rdModel3_numMeshLights,
            pCurMesh->vertexNormals,
            pCurMesh->aVertices,
            pCurMesh->vertices_i,
            pCurMesh->vertices_unk,
            pCurMesh->numVertices,
            rdCamera_g_pCurCamera->attenuationMin);
        rdModel3_CinematicLight(pCurMesh, &matInv, orient); // Added (remaster)
    }
    else if (rdModel3_lightingMode == RD_LIGHTMODE_6_UNK) // MOTS added
    {
        for (int i = 0; i < pMesh->numFaces; i++)
        {
            rdFace* face = &pMesh->faces[i];
            face->extraLight = pMesh->extraLight;
        }
    }

    // This is about 1/2 of the render time for E-11, 1/4 for saber
    // Before this is about 1/2 the render time for saber
    rdMatrix_TransformPoint34(&localCamera, &rdCamera_g_camMatrix.scale, &matInv);
    rdFace* face = &pMesh->faces[0];

    // Be extra sure we're setting backface culling
#ifdef TARGET_TWL
    rdroid_g_curRenderOptions |= 1;
#endif

    for (int i = 0; i < pMesh->numFaces; i++)
    {
        int flags = 0;
        flex_t normalCheck = (localCamera.y - pCurMesh->aVertices[*face->vertexPosIdx].y) * face->normal.y
           + (localCamera.x - pCurMesh->aVertices[*face->vertexPosIdx].x) * face->normal.x
           + (localCamera.z - pCurMesh->aVertices[*face->vertexPosIdx].z) * face->normal.z;
        
        // Allow rendering faces facing away from camera if they're double-sided,
        // or we aren't doing backface culling
        if ( normalCheck <= 0.0 )
        {
            flags = 1;
            if ( !(face->type & 1) && (rdroid_g_curRenderOptions & 1) )
            {
                ++face;
                continue;
            }
        }

        // Everything except rdModel3_DrawFace: 2ms
        rdModel3_DrawFace(face, flags);
        ++face;
    }

    if (pMesh->pSkin && pMesh->aVertices == pMesh->pSkin->aSkinned) // Added (remaster)
        rdModel3_SkinEnd(pMesh);
}

// MOTS altered (RGB aLights)
// Added (remaster): cinematic light on models (r_cinematicLight, needs per-pixel lighting).
// Per vertex, with the mesh's smooth normals: the flat ambient becomes a fill, a key light
// comes from the upper right of the shot (fixed relative to the camera, a little in front of
// the subject), and a rim lights edges turning away from the camera. Model faces then skip
// the ambient floor (rdModel3_DrawFace), so the shadow side can actually go dark.
int rdModel3_bCinematicMesh = 0;
void rdModel3_CinematicLight(rdMesh* pMesh, rdMatrix34* pMatInv, rdMatrix34* pOrient)
{
    rdModel3_bCinematicMesh = 0;
    if (!jkPlayer_bCinematicLight || !jkPlayer_bPerPixelLighting || !pMesh->vertexNormals || !rdCamera_g_pCurCamera)
        return;
    const rdMatrix34* cam = &rdCamera_g_camMatrix;
    rdVector3 keyW, keyL, camL;
    keyW.x = 0.62f * cam->rvec.x - 0.38f * cam->lvec.x + 0.69f * cam->uvec.x;
    keyW.y = 0.62f * cam->rvec.y - 0.38f * cam->lvec.y + 0.69f * cam->uvec.y;
    keyW.z = 0.62f * cam->rvec.z - 0.38f * cam->lvec.z + 0.69f * cam->uvec.z;
    // Aim the key at the strongest nearby glowing source when one dominates (a lamp, a strip),
    // keeping some of the screen-side key so the subject always has a lit and a shadow side.
    if (pOrient)
    {
        rdVector3 toLight;
        float w = sithGlowLights_Strongest(&pOrient->scale, &toLight) * 0.8f;
        if (w > 0.0f)
        {
            rdVector_Normalize3Acc(&keyW);
            keyW.x = keyW.x * (1.0f - w) + toLight.x * w;
            keyW.y = keyW.y * (1.0f - w) + toLight.y * w;
            keyW.z = keyW.z * (1.0f - w) + toLight.z * w;
        }
    }
    rdMatrix_TransformVector34(&keyL, &keyW, pMatInv);
    rdVector_Normalize3Acc(&keyL);
    rdMatrix_TransformPoint34(&camL, &cam->scale, pMatInv);
    flex_t amb = rdCamera_g_pCurCamera->ambientLight;
    flex_t fill = jkPlayer_cineFill, keyS = jkPlayer_cineKey * 1.3f, rimS = jkPlayer_cineRim * 0.8f;
    for (int i = 0; i < pMesh->numVertices; i++)
    {
        const rdVector3* n = &pMesh->vertexNormals[i];
        rdVector3 v;
        rdVector_Sub3(&v, &camL, &pMesh->aVertices[i]);
        rdVector_Normalize3Acc(&v);
        flex_t base = pMesh->vertices_unk[i] > amb ? pMesh->vertices_unk[i] : amb;
        flex_t key = rdVector_Dot3(n, &keyL);
        if (key < 0) key = 0;
        flex_t ndv = rdVector_Dot3(n, &v);
        if (ndv < 0) ndv = 0;
        if (ndv > 1) ndv = 1;
        flex_t rim = (1 - ndv) * (1 - ndv) * (1 - ndv);
        flex_t lit = base * (fill + keyS * key) + rimS * rim * (base > 0.25f ? base : 0.25f);
        pMesh->vertices_unk[i] = lit;
    }
    rdModel3_bCinematicMesh = 1;
}

int rdModel3_DrawFace(rdFace *pFace, int lightFlags)
{
    rdProcEntry *procEntry;
    rdGeoMode_t geometryMode;
    rdLightMode_t lightingMode;
    rdTexMode_t textureMode;
    rdVector3 faceNormal;
    int flags;

    procEntry = rdCache_GetProcEntry();
    if ( !procEntry )
        return 0;

    // Force diffuse lighting, we can't be ballers on DSi
#ifdef TARGET_TWL
    rdModel3_lightingMode = RD_LIGHTMODE_DIFFUSE;
#endif

    geometryMode = rdModel3_geometryMode;
    if ( rdModel3_geometryMode >= pFace->geometryMode )
        geometryMode = pFace->geometryMode;

    lightingMode = rdModel3_lightingMode;
    if ( rdModel3_lightingMode >= pFace->lightingMode )
        lightingMode = pFace->lightingMode;

    textureMode = rdModel3_textureMode;
    if ( rdModel3_textureMode >= pFace->textureMode )
        textureMode = pFace->textureMode;

    // MOTS added
    if ((pFace->type & 0x10) != 0) {
        lightingMode = RD_LIGHTMODE_NOTLIT;
    }

    // Added: safeguard
    if (!pFace->vertexUVIdx && geometryMode == RD_GEOMETRY_FULL) {
        geometryMode = RD_GEOMETRY_SOLID;
    }

    procEntry->geometryMode = geometryMode;
    procEntry->lightingMode = lightingMode;
    procEntry->textureMode = textureMode;
    vertexDst.verticesOrig = procEntry->aVertices;
    vertexDst.aTexVerticies = procEntry->aTexVerticies;
    vertexDst.paDynamicLight = procEntry->vertexIntensities;
    vertexSrc.numVertices = pFace->numVertices;
    vertexSrc.vertexPosIdx = pFace->vertexPosIdx;
    vertexSrc.vertexUVIdx = pFace->vertexUVIdx;

    // MOTS added: RGB
    if ((rdGetVertexColorMode() == 0) || (procEntry->lightingMode == RD_LIGHTMODE_DIFFUSE)) {
        if (meshFrustumCull != SPHERE_FULLY_INSIDE)
            rdPrimit3_ClipFace(rdCamera_g_pCurCamera->pClipFrustum, geometryMode, lightingMode, textureMode, &vertexSrc, &vertexDst, &pFace->texVertOffset);
        else
            rdPrimit3_NoClipFace(geometryMode, lightingMode, textureMode, &vertexSrc, &vertexDst, &pFace->texVertOffset);
    }
    else {
        vertexSrc.paRedIntensities = pCurMesh->paRedIntensities;
        vertexSrc.paGreenIntensities = pCurMesh->paGreenIntensities;
        vertexSrc.paBlueIntensities = pCurMesh->paBlueIntensities;
        vertexDst.paRedIntensities = procEntry->paRedIntensities;
        vertexDst.paGreenIntensities = procEntry->paGreenIntensities;
        vertexDst.paBlueIntensities = procEntry->paBlueIntensities;
        //printf("%p %p %p, %p %p %p\n", vertexSrc.paRedIntensities, vertexSrc.paGreenIntensities, vertexSrc.paBlueIntensities, vertexDst.paRedIntensities, vertexDst.paGreenIntensities, vertexDst.paBlueIntensities);
        if (meshFrustumCull != SPHERE_FULLY_INSIDE)
            rdPrimit3_ClipFaceRGB(rdCamera_g_pCurCamera->pClipFrustum, geometryMode, lightingMode, textureMode, &vertexSrc, &vertexDst, &pFace->texVertOffset);
        else
            rdPrimit3_NoClipFaceRGB(geometryMode, lightingMode, textureMode, &vertexSrc, &vertexDst, &pFace->texVertOffset);
    }

    if ( vertexDst.numVertices < 3u )
        return 0;

    if ( procEntry->lightingMode == RD_LIGHTMODE_DIFFUSE )
    {
        if ( lightFlags )
        {
            rdVector_Neg3(&faceNormal, &pFace->normal);
            procEntry->light_level_static = rdLight_CalcFaceIntensity(
                      apMeshLights,
                      rdModel3_aLocalLightPos,
                      rdModel3_numMeshLights,
                      pFace,
                      &faceNormal,
                      pCurMesh->aVertices,
                      rdCamera_g_pCurCamera->attenuationMin);
        }
        else
        {
            procEntry->light_level_static = rdLight_CalcFaceIntensity(
                      apMeshLights,
                      rdModel3_aLocalLightPos,
                      rdModel3_numMeshLights,
                      pFace,
                      &pFace->normal,
                      pCurMesh->aVertices,
                      rdCamera_g_pCurCamera->attenuationMin);
        }
    }
    rdCamera_g_pCurCamera->pfProjectList(vertexDst.verticesOrig, vertexDst.aVertices, vertexDst.numVertices);
    if ( rdroid_g_curRenderOptions & 2 )
        procEntry->ambientLight = rdCamera_g_pCurCamera->ambientLight;
    else
        procEntry->ambientLight = 0.0;
    if (rdModel3_bCinematicMesh && procEntry->lightingMode == RD_LIGHTMODE_GOURAUD)
        procEntry->ambientLight = 0.0; // Added (remaster): cinematic light already includes the fill

    int isIdentityMap = (rdColormap_pCurMap == rdColormap_pIdentityMap);
    procEntry->wallCel = pFace->wallCel;

    
#if defined(TARGET_TWL)
    if ( procEntry->lightingMode == 3 )
    {
        procEntry->light_level_static = *procEntry->vertexIntensities;
    }
    // These are software renderer optimizations, skip
#elif !defined(SDL2_RENDER)
    if ( procEntry->ambientLight < 1.0 )
    {
        if ( procEntry->lightingMode == RD_LIGHTMODE_DIFFUSE )
        {
            if ( procEntry->light_level_static >= 1.0 && isIdentityMap )
            {
                procEntry->lightingMode = RD_LIGHTMODE_FULLYLIT;
            }
            else if ( procEntry->light_level_static <= 0.0 )
            {
                procEntry->lightingMode = RD_LIGHTMODE_NOTLIT;
            }
        }
        else if (rdGetVertexColorMode() == 0 && procEntry->lightingMode == RD_LIGHTMODE_GOURAUD) {
            for (int i = 1; i < vertexDst.numVertices; i++ )
            {
                    flex_t level = procEntry->vertexIntensities[i] - procEntry->vertexIntensities[0];
                    if ( level < 0.0 )
                        level = -level;

                    if ( level > 0.015625 )
                        break;
            }
        }        
        else if (rdGetVertexColorMode() == 0 && procEntry->vertexIntensities[0] != 1.0 ) // TODO: Re-decompile this for MoTS
        {
            if ( procEntry->vertexIntensities[0] == 0.0 )
            {
                procEntry->lightingMode = RD_LIGHTMODE_NOTLIT;
                procEntry->light_level_static = 0.0;
            }
            else
            {
                procEntry->lightingMode = RD_LIGHTMODE_DIFFUSE;
                procEntry->light_level_static = procEntry->vertexIntensities[0];
            }
        }
        else if (rdGetVertexColorMode() == 0 && isIdentityMap )
        {
            procEntry->lightingMode = RD_LIGHTMODE_FULLYLIT;
        }
        else if (rdGetVertexColorMode() == 0) { // TODO: Re-decompile this for MoTS
            procEntry->lightingMode = RD_LIGHTMODE_DIFFUSE;
            procEntry->light_level_static = 1.0;
        }
    }
    else if ( !isIdentityMap )
    {
        procEntry->lightingMode = RD_LIGHTMODE_DIFFUSE;
        procEntry->light_level_static = 1.0;
    }
    else {
        procEntry->lightingMode = RD_LIGHTMODE_FULLYLIT;
    }
#else
    // Nothing for SDL2
#endif

    flags = 1;
    if ( procEntry->geometryMode >= 4 )
        flags = 3;
    if ( procEntry->lightingMode >= 3 )
        flags |= 4u;

    procEntry->light_flags = lightFlags | 0x08000000u; // Added (remaster): model face (cinematic key/rim light)
    procEntry->type = pFace->type;
    procEntry->extralight = pFace->extraLight;
    procEntry->material = pFace->material;
    rdCache_AddProcFace(0, vertexDst.numVertices, flags);
    return 1;
}

// Added: Data preloading
void rdModel3_EnsureMaterialData(rdThing *pRdThing) {
    rdModel3* pModel3 = NULL;

    if (!pRdThing) {
        return;
    }
    pModel3 = pRdThing->model3;
    if (!pModel3 || !pModel3->aMaterials) {
        return;
    }

    for (int i = 0; i < pModel3->sizeMaterials; i++)
    {
        rdMaterial_EnsureData(pModel3->aMaterials[i]);
    }
}
