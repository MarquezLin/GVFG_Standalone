#ifndef __GVFG_SDK_API_H__
#define __GVFG_SDK_API_H__

#ifdef __cplusplus
extern "C" {
#endif
    
#include <windows.h>    
#include "gvfgframework.h"
    
#define GUID_DEVINTERFACE_GVFG100 \
     {0x8c47b9c3, 0x1faa, 0x4557, {0xbc, 0x1d, 0xf2, 0x25, 0xd2, 0x6c, 0x9e, 0x91}}
             

typedef struct _GVFG_CONTEXT GVFG_CONTEXT, *PGVFG_CONTEXT;

typedef enum GVFG_HRESULT {
    
    GVFG_HRESULT_OK                     = 0x0,
    GVFG_HRESULT_ERROR                  = 0x1,
    GVFG_HRESULT_DEV_ERROR              = 0x2,
    GVFG_HRESULT_DEV_BUSY               = 0x4,
    GVFG_HRESULT_VIDEO_CHN_ERROR        = 0x8,
    GVFG_HRESULT_CONTEXT_ERROR          = 0x10,
    GVFG_HRESULT_VIDEO_CHN_INVALID      = 0x11,
    GVFG_HRESULT_API_ERROR              = 0x12

} GVFG_HRESULT;


#define  GVFG_VIDEO_INTERFACE_SDI       (0)               
#define  GVFG_VIDEO_INTERFACE_HDMI      (1)

#define  GVFG_VIDEO_FRAME_BUF_COPY       ((ULONG)0x1)
#define  GVFG_VIDEO_FRAME_ZERO_COPY      ((ULONG)0x2)
#define  GVFG_AUDIO_FRAME_BUF_COPY       ((ULONG)0x4)
#define  GVFG_AV_FRAME_MEM_MODE_DEFAULT  ((ULONG)0xFF)


#define GVFG_MAKEFOURCC(ch0, ch1, ch2, ch3)             \
    ((ULONG)(UCHAR)(ch0)        |                       \
    ((ULONG)(UCHAR)(ch1) << 8)  |                       \
    ((ULONG)(UCHAR)(ch2) << 16) |                       \
    ((ULONG)(UCHAR)(ch3) << 24))


typedef struct _GVFG_DEV_INFO {
    
    UINT16     NumVideoChn;                     // number of video channel.
                                                // VFG 100 SDI  model is fixed at 1
                                                // VFG 100 HDMI mode is fixed at 1
    
    UINT16     HasAudio;                        // VFG100 has audio channel

    
} GVFG_DEV_INFO, *PGVFG_DEV_INFO;


typedef struct _GVFG_VIDEO_INFO {
    
    UINT32    Fourcc;                          // color format, makefourcc('Y210') or makefourcc('YUY2')
    UINT32    Width;
    UINT32    Height;
    UINT32    cbBufSize;                       // buffer size is calcuated from width,
                                               // height and color format

    UINT32    VideoInterface;                  // GVFG_VIDEO_INTERFACE_XXX
    UINT32    VideoSignalLock;
    UINT64    FrameCount;
    
} GVFG_VIDEO_INFO, *PGVFG_VIDEO_INFO;


typedef struct _GVFG_AUDIO_INFO {

    UINT16   Channels;           // 2, stero sound
    UINT16   SamplesPerSec;      // 48000
    UINT16   BitsPerSample;      // 16,  sample data size in bit, sign integer
    UINT16   FramesPerSec;       // 100, number audio frames in 1 second
    UINT32   cbBufSize;          // audio frame in bytes. fixed at 1920
    UINT64   FrameCount;
    
} GVFG_AUDIO_INFO, *PGVFG_AUDIO_INFO;




typedef struct _GVFG_VIDEO_CHN_EVENT {

    HANDLE hVideoFrameInEvent;
    HANDLE hAudioFrameInEvent;
    HANDLE hVideoFormatChangedEvent;
    HANDLE hVideoInputPluginEvent;
    HANDLE hVideoInputUnplugEvent;
    HANDLE hVideoExtraFrame;
    HANDLE hAudioExtraFrame;

} GVFG_VIDEO_CHN_EVENT, *PGVFG_VIDEO_CHN_EVENT;



// SDI Video input signal information.

#define GVFG_SDI_MODE_HD 0
#define GVFG_SDI_MODE_SD 1
#define GVFG_SDI_MODE_3G 2


#define GVFG_SDI_RESOL_SMPTE_ST_274_1920x1080   0x0
#define GVFG_SDI_RESOL_SMPTE_ST_296_1280x720    0x1
#define GVFG_SDI_RESOL_SMPTE_2048_2048x1080     0x2
#define GVFG_SDI_RERSOL_SMPTE_295_1920x1080     0x3
#define GVFG_SDI_RESOL_NTSC_720x486             0x8
#define GVFG_SDI_RESOL_PAL_720x576              0x9
#define GVFG_SDI_RESOL_UNKNOWN                  0xF


#define GVFG_SDI_FPS_NONE                     0x0
#define GVFG_SDI_FPS_23_98                    0x2
#define GVFG_SDI_FPS_24                       0x3
#define GVFG_SDI_FPS_47_95                    0x4
#define GVFG_SDI_FPS_25                       0x5
#define GVFG_SDI_FPS_29_97                    0x6
#define GVFG_SDI_FPS_30                       0x7
#define GVFG_SDI_FPS_48                       0x8
#define GVFG_SDI_FPS_50                       0x9 
#define GVFG_SDI_FPS_59_94                    0xa
#define GVFG_SDI_FPS_60                       0xb
    

typedef struct _GVFG_SDI_VIDEO_INFO {

    UINT32 VideoSignalLock;               // 0: signal is not locked.
                                          // 1: signal is locked.
    
    UINT32 Mode;                          // GVFG_SDI_MODE_XXX
    UINT32 Resol;                         // GVFG_SDI_RESOL_XXX
    UINT32 Fps;                           // GVFG_SDI_FPS_XXX
    
    UINT32 Progressive;                   // 1: Signal is progressive
                                          // 0: Signal is interlace
    
    UINT32 LevelB;                        // 1: Signal 3G SDI B. Only when Mode is 3G-SDI
    UINT32 St352Payload;                  // SMPTE ST 352 payload ID
    UINT32 ErrorCount;                    // error count
    
} GVFG_SDI_VIDEO_INFO, *PGVFG_SDI_VIDEO_INFO;
    

typedef struct _GVFG_SDI_VIDEO_INFO_STR {

    char VideoSignalLock[32];         // 0: signal is not locked.
                                      // 1: signal is locked.
    
    char Mode[16];                    // Mode and LevelB will be combined as single string
    char Resol[64];         
    char Fps[24];           
    
    char Progressive[16];             // 1: Signal is progressive
                                      // 0: Signal is interlace
    
    char St352PayloadByte0Fmt[64];
    char St352PayloadByte1Fps[24];
    char St352PayloadByte2Chroma[36];
    char St352PayloadByte3BitDepth[24];
    
} GVFG_SDI_VIDEO_INFO_STR, *PGVFG_SDI_VIDEO_INFO_STR;

    
GVFG_SDK_API GVFG_HRESULT GvfgCreateEvents(GVFG_PARAM_IN PGVFG_VIDEO_CHN_EVENT pEvents,
                                           GVFG_PARAM_IN BOOL audioDisable);
    
GVFG_SDK_API GVFG_HRESULT GvfgDestroyEvents(GVFG_PARAM_IN PGVFG_VIDEO_CHN_EVENT pEvents);


GVFG_SDK_API GVFG_HRESULT GvfgOpenDev(GVFG_PARAM_IN HANDLE hDev,
                                      GVFG_PARAM_IN PGVFG_CONTEXT *pCxt,
                                      GVFG_PARAM_IN ULONG AVMemMode);

GVFG_SDK_API GVFG_HRESULT GvfgCloseDev(GVFG_PARAM_IN PGVFG_CONTEXT *pCxt);

GVFG_SDK_API GVFG_HRESULT GvfgGetDevInfo(GVFG_PARAM_IN  PGVFG_CONTEXT pCxt,
                                         GVFG_PARAM_OUT PGVFG_DEV_INFO pDevInfo);

GVFG_SDK_API GVFG_HRESULT GvfgOpenVideoChn(GVFG_PARAM_IN PGVFG_CONTEXT pCxt,
                                           GVFG_PARAM_IN ULONG videoChn,
                                           GVFG_PARAM_IN PGVFG_VIDEO_CHN_EVENT pEvents);



GVFG_SDK_API GVFG_HRESULT GvfgCloseVideoChn(GVFG_PARAM_IN PGVFG_CONTEXT pCxt,
                                            GVFG_PARAM_IN ULONG videoChn);


GVFG_SDK_API GVFG_HRESULT GvfgGetSdiVideoInputInfo(GVFG_PARAM_IN  PGVFG_CONTEXT pCxt,
                                                   GVFG_PARAM_IN  ULONG videoChn,
                                                   GVFG_PARAM_OUT PGVFG_SDI_VIDEO_INFO pSdiInfo);



GVFG_SDK_API GVFG_HRESULT GvfgStringifySdiVideoInputInfo(GVFG_PARAM_IN  PGVFG_CONTEXT pCxt,
                                                         GVFG_PARAM_IN PGVFG_SDI_VIDEO_INFO pSdiInfo,
                                                         GVFG_PARAM_OUT PGVFG_SDI_VIDEO_INFO_STR pSdiInfoStr);
    
    
GVFG_SDK_API GVFG_HRESULT GvfgGetVideoInfo(GVFG_PARAM_IN  PGVFG_CONTEXT pCxt,
                                           GVFG_PARAM_IN  ULONG videoChn,
                                           GVFG_PARAM_OUT PGVFG_VIDEO_INFO pVideoInfo);

GVFG_SDK_API GVFG_HRESULT GvfgGetAudioInfo(GVFG_PARAM_IN  PGVFG_CONTEXT pCxt,
                                           GVFG_PARAM_IN  ULONG videoChn,
                                           GVFG_PARAM_OUT PGVFG_AUDIO_INFO pAudioInfo);


GVFG_SDK_API GVFG_HRESULT GvfgStartCapture(GVFG_PARAM_IN PGVFG_CONTEXT pCxt,
                                           GVFG_PARAM_IN ULONG videoChn);

GVFG_SDK_API GVFG_HRESULT GvfgStopCapture(GVFG_PARAM_IN PGVFG_CONTEXT pCxt,
                                          GVFG_PARAM_IN ULONG videoChn);


GVFG_SDK_API GVFG_HRESULT GvfgGetVideoFrame(GVFG_PARAM_IN  PGVFG_CONTEXT pCxt,
                                            GVFG_PARAM_IN  ULONG videoChn,
                                            GVFG_PARAM_OUT VOID *pBuf,
                                            GVFG_PARAM_IN  ULONG cbBufSize);
    
GVFG_SDK_API GVFG_HRESULT GvfgGetVideoFrameZeroCopy(GVFG_PARAM_IN  PGVFG_CONTEXT pCxt,
                                                    GVFG_PARAM_IN  ULONG videoChn,
                                                    GVFG_PARAM_OUT VOID **pBuf);


GVFG_SDK_API GVFG_HRESULT GvfgReleaseVideoFrameZeroCopy(GVFG_PARAM_IN  PGVFG_CONTEXT pCxt,
                                                        GVFG_PARAM_IN  ULONG videoChn);
    
    
GVFG_SDK_API GVFG_HRESULT GvfgGetAudioFrame(GVFG_PARAM_IN PGVFG_CONTEXT pCxt,
                                            GVFG_PARAM_IN ULONG videoChn,
                                            GVFG_PARAM_OUT VOID *pBuf,
                                            GVFG_PARAM_IN ULONG cbBufSize);


    

#ifdef __cplusplus
}
#endif

#endif // __GVFG_SDK_API_H__



