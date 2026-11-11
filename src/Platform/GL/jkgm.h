#ifndef _PLATFORM_GL_JKGM_H
#define _PLATFORM_GL_JKGM_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void* jkgm_alloc_aligned(size_t amt);
void jkgm_aligned_free(void* p);

#ifdef SDL2_RENDER
#if defined(TARGET_CAN_JKGM)
void jkgm_startup();
void jkgm_populate_cache();
void jkgm_populate_shortcuts(tVBuffer *vbuf, rdDDrawSurface *texture, rdMaterial* material, int is_alpha_tex, int mipmap_level, int cel);
int jkgm_std3D_AddToTextureCache(tVBuffer *vbuf, rdDDrawSurface *texture, int is_alpha_tex, int no_alpha, rdMaterial* material, int cel);
void jkgm_free_cache_entry(jkgm_cache_entry_t* entry);
void jkgm_write_png(const char *pFname, int width, int height, uint8_t* paFramebuffer);
// Added (remaster): palette (768 bytes, as in the level's colormap) used to hash textures
// for HD pack signatures while a level loads. NULL clears it.
void jkgm_set_hash_palette(const uint8_t* pPal768);

#endif // ARCH_WASM
#endif //SDL2_RENDER

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_GL_JKGM_H