#include "Platform/std3D.h"
#include <ctype.h> // Added (remaster): light map file names

#include "Raster/rdCache.h"
#include "Raster/rdZRaster.h"
#include "Win95/stdDisplay.h"
#include "Win95/Window.h"
#include "Win95/Video.h"
#include "World/sithWorld.h"
#include "Engine/rdColormap.h"
#include "Main/jkGame.h"
#include "World/jkPlayer.h"
#include "Engine/rdCamera.h" // Added (remaster): per-pixel lighting
#include "Primitives/rdMatrix.h"
#include "Engine/sithGlowLights.h" // Added (remaster): glow-light highlights
#include "Engine/sithRender.h"
#include "General/stdBitmap.h"
#include "stdPlatform.h"

#include "jk.h"

#include <stdbool.h>
#include <stdio.h>
#include <math.h> // Added (remaster): sqrt for the auto render scale
#include <stdlib.h>
#include "Platform/GL/shader_utils.h"
#include "Platform/GL/jkgm.h"

#include "SDL2_helper.h"
#include "Engine/sithSky.h" // Added (remaster)
#include "Engine/sithAtmos.h" // Added (remaster): volumetric fog
#include "General/stdString.h"

#ifdef WIN32
// Force Optimus/AMD to use non-integrated GPUs by default.
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

#define TEX_MODE_TEST 0
#define TEX_MODE_WORLDPAL 1
#define TEX_MODE_BILINEAR 2
#define TEX_MODE_16BPP 5
#define TEX_MODE_BILINEAR_16BPP 6

// Added: Custom RGB lightsaber triangle marker.
#define RD_TRI_FLAG_SABER_RGB 0x40000
#define RD_TRI_FLAG_SABER_BLADE 0x80000 // Added (remaster): procedural lightsaber blade (rdCache)
#define RD_TRI_FLAG_SKY 0x100000        // Added (remaster): volumetric sky surface (rdCache)
#define RD_TRI_FLAG_MODEL 0x200000      // Added (remaster): face of a 3D model (cinematic light)

typedef struct std3DSimpleTexStage
{
    GLuint program;

    GLint attribute_coord3d;
    GLint attribute_v_color;
    GLint attribute_v_uv;
    GLint attribute_v_norm;

    GLint uniform_mvp;
    GLint uniform_tex;
    GLint uniform_tex2;
    GLint uniform_tex3;
    GLint uniform_iResolution;

    GLint uniform_param1;
    GLint uniform_param2;
    GLint uniform_param3;
} std3DSimpleTexStage;

typedef struct std3DIntermediateFbo
{
    GLuint fbo;
    GLuint tex;

    GLuint rbo;
    int32_t w;
    int32_t h;

    int32_t iw;
    int32_t ih;
} std3DIntermediateFbo;

typedef struct std3DFramebuffer
{
    GLuint fbo;
    GLuint tex0;
    GLuint tex1;
    GLuint tex2;
    GLuint tex3;
    GLuint tex4; // Added (remaster): reflection data (octahedral normal, signed depth)
    GLuint tex5; // Added (remaster): material per pixel (R = roughness, G = metalness)

    std3DIntermediateFbo window;
    std3DIntermediateFbo main;

    int enable_extra;
    std3DIntermediateFbo blur1;
    std3DIntermediateFbo blur2;
    std3DIntermediateFbo blur3;
    std3DIntermediateFbo blur4;
    //std3DIntermediateFbo blurBlend;

    std3DIntermediateFbo ssaoBlur1;
    std3DIntermediateFbo ssaoBlur2;
    //std3DIntermediateFbo ssaoBlur3;

    std3DIntermediateFbo grade; // Added (remaster): half-float scene composite for the grading pass
    std3DIntermediateFbo hazeBlur1; // Added (remaster): blurred scene that lights the haze
    std3DIntermediateFbo hazeBlur2;

    GLuint rbo;
    int32_t w;
    int32_t h;
} std3DFramebuffer;

GLint std3D_windowFbo = 0;
std3DFramebuffer std3D_framebuffers[2];
std3DFramebuffer *std3D_pFb = NULL;

static bool has_initted = false;

static void* last_overlay = NULL;

static int std3D_activeFb = 1;

int init_once = 0;
GLuint programDefault, programMenu;
GLint attribute_coord3d, attribute_v_color, attribute_v_light, attribute_v_uv, attribute_v_norm;
// Added (remaster): per-pixel lighting. v_invz carries 1/(32*view depth) from the render cache
// (D3DVERTEX.nx), which the shader uses to rebuild each pixel's camera-space position.
#define STD3D_PPL_MAX_LIGHTS (32)
GLint attribute_v_invz = -1;
static GLint uniform_ppl = -1, uniform_pplNumLights = -1, uniform_pplLightPos = -1, uniform_pplLightCol = -1;
static GLint uniform_fixtureBloom = -1; // Added (remaster)
static GLint uniform_pplAtten = -1, uniform_pplProj = -1, uniform_pplScreen = -1, uniform_pplSpecular = -1, uniform_pplWetness = -1;
static GLint uniform_pplNumGlow = -1, uniform_pplGlowPos = -1, uniform_pplGlowCol = -1;
static GLint uniform_ssrWrite = -1, uniform_pplUpCam = -1;
static GLint uniform_texOrm = -1, uniform_texNormal = -1, uniform_hasOrm = -1, uniform_hasNormal = -1; // Added (remaster)
static GLint std3D_ssrUniformTexMat = -1;
// Added (remaster): screen-space reflection pass, and the projection values it needs from
// the last world draw of the frame.
std3DSimpleTexStage std3D_ssrStage;
static GLint std3D_ssrUniformProj = -1, std3D_ssrUniformScreen = -1, std3D_ssrUniformUpCam = -1;
static GLint std3D_ssrUniformWetness = -1, std3D_ssrUniformStrength = -1, std3D_ssrUniformRoughness = -1;
static float std3D_ssrProj[4], std3D_ssrScreen[4], std3D_ssrUpCam[3];
static int std3D_ssrHaveCamera = 0;
// Added (remaster): atmosphere passes, and the glow lights of the current frame (camera
// space), which the shaft pass aims at.
std3DSimpleTexStage std3D_hazeStage;
// Added (remaster): volumetric fog (sithAtmos), lit by the baked light volume.
std3DSimpleTexStage std3D_fogStage;
std3DSimpleTexStage std3D_flareStage; // Added (remaster): lens flares
static GLuint std3D_fogTex[2] = {0, 0};
static GLint std3D_fogU[16];
static float std3D_fogCamPos[3];
enum { FOGU_FINE, FOGU_COARSE, FOGU_FORIGIN, FOGU_FSIZE, FOGU_CORIGIN, FOGU_CSIZE, FOGU_PROJ, FOGU_SCREEN,
       FOGU_WX, FOGU_WY, FOGU_WZ, FOGU_CAM, FOGU_FOG, FOGU_FOG2, FOGU_HFOG,
       FOGU_NVOL, FOGU_VCENTER, FOGU_VSIZE, FOGU_VPARAMS, FOGU_VTINT };
std3DSimpleTexStage std3D_shaftStage;
static GLint std3D_shaftUniformNum = -1, std3D_shaftUniformPos = -1, std3D_shaftUniformCol = -1;
// Added (remaster): volumetric sky (sithSky.c, sky_f.glsl). The cloud panorama is refreshed a
// slice of columns per frame at frame start; sky surfaces sample it on texture unit 7.
#define STD3D_SKY_PANO_W 4096
#define STD3D_SKY_PANO_H 1024
#define STD3D_SKY_SLICES 8
std3DSimpleTexStage std3D_skyStage;
static std3DIntermediateFbo std3D_skyPano;
static int std3D_skyPanoReady = 0, std3D_skySlice = 0, std3D_skyFilled = 0;
static const sithSkyPreset* std3D_skyPanoPreset = NULL;
static GLuint std3D_skyShapeTex = 0, std3D_skyDetailTex = 0;
static GLint std3D_skyU_shape = -1, std3D_skyU_detail = -1, std3D_skyU_sunDir = -1, std3D_skyU_sunCol = -1;
static GLint std3D_skyU_ambTop = -1, std3D_skyU_ambBottom = -1, std3D_skyU_horizon = -1, std3D_skyU_layer = -1, std3D_skyU_wind = -1;
static GLint uniform_sky = -1, uniform_skyPano = -1, uniform_skyWX = -1, uniform_skyWY = -1, uniform_skyWZ = -1;
static GLint uniform_skySunDir = -1, uniform_skySunCol = -1, uniform_skyZenith = -1, uniform_skyHorizon = -1;
static GLint uniform_skyBelow = -1, uniform_skyFlash = -1;
static GLint uniform_model = -1, uniform_cine = -1; // Added (remaster): cinematic key/rim light on models
// Added (remaster): baked light maps (see std3D_LightmapUpdate).
GLint attribute_v_lmSurf = -1;
GLint attribute_v_glow = -1; // Added (remaster): light-probe glow on models (vertex nz)
static GLint uniform_lmOn = -1, uniform_lmAtlas = -1, uniform_lmData = -1, uniform_lmParams = -1, uniform_lmCamPos = -1;
static float std3D_skyWorldAxes[9]; // world X, Y, Z axes in camera space (last world draw)
static int std3D_frameNumGlow = 0;
static float std3D_frameGlowPos[SITHGLOWLIGHTS_MAX_GATHER * 4], std3D_frameGlowCol[SITHGLOWLIGHTS_MAX_GATHER * 4];
GLint uniform_mvp, uniform_tex, uniform_texEmiss, uniform_displacement_map, uniform_tex_mode, uniform_blend_mode, uniform_worldPalette, uniform_worldPaletteLights;
GLint uniform_tint, uniform_filter, uniform_fade, uniform_add, uniform_emissiveFactor, uniform_albedoFactor;
GLint uniform_light_mult, uniform_displacement_factor, uniform_iResolution;

// Added: Custom RGB lightsaber shader uniforms.
static GLint uniform_saberRgbEnabled = -1;
static GLint uniform_saberRgbColor = -1;
static GLint uniform_saberBlade = -1;      // Added (remaster)
static GLint uniform_saberStrength = -1;   // Added (remaster)
static GLint uniform_time = -1;            // Added (remaster): seconds, for animated effects

// RGB state defined in jkSaber.c.
extern float jkSaber_rgbRed;
extern float jkSaber_rgbGreen;
extern float jkSaber_rgbBlue;

GLint programMenu_attribute_coord3d, programMenu_attribute_v_color, programMenu_attribute_v_uv, programMenu_attribute_v_norm;
GLint programMenu_uniform_mvp, programMenu_uniform_tex, programMenu_uniform_displayPalette;

std3DSimpleTexStage std3D_uiProgram;
std3DSimpleTexStage std3D_texFboStage;
std3DSimpleTexStage std3D_blurStage;
std3DSimpleTexStage std3D_ssaoStage;
std3DSimpleTexStage std3D_ssaoMixStage;

// Added (remaster): colour grading pass and its 3D LUT
std3DSimpleTexStage std3D_gradeStage;
static GLint std3D_gradeUniformLut = -1, std3D_gradeUniformLutSize = -1;
static GLint std3D_gradeUniformDomainMin = -1, std3D_gradeUniformDomainMax = -1;
static GLuint std3D_gradeLutTex = 0;
static int std3D_gradeLutSize = 0;
static float std3D_gradeLutDomainMin[3] = {0.0f, 0.0f, 0.0f};
static float std3D_gradeLutDomainMax[3] = {1.0f, 1.0f, 1.0f};

GLuint blank_tex, blank_tex_white;
void* blank_data = NULL, *blank_data_white = NULL;
GLuint worldpal_texture;
void* worldpal_data = NULL;
GLuint worldpal_lights_texture;
void* worldpal_lights_data = NULL;
GLuint displaypal_texture;
void* displaypal_data = NULL;
GLuint tiledrand_texture;
rdVector3* tiledrand_data = NULL;

size_t std3D_loadedUITexturesAmt = 0;
stdBitmap* std3D_aUIBitmaps[STD3D_MAX_TEXTURES] = {0};
GLuint std3D_aUITextures[STD3D_MAX_TEXTURES] = {0};
static rdUITri GL_tmpUITris[STD3D_MAX_UI_TRIS] = {0};
static size_t GL_tmpUITrisAmt = 0;
GLuint last_ui_tex = 0;
int last_ui_flags = 0;
static D3DVERTEX GL_tmpUIVertices[STD3D_MAX_UI_VERTICES] = {0};
static size_t GL_tmpUIVerticesAmt = 0;

rdDDrawSurface* std3D_aLoadedSurfaces[STD3D_MAX_TEXTURES] = {0};
GLuint std3D_aLoadedTextures[STD3D_MAX_TEXTURES] = {0};
size_t std3D_loadedTexturesAmt = 0;
static rdTri GL_tmpTris[STD3D_MAX_TRIS] = {0};
static size_t GL_tmpTrisAmt = 0;
static rdLine GL_tmpLines[STD3D_MAX_VERTICES] = {0};
static size_t GL_tmpLinesAmt = 0;
static D3DVERTEX GL_tmpVertices[STD3D_MAX_VERTICES] = {0};
static size_t GL_tmpVerticesAmt = 0;
static size_t rendered_tris = 0;

static void* loaded_colormap = NULL;

rdDDrawSurface* last_tex = NULL;
int last_flags = 0;

D3DVERTEX* world_data_all = NULL;
GLushort* world_data_elements = NULL;
GLuint world_vbo_all;
GLuint world_ibo_triangle;

D3DVERTEX* menu_data_all = NULL;
GLushort* menu_data_elements = NULL;
GLuint menu_vbo_all;
GLuint menu_ibo_triangle;

extern int jkGuiBuildMulti_bRendering;

int std3D_bInitted = 0;
rdColormap std3D_ui_colormap;
int std3D_bReinitHudElements = 0;

void std3D_generateIntermediateFbo(int32_t width, int32_t height, std3DIntermediateFbo* pFbo, int isFloat)
{
    // Generate the framebuffer
    memset(pFbo, 0, sizeof(*pFbo));

    pFbo->w = width;
    pFbo->h = height;
    pFbo->iw = width;
    pFbo->ih = height;

    glActiveTexture(GL_TEXTURE0);

    glGenFramebuffers(1, &pFbo->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, pFbo->fbo);
    
    // Set up our framebuffer texture
    glGenTextures(1, &pFbo->tex);
    glBindTexture(GL_TEXTURE_2D, pFbo->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, isFloat ? GL_RGBA16F : GL_RGBA8, width, height, 0, GL_RGBA, isFloat ? GL_FLOAT : GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
    glGenerateMipmap(GL_TEXTURE_2D);
    
    // Attach fbTex to our currently bound framebuffer fb
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pFbo->tex, 0);

    // Set up our render buffer
    glGenRenderbuffers(1, &pFbo->rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, pFbo->rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    
    // Bind it to our framebuffer fb
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, pFbo->rbo);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        stdPlatform_Printf("std3D: ERROR, Framebuffer is incomplete!\n");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void std3D_deleteIntermediateFbo(std3DIntermediateFbo* pFbo)
{
    glDeleteFramebuffers(1, &pFbo->fbo);
    glDeleteTextures(1, &pFbo->tex);
    glDeleteRenderbuffers(1, &pFbo->rbo);
}

// Added (remaster): reflections need per-pixel lighting (they reuse its position/normal rebuild).
static int std3D_ReflectionsActive()
{
    return jkPlayer_bReflections && jkPlayer_bPerPixelLighting && !jkGuiBuildMulti_bRendering;
}

// Added (remaster): the extra depth/normal target is written whenever something reads it.
static int std3D_AtmosphereActive()
{
    return (jkPlayer_bHaze || jkPlayer_bLightShafts) && jkPlayer_bPerPixelLighting && !jkGuiBuildMulti_bRendering;
}
static int std3D_GBufferActive()
{
    return std3D_ReflectionsActive() || std3D_AtmosphereActive();
}

static void std3D_SetSceneDrawBuffers()
{
    static const GLenum bufs[6] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3, GL_COLOR_ATTACHMENT4, GL_COLOR_ATTACHMENT5};
    glDrawBuffers(std3D_GBufferActive() ? 6 : (jkPlayer_enableSSAO ? 4 : 2), bufs);
}

void std3D_generateFramebuffer(int32_t width, int32_t height, std3DFramebuffer* pFb)
{
    // Generate the framebuffer
    memset(pFb, 0, sizeof(*pFb));

    pFb->w = width;
    pFb->h = height;

    glActiveTexture(GL_TEXTURE0);

    glGenFramebuffers(1, &pFb->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, pFb->fbo);
    
    // Set up our framebuffer texture
    glGenTextures(1, &pFb->tex0);
    glBindTexture(GL_TEXTURE_2D, pFb->tex0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    // Added (remaster): cap the mip chain. The haze pass builds mips of this every frame and
    // the default (up to level 1000) meant a full chain of the whole scene each time.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 2);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Attach fbTex to our currently bound framebuffer fb
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pFb->tex0, 0);

    // Set up our emissive fb texture
    glGenTextures(1, &pFb->tex1);
    glBindTexture(GL_TEXTURE_2D, pFb->tex1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
    glGenerateMipmap(GL_TEXTURE_2D);
    //glGenerateMipmap(GL_TEXTURE_2D);
    
    // Attach fbTex to our currently bound framebuffer fb
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, pFb->tex1, 0);

    // Set up our position fb texture
    glGenTextures(1, &pFb->tex2);
    glBindTexture(GL_TEXTURE_2D, pFb->tex2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Attach fbTex to our currently bound framebuffer fb
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, pFb->tex2, 0);

    // Set up our normal fb texture
    glGenTextures(1, &pFb->tex3);
    glBindTexture(GL_TEXTURE_2D, pFb->tex3);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Attach fbTex to our currently bound framebuffer fb
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, pFb->tex3, 0);

    // Added (remaster): reflection data target. Only drawn to when reflections are on.
    glGenTextures(1, &pFb->tex4);
    glBindTexture(GL_TEXTURE_2D, pFb->tex4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT4, GL_TEXTURE_2D, pFb->tex4, 0);

    // Added (remaster): material target (roughness, metalness) for the reflection pass.
    glGenTextures(1, &pFb->tex5);
    glBindTexture(GL_TEXTURE_2D, pFb->tex5);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT5, GL_TEXTURE_2D, pFb->tex5, 0);

    // Set up our render buffer
    glGenRenderbuffers(1, &pFb->rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, pFb->rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    
    // Bind it to our framebuffer fb
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, pFb->rbo);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        stdPlatform_Printf("std3D: ERROR, Framebuffer is incomplete!\n");

    std3D_SetSceneDrawBuffers();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (jkPlayer_enableSSAO)
    {
        // (Tried half resolution for speed: this SSAO's depth/noise sampling bands visibly on large
        // flat surfaces then, so it stays at full resolution. See docs/DECISIONS.md.)
        std3D_generateIntermediateFbo(width, height, &pFb->ssaoBlur1, 0);
        std3D_generateIntermediateFbo(pFb->ssaoBlur1.w/2, pFb->ssaoBlur1.h/2, &pFb->ssaoBlur2, 0);
        //std3D_generateIntermediateFbo(pFb->ssaoBlur2.w/2, pFb->ssaoBlur2.h/2, &pFb->ssaoBlur3, 0);

        pFb->enable_extra |= 2;
    }

    if (jkPlayer_enableBloom)
    {
        pFb->enable_extra |= 1;
        // Added (remaster): first bloom blur at quarter resolution (was full, then half: 48
        // samples per pixel every frame). The later levels are quarter-steps of this, as before.
        std3D_generateIntermediateFbo(width / 4, height / 4, &pFb->blur1, 1);
        //std3D_generateIntermediateFbo(width, height, &pFb->blurBlend, 1);
        std3D_generateIntermediateFbo(pFb->blur1.w/4, pFb->blur1.h/4, &pFb->blur2, 1);
        std3D_generateIntermediateFbo(pFb->blur2.w/4, pFb->blur2.h/4, &pFb->blur3, 1);
        std3D_generateIntermediateFbo(pFb->blur3.w/4, pFb->blur3.h/4, &pFb->blur4, 1);

        /*pFb->blur1.iw = width;
        pFb->blur1.ih = height;
        pFb->blur2.iw = width;
        pFb->blur2.ih = height;
        pFb->blur3.iw = width;
        pFb->blur3.ih = height;
        pFb->blur4.iw = width;
        pFb->blur4.ih = height;*/
    }

    // Added (remaster): half-float target so bloom above 1.0 survives until the grade pass.
    if (jkPlayer_enableGrade)
    {
        pFb->enable_extra |= 4;
        std3D_generateIntermediateFbo(Window_xSize, Window_ySize, &pFb->grade, 1);
    }

    // Added (remaster): low-res blurred copies of the scene. They light the haze and give
    // rough (glossy) reflections something blurry to reflect.
    if (jkPlayer_bHaze || jkPlayer_bReflections)
    {
        pFb->enable_extra |= 8;
        std3D_generateIntermediateFbo(width / 4, height / 4, &pFb->hazeBlur1, 1);
        std3D_generateIntermediateFbo(width / 16, height / 16, &pFb->hazeBlur2, 1);
    }

    pFb->main.fbo = pFb->fbo;
    pFb->main.tex = pFb->tex1;
    pFb->main.rbo = pFb->rbo;
    pFb->main.w = pFb->w;
    pFb->main.h = pFb->h;
    pFb->main.iw = pFb->w;
    pFb->main.ih = pFb->h;

    pFb->window.fbo = std3D_windowFbo;
    pFb->window.w = Window_xSize;
    pFb->window.h = Window_ySize;
    pFb->window.iw = Window_xSize;
    pFb->window.ih = Window_ySize;
}

void std3D_deleteFramebuffer(std3DFramebuffer* pFb)
{
    glDeleteFramebuffers(1, &pFb->fbo);
    glDeleteTextures(1, &pFb->tex0);
    glDeleteTextures(1, &pFb->tex1);
    glDeleteTextures(1, &pFb->tex2);
    glDeleteTextures(1, &pFb->tex3);
    glDeleteTextures(1, &pFb->tex4); // Added (remaster)
    glDeleteTextures(1, &pFb->tex5);
    glDeleteRenderbuffers(1, &pFb->rbo);

    std3D_deleteIntermediateFbo(&pFb->blur1);
    std3D_deleteIntermediateFbo(&pFb->blur2);
    std3D_deleteIntermediateFbo(&pFb->blur3);
    std3D_deleteIntermediateFbo(&pFb->blur4);
    //std3D_deleteIntermediateFbo(&pFb->blurBlend);

    std3D_deleteIntermediateFbo(&pFb->ssaoBlur1);
    std3D_deleteIntermediateFbo(&pFb->ssaoBlur2);
    //std3D_deleteIntermediateFbo(&pFb->ssaoBlur3);

    std3D_deleteIntermediateFbo(&pFb->grade); // Added (remaster)
    std3D_deleteIntermediateFbo(&pFb->hazeBlur1);
    std3D_deleteIntermediateFbo(&pFb->hazeBlur2);
}

void std3D_swapFramebuffers()
{
    if (std3D_activeFb == 2)
    {
        std3D_activeFb = 1;
        std3D_pFb = &std3D_framebuffers[0];
    }
    else
    {
        std3D_activeFb = 2;
        std3D_pFb = &std3D_framebuffers[1];
    }
}

GLuint std3D_loadProgram(const char* fpath_base)
{
    GLuint out;
    GLint link_ok = GL_FALSE;
    
    char* tmp_vert = (char*)malloc(strlen(fpath_base) + 32);
    char* tmp_frag = (char*)malloc(strlen(fpath_base) + 32);
    
    strcpy(tmp_vert, fpath_base);
    strcat(tmp_vert, "_v.glsl");
    
    strcpy(tmp_frag, fpath_base);
    strcat(tmp_frag, "_f.glsl");
    
    GLuint vs, fs;
    if ((vs = load_shader_file(tmp_vert, GL_VERTEX_SHADER))   == 0) return 0;
    if ((fs = load_shader_file(tmp_frag, GL_FRAGMENT_SHADER)) == 0) return 0;
    
    free(tmp_vert);
    free(tmp_frag);
    
    out = glCreateProgram();
    glAttachShader(out, vs);
    glAttachShader(out, fs);
    glLinkProgram(out);
    glGetProgramiv(out, GL_LINK_STATUS, &link_ok);
    if (!link_ok) 
    {
        print_log(out);
        return 0;
    }
    
    return out;
}

GLint std3D_tryFindAttribute(GLuint program, const char* attribute_name)
{
    GLint out = glGetAttribLocation(program, attribute_name);
    if (out == -1) {
        stdPlatform_Printf("std3D: Could not bind attribute %s!\n", attribute_name);
    }
    return out;
}

GLint std3D_tryFindUniform(GLuint program, const char* uniform_name)
{
    GLint out = glGetUniformLocation(program, uniform_name);
    if (out == -1) {
        stdPlatform_Printf("std3D: Could not bind uniform %s!\n", uniform_name);
    }
    return out;
}

bool std3D_loadSimpleTexProgram(const char* fpath_base, std3DSimpleTexStage* pOut)
{
    if (!pOut) return false;
    if ((pOut->program = std3D_loadProgram(fpath_base)) == 0) return false;
    
    pOut->attribute_coord3d = std3D_tryFindAttribute(pOut->program, "coord3d");
    pOut->attribute_v_color = std3D_tryFindAttribute(pOut->program, "v_color");
    pOut->attribute_v_uv = std3D_tryFindAttribute(pOut->program, "v_uv");
    pOut->uniform_mvp = std3D_tryFindUniform(pOut->program, "mvp");
    pOut->uniform_iResolution = std3D_tryFindUniform(pOut->program, "iResolution");
    pOut->uniform_tex = std3D_tryFindUniform(pOut->program, "tex");
    pOut->uniform_tex2 = std3D_tryFindUniform(pOut->program, "tex2");
    pOut->uniform_tex3 = std3D_tryFindUniform(pOut->program, "tex3");

    pOut->uniform_param1 = std3D_tryFindUniform(pOut->program, "param1");
    pOut->uniform_param2 = std3D_tryFindUniform(pOut->program, "param2");
    pOut->uniform_param3 = std3D_tryFindUniform(pOut->program, "param3");

    return true;
}

