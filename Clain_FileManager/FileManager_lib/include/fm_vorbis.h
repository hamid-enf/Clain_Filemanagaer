#ifndef FM_VORBIS_H
#define FM_VORBIS_H
#include "fm_types.h"
#include "fm_audio.h"
FM_BEGIN_DECLS
FM_Result FM_Vorbis_Init(void);
const FM_AudioDecoderOps *FM_Vorbis_GetOps(void);

/*
 * Packet-stream decoding.
 *
 * The decoder above takes a complete Ogg Vorbis file in memory.  A video
 * container (MKV / WebM "A_VORBIS") carries the same codec differently:
 * the three Vorbis header packets once, in the track description, then
 * one audio packet at a time.  A Vorbis packet cannot be decoded on its
 * own (its output overlaps the packet before it), so this decoder keeps
 * its state from packet to packet.
 *
 *   FM_Vorbis_StreamOpen    the three header packets (identification,
 *                           comment, setup).  Mono and stereo only
 *                           (FM_ERR_UNSUPPORTED otherwise); headers that
 *                           do not parse are FM_ERR_CORRUPTED.
 *   FM_Vorbis_StreamDecode  one audio packet -> interleaved 16-bit PCM,
 *                           little-endian, *written bytes.  FM_OK with
 *                           *written == 0 is normal: the first packet of
 *                           a stream, and the first two after a reset,
 *                           produce nothing.  One packet is up to 16 KB
 *                           of PCM; what does not fit `cap` stays in the
 *                           decoder: fetch it with packet == NULL before
 *                           the next packet (FM_Vorbis_StreamPending
 *                           tells how many bytes are waiting).
 *                           FM_ERR_CORRUPTED: the packet was refused; go
 *                           on with the next one.
 *   FM_Vorbis_StreamReset   the next packet does not follow the last one
 *                           (seek).
 *
 * Memory: what stb_vorbis needs for the stream (the codebooks; some
 * hundred KB for a usual file) is allocated at open and kept until
 * close, plus a buffer of the size of the largest packet.
 */
typedef struct FM_VorbisStream FM_VorbisStream;

FM_Result FM_Vorbis_StreamOpen(const uint8_t *const hdr[3],
                               const uint32_t hdr_len[3],
                               FM_VorbisStream **out);
FM_Result FM_Vorbis_StreamDecode(FM_VorbisStream *s, const uint8_t *packet,
                                 uint32_t len, uint8_t *pcm, size_t cap,
                                 size_t *written);
size_t    FM_Vorbis_StreamPending(const FM_VorbisStream *s);
uint32_t  FM_Vorbis_StreamChannels(const FM_VorbisStream *s);
void      FM_Vorbis_StreamReset(FM_VorbisStream *s);
void      FM_Vorbis_StreamClose(FM_VorbisStream *s);
FM_END_DECLS
#endif
