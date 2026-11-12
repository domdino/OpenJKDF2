#ifndef _SITHSKY_H
#define _SITHSKY_H

// Added (remaster): volumetric cloud sky. On levels whose sky texture has a preset, sky
// surfaces are drawn by the renderer from a raymarched cloud panorama (std3D + sky_f.glsl)
// instead of the 1997 scrolling texture. This file holds the presets, picks one per level,
// times the lightning, and builds the tileable 3D noise the clouds are made of.

#include "types.h"

typedef struct sithSkyPreset
{
    const char* pName;
    const char* aMats[4];       // sky materials this preset replaces (name without folder)
    float sunAzimuth;           // degrees, 0 = world +X, counter-clockwise from above
    float sunElevation;         // degrees above the horizon
    float sunColor[3];          // light from the sun or moon
    float sunIntensity;
    float sunSize;              // disc radius in degrees (0 = no disc)
    float zenith[3];            // clear sky colour overhead
    float horizon[3];           // clear sky colour at the horizon
    float ambientTop[3];        // light on cloud tops (sky light)
    float ambientBottom[3];     // light on cloud bases (bounce from below)
    float coverage;             // 0 = clear, 1 = overcast
    float density;              // extinction per km inside a full cloud
    float cloudBase, cloudTop;  // km above the viewer
    float wind;                 // km per second the clouds drift
    float stars;                // 0..1
    float lightning;            // flashes per minute (0 = none)
    float below[3];             // colour below the horizon (a sea of murk)
    float exposure;             // overall sky brightness (the grade darkens the scene a lot)
} sithSkyPreset;

// The preset for the current world (rechecked when the level changes), NULL if this level
// keeps its original sky or the feature is off.
const sithSkyPreset* sithSky_GetActive(void);

// True if this material is one of the active preset's sky textures. Some levels paint sky
// textures on ordinary faces too (fake windows, the sky above a wall); those get the sky.
int sithSky_IsSkyMaterial(rdMaterial* pMat);

// Effective sun direction (world, unit) and coverage, after the r_sky* overrides.
void sithSky_GetSunDir(const sithSkyPreset* p, float* pOut3);
float sithSky_GetCoverage(const sithSkyPreset* p);

// Lightning at time t (seconds): returns 0..1 intensity, and the flash direction (world, unit).
float sithSky_GetFlash(const sithSkyPreset* p, double t, float* pDir3);

// Tileable 3D noise for the cloud shapes: size^3 bytes, built once on first call.
// Shape: R = Perlin-Worley (base shape), G/B/A = Worley at 3 frequencies (erosion).
const uint8_t* sithSky_GetShapeNoise(int* pSize);
// Detail: R/G/B = Worley at 3 higher frequencies, size^3 * 4 bytes (RGBA).
const uint8_t* sithSky_GetDetailNoise(int* pSize);

#endif // _SITHSKY_H
