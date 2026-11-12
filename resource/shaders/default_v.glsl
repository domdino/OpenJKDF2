in vec3 coord3d;
in vec4 v_color;
in float v_light;
in vec2 v_uv;
in float v_invz; // Added (remaster): 1/(32*view depth), for per-pixel lighting
in float v_lmSurf; // Added (remaster): light map world surface + 1 (0 = none)
in float v_glow; // Added (remaster): light-probe overbright on models, added on top as glow
uniform mat4 mvp;
// Added (remaster): terrain skin (tools/lightbake/terrain_skin.py). Its vertices are world
// positions; project them here exactly as the engine projects level faces (rdCamera_PerspProject,
// then rdCache's depth and the mvp above), so they sit in the same depth buffer.
uniform int u_worldSpace;
uniform vec3 u_skinCam;      // camera position in world space
uniform float u_skinFar;     // far plane
uniform vec3 u_skyWX;        // world axes in camera space
uniform vec3 u_skyWY;
uniform vec3 u_skyWZ;
uniform vec4 u_pplProj;      // half screen width, half screen height, focal length
uniform vec4 u_pplScreen;    // canvas width, height (pixels the projection works in)
out vec4 f_color;
out float f_light;
out vec2 f_uv;
out vec3 f_coord;
noperspective out float f_invz; // 1/depth is linear in screen space, so no perspective correction
flat out float f_lmSurf;
out float f_glow;

void main(void)
{
    if (u_worldSpace != 0) {
        vec3 d = coord3d - u_skinCam;
        vec3 c = d.x * u_skyWX + d.y * u_skyWY + d.z * u_skyWZ;   // x right, y depth, z up
        // Clip space, written so it stays linear in the camera-space point (GL clips the
        // triangles that cross the near plane): x and y are the engine's pixel projection
        // (half size + X f / Y) taken to -1..1 and times w; w = Y far as the level's own; depth
        // -1/w is the level's 1 - 1/(Y far), less 1.
        float f = u_pplProj.z, far = u_skinFar;
        gl_Position = vec4(((2.0 * u_pplProj.x / u_pplScreen.x - 1.0) * c.y + 2.0 * f * c.x / u_pplScreen.x) * far,
                           ((1.0 - 2.0 * u_pplProj.y / u_pplScreen.y) * c.y + 2.0 * f * c.z / u_pplScreen.y) * far,
                           -1.0, c.y * far);
        f_coord = vec3(0.0);
        f_invz = 0.0;   // the fragment shader takes it from gl_FragCoord.w instead
    } else {
        vec4 pos = mvp * vec4(coord3d, 1.0);
        pos.w = 1.0/(1.0-coord3d.z);
        pos.xyz *= pos.w;
        gl_Position = pos;
        f_coord = coord3d;
        f_invz = v_invz;
    }
    f_color = (u_worldSpace == 2) ? vec4(v_color.rgb, 1.0) : v_color.bgra; // rubble: baked light, as is
    f_uv = v_uv;
    f_light = v_light;
    f_lmSurf = v_lmSurf;
    f_glow = v_glow;
}
