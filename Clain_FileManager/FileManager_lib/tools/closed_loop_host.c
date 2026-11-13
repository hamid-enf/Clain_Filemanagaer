/* FM-NOT-FIRMWARE: host-only companion for closed_loop.py. */
#ifndef USE_HAL_DRIVER
#include "file_manager.h"
#include "fm_audio.h"
#include "fm_h264.h"
#include "fm_image.h"
#include "fm_mjpeg.h"
#include "fm_text.h"
#include "fm_video.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern FM_Result PosixFS_Init(FM_FS *fs);
#define W 96u
#define H 64u
#define FRAMES 12u
#define RATE 12000u
#define VIDEO_PIXELS (W * H * 3u)
static unsigned seed;
static uint8_t images[FRAMES][VIDEO_PIXELS];
static int16_t samples[RATE];
static uint8_t pixels[VIDEO_PIXELS];
static int16_t pcm[4096];
static int checked(FM_Result r, const char *name) {
  if (r != FM_OK) {
    fprintf(stderr, "%s: %d\n", name, (int)r);
    return 1;
  }
  return 0;
}
static void source(void) {
  unsigned f, x, y;
  for (f = 0; f < FRAMES; f++)
    for (y = 0; y < H; y++)
      for (x = 0; x < W; x++) {
        size_t i = 3 * (y * W + x);
        images[f][i] = (uint8_t)(x * 255 / (W - 1));
        images[f][i + 1] = (uint8_t)(y * 255 / (H - 1));
        images[f][i + 2] = (uint8_t)(64 + (x + y + f * 3 + seed) % 128);
        if (x >= f * 4 && x < f * 4 + 12 && y >= 24 && y < 40)
          images[f][i] = images[f][i + 1] = images[f][i + 2] = 255;
      }
  for (x = 0; x < RATE; x++)
    samples[x] = (int16_t)((int32_t)((x * (37u + seed)) % 20001u) - 10000);
}
static int write_audio(const char *path, FM_AudioFileFormat fmt) {
  FM_AudioFileWriter w;
  FM_AudioEncodeConfig c;
  FM_Result r;
  size_t wrote = 0;
  memset(&w, 0, sizeof w);
  memset(&c, 0, sizeof c);
  c.sample_rate = RATE;
  c.channels = 1;
  c.bits_per_sample = 16;
  r = AudioFileWriter_Open(&w, path, fmt, &c);
  if (checked(r, "audio open"))
    return 1;
  r = AudioFileWriter_WriteFrames(&w, samples, RATE, &wrote);
  if (r != FM_OK || wrote != RATE) {
    fprintf(stderr, "audio write %d %zu\n", r, wrote);
    AudioFileWriter_Close(&w);
    return 1;
  }
  return checked(AudioFileWriter_Close(&w), "audio close");
}
static int generate(void) {
  FM_Image img;
  FM_VideoSaveConfig c;
  FM_VideoFileWriter v;
  unsigned f;
  char text[256];
  size_t got = 0;
  seed = (unsigned)strtoul(
      getenv("CLOSED_LOOP_SEED") ? getenv("CLOSED_LOOP_SEED") : "0", NULL, 10);
  if (seed > 100u)
    return 2;
  source();
  if (checked(File_WriteAll("/reference.rgb", images, sizeof images),
              "write RGB") ||
      checked(File_WriteAll("/reference.pcm", samples, sizeof samples),
              "write PCM"))
    return 1;
  memset(&img, 0, sizeof img);
  img.width = W;
  img.height = H;
  img.format = FM_PIXEL_RGB888;
  img.channels = 3;
  img.bit_depth = 24;
  img.data = images[0];
  img.data_size = VIDEO_PIXELS;
  img.data_capacity = VIDEO_PIXELS;
  if (checked(Image_Save("/library.png", &img), "save png") ||
      checked(Image_Save("/library.bmp", &img), "save bmp") ||
      checked(Image_Save("/library.jpg", &img), "save jpeg") ||
      write_audio("/library.wav", FM_AUDIO_FILE_WAV) ||
      write_audio("/library.flac", FM_AUDIO_FILE_FLAC))
    return 1;
  memset(&v, 0, sizeof v);
  memset(&c, 0, sizeof c);
  c.width = W;
  c.height = H;
  c.fps = 12;
  c.quality = 90;
  c.chroma_full = 1;
  c.audio_rate = RATE;
  c.audio_channels = 1;
  if (checked(VideoFileWriter_Open(&v, "/library.avi", &c), "video open"))
    return 1;
  for (f = 0; f < FRAMES; f++) {
    FM_Result r = VideoFileWriter_WriteFrame(&v, images[f], FM_PIXEL_RGB888);
    if (r == FM_OK)
      r = VideoFileWriter_WriteAudio(&v, samples + f * (RATE / FRAMES),
                                     RATE / FRAMES);
    if (r != FM_OK) {
      VideoFileWriter_Close(&v);
      return checked(r, "video write");
    }
  }
  if (checked(VideoFileWriter_Close(&v), "video close"))
    return 1;
  if (checked(Text_WriteAll("/library.txt", "alpha beta\nsecond beta\nlast\n"),
              "text write") ||
      checked(Text_Replace("/library.txt", "beta", "omega"), "text replace") ||
      checked(Text_InsertLine("/library.txt", 2, "inserted"), "text insert") ||
      checked(Text_DeleteLine("/library.txt", 3), "text delete") ||
      checked(Text_ReadAll("/library.txt", text, sizeof text, &got),
              "text read"))
    return 1;
  if (strcmp(text, "alpha omega\ninserted\nlast\n")) {
    fprintf(stderr, "text result mismatch: %s\n", text);
    return 1;
  }
  puts("library generation and text round-trip PASS");
  return 0;
}
static int dump_image(const char *input, const char *output) {
  FM_Image i;
  FILE *f;
  FM_Result r;
  memset(&i, 0, sizeof i);
  r = Image_Load(input, &i, pixels, sizeof pixels);
  if (checked(r, "decode image"))
    return 1;
  if (i.width != W || i.height != H || i.format != FM_PIXEL_RGB888) {
    fprintf(stderr, "image layout mismatch\n");
    return 1;
  }
  f = fopen(output, "wb");
  if (!f)
    return 1;
  if (fwrite(pixels, 1, sizeof pixels, f) != sizeof pixels) {
    fclose(f);
    return 1;
  }
  return fclose(f) != 0;
}
static int dump_audio(const char *input, const char *output, uint32_t expected_rate) {
  FM_Audio a;
  FILE *f;
  FM_Result r;
  size_t n;
  memset(&a, 0, sizeof a);
  r = Audio_Open(input, &a);
  if (checked(r, "decode audio"))
    return 1;
  if (a.info.channels != 1 || a.info.sample_rate != expected_rate) {
    Audio_Close(&a);
    return 1;
  }
  f = fopen(output, "wb");
  if (!f) {
    Audio_Close(&a);
    return 1;
  }
  do {
    n = 0;
    r = Audio_Read(&a, pcm, 4096, &n);
    if (n && fwrite(pcm, sizeof pcm[0], n, f) != n) {
      fclose(f);
      Audio_Close(&a);
      return 1;
    }
  } while (r == FM_OK && n);
  fclose(f);
  Audio_Close(&a);
  if (r != FM_ERR_EOF && r != FM_OK)
    return checked(r, "audio read");
  return 0;
}
static int dump_video(const char *input, const char *output,
                      const char *audio_out) {
  FM_Video v;
  FM_VideoInfo i;
  FILE *f, *a;
  FM_Result r;
  unsigned n;
  size_t got;
  uint8_t abuf[4096];
  memset(&v, 0, sizeof v);
  if (checked(Video_Open(input, &v), "video decode open"))
    return 1;
  if (checked(Video_GetInfo(&v, &i), "video info") || i.width != W ||
      i.height != H || i.frame_count != FRAMES) {
    Video_Close(&v);
    return 1;
  }
  if (checked(Video_RegisterCodec(&v, MJPEG_GetCodecOps()), "video register")) {
    Video_Close(&v);
    return 1;
  }
  f = fopen(output, "wb");
  a = fopen(audio_out, "wb");
  if (!f || !a) {
    if (f)
      fclose(f);
    if (a)
      fclose(a);
    Video_Close(&v);
    return 1;
  }
  for (n = 0; n < FRAMES; n++) {
    r = Video_DecodeFrame(&v, pixels, sizeof pixels);
    if (r != FM_OK || fwrite(pixels, 1, sizeof pixels, f) != sizeof pixels) {
      fprintf(stderr, "frame %u r=%d\n", n, (int)r);
      fclose(f);
      fclose(a);
      Video_Close(&v);
      return 1;
    }
  }
  do {
    got = 0;
    r = Video_ReadAudioFrame(&v, abuf, sizeof abuf, &got);
    if (got && fwrite(abuf, 1, got, a) != got) {
      fclose(f);
      fclose(a);
      Video_Close(&v);
      return 1;
    }
  } while (r == FM_OK && got);
  fclose(f);
  fclose(a);
  Video_Close(&v);
  return r == FM_ERR_EOF || r == FM_OK ? 0 : checked(r, "video audio");
}
static int dump_h264(const char *input, const char *output) {
  FM_Video v;
  FM_VideoInfo i;
  FILE *f;
  FM_Result r;
  unsigned n;
  memset(&v, 0, sizeof v);
  if (checked(Video_Open(input, &v), "H264 video open")) return 1;
  if (checked(Video_GetInfo(&v, &i), "H264 video info") ||
      i.width != W || i.height != H || i.frame_count != FRAMES ||
      checked(Video_RegisterCodec(&v, H264_GetCodecOps()), "H264 register")) {
    Video_Close(&v);
    return 1;
  }
  f = fopen(output, "wb");
  if (!f) { Video_Close(&v); return 1; }
  for (n = 0; n < FRAMES; n++) {
    r = Video_DecodeFrame(&v, pixels, sizeof pixels);
    if (r != FM_OK || fwrite(pixels, 1, sizeof pixels, f) != sizeof pixels) {
      fprintf(stderr, "H264 frame %u r=%d\n", n, (int)r);
      fclose(f); Video_Close(&v); return 1;
    }
  }
  fclose(f);
  Video_Close(&v);
  return 0;
}
int main(int argc, char **argv) {
  FM_FS fs;
  int r;
  if (FileManager_Init() != FM_OK || PosixFS_Init(&fs) != FM_OK ||
      FS_Mount("/", &fs) != FM_OK)
    return 2;
  if (argc != 2)
    return 2;
  if (strcmp(argv[1], "generate") == 0)
    r = generate();
  else if (strcmp(argv[1], "verify") == 0) {
    r = dump_image("/external.png", "decoded_external.rgb") ||
        dump_image("/external.bmp", "decoded_external_bmp.rgb") ||
        dump_image("/external.jpg", "decoded_external_jpg.rgb") ||
        dump_audio("/external.wav", "decoded_external.pcm", RATE) ||
        dump_audio("/external.flac", "decoded_external_flac.pcm", RATE) ||
        dump_audio("/external.mp3", "decoded_external_mp3.pcm", RATE) ||
        dump_audio("/external.ogg", "decoded_external_ogg.pcm", RATE) ||
        dump_audio("/external.opus", "decoded_external_opus.pcm", 48000u) ||
        dump_h264("/external_h264.mp4", "decoded_external_h264.rgb") ||
        dump_video("/external.avi", "decoded_external_video.rgb",
                   "decoded_external_video.pcm") ||
        dump_video("/library.avi", "decoded_library_video.rgb",
                   "decoded_library_video.pcm");
    if (!r)
      puts("library decodes external PNG/WAV/FLAC/AVI and its AVI: PASS");
  } else
    r = 2;
  (void)FS_Unmount("/");
  FileManager_Deinit();
  return r;
}
#endif