int init_resources()
{
    stdPlatform_Printf("std3D: OpenGL init...\n");

    std3D_bReinitHudElements = 1;

    memset(std3D_aUITextures, 0, sizeof(std3D_aUITextures));

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &std3D_windowFbo);


    int32_t tex_w = Window_xSize;
    int32_t tex_h = Window_ySize;

    std3D_generateFramebuffer(tex_w, tex_h, &std3D_framebuffers[0]);
    std3D_generateFramebuffer(tex_w, tex_h, &std3D_framebuffers[1]);

    std3D_activeFb = 1;
    std3D_pFb = &std3D_framebuffers[0];
    
    if ((programDefault = std3D_loadProgram("shaders/default")) == 0) return false;
    if ((programMenu = std3D_loadProgram("shaders/menu")) == 0) return false;
    if (!std3D_loadSimpleTexProgram("shaders/ui", &std3D_uiProgram)) return false;
    if (!std3D_loadSimpleTexProgram("shaders/texfbo", &std3D_texFboStage)) return false;
    if (!std3D_loadSimpleTexProgram("shaders/blur", &std3D_blurStage)) return false;
    if (!std3D_loadSimpleTexProgram("shaders/ssao", &std3D_ssaoStage)) return false;
    if (!std3D_loadSimpleTexProgram("shaders/ssao_mix", &std3D_ssaoMixStage)) return false;
    // Added (remaster): colour grading pass
    if (!std3D_loadSimpleTexProgram("shaders/grade", &std3D_gradeStage)) return false;
    std3D_gradeUniformLut = std3D_tryFindUniform(std3D_gradeStage.program, "lut");
    std3D_gradeUniformLutSize = std3D_tryFindUniform(std3D_gradeStage.program, "lutSize");
    std3D_gradeUniformDomainMin = std3D_tryFindUniform(std3D_gradeStage.program, "lutDomainMin");
    std3D_gradeUniformDomainMax = std3D_tryFindUniform(std3D_gradeStage.program, "lutDomainMax");
    // Added (remaster): screen-space reflections
    if (!std3D_loadSimpleTexProgram("shaders/ssr", &std3D_ssrStage)) return false;
    std3D_ssrUniformProj = std3D_tryFindUniform(std3D_ssrStage.program, "u_proj");
    std3D_ssrUniformScreen = std3D_tryFindUniform(std3D_ssrStage.program, "u_screen");
    std3D_ssrUniformUpCam = std3D_tryFindUniform(std3D_ssrStage.program, "u_upCam");
    std3D_ssrUniformWetness = std3D_tryFindUniform(std3D_ssrStage.program, "u_wetness");
    std3D_ssrUniformStrength = std3D_tryFindUniform(std3D_ssrStage.program, "u_strength");
    std3D_ssrUniformRoughness = std3D_tryFindUniform(std3D_ssrStage.program, "u_roughness");
    std3D_ssrUniformTexMat = std3D_tryFindUniform(std3D_ssrStage.program, "texMat");
    // Added (remaster): atmosphere
    if (!std3D_loadSimpleTexProgram("shaders/haze", &std3D_hazeStage)) return false;
    // Added (remaster): volumetric fog. Not fatal if it fails: haze is the fallback.
    if (std3D_loadSimpleTexProgram("shaders/fog", &std3D_fogStage))
    {
        static const char* names[] = {"u_fine", "u_coarse", "u_fineOrigin", "u_fineSize", "u_coarseOrigin", "u_coarseSize",
                                      "u_proj", "u_screen", "u_WX", "u_WY", "u_WZ", "u_camPos", "u_fog", "u_fog2", "u_hfog",
                                      "u_numVols", "u_volCenter", "u_volSize", "u_volParams", "u_volTint"};
        for (int k = 0; k < 20; k++) std3D_fogU[k] = std3D_tryFindUniform(std3D_fogStage.program, names[k]);
    }
    else std3D_fogStage.program = 0;
    if (!std3D_loadSimpleTexProgram("shaders/flare", &std3D_flareStage)) std3D_flareStage.program = 0; // Added (remaster)
    if (!std3D_loadSimpleTexProgram("shaders/shafts", &std3D_shaftStage)) return false;
    std3D_shaftUniformNum = std3D_tryFindUniform(std3D_shaftStage.program, "u_numShafts");
    std3D_shaftUniformPos = std3D_tryFindUniform(std3D_shaftStage.program, "u_shaftPos");
    std3D_shaftUniformCol = std3D_tryFindUniform(std3D_shaftStage.program, "u_shaftCol");
    // Added (remaster): volumetric sky panorama
    if (!std3D_loadSimpleTexProgram("shaders/sky", &std3D_skyStage)) return false;
    std3D_skyU_shape = std3D_tryFindUniform(std3D_skyStage.program, "u_shape");
    std3D_skyU_detail = std3D_tryFindUniform(std3D_skyStage.program, "u_detail");
    std3D_skyU_sunDir = std3D_tryFindUniform(std3D_skyStage.program, "u_sunDir");
    std3D_skyU_sunCol = std3D_tryFindUniform(std3D_skyStage.program, "u_sunCol");
    std3D_skyU_ambTop = std3D_tryFindUniform(std3D_skyStage.program, "u_ambTop");
    std3D_skyU_ambBottom = std3D_tryFindUniform(std3D_skyStage.program, "u_ambBottom");
    std3D_skyU_horizon = std3D_tryFindUniform(std3D_skyStage.program, "u_horizon");
    std3D_skyU_layer = std3D_tryFindUniform(std3D_skyStage.program, "u_layer");
    std3D_skyU_wind = std3D_tryFindUniform(std3D_skyStage.program, "u_wind");

    // Attributes/uniforms
    attribute_coord3d = std3D_tryFindAttribute(programDefault, "coord3d");
    attribute_v_color = std3D_tryFindAttribute(programDefault, "v_color");
    attribute_v_light = std3D_tryFindAttribute(programDefault, "v_light");
    attribute_v_uv = std3D_tryFindAttribute(programDefault, "v_uv");
    // Added (remaster): per-pixel lighting
    attribute_v_invz = std3D_tryFindAttribute(programDefault, "v_invz");
    uniform_ppl = std3D_tryFindUniform(programDefault, "u_ppl");
    uniform_pplNumLights = std3D_tryFindUniform(programDefault, "u_pplNumLights");
    uniform_pplLightPos = std3D_tryFindUniform(programDefault, "u_pplLightPos");
    uniform_pplLightCol = std3D_tryFindUniform(programDefault, "u_pplLightCol");
    uniform_pplAtten = std3D_tryFindUniform(programDefault, "u_pplAtten");
    uniform_pplProj = std3D_tryFindUniform(programDefault, "u_pplProj");
    uniform_pplScreen = std3D_tryFindUniform(programDefault, "u_pplScreen");
    uniform_pplSpecular = std3D_tryFindUniform(programDefault, "u_pplSpecular");
    uniform_fixtureBloom = std3D_tryFindUniform(programDefault, "u_fixtureBloom");
    uniform_pplWetness = std3D_tryFindUniform(programDefault, "u_pplWetness");
    uniform_pplNumGlow = std3D_tryFindUniform(programDefault, "u_pplNumGlow");
    uniform_pplGlowPos = std3D_tryFindUniform(programDefault, "u_pplGlowPos");
    uniform_pplGlowCol = std3D_tryFindUniform(programDefault, "u_pplGlowCol");
    uniform_ssrWrite = std3D_tryFindUniform(programDefault, "u_ssrWrite");
    uniform_pplUpCam = std3D_tryFindUniform(programDefault, "u_pplUpCam");
    uniform_texOrm = std3D_tryFindUniform(programDefault, "texOrm");
    uniform_texNormal = std3D_tryFindUniform(programDefault, "texNormal");
    uniform_hasOrm = std3D_tryFindUniform(programDefault, "u_hasOrm");
    uniform_hasNormal = std3D_tryFindUniform(programDefault, "u_hasNormal");
    uniform_mvp = std3D_tryFindUniform(programDefault, "mvp");
    uniform_tex = std3D_tryFindUniform(programDefault, "tex");
    uniform_texEmiss = std3D_tryFindUniform(programDefault, "texEmiss");
    uniform_worldPalette = std3D_tryFindUniform(programDefault, "worldPalette");
    uniform_worldPaletteLights = std3D_tryFindUniform(programDefault, "worldPaletteLights");
    uniform_displacement_map = std3D_tryFindUniform(programDefault, "displacement_map");
    uniform_tex_mode = std3D_tryFindUniform(programDefault, "tex_mode");
    uniform_blend_mode = std3D_tryFindUniform(programDefault, "blend_mode");
    uniform_tint = std3D_tryFindUniform(programDefault, "colorEffects_tint");
    uniform_filter = std3D_tryFindUniform(programDefault, "colorEffects_filter");
    uniform_fade = std3D_tryFindUniform(programDefault, "colorEffects_fade");
    uniform_add = std3D_tryFindUniform(programDefault, "colorEffects_add");
    uniform_emissiveFactor = std3D_tryFindUniform(programDefault, "emissiveFactor");
    uniform_albedoFactor = std3D_tryFindUniform(programDefault, "albedoFactor");
    uniform_light_mult = std3D_tryFindUniform(programDefault, "light_mult");
    uniform_displacement_factor = std3D_tryFindUniform(programDefault, "displacement_factor");
    uniform_iResolution = std3D_tryFindUniform(programDefault, "iResolution");

    uniform_saberRgbEnabled = std3D_tryFindUniform(
        programDefault, "saberRgbEnabled"
    );

    uniform_saberBlade = std3D_tryFindUniform(programDefault, "saberBlade");
    uniform_sky = std3D_tryFindUniform(programDefault, "u_sky"); // Added (remaster): volumetric sky
    uniform_model = std3D_tryFindUniform(programDefault, "u_model");
    uniform_cine = std3D_tryFindUniform(programDefault, "u_cine");
    attribute_v_lmSurf = std3D_tryFindAttribute(programDefault, "v_lmSurf"); // Added (remaster): light maps
    attribute_v_glow = std3D_tryFindAttribute(programDefault, "v_glow"); // Added (remaster): light probes
    uniform_lmOn = std3D_tryFindUniform(programDefault, "u_lmOn");
    uniform_lmAtlas = std3D_tryFindUniform(programDefault, "u_lmAtlas");
    uniform_lmData = std3D_tryFindUniform(programDefault, "u_lmData");
    uniform_lmParams = std3D_tryFindUniform(programDefault, "u_lmParams");
    uniform_lmCamPos = std3D_tryFindUniform(programDefault, "u_lmCamPos");
    uniform_skyPano = std3D_tryFindUniform(programDefault, "u_skyPano");
    uniform_skyWX = std3D_tryFindUniform(programDefault, "u_skyWX");
    uniform_skyWY = std3D_tryFindUniform(programDefault, "u_skyWY");
    uniform_skyWZ = std3D_tryFindUniform(programDefault, "u_skyWZ");
    uniform_skySunDir = std3D_tryFindUniform(programDefault, "u_skySunDir");
    uniform_skySunCol = std3D_tryFindUniform(programDefault, "u_skySunCol");
    uniform_skyZenith = std3D_tryFindUniform(programDefault, "u_skyZenith");
    uniform_skyHorizon = std3D_tryFindUniform(programDefault, "u_skyHorizon");
    uniform_skyBelow = std3D_tryFindUniform(programDefault, "u_skyBelowExp");
    uniform_skyFlash = std3D_tryFindUniform(programDefault, "u_skyFlash");
    uniform_saberStrength = std3D_tryFindUniform(programDefault, "saberStrength");
    uniform_time = std3D_tryFindUniform(programDefault, "u_time");
    uniform_saberRgbColor = std3D_tryFindUniform(
        programDefault, "saberRgbColor"
    );
    
    programMenu_attribute_coord3d = std3D_tryFindAttribute(programMenu, "coord3d");
    programMenu_attribute_v_color = std3D_tryFindAttribute(programMenu, "v_color");
    programMenu_attribute_v_uv = std3D_tryFindAttribute(programMenu, "v_uv");
    programMenu_uniform_mvp = std3D_tryFindUniform(programMenu, "mvp");
    programMenu_uniform_tex = std3D_tryFindUniform(programMenu, "tex");
    programMenu_uniform_displayPalette = std3D_tryFindUniform(programMenu, "displayPalette");
    
    // Blank texture
    glGenTextures(1, &blank_tex);
    blank_data = jkgm_alloc_aligned(0x400);
    memset(blank_data, 0x0, 0x400);
    
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 16, 16, 0, GL_RGB, GL_UNSIGNED_BYTE, blank_data);

    // Blank texture
    glGenTextures(1, &blank_tex_white);
    blank_data_white = jkgm_alloc_aligned(0x400);
    memset(blank_data_white, 0xFF, 0x400);
    
    glBindTexture(GL_TEXTURE_2D, blank_tex_white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 16, 16, 0, GL_RGB, GL_UNSIGNED_BYTE, blank_data_white);

    // World palette
    glGenTextures(1, &worldpal_texture);
    worldpal_data = jkgm_alloc_aligned(0x300);
    memset(worldpal_data, 0xFF, 0x300);
    
    glBindTexture(GL_TEXTURE_2D, worldpal_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    //glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    //glPixelStorei(GL_PACK_ALIGNMENT, 1);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, worldpal_data);

    // World palette aLights
    glGenTextures(1, &worldpal_lights_texture);
    worldpal_lights_data = jkgm_alloc_aligned(0x4000);
    memset(worldpal_lights_data, 0xFF, 0x4000);
    
    glBindTexture(GL_TEXTURE_2D, worldpal_lights_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    //glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    //glPixelStorei(GL_PACK_ALIGNMENT, 1);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 0x40, 0, GL_RED, GL_UNSIGNED_BYTE, worldpal_lights_data);
    
    
    // Display palette
    glGenTextures(1, &displaypal_texture);
    displaypal_data = jkgm_alloc_aligned(0x400);
    memset(displaypal_data, 0xFF, 0x300);
    
    glBindTexture(GL_TEXTURE_2D, displaypal_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    //glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    //glPixelStorei(GL_PACK_ALIGNMENT, 1);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, displaypal_data);

    // Tiled random
    // FLEXTODO
    glGenTextures(1, &tiledrand_texture);
    if (tiledrand_data) {
        free(tiledrand_data);
    }
    tiledrand_data = (rdVector3*)malloc(3 * 4 * 4 * sizeof(float));
    memset(tiledrand_data, 0, 3 * 4 * 4 * sizeof(float));

    for (int i = 0; i < 4*4; i++)
    {
        tiledrand_data[i].x = (_frand() * 2.0) - 1.0;
        tiledrand_data[i].y = (_frand() * 2.0) - 1.0;
        tiledrand_data[i].z = 0.0;
        rdVector_Normalize3Acc(&tiledrand_data[i]);
    }

    glBindTexture(GL_TEXTURE_2D, tiledrand_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, 4, 4, 0, GL_RGB, GL_FLOAT, tiledrand_data);

    unsigned int vao;
    glGenVertexArrays( 1, &vao );
    glBindVertexArray( vao ); 

    world_data_all = (D3DVERTEX*)malloc(STD3D_MAX_VERTICES * sizeof(D3DVERTEX));
    world_data_elements = (GLushort*)malloc(sizeof(GLushort) * 3 * STD3D_MAX_TRIS);

    menu_data_all = (D3DVERTEX*)malloc(STD3D_MAX_UI_VERTICES * sizeof(D3DVERTEX));
    menu_data_elements = (GLushort*)malloc(sizeof(GLushort) * 3 * STD3D_MAX_UI_TRIS);

    glGenBuffers(1, &world_vbo_all);
    glGenBuffers(1, &world_ibo_triangle);

    glGenBuffers(1, &menu_vbo_all);
    glGenBuffers(1, &menu_ibo_triangle);

    has_initted = true;
    return true;
}

// Added (remaster): mipmaps and anisotropic filtering for true-colour textures (16-bit
// MATs and HD jkgm materials). Call with the texture bound. bGenMips builds the mip
// chain after the level 0 upload, so filtering can be toggled later without re-uploading.
// Filtering off keeps the original crisp nearest-neighbour look.
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
void std3D_SetTrueColorFiltering(int bFilter, int bGenMips)
{
    static float maxAniso = -1.0f;
    if (maxAniso < 0.0f) {
        GLfloat v = 1.0f;
        while (glGetError() != GL_NO_ERROR) {}
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &v);
        maxAniso = (glGetError() == GL_NO_ERROR && v > 1.0f) ? (v > 16.0f ? 16.0f : v) : 1.0f;
        stdPlatform_Printf("std3D: anisotropic filtering max %.0fx\n", maxAniso);
    }
    // r_anisotropy (default 8) caps it: 16x cost real time with HD textures for little gain.
    float aniso = maxAniso;
    if (jkPlayer_anisotropy >= 1 && jkPlayer_anisotropy < aniso) aniso = (float)jkPlayer_anisotropy;

    if (bGenMips) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, bFilter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, bFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
    if (maxAniso > 1.0f)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, bFilter ? aniso : 1.0f);
}

int std3D_Startup()
{
    if (std3D_bInitted) {
        return 1;
    }

#ifdef TARGET_CAN_JKGM
    jkgm_startup();
#endif

    memset(&std3D_ui_colormap, 0, sizeof(std3D_ui_colormap));
    rdColormap_LoadEntry("misc\\cmp\\UIColormap.cmp", &std3D_ui_colormap);

    std3D_bReinitHudElements = 1;

    std3D_bInitted = 1;
    return 1;
}

void std3D_Shutdown()
{
    if (!std3D_bInitted) {
        return;
    }

    std3D_bReinitHudElements = 0;

    rdColormap_FreeEntry(&std3D_ui_colormap);
    std3D_bInitted = 0;
}

void std3D_FreeResources()
{
    std3D_PurgeEntireTextureCache();

    glDeleteProgram(programDefault);
    glDeleteProgram(programMenu);
    // Added (remaster): grading pass. The LUT is reloaded on next use.
    glDeleteProgram(std3D_gradeStage.program);
    glDeleteProgram(std3D_ssrStage.program); // Added (remaster)
    glDeleteProgram(std3D_hazeStage.program);
    glDeleteProgram(std3D_shaftStage.program);
    if (std3D_gradeLutTex) glDeleteTextures(1, &std3D_gradeLutTex);
    std3D_gradeLutTex = 0;
    std3D_gradeLutSize = 0;
    std3D_deleteFramebuffer(&std3D_framebuffers[0]);
    std3D_deleteFramebuffer(&std3D_framebuffers[1]);
    glDeleteTextures(1, &blank_tex);
    glDeleteTextures(1, &blank_tex_white);
    glDeleteTextures(1, &worldpal_texture);
    glDeleteTextures(1, &worldpal_lights_texture);
    glDeleteTextures(1, &displaypal_texture);
    if (blank_data)
        jkgm_aligned_free(blank_data);
    if (blank_data_white)
        jkgm_aligned_free(blank_data_white);
    if (worldpal_data)
        jkgm_aligned_free(worldpal_data);
    if (worldpal_lights_data)
        jkgm_aligned_free(worldpal_lights_data);
    if (displaypal_data)
        jkgm_aligned_free(displaypal_data);

    blank_data = NULL;
    blank_data_white = NULL;
    worldpal_data = NULL;
    worldpal_lights_data = NULL;
    displaypal_data = NULL;

    if (world_data_all)
        free(world_data_all);
    world_data_all = NULL;

    if (world_data_elements)
        free(world_data_elements);
    world_data_elements = NULL;

    if (menu_data_all)
        free(menu_data_all);
    menu_data_all = NULL;

    if (menu_data_elements)
        free(menu_data_elements);
    menu_data_elements = NULL;

    loaded_colormap = NULL;

    glDeleteBuffers(1, &world_vbo_all);
    glDeleteBuffers(1, &world_ibo_triangle);

    glDeleteBuffers(1, &menu_vbo_all);

    std3D_bReinitHudElements = 1;

    has_initted = false;
}

// Added (remaster): volumetric sky. Builds the noise textures and panorama target on first
// use, then raymarches one slice of panorama columns (all of them on a new level).
static GLuint std3D_SkyMake3D(const uint8_t* pData, int n)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_3D, t);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, n, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, pData);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
    glGenerateMipmap(GL_TEXTURE_3D);
    glBindTexture(GL_TEXTURE_3D, 0);
    return t;
}

void std3D_DrawSimpleTex(std3DSimpleTexStage* pStage, std3DIntermediateFbo* pFbo, GLuint texId, GLuint texId2, GLuint texId3, flex_t param1, flex_t param2, flex_t param3, int gen_mips);

static void std3D_SkyUpdatePanorama(void)
{
    const sithSkyPreset* p = sithSky_GetActive();
    if (!p || !std3D_skyStage.program || jkGuiBuildMulti_bRendering) return;
    if (!std3D_skyShapeTex)
    {
        int n = 0;
        const uint8_t* d = sithSky_GetShapeNoise(&n);
        if (!d) return;
        std3D_skyShapeTex = std3D_SkyMake3D(d, n);
        d = sithSky_GetDetailNoise(&n);
        if (d) std3D_skyDetailTex = std3D_SkyMake3D(d, n);
    }
    if (!std3D_skyPanoReady)
    {
        std3D_generateIntermediateFbo(STD3D_SKY_PANO_W, STD3D_SKY_PANO_H, &std3D_skyPano, 1);
        glBindTexture(GL_TEXTURE_2D, std3D_skyPano.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        std3D_skyPanoReady = 1;
    }
    if (p != std3D_skyPanoPreset) { std3D_skyPanoPreset = p; std3D_skyFilled = 0; }

    float sun[3];
    sithSky_GetSunDir(p, sun);
    double t = (double)SDL_GetTicksNS() / 1e9;
    glUseProgram(std3D_skyStage.program);
    glUniform1i(std3D_skyU_shape, 3);
    glUniform1i(std3D_skyU_detail, 4);
    glUniform3fv(std3D_skyU_sunDir, 1, sun);
    glUniform3f(std3D_skyU_sunCol, p->sunColor[0] * p->sunIntensity, p->sunColor[1] * p->sunIntensity, p->sunColor[2] * p->sunIntensity);
    glUniform3fv(std3D_skyU_ambTop, 1, p->ambientTop);
    glUniform3fv(std3D_skyU_ambBottom, 1, p->ambientBottom);
    glUniform3fv(std3D_skyU_horizon, 1, p->horizon);
    glUniform4f(std3D_skyU_layer, p->cloudBase, p->cloudTop, sithSky_GetCoverage(p), p->density);
    glUniform2f(std3D_skyU_wind, (float)fmod(t * p->wind, 360.0), (float)fmod(t * p->wind * 0.35, 360.0));
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_3D, std3D_skyShapeTex);
    glActiveTexture(GL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_3D, std3D_skyDetailTex);
    glActiveTexture(GL_TEXTURE0);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    // The whole panorama on a new level/preset, otherwise one slice of columns per frame.
    if (std3D_skyFilled)
    {
        int w = STD3D_SKY_PANO_W / STD3D_SKY_SLICES;
        glEnable(GL_SCISSOR_TEST);
        glScissor(std3D_skySlice * w, 0, w, STD3D_SKY_PANO_H);
        std3D_skySlice = (std3D_skySlice + 1) % STD3D_SKY_SLICES;
    }
    std3D_DrawSimpleTex(&std3D_skyStage, &std3D_skyPano, blank_tex, 0, 0, (float)fmod(t, 3600.0), 0.0, 0.0, 0);
    glDisable(GL_SCISSOR_TEST);
    {
        // Debug: OPENJKDF2_SKY_DEBUG=1 prints the panorama's value range after the first fill.
        static int dbg = -1;
        if (dbg < 0) { const char* e = getenv("OPENJKDF2_SKY_DEBUG"); dbg = (e && e[0] == '1'); }
        if (dbg && !std3D_skyFilled) {
            static float px[STD3D_SKY_PANO_W * 4];
            glBindFramebuffer(GL_FRAMEBUFFER, std3D_skyPano.fbo);
            for (int row = 0; row < STD3D_SKY_PANO_H; row += STD3D_SKY_PANO_H / 8) {
                glReadPixels(0, row, STD3D_SKY_PANO_W, 1, GL_RGBA, GL_FLOAT, px);
                float mn[4] = {1e9, 1e9, 1e9, 1e9}, mx[4] = {-1e9, -1e9, -1e9, -1e9}, meanT = 0.0f;
                for (int i = 0; i < STD3D_SKY_PANO_W; i++) { meanT += px[i*4+3]; for (int c = 0; c < 4; c++) {
                    float v = px[i*4+c]; if (v < mn[c]) mn[c] = v; if (v > mx[c]) mx[c] = v; } }
                stdPlatform_Printf("sky pano row %4d: r %.3f..%.3f g %.3f..%.3f b %.3f..%.3f T %.3f..%.3f mean %.2f\n", row, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], mn[3], mx[3], meanT / STD3D_SKY_PANO_W);
            }
            // OPENJKDF2_SKY_DUMP=path.ppm: the whole panorama (clouds over black, x exposure).
            const char* dump = getenv("OPENJKDF2_SKY_DUMP");
            FILE* f = dump ? fopen(dump, "wb") : NULL;
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", STD3D_SKY_PANO_W / 2, STD3D_SKY_PANO_H / 2);
                static float rowpx[STD3D_SKY_PANO_W * 4];
                static unsigned char out[STD3D_SKY_PANO_W / 2 * 3];
                for (int row = STD3D_SKY_PANO_H - 2; row >= 0; row -= 2) {
                    glReadPixels(0, row, STD3D_SKY_PANO_W, 1, GL_RGBA, GL_FLOAT, rowpx);
                    for (int i = 0; i < STD3D_SKY_PANO_W / 2; i++) for (int c = 0; c < 3; c++) {
                        float v = rowpx[i*8+c] * p->exposure + 0.25f * rowpx[i*8+3]; // clear sky shows as dark grey
                        v = v / (1.0f + v);
                        out[i*3+c] = (unsigned char)(v * 255.0f);
                    }
                    fwrite(out, 1, sizeof(out), f);
                }
                fclose(f);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
    }
    std3D_skyFilled = 1;

    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_3D, 0);
    glActiveTexture(GL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_3D, 0);
    glActiveTexture(GL_TEXTURE0);
}

// Added (remaster): baked light maps. tools/blender/lightmap_sidecar.py writes
// jkgm/lightmaps/<level>.jklm next to the HD packs: an RGB half-float atlas of baked light and,
// per world surface, how world positions map into it. Level faces carry their surface number to
// the shader (rdCache puts it in the vertex's spare ny), the shader rebuilds each pixel's world
// position and uses the baked light in place of the 1997 corner light. Loaded when a level
// starts; a missing file or one baked for a different version of the level means the original
// light.
#define STD3D_LM_PER_ROW 1024
static GLuint std3D_lmAtlas = 0, std3D_lmData = 0;
static int std3D_lmReady = 0, std3D_lmWorldSurfaces = -1;
static SithWorld* std3D_lmWorld = NULL;
static char std3D_lmWorldName[32];
static float std3D_lmFill = 0.0f, std3D_lmGain = 1.0f;

static void std3D_LightmapFree(void)
{
    if (std3D_lmAtlas) glDeleteTextures(1, &std3D_lmAtlas);
    if (std3D_lmData) glDeleteTextures(1, &std3D_lmData);
    std3D_lmAtlas = std3D_lmData = 0;
    std3D_lmReady = 0;
}

static void std3D_LightmapLoad(SithWorld* w)
{
    char name[64], path[128];
    int i;
    for (i = 0; i < 63 && w->map_jkl_fname[i] && w->map_jkl_fname[i] != '.'; i++)
        name[i] = (char)tolower((unsigned char)w->map_jkl_fname[i]);
    name[i] = 0;
    snprintf(path, sizeof(path), "jkgm/lightmaps/%s.jklm", name);
    FILE* f = fopen(path, "rb");
    if (!f) return;

    char magic[4];
    uint32_t hdr[5];
    float fg[2];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "JKLM", 4) || fread(hdr, 4, 5, f) != 5 || fread(fg, 4, 2, f) != 2
        || hdr[0] != 1) {
        stdPlatform_Printf("lightmap: %s is not a version 1 light map, ignored\n", path);
        fclose(f);
        return;
    }
    uint32_t numVerts = hdr[1], n = hdr[2], W = hdr[3], H = hdr[4];
    if ((int)numVerts != w->numVertices || n == 0 || (int)n > w->numSurfaces || W == 0 || H == 0 || W > 16384 || H > 16384) {
        stdPlatform_Printf("lightmap: %s was baked for a different version of the level (%u verts, %u surfaces; level has %d, %d), ignored\n",
                           path, numVerts, n, w->numVertices, w->numSurfaces);
        fclose(f);
        return;
    }
    // Surface table: 3 RGBA32F texels per surface, STD3D_LM_PER_ROW surfaces per row.
    uint32_t rows = (n + STD3D_LM_PER_ROW - 1) / STD3D_LM_PER_ROW;
    float* rec = (float*)calloc((size_t)rows * STD3D_LM_PER_ROW * 12, sizeof(float));
    uint16_t* px = (uint16_t*)malloc((size_t)W * H * 3 * sizeof(uint16_t));
    int ok = rec && px && fread(rec, sizeof(float) * 12, n, f) == n && fread(px, (size_t)W * 6, H, f) == H;
    fclose(f);
    if (!ok) {
        stdPlatform_Printf("lightmap: %s is truncated, ignored\n", path);
        free(rec);
        free(px);
        return;
    }

    glGenTextures(1, &std3D_lmData);
    glBindTexture(GL_TEXTURE_2D, std3D_lmData);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, STD3D_LM_PER_ROW * 3, rows, 0, GL_RGBA, GL_FLOAT, rec);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &std3D_lmAtlas);
    glBindTexture(GL_TEXTURE_2D, std3D_lmAtlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, W, H, 0, GL_RGB, GL_HALF_FLOAT, px);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(rec);
    free(px);

    std3D_lmFill = fg[0];
    std3D_lmGain = fg[1];
    std3D_lmReady = 1;
    stdPlatform_Printf("lightmap: loaded %s (%ux%u, %u surfaces, fill %.2f)\n", path, W, H, n, std3D_lmFill);
}

