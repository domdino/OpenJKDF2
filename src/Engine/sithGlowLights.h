#ifndef _SITHGLOWLIGHTS_H
#define _SITHGLOWLIGHTS_H

// Added (remaster): static "glow lights" placed at glowing level surfaces (light panels,
// floor strips, lamps), so per-pixel lighting can add wet highlights from them. The
// baked level lighting already contains their glow, so these mostly add specular.

#include "types.h"

#define SITHGLOWLIGHTS_MAX_GATHER (24)

// Number of glow lights found in the current world (rebuilt automatically on level change).
int sithGlowLights_Count(void);

// Fill up to maxLights of the nearest glow lights in currently visible sectors, already in
// camera space: aPos = xyz + radius, aCol = rgb + intensity. Returns how many were written.
int sithGlowLights_Gather(rdCamera* pCamera, float* aPos, float* aCol, int maxLights);

// The strongest glow light near a world position among the visible sectors (by intensity over
// squared distance). Writes the unit direction from pos to it; returns 0..1 how dominant it is
// (0 = nothing near). Used to aim the cinematic key light on characters at a real source.
float sithGlowLights_Strongest(const rdVector3* pPos, rdVector3* pOutDir);

// Representative colour of a material (its glowing texels if it has enough, else all
// texels), normalised so the brightest channel is 1. Returns 0 if it can't be read.
int sithGlowLights_MaterialColor(rdMaterial* pMat, float* pOutRGB);

#endif // _SITHGLOWLIGHTS_H
