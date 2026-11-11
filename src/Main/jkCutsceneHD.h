#ifndef _JKCUTSCENEHD_H
#define _JKCUTSCENEHD_H

// Added (remaster): HD replacement cutscenes.
//
// When a cutscene like resource/video/01-02A.SMK starts, the engine looks for
// hdvideo/01-02A.mov (or .mp4 / .m4v) in the game folder. If found, the SMK still
// plays underneath as the clock, audio and subtitle source, and the HD file supplies
// the picture at the matching time. So the HD file must keep the original's length;
// resolution, frame rate and codec are free (anything AVFoundation reads: H.264,
// HEVC, ProRes).
//
// macOS only (AVFoundation). Other platforms always get the original video.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opens the HD version of the cutscene at smkPath, if any. Returns 1 if found.
int jkCutsceneHD_Open(const char* smkPath);

// BGRA frame showing at `seconds`, or NULL. *pbNew is 1 if it changed since the last call.
// The pointer stays valid until the next call or Close.
const uint8_t* jkCutsceneHD_FrameAt(double seconds, int* pW, int* pH, int* pStride, int* pbNew);

int jkCutsceneHD_IsOpen(void);
void jkCutsceneHD_Close(void);

#ifdef __cplusplus
}
#endif

#endif // _JKCUTSCENEHD_H