// The current level's light map, loading it when the level changes. 1 if there is one.
static int std3D_LightmapUpdate(void)
{
    SithWorld* w = sithWorld_g_pCurrentWorld;
    if (!jkPlayer_bLightmaps || !w) return 0;
    // Key on the name and surface count too: a new level can reuse the old one's memory.
    if (w != std3D_lmWorld || w->numSurfaces != std3D_lmWorldSurfaces
        || strncmp(w->map_jkl_fname, std3D_lmWorldName, sizeof(std3D_lmWorldName)))
    {
        std3D_lmWorld = w;
        std3D_lmWorldSurfaces = w->numSurfaces;
        strncpy(std3D_lmWorldName, w->map_jkl_fname, sizeof(std3D_lmWorldName));
        std3D_LightmapFree();
        std3D_LightmapLoad(w);
    }
    return std3D_lmReady;
}

int std3D_StartScene()
{
    if (Main_bHeadless) return 1;

    ++std3D_frameCount;

    //printf("Begin draw\n");
    if (!has_initted)
    {
        if (!init_resources()) {
            stdPlatform_Printf("std3D: Failed to init resources, exiting...");
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "Failed to init resources, exiting...", NULL);
            exit(-1);
        }
    }
    
    rendered_tris = 0;
    
    std3D_swapFramebuffers();
    
    double supersample_level = jkPlayer_ssaaMultiple; // Can also be set lower
    // Added (remaster): on very large displays (e.g. a 7680x3240 ultrawide, 25 megapixels) render
    // the 3D scene at a scale that keeps it near a pixel budget, then upscale; the 1997 textures
    // are so magnified there that it costs no visible sharpness. MacBook-size screens are under
    // the budget and stay native. r_autoRenderScale 0 = always use r_ssaaMultiple as is.
    if (jkPlayer_bAutoRenderScale && Window_xSize > 0 && Window_ySize > 0) {
        double px = (double)Window_xSize * Window_ySize * supersample_level * supersample_level;
        const double budget = 14.0e6;
        if (px > budget) supersample_level *= sqrt(budget / px);
    }
    int32_t tex_w = (int32_t)((double)Window_xSize * supersample_level);
    int32_t tex_h = (int32_t)((double)Window_ySize * supersample_level);

    if (tex_w != std3D_pFb->w || tex_h != std3D_pFb->h 
        || (!(std3D_pFb->enable_extra & 1) && jkPlayer_enableBloom)
        || (!(std3D_pFb->enable_extra & 2) && jkPlayer_enableSSAO)
        || (!!(std3D_pFb->enable_extra & 4) != !!jkPlayer_enableGrade) // Added (remaster)
        || (!!(std3D_pFb->enable_extra & 8) != !!(jkPlayer_bHaze || jkPlayer_bReflections))
        || (jkPlayer_enableGrade && (std3D_pFb->grade.w != Window_xSize || std3D_pFb->grade.h != Window_ySize)))
    {
        std3D_deleteFramebuffer(std3D_pFb);
        std3D_generateFramebuffer(tex_w, tex_h, std3D_pFb);
    }

    std3D_SkyUpdatePanorama(); // Added (remaster): volumetric sky, before the scene is bound

    glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->fbo);
    std3D_SetSceneDrawBuffers(); // so the clear below covers tex2-3 when SSAO was just turned on
    glEnable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glDepthFunc(GL_LESS);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    glCullFace(GL_FRONT);
    //glClampColor(GL_CLAMP_FRAGMENT_COLOR, GL_FALSE);

    // Technically this should be from Clear2
    glClearColor(0.0, 0.0, 0.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    
    if (jkGuiBuildMulti_bRendering && rdColormap_pCurMap && loaded_colormap != rdColormap_pCurMap)
    {
        glBindTexture(GL_TEXTURE_2D, worldpal_texture);
        memcpy(worldpal_data, rdColormap_pCurMap->colors, 0x300);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGB, GL_UNSIGNED_BYTE, worldpal_data);
    
        if (rdColormap_pCurMap->lightlevel)
        {
            glBindTexture(GL_TEXTURE_2D, worldpal_lights_texture);
            memcpy(worldpal_lights_data, rdColormap_pCurMap->lightlevel, 0x4000);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 0x40, GL_RED, GL_UNSIGNED_BYTE, worldpal_lights_data);
        }

        loaded_colormap = rdColormap_pCurMap;
    }
    else if (sithWorld_g_pCurrentWorld && sithWorld_g_pCurrentWorld->colormaps && loaded_colormap != sithWorld_g_pCurrentWorld->colormaps)
    {
        glBindTexture(GL_TEXTURE_2D, worldpal_texture);
        memcpy(worldpal_data, sithWorld_g_pCurrentWorld->colormaps->colors, 0x300);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGB, GL_UNSIGNED_BYTE, worldpal_data);
    
        if (sithWorld_g_pCurrentWorld->colormaps->lightlevel)
        {
            glBindTexture(GL_TEXTURE_2D, worldpal_lights_texture);
            memcpy(worldpal_lights_data, sithWorld_g_pCurrentWorld->colormaps->lightlevel, 0x4000);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 0x40, GL_RED, GL_UNSIGNED_BYTE, worldpal_lights_data);
        }

        loaded_colormap = sithWorld_g_pCurrentWorld->colormaps;
    }

    if (memcmp(displaypal_data, stdDisplay_masterPalette, 0x300))
    {
        glBindTexture(GL_TEXTURE_2D, displaypal_texture);
        memcpy(displaypal_data, stdDisplay_masterPalette, 0x300);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGB, GL_UNSIGNED_BYTE, displaypal_data);
    }

#if 0
    // New random values
    glBindTexture(GL_TEXTURE_2D, tiledrand_texture);
    for (int i = 0; i < 4*4; i++)
    {
        tiledrand_data[i].x = (_frand() * 2.0) - 1.0;
        tiledrand_data[i].y = (_frand() * 2.0) - 1.0;
        tiledrand_data[i].z = 0.0;
        rdVector_Normalize3Acc(&tiledrand_data[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGB, GL_FLOAT, tiledrand_data);
    }
#endif

    // Describe our aVertices array to OpenGL (it can't guess its format automatically)
    glBindBuffer(GL_ARRAY_BUFFER, world_vbo_all);
    glBufferData(GL_ARRAY_BUFFER, 1 * sizeof(D3DVERTEX), GL_tmpVertices, GL_STREAM_DRAW);
    glVertexAttribPointer(
        attribute_coord3d, // attribute
        3,                 // number of elements per vertex, here (x,y,z)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // data stride
        (GLvoid*)offsetof(D3DVERTEX, x)                  // offset of first element
    );
    
    glVertexAttribPointer(
        attribute_v_color, // attribute
        4,                 // number of elements per vertex, here (R,G,B,A)
        GL_UNSIGNED_BYTE,  // the type of each element
        GL_TRUE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, color) // offset of first element
    );

    glVertexAttribPointer(
        attribute_v_light, // attribute
        1,                 // number of elements per vertex, here (L)
        GL_FLOAT,  // the type of each element
        GL_FALSE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, lightLevel) // offset of first element
    );

    glVertexAttribPointer(
        attribute_v_uv,    // attribute
        2,                 // number of elements per vertex, here (U,V)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // take our values as-is
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, tu)                  // offset of first element
    );

    glEnableVertexAttribArray(attribute_coord3d);
    glEnableVertexAttribArray(attribute_v_color);
    glEnableVertexAttribArray(attribute_v_light);
    glEnableVertexAttribArray(attribute_v_uv);

    // Added (remaster): per-pixel lighting depth input
    if (attribute_v_invz >= 0) {
        glVertexAttribPointer(attribute_v_invz, 1, GL_FLOAT, GL_FALSE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, nx));
        glEnableVertexAttribArray(attribute_v_invz);
    }
    // Added (remaster): light map surface (+1, 0 = none) rides in the vertex's spare ny.
    if (attribute_v_glow >= 0) {
        glVertexAttribPointer(attribute_v_glow, 1, GL_FLOAT, GL_FALSE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, nz));
        glEnableVertexAttribArray(attribute_v_glow);
    }
    if (attribute_v_lmSurf >= 0) {
        glVertexAttribPointer(attribute_v_lmSurf, 1, GL_FLOAT, GL_FALSE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, ny));
        glEnableVertexAttribArray(attribute_v_lmSurf);
    }
    
    return 1;
}

int std3D_EndScene()
{
    if (Main_bHeadless) {
        last_tex = NULL;
        last_flags = 0;
        std3D_ResetRenderList();
        return 1;
    }

    glDisableVertexAttribArray(attribute_v_uv);
    if (attribute_v_invz >= 0) glDisableVertexAttribArray(attribute_v_invz); // Added (remaster)
    if (attribute_v_lmSurf >= 0) glDisableVertexAttribArray(attribute_v_lmSurf); // Added (remaster)
    if (attribute_v_glow >= 0) glDisableVertexAttribArray(attribute_v_glow); // Added (remaster)
    glDisableVertexAttribArray(attribute_v_color);
    glDisableVertexAttribArray(attribute_coord3d);

    //printf("End draw\n");
    last_tex = NULL;
    last_flags = 0;
    std3D_ResetRenderList();
    //printf("%u tris\n", rendered_tris);
    return 1;
}

void std3D_ResetUIRenderList()
{
    rendered_tris += GL_tmpUITrisAmt;

    GL_tmpUIVerticesAmt = 0;
    GL_tmpUITrisAmt = 0;
    //GL_tmpLinesAmt = 0;
    
    //memset(GL_tmpTris, 0, sizeof(GL_tmpTris));
    //memset(GL_tmpVertices, 0, sizeof(GL_tmpVertices));
}

void std3D_ResetRenderList()
{
    rendered_tris += GL_tmpTrisAmt;

    GL_tmpVerticesAmt = 0;
    GL_tmpTrisAmt = 0;
    GL_tmpLinesAmt = 0;
    
    //memset(GL_tmpTris, 0, sizeof(GL_tmpTris));
    //memset(GL_tmpVertices, 0, sizeof(GL_tmpVertices));
}

int std3D_RenderListVerticesFinish()
{
    return 1;
}

void std3D_DrawMenuSubrect(flex_t x, flex_t y, flex_t w, flex_t h, flex_t dstX, flex_t dstY, flex_t scale)
{
    //double tex_w = (double)Window_xSize;
    //double tex_h = (double)Window_ySize;
    double tex_w = Video_menuBuffer.format.width;
    double tex_h = Video_menuBuffer.format.height;

    float w_dst = w;
    float h_dst = h;

    if (scale == 0.0)
    {
        w_dst = (w / tex_w) * (double)Window_xSize;
        h_dst = (h / tex_h) * (double)Window_ySize;

        dstX = (dstX / tex_w) * (double)Window_xSize;
        dstY = (dstY / tex_h) * (double)Window_ySize;

        scale = 1.0;
    }

    double u1 = (x / tex_w);
    double u2 = ((x+w) / tex_w);
    double v1 = (y / tex_h);
    double v2 = ((y+h) / tex_h);

    GL_tmpVertices[GL_tmpVerticesAmt+0].x = dstX;
    GL_tmpVertices[GL_tmpVerticesAmt+0].y = dstY;
    GL_tmpVertices[GL_tmpVerticesAmt+0].z = 0.0;
    GL_tmpVertices[GL_tmpVerticesAmt+0].tu = u1;
    GL_tmpVertices[GL_tmpVerticesAmt+0].tv = v1;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+0].nx = 0;
    GL_tmpVertices[GL_tmpVerticesAmt+0].color = 0xFFFFFFFF;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+0].nz = 0;
    
    GL_tmpVertices[GL_tmpVerticesAmt+1].x = dstX;
    GL_tmpVertices[GL_tmpVerticesAmt+1].y = dstY + (scale * h_dst);
    GL_tmpVertices[GL_tmpVerticesAmt+1].z = 0.0;
    GL_tmpVertices[GL_tmpVerticesAmt+1].tu = u1;
    GL_tmpVertices[GL_tmpVerticesAmt+1].tv = v2;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+1].nx = 0;
    GL_tmpVertices[GL_tmpVerticesAmt+1].color = 0xFFFFFFFF;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+1].nz = 0;
    
    GL_tmpVertices[GL_tmpVerticesAmt+2].x = dstX + (scale * w_dst);
    GL_tmpVertices[GL_tmpVerticesAmt+2].y = dstY + (scale * h_dst);
    GL_tmpVertices[GL_tmpVerticesAmt+2].z = 0.0;
    GL_tmpVertices[GL_tmpVerticesAmt+2].tu = u2;
    GL_tmpVertices[GL_tmpVerticesAmt+2].tv = v2;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+2].nx = 0;
    GL_tmpVertices[GL_tmpVerticesAmt+2].color = 0xFFFFFFFF;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+2].nz = 0;
    
    GL_tmpVertices[GL_tmpVerticesAmt+3].x = dstX + (scale * w_dst);
    GL_tmpVertices[GL_tmpVerticesAmt+3].y = dstY;
    GL_tmpVertices[GL_tmpVerticesAmt+3].z = 0.0;
    GL_tmpVertices[GL_tmpVerticesAmt+3].tu = u2;
    GL_tmpVertices[GL_tmpVerticesAmt+3].tv = v1;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+3].nx = 0;
    GL_tmpVertices[GL_tmpVerticesAmt+3].color = 0xFFFFFFFF;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+3].nz = 0;
    
    GL_tmpTris[GL_tmpTrisAmt+0].v1 = GL_tmpVerticesAmt+1;
    GL_tmpTris[GL_tmpTrisAmt+0].v2 = GL_tmpVerticesAmt+0;
    GL_tmpTris[GL_tmpTrisAmt+0].v3 = GL_tmpVerticesAmt+2;
    
    GL_tmpTris[GL_tmpTrisAmt+1].v1 = GL_tmpVerticesAmt+0;
    GL_tmpTris[GL_tmpTrisAmt+1].v2 = GL_tmpVerticesAmt+3;
    GL_tmpTris[GL_tmpTrisAmt+1].v3 = GL_tmpVerticesAmt+2;
    
    GL_tmpVerticesAmt += 4;
    GL_tmpTrisAmt += 2;
}

void std3D_DrawMenuSubrect2(flex_t x, flex_t y, flex_t w, flex_t h, flex_t dstX, flex_t dstY, flex_t scale)
{
    //double tex_w = (double)Window_xSize;
    //double tex_h = (double)Window_ySize;
    double tex_w = Video_menuBuffer.format.width;
    double tex_h = Video_menuBuffer.format.height;

    float w_dst = w;
    float h_dst = h;

    if (scale == 0.0)
    {
        w_dst = (w / tex_w) * (double)Window_xSize;
        h_dst = (h / tex_h) * (double)Window_ySize;

        dstX = (dstX / tex_w) * (double)Window_xSize;
        dstY = (dstY / tex_h) * (double)Window_ySize;

        scale = 1.0;
    }

    double u1 = (x / tex_w);
    double u2 = ((x+w) / tex_w);
    double v1 = (y / tex_h);
    double v2 = ((y+h) / tex_h);

    GL_tmpVertices[GL_tmpVerticesAmt+0].x = dstX;
    GL_tmpVertices[GL_tmpVerticesAmt+0].y = dstY;
    GL_tmpVertices[GL_tmpVerticesAmt+0].z = 0.0;
    GL_tmpVertices[GL_tmpVerticesAmt+0].tu = u1;
    GL_tmpVertices[GL_tmpVerticesAmt+0].tv = v1;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+0].nx = 0;
    GL_tmpVertices[GL_tmpVerticesAmt+0].color = 0x000000FF;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+0].nz = 0;
    
    GL_tmpVertices[GL_tmpVerticesAmt+1].x = dstX;
    GL_tmpVertices[GL_tmpVerticesAmt+1].y = dstY + (scale * h_dst);
    GL_tmpVertices[GL_tmpVerticesAmt+1].z = 0.0;
    GL_tmpVertices[GL_tmpVerticesAmt+1].tu = u1;
    GL_tmpVertices[GL_tmpVerticesAmt+1].tv = v2;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+1].nx = 0;
    GL_tmpVertices[GL_tmpVerticesAmt+1].color = 0x000000FF;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+1].nz = 0;
    
    GL_tmpVertices[GL_tmpVerticesAmt+2].x = dstX + (scale * w_dst);
    GL_tmpVertices[GL_tmpVerticesAmt+2].y = dstY + (scale * h_dst);
    GL_tmpVertices[GL_tmpVerticesAmt+2].z = 0.0;
    GL_tmpVertices[GL_tmpVerticesAmt+2].tu = u2;
    GL_tmpVertices[GL_tmpVerticesAmt+2].tv = v2;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+2].nx = 0;
    GL_tmpVertices[GL_tmpVerticesAmt+2].color = 0x000000FF;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+2].nz = 0;
    
    GL_tmpVertices[GL_tmpVerticesAmt+3].x = dstX + (scale * w_dst);
    GL_tmpVertices[GL_tmpVerticesAmt+3].y = dstY;
    GL_tmpVertices[GL_tmpVerticesAmt+3].z = 0.0;
    GL_tmpVertices[GL_tmpVerticesAmt+3].tu = u2;
    GL_tmpVertices[GL_tmpVerticesAmt+3].tv = v1;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+3].nx = 0;
    GL_tmpVertices[GL_tmpVerticesAmt+3].color = 0x000000FF;
    *(uint32_t*)&GL_tmpVertices[GL_tmpVerticesAmt+3].nz = 0;
    
    GL_tmpTris[GL_tmpTrisAmt+0].v1 = GL_tmpVerticesAmt+1;
    GL_tmpTris[GL_tmpTrisAmt+0].v2 = GL_tmpVerticesAmt+0;
    GL_tmpTris[GL_tmpTrisAmt+0].v3 = GL_tmpVerticesAmt+2;
    
    GL_tmpTris[GL_tmpTrisAmt+1].v1 = GL_tmpVerticesAmt+0;
    GL_tmpTris[GL_tmpTrisAmt+1].v2 = GL_tmpVerticesAmt+3;
    GL_tmpTris[GL_tmpTrisAmt+1].v3 = GL_tmpVerticesAmt+2;
    
    GL_tmpVerticesAmt += 4;
    GL_tmpTrisAmt += 2;
}

static rdDDrawSurface* test_idk = NULL;
void std3D_DrawSimpleTex(std3DSimpleTexStage* pStage, std3DIntermediateFbo* pFbo, GLuint texId, GLuint texId2, GLuint texId3, flex_t param1, flex_t param2, flex_t param3, int gen_mips);
void std3D_DrawMapOverlay();
void std3D_DrawUIRenderList();

#ifdef RDRASTER_SOFTWARE_RENDERER
static GLuint std3D_swWorldTexId = 0;
static int    std3D_swWorldTexW = 0;
static int    std3D_swWorldTexH = 0;

// Present the software-rendered world buffer as a full-window quad, underneath the HUD/menu overlay.
// The software renderer draws the world at the full menu-buffer resolution into Video_pSwWorldBuffer
// (see the jkGame_Update software bracket, which redirects the camera canvas there). Here it is
// uploaded to a texture and stretched across the whole window through the menu shader — so the world
// fills the window rather than the 640x480 sub-rect the menu present samples. The menu shader
// discards palette index 0, so untouched pixels show the GL clear; the HUD menu quad, drawn after
// this in std3D_DrawMenu, composites on top.
static void std3D_PresentSWWorld(void)
{
    if (!Video_swWorldPresentPending)
        return;
    Video_swWorldPresentPending = 0;

    tVBuffer* pWorld = Video_pSwWorldBuffer;
    if (pWorld == NULL || pWorld->sdlSurface == NULL)
        return;

    int w = pWorld->format.width;
    int h = pWorld->format.height;

    // (Re)create the GL texture on first use or when the world buffer resized.
    if (std3D_swWorldTexId == 0 || std3D_swWorldTexW != w || std3D_swWorldTexH != h)
    {
        if (std3D_swWorldTexId != 0)
            glDeleteTextures(1, &std3D_swWorldTexId);
        glGenTextures(1, &std3D_swWorldTexId);
        glBindTexture(GL_TEXTURE_2D, std3D_swWorldTexId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
        std3D_swWorldTexW = w;
        std3D_swWorldTexH = h;
    }
    // Upload the current frame's world pixels EVERY frame (like the menu buffer) — uploading only on
    // (re)create would freeze the world on the first rendered frame.
    glBindTexture(GL_TEXTURE_2D, std3D_swWorldTexId);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, pWorld->sdlSurface->pixels);

    glBindFramebuffer(GL_FRAMEBUFFER, std3D_windowFbo);
    glDepthMask(GL_TRUE);
    glCullFace(GL_FRONT);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthFunc(GL_ALWAYS);
    glUseProgram(programMenu);

    // Full-window quad sampling the entire world texture, same tv convention as the menu present
    // (screen top y=0 -> tv=0), so the top-down 8bpp buffer renders upright.
    GL_tmpVertices[0].x = 0.0;           GL_tmpVertices[0].y = 0.0;           GL_tmpVertices[0].tu = 0.0; GL_tmpVertices[0].tv = 0.0;
    GL_tmpVertices[1].x = 0.0;           GL_tmpVertices[1].y = Window_ySize;  GL_tmpVertices[1].tu = 0.0; GL_tmpVertices[1].tv = 1.0;
    GL_tmpVertices[2].x = Window_xSize;  GL_tmpVertices[2].y = Window_ySize;  GL_tmpVertices[2].tu = 1.0; GL_tmpVertices[2].tv = 1.0;
    GL_tmpVertices[3].x = Window_xSize;  GL_tmpVertices[3].y = 0.0;           GL_tmpVertices[3].tu = 1.0; GL_tmpVertices[3].tv = 0.0;
    for (int i = 0; i < 4; i++)
    {
        GL_tmpVertices[i].z = 0.0;
        *(uint32_t*)&GL_tmpVertices[i].nx = 0;
        *(uint32_t*)&GL_tmpVertices[i].nz = 0;
        GL_tmpVertices[i].color = 0xFFFFFFFF;
    }
    GL_tmpTris[0].v1 = 1; GL_tmpTris[0].v2 = 0; GL_tmpTris[0].v3 = 2;
    GL_tmpTris[1].v1 = 0; GL_tmpTris[1].v2 = 3; GL_tmpTris[1].v3 = 2;

    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, std3D_swWorldTexId);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, displaypal_texture);
    glActiveTexture(GL_TEXTURE0 + 0);
    glUniform1i(programMenu_uniform_tex, 0);
    glUniform1i(programMenu_uniform_displayPalette, 1);

    glBindBuffer(GL_ARRAY_BUFFER, menu_vbo_all);
    glBufferData(GL_ARRAY_BUFFER, 4 * sizeof(D3DVERTEX), GL_tmpVertices, GL_STREAM_DRAW);
    glVertexAttribPointer(programMenu_attribute_coord3d, 3, GL_FLOAT, GL_FALSE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, x));
    glVertexAttribPointer(programMenu_attribute_v_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, color));
    glVertexAttribPointer(programMenu_attribute_v_uv, 2, GL_FLOAT, GL_FALSE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, tu));
    glEnableVertexAttribArray(programMenu_attribute_coord3d);
    glEnableVertexAttribArray(programMenu_attribute_v_color);
    glEnableVertexAttribArray(programMenu_attribute_v_uv);

    {
        float scaleX = 1.0 / ((double)Window_xSize / 2.0);
        float scaleY = 1.0 / ((double)Window_ySize / 2.0);
        float width = Window_xSize, height = Window_ySize;
        float d3dmat[16] = {
            scaleX, 0, 0, 0,
            0, -scaleY, 0, 0,
            0, 0, 1, 0,
            -(width / 2) * scaleX, (height / 2) * scaleY, -1, 1
        };
        glUniformMatrix4fv(programMenu_uniform_mvp, 1, GL_FALSE, d3dmat);
        glViewport(0, 0, width, height);
    }

    menu_data_elements[0] = GL_tmpTris[0].v1;
    menu_data_elements[1] = GL_tmpTris[0].v2;
    menu_data_elements[2] = GL_tmpTris[0].v3;
    menu_data_elements[3] = GL_tmpTris[1].v1;
    menu_data_elements[4] = GL_tmpTris[1].v2;
    menu_data_elements[5] = GL_tmpTris[1].v3;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, menu_ibo_triangle);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, 6 * sizeof(GLushort), menu_data_elements, GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);

    glDisableVertexAttribArray(programMenu_attribute_v_uv);
    glDisableVertexAttribArray(programMenu_attribute_v_color);
    glDisableVertexAttribArray(programMenu_attribute_coord3d);
}
#endif

// Added (remaster): HD replacement cutscene frame (see Main/jkCutsceneHD.h). Uploaded by
// jkCutscene whenever the HD decoder has a new frame; drawn by std3D_DrawMenu in place of the
// palettised SMK picture, while subtitles and the pause text still come from the menu buffer.
static GLuint std3D_cutsceneTex = 0;
static int std3D_cutsceneTexW = 0, std3D_cutsceneTexH = 0;
static int std3D_bCutsceneHd = 0;

