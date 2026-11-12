#ifndef _SITHLIGHTPROBES_H
#define _SITHLIGHTPROBES_H

// Added (remaster): baked light probes for models (light bake step 4).
// tools/blender/probe_bake.py bakes, on a grid over every floor at chest height, the light
// arriving from six directions (+X, -X, +Y, -Y, +Z, -Z, in colour) with the same lights as the
// level's light map, and writes jkgm/lightmaps/<level>.jklp. Models blend the nearest probes
// and light each vertex from the side it faces (rdModel3_CinematicLight), so Kyle walking past
// a red strip turns red on that side. Loaded lazily when a level starts; a missing file or one
// baked for another version of the level means no probes (the cinematic rig as before).

#include "types.h"

// The sector the model being drawn is in (sithRender sets it around each thing), or NULL.
extern SithSector* sithLightProbes_pThingSector;

// Probes come in two layers: just above the floor and at chest height. A sample is the blend of
// the nearest of each, as ambient cubes ([face][rgb], faces +X, -X, +Y, -Y, +Z, -Z, world axes),
// with the world height of each; a model vertex mixes the two by its own height.
typedef struct sithLightProbeSample
{
    float low[6][3], high[6][3];
    float zLow, zHigh;
} sithLightProbeSample;

// Blend the probes near pPos (in pSector and the sectors it opens into; NULL = the camera's
// sector). Returns 0 when the level has no probes or none are near.
int sithLightProbes_Sample(const rdVector3* pPos, SithSector* pSector, sithLightProbeSample* pOut);

// Light arriving at a surface with world normal n, from an ambient cube.
void sithLightProbes_Eval(const float aCube[6][3], const rdVector3* n, float out[3]);

#endif // _SITHLIGHTPROBES_H
