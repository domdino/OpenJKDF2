#ifndef _SITHATMOS_H
#define _SITHATMOS_H

// Added (remaster): atmosphere. Volumetric fog lit by the baked light volume
// (tools/blender/volume_bake.py writes jkgm/lightmaps/<level>.jklv: light in the air on a fine
// grid inside normal sectors and a coarse one everywhere). The volume is sparse, so each frame
// the part around the camera is copied into two small dense grids (clipmaps) that the fog shader
// samples: fine (SITHATMOS_FINE cells, ~24 units across) and coarse (~64 cells of the coarse
// spacing). See docs/stage2-atmosphere-plan.md.

#include "types.h"

#define SITHATMOS_FINE_XY 96
#define SITHATMOS_FINE_Z 48
#define SITHATMOS_COARSE_XY 64
#define SITHATMOS_COARSE_Z 32

typedef struct sithAtmosGrid
{
    float* data;        // RGBA per cell (A = 1 where the bake has a point, 0 = no data)
    int dims[3];
    int origin[3];      // grid index of cell 0
    float spacing;      // world units per cell
    int dirty;          // changed since the last upload
} sithAtmosGrid;

// Update the clipmaps around the camera (loads the level's volume when the level changes).
// Returns 0 when the level has no light volume (the fog falls back to screen-space haze).
int sithAtmos_Update(const rdVector3* pCamPos, sithAtmosGrid** ppFine, sithAtmosGrid** ppCoarse);

// The current level's height fog: density at and below the ground height, the ground height
// (world z) and how fast it thins with height (units). Density 0 = none (indoor levels).
void sithAtmos_GetHeightFog(float* pDensity, float* pGroundZ, float* pFalloff);

#endif // _SITHATMOS_H