void std3D_SetCutsceneFrame(const uint8_t* bgra, int w, int h, int stride)
{
    if (Main_bHeadless || !bgra || w <= 0 || h <= 0) return;
    if (std3D_cutsceneTex == 0 || std3D_cutsceneTexW != w || std3D_cutsceneTexH != h)
    {
        if (std3D_cutsceneTex) glDeleteTextures(1, &std3D_cutsceneTex);
        glGenTextures(1, &std3D_cutsceneTex);
        glBindTexture(GL_TEXTURE_2D, std3D_cutsceneTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        std3D_cutsceneTexW = w;
        std3D_cutsceneTexH = h;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, std3D_cutsceneTex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, bgra);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    std3D_bCutsceneHd = 1;
}

void std3D_ClearCutsceneFrame(void)
{
    std3D_bCutsceneHd = 0;
    if (std3D_cutsceneTex && !Main_bHeadless) glDeleteTextures(1, &std3D_cutsceneTex);
    std3D_cutsceneTex = 0;
    std3D_cutsceneTexW = std3D_cutsceneTexH = 0;
}

// Draws the HD frame into the original video's box (x, y, w, h in window pixels). If the HD
// file's shape differs from the original's (e.g. Topaz fitting 640x300 into 1920x1080 with black
// bars), the picture was fitted inside the frame, so the centre is cropped back to the original
// shape and the bars disappear.
static void std3D_DrawCutsceneHD(float x, float y, float w, float h)
{
    float srcAspect = (float)std3D_cutsceneTexW / (float)std3D_cutsceneTexH;
    float boxAspect = w / h;
    float u0 = 0, u1 = 1, v0 = 0, v1 = 1;
    if (srcAspect < boxAspect * 0.99f) {        // taller than the original: bars top and bottom
        float keep = srcAspect / boxAspect;
        v0 = (1.0f - keep) * 0.5f; v1 = 1.0f - v0;
    }
    else if (srcAspect > boxAspect * 1.01f) {   // wider: bars left and right
        float keep = boxAspect / srcAspect;
        u0 = (1.0f - keep) * 0.5f; u1 = 1.0f - u0;
    }
    float dx = x, dy = y, dw = w, dh = h;

    D3DVERTEX v[4];
    memset(v, 0, sizeof(v));
    v[0].x = dx;      v[0].y = dy;      v[0].tu = u0; v[0].tv = v0;
    v[1].x = dx;      v[1].y = dy + dh; v[1].tu = u0; v[1].tv = v1;
    v[2].x = dx + dw; v[2].y = dy + dh; v[2].tu = u1; v[2].tv = v1;
    v[3].x = dx + dw; v[3].y = dy;      v[3].tu = u1; v[3].tv = v0;
    for (int i = 0; i < 4; i++) v[i].color = 0xFFFFFFFF;
    GLushort idx[6] = { 1, 0, 2, 0, 3, 2 };

    float width = (float)Window_xSize, height = (float)Window_ySize;
    float scaleX = 1.0f / (width / 2.0f), scaleY = 1.0f / (height / 2.0f);
    float d3dmat[16] = {
        scaleX, 0, 0, 0,
        0, -scaleY, 0, 0,
        0, 0, 1, 0,
        -(width / 2) * scaleX, (height / 2) * scaleY, -1, 1
    };

    glUseProgram(std3D_uiProgram.program);
    glViewport(0, 0, Window_xSize, Window_ySize);
    glBlendFunc(GL_ONE, GL_ZERO);
    glUniformMatrix4fv(std3D_uiProgram.uniform_mvp, 1, GL_FALSE, d3dmat);
    glUniform2f(std3D_uiProgram.uniform_iResolution, width, height);
    glUniform1f(std3D_uiProgram.uniform_param1, 0.0);
    glUniform1f(std3D_uiProgram.uniform_param2, 1.0);
    glUniform1f(std3D_uiProgram.uniform_param3, jkPlayer_gamma);
    glUniform1i(std3D_uiProgram.uniform_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, std3D_cutsceneTex);

    glBindBuffer(GL_ARRAY_BUFFER, menu_vbo_all);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
    glEnableVertexAttribArray(std3D_uiProgram.attribute_coord3d);
    glEnableVertexAttribArray(std3D_uiProgram.attribute_v_color);
    glEnableVertexAttribArray(std3D_uiProgram.attribute_v_uv);
    glVertexAttribPointer(std3D_uiProgram.attribute_coord3d, 3, GL_FLOAT, GL_FALSE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, x));
    glVertexAttribPointer(std3D_uiProgram.attribute_v_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, color));
    glVertexAttribPointer(std3D_uiProgram.attribute_v_uv, 2, GL_FLOAT, GL_FALSE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, tu));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, menu_ibo_triangle);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
    glDisableVertexAttribArray(std3D_uiProgram.attribute_coord3d);
    glDisableVertexAttribArray(std3D_uiProgram.attribute_v_color);
    glDisableVertexAttribArray(std3D_uiProgram.attribute_v_uv);

    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glUseProgram(programMenu);
}

void std3D_DrawMenu()
{
    if (Main_bHeadless) return;

    //printf("Draw menu\n");
    std3D_DrawSceneFbo();
    //glFlush();

    // When the software renderer presented the world this frame, its buffer already contains the HUD
    // (Video_swCompositeOverlaysIntoWorld folded it in), so the in-game menu-overlay quad is skipped.
    int rdsw_bWorldPresented = 0;
#ifdef RDRASTER_SOFTWARE_RENDERER
    rdsw_bWorldPresented = Video_swWorldPresentPending; // capture before PresentSWWorld clears it
    // Draw the full-window software world (world + composited HUD) as the single frame.
    std3D_PresentSWWorld();
#endif

    glBindFramebuffer(GL_FRAMEBUFFER, std3D_windowFbo);
    glDepthMask(GL_TRUE);
    glCullFace(GL_FRONT);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthFunc(GL_ALWAYS);
    glUseProgram(programMenu);
    
    float menu_w, menu_h, menu_u, menu_v, menu_x;
    menu_w = (double)Window_xSize;
    menu_h = (double)Window_ySize;
    menu_u = 1.0;
    menu_v = 1.0;
    menu_x = 0.0;
    
    int bFixHudScale = 0;

    double fake_windowW = (double)Window_xSize;
    double fake_windowH = (double)Window_ySize;

    if (!jkGame_isDDraw && !jkGuiBuildMulti_bRendering && !jkCutscene_isRendering)
    {
        //menu_w = 640.0;
        //menu_h = 480.0;

        // Stretch screen
        menu_u = (1.0 / Video_menuBuffer.format.width) * 640.0;
        menu_v = (1.0 / Video_menuBuffer.format.height) * 480.0;

        // Keep 4:3 aspect
        menu_x = (menu_w - (menu_h * (640.0 / 480.0))) / 2.0;
        menu_w = (menu_h * (640.0 / 480.0));
    }
    else if (jkCutscene_isRendering) {
        bFixHudScale = 1;

        //menu_w = 640.0;
        //menu_h = 480.0;

        menu_w = Video_menuBuffer.format.width;
        menu_h = Video_menuBuffer.format.height;

        // For ultrawide screens, limit the width to 16:9
        if (Window_xSize > Window_ySize && ((double)Window_xSize / (double)Window_ySize) > (Main_bMotsCompat ? (16.0/9.0) : (21.0/9.0))) {
            fake_windowW = fake_windowH * (16.0/9.0);
        }

        // Keep 4:3 aspect
        menu_x = (menu_w - (menu_h * (640.0 / 480.0))) / 2.0;

    }
    else if (jkGuiBuildMulti_bRendering)
    {
        bFixHudScale = 1;

        // Stretch screen
        menu_u = (1.0 / Video_menuBuffer.format.width) * 640.0;
        menu_v = (1.0 / Video_menuBuffer.format.height) * 480.0;

        // Keep 4:3 aspect
        menu_x = (menu_w - (menu_h * (640.0 / 480.0))) / 2.0;
        menu_w = (menu_h * (640.0 / 480.0));
    }
    else
    {
        bFixHudScale = 0;

        menu_w = Video_menuBuffer.format.width;
        menu_h = Video_menuBuffer.format.height;
    }

    if (!bFixHudScale)
    {
        GL_tmpVertices[0].x = menu_x;
        GL_tmpVertices[0].y = 0.0;
        GL_tmpVertices[0].z = 0.0;
        GL_tmpVertices[0].tu = 0.0;
        GL_tmpVertices[0].tv = 0.0;
        *(uint32_t*)&GL_tmpVertices[0].nx = 0;
        GL_tmpVertices[0].color = 0xFFFFFFFF;
        *(uint32_t*)&GL_tmpVertices[0].nz = 0;
        
        GL_tmpVertices[1].x = menu_x;
        GL_tmpVertices[1].y = menu_h;
        GL_tmpVertices[1].z = 0.0;
        GL_tmpVertices[1].tu = 0.0;
        GL_tmpVertices[1].tv = menu_v;
        *(uint32_t*)&GL_tmpVertices[1].nx = 0;
        GL_tmpVertices[1].color = 0xFFFFFFFF;
        *(uint32_t*)&GL_tmpVertices[1].nz = 0;
        
        GL_tmpVertices[2].x = menu_x + menu_w;
        GL_tmpVertices[2].y = menu_h;
        GL_tmpVertices[2].z = 0.0;
        GL_tmpVertices[2].tu = menu_u;
        GL_tmpVertices[2].tv = menu_v;
        *(uint32_t*)&GL_tmpVertices[2].nx = 0;
        GL_tmpVertices[2].color = 0xFFFFFFFF;
        *(uint32_t*)&GL_tmpVertices[2].nz = 0;
        
        GL_tmpVertices[3].x = menu_x + menu_w;
        GL_tmpVertices[3].y = 0.0;
        GL_tmpVertices[3].z = 0.0;
        GL_tmpVertices[3].tu = menu_u;
        GL_tmpVertices[3].tv = 0.0;
        *(uint32_t*)&GL_tmpVertices[3].nx = 0;
        GL_tmpVertices[3].color = 0xFFFFFFFF;
        *(uint32_t*)&GL_tmpVertices[3].nz = 0;
        
        GL_tmpTris[0].v1 = 1;
        GL_tmpTris[0].v2 = 0;
        GL_tmpTris[0].v3 = 2;
        
        GL_tmpTris[1].v1 = 0;
        GL_tmpTris[1].v2 = 3;
        GL_tmpTris[1].v3 = 2;

        // The software renderer already folded the HUD into the world buffer it just presented, so
        // suppress this redundant in-game menu-overlay quad (else the HUD would be drawn twice).
        GL_tmpVerticesAmt = rdsw_bWorldPresented ? 0 : 4;
        GL_tmpTrisAmt = rdsw_bWorldPresented ? 0 : 2;
    }
    else if (jkGuiBuildMulti_bRendering)
    {
        GL_tmpVerticesAmt = 0;
        GL_tmpTrisAmt = 0;

        // Main View
        std3D_DrawMenuSubrect(0, 0, 640, 480, menu_x, 0, menu_w/640.0);
    }
    else if (jkCutscene_isRendering)
    {
        GL_tmpVerticesAmt = 0;
        GL_tmpTrisAmt = 0;

        int video_height = Main_bMotsCompat ? 350 : 300;
        int subs_y = 350;
        int subs_h = 130;
        if (Main_bMotsCompat) {
            subs_y = 400;
            subs_h = 80;
        }

        float partial_menu_w = (menu_h * (640.0 / 480.0));
        float upscale = fake_windowW/640.0;
        float upscale2 = (fake_windowH - (50 + video_height * upscale))/((double)subs_h);
        float upscale3 = 1.0;//Window_ySize/480.0;

        if (upscale2 < 1.0) {
            upscale2 = 1.0;
            if (fake_windowH > 480.0) {
                upscale2 = 2.0;
            }
        }
        if (upscale2 > upscale) {
            upscale2 = upscale;
        }

        float shift_y = ((double)Window_ySize - fake_windowH) / 2.0;
        float shift_x = ((double)Window_xSize - fake_windowW) / 2.0;

        float sub_width = 640*upscale2;
        float sub_x = (fake_windowW - sub_width) / 2.0;

        float pause_width = 640*upscale3;
        float pause_x = (fake_windowW - pause_width) / 2.0;

        //printf("%f %f, %f %f %f, %d %d\n", sub_x, pause_x, upscale, upscale2, upscale3, Window_xSize, Window_ySize);

        // Main View: the HD replacement frame if one is playing, else the original SMK picture
        if (std3D_bCutsceneHd && std3D_cutsceneTex) {
            std3D_DrawCutsceneHD(shift_x, shift_y + 50, 640 * upscale, video_height * upscale);
        }
        glBlendFunc(GL_SRC_ALPHA, GL_SRC_ALPHA);
        if (!(std3D_bCutsceneHd && std3D_cutsceneTex)) {
            std3D_DrawMenuSubrect(0, 50, 640, video_height, shift_x + 0, shift_y + 50, upscale);
        }

        // Subtitles
        if (jkCutscene_dword_55B750) {
            

            // Some monitors might not have a bottom black bar, so draw an outline
            std3D_DrawMenuSubrect2(0, subs_y, 640, subs_h, shift_x + sub_x-2, shift_y + fake_windowH - (subs_h*upscale2), upscale2);
            std3D_DrawMenuSubrect2(0, subs_y, 640, subs_h, shift_x + sub_x+2, shift_y + fake_windowH - (subs_h*upscale2), upscale2);
            std3D_DrawMenuSubrect2(0, subs_y, 640, subs_h, shift_x + sub_x, shift_y + fake_windowH - (subs_h*upscale2) - 2, upscale2);
            std3D_DrawMenuSubrect2(0, subs_y, 640, subs_h, shift_x + sub_x, shift_y + fake_windowH - (subs_h*upscale2) + 2, upscale2);

            //std3D_DrawMenuSubrect2(0, subs_y, 640, subs_h, shift_x + sub_x-2, shift_y + fake_windowH - (subs_h*upscale2) -2, upscale2);
            //std3D_DrawMenuSubrect2(0, subs_y, 640, subs_h, shift_x + sub_x+2, shift_y + fake_windowH - (subs_h*upscale2) +2, upscale2);
            //std3D_DrawMenuSubrect2(0, subs_y, 640, subs_h, shift_x + sub_x+2, shift_y + fake_windowH - (subs_h*upscale2) - 2, upscale2);
            //std3D_DrawMenuSubrect2(0, subs_y, 640, subs_h, shift_x + sub_x-2, shift_y + fake_windowH - (subs_h*upscale2) + 2, upscale2);

            std3D_DrawMenuSubrect(0, subs_y, 640, subs_h, shift_x + sub_x, shift_y + fake_windowH - (subs_h*upscale2), upscale2);
        }

        // Paused
        std3D_DrawMenuSubrect(0, 10, 640, 40, shift_x + pause_x, shift_y + 0*upscale, upscale3);
    }
    else
    {
        GL_tmpVerticesAmt = 0;
        GL_tmpTrisAmt = 0;

        // Main View
        std3D_DrawMenuSubrect(0, 128, menu_w, menu_h-256, 0, 128, 0.0);

        float hudScale = Window_ySize / 480.0;

        /*if (menu_w >= 3600)
            hudScale = 4;
        else if (menu_w >= 1800)
            hudScale = 3;
        else if (menu_w >= 1200)
            hudScale = 2;*/

        // Left and Right HUD
        std3D_DrawMenuSubrect(0, menu_h - 64, 64, 64, 0, Window_ySize - 64*hudScale, hudScale);
        std3D_DrawMenuSubrect(menu_w - 64, menu_h - 64, 64, 64, Window_xSize - 64*hudScale, Window_ySize - 64*hudScale, hudScale);

        // Items
        std3D_DrawMenuSubrect((menu_w / 2) - 128, menu_h - 64, 256, 64, (Window_xSize / 2) - (128*hudScale), Window_ySize - 64*hudScale, hudScale);

        // Text
        float textScale = hudScale;
        if (jkDev_BMFontHeight > 11) {
            textScale *= 11.0 / (float)jkDev_BMFontHeight;
        }
        float textWidth = menu_w - (48*2);
        float textHeight = jkDev_BMFontHeight * 5.5;
        float destTextWidth = textWidth * textScale;
        std3D_DrawMenuSubrect(48, 0, menu_w - (48*2), textHeight, (Window_xSize / 2) - (destTextWidth / 2), 0, textScale);

        // Active forcepowers/items
        std3D_DrawMenuSubrect(menu_w - 48, 0, 48, 128, Window_xSize - (48*hudScale), 0, hudScale);
    }

    glActiveTexture(GL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, Video_menuTexId);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, Video_menuBuffer.format.width, Video_menuBuffer.format.height, GL_RED, GL_UNSIGNED_BYTE, Video_menuBuffer.sdlSurface->pixels);

    //GLushort data_elements[32 * 3];
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, displaypal_texture);

    glActiveTexture(GL_TEXTURE0 + 0);
    glUniform1i(programMenu_uniform_tex, 0);
    glUniform1i(programMenu_uniform_displayPalette, 1);

    D3DVERTEX* vertexes = GL_tmpVertices;

    glBindBuffer(GL_ARRAY_BUFFER, menu_vbo_all);
    glBufferData(GL_ARRAY_BUFFER, GL_tmpVerticesAmt * sizeof(D3DVERTEX), GL_tmpVertices, GL_STREAM_DRAW);
    glVertexAttribPointer(
        programMenu_attribute_coord3d, // attribute
        3,                 // number of elements per vertex, here (x,y,z)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // data stride
        (GLvoid*)offsetof(D3DVERTEX, x)                  // offset of first element
    );
    
    glVertexAttribPointer(
        programMenu_attribute_v_color, // attribute
        4,                 // number of elements per vertex, here (R,G,B,A)
        GL_UNSIGNED_BYTE,  // the type of each element
        GL_TRUE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, color) // offset of first element
    );

    /*glVertexAttribPointer(
        std3D_texFboStage.attribute_v_light, // attribute
        1,                 // number of elements per vertex, here (L)
        GL_FLOAT,  // the type of each element
        GL_FALSE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, lightLevel) // offset of first element
    );*/

    glVertexAttribPointer(
        programMenu_attribute_v_uv,    // attribute
        2,                 // number of elements per vertex, here (U,V)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // take our values as-is
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, tu)                  // offset of first element
    );

    glEnableVertexAttribArray(programMenu_attribute_coord3d);
    glEnableVertexAttribArray(programMenu_attribute_v_color);
    glEnableVertexAttribArray(programMenu_attribute_v_uv);

    {

    float maxX, maxY, scaleX, scaleY, width, height;

    scaleX = 1.0/((double)Window_xSize / 2.0);
    scaleY = 1.0/((double)Window_ySize / 2.0);
    maxX = 1.0;
    maxY = 1.0;
    width = Window_xSize;
    height = Window_ySize;
    
    float d3dmat[16] = {
       maxX*scaleX,      0,                                          0,      0, // right
       0,                                       -maxY*scaleY,               0,      0, // up
       0,                                       0,                                          1,     0, // forward
       -(width/2)*scaleX,  (height/2)*scaleY,     -1,      1  // pos
    };
    
    glUniformMatrix4fv(programMenu_uniform_mvp, 1, GL_FALSE, d3dmat);
    glViewport(0, 0, width, height);

    }
    
    rdTri* tris = GL_tmpTris;
    
    rdDDrawSurface* last_tex = (rdDDrawSurface*)(intptr_t)-1;
    int last_tex_idx = 0;
    //GLushort* data_elements = malloc(sizeof(GLushort) * 3 * GL_tmpTrisAmt);
    for (int j = 0; j < GL_tmpTrisAmt; j++)
    {
        menu_data_elements[(j*3)+0] = tris[j].v1;
        menu_data_elements[(j*3)+1] = tris[j].v2;
        menu_data_elements[(j*3)+2] = tris[j].v3;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, menu_ibo_triangle);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GL_tmpTrisAmt * 3 * sizeof(GLushort), menu_data_elements, GL_STREAM_DRAW);

    // Perf: draw the count just uploaded. Asking GL for the buffer's size makes the
    // CPU wait for the GPU, every frame.
    glDrawElements(GL_TRIANGLES, (GLsizei)(GL_tmpTrisAmt * 3), GL_UNSIGNED_SHORT, 0);

    glDisableVertexAttribArray(programMenu_attribute_v_uv);
    glDisableVertexAttribArray(programMenu_attribute_v_color);
    glDisableVertexAttribArray(programMenu_attribute_coord3d);

    std3D_DrawMapOverlay();
    std3D_DrawUIRenderList();

    last_flags = 0;
}

void std3D_DrawMapOverlay()
{
    if (Main_bHeadless) return;

    //glFlush();

    glBindFramebuffer(GL_FRAMEBUFFER, std3D_windowFbo);
    glDepthMask(GL_TRUE);
    glCullFace(GL_FRONT);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthFunc(GL_ALWAYS);
    glUseProgram(programMenu);
    
    float menu_w = (double)Window_xSize;
    float menu_h = (double)Window_ySize;

    if (!jkGame_isDDraw)
    {
        return;
    }

    menu_w = Video_menuBuffer.format.width;
    menu_h = Video_menuBuffer.format.height;

    GL_tmpVerticesAmt = 0;
    GL_tmpTrisAmt = 0;

    // Main View
    std3D_DrawMenuSubrect(0, 0, menu_w, menu_h, 0, 0, 0.0);

    glActiveTexture(GL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, Video_overlayTexId);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, Video_overlayMapBuffer.format.width, Video_overlayMapBuffer.format.height, GL_RED, GL_UNSIGNED_BYTE, Video_overlayMapBuffer.sdlSurface->pixels);

    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, displaypal_texture);

    glActiveTexture(GL_TEXTURE0 + 0);
    glUniform1i(programMenu_uniform_tex, 0);
    glUniform1i(programMenu_uniform_displayPalette, 1);

    D3DVERTEX* vertexes = GL_tmpVertices;
    glBindBuffer(GL_ARRAY_BUFFER, menu_vbo_all);
    glBufferData(GL_ARRAY_BUFFER, GL_tmpVerticesAmt * sizeof(D3DVERTEX), GL_tmpVertices, GL_STREAM_DRAW);
    glVertexAttribPointer(
        programMenu_attribute_coord3d, // attribute
        3,                 // number of elements per vertex, here (x,y,z)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // data stride
        (GLvoid*)offsetof(D3DVERTEX, x)                  // offset of first element
    );
    
    glVertexAttribPointer(
        programMenu_attribute_v_color, // attribute
        4,                 // number of elements per vertex, here (R,G,B,A)
        GL_UNSIGNED_BYTE,  // the type of each element
        GL_TRUE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, color) // offset of first element
    );

    glVertexAttribPointer(
        programMenu_attribute_v_uv,    // attribute
        2,                 // number of elements per vertex, here (U,V)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // take our values as-is
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, tu)                  // offset of first element
    );

    glEnableVertexAttribArray(programMenu_attribute_coord3d);
    glEnableVertexAttribArray(programMenu_attribute_v_color);
    glEnableVertexAttribArray(programMenu_attribute_v_uv);


    {

    float maxX, maxY, scaleX, scaleY, width, height;

    scaleX = 1.0/((double)Window_xSize / 2.0);
    scaleY = 1.0/((double)Window_ySize / 2.0);
    maxX = 1.0;
    maxY = 1.0;
    width = Window_xSize;
    height = Window_ySize;
    
    float d3dmat[16] = {
       maxX*scaleX,      0,                                          0,      0, // right
       0,                                       -maxY*scaleY,               0,      0, // up
       0,                                       0,                                          1,     0, // forward
       -(width/2)*scaleX,  (height/2)*scaleY,     -1,      1  // pos
    };
    
    glUniformMatrix4fv(programMenu_uniform_mvp, 1, GL_FALSE, d3dmat);
    glViewport(0, 0, width, height);

    }
    
    rdTri* tris = GL_tmpTris;
    
    rdDDrawSurface* last_tex = (rdDDrawSurface*)(intptr_t)-1;
    int last_tex_idx = 0;
    //GLushort* data_elements = malloc(sizeof(GLushort) * 3 * GL_tmpTrisAmt);
    for (int j = 0; j < GL_tmpTrisAmt; j++)
    {
        menu_data_elements[(j*3)+0] = tris[j].v1;
        menu_data_elements[(j*3)+1] = tris[j].v2;
        menu_data_elements[(j*3)+2] = tris[j].v3;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, menu_ibo_triangle);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GL_tmpTrisAmt * 3 * sizeof(GLushort), menu_data_elements, GL_STREAM_DRAW);

    // Perf: draw the count just uploaded. Asking GL for the buffer's size makes the
    // CPU wait for the GPU, every frame.
    glDrawElements(GL_TRIANGLES, (GLsizei)(GL_tmpTrisAmt * 3), GL_UNSIGNED_SHORT, 0);

    glDisableVertexAttribArray(programMenu_attribute_v_uv);
    glDisableVertexAttribArray(programMenu_attribute_v_color);
    glDisableVertexAttribArray(programMenu_attribute_coord3d);
}

void std3D_DrawUIBitmapRGBA(stdBitmap* pBmp, int mipIdx, flex_t dstX, flex_t dstY, rdRect* srcRect, flex_t scaleX, flex_t scaleY, int bAlphaOverwrite, uint8_t color_r, uint8_t color_g, uint8_t color_b, uint8_t color_a)
{
    float internalWidth = Video_menuBuffer.format.width;
    float internalHeight = Video_menuBuffer.format.height;

    if (!pBmp) return;
    if (!pBmp->abLoadedToGPU[mipIdx]) {
        std3D_AddBitmapToTextureCache(pBmp, mipIdx, !(pBmp->palFmt & 1), 0);
    }

    if (jkGuiBuildMulti_bRendering) {
        internalWidth = 640.0;
        internalHeight = 480.0;
    }

    double scaleX_ = (double)Window_xSize/(double)internalWidth;
    double scaleY_ = (double)Window_ySize/(double)internalHeight;

    dstX *= scaleX_;
    dstY *= scaleY_;

    //double tex_w = (double)Window_xSize;
    //double tex_h = (double)Window_ySize;
    double tex_w = pBmp->mipSurfaces[0]->format.width;
    double tex_h = pBmp->mipSurfaces[0]->format.height;

    double w = tex_w;
    double h = tex_h;
    double x = 0;
    double y = 0;

    if (srcRect) {
        x = srcRect->x;
        y = srcRect->y;
        w = srcRect->width;
        h = srcRect->height;
    }

    float w_dst = w;
    float h_dst = h;

    if (scaleX == 0.0 && scaleY == 0.0)
    {
        w_dst = (w / tex_w) * (double)Window_xSize;
        h_dst = (h / tex_h) * (double)Window_ySize;

        dstX = (dstX / tex_w) * (double)Window_xSize;
        dstY = (dstY / tex_h) * (double)Window_ySize;

        scaleX = 1.0;
        scaleY = 1.0;
    }

    double dstScaleX = scaleX;
    double dstScaleY = scaleY;
    dstScaleX *= scaleX_;
    dstScaleY *= scaleY_;

    double u1 = (x / tex_w);
    double u2 = ((x+w) / tex_w);
    double v1 = (y / tex_h);
    double v2 = ((y+h) / tex_h);

    uint32_t color = 0;

    color |= (color_r << 0);
    color |= (color_g << 8);
    color |= (color_b << 16);
    color |= (color_a << 24);

    if (GL_tmpUIVerticesAmt + 4 > STD3D_MAX_UI_VERTICES) {
        return;
    }
    if (GL_tmpUITrisAmt + 2 > STD3D_MAX_UI_TRIS) {
        return;
    }

    if (dstY + (dstScaleY * h_dst) < 0.0 || dstX + (dstScaleX * w_dst) < 0.0) {
        return;
    }
    if (dstY > Window_ySize || dstX > Window_xSize) {
        return;
    }

    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].x = dstX;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].y = dstY;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].z = 0.0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].tu = u1;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].tv = v1;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].nx = 0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].color = color;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].nz = 0;
    
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].x = dstX;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].y = dstY + (dstScaleY * h_dst);
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].z = 0.0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].tu = u1;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].tv = v2;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].nx = 0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].color = color;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].nz = 0;
    
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].x = dstX + (dstScaleX * w_dst);
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].y = dstY + (dstScaleY * h_dst);
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].z = 0.0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].tu = u2;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].tv = v2;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].nx = 0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].color = color;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].nz = 0;
    
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].x = dstX + (dstScaleX * w_dst);
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].y = dstY;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].z = 0.0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].tu = u2;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].tv = v1;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].nx = 0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].color = color;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].nz = 0;
    
    GL_tmpUITris[GL_tmpUITrisAmt+0].v1 = GL_tmpUIVerticesAmt+1;
    GL_tmpUITris[GL_tmpUITrisAmt+0].v2 = GL_tmpUIVerticesAmt+0;
    GL_tmpUITris[GL_tmpUITrisAmt+0].v3 = GL_tmpUIVerticesAmt+2;
    GL_tmpUITris[GL_tmpUITrisAmt+0].flags = bAlphaOverwrite;
    GL_tmpUITris[GL_tmpUITrisAmt+0].texture = pBmp->aTextureIds[mipIdx];
    
    GL_tmpUITris[GL_tmpUITrisAmt+1].v1 = GL_tmpUIVerticesAmt+0;
    GL_tmpUITris[GL_tmpUITrisAmt+1].v2 = GL_tmpUIVerticesAmt+3;
    GL_tmpUITris[GL_tmpUITrisAmt+1].v3 = GL_tmpUIVerticesAmt+2;
    GL_tmpUITris[GL_tmpUITrisAmt+1].flags = bAlphaOverwrite;
    GL_tmpUITris[GL_tmpUITrisAmt+1].texture = pBmp->aTextureIds[mipIdx];
    
    GL_tmpUIVerticesAmt += 4;
    GL_tmpUITrisAmt += 2;
}

void std3D_DrawUIBitmap(stdBitmap* pBmp, int mipIdx, flex_t dstX, flex_t dstY, rdRect* srcRect, flex_t scale, int bAlphaOverwrite)
{
    std3D_DrawUIBitmapRGBA(pBmp, mipIdx, dstX, dstY, srcRect, scale, scale, bAlphaOverwrite, 0xFF, 0xFF, 0xFF, 0xFF);
}

