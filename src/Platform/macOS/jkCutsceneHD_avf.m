// Added (remaster): HD replacement cutscene decoding via AVFoundation (hardware
// H.264/HEVC/ProRes). See src/Main/jkCutsceneHD.h.

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include "Main/jkCutsceneHD.h"

#if __has_feature(objc_arc)
#define HD_RELEASE(x) do { (x) = nil; } while (0)
#else
#define HD_RELEASE(x) do { [(x) release]; (x) = nil; } while (0)
#endif

static AVURLAsset* hd_asset = nil;
static AVAssetReader* hd_reader = nil;
static AVAssetReaderTrackOutput* hd_output = nil;
static CMSampleBufferRef hd_cur = NULL;   // frame on screen
static CMSampleBufferRef hd_next = NULL;  // the one after it
static CVPixelBufferRef hd_locked = NULL; // hd_cur's pixels while locked
static int hd_bShownCur = 0;
static int hd_bEnded = 0;

static CMSampleBufferRef hd_copyNext(void)
{
    if (hd_bEnded || !hd_output) return NULL;
    CMSampleBufferRef s = [hd_output copyNextSampleBuffer];
    if (!s) hd_bEnded = 1;
    return s;
}

static void hd_unlock(void)
{
    if (hd_locked) {
        CVPixelBufferUnlockBaseAddress(hd_locked, kCVPixelBufferLock_ReadOnly);
        hd_locked = NULL;
    }
}

static int hd_findFile(const char* smkPath, char* out, size_t outLen)
{
    const char* base = smkPath;
    for (const char* p = smkPath; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    char name[256];
    snprintf(name, sizeof(name), "%s", base);
    char* dot = strrchr(name, '.');
    if (dot) *dot = 0;

    char upper[256], lower[256];
    size_t n = strlen(name);
    for (size_t i = 0; i <= n; i++) {
        upper[i] = (char)toupper((unsigned char)name[i]);
        lower[i] = (char)tolower((unsigned char)name[i]);
    }
    const char* names[3] = { name, upper, lower };
    const char* exts[6] = { "mov", "mp4", "m4v", "MOV", "MP4", "M4V" };
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 6; j++) {
            snprintf(out, outLen, "hdvideo/%s.%s", names[i], exts[j]);
            if (access(out, R_OK) == 0) return 1;
        }
    }
    return 0;
}

int jkCutsceneHD_IsOpen(void)
{
    return hd_reader != nil;
}

void jkCutsceneHD_Close(void)
{
    hd_unlock();
    if (hd_cur) { CFRelease(hd_cur); hd_cur = NULL; }
    if (hd_next) { CFRelease(hd_next); hd_next = NULL; }
    if (hd_reader) [hd_reader cancelReading];
    HD_RELEASE(hd_output);
    HD_RELEASE(hd_reader);
    HD_RELEASE(hd_asset);
    hd_bShownCur = 0;
    hd_bEnded = 0;
}

int jkCutsceneHD_Open(const char* smkPath)
{
    jkCutsceneHD_Close();

    char path[512];
    if (!smkPath || !hd_findFile(smkPath, path, sizeof(path))) return 0;

    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
        hd_asset = [[AVURLAsset alloc] initWithURL:url options:nil];

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSArray* tracks = [hd_asset tracksWithMediaType:AVMediaTypeVideo];
#pragma clang diagnostic pop
        if (!tracks || [tracks count] == 0) {
            printf("jkCutsceneHD: %s has no video track\n", path);
            jkCutsceneHD_Close();
            return 0;
        }

        NSError* err = nil;
        hd_reader = [[AVAssetReader alloc] initWithAsset:hd_asset error:&err];
        if (!hd_reader) {
            printf("jkCutsceneHD: can't read %s: %s\n", path, err ? [[err localizedDescription] UTF8String] : "?");
            jkCutsceneHD_Close();
            return 0;
        }

        NSDictionary* settings = @{ (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA) };
        hd_output = [[AVAssetReaderTrackOutput alloc] initWithTrack:[tracks objectAtIndex:0] outputSettings:settings];
        hd_output.alwaysCopiesSampleData = NO;
        if (![hd_reader canAddOutput:hd_output]) {
            printf("jkCutsceneHD: can't decode %s\n", path);
            jkCutsceneHD_Close();
            return 0;
        }
        [hd_reader addOutput:hd_output];
        if (![hd_reader startReading]) {
            printf("jkCutsceneHD: can't start %s\n", path);
            jkCutsceneHD_Close();
            return 0;
        }

        hd_cur = hd_copyNext();
        hd_next = hd_copyNext();
        if (!hd_cur) {
            printf("jkCutsceneHD: %s has no frames\n", path);
            jkCutsceneHD_Close();
            return 0;
        }
        CVImageBufferRef img = CMSampleBufferGetImageBuffer(hd_cur);
        printf("jkCutsceneHD: playing %s (%zux%zu, %.3f s)\n", path,
               img ? CVPixelBufferGetWidth(img) : 0, img ? CVPixelBufferGetHeight(img) : 0,
               CMTimeGetSeconds(hd_asset.duration));
    }
    return 1;
}

const uint8_t* jkCutsceneHD_FrameAt(double seconds, int* pW, int* pH, int* pStride, int* pbNew)
{
    *pbNew = 0;
    if (!hd_reader || !hd_cur) return NULL;

    @autoreleasepool {
        // Advance while the following frame is already due.
        while (hd_next && CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(hd_next)) <= seconds) {
            hd_unlock();
            CFRelease(hd_cur);
            hd_cur = hd_next;
            hd_next = hd_copyNext();
            hd_bShownCur = 0;
        }
    }

    CVImageBufferRef img = CMSampleBufferGetImageBuffer(hd_cur);
    if (!img) return NULL;
    if (!hd_locked) {
        if (CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return NULL;
        hd_locked = img;
    }
    *pW = (int)CVPixelBufferGetWidth(img);
    *pH = (int)CVPixelBufferGetHeight(img);
    *pStride = (int)CVPixelBufferGetBytesPerRow(img);
    *pbNew = !hd_bShownCur;
    hd_bShownCur = 1;
    return (const uint8_t*)CVPixelBufferGetBaseAddress(img);
}
