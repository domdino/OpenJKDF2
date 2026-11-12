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

// Hand-placed fog volumes (jkgm/atmos/<level>.json, placed in Blender with
// tools/blender/atmos_volumes.py): boxes and ellipsoids of extra fog with a tint and soft edges.
#define SITHATMOS_MAX_SHADER_VOLUMES 16
// The volumes nearest the camera (up to max), packed for the fog shader, 4 floats each:
//   center xyz + shape (0 box, 1 ellipsoid); half size xyz + edge softness (0-1);
//   cos and sin of the rotation about z, density, 0; tint rgb + 0. Returns how many.
int sithAtmos_GetVolumes(const rdVector3* pCamPos, float* pCenter, float* pSize, float* pParams, float* pTint, int max);

// Smoke and steam emitters (same JSON, "emitters" array). Particles are a function of time only
// (no simulation state): each emitter keeps rate x life particles cycling from birth to death.
#define SITHATMOS_MAX_PARTICLES 96
typedef struct sithAtmosParticle
{
    float pos[3];      // world
    float radius;      // world units
    float age;         // 0 = just born, 1 = dying
    float seed;        // 0-1, varies per particle and per cycle
    float kind;        // 0 steam, 1 smoke, 2 dust mote
    float tint[3];
    float opacity;
} sithAtmosParticle;

// Particles of emitters within reach of the camera (up to max). Returns how many.
int sithAtmos_GetParticles(float timeSec, const rdVector3* pCamPos, sithAtmosParticle* pOut, int max);

// Dust motes: tiny specks fixed in the world around the camera (so they parallax as it moves),
// drifting slowly, one per cell of a grid, in front of the camera (pFwd: its world forward).
// The shader shows them only where the baked light in the air is bright. Returns how many.
int sithAtmos_GetDust(float timeSec, const rdVector3* pCamPos, const float* pFwd, float amount, sithAtmosParticle* pOut, int max);

#endif // _SITHATMOS_H