void std3D_DrawUIClearedRect(uint8_t palIdx, rdRect* dstRect)
{
    if (!displaypal_data) return;
    uint32_t color = 0;
    uint8_t color_r = ((uint8_t*)displaypal_data)[(palIdx*3) + 0];
    uint8_t color_g = ((uint8_t*)displaypal_data)[(palIdx*3) + 1];
    uint8_t color_b = ((uint8_t*)displaypal_data)[(palIdx*3) + 2];

    std3D_DrawUIClearedRectRGBA(color_r, color_g, color_b, 0xFF, dstRect);
}

void std3D_DrawUIClearedRectRGBA(uint8_t color_r, uint8_t color_g, uint8_t color_b, uint8_t color_a, rdRect* dstRect)
{
    if (!has_initted) return;
    if (!dstRect) return;
    double dstX = dstRect->x;
    double dstY = dstRect->y;

    float internalWidth = Video_menuBuffer.format.width;
    float internalHeight = Video_menuBuffer.format.height;
    if (!internalWidth || !internalHeight) return;

    if (jkGuiBuildMulti_bRendering) {
        internalWidth = 640.0;
        internalHeight = 480.0;
    }

    double scaleX = (double)Window_xSize/(double)internalWidth;
    double scaleY = (double)Window_ySize/(double)internalHeight;

    dstX *= scaleX;
    dstY *= scaleY;

    //double tex_w = (double)Window_xSize;
    //double tex_h = (double)Window_ySize;
    double tex_w = dstRect->width;
    double tex_h = dstRect->height;
    if (!tex_w || !tex_h) return;

    double w = tex_w;
    double h = tex_h;
    double x = 0;
    double y = 0;

    float w_dst = w;
    float h_dst = h;
    double scale = 1.0;

    double dstScaleX = scale;
    double dstScaleY = scale;
    dstScaleX *= scaleX;
    dstScaleY *= scaleY;

    double u1 = (x / tex_w);
    double u2 = ((x+w) / tex_w);
    double v1 = (y / tex_h);
    double v2 = ((y+h) / tex_h);

    uint32_t color = 0;

    color |= (color_r << 0);
    color |= (color_g << 8);
    color |= (color_b << 16);
    color |= (color_a << 24);
    if (GL_tmpUIVerticesAmt + 4 > STD3D_MAX_UI_VERTICES) {
        return;
    }
    if (GL_tmpUITrisAmt + 2 > STD3D_MAX_UI_TRIS) {
        return;
    }

    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].x = dstX;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].y = dstY;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].z = 0.0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].tu = u1;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].tv = v1;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].nx = 0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].color = color;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+0].nz = 0;
    
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].x = dstX;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].y = dstY + (dstScaleY * h_dst);
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].z = 0.0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].tu = u1;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].tv = v2;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].nx = 0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].color = color;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+1].nz = 0;
    
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].x = dstX + (dstScaleX * w_dst);
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].y = dstY + (dstScaleY * h_dst);
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].z = 0.0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].tu = u2;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].tv = v2;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].nx = 0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].color = color;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+2].nz = 0;
    
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].x = dstX + (dstScaleX * w_dst);
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].y = dstY;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].z = 0.0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].tu = u2;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].tv = v1;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].nx = 0;
    GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].color = color;
    *(uint32_t*)&GL_tmpUIVertices[GL_tmpUIVerticesAmt+3].nz = 0;
    
    GL_tmpUITris[GL_tmpUITrisAmt+0].v1 = GL_tmpUIVerticesAmt+1;
    GL_tmpUITris[GL_tmpUITrisAmt+0].v2 = GL_tmpUIVerticesAmt+0;
    GL_tmpUITris[GL_tmpUITrisAmt+0].v3 = GL_tmpUIVerticesAmt+2;
    GL_tmpUITris[GL_tmpUITrisAmt+0].flags = 0;
    GL_tmpUITris[GL_tmpUITrisAmt+0].texture = blank_tex_white;
    
    GL_tmpUITris[GL_tmpUITrisAmt+1].v1 = GL_tmpUIVerticesAmt+0;
    GL_tmpUITris[GL_tmpUITrisAmt+1].v2 = GL_tmpUIVerticesAmt+3;
    GL_tmpUITris[GL_tmpUITrisAmt+1].v3 = GL_tmpUIVerticesAmt+2;
    GL_tmpUITris[GL_tmpUITrisAmt+1].flags = 0;
    GL_tmpUITris[GL_tmpUITrisAmt+1].texture = blank_tex_white;
    
    GL_tmpUIVerticesAmt += 4;
    GL_tmpUITrisAmt += 2;
}

void std3D_DrawUIRenderList()
{
    if (Main_bHeadless) return;
    if (!GL_tmpUITrisAmt) return;

    //glFlush();

    //printf("Draw render list\n");
    glBindFramebuffer(GL_FRAMEBUFFER, std3D_windowFbo);
    glDepthMask(GL_TRUE);
    glCullFace(GL_FRONT);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthFunc(GL_ALWAYS);
    glUseProgram(std3D_uiProgram.program); // TODO: simpler shader
    
    last_ui_tex = 0;
    last_ui_flags = -1;

    // Generate aVertices list
    D3DVERTEX* vertexes = GL_tmpUIVertices;

    float maxX, maxY, scaleX, scaleY, width, height;

    float internalWidth = Window_xSize;//Video_menuBuffer.format.width;
    float internalHeight = Window_ySize;//Video_menuBuffer.format.height;

    if (jkGuiBuildMulti_bRendering) {
        internalWidth = 640.0;
        internalHeight = 480.0;
    }

    maxX = 1.0;
    maxY = 1.0;
    scaleX = 1.0/((double)internalWidth / 2.0);
    scaleY = 1.0/((double)internalHeight / 2.0);
    width = Window_xSize;
    height = Window_ySize;

    if (jkGuiBuildMulti_bRendering) {
        width = 640;
        height = 480;
    }

    // JKDF2's vertical FOV is fixed with their projection, for whatever reason. 
    // This ends up resulting in the view looking squished vertically at wide/ultrawide aspect ratios.
    // To compensate, we zoom the y axis here.
    // I also went ahead and fixed vertical displays in the same way because it seems to look better.
    float zoom_yaspect = 1.0;
    float zoom_xaspect = 1.0;
    
    float shift_add_x = 0;
    float shift_add_y = 0;
    
    glUniform1i(std3D_uiProgram.uniform_tex, 0);
    glUniform1i(std3D_uiProgram.uniform_tex2, 1);
    glUniform1i(std3D_uiProgram.uniform_tex3, 2);

    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, blank_tex_white);
    
    {
    
    float d3dmat[16] = {
       maxX*scaleX,      0,                                          0,      0, // right
       0,                                       -maxY*scaleY,               0,      0, // up
       0,                                       0,                                          1,     0, // forward
       -(width/2)*scaleX,  (height/2)*scaleY,     -1,      1  // pos
    };
    
    glUniformMatrix4fv(std3D_uiProgram.uniform_mvp, 1, GL_FALSE, d3dmat);
    glViewport(0, 0, width, height);
    glUniform2f(std3D_uiProgram.uniform_iResolution, internalWidth, internalHeight);

    float param1 = 1.0;
    float param2 = 1.0;

    glUniform1f(std3D_uiProgram.uniform_param1, param1);
    glUniform1f(std3D_uiProgram.uniform_param2, param2);
    glUniform1f(std3D_uiProgram.uniform_param3, jkPlayer_gamma);
    
    }

    rdUITri* tris = GL_tmpUITris;
    glEnableVertexAttribArray(std3D_uiProgram.attribute_coord3d);
    glEnableVertexAttribArray(std3D_uiProgram.attribute_v_color);
    glEnableVertexAttribArray(std3D_uiProgram.attribute_v_uv);

    glBindBuffer(GL_ARRAY_BUFFER, menu_vbo_all);
    glBufferData(GL_ARRAY_BUFFER, GL_tmpUIVerticesAmt * sizeof(D3DVERTEX), GL_tmpUIVertices, GL_STREAM_DRAW);
    glVertexAttribPointer(
        std3D_uiProgram.attribute_coord3d, // attribute
        3,                 // number of elements per vertex, here (x,y,z)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // data stride
        (GLvoid*)offsetof(D3DVERTEX, x)                  // offset of first element
    );
    
    glVertexAttribPointer(
        std3D_uiProgram.attribute_v_color, // attribute
        4,                 // number of elements per vertex, here (R,G,B,A)
        GL_UNSIGNED_BYTE,  // the type of each element
        GL_TRUE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, color) // offset of first element
    );

    /*glVertexAttribPointer(
        std3D_uiProgram.attribute_v_light, // attribute
        1,                 // number of elements per vertex, here (L)
        GL_FLOAT,  // the type of each element
        GL_FALSE,          // normalize fixed-point data?
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, lightLevel) // offset of first element
    );*/

    glVertexAttribPointer(
        std3D_uiProgram.attribute_v_uv,    // attribute
        2,                 // number of elements per vertex, here (U,V)
        GL_FLOAT,          // the type of each element
        GL_FALSE,          // take our values as-is
        sizeof(D3DVERTEX),                 // no extra data between each position
        (GLvoid*)offsetof(D3DVERTEX, tu)                  // offset of first element
    );
    
    //glEnableVertexAttribArray(attribute_v_norm);

    int last_flags = 0;
    int last_tex_idx = 0;
    //GLushort* menu_data_elements = malloc(sizeof(GLushort) * 3 * GL_tmpTrisAmt);
    for (int j = 0; j < GL_tmpUITrisAmt; j++)
    {
        menu_data_elements[(j*3)+0] = tris[j].v1;
        menu_data_elements[(j*3)+1] = tris[j].v2;
        menu_data_elements[(j*3)+2] = tris[j].v3;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, menu_ibo_triangle);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GL_tmpUITrisAmt * 3 * sizeof(GLushort), menu_data_elements, GL_STREAM_DRAW);
    
    int do_batch = 0;

    int tex_id = tris[0].texture;
    glActiveTexture(GL_TEXTURE0 + 0);
    if (tex_id == 0)
        glBindTexture(GL_TEXTURE_2D, blank_tex_white);
    else
        glBindTexture(GL_TEXTURE_2D, tex_id);

    if (tris[0].flags) {
        glUniform1f(std3D_uiProgram.uniform_param1, 1.0);
    }
    else {
        glUniform1f(std3D_uiProgram.uniform_param1, 0.0);
    }
    
    for (int j = 0; j < GL_tmpUITrisAmt; j++)
    {
        if (tris[j].texture != last_ui_tex || tris[j].flags != last_ui_flags)
        {
            do_batch = 1;
        }
        
        if (do_batch)
        {
            int num_tris_batch = j - last_tex_idx;

            if (num_tris_batch)
            {
                //printf("batch %u~%u\n", last_tex_idx, j);
                glDrawElements(GL_TRIANGLES, num_tris_batch * 3, GL_UNSIGNED_SHORT, (GLvoid*)((intptr_t)&menu_data_elements[last_tex_idx * 3] - (intptr_t)&menu_data_elements[0]));
            }

            int tex_id = tris[j].texture;
            glActiveTexture(GL_TEXTURE0 + 0);
            if (tex_id == 0)
                glBindTexture(GL_TEXTURE_2D, blank_tex_white);
            else
                glBindTexture(GL_TEXTURE_2D, tex_id);

            if (tris[j].flags) {
                glUniform1f(std3D_uiProgram.uniform_param1, 1.0);
            }
            else {
                glUniform1f(std3D_uiProgram.uniform_param1, 0.0);
            }
            
            last_ui_tex = tris[j].texture;
            last_ui_flags = tris[j].flags;
            last_tex_idx = j;

            do_batch = 0;
        }
        //printf("tri %u: %u,%u,%u\n", j, tris[j].v1, tris[j].v2, tris[j].v3);
        
        
        /*int vert = tris[j].v1;
        stdPlatform_Printf("%u: %f %f %f, %f %f %f, %f %f\n", vert, vertexes[vert].x, vertexes[vert].y, vertexes[vert].z,
                                      vertexes[vert].nx, vertexes[vert].ny, vertexes[vert].nz,
                                      vertexes[vert].tu, vertexes[vert].tv);
        
        vert = tris[j].v2;
        stdPlatform_Printf("%u: %f %f %f, %f %f %f, %f %f\n", vert, vertexes[vert].x, vertexes[vert].y, vertexes[vert].z,
                                      vertexes[vert].nx, vertexes[vert].ny, vertexes[vert].nz,
                                      vertexes[vert].tu, vertexes[vert].tv);
        
        vert = tris[j].v3;
        stdPlatform_Printf("%u: %f %f %f, %f %f %f, %f %f\n", vert, vertexes[vert].x, vertexes[vert].y, vertexes[vert].z,
                                      vertexes[vert].nx, vertexes[vert].ny, vertexes[vert].nz,
                                      vertexes[vert].tu, vertexes[vert].tv);*/
    }
    
    int remaining_batch = GL_tmpUITrisAmt - last_tex_idx;

    if (remaining_batch)
    {
        glDrawElements(GL_TRIANGLES, remaining_batch * 3, GL_UNSIGNED_SHORT, (GLvoid*)((intptr_t)&menu_data_elements[last_tex_idx * 3] - (intptr_t)&menu_data_elements[0]));
    }

    // Done drawing    
    glBindTexture(GL_TEXTURE_2D, blank_tex_white);

    glDisableVertexAttribArray(std3D_uiProgram.attribute_coord3d);
    glDisableVertexAttribArray(std3D_uiProgram.attribute_v_color);
    glDisableVertexAttribArray(std3D_uiProgram.attribute_v_uv);
    
    std3D_ResetUIRenderList();
}

void std3D_DrawSimpleTex(std3DSimpleTexStage* pStage, std3DIntermediateFbo* pFbo, GLuint texId, GLuint texId2, GLuint texId3, flex_t param1, flex_t param2, flex_t param3, int gen_mips)
{
    glBindFramebuffer(GL_FRAMEBUFFER, pFbo->fbo);
    glDepthFunc(GL_ALWAYS);
    glUseProgram(pStage->program);
    
    float menu_w, menu_h, menu_u, menu_v, menu_x;
    menu_w = (double)pFbo->w;
    menu_h = (double)pFbo->h;
    menu_u = 1.0;
    menu_v = 1.0;
    menu_x = 0.0;

    GL_tmpVertices[0].x = menu_x;
    GL_tmpVertices[0].y = 0.0;
    GL_tmpVertices[0].z = 0.0;
    GL_tmpVertices[0].tu = 0.0;
    GL_tmpVertices[0].tv = menu_v;
    *(uint32_t*)&GL_tmpVertices[0].nx = 0;
    GL_tmpVertices[0].color = 0xFFFFFFFF;
    *(uint32_t*)&GL_tmpVertices[0].nz = 0;
    
    GL_tmpVertices[1].x = menu_x;
    GL_tmpVertices[1].y = menu_h;
    GL_tmpVertices[1].z = 0.0;
    GL_tmpVertices[1].tu = 0.0;
    GL_tmpVertices[1].tv = 0.0;
    *(uint32_t*)&GL_tmpVertices[1].nx = 0;
    GL_tmpVertices[1].color = 0xFFFFFFFF;
    *(uint32_t*)&GL_tmpVertices[1].nz = 0;
    
    GL_tmpVertices[2].x = menu_x + menu_w;
    GL_tmpVertices[2].y = menu_h;
    GL_tmpVertices[2].z = 0.0;
    GL_tmpVertices[2].tu = menu_u;
    GL_tmpVertices[2].tv = 0.0;
    *(uint32_t*)&GL_tmpVertices[2].nx = 0;
    GL_tmpVertices[2].color = 0xFFFFFFFF;
    *(uint32_t*)&GL_tmpVertices[2].nz = 0;
    
    GL_tmpVertices[3].x = menu_x + menu_w;
    GL_tmpVertices[3].y = 0.0;
    GL_tmpVertices[3].z = 0.0;
    GL_tmpVertices[3].tu = menu_u;
    GL_tmpVertices[3].tv = menu_v;
    *(uint32_t*)&GL_tmpVertices[3].nx = 0;
    GL_tmpVertices[3].color = 0xFFFFFFFF;
    *(uint32_t*)&GL_tmpVertices[3].nz = 0;
    
    GL_tmpTris[0].v1 = 1;
    GL_tmpTris[0].v2 = 0;
    GL_tmpTris[0].v3 = 2;
    
    GL_tmpTris[1].v1 = 0;
    GL_tmpTris[1].v2 = 3;
    GL_tmpTris[1].v3 = 2;
    
    GL_tmpVerticesAmt = 4;
    GL_tmpTrisAmt = 2;
    
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, texId);
    if (gen_mips)
        glGenerateMipmap(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, texId2 ? texId2 : blank_tex);
    if (texId2 && gen_mips)
        glGenerateMipmap(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, texId3 ? texId3 : blank_tex);
    if (texId3 && gen_mips)
        glGenerateMipmap(GL_TEXTURE_2D);

    GLushort data_elements[32 * 3];

    D3DVERTEX* vertexes = GL_tmpVertices;
    
    glUniform1i(pStage->uniform_tex, 0);
    glUniform1i(pStage->uniform_tex2, 1);
    glUniform1i(pStage->uniform_tex3, 2);
    
    {

    float maxX, maxY, scaleX, scaleY, width, height;

    scaleX = 1.0/((double)pFbo->w / 2.0);
    scaleY = 1.0/((double)pFbo->h / 2.0);
    maxX = 1.0;
    maxY = 1.0;
    width = pFbo->w;
    height = pFbo->h;
    
    float d3dmat[16] = {
       maxX*scaleX,      0,                                          0,      0, // right
       0,                                       -maxY*scaleY,               0,      0, // up
       0,                                       0,                                          1,     0, // forward
       -(width/2)*scaleX,  (height/2)*scaleY,     -1,      1  // pos
    };
    
    glUniformMatrix4fv(pStage->uniform_mvp, 1, GL_FALSE, d3dmat);
    glViewport(0, 0, width, height);
    glUniform2f(pStage->uniform_iResolution, pFbo->iw, pFbo->ih);

    glUniform1f(pStage->uniform_param1, param1);
    glUniform1f(pStage->uniform_param2, param2);
    glUniform1f(pStage->uniform_param3, param3);

    }
    
    rdTri* tris = GL_tmpTris;
    glBindBuffer(GL_ARRAY_BUFFER, menu_vbo_all);
    glBufferData(GL_ARRAY_BUFFER, GL_tmpVerticesAmt * sizeof(D3DVERTEX), GL_tmpVertices, GL_STREAM_DRAW);

    if (pStage->attribute_coord3d >= 0) {
        glVertexAttribPointer(pStage->attribute_coord3d, 3, GL_FLOAT, GL_FALSE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, x));
        glEnableVertexAttribArray(pStage->attribute_coord3d);
    }
    if (pStage->attribute_v_color >= 0) {
        glVertexAttribPointer(pStage->attribute_v_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, color));
        glEnableVertexAttribArray(pStage->attribute_v_color);
    }
    if (pStage->attribute_v_uv >= 0) {
        glVertexAttribPointer(pStage->attribute_v_uv, 2, GL_FLOAT, GL_FALSE, sizeof(D3DVERTEX), (GLvoid*)offsetof(D3DVERTEX, tu));
        glEnableVertexAttribArray(pStage->attribute_v_uv);
    }
    
    rdDDrawSurface* last_tex = (rdDDrawSurface*)(intptr_t)-1;
    int last_tex_idx = 0;
    //GLushort* data_elements = malloc(sizeof(GLushort) * 3 * GL_tmpTrisAmt);
    for (int j = 0; j < GL_tmpTrisAmt; j++)
    {
        data_elements[(j*3)+0] = tris[j].v1;
        data_elements[(j*3)+1] = tris[j].v2;
        data_elements[(j*3)+2] = tris[j].v3;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, menu_ibo_triangle);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GL_tmpTrisAmt * 3 * sizeof(GLushort), data_elements, GL_STREAM_DRAW);

    // Perf: draw the count just uploaded. Asking GL for the buffer's size makes the
    // CPU wait for the GPU, every frame.
    glDrawElements(GL_TRIANGLES, (GLsizei)(GL_tmpTrisAmt * 3), GL_UNSIGNED_SHORT, 0);

    if (pStage->attribute_v_uv >= 0) glDisableVertexAttribArray(pStage->attribute_v_uv);
    if (pStage->attribute_v_color >= 0) glDisableVertexAttribArray(pStage->attribute_v_color);
    if (pStage->attribute_coord3d >= 0) glDisableVertexAttribArray(pStage->attribute_coord3d);
    
    //free(data_elements);
        
    //glBindTexture(GL_TEXTURE_2D, 0);
}

// Added (remaster): .cube LUT loader for the grading pass. Re-checks the file twice a
// second and reloads when it changes, so a LUT re-exported from Resolve shows up live.
// Path is r_gradeLut, relative to the game data folder.
#include <sys/stat.h>
#include <ctype.h>
static char std3D_gradeLutPath[SITHCVAR_MAX_STRLEN];
static long long std3D_gradeLutStamp = 0;
static uint32_t std3D_gradeLutLastCheckMs = 0;
static int std3D_gradeLutChecked = 0;

static long long std3D_GradeFileStamp(const struct stat* pSt)
{
#ifdef __APPLE__
    return (long long)pSt->st_mtimespec.tv_sec * 1000000000LL + pSt->st_mtimespec.tv_nsec + (long long)pSt->st_size * 7;
#else
    return (long long)pSt->st_mtime * 1000000000LL + (long long)pSt->st_size * 7;
#endif
}

static int std3D_GradeParseCube(const char* pPath, float** ppData, int* pSize, float* pMin, float* pMax)
{
    FILE* f = fopen(pPath, "r");
    if (!f) return 0;

    char line[512];
    int size = 0, count = 0, total = 0;
    float* data = NULL;
    pMin[0] = pMin[1] = pMin[2] = 0.0f;
    pMax[0] = pMax[1] = pMax[2] = 1.0f;

    while (fgets(line, sizeof(line), f))
    {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r') continue;

        if (!strncmp(p, "LUT_1D_SIZE", 11)) {
            stdPlatform_Printf("std3D: grade LUT `%s` is 1D, only 3D .cube LUTs are supported\n", pPath);
            goto fail;
        }
        if (!strncmp(p, "LUT_3D_SIZE", 11)) {
            size = atoi(p + 11);
            if (size < 2 || size > 256 || data) goto fail;
            total = size * size * size;
            data = (float*)malloc(sizeof(float) * 3 * total);
            if (!data) goto fail;
            continue;
        }
        if (!strncmp(p, "DOMAIN_MIN", 10)) { sscanf(p + 10, "%f %f %f", &pMin[0], &pMin[1], &pMin[2]); continue; }
        if (!strncmp(p, "DOMAIN_MAX", 10)) { sscanf(p + 10, "%f %f %f", &pMax[0], &pMax[1], &pMax[2]); continue; }
        if (!strncmp(p, "LUT_3D_INPUT_RANGE", 18)) {
            float lo = 0.0f, hi = 1.0f;
            if (sscanf(p + 18, "%f %f", &lo, &hi) == 2) {
                pMin[0] = pMin[1] = pMin[2] = lo;
                pMax[0] = pMax[1] = pMax[2] = hi;
            }
            continue;
        }
        if (isalpha((unsigned char)*p)) continue; // TITLE and other keywords

        float r, g, b;
        if (sscanf(p, "%f %f %f", &r, &g, &b) == 3) {
            if (!data || count >= total) goto fail;
            data[count * 3 + 0] = r;
            data[count * 3 + 1] = g;
            data[count * 3 + 2] = b;
            count++;
        }
    }
    fclose(f);
    if (!data || count != total) {
        free(data);
        stdPlatform_Printf("std3D: grade LUT `%s` has %d entries, expected %d\n", pPath, count, total);
        return 0;
    }
    *ppData = data;
    *pSize = size;
    return 1;

fail:
    fclose(f);
    free(data);
    stdPlatform_Printf("std3D: could not read grade LUT `%s`\n", pPath);
    return 0;
}

static void std3D_GradeUpdateLut(void)
{
    uint32_t nowMs = stdPlatform_GetTimeMsec();
    int bPathChanged = strcmp(std3D_gradeLutPath, jkPlayer_gradeLut) != 0;
    if (!bPathChanged && std3D_gradeLutChecked && (nowMs - std3D_gradeLutLastCheckMs) < 500 && (std3D_gradeLutTex || std3D_gradeLutStamp == -1))
        return;
    std3D_gradeLutLastCheckMs = nowMs;
    std3D_gradeLutChecked = 1;
    stdString_SafeStrCopy(std3D_gradeLutPath, jkPlayer_gradeLut, sizeof(std3D_gradeLutPath));

    struct stat st;
    if (!std3D_gradeLutPath[0] || stat(std3D_gradeLutPath, &st) != 0) {
        if (bPathChanged || std3D_gradeLutStamp != -1)
            stdPlatform_Printf("std3D: grade LUT `%s` not found, grading without a LUT\n", std3D_gradeLutPath);
        std3D_gradeLutStamp = -1;
        std3D_gradeLutSize = 0;
        return;
    }

    long long stamp = std3D_GradeFileStamp(&st);
    if (!bPathChanged && std3D_gradeLutTex && stamp == std3D_gradeLutStamp)
        return;
    std3D_gradeLutStamp = stamp;

    float* data = NULL;
    int size = 0;
    if (!std3D_GradeParseCube(std3D_gradeLutPath, &data, &size, std3D_gradeLutDomainMin, std3D_gradeLutDomainMax)) {
        std3D_gradeLutSize = 0;
        return;
    }

    if (!std3D_gradeLutTex) glGenTextures(1, &std3D_gradeLutTex);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_3D, std3D_gradeLutTex);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB16F, size, size, size, 0, GL_RGB, GL_FLOAT, data);
    glBindTexture(GL_TEXTURE_3D, 0);
    glActiveTexture(GL_TEXTURE0);
    free(data);

    std3D_gradeLutSize = size;
    stdPlatform_Printf("std3D: loaded grade LUT `%s` (%dx%dx%d)\n", std3D_gradeLutPath, size, size, size);
}

