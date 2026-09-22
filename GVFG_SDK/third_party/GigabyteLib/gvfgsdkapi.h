/*++

Copyright (C) Gigabyte, All Rights Reserved

Module Name:

    gvfgsdkapi.h

Abstract:

    This module contains the type, structure and function definitions
    for Gigabyte video frame grabber SDK.

Environment:

    Windows Win32

--*/


#ifndef __GVFG_SDK_API_H__
#define __GVFG_SDK_API_H__

#ifdef __cplusplus
extern "C" {
#endif
    
#include <windows.h>    
#include "gvfgframework.h"

/* Interface ID Declarations.
 * This is the interface GUID used to enumerate GVFG100 device(s).
 * SetupDiGetClassDevs(...) and SetupDiEnumDeviceInterfaces(...) are
 * Win32 APIs used to enumeate devices.
 */
    
#define GUID_DEVINTERFACE_GVFG100 \
     {0x8c47b9c3, 0x1faa, 0x4557, {0xbc, 0x1d, 0xf2, 0x25, 0xd2, 0x6c, 0x9e, 0x91}}
             

// Per device context, this is a data abstraction used by the SDK API. 
    
typedef struct _GVFG_CONTEXT GVFG_CONTEXT, *PGVFG_CONTEXT;

    
// SDK API return code. 
    
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

    
/* GvfgSetVideoColorDepth(...) colorDepth parameter.
 *
 * It is used to set the color bit depth of video.
 * 8  bits has YUY2 color space.
 * 10 bits has Y210 color spcae.
 *
 */
    
#define GVFG_VIDEO_COLOR_DEPTH_8_BITS         (8)
#define GVFG_VIDEO_COLOR_DEPTH_10_BITS        (10)


// GVFG_VIDEO_INFO.VideoInterface values
    
#define  GVFG_VIDEO_INTERFACE_SDI       (0)               
#define  GVFG_VIDEO_INTERFACE_HDMI      (1)


/* GvfgOpenDev(...) AVMemMode parameter.
 *
 * Frame data passed from driver to application can
 * be in two modes:
 *    a. data copy to user provided buffer
 *    b. data memory pointer
 *    
 * Audio frame supports user buffer copy at this time.
 */
    
#define  GVFG_VIDEO_FRAME_BUF_COPY       ((ULONG)0x1)   // data copied to user provided buffer
#define  GVFG_VIDEO_FRAME_ZERO_COPY      ((ULONG)0x2)   // pass memory pointer to user
#define  GVFG_AUDIO_FRAME_BUF_COPY       ((ULONG)0x4)
#define  GVFG_AV_FRAME_MEM_MODE_DEFAULT  ((ULONG)0xFF)  // video and audio frame are user buffers copy.


#define GVFG_MAKEFOURCC(ch0, ch1, ch2, ch3)             \
    ((ULONG)(UCHAR)(ch0)        |                       \
    ((ULONG)(UCHAR)(ch1) << 8)  |                       \
    ((ULONG)(UCHAR)(ch2) << 16) |                       \
    ((ULONG)(UCHAR)(ch3) << 24))


/* GvfgGetDevInfo(...) parameter.
 *
 * Device information
 */
    
typedef struct _GVFG_DEV_INFO {
    
    UINT16     NumVideoChn;                     // number of video channel.
                                                // VFG 100 SDI  model is fixed at 1
                                                // VFG 100 HDMI model is fixed at 1
    
    UINT16     HasAudio;                        // VFG100 has audio channel
                                                // 0: no audio channels
                                                // 1: has audio channels
    
} GVFG_DEV_INFO, *PGVFG_DEV_INFO;

    
/* GvfgGetVideoInfo(...) pDevInfo parameter.
 *
 * video frame information.
 */
    
typedef struct _GVFG_VIDEO_INFO {
    
    UINT32    Fourcc;                          // color format:
                                               // 8  bits color space: makefourcc('Y','2','1',0')
                                               // 10 bits color space: makefourcc('Y','U','Y','2')

    UINT32    Width;                           // pixels
    UINT32    Height;                          // pixels
    
    UINT32    cbBufSize;                       // buffer size: number of bytes.
                                               // Calcuated from width, height and color format. 

    UINT32    VideoInterface;                  // GVFG_VIDEO_INTERFACE_XXX
    UINT32    VideoSignalLock;                 // 0: no video signal lock
                                               // 1: video signal lock
    
    UINT64    FrameCount;                      // Driver keeps track of the video frame count.
                                               // This is for book-keeping purpose. 
    
} GVFG_VIDEO_INFO, *PGVFG_VIDEO_INFO;

    
/* GvfgGetAudioInfo(...) pAudioInfo parameter.
 *
 * video frame information.
 */
    
typedef struct _GVFG_AUDIO_INFO {

    UINT16   Channels;           // number of audio channels.
                                 // Fixed at 2 for stero sound
    
    UINT16   SamplesPerSec;      // Sampling rate.
                                 // Fixed at 48000
    
    UINT16   BitsPerSample;      // Sampling data size in bit in sign integer.
                                 // Fixed at 16
    
    UINT16   FramesPerSec;       // Number audio frames in 1 second.
                                 // Fixed at 1000
    
    UINT32   cbBufSize;          // buffer size: number of bytes.
                                 // fixed at 1920
    
    UINT64   FrameCount;         // Driver keeps track of the video frame count.
                                 // This is for book-keeping purpose. 
    
} GVFG_AUDIO_INFO, *PGVFG_AUDIO_INFO;

    
/* GvfgCreateEvents(...) and GvfgOpenVideoChn(...) pEvents parameter.
 *
 * Events notify handles. 
 * Driver notifies user about an event.
 * User should use WIN32 API WaitForSingleObject(...) or WaitForMultipleObjects(...)
 * to wait for events.
 */

typedef struct _GVFG_VIDEO_CHN_EVENT {

    HANDLE hVideoFrameInEvent;            // new video frame arrvied
    HANDLE hAudioFrameInEvent;            // new audio frame arrived
    
    HANDLE hVideoFormatChangedEvent;      // video color space changed.
                                          // This is triggered when
                                          // changing video color space. 

    HANDLE hVideoInputPluginEvent;        // Video cable is plugged in
     
    HANDLE hVideoInputUnplugEvent;        // video cable is unplug.
    
    HANDLE hVideoExtraFrame;              // Extra video frame is queued in the driver
    
    HANDLE hAudioExtraFrame;              // Extra audio frame is queued in the driver.

} GVFG_VIDEO_CHN_EVENT, *PGVFG_VIDEO_CHN_EVENT;

    
// SDI ST352 Video input signal information.

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



/*********************************************************************
 * FUNCTION: Create events notify handles.
 * PURPOSE:  Driver uses these handles to notify user active events. 
 * PARAMS:   pEvents - User allocates GVFG_VIDEO_CHN_EVENT. 
 *           audioDisable - disable audio frame data event notifier
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/    
GVFG_SDK_API GVFG_HRESULT GvfgCreateEvents(GVFG_PARAM_IN PGVFG_VIDEO_CHN_EVENT pEvents,
                                           GVFG_PARAM_IN BOOL audioDisable);


/*********************************************************************
 * FUNCTION: Delete events notify handles.
 * PURPOSE:  When events are no longer needed, user should destroy them.
 * PARAMS:   pEvents - created by the GvfgCreateEvents(...)
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/
GVFG_SDK_API GVFG_HRESULT GvfgDestroyEvents(GVFG_PARAM_IN PGVFG_VIDEO_CHN_EVENT pEvents);


/*********************************************************************
 * FUNCTION: Reset events notify handles.
 * PURPOSE:  When events need to be clean/reset to the waiting state
 * PARAMS:   pEvents - created by the GvfgCreateEvents(...)
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/
GVFG_SDK_API GVFG_HRESULT GvfgResetEvents(GVFG_PARAM_IN PGVFG_VIDEO_CHN_EVENT pEvents);

    
    
/*********************************************************************
 * FUNCTION: Open Gigabyte video frame grabber.
 *           This should be the first function to call to setup the device.
 *
 * PURPOSE:  Setup Gigabyte video frame grabber and create device context.
 * 
 * PARAMS:   hDev - this is the device handler created by WIN32 API CreateFileA(...)
 *                  It is user's responsibility to enumerate Gigabyte device by using
 *                  GUID.
 *           pCxt - Gigabyte device context.
 *                  user should use a POINTER only, PGVFG_CONTEXT(pointer),
 *                  and pass in its address (double pointer).
 *
 *           AVMemMode - decides how video and audio frame data are passed to user.
 *                       User can supply user space buffer for data copy or
 *                       access driver's memory space (as a pointer).
 *                       
 *                       Logical OR of
 *                       GVFG_VIDEO_FRAME_ZERO_COPY/ GVFG_VIDEO_FRAME_BUF_COPY and
 *                       GVFG_AUDIO_FRAME_BUF_COPY. 
 *
 *                       Example: 1. GVFG_VIDEO_FRAME_ZERO_COPY | GVFG_AUDIO_FRAME_BUF_COPY
 *                                2. GVFG_VIDEO_FRAME_BUF_COPY  | GVFG_AUDIO_FRAME_BUF_COPY
 *                                3. GVFG_AV_FRAME_MEM_MODE_DEFAULT
 *
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/
GVFG_SDK_API GVFG_HRESULT GvfgOpenDev(GVFG_PARAM_IN  HANDLE hDev,
                                      GVFG_PARAM_OUT PGVFG_CONTEXT *pCxt,
                                      GVFG_PARAM_IN  ULONG AVMemMode);

    
/*********************************************************************
 * FUNCTION: Close Gigabyte video frame grabber.
 *
 *
 * PURPOSE:  Deactivate Gigabyte video frame grabber and destory device context.
 * 
 * PARAMS:   pCxt - Gigabyte device context.
 *                  Device context created by the GvfgOpenDev(...)
 *
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/
GVFG_SDK_API GVFG_HRESULT GvfgCloseDev(GVFG_PARAM_IN PGVFG_CONTEXT *pCxt);

    
/*********************************************************************
 * FUNCTION: Get Gigabyte device information.
 *
 *
 * PURPOSE:  Get number of video channels and audio function state.
 * 
 * PARAMS:   pCxt - Gigabyte device context.
 *                  Device context created by the GvfgOpenDev(...).
 *
 *           pDevInfo - device information. 
 * 
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/
GVFG_SDK_API GVFG_HRESULT GvfgGetDevInfo(GVFG_PARAM_IN  PGVFG_CONTEXT pCxt,
                                         GVFG_PARAM_OUT PGVFG_DEV_INFO pDevInfo);

    
/*********************************************************************
 * FUNCTION: Open a video channel.
 *
 *
 * PURPOSE:  Activate video channel and register events.
 * 
 * PARAMS:   pCxt - Gigabyte device context.
 *                  Device context created by the GvfgOpenDev(...).
 *
 *           videoChn - video channel index, counting from 0. 
 *                      This should always < GVFG_DEV_INFO.NumVideoChn.
 *
 *           pEvents -  events handler.
 *                      Created by GvfgCreateEvents(...)
 * 
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/
GVFG_SDK_API GVFG_HRESULT GvfgOpenVideoChn(GVFG_PARAM_IN PGVFG_CONTEXT pCxt,
                                           GVFG_PARAM_IN ULONG videoChn,
                                           GVFG_PARAM_IN PGVFG_VIDEO_CHN_EVENT pEvents);

    
/*********************************************************************
 * FUNCTION: Close a video channel.
 *
 *
 * PURPOSE:  Deactivate a video channel and un-registers events when opening the video channel. 
 * 
 * PARAMS:   pCxt - Gigabyte device context.
 *                  Device context created by the GvfgOpenDev(...)
 *
 *           videoChn - video channel index, counting from 0. 
 *                      This should always < GVFG_DEV_INFO.NumVideoChn.
 *
 * 
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/
GVFG_SDK_API GVFG_HRESULT GvfgCloseVideoChn(GVFG_PARAM_IN PGVFG_CONTEXT pCxt,
                                            GVFG_PARAM_IN ULONG videoChn);

    
/*********************************************************************
 * FUNCTION: Get S352 SDI video interface information.
 *
 *
 * PURPOSE:  SDI standard (ST352) can provide video information within video stream.
 *           This is not a requirement. Some SDI video source does not implement this standard.
 *
 *           If  GVFG_SDI_VIDEO_INFO.St352Payload is 0, it is likely video
 *           source does not use ST352.
 * 
 * PARAMS:   pCxt - Gigabyte device context.
 *                  Device context created by the GvfgOpenDev(...)
 *
 *           videoChn - video channel index.
 *
 *           pSdiInfo - SDI ST352 information. 
 *
 * 
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/
GVFG_SDK_API GVFG_HRESULT GvfgGetSdiVideoInputInfo(GVFG_PARAM_IN  PGVFG_CONTEXT pCxt,
                                                   GVFG_PARAM_IN  ULONG videoChn,
                                                   GVFG_PARAM_OUT PGVFG_SDI_VIDEO_INFO pSdiInfo);

    
/*********************************************************************
 * FUNCTION: Pretty print and strigify S352 SDI video interface information,
 *           a helper function.
 *
 *
 * PURPOSE:  If user knows how to decode GVFG_SDI_VIDEO_INFO.St352Payload or wants to
 *           pretty print his own SDI information based on GvfgGetSdiVideoInputInfo(...),
 *           this function would not be usefull. 
 *              
 * 
 * PARAMS:   pCxt - Gigabyte device context.
 *                  Device context created by the GvfgOpenDev(...)
 *
 *           videoChn - video channel index.
 *
 *           pSdiInfo - SDI ST352 information from GvfgGetSdiVideoInputInfo(...). 
 *
 * 
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/
GVFG_SDK_API GVFG_HRESULT GvfgStringifySdiVideoInputInfo(GVFG_PARAM_IN  PGVFG_CONTEXT pCxt,
                                                         GVFG_PARAM_IN PGVFG_SDI_VIDEO_INFO pSdiInfo,
                                                         GVFG_PARAM_OUT PGVFG_SDI_VIDEO_INFO_STR pSdiInfoStr);
    

/*********************************************************************
 * FUNCTION: Get video channel property (widht, height, color space, interface and etc)
 *
 *
 * PURPOSE:  In order to use a video frame, some information is necessary. 
 *           1. color space (fourcc)
 *           2. width
 *           3. height
 *           4. frame size
 *
 *           A frame size can be calculated from width, height, and color space.
 *           Howerver, frame size is already calculated. User can double check it. 
 *
 * NOTE:     width and height are meaningful only when video input source is stable, i.e.
 *           signal lock. 
 *              
 * 
 * PARAMS:   pCxt - Gigabyte device context.
 *                  Device context created by the GvfgOpenDev(...)
 *
 *           videoChn - video channel index.
 *
 *           pVideoInfo - video frame information. 
 *
 * 
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/
GVFG_SDK_API GVFG_HRESULT GvfgGetVideoInfo(GVFG_PARAM_IN  PGVFG_CONTEXT pCxt,
                                           GVFG_PARAM_IN  ULONG videoChn,
                                           GVFG_PARAM_OUT PGVFG_VIDEO_INFO pVideoInfo);


/*********************************************************************
 * FUNCTION: Get PCM audio property.
 *
 *
 * PURPOSE:  In order to use a audio frame, some information is necessary. 
 *           1. sampling rate
 *           2. sample size,
 *           3. number of PCM channels. 
 *           4. frame size
 *
 *           A frame size can be calculated from sampling rate, sample size, and
 *           number of audio channels.
 *
 *           2 channels, 48K sampling rate, signed 16 bits PCM format are supported.
 *    
 * NOTE:     Every single audio frame includes left and right channels and
 *           There are 100 audio frames per second.
 * 
 * PARAMS:   pCxt - Gigabyte device context.
 *                  Device context created by the GvfgOpenDev(...)
 *
 *           videoChn - video channel index.
 *
 *           pAudioInfo - audio frame information. 
 *
 * 
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/    
GVFG_SDK_API GVFG_HRESULT GvfgGetAudioInfo(GVFG_PARAM_IN  PGVFG_CONTEXT pCxt,
                                           GVFG_PARAM_IN  ULONG videoChn,
                                           GVFG_PARAM_OUT PGVFG_AUDIO_INFO pAudioInfo);

    
/*********************************************************************
 * FUNCTION: Start capturing video and/or audio
 *
 *
 * PURPOSE:  Activate video/audio DMA.
 *           After GvfgOpenVideoChn(...), user should wait for video and audio events.
 *
 * NOTE:     For 1080P@60, every video frame is around 16 ms. Depedning on the user application,
 *           user may take more than 16ms to process a video frame, hence the next frame may already
 *           be ready in the driver.
 *
 *           User should also listen to
 *           1. GVFG_VIDEO_CHN_EVENT.hVideoExtraFrame and
 *           2. GVFG_VIDEO_CHN_EVENT.hAudioExtraFrame.
 *
 *           And treat them as new video and audio frame event.
 * 
 * PARAMS:   pCxt - Gigabyte device context.
 *                  Device context created by the GvfgOpenDev(...)
 *
 *           videoChn - video channel index.
 *
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/
GVFG_SDK_API GVFG_HRESULT GvfgStartCapture(GVFG_PARAM_IN PGVFG_CONTEXT pCxt,
                                           GVFG_PARAM_IN ULONG videoChn);

    
/*********************************************************************
 * FUNCTION: Stop capturing video and/or audio
 *
 *
 * PURPOSE:  Stop video and audio DMA.
 * 
 * PARAMS:   pCxt - Gigabyte device context.
 *                  Device context created by the GvfgOpenDev(...)
 *
 *           videoChn - video channel index.
 *
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/    
GVFG_SDK_API GVFG_HRESULT GvfgStopCapture(GVFG_PARAM_IN PGVFG_CONTEXT pCxt,
                                          GVFG_PARAM_IN ULONG videoChn);

    
    
/*********************************************************************
 * FUNCTION: Get video frame data, user buffer copy mode.
 *
 * PURPOSE:  Driver copy frame data to user buffer.
 *           User should allocate frame buffer based on the GvfgGetVideoInfo(...).
 *           
 * NOTE:     This function should only be called when GVFG_VIDEO_FRAME_BUF_COPY
 *           is used with GvfgOpenDev(...). 
 *
 * PARAMS:   pCxt - Gigabyte device context.
 *                  Device context created by the GvfgOpenDev(...).
 *
 *           videoChn - video channel index.
 * 
 *           pBuf      - user allocated buffer.
 *           cbBufSize - user allocated buffer size in bytes.
 *
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/  
GVFG_SDK_API GVFG_HRESULT GvfgGetVideoFrame(GVFG_PARAM_IN  PGVFG_CONTEXT pCxt,
                                            GVFG_PARAM_IN  ULONG videoChn,
                                            GVFG_PARAM_OUT VOID *pBuf,
                                            GVFG_PARAM_IN  ULONG cbBufSize);


/*********************************************************************
 * FUNCTION: Get video frame data, zero copy mode.
 *
 * PURPOSE:  Driver allows user to access frame memory stored in kernel mode.
 *           Compared to user buffer copy mode, user passes a double pointer.
 *            
 *
 * NOTE:     This function should only be called when GVFG_VIDEO_FRAME_ZERO_COPY
 *           is used with GvfgOpenDev(...).
 * 
 *           It is strongly recommended calling GvfgReleaseVideoFrameZeroCopy(...)
 *           to notify driver that a frame is done processing by the user.
 *
 *           DO NOT double-release as driver may get confused. 
 *
 * PARAMS:   pCxt - Gigabyte device context.
 *                  Device context created by the GvfgOpenDev(...).
 *
 *           videoChn - video channel index.
 * 
 *           pBuf     - user pointer (double pointer) to access kernel memory.
 *         
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/
GVFG_SDK_API GVFG_HRESULT GvfgGetVideoFrameZeroCopy(GVFG_PARAM_IN  PGVFG_CONTEXT pCxt,
                                                    GVFG_PARAM_IN  ULONG videoChn,
                                                    GVFG_PARAM_OUT VOID **pBuf);

    
/*********************************************************************
 * FUNCTION: Release a video frame, zero copy mode.
 *
 * PURPOSE:  Notify driver that a zero copy frame is finished processing.
 *
 *
 * NOTE:     This function should only be called when GVFG_VIDEO_FRAME_ZERO_COPY
 *           is used with GvfgOpenDev(...) and GvfgGetVideoFrameZeroCopy(...) is called.
 * 
 *           DO NOT double-release as driver may get confused. 
 *
 * PARAMS:   pCxt - Gigabyte device context.
 *                  Device context created by the GvfgOpenDev(...).
 *
 *           videoChn - video channel index.
 *         
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/
GVFG_SDK_API GVFG_HRESULT GvfgReleaseVideoFrameZeroCopy(GVFG_PARAM_IN  PGVFG_CONTEXT pCxt,
                                                        GVFG_PARAM_IN  ULONG videoChn);
    

/*********************************************************************
 * FUNCTION: Get audio frame data, user buffer copy mode.
 *
 * PURPOSE:  Driver copy frame data to user buffer.
 *           User should allocate frame buffer based on the GvfgGetAudioInfo(...).
 *           
 * NOTE:     This function should only be called when GVFG_AUDIO_FRAME_BUF_COPY
 *           is used with GvfgOpenDev(...) and audio is NOT disabled when calling
 *           GvfgCreateEvents(...). 
 *
 * PARAMS:   pCxt - Gigabyte device context.
 *                  Device context created by the GvfgOpenDev(...).
 *
 *           videoChn - video channel index.
 * 
 *           pBuf      - user allocated buffer.
 *           cbBufSize - user allocated buffer size in bytes.
 *
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/      
GVFG_SDK_API GVFG_HRESULT GvfgGetAudioFrame(GVFG_PARAM_IN PGVFG_CONTEXT pCxt,
                                            GVFG_PARAM_IN ULONG videoChn,
                                            GVFG_PARAM_OUT VOID *pBuf,
                                            GVFG_PARAM_IN ULONG cbBufSize);


/*********************************************************************
 * FUNCTION: Set 8 bits or 10 bits 4:2:2 video data color space. 
 *
 * PURPOSE:  8 bits (YUY2) is the default video frame data color space.
 *           For vivid and detailed color scale, 10 bits (Y210) can be selected.
 *
 *           Please note Y210 required twice the bandwidth compared to YUY2. 
 *           
 *           
 * NOTE:     This function should only be called after calling GvfgOpenVideoChn(...)
 *           and video/audio is not capturing. 
 *
 *           GVFG_VIDEO_CHN_EVENT.hVideoFormatChangedEvent will be triggered if
 *           color space is changed successfully.
 *            
 *
 * PARAMS:   pCxt - Gigabyte device context.
 *                  Device context created by the 
 *
 *           videoChn - video channel index.
 * 
 *           colorDepth - Set 8 or 10 bits color space.
 *                        1. GVFG_VIDEO_COLOR_DEPTH_8_BITS
 *                        2. GVFG_VIDEO_COLOR_DEPTH_10_BITS
 *
 * RETURNS:  GVFG_HRESULT_OK: success.
 *           other:           failure.
 *
 *********************************************************************/      
GVFG_SDK_API GVFG_HRESULT GvfgSetVideoColorDepth(GVFG_PARAM_IN PGVFG_CONTEXT pCxt,
                                                 GVFG_PARAM_IN ULONG videoChn,
                                                 GVFG_PARAM_IN ULONG colorDepth);    

    

#ifdef __cplusplus
}
#endif

#endif // __GVFG_SDK_API_H__



