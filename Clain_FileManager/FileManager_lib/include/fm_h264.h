/*
 * FileManager - H.264 codec plugin (h264bsd backend)
 *
 * Provides an FM_VideoCodecOps implementation backed by the vendored
 * h264bsd software decoder (third_party/h264bsd).  The plugin decodes
 * Annex-B byte streams (start-code delimited NAL units) into RGB888
 * pixels.
 *
 * Notes:
 *   - h264bsd allocates its reference-picture buffers with malloc/free;
 *     memory use scales with the stream resolution.  On MCUs this needs
 *     external RAM (SDRAM on STM32 H7, PSRAM on ESP32).
 *   - Decoding is single-instance per FM_Video (one decoder context).
 *
 * Usage:
 *     FM_Video v;
 *     Video_Open("/movie.mp4", &v);
 *     Video_RegisterCodec(&v, H264_GetCodecOps());
 *     uint8_t rgb[W*H*3];
 *     Video_DecodeFrame(&v, rgb, sizeof(rgb));
 */

#ifndef FM_H264_H
#define FM_H264_H

#include "fm_video.h"

FM_BEGIN_DECLS

#if FM_USE_H264

/* Return the h264bsd-backed codec plugin.  Attach it with
 * Video_RegisterCodec(). */
const FM_VideoCodecOps *H264_GetCodecOps(void);

/* Diagnostics for the most recent failed H.264 decode.  bsd_result is
 * the raw h264bsd return code (3 = error, 4 = param-set error,
 * 5 = memalloc error), consumed/input are byte counts, first_bytes
 * holds the head of the offending frame payload and fail_bytes the
 * bytes at offset `consumed` where the decoder rejected the stream
 * (healthy Annex-B looks like 00 00 00 01 xx there).  skipped counts
 * NAL units dropped by the error-resilient decoder before it gave up
 * or recovered.  resets counts decoder re-initialisations performed by
 * the self-healing logic (keyframe replay after a broken reference
 * chain); a healthy stream reports 0. */
typedef struct
{
    uint32_t bsd_result;     /* last non-OK h264bsd result           */
    uint32_t input_len;      /* bytes handed to the decoder          */
    uint32_t consumed;       /* bytes consumed before the failure    */
    uint32_t skipped;        /* NALs skipped by error resilience     */
    uint32_t resets;         /* decoder resets by keyframe replay    */
    uint8_t  first_bytes[8]; /* head of the frame payload            */
    uint8_t  fail_bytes[8];  /* bytes at the failure position        */
    uint32_t alloc_failures; /* FM_Alloc refusals during this decode */
    uint32_t skip_nal_type;  /* NAL type of the first skipped NAL     */
    uint32_t aub_fail;       /* CheckAccessUnitBoundary reject reason:
                              * 0 none, 1 PPS not stored, 2 SPS not
                              * stored, 3 SPS switch outside IDR    */
    uint32_t aub_pps_id;     /* pps_id the rejected slice named      */
    uint32_t sps_seen;       /* SPS NALs handed to h264bsd           */
    uint32_t pps_seen;       /* PPS NALs handed to h264bsd           */
    uint32_t activate_fail;  /* h264bsdActivateParamSets reason code:
                              * 0 = none, 1 = HANTRO_NOK (pps/sps
                              * missing or CheckPps rejected the
                              * combination), 2 = MEMORY_ALLOCATION_ERROR,
                              * other = raw code                     */
} FM_H264Diag;

const FM_H264Diag *H264_GetLastError(void);

#endif /* FM_USE_H264 */

FM_END_DECLS

#endif /* FM_H264_H */