void std3D_DrawSceneFbo()
{
    //printf("Draw scene FBO\n");
    glEnable(GL_BLEND);
    
    glBlendEquation(GL_FUNC_ADD);

    glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->window.fbo);
    glClear( GL_COLOR_BUFFER_BIT );

    static float frameNum = 1.0;
    //frameNum += (rand() % 16);

    int draw_ssao = jkPlayer_enableSSAO;
    int draw_bloom = jkPlayer_enableBloom;

    float add_luma = (((float)rdroid_curColorEffects.add.x / 255.0f) * 0.2125)
                     + (((float)rdroid_curColorEffects.add.y / 255.0f)* 0.7154)
                     + (((float)rdroid_curColorEffects.add.z / 255.0f) * 0.0721); // FLEXTODO

    // HACK: Force blinding shouldn't show the SSAO
    if (add_luma >= 0.7) {
        draw_ssao = 0;
    }

    if (!jkGame_isDDraw && !jkGuiBuildMulti_bRendering)
    {
        return;
    }

    // Added (remaster): with grading on, the scene composites into a half-float buffer
    // first, then one pass grades it onto the window. The HUD is drawn later, ungraded.
    int draw_grade = jkPlayer_enableGrade && (std3D_pFb->enable_extra & 4) && std3D_gradeStage.program;
    std3DIntermediateFbo* pOut = draw_grade ? &std3D_pFb->grade : &std3D_pFb->window;
    if (draw_grade)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->grade.fbo);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    if (jkGuiBuildMulti_bRendering) {
        draw_ssao = 0;
        //draw_bloom = 0;
    }

    if (draw_bloom)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->blur1.fbo);
        glClear( GL_COLOR_BUFFER_BIT );
        glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->blur2.fbo);
        glClear( GL_COLOR_BUFFER_BIT );
        glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->blur3.fbo);
        glClear( GL_COLOR_BUFFER_BIT );
        glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->blur4.fbo);
        glClear( GL_COLOR_BUFFER_BIT );
        //glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->blurBlend.fbo);
        //glClear( GL_COLOR_BUFFER_BIT );
    }

    // Clear SSAO stuff
    if (draw_ssao)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->ssaoBlur1.fbo);
        glClear( GL_COLOR_BUFFER_BIT );
        glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->ssaoBlur2.fbo);
        glClear( GL_COLOR_BUFFER_BIT );
    }

    float rad_scale = (float)std3D_pFb->w / 640.0;

    if (!draw_ssao)
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        std3D_DrawSimpleTex(&std3D_texFboStage, pOut, std3D_pFb->tex0, 0, 0, 1.0, 1.0, jkPlayer_gamma, 0);
        //std3D_DrawSimpleTex(&std3D_texFboStage, pOut, std3D_pFb->tex1, 0, 0, 0.0, 1.0, jkPlayer_gamma, 0); // test emission output
    }
    else
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        
        std3D_DrawSimpleTex(&std3D_ssaoStage, &std3D_pFb->ssaoBlur1, std3D_pFb->tex2, std3D_pFb->tex3, tiledrand_texture, frameNum, 0.0, 0.0, 0); // test ssao output
        std3D_DrawSimpleTex(&std3D_blurStage, &std3D_pFb->ssaoBlur2, std3D_pFb->ssaoBlur1.tex, 0, 0, 14.0, 3.0, 1.0 * rad_scale, 1);
        //std3D_DrawSimpleTex(&std3D_blurStage, &std3D_pFb->ssaoBlur3, std3D_pFb->ssaoBlur2.tex, 0, 0, 8.0, 3.0, 4.0);

        glBlendFunc(GL_SRC_ALPHA, GL_SRC_ALPHA);
        std3D_DrawSimpleTex(&std3D_ssaoMixStage, pOut, std3D_pFb->ssaoBlur2.tex, std3D_pFb->tex0, 0, 0.0, 0.0, jkPlayer_gamma, 0);
    }

    // Added (remaster): blurred copies of the scene, shared by glossy reflections and haze.
    int bSceneBlur = (std3D_pFb->enable_extra & 8) && (std3D_AtmosphereActive() || std3D_ReflectionsActive());
    if (bSceneBlur)
    {
        glDisable(GL_BLEND);
        // 8 directions x 2 rings = 17 samples: the result is meant to be very soft.
        std3D_DrawSimpleTex(&std3D_blurStage, &std3D_pFb->hazeBlur1, std3D_pFb->tex0, 0, 0, 8.0, 2.0, 2.0 * rad_scale, 1);
        std3D_DrawSimpleTex(&std3D_blurStage, &std3D_pFb->hazeBlur2, std3D_pFb->hazeBlur1.tex, 0, 0, 8.0, 2.0, 2.0 * rad_scale, 1);
        glEnable(GL_BLEND);
    }

    // Added (remaster): screen-space reflections, added on top of the composited scene.
    if (std3D_ReflectionsActive() && std3D_ssrHaveCamera && std3D_ssrStage.program && !jkGuiBuildMulti_bRendering)
    {
        glUseProgram(std3D_ssrStage.program);
        glUniform4fv(std3D_ssrUniformProj, 1, std3D_ssrProj);
        glUniform4fv(std3D_ssrUniformScreen, 1, std3D_ssrScreen);
        glUniform3fv(std3D_ssrUniformUpCam, 1, std3D_ssrUpCam);
        glUniform1f(std3D_ssrUniformWetness, jkPlayer_wetness);
        glUniform1f(std3D_ssrUniformStrength, jkPlayer_reflectionStrength);
        glUniform1f(std3D_ssrUniformRoughness, jkPlayer_reflectionRoughness);
        glUniform1i(std3D_ssrUniformTexMat, 5);
        glActiveTexture(GL_TEXTURE0 + 5);
        glBindTexture(GL_TEXTURE_2D, std3D_pFb->tex5);
        glActiveTexture(GL_TEXTURE0);
        glBlendFunc(GL_ONE, GL_ONE);
        std3D_DrawSimpleTex(&std3D_ssrStage, pOut, std3D_pFb->tex0, std3D_pFb->tex4,
                            bSceneBlur ? std3D_pFb->hazeBlur1.tex : std3D_pFb->tex0, 0.0, 0.0, 0.0, 0);
    }

    // Added (remaster): volumetric fog, where the level has a baked light volume (sithAtmos).
    // It replaces the haze below.
    int bFogDone = 0;
    if (jkPlayer_bVolumetricFog && jkPlayer_bHaze && std3D_AtmosphereActive() && (std3D_pFb->enable_extra & 8)
        && std3D_fogStage.program && std3D_ssrHaveCamera)
    {
        sithAtmosGrid *gF = NULL, *gC = NULL;
        rdVector3 cam = {std3D_fogCamPos[0], std3D_fogCamPos[1], std3D_fogCamPos[2]};
        if (sithAtmos_Update(&cam, &gF, &gC))
        {
            sithAtmosGrid* g[2] = {gF, gC};
            for (int k = 0; k < 2; k++)
            {
                if (!std3D_fogTex[k])
                {
                    glGenTextures(1, &std3D_fogTex[k]);
                    glBindTexture(GL_TEXTURE_3D, std3D_fogTex[k]);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
                    g[k]->dirty = 1;
                }
                if (g[k]->dirty && g[k]->data)
                {
                    glBindTexture(GL_TEXTURE_3D, std3D_fogTex[k]);
                    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA16F, g[k]->dims[0], g[k]->dims[1], g[k]->dims[2], 0, GL_RGBA, GL_FLOAT, g[k]->data);
                    g[k]->dirty = 0;
                }
            }
            glBindTexture(GL_TEXTURE_3D, 0);
            glUseProgram(std3D_fogStage.program);
            for (int k = 0; k < 2; k++)
            {
                glActiveTexture(GL_TEXTURE0 + 6 + k);
                glBindTexture(GL_TEXTURE_3D, std3D_fogTex[k]);
                glUniform1i(std3D_fogU[FOGU_FINE + k], 6 + k);
                float sp = g[k]->spacing;
                glUniform3f(std3D_fogU[FOGU_FORIGIN + 2 * k], (g[k]->origin[0] - 0.5f) * sp, (g[k]->origin[1] - 0.5f) * sp, (g[k]->origin[2] - 0.5f) * sp);
                glUniform3f(std3D_fogU[FOGU_FSIZE + 2 * k], g[k]->dims[0] * sp, g[k]->dims[1] * sp, g[k]->dims[2] * sp);
            }
            glActiveTexture(GL_TEXTURE0);
            glUniform4fv(std3D_fogU[FOGU_PROJ], 1, std3D_ssrProj);
            glUniform4fv(std3D_fogU[FOGU_SCREEN], 1, std3D_ssrScreen);
            glUniform3fv(std3D_fogU[FOGU_WX], 1, &std3D_skyWorldAxes[0]);
            glUniform3fv(std3D_fogU[FOGU_WY], 1, &std3D_skyWorldAxes[3]);
            glUniform3fv(std3D_fogU[FOGU_WZ], 1, &std3D_skyWorldAxes[6]);
            glUniform3fv(std3D_fogU[FOGU_CAM], 1, std3D_fogCamPos);
            glUniform4f(std3D_fogU[FOGU_FOG], jkPlayer_fogDensity, jkPlayer_fogGain, jkPlayer_fogMaxDist, (float)(SDL_GetTicks() % 1000000) / 1000.0f);
            glUniform4f(std3D_fogU[FOGU_FOG2], jkPlayer_fogNoise, 0.6f, 3.0f, 32.0f);
            {
                float hd, hz, hf;
                sithAtmos_GetHeightFog(&hd, &hz, &hf);
                glUniform4f(std3D_fogU[FOGU_HFOG], hd * jkPlayer_heightFog, hz, hf, 0.0f);
                // Hand-placed fog volumes near the camera.
                float vc[SITHATMOS_MAX_SHADER_VOLUMES * 4], vs[SITHATMOS_MAX_SHADER_VOLUMES * 4];
                float vp[SITHATMOS_MAX_SHADER_VOLUMES * 4], vt[SITHATMOS_MAX_SHADER_VOLUMES * 4];
                int nv = jkPlayer_fogVolumes ? sithAtmos_GetVolumes(&cam, vc, vs, vp, vt, SITHATMOS_MAX_SHADER_VOLUMES) : 0;
                glUniform1i(std3D_fogU[FOGU_NVOL], nv);
                if (nv) {
                    glUniform4fv(std3D_fogU[FOGU_VCENTER], nv, vc);
                    glUniform4fv(std3D_fogU[FOGU_VSIZE], nv, vs);
                    glUniform4fv(std3D_fogU[FOGU_VPARAMS], nv, vp);
                    glUniform4fv(std3D_fogU[FOGU_VTINT], nv, vt);
                }
            }
            glBlendFunc(GL_ONE, GL_SRC_ALPHA);
            std3D_DrawSimpleTex(&std3D_fogStage, pOut, std3D_pFb->tex4, 0, 0, 0.0, 0.0, 0.0, 0);
            for (int k = 0; k < 2; k++)
            {
                glActiveTexture(GL_TEXTURE0 + 6 + k);
                glBindTexture(GL_TEXTURE_3D, 0);
            }
            glActiveTexture(GL_TEXTURE0);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            bFogDone = 1;
        }
    }

    // Added (remaster): haze. Distance fog from the depth target, coloured by a heavily
    // blurred copy of the scene so it glows near lights. Alpha-blended over the scene.
    if (!bFogDone && jkPlayer_bHaze && std3D_AtmosphereActive() && (std3D_pFb->enable_extra & 8) && std3D_hazeStage.program)
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        std3D_DrawSimpleTex(&std3D_hazeStage, pOut, std3D_pFb->hazeBlur2.tex, std3D_pFb->tex4, 0,
                            jkPlayer_hazeDensity, jkPlayer_hazeStrength, 0.0, 0);
    }

    // Added (remaster): light shafts streaming from the strongest glow lights on screen.
    if (jkPlayer_bLightShafts && std3D_AtmosphereActive() && std3D_ssrHaveCamera && std3D_shaftStage.program && (std3D_frameNumGlow > 0 || sithSky_GetActive()))
    {
        // Pick up to 4: in front of the camera, near the screen, brightest and closest first.
        float aPos[16], aCol[16], aScore[4];
        int n = 0;
        for (int i = 0; i < std3D_frameNumGlow; i++)
        {
            float x = std3D_frameGlowPos[i*4+0], y = std3D_frameGlowPos[i*4+1], z = std3D_frameGlowPos[i*4+2];
            if (y < 0.05f) continue;
            float cx = std3D_ssrProj[0] + x * std3D_ssrProj[2] / y;
            float cy = std3D_ssrProj[1] - z * std3D_ssrProj[2] / y;
            float u = cx / std3D_ssrScreen[0], v = 1.0f - cy / std3D_ssrScreen[1];
            if (u < -0.2f || u > 1.2f || v < -0.2f || v > 1.2f) continue;
            float score = std3D_frameGlowCol[i*4+3] / (y > 0.3f ? y : 0.3f);
            int slot = n;
            if (n == 4) {
                slot = 0;
                for (int k = 1; k < 4; k++) if (aScore[k] < aScore[slot]) slot = k;
                if (score <= aScore[slot]) continue;
            } else n++;
            aScore[slot] = score;
            aPos[slot*4+0] = u; aPos[slot*4+1] = v; aPos[slot*4+2] = std3D_frameGlowCol[i*4+3]; aPos[slot*4+3] = 0.0f;
            aCol[slot*4+0] = std3D_frameGlowCol[i*4+0]; aCol[slot*4+1] = std3D_frameGlowCol[i*4+1];
            aCol[slot*4+2] = std3D_frameGlowCol[i*4+2]; aCol[slot*4+3] = 1.0f;
        }
        // Added (remaster): the volumetric sky's sun/moon, when it's in front of the camera.
        const sithSkyPreset* sp = sithSky_GetActive();
        if (sp && n < 4)
        {
            float sun[3];
            sithSky_GetSunDir(sp, sun);
            const float* A = std3D_skyWorldAxes;
            float x = sun[0]*A[0] + sun[1]*A[3] + sun[2]*A[6];
            float y = sun[0]*A[1] + sun[1]*A[4] + sun[2]*A[7];
            float z = sun[0]*A[2] + sun[1]*A[5] + sun[2]*A[8];
            if (y > 0.05f)
            {
                float cx = std3D_ssrProj[0] + x * std3D_ssrProj[2] / y;
                float cy = std3D_ssrProj[1] - z * std3D_ssrProj[2] / y;
                float u = cx / std3D_ssrScreen[0], v = 1.0f - cy / std3D_ssrScreen[1];
                if (u > -0.6f && u < 1.6f && v > -0.6f && v < 1.6f)
                {
                    aPos[n*4+0] = u; aPos[n*4+1] = v; aPos[n*4+2] = 1.0f; aPos[n*4+3] = 1.0f;
                    aCol[n*4+0] = sp->sunColor[0]; aCol[n*4+1] = sp->sunColor[1]; aCol[n*4+2] = sp->sunColor[2]; aCol[n*4+3] = 1.0f;
                    n++;
                }
            }
        }
        if (n)
        {
            glUseProgram(std3D_shaftStage.program);
            glUniform1i(std3D_shaftUniformNum, n);
            glUniform4fv(std3D_shaftUniformPos, n, aPos);
            glUniform4fv(std3D_shaftUniformCol, n, aCol);
            glBlendFunc(GL_ONE, GL_ONE);
            std3D_DrawSimpleTex(&std3D_shaftStage, pOut, std3D_pFb->tex1, std3D_pFb->tex4, std3D_pFb->tex0, jkPlayer_shaftStrength, 0.0, 0.0, 0);
        }
    }

    glBlendFunc(GL_SRC_ALPHA, GL_SRC_ALPHA);
    if (!draw_bloom)
        std3D_DrawSimpleTex(&std3D_texFboStage, pOut, std3D_pFb->tex1, 0, 0, 1.0, 1.0, jkPlayer_gamma, 0);

    if (draw_bloom)
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        
        std3D_DrawSimpleTex(&std3D_blurStage, &std3D_pFb->blur1, std3D_pFb->tex1, 0, 0, 16.0, 3.0, 0.5 * rad_scale, 1); // Added (remaster): quarter-res target, so a quarter of the full-res radius keeps the same look
        std3D_DrawSimpleTex(&std3D_blurStage, &std3D_pFb->blur2, std3D_pFb->blur1.tex, 0, 0, 16.0, 3.0, 2.0 * rad_scale, 1);
        std3D_DrawSimpleTex(&std3D_blurStage, &std3D_pFb->blur3, std3D_pFb->blur2.tex, 0, 0, 16.0, 3.0, 2.0 * rad_scale, 1);
        std3D_DrawSimpleTex(&std3D_blurStage, &std3D_pFb->blur4, std3D_pFb->blur3.tex, 0, 0, 16.0, 3.0, 2.0 * rad_scale, 1);

        float bloom_intensity = 1.0;
        float bloom_gamma = 1.0;
        glBlendFunc(GL_SRC_ALPHA, GL_SRC_ALPHA);
        /*std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->blurBlend, std3D_pFb->tex1, 0, 0, 1.0, bloom_intensity * 1.0, bloom_gamma, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->blurBlend, std3D_pFb->blur1.tex, 0, 0, 1.0, bloom_intensity * 1.0, bloom_gamma, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->blurBlend, std3D_pFb->blur2.tex, 0, 0, 1.0, bloom_intensity * 1.2, bloom_gamma, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->blurBlend, std3D_pFb->blur3.tex, 0, 0, 1.0, bloom_intensity * 1.0, bloom_gamma, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, &std3D_pFb->blurBlend, std3D_pFb->blur4.tex, 0, 0, 1.0, bloom_intensity * 1.2, bloom_gamma, 0);
        */

        glBlendFunc(GL_SRC_ALPHA, GL_SRC_ALPHA);
        //std3D_DrawSimpleTex(&std3D_texFboStage, pOut, std3D_pFb->blurBlend.tex, 0, 0, 1.0, 1.0, 1.0, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, pOut, std3D_pFb->tex1, 0, 0, 1.0, 1.5, jkPlayer_gamma, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, pOut, std3D_pFb->blur1.tex, 0, 0, 1.0, bloom_intensity * 1.5, jkPlayer_gamma, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, pOut, std3D_pFb->blur2.tex, 0, 0, 1.0, bloom_intensity * 1.0, jkPlayer_gamma, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, pOut, std3D_pFb->blur3.tex, 0, 0, 1.0, bloom_intensity * 1.0, jkPlayer_gamma, 0);
        std3D_DrawSimpleTex(&std3D_texFboStage, pOut, std3D_pFb->blur4.tex, 0, 0, 1.0, bloom_intensity * 0.8, jkPlayer_gamma, 0);

        // Added (remaster): lens flares (streak, ghosts, halo) from the bloom source.
        if (jkPlayer_bLensFlares && std3D_flareStage.program && !jkGuiBuildMulti_bRendering)
        {
            glBlendFunc(GL_ONE, GL_ONE);
            std3D_DrawSimpleTex(&std3D_flareStage, pOut, std3D_pFb->blur1.tex, 0, 0, jkPlayer_flareStrength,
                                (float)std3D_pFb->w / (float)(std3D_pFb->h ? std3D_pFb->h : 1), 0.0, 0);
            glBlendFunc(GL_SRC_ALPHA, GL_SRC_ALPHA);
        }
    }
    // Added (remaster): grade the composited scene onto the window.
    if (draw_grade)
    {
        std3D_GradeUpdateLut();
        glUseProgram(std3D_gradeStage.program);
        glActiveTexture(GL_TEXTURE0 + 3);
        glBindTexture(GL_TEXTURE_3D, std3D_gradeLutSize ? std3D_gradeLutTex : 0);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(std3D_gradeUniformLut, 3);
        glUniform1f(std3D_gradeUniformLutSize, (float)std3D_gradeLutSize);
        glUniform3fv(std3D_gradeUniformDomainMin, 1, std3D_gradeLutDomainMin);
        glUniform3fv(std3D_gradeUniformDomainMax, 1, std3D_gradeLutDomainMax);

        glDisable(GL_BLEND);
        std3D_DrawSimpleTex(&std3D_gradeStage, &std3D_pFb->window, std3D_pFb->grade.tex, 0, 0,
                            jkPlayer_gradeExposure, jkPlayer_gradeToneCurve ? 1.0 : 0.0,
                            std3D_gradeLutSize ? jkPlayer_gradeStrength : 0.0, 0);
        glEnable(GL_BLEND);

        glActiveTexture(GL_TEXTURE0 + 3);
        glBindTexture(GL_TEXTURE_3D, 0);
        glActiveTexture(GL_TEXTURE0);
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void std3D_DoTex(rdDDrawSurface* tex, rdTri* tri, int tris_left)
{
    if (!tex) {
        glActiveTexture(GL_TEXTURE0 + 3);
        glBindTexture(GL_TEXTURE_2D, blank_tex); // emissive
        glActiveTexture(GL_TEXTURE0 + 4);
        glBindTexture(GL_TEXTURE_2D, blank_tex); // displace

        glActiveTexture(GL_TEXTURE0 + 0);
        glBindTexture(GL_TEXTURE_2D, blank_tex_white);
        glUniform1i(uniform_tex_mode, TEX_MODE_TEST);
        glUniform1i(uniform_blend_mode, 2);
        return;
    }
    int tex_id = tex->texture_id;
    glActiveTexture(GL_TEXTURE0 + 0);
    if (tex_id == 0)
        glBindTexture(GL_TEXTURE_2D, blank_tex_white);
    else
        glBindTexture(GL_TEXTURE_2D, tex_id);

    int emiss_tex_id = tex->emissive_texture_id;
    glActiveTexture(GL_TEXTURE0 + 3);
    if (emiss_tex_id == 0) {
        glBindTexture(GL_TEXTURE_2D, blank_tex);
    }
    else {
        //printf("emissive tex id %x\n", emiss_tex_id);
        glBindTexture(GL_TEXTURE_2D, emiss_tex_id);

        // HACK
        if (tri[0].flags & 0x600) {
            //glUniform1i(uniform_blend_mode, 6);
            //last_flags |= 0x200;
        }

        
        for (int i = 0; i < tris_left; i++) {
            if (tri[i].texture != tex) break;
            if (tri[i].flags & 0x600) {
                //tri[i].flags |= 0x200;
            }
        }
    }

    int displace_tex_id = tex->displacement_texture_id;
    glActiveTexture(GL_TEXTURE0 + 4);
    if (displace_tex_id == 0) {
        glBindTexture(GL_TEXTURE_2D, blank_tex);
    }
    else {
        glBindTexture(GL_TEXTURE_2D, displace_tex_id);
    }
    // Added (remaster): material maps (ORM, normal), when the HD pack provides them.
    glActiveTexture(GL_TEXTURE0 + 5);
    glBindTexture(GL_TEXTURE_2D, tex->orm_texture_id ? tex->orm_texture_id : blank_tex);
    glActiveTexture(GL_TEXTURE0 + 6);
    glBindTexture(GL_TEXTURE_2D, tex->normal_texture_id ? tex->normal_texture_id : blank_tex);
    glUniform1i(uniform_hasOrm, tex->orm_texture_id != 0);
    glUniform1i(uniform_hasNormal, tex->normal_texture_id != 0);
    glActiveTexture(GL_TEXTURE0 + 4);
    //if (tex->emissive_factor[0] != 0.0 || tex->emissive_factor[1] != 0.0 || tex->emissive_factor[2] != 0.0)
    //    stdPlatform_Printf("%f %f %f\n", tex->emissive_factor[0], tex->emissive_factor[1], tex->emissive_factor[2]);
    float emissive_mult = (jkPlayer_enableBloom ? 1.0 : 5.0);
    glUniform3f(uniform_emissiveFactor, tex->emissive_factor[0] * emissive_mult, tex->emissive_factor[1] * emissive_mult, tex->emissive_factor[2] * emissive_mult);
    glUniform4f(uniform_albedoFactor, tex->albedo_factor[0], tex->albedo_factor[1], tex->albedo_factor[2], tex->albedo_factor[3]);
    if (tex->displacement_factor) {
        //printf("%f\n", tex->displacement_factor);
        //tex->displacement_factor = -0.4;
    }
    glUniform1f(uniform_displacement_factor, tex->displacement_factor);
    glActiveTexture(GL_TEXTURE0 + 0);

    if (!jkPlayer_enableTextureFilter)
        glUniform1i(uniform_tex_mode, tex->is_16bit ? TEX_MODE_16BPP : TEX_MODE_WORLDPAL);
    else
        glUniform1i(uniform_tex_mode, tex->is_16bit ? TEX_MODE_BILINEAR_16BPP : TEX_MODE_BILINEAR);
    
     glActiveTexture(GL_TEXTURE0 + 0);

    if (tex_id == 0)
        glUniform1i(uniform_tex_mode, TEX_MODE_TEST);
}

void std3D_DrawRenderList()
{
    if (Main_bHeadless) return;

    //printf("Draw render list\n");
    glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->fbo);
    glUseProgram(programDefault);

    std3D_SetSceneDrawBuffers();
    
    last_tex = NULL;

    // Generate aVertices list
    D3DVERTEX* vertexes = GL_tmpVertices;

    float maxX, maxY, scaleX, scaleY, width, height;

    float internalWidth = Video_menuBuffer.format.width;
    float internalHeight = Video_menuBuffer.format.height;

    if (jkGuiBuildMulti_bRendering) {
        internalWidth = 640.0;
        internalHeight = 480.0;
    }

    maxX = 1.0;
    maxY = 1.0;
    scaleX = 1.0/((double)internalWidth / 2.0);
    scaleY = 1.0/((double)internalHeight / 2.0);
    width = std3D_pFb->w;
    height = std3D_pFb->h;

    if (jkGuiBuildMulti_bRendering) {
        width = 640;
        height = 480;
    }

    // JKDF2's vertical FOV is fixed with their projection, for whatever reason. 
    // This ends up resulting in the view looking squished vertically at wide/ultrawide aspect ratios.
    // To compensate, we zoom the y axis here.
    // I also went ahead and fixed vertical displays in the same way because it seems to look better.
    float zoom_yaspect = 1.0;//(width/height);
    float zoom_xaspect = 1.0;//(height/width);

    if (height > width)
    {
        zoom_yaspect = 1.0;
    }

    if (width > height)
    {
        zoom_xaspect = 1.0;
    }

    // We no longer need all the weird squishing
    if (!jkGuiBuildMulti_bRendering) {
        zoom_yaspect = 1.0;
        zoom_xaspect = 1.0;
    }
    
    float shift_add_x = 0;
    float shift_add_y = 0;

    if (jkGuiBuildMulti_bRendering) {
        float menu_w, menu_h, menu_x;
        menu_w = (double)std3D_pFb->w;
        menu_h = (double)std3D_pFb->h;

        // Keep 4:3 aspect
        menu_x = (menu_w - (menu_h * (640.0 / 480.0))) / 2.0;

        width = std3D_pFb->w;
        height = std3D_pFb->h;

        zoom_xaspect = (height/width);

        shift_add_x = (((1.0 - ((menu_x * zoom_xaspect) / std3D_pFb->w)) + 0.15) * zoom_xaspect);
        shift_add_y = -0.5;
        zoom_yaspect = 1.0;
    }

    glBindBuffer(GL_ARRAY_BUFFER, world_vbo_all);
    glBufferData(GL_ARRAY_BUFFER, GL_tmpVerticesAmt * sizeof(D3DVERTEX), vertexes, GL_STREAM_DRAW);
    
    glUniform1i(uniform_tex_mode, TEX_MODE_TEST);
    glUniform1i(uniform_blend_mode, 2);
    glActiveTexture(GL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, blank_tex);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, worldpal_lights_texture);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, worldpal_texture);
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_2D, blank_tex_white);
    
    glUniform1i(uniform_tex, 0);
    glUniform1i(uniform_worldPalette, 1);
    glUniform1i(uniform_worldPaletteLights, 2);
    glUniform1i(uniform_texEmiss, 3);
    glUniform1i(uniform_displacement_map, 4);
    glUniform1i(uniform_texOrm, 5);    // Added (remaster)
    glUniform1i(uniform_texNormal, 6); // Added (remaster)
    
    {
    
    float d3dmat[16] = {
       (float)(maxX*scaleX*zoom_xaspect),      0,                                          0,      0, // right
       0,                                       (float)(-maxY*scaleY*zoom_yaspect),               0,      0, // up
       0,                                       0,                                          1,     0, // forward
       (float)(-(internalWidth/2)*scaleX*zoom_xaspect + shift_add_x),  (float)((internalHeight/2)*scaleY*zoom_yaspect + shift_add_y),     (float)((!rdCamera_g_pCurCamera || rdCamera_g_pCurCamera->projectType == rdCameraProjectType_Perspective) ? -1 : 1),      1  // pos
    };
    
    glUniformMatrix4fv(uniform_mvp, 1, GL_FALSE, d3dmat);
    glViewport(0, 0, width, height);
    
    }

    glUniform2f(uniform_iResolution, width, height);

    // Added (remaster): per-pixel dynamic lights. The camera's light list (blaster bolts,
    // sabers, explosions, headlights) goes to the shader in camera space, plus what it
    // needs to rebuild each pixel's camera-space position from depth.
    {
        int bPpl = jkPlayer_bPerPixelLighting && rdCamera_g_pCurCamera && rdCamera_g_pCurCamera->pCanvas
                   && !jkGuiBuildMulti_bRendering && attribute_v_invz >= 0;
        glUniform1i(uniform_ppl, bPpl);
        glUniform1i(uniform_lmOn, 0); // Added (remaster): set below when there's a light map
        glUniform1i(uniform_ssrWrite, bPpl && std3D_GBufferActive()); // Added (remaster)
        if (bPpl)
        {
            static float aPos[STD3D_PPL_MAX_LIGHTS * 4];
            static float aCol[STD3D_PPL_MAX_LIGHTS * 4];
            int n = 0;
            for (int i = 0; i < rdCamera_g_pCurCamera->numLights && n < STD3D_PPL_MAX_LIGHTS; i++)
            {
                rdLight* pLight = rdCamera_g_pCurCamera->aLights[i];
                if (!pLight || pLight->intensity <= 0.0) continue;
                rdVector3 camPos;
                rdMatrix_TransformPoint34(&camPos, &rdCamera_g_pCurCamera->aLightPositions[i], &rdCamera_g_pCurCamera->orient);
                aPos[n*4+0] = camPos.x;
                aPos[n*4+1] = camPos.y;
                aPos[n*4+2] = camPos.z;
                aPos[n*4+3] = pLight->maxRadius;
                // White unless the light has a colour this frame (sabers take the blade's
                // colour). MotS coloured lights plug in here later.
                float rgb[3] = {1.0f, 1.0f, 1.0f};
                sithRender_GetLightColor(pLight, rgb);
                aCol[n*4+0] = rgb[0];
                aCol[n*4+1] = rgb[1];
                aCol[n*4+2] = rgb[2];
                aCol[n*4+3] = pLight->intensity;
                n++;
            }
            // Debug: OPENJKDF2_PPL_DEBUG=1 prints the light list every 120 frames.
            {
                static int dbg = -1, dbgFrame = -1;
                if (dbg < 0) { const char* e = getenv("OPENJKDF2_PPL_DEBUG"); dbg = (e && e[0] == '1'); }
                if (dbg && dbgFrame != std3D_frameCount && (std3D_frameCount % 120) == 0) {
                    dbgFrame = std3D_frameCount;
                    stdPlatform_Printf("ppl: %d lights (camera has %d), atten %.3f\n", n, rdCamera_g_pCurCamera->numLights, rdCamera_g_pCurCamera->attenuationMax);
                    for (int i = 0; i < n; i++)
                        stdPlatform_Printf("  cam %.2f %.2f %.2f r=%.2f i=%.2f col=%.2f %.2f %.2f\n",
                                           aPos[i*4], aPos[i*4+1], aPos[i*4+2], aPos[i*4+3], aCol[i*4+3], aCol[i*4], aCol[i*4+1], aCol[i*4+2]);
                }
            }
            glUniform1i(uniform_pplNumLights, n);
            if (n) {
                glUniform4fv(uniform_pplLightPos, n, aPos);
                glUniform4fv(uniform_pplLightCol, n, aCol);
            }
            glUniform1f(uniform_pplAtten, rdCamera_g_pCurCamera->attenuationMax);
            glUniform4f(uniform_pplProj, rdCamera_g_pCurCamera->pCanvas->half_screen_width,
                        rdCamera_g_pCurCamera->pCanvas->half_screen_height,
                        rdCamera_g_pCurCamera->focalLength, 0.0f);
            glUniform4f(uniform_pplScreen, internalWidth, internalHeight, width, height);
            glUniform1f(uniform_pplSpecular, jkPlayer_specular);
            glUniform1f(uniform_fixtureBloom, jkPlayer_enableBloom ? jkPlayer_fixtureBloom : 0.0f); // Added (remaster)

            // Added (remaster): world up in camera space (floors reflect most), and the values
            // the reflection pass needs later in the frame.
            {
                rdVector3 up = {0.0, 0.0, 1.0}, upCam;
                rdMatrix_TransformVector34(&upCam, &up, &rdCamera_g_pCurCamera->orient);
                glUniform3f(uniform_pplUpCam, upCam.x, upCam.y, upCam.z);
                std3D_ssrUpCam[0] = upCam.x; std3D_ssrUpCam[1] = upCam.y; std3D_ssrUpCam[2] = upCam.z;
                std3D_ssrProj[0] = rdCamera_g_pCurCamera->pCanvas->half_screen_width;
                std3D_ssrProj[1] = rdCamera_g_pCurCamera->pCanvas->half_screen_height;
                std3D_ssrProj[2] = rdCamera_g_pCurCamera->focalLength;
                std3D_ssrProj[3] = 0.0f;
                std3D_ssrScreen[0] = internalWidth; std3D_ssrScreen[1] = internalHeight;
                std3D_ssrScreen[2] = width; std3D_ssrScreen[3] = height;
                std3D_ssrHaveCamera = 1;

                // Added (remaster): volumetric sky. World axes in camera space turn each sky
                // pixel's view ray into a world direction.
                rdVector3 ax[3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
                for (int k = 0; k < 3; k++) {
                    rdVector3 o;
                    rdMatrix_TransformVector34(&o, &ax[k], &rdCamera_g_pCurCamera->orient);
                    std3D_skyWorldAxes[k*3+0] = o.x; std3D_skyWorldAxes[k*3+1] = o.y; std3D_skyWorldAxes[k*3+2] = o.z;
                }
                // World axes also turn camera-space positions into world positions (light maps).
                glUniform3fv(uniform_skyWX, 1, &std3D_skyWorldAxes[0]);
                glUniform3fv(uniform_skyWY, 1, &std3D_skyWorldAxes[3]);
                glUniform3fv(uniform_skyWZ, 1, &std3D_skyWorldAxes[6]);

                // Added (remaster): baked light maps.
                int bLm = std3D_LightmapUpdate();
                glUniform1i(uniform_lmOn, bLm);
                if (bLm) {
                    float fill = (jkPlayer_lightmapFill >= 0.0) ? (float)jkPlayer_lightmapFill : std3D_lmFill;
                    glUniform4f(uniform_lmParams, fill, std3D_lmGain * (float)jkPlayer_lightmapGain, (float)jkPlayer_lightmapMax, 0.0f);
                    glUniform3f(uniform_lmCamPos, rdCamera_g_camMatrix.scale.x, rdCamera_g_camMatrix.scale.y, rdCamera_g_camMatrix.scale.z);
                    std3D_fogCamPos[0] = rdCamera_g_camMatrix.scale.x; // Added (remaster): volumetric fog
                    std3D_fogCamPos[1] = rdCamera_g_camMatrix.scale.y;
                    std3D_fogCamPos[2] = rdCamera_g_camMatrix.scale.z;
                    glUniform1i(uniform_lmAtlas, 8);
                    glUniform1i(uniform_lmData, 9);
                    glActiveTexture(GL_TEXTURE0 + 8);
                    glBindTexture(GL_TEXTURE_2D, std3D_lmAtlas);
                    glActiveTexture(GL_TEXTURE0 + 9);
                    glBindTexture(GL_TEXTURE_2D, std3D_lmData);
                    glActiveTexture(GL_TEXTURE0);
                }

                const sithSkyPreset* sp = sithSky_GetActive();
                if (sp && std3D_skyPanoReady) {
                    float sun[3], fdir[3];
                    sithSky_GetSunDir(sp, sun);
                    float flash = sithSky_GetFlash(sp, (double)SDL_GetTicksNS() / 1e9, fdir);
                    glUniform3fv(uniform_skyWX, 1, &std3D_skyWorldAxes[0]);
                    glUniform3fv(uniform_skyWY, 1, &std3D_skyWorldAxes[3]);
                    glUniform3fv(uniform_skyWZ, 1, &std3D_skyWorldAxes[6]);
                    glUniform3fv(uniform_skySunDir, 1, sun);
                    glUniform3f(uniform_skySunCol, sp->sunColor[0] * sp->sunIntensity, sp->sunColor[1] * sp->sunIntensity, sp->sunColor[2] * sp->sunIntensity);
                    glUniform4f(uniform_skyZenith, sp->zenith[0], sp->zenith[1], sp->zenith[2], sp->stars);
                    glUniform4f(uniform_skyHorizon, sp->horizon[0], sp->horizon[1], sp->horizon[2], sp->sunSize * 0.0174533f);
                    glUniform4f(uniform_skyBelow, sp->below[0], sp->below[1], sp->below[2], sp->exposure);
                    glUniform4f(uniform_skyFlash, fdir[0], fdir[1], fdir[2], flash);
                    glUniform1i(uniform_skyPano, 7);
                    glActiveTexture(GL_TEXTURE0 + 7);
                    glBindTexture(GL_TEXTURE_2D, std3D_skyPano.tex);
                    glActiveTexture(GL_TEXTURE0);
                }
            }
            glUniform1f(uniform_pplWetness, jkPlayer_wetness);

            // Glow lights: gathered once per frame (nearest in visible sectors), reused by
            // every world draw in that frame.
            static int lastGlowFrame = -1;
            if (lastGlowFrame != std3D_frameCount) {
                lastGlowFrame = std3D_frameCount;
                // Gathered for glow highlights and for light shafts (which aim at them).
                std3D_frameNumGlow = (jkPlayer_bGlowLights || jkPlayer_bLightShafts)
                    ? sithGlowLights_Gather(rdCamera_g_pCurCamera, std3D_frameGlowPos, std3D_frameGlowCol, SITHGLOWLIGHTS_MAX_GATHER) : 0;
            }
            int numGlow = jkPlayer_bGlowLights ? std3D_frameNumGlow : 0;
            glUniform1i(uniform_pplNumGlow, numGlow);
            if (numGlow) {
                glUniform4fv(uniform_pplGlowPos, numGlow, std3D_frameGlowPos);
                glUniform4fv(uniform_pplGlowCol, numGlow, std3D_frameGlowCol);
            }
        }
    }

    //rdroid_curColorEffects.tint.x = 0.0;
    //rdroid_curColorEffects.tint.y = 0.5;
    //rdroid_curColorEffects.tint.z = 0.5;

#if 0
    //if (rdroid_curColorEffects.filter.x || rdroid_curColorEffects.filter.y || rdroid_curColorEffects.filter.z)
    //if (rdroid_curColorEffects.tint.x || rdroid_curColorEffects.tint.y || rdroid_curColorEffects.tint.z)
    if (rdroid_curColorEffects.add.x || rdroid_curColorEffects.add.y || rdroid_curColorEffects.add.z)
    {
        stdPlatform_Printf("a %f %f %f ", rdroid_curColorEffects.tint.x, rdroid_curColorEffects.tint.y, rdroid_curColorEffects.tint.z);
        stdPlatform_Printf("%d %d %d ", rdroid_curColorEffects.filter.x, rdroid_curColorEffects.filter.y, rdroid_curColorEffects.filter.z);
        stdPlatform_Printf("%d %d %d ", rdroid_curColorEffects.add.x, rdroid_curColorEffects.add.y, rdroid_curColorEffects.add.z);
        stdPlatform_Printf("%f\n", rdroid_curColorEffects.fade);
    }
#endif

    glUniform3f(uniform_tint, rdroid_curColorEffects.tint.x, rdroid_curColorEffects.tint.y, rdroid_curColorEffects.tint.z);
    if (rdroid_curColorEffects.filter.x || rdroid_curColorEffects.filter.y || rdroid_curColorEffects.filter.z)
        glUniform3f(uniform_filter, rdroid_curColorEffects.filter.x ? 1.0 : 0.25, rdroid_curColorEffects.filter.y ? 1.0 : 0.25, rdroid_curColorEffects.filter.z ? 1.0 : 0.25);
    else
        glUniform3f(uniform_filter, 1.0, 1.0, 1.0);
    glUniform1f(uniform_fade, rdroid_curColorEffects.fade);
    glUniform3f(uniform_add, (float)rdroid_curColorEffects.add.x / 255.0f, (float)rdroid_curColorEffects.add.y / 255.0f, (float)rdroid_curColorEffects.add.z / 255.0f);
    glUniform3f(uniform_emissiveFactor, 0.0, 0.0, 0.0);
    glUniform4f(uniform_albedoFactor, 1.0, 1.0, 1.0, 1.0);
    glUniform1f(uniform_light_mult, jkGuiBuildMulti_bRendering ? 0.85 : (jkPlayer_enableBloom ? 0.9 : 0.85));
    glUniform1f(uniform_displacement_factor, 1.0);

    rdTri* tris = GL_tmpTris;
    rdLine* lines = GL_tmpLines;
    
    //glEnableVertexAttribArray(attribute_v_norm);

    int last_tex_idx = 0;
    //GLushort* world_data_elements = malloc(sizeof(GLushort) * 3 * GL_tmpTrisAmt);
    for (int j = 0; j < GL_tmpTrisAmt; j++)
    {
        world_data_elements[(j*3)+0] = tris[j].v1;
        world_data_elements[(j*3)+1] = tris[j].v2;
        world_data_elements[(j*3)+2] = tris[j].v3;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, world_ibo_triangle);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GL_tmpTrisAmt * 3 * sizeof(GLushort), world_data_elements, GL_STREAM_DRAW);
    
    int do_batch = 0;
    
    //glDepthFunc(GL_LESS);
    //glDepthMask(GL_TRUE);
    //glCullFace(GL_FRONT);

    if (last_tex) {
        std3D_DoTex(last_tex, &tris[0], GL_tmpTrisAmt);
    }

    last_flags = tris[0].flags;

    // Added: Set custom RGB state for the first triangle batch.
    glUniform1i(
        uniform_saberRgbEnabled,
        (last_flags& RD_TRI_FLAG_SABER_RGB) ? 1 : 0
    );
    // Added (remaster): procedural saber blades, and time for their plasma
    glUniform1i(uniform_saberBlade, (last_flags & RD_TRI_FLAG_SABER_BLADE) ? 1 : 0);
    glUniform1i(uniform_sky, (last_flags & RD_TRI_FLAG_SKY) ? 1 : 0); // Added (remaster)
    glUniform1i(uniform_model, (last_flags & RD_TRI_FLAG_MODEL) ? 1 : 0);
    glUniform4f(uniform_cine, jkPlayer_cineFill, jkPlayer_cineKey, jkPlayer_cineRim, jkPlayer_bCinematicLight ? 1.0f : 0.0f);
    glUniform1f(uniform_saberStrength, jkPlayer_saberGlowStrength);
    glUniform1f(uniform_time, (float)((double)SDL_GetTicksNS() / 1e9));

    glUniform3f(
        uniform_saberRgbColor,
        jkSaber_rgbRed,
        jkSaber_rgbGreen,
        jkSaber_rgbBlue
    );

    if (last_flags & 0x800) {
        glDepthFunc(GL_LESS);
        //glClear(GL_DEPTH_BUFFER_BIT);
    }
    else {
        glDepthFunc(GL_ALWAYS);
    }

    if (last_flags & 0x600) {
        
        if (last_flags & 0x200) {
            glUniform1i(uniform_blend_mode, D3DBLEND_INVSRCALPHA);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        else {
            glUniform1i(uniform_blend_mode, D3DBLEND_SRCALPHA);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
    }
    else {
        glUniform1i(uniform_blend_mode, D3DBLEND_ONE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    if (last_flags & 0x1000)
    {
        glDepthMask(GL_TRUE);
    }
    else
    {
        glDepthMask(GL_FALSE);
    }

    if (last_flags & 0x10000) {
        glCullFace(GL_BACK);
    }
    else
    {
        glCullFace(GL_FRONT);
    }
    
    for (int j = 0; j < GL_tmpTrisAmt; j++)
    {
        if (tris[j].texture != last_tex || tris[j].flags != last_flags)
        {
            do_batch = 1;
        }
        
        if (do_batch)
        {
            int num_tris_batch = j - last_tex_idx;
            rdDDrawSurface* tex = tris[j].texture;


            
            test_idk = tex;

            if (num_tris_batch)
            {
                //printf("batch %u~%u\n", last_tex_idx, j);
                glDrawElements(GL_TRIANGLES, num_tris_batch * 3, GL_UNSIGNED_SHORT, (GLvoid*)((intptr_t)&world_data_elements[last_tex_idx * 3] - (intptr_t)&world_data_elements[0]));
            }

            std3D_DoTex(tex, &tris[j], GL_tmpTrisAmt-j);
            
            int changed_flags = (last_flags ^ tris[j].flags);

            if (changed_flags & RD_TRI_FLAG_SABER_BLADE) // Added (remaster)
                glUniform1i(uniform_saberBlade, (tris[j].flags & RD_TRI_FLAG_SABER_BLADE) ? 1 : 0);
            if (changed_flags & RD_TRI_FLAG_SKY) // Added (remaster)
                glUniform1i(uniform_sky, (tris[j].flags & RD_TRI_FLAG_SKY) ? 1 : 0);
            if (changed_flags & RD_TRI_FLAG_MODEL) // Added (remaster)
                glUniform1i(uniform_model, (tris[j].flags & RD_TRI_FLAG_MODEL) ? 1 : 0);

            // Added: Switch RGB override when entering or leaving a saber batch.
            if (changed_flags & RD_TRI_FLAG_SABER_RGB)
            {
                glUniform1i(
                    uniform_saberRgbEnabled,
                    (tris[j].flags & RD_TRI_FLAG_SABER_RGB) ? 1 : 0
                );
            }

            if (changed_flags & 0x600)
            {
                if (tris[j].flags & 0x600) {
                    
                    if (tris[j].flags & 0x200) {
                        glUniform1i(uniform_blend_mode, D3DBLEND_INVSRCALPHA);
                        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    }
                    else {
                        //printf ("flags %x\n", tris[j].flags);
                        glUniform1i(uniform_blend_mode, D3DBLEND_SRCALPHA);
                        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    }
                }
                else {
                    glUniform1i(uniform_blend_mode, D3DBLEND_ONE);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                }
            }
            
            if (changed_flags & 0x1800)
            {
                if (tris[j].flags & 0x800)
                {
                    glDepthFunc(GL_LESS);
                }
                else
                {
                    glDepthFunc(GL_ALWAYS);
                    //glClear(GL_DEPTH_BUFFER_BIT);
                }
                
                if (tris[j].flags & 0x1000)
                {
                    glDepthMask(GL_TRUE);
                }
                else
                {
                    glDepthMask(GL_FALSE);
                }
            }

            if (changed_flags & 0x10000)
            {
                if (tris[j].flags & 0x10000) {
                    glCullFace(GL_BACK);
                }
                else
                {
                    glCullFace(GL_FRONT);
                }
            }
            
            last_tex = tris[j].texture;
            last_flags = tris[j].flags;
            last_tex_idx = j;

            do_batch = 0;
        }
        //printf("tri %u: %u,%u,%u, flags %x\n", j, tris[j].v1, tris[j].v2, tris[j].v3, tris[j].flags);
        
        
        /*int vert = tris[j].v1;
        stdPlatform_Printf("%u: %f %f %f, %f %f %f, %f %f\n", vert, vertexes[vert].x, vertexes[vert].y, vertexes[vert].z,
                                      vertexes[vert].nx, vertexes[vert].ny, vertexes[vert].nz,
                                      vertexes[vert].tu, vertexes[vert].tv);
        
        vert = tris[j].v2;
        stdPlatform_Printf("%u: %f %f %f, %f %f %f, %f %f\n", vert, vertexes[vert].x, vertexes[vert].y, vertexes[vert].z,
                                      vertexes[vert].nx, vertexes[vert].ny, vertexes[vert].nz,
                                      vertexes[vert].tu, vertexes[vert].tv);
        
        vert = tris[j].v3;
        stdPlatform_Printf("%u: %f %f %f, %f %f %f, %f %f\n", vert, vertexes[vert].x, vertexes[vert].y, vertexes[vert].z,
                                      vertexes[vert].nx, vertexes[vert].ny, vertexes[vert].nz,
                                      vertexes[vert].tu, vertexes[vert].tv);*/
    }
    
    int remaining_batch = GL_tmpTrisAmt - last_tex_idx;

    if (remaining_batch)
    {
        glDrawElements(GL_TRIANGLES, remaining_batch * 3, GL_UNSIGNED_SHORT, (GLvoid*)((intptr_t)&world_data_elements[last_tex_idx * 3] - (intptr_t)&world_data_elements[0]));
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    
    
#if 0
    // Draw all lines
    world_data_elements = malloc(sizeof(GLushort) * 2 * GL_tmpLinesAmt);
    for (int j = 0; j < GL_tmpLinesAmt; j++)
    {
        world_data_elements[(j*2)+0] = lines[j].v1;
        world_data_elements[(j*2)+1] = lines[j].v2;
    }
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, world_ibo_triangle);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GL_tmpLinesAmt * 2 * sizeof(GLushort), world_data_elements, GL_STREAM_DRAW);

    // Perf: draw the count just uploaded rather than asking GL for the buffer's size (a GPU wait).
    glDrawElements(GL_LINES, (GLsizei)(GL_tmpLinesAmt * 2), GL_UNSIGNED_SHORT, 0);
#endif
        
    // Done drawing    
    glBindTexture(GL_TEXTURE_2D, worldpal_texture);
    glCullFace(GL_FRONT);
    
    std3D_ResetRenderList();
}

int std3D_SetCurrentPalette(rdColor24 *a1, int a2)
{
    return 1;
}

void std3D_GetValidDimension(unsigned int inW, unsigned int inH, unsigned int *outW, unsigned int *outH)
{
    // TODO hack for JKE? I don't know what they're doing
    *outW = inW > 256 ? 256 : inW;
    *outH = inH > 256 ? 256 : inH;
}

int std3D_DrawOverlay()
{
    return 1;
}

void std3D_UnloadAllTextures()
{
#ifndef SDL2_RENDER
    if (!Main_bHeadless) {
        glDeleteTextures(std3D_loadedTexturesAmt, std3D_aLoadedTextures);
    }
    std3D_loadedTexturesAmt = 0;
#else
    std3D_UpdateSettings();
#endif
}

void std3D_AddRenderListTris(rdTri *tris, unsigned int num_tris)
{
    if (Main_bHeadless) return;

    if (GL_tmpTrisAmt + num_tris > STD3D_MAX_TRIS)
    {
        return;
    }
    
    memcpy(&GL_tmpTris[GL_tmpTrisAmt], tris, sizeof(rdTri) * num_tris);
    
    GL_tmpTrisAmt += num_tris;
}

void std3D_AddRenderListLines(rdLine* lines, uint32_t num_lines)
{
    if (Main_bHeadless) return;

    if (GL_tmpLinesAmt + num_lines > STD3D_MAX_VERTICES)
    {
        return;
    }
    
    memcpy(&GL_tmpLines[GL_tmpLinesAmt], lines, sizeof(rdLine) * num_lines);
    GL_tmpLinesAmt += num_lines;
}

int std3D_AddRenderListVertices(D3DVERTEX *aVertices, int count)
{
    if (Main_bHeadless) return 1;

    if (GL_tmpVerticesAmt + count >= STD3D_MAX_VERTICES)
    {
        return 0;
    }
    
    memcpy(&GL_tmpVertices[GL_tmpVerticesAmt], aVertices, sizeof(D3DVERTEX) * count);
    
    GL_tmpVerticesAmt += count;
    
    return 1;
}

void std3D_AddRenderListUITris(rdUITri *tris, unsigned int num_tris)
{
    if (Main_bHeadless) return;

    if (GL_tmpUITrisAmt + num_tris > STD3D_MAX_TRIS)
    {
        return;
    }
    
    memcpy(&GL_tmpUITris[GL_tmpUITrisAmt], tris, sizeof(rdUITri) * num_tris);
    
    GL_tmpUITrisAmt += num_tris;
}

int std3D_ClearZBuffer()
{
#ifdef RDRASTER_SW_ZBUFFER
    // the software rasterizer keeps its own depth buffer; clear it on the same hook JK uses
    // (scene start + the POV weapon's clear) so the software path stays in lockstep with the HW one.
    rdZRaster_ClearZBuffer();
#endif
    glDepthMask(GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, std3D_pFb->fbo);
    glClear(GL_DEPTH_BUFFER_BIT);
    return 1;
}

int std3D_AddToTextureCache(tVBuffer *vbuf, rdDDrawSurface *texture, int is_alpha_tex, int no_alpha)
{
    if (Main_bHeadless) return 1;
    if (!vbuf || !texture) return 1;
    if (texture->texture_loaded) return 1;

    // Added: when the list is full, reuse a slot std3D_PurgeTextureEntry emptied. A material freed
    // on its own (the character screen's model, each time you change skin) leaves a hole, and the
    // count only goes back to 0 when a level loads, so going through a few hundred skins filled it.
    size_t slot = std3D_loadedTexturesAmt;
    if (slot >= STD3D_MAX_TEXTURES) {
        for (slot = 0; slot < STD3D_MAX_TEXTURES; slot++) {
            if (!std3D_aLoadedSurfaces[slot] && !std3D_aLoadedTextures[slot]) break;
        }
    }

    if (slot >= STD3D_MAX_TEXTURES) {
        stdPlatform_Printf("ERROR: Texture cache exhausted!! Ask ShinyQuagsire to increase the size.\n");
        return 1;
    }
    //printf("Add to texture cache\n");

    GLuint image_texture;
    glGenTextures(1, &image_texture);
    uint8_t* image_8bpp = (uint8_t*)vbuf->sdlSurface->pixels;
    uint16_t* image_16bpp = (uint16_t*)vbuf->sdlSurface->pixels;
    uint8_t* pal = (uint8_t*)vbuf->palette;
    
    uint32_t width, height;
    width = vbuf->format.width;
    height = vbuf->format.height;

    glBindTexture(GL_TEXTURE_2D, image_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    //glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    //glPixelStorei(GL_PACK_ALIGNMENT, 1);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    if (jkPlayer_enableTextureFilter && texture->is_16bit)
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    else
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    }

    if (vbuf->format.format.is16bit)
    {
        texture->is_16bit = 1;
#if 1
        if (!is_alpha_tex)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0,  GL_RGB, GL_UNSIGNED_SHORT_5_6_5_REV, image_8bpp);
        else
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,  GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, image_8bpp);
#endif
        std3D_SetTrueColorFiltering(jkPlayer_enableTextureFilter, 1); // Added (remaster)

#ifdef __NOTDEF_FORMAT_CONVERSION
        void* image_data = malloc(width*height*4);
    
        for (int j = 0; j < height; j++)
        {
            for (int i = 0; i < width; i++)
            {
                uint32_t index = (i*height) + j;
                uint32_t val_rgba = 0x00000000;
                
                uint16_t val = image_16bpp[index];
                if (!is_alpha_tex) // RGB565
                {
                    uint8_t val_a1 = 1;
                    uint8_t val_r5 = (val >> 11) & 0x1F;
                    uint8_t val_g6 = (val >> 5) & 0x3F;
                    uint8_t val_b5 = (val >> 0) & 0x1F;

                    uint8_t val_a8 = val_a1 ? 0xFF : 0x0;
                    uint8_t val_r8 = ( val_r5 * 527 + 23 ) >> 6;
                    uint8_t val_g8 = ( val_g6 * 259 + 33 ) >> 6;
                    uint8_t val_b8 = ( val_b5 * 527 + 23 ) >> 6;

#ifdef __NOTDEF_TRANSPARENT_BLACK
                    uint8_t transparent_r8 = (vbuf->transparent_color >> 16) & 0xFF;
                    uint8_t transparent_g8 = (vbuf->transparent_color >> 8) & 0xFF;
                    uint8_t transparent_b8 = (vbuf->transparent_color >> 0) & 0xFF;

                    if (val_r8 == transparent_r8 && val_g8 == transparent_g8 && val_b8 == transparent_b8) {
                        val_a1 = 0;
                    }
#endif // __NOTDEF_TRANSPARENT_BLACK

                    val_rgba |= (val_a8 << 24);
                    val_rgba |= (val_b8 << 16);
                    val_rgba |= (val_g8 << 8);
                    val_rgba |= (val_r8 << 0);
                }
                else // RGB1555
                {
                    uint8_t val_a1 = (val >> 15);
                    uint8_t val_r5 = (val >> 10) & 0x1F;
                    uint8_t val_g5 = (val >> 5) & 0x1F;
                    uint8_t val_b5 = (val >> 0) & 0x1F;

                    uint8_t val_a8 = val_a1 ? 0xFF : 0x0;
                    uint8_t val_r8 = ( val_r5 * 527 + 23 ) >> 6;
                    uint8_t val_g8 = ( val_g5 * 527 + 23 ) >> 6;
                    uint8_t val_b8 = ( val_b5 * 527 + 23 ) >> 6;

                    val_rgba |= (val_a8 << 24);
                    val_rgba |= (val_b8 << 16);
                    val_rgba |= (val_g8 << 8);
                    val_rgba |= (val_r8 << 0);
                }
                    
                *(uint32_t*)(image_data + index*4) = val_rgba;
            }
        }
        
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, image_data);

        texture->pDataDepthConverted = image_data;
#endif // __NOTDEF_FORMAT_CONVERSION
    }
    else {
#if 0
        void* image_data = malloc(width*height*4);
    
        for (int j = 0; j < height; j++)
        {
            for (int i = 0; i < width; i++)
            {
                uint32_t index = (i*height) + j;
                uint32_t val_rgba = 0xFF000000;
                
                if (pal)
                {
                    uint8_t val = image_8bpp[index];
                    val_rgba |= (pal[(val * 3) + 2] << 16);
                    val_rgba |= (pal[(val * 3) + 1] << 8);
                    val_rgba |= (pal[(val * 3) + 0] << 0);
                }
                else
                {
                    uint8_t val = image_8bpp[index];
                    rdColor24* pal_master = (rdColor24*)sithWorld_g_pCurrentWorld->colormaps->colors;//stdDisplay_gammaPalette;
                    rdColor24* color = &pal_master[val];
                    val_rgba |= (color->r << 16);
                    val_rgba |= (color->g << 8);
                    val_rgba |= (color->b << 0);
                }
                
                *(uint32_t*)(image_data + index*4) = val_rgba;
            }
        }
        
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);

        texture->pDataDepthConverted = image_data;
