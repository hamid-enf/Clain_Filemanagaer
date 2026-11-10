/*
 * FileManager - Optional hardware decoder registry
 *
 * Lets an application plug in hardware/accelerated codecs for the video
 * subsystem.  A hardware decoder is just an FM_VideoCodecOps whose
 * decode_frame callback drives the hardware (e.g. the STM32 JPEG codec, an
 * ESP32 JPEG/H.264 accelerator, or a licensed IP block).
 *
 * Once registered, Video_DecodeFrame() prefers the hardware decoder over the
 * bundled software decoders (h264bsd / picojpeg) for the matching codec.
 *
 * Usage:
 *     HWDecoder_Register(FM_VIDEO_CODEC_MJPEG, &my_hw_jpeg_ops);
 *     Video_Open("/cam.avi", &v);
 *     Video_DecodeFrame(&v, rgb, sizeof(rgb));  // uses my_hw_jpeg_ops
 */

#ifndef FM_HW_H
#define FM_HW_H

#include "fm_video.h"

FM_BEGIN_DECLS

/* Register a hardware decoder for `codec`.  Replaces any previous binding. */
FM_Result HWDecoder_Register(FM_VideoCodec codec, const FM_VideoCodecOps *ops);
FM_Result HWDecoder_Unregister(FM_VideoCodec codec);
FM_Bool   HWDecoder_Has(FM_VideoCodec codec);
const FM_VideoCodecOps *HWDecoder_Get(FM_VideoCodec codec);

FM_END_DECLS

#endif /* FM_HW_H */
