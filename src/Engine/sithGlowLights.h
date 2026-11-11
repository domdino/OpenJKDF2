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

#endif // _SITHGLOWLIGHTS_H