#endif
        texture->is_16bit = 0;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, image_8bpp);

        texture->pDataDepthConverted = NULL;
    }

    
    std3D_aLoadedSurfaces[slot] = texture;
    std3D_aLoadedTextures[slot] = image_texture;
    if (slot == std3D_loadedTexturesAmt) std3D_loadedTexturesAmt++;
    /*ext->surfacebuf = image_data;
    ext->surfacetex = image_texture;
    ext->surfacepaltex = pal_texture;*/
    
    texture->texture_id = image_texture;
    texture->emissive_texture_id = 0;
    texture->displacement_texture_id = 0;
    texture->orm_texture_id = 0;
    texture->normal_texture_id = 0;
    texture->texture_loaded = 1;
    texture->emissive_factor[0] = 0.0;
    texture->emissive_factor[1] = 0.0;
    texture->emissive_factor[2] = 0.0;
    texture->albedo_factor[0] = 1.0;
    texture->albedo_factor[1] = 1.0;
    texture->albedo_factor[2] = 1.0;
    texture->albedo_factor[3] = 1.0;
    texture->displacement_factor = 0.0;
    texture->albedo_data = NULL;
    texture->displacement_data = NULL;
    texture->emissive_data = NULL;

    glBindTexture(GL_TEXTURE_2D, blank_tex);
    
    return 1;
}

int std3D_GetBitmapCacheIdx()
{
    for (int i = 0; i < STD3D_MAX_TEXTURES; i++)
    {
        if (!std3D_aUIBitmaps[i]) {
            return i;
        }
    }
    return -1;
}

int std3D_AddBitmapToTextureCache(stdBitmap *texture, int mipIdx, int is_alpha_tex, int no_alpha)
{
    if (Main_bHeadless) return 1;
    if (!has_initted) return 0;
    if (!texture) return 1;
    if (mipIdx >= texture->numMips) return 1;
    if (!texture->abLoadedToGPU || texture->abLoadedToGPU[mipIdx]) return 1;

    tVBuffer *vbuf = texture->mipSurfaces[mipIdx];
     if (!vbuf) return 1;

    int cacheIdx = std3D_GetBitmapCacheIdx();
    if (cacheIdx < 0) {
        stdPlatform_Printf("ERROR: Texture cache exhausted!! Ask ShinyQuagsire to increase the size.\n");
        return 1;
    }
    //printf("Add to texture cache\n");
    
    GLuint image_texture;
    glGenTextures(1, &image_texture);
    uint8_t* image_8bpp = (uint8_t*)vbuf->sdlSurface->pixels;
    uint16_t* image_16bpp = (uint16_t*)vbuf->sdlSurface->pixels;
    uint8_t* pal = (uint8_t*)vbuf->palette;
    
    uint32_t width, height;
    width = vbuf->format.width;
    height = vbuf->format.height;

    glBindTexture(GL_TEXTURE_2D, image_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    //glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    //glPixelStorei(GL_PACK_ALIGNMENT, 1);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    /*if (jkPlayer_enableTextureFilter && texture->is_16bit)
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    else*/
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    }

    if (vbuf->format.format.is16bit || texture->format.bpp == 16)
    {
        texture->is_16bit = 1;

#if 0
        if (!is_alpha_tex)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0,  GL_RGB, GL_UNSIGNED_SHORT_5_6_5, image_8bpp);
        else
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,  GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, image_8bpp);
#endif
        uint32_t tex_width, tex_height, tex_row_stride;
        uint32_t row_stride = vbuf->format.rowSize / 2;
        tex_width = width;//vbuf->format.rowSize / 2;
        tex_height = height;
        tex_row_stride = width;

        void* image_data = malloc(tex_width*tex_height*4);
        memset(image_data, 0, tex_width*tex_height*4);
    
        //uint32_t index = 0;
        for (int j = 0; j < height; j++)
        {
            for (int i = 0; i < width; i++)
            {
                uint32_t index = (j*row_stride) + i;
                uint32_t tex_index = (j*tex_row_stride) + i;
                uint32_t val_rgba = 0x00000000;
                
                uint16_t val = image_16bpp[index];
                if (vbuf->format.format.r_bits == 5 && vbuf->format.format.g_bits == 6 && vbuf->format.format.b_bits == 5) // RGB565
                {
                    uint8_t val_a1 = 1;
                    uint8_t val_r5 = (val >> 11) & 0x1F;
                    uint8_t val_g6 = (val >> 5) & 0x3F;
                    uint8_t val_b5 = (val >> 0) & 0x1F;

                    uint8_t val_a8 = val_a1 ? 0xFF : 0x0;
                    uint8_t val_r8 = ( val_r5 * 527 + 23 ) >> 6;
                    uint8_t val_g8 = ( val_g6 * 259 + 33 ) >> 6;
                    uint8_t val_b8 = ( val_b5 * 527 + 23 ) >> 6;

                    //uint8_t transparent_r8 = (vbuf->transparent_color >> 16) & 0xFF;
                    //uint8_t transparent_g8 = (vbuf->transparent_color >> 8) & 0xFF;
                    //uint8_t transparent_b8 = (vbuf->transparent_color >> 0) & 0xFF;

                    uint8_t transparent_r5 = (vbuf->transparent_color >> 11) & 0x1F;
                    uint8_t transparent_g6 = (vbuf->transparent_color >> 5) & 0x3F;
                    uint8_t transparent_b5 = (vbuf->transparent_color >> 0) & 0x1F;

                    uint8_t transparent_r8 = ( transparent_r5 * 527 + 23 ) >> 6;
                    uint8_t transparent_g8 = ( transparent_g6 * 259 + 33 ) >> 6;
                    uint8_t transparent_b8 = ( transparent_b5 * 527 + 23 ) >> 6;

                    //
                    if (vbuf->transparent_color && val_r5 == transparent_r5 && val_g6 == transparent_g6 && val_b5 == transparent_b5) {
                        val_a8 = 0;
                        //val_r8 = 0;
                        //val_g8 = 0;
                        //val_b8 = 0;
                    }

                    val_rgba |= (val_a8 << 24);
                    val_rgba |= (val_b8 << 16);
                    val_rgba |= (val_g8 << 8);
                    val_rgba |= (val_r8 << 0);

#if 0
                    val_rgba = 0xFF000000;
                    val_rgba |= (transparent_b8 << 16);
                    val_rgba |= (transparent_g8 << 8);
                    val_rgba |= (transparent_r8 << 0);
#endif
                }
                else if (vbuf->format.format.r_bits == 5 && vbuf->format.format.g_bits == 5 && vbuf->format.format.b_bits == 5) // RGB1555
                {
                    uint8_t val_a1 = (val >> 15);
                    uint8_t val_r5 = (val >> 10) & 0x1F;
                    uint8_t val_g5 = (val >> 5) & 0x1F;
                    uint8_t val_b5 = (val >> 0) & 0x1F;

                    uint8_t val_a8 = val_a1 ? 0xFF : 0x0;
                    uint8_t val_r8 = ( val_r5 * 527 + 23 ) >> 6;
                    uint8_t val_g8 = ( val_g5 * 527 + 23 ) >> 6;
                    uint8_t val_b8 = ( val_b5 * 527 + 23 ) >> 6;

                    uint8_t transparent_a1 = (vbuf->transparent_color >> 15) & 0x1;
                    uint8_t transparent_r5 = (vbuf->transparent_color >> 10) & 0x1F;
                    uint8_t transparent_g5 = (vbuf->transparent_color >> 5) & 0x1F;
                    uint8_t transparent_b5 = (vbuf->transparent_color >> 0) & 0x1F;

                    uint8_t transparent_r8 = ( transparent_r5 * 527 + 23 ) >> 6;
                    uint8_t transparent_g8 = ( transparent_g5 * 527 + 23 ) >> 6;
                    uint8_t transparent_b8 = ( transparent_b5 * 527 + 23 ) >> 6;

#if 0
                    //vbuf->transparent_color && 
                    if (val_a1 == transparent_a1 && val_r5 == transparent_r5 && val_g5 == transparent_g5 && val_b5 == transparent_b5) {
                        val_a8 = 0;
                        //val_r8 = 0;
                        //val_g8 = 0;
                        //val_b8 = 0;
                    }
#endif

                    val_rgba |= (val_a8 << 24);
                    val_rgba |= (val_b8 << 16);
                    val_rgba |= (val_g8 << 8);
                    val_rgba |= (val_r8 << 0);
#if 0
                    val_rgba = 0xFF000000;
                    val_rgba |= (transparent_b8 << 16);
                    val_rgba |= (transparent_g8 << 8);
                    val_rgba |= (transparent_r8 << 0);
#endif
                }
                else {
                    stdPlatform_Printf("wtf is this %u %u %u %u\n", vbuf->format.format.unk_40, vbuf->format.format.r_bits, vbuf->format.format.g_bits, vbuf->format.format.b_bits);
                }
                    
                ((uint32_t*)image_data)[tex_index] = val_rgba;
            }
        }
        
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex_width, tex_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);

        texture->paDataDepthConverted[mipIdx] = image_data;
    }
    else {
        texture->is_16bit = 0;
#if 1
        uint32_t tex_width, tex_height, tex_row_stride;
        uint32_t row_stride = vbuf->format.rowSize;
        tex_width = width;//vbuf->format.rowSize / 2;
        tex_height = height;
        tex_row_stride = width;

        void* image_data = malloc(tex_width*tex_height*4);
        memset(image_data, 0, tex_width*tex_height*4);

        void* palette_data = texture->palette;//displaypal_data;

        if (!palette_data) 
        {
            palette_data = std3D_ui_colormap.colors;//jkGui_stdBitmaps[2]->palette;
            pal = NULL;//palette_data;
        }
        else {
            pal = NULL;//texture->palette;
        }
    
        for (int j = 0; j < height; j++)
        {
            for (int i = 0; i < width; i++)
            {
                uint32_t index = (j*row_stride) + i;
                uint32_t tex_index = (j*tex_row_stride) + i;
                uint32_t val_rgba = 0x00000000;
                
                if (pal)
                {
                    uint8_t val = image_8bpp[index];
                    val_rgba |= (pal[(val * 3) + 2] << 16);
                    val_rgba |= (pal[(val * 3) + 1] << 8);
                    val_rgba |= (pal[(val * 3) + 0] << 0);
                    val_rgba |= (0xFF << 24);

                    if (!val) {
                        val_rgba = 0;
                    }
                }
                else
                {
                    uint8_t val = image_8bpp[index];
#if 0
                    if (sithWorld_g_pCurrentWorld && sithWorld_g_pCurrentWorld->colormaps && sithWorld_g_pCurrentWorld->colormaps->colors)
                    {
                        rdColor24* pal_master = (rdColor24*)sithWorld_g_pCurrentWorld->colormaps->colors;//stdDisplay_gammaPalette;
                        rdColor24* color = &pal_master[val];
                        val_rgba |= (color->r << 16);
                        val_rgba |= (color->g << 8);
                        val_rgba |= (color->b << 0);
                        val_rgba |= (0xFF << 24);
                        stdPlatform_Printf("%x %x\n", val_rgba, val);
                    }
                    else {
                        val_rgba = 0xFFFFFFFF; // HACK
                    }
#endif

                    if (palette_data)
                    {
                        uint8_t color_r = ((uint8_t*)palette_data)[(val*3) + 0];
                        uint8_t color_g = ((uint8_t*)palette_data)[(val*3) + 1];
                        uint8_t color_b = ((uint8_t*)palette_data)[(val*3) + 2];

                        val_rgba |= (0xFF << 24);
                        val_rgba |= (color_b << 16);
                        val_rgba |= (color_g << 8);
                        val_rgba |= (color_r << 0);
                    }
                    else {
                        val_rgba = 0xFFFFFF00; // HACK
                    }
                    

                    if (!val) {
                        val_rgba = 0;
                    }
                }
                
                ((uint32_t*)image_data)[tex_index] = val_rgba;
            }
        }
        
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);

        texture->paDataDepthConverted[mipIdx] = image_data;
#endif
        //glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, image_8bpp);
    }
    
    std3D_aUIBitmaps[cacheIdx] = texture;
    std3D_aUITextures[cacheIdx] = image_texture;
    std3D_loadedUITexturesAmt++;
    /*ext->surfacebuf = image_data;
    ext->surfacetex = image_texture;
    ext->surfacepaltex = pal_texture;*/
    
    texture->aTextureIds[mipIdx] = image_texture;
    texture->abLoadedToGPU[mipIdx] = 1;

    glBindTexture(GL_TEXTURE_2D, blank_tex);
    
    return 1;
}

void std3D_UpdateFrameCount(rdDDrawSurface *pTexture) {
    //pTexture->frameNum = std3D_frameCount; // lol LEC bug
    std3D_RemoveTextureFromCacheList(pTexture);
    std3D_AddTextureToCacheList(pTexture);
    pTexture->frameNum = std3D_frameCount;
}
void std3D_RemoveTextureFromCacheList(rdDDrawSurface *surface) {
}
void std3D_AddTextureToCacheList(rdDDrawSurface *surface) {
}

// Added helpers
void std3D_UpdateSettings()
{
    jk_printf("Updating texture cache...\n");
    for (int i = 0; i < STD3D_MAX_TEXTURES; i++)
    {
        rdDDrawSurface* tex = std3D_aLoadedSurfaces[i];
        if (!tex) continue;

        if (!std3D_aLoadedTextures[i]) continue;
        glBindTexture(GL_TEXTURE_2D, std3D_aLoadedTextures[i]);

        // Added (remaster): true-colour textures get mips + anisotropic via the shared helper.
        // 8-bit paletted textures stay nearest here; their filtering happens in the shader.
        if (tex->is_16bit)
        {
            std3D_SetTrueColorFiltering(jkPlayer_enableTextureFilter, 0);
        }
        else
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        }

        if (tex->emissive_texture_id != 0) {
            glBindTexture(GL_TEXTURE_2D, tex->emissive_texture_id);
            std3D_SetTrueColorFiltering(jkPlayer_enableTextureFilter, 0);
        }

        if (tex->displacement_texture_id != 0) {
            glBindTexture(GL_TEXTURE_2D, tex->displacement_texture_id);
            std3D_SetTrueColorFiltering(jkPlayer_enableTextureFilter, 0);
        }
    }

#if 0
    for (int i = 0; i < STD3D_MAX_TEXTURES; i++)
    {
        if (!std3D_aUITextures[i]) continue;
        glBindTexture(GL_TEXTURE_2D, std3D_aUITextures[i]);

        if (jkPlayer_enableTextureFilter)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        }
        else
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        }
        
    }
#endif

    glBindTexture(GL_TEXTURE_2D, blank_tex);
}

// Added
#ifdef TARGET_CAN_JKGM
// Added: PNG encoding takes ~90 ms at 1440p and froze the game, so it runs on a thread
typedef struct std3D_ScreenshotJob
{
    char aPath[128];
    int w, h;
    uint8_t* pData;
} std3D_ScreenshotJob;

static int std3D_ScreenshotThread(void* pArg)
{
    std3D_ScreenshotJob* pJob = (std3D_ScreenshotJob*)pArg;
    jkgm_write_png(pJob->aPath, pJob->w, pJob->h, pJob->pData);
    free(pJob->pData);
    free(pJob);
    return 0;
}
#endif

// Fixed: read the composited window (HUD, messages, console), not the 3D scene FBO.
// Call between std3D_DrawMenu and the buffer swap (jkGame_ScreenshotFlush does).
void std3D_Screenshot(const char* pFpath)
{
#ifdef TARGET_CAN_JKGM
    int w = Window_xSize, h = Window_ySize;
    if (w <= 0 || h <= 0) return;

    std3D_ScreenshotJob* pJob = (std3D_ScreenshotJob*)malloc(sizeof(std3D_ScreenshotJob));
    uint8_t* data = (uint8_t*)malloc(w * h * 3 * sizeof(uint8_t));
    if (!pJob || !data) { free(pJob); free(data); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, std3D_windowFbo);
    if (!std3D_windowFbo) glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1); // Fixed: rows are tight, widths like 1366 overflowed
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    stdString_SafeStrCopy(pJob->aPath, pFpath, sizeof(pJob->aPath));
    pJob->w = w;
    pJob->h = h;
    pJob->pData = data;
    SDL_Thread* pThread = SDL_CreateThread(std3D_ScreenshotThread, "std3D_Screenshot", pJob);
    if (pThread)
        SDL_DetachThread(pThread);
    else
        std3D_ScreenshotThread(pJob);
#endif
}

#ifdef RDRASTER_SOFTWARE_RENDERER
// Capture the presented WINDOW framebuffer (what the user actually sees, including the software
// world present + HUD overlay), unlike std3D_Screenshot which reads the pre-composite scene FBO.
// Used for headless verification of the software-renderer full-window present.
void std3D_ScreenshotWindow(const char* pFpath)
{
#ifdef TARGET_CAN_JKGM
    int w = Window_xSize, h = Window_ySize;
    if (w <= 0 || h <= 0) return;
    uint8_t* data = (uint8_t*)malloc(w * h * 3 * sizeof(uint8_t));
    glBindFramebuffer(GL_FRAMEBUFFER, std3D_windowFbo);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, data);
    // jkgm_write_png already emits rows bottom-to-top, matching glReadPixels' bottom-left origin, so
    // pass the raw buffer (no manual flip) — exactly like std3D_Screenshot.
    jkgm_write_png(pFpath, w, h, data);
    free(data);
#endif
}
#endif

int std3D_HasAlpha()
{
    return 1;
}

int std3D_HasModulateAlpha()
{
    return 1;
}

int std3D_HasAlphaFlatStippled()
{
    return 1;
}

void std3D_PurgeBitmapRefs(stdBitmap *pBitmap)
{
    for (int i = 0; i < STD3D_MAX_TEXTURES; i++)
    {
        int texId = std3D_aUITextures[i];
        stdBitmap* tex = std3D_aUIBitmaps[i];
        if (!tex) continue;
        if (tex != pBitmap) continue;

        for (int j = 0; j < tex->numMips; j++)
        {
            if (tex->aTextureIds[j] == texId) {
                std3D_PurgeUIEntry(i, j);
                break;
            }
        }
    }
}

void std3D_PurgeSurfaceRefs(rdDDrawSurface *texture)
{
    for (int i = 0; i < STD3D_MAX_TEXTURES; i++)
    {
        rdDDrawSurface* tex = std3D_aLoadedSurfaces[i];
        if (!tex) continue;
        if (tex != texture) continue;

        std3D_PurgeTextureEntry(i);
    }
}

void std3D_PurgeTextureEntry(int i) {
    if (std3D_aLoadedTextures[i]) {
        glDeleteTextures(1, &std3D_aLoadedTextures[i]);
        std3D_aLoadedTextures[i] = 0;
    }

    rdDDrawSurface* tex = std3D_aLoadedSurfaces[i];
    if (!tex) return;

    if (tex->pDataDepthConverted != NULL) {
        free(tex->pDataDepthConverted);
        tex->pDataDepthConverted = NULL;
    }

    if (tex->albedo_data != NULL) {
        //jkgm_aligned_free(tex->albedo_data);
        tex->albedo_data = NULL;
    }

    if (tex->emissive_data != NULL) {
        //jkgm_aligned_free(tex->emissive_data);
        tex->emissive_data = NULL;
    }

    if (tex->displacement_data != NULL) {
        //jkgm_aligned_free(tex->displacement_data);
        tex->displacement_data = NULL;
    }

    if (tex->emissive_texture_id != 0) {
        glDeleteTextures(1, &tex->emissive_texture_id);
        tex->emissive_texture_id = 0;
    }

    if (tex->orm_texture_id != 0) { // Added (remaster)
        glDeleteTextures(1, &tex->orm_texture_id);
        tex->orm_texture_id = 0;
    }
    if (tex->normal_texture_id != 0) { // Added (remaster)
        glDeleteTextures(1, &tex->normal_texture_id);
        tex->normal_texture_id = 0;
    }
    if (tex->displacement_texture_id != 0) {
        glDeleteTextures(1, &tex->displacement_texture_id);
        tex->displacement_texture_id = 0;
    }

    tex->emissive_factor[0] = 0.0;
    tex->emissive_factor[1] = 0.0;
    tex->emissive_factor[2] = 0.0;
    tex->albedo_factor[0] = 1.0;
    tex->albedo_factor[1] = 1.0;
    tex->albedo_factor[2] = 1.0;
    tex->albedo_factor[3] = 1.0;
    tex->displacement_factor = 0.0;

    tex->texture_loaded = 0;
    tex->texture_id = 0;

    std3D_aLoadedSurfaces[i] = NULL;
}

void std3D_PurgeUIEntry(int i, int idx) {
    if (std3D_aUITextures[i]) {
        glDeleteTextures(1, &std3D_aUITextures[i]);
        std3D_aUITextures[i] = 0;
    }

    stdBitmap* tex = std3D_aUIBitmaps[i];
    if (!tex) return;

    tex->abLoadedToGPU[idx] = 0;
    tex->aTextureIds[idx] = 0;
    free(tex->paDataDepthConverted[idx]);
    tex->paDataDepthConverted[idx] = NULL;

    std3D_aUIBitmaps[i] = NULL;
    std3D_loadedUITexturesAmt--;
}

// From https://github.com/smlu/OpenJones3D/blob/main/Libs/std/Win95/std3D.c
int std3D_PurgeTextureCache(size_t size)
{
    size_t purgedBytes = 0;
    for ( rdDDrawSurface* pCacheTexture = std3D_pFirstTexCache; pCacheTexture && pCacheTexture->frameNum != std3D_frameCount; pCacheTexture = pCacheTexture->pNextCachedTexture )
    {
        if ( pCacheTexture->textureSize == size )
        {
            //IDirect3DTexture2_Release(pCacheTexture->pD3DCachedTex);
            std3D_PurgeSurfaceRefs(pCacheTexture);
            //pCacheTexture->pD3DCachedTex = NULL;
            std3D_RemoveTextureFromCacheList(pCacheTexture);
            return 1;
        }
    }

    rdDDrawSurface* pNextCachedTexture = NULL;
    for ( rdDDrawSurface* pCacheTexture = std3D_pFirstTexCache; pCacheTexture && purgedBytes < size; pCacheTexture = pNextCachedTexture )
    {
        pNextCachedTexture = pCacheTexture->pNextCachedTexture;
        if ( pCacheTexture->frameNum != std3D_frameCount )
        {
            //if ( pCacheTexture->pD3DCachedTex ) { // Added: Added check for null pointer
                //IDirect3DTexture2_Release(pCacheTexture->pD3DCachedTex);
                std3D_PurgeSurfaceRefs(pCacheTexture);
            //}
            //pCacheTexture->pD3DCachedTex = NULL;
            purgedBytes += pCacheTexture->textureSize;
            std3D_RemoveTextureFromCacheList(pCacheTexture);
        }
    }

    return purgedBytes != 0;
}

void std3D_PurgeEntireTextureCache()
{
    // Added: a new world's colormap can sit where the old one's was, so the
    // pointer check in std3D_StartScene would keep the old world's palette
    // (a level loaded after another, a palette switched back and forth).
    loaded_colormap = NULL;

    if (Main_bHeadless) {
        std3D_loadedTexturesAmt = 0;
        return;
    }

    if (!std3D_loadedTexturesAmt) {
        jk_printf("Skipping texture cache purge, nothing loaded.\n");
        return;
    }

    jk_printf("Purging texture cache... %x\n", std3D_loadedTexturesAmt);
    for (int i = 0; i < std3D_loadedTexturesAmt; i++)
    {
        std3D_PurgeTextureEntry(i);
    }
    std3D_loadedTexturesAmt = 0;
}

void std3D_InitializeViewport(rdRect *viewRect)
{
    signed int v1; // ebx
    signed int height; // ebp

    float viewXMax_2; // [esp+14h] [ebp+4h]
    float viewRectYMax; // [esp+14h] [ebp+4h]

    std3D_rectViewIdk.x = viewRect->x;
    v1 = viewRect->width;
    std3D_rectViewIdk.y = viewRect->y;
    std3D_rectViewIdk.width = v1;
    height = viewRect->height;
    memset(std3D_aViewIdk, 0, sizeof(std3D_aViewIdk));
    std3D_aViewIdk[0] = (float)std3D_rectViewIdk.x;
    std3D_aViewIdk[1] = (float)std3D_rectViewIdk.y;
    std3D_rectViewIdk.height = height;
    std3D_aViewTris[0].v1 = 0;
    std3D_aViewTris[0].v2 = 1;
    viewXMax_2 = (float)(v1 + std3D_rectViewIdk.x);
    std3D_aViewIdk[8] = viewXMax_2;
    std3D_aViewIdk[9] = std3D_aViewIdk[1];
    std3D_aViewIdk[16] = viewXMax_2;
    viewRectYMax = (float)(height + std3D_rectViewIdk.y);
    std3D_aViewTris[0].texture = 0;
    std3D_aViewIdk[17] = viewRectYMax;
    std3D_aViewIdk[25] = viewRectYMax;
    std3D_aViewIdk[24] = std3D_aViewIdk[0];
    std3D_aViewTris[0].v3 = 2;
    std3D_aViewTris[0].flags = 0x8200;
    std3D_aViewTris[1].v1 = 0;
    std3D_aViewTris[1].v2 = 2;
    std3D_aViewTris[1].v3 = 3;
    std3D_aViewTris[1].texture = 0;
    std3D_aViewTris[1].flags = 0x8200;
}

int std3D_GetValidDimensions(int a1, int a2, int a3, int a4)
{
    int result; // eax

    std3D_gpuMaxTexSizeMaybe = a1;
    result = a4;
    std3D_dword_53D66C = a2;
    std3D_dword_53D670 = a3;
    std3D_dword_53D674 = a4;
    return result;
}

int std3D_FindClosestDevice(uint32_t index, int a2)
{
    return 0;
}

int std3D_SetRenderList(intptr_t a1)
{
    std3D_renderList = a1;
    return std3D_CreateExecuteBuffer();
}

intptr_t std3D_GetRenderList()
{
    return std3D_renderList;
}

int std3D_CreateExecuteBuffer()
{
    return 1;
}

int std3D_IsReady()
{
    return has_initted;
}
