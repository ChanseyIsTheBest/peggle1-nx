// culdcept-nx (2026-11-20): the decoder of nxvideo.h on devkitPro's FFmpeg 7.1 (switch-ffmpeg with nvtegra). Built by
// tools/switch/nxvideo/build.sh into one object with its FFmpeg linked in and every symbol but nxv_* made local.
#include "nxvideo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>  // armGetSystemTick, mmuRequestSetAndWait (the clocks: nxv_options)

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_nvtegra.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/version.h>

struct NxvDecoder {
  const AVCodec *codec;
  AVCodecContext *avctx;
  AVBufferRef *device;  // the NVDEC device, when hardware
  AVPacket *packet;
  AVFrame *frame;     // as decoded (AV_PIX_FMT_NVTEGRA on hardware)
  AVFrame *sw_frame;  // the hardware frame transferred to memory
  uint8_t *extradata;
  int extradata_size, width, height, hw, fell_back;
  // 2026-11-20 (the console: the movies at 16-25 ms a frame in the game, 4.5 ms in the benchmark): the clocks and the
  // time in each step
  uint32_t nvdec_max;      // the NVDEC clock nvtegra's DFS starts from (its maximum), Hz
  uint64_t restores;       // the times DFS had lowered it and it was put back
  uint64_t t_send, t_recv, t_xfer, timed;  // system ticks
  // 2026-11-21 (the console: 18 ms a frame in the transfer, the 60 fps narrations dropping frames): FFmpeg's nvtegra
  // copies the picture out of NVDEC's (block-linear) surface with the VIC only into a picture whose planes and pitches
  // are 256-byte aligned; FFmpeg's own picture (av_frame_get_buffer's) is not, so every frame went to its CPU fallback
  // ("Frame address/pitch not aligned to 256, falling back to cpu transfer"): the CPU un-tiles the surface, 18 ms a
  // frame while the game runs. This picture is the decoder's own: NV12 (the surface's format: the VIC's copy is the
  // CPU's byte for byte; nvtegra's YUV420P copy leaves U at 255), planes and pitches 256-aligned, in whole pages of its
  // own (nvtegra maps it for the VIC each frame).
  uint8_t *vic_buf;
  size_t vic_size;
  int vic_w, vic_h, vic_failed;
  uint64_t vic_frames, cpu_frames;
  uint64_t checked, checked_differ, checked_samples;  // verify: frames compared, of them different, samples different
};

// 2026-11-20: nvtegra's DFS (av_nvtegra_dfs_update) sets the NVDEC clock every 20 frames to the bitrate it saw over
// wall time x 1.2: as fast as the frames came. A player that asks for a frame only after the last one is shown (the
// game's) gets a clock that keeps the decode as slow as it already was (a frame took 16-25 ms in the game at 60 fps;
// back to back, 4.5 ms). Kept at the clock DFS started from (its maximum) while a movie plays; and the VIC, which
// copies the picture out of NVDEC's surface (the transfer), at its maximum (nvtegra never sets it).
static int g_keep_max_clock = 1, g_vic_copy = 1, g_verify = 0;
static char g_dump_dir[160];
static int g_dumped;
void nxv_options(int keep_max_clock, int vic_copy, int verify, const char *dump_dir) {
  g_keep_max_clock = keep_max_clock;
  g_vic_copy = vic_copy;
  g_verify = verify;
  snprintf(g_dump_dir, sizeof(g_dump_dir), "%s", dump_dir ? dump_dir : "");
}

static AVNVTegraDeviceContext *nvctx(NxvDecoder *d) {
  if (!d->hw || !d->device) return NULL;
  return (AVNVTegraDeviceContext *)((AVHWDeviceContext *)d->device->data)->hwctx;
}

static enum AVPixelFormat pick_nvtegra(AVCodecContext *avctx, const enum AVPixelFormat *formats) {
  for (const enum AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; ++f)
    if (*f == AV_PIX_FMT_NVTEGRA) return *f;
  return formats[0];  // the hardware path was not offered: software
}

// The decoders by name, not through avcodec_find_decoder: its table names every codec in the library (18 MB of
// libavcodec.a), and the partial link would take them all. Each is an FFCodec, whose first member is its AVCodec.
extern const AVCodec ff_hevc_decoder, ff_h264_decoder, ff_wmv3_decoder;

static const AVCodec *find_codec(int codec) {
  switch (codec) {
    case NXV_HEVC: return &ff_hevc_decoder;
    case NXV_H264: return &ff_h264_decoder;
    case NXV_WMV3: return &ff_wmv3_decoder;
  }
  return NULL;
}

// FFmpeg's own lookups, over these three decoders only. libavcodec's general code (options.o, avcodec.o, utils.o)
// calls them, and their definitions in allcodecs.o, parsers.o and bitstream_filters.o hold the tables of every codec,
// parser and filter in the library (497 decoders: 10.8 MB of code and 21 MB of tables in the object). Defined here, the
// partial link takes none of those three files. No parser and no bitstream filter is used (the decoders take whole
// frames; HEVC, H.264 and WMV3 name no bitstream filter of their own).
#include <libavcodec/bsf.h>
const AVCodec *av_codec_iterate(void **opaque) {
  static const AVCodec *const list[] = {&ff_hevc_decoder, &ff_h264_decoder, &ff_wmv3_decoder};
  uintptr_t i = (uintptr_t)*opaque;
  if (i >= sizeof(list) / sizeof(list[0])) return NULL;
  *opaque = (void *)(i + 1);
  return list[i];
}
const AVCodec *avcodec_find_decoder(enum AVCodecID id) {
  void *it = NULL;
  for (const AVCodec *c; (c = av_codec_iterate(&it));)
    if (c->id == id) return c;
  return NULL;
}
const AVCodec *avcodec_find_decoder_by_name(const char *name) {
  void *it = NULL;
  for (const AVCodec *c; (c = av_codec_iterate(&it));)
    if (name && !strcmp(c->name, name)) return c;
  return NULL;
}
const AVCodec *avcodec_find_encoder(enum AVCodecID id) { return NULL; }
const AVCodec *avcodec_find_encoder_by_name(const char *name) { return NULL; }
const struct AVCodecParser *av_parser_iterate(void **opaque) { return NULL; }
const AVBitStreamFilter *av_bsf_get_by_name(const char *name) { return NULL; }
const AVBitStreamFilter *av_bsf_iterate(void **opaque) { return NULL; }
const AVClass *ff_bsf_child_class_iterate(void **opaque) { return NULL; }

static void close_codec(NxvDecoder *d) {
  if (d->avctx) avcodec_free_context(&d->avctx);
  if (d->device) av_buffer_unref(&d->device);
  d->hw = 0;
}

// The codec context, on NVDEC when `hw` and the device opens.
static int open_codec(NxvDecoder *d, int hw, char *why, int why_size) {
  d->avctx = avcodec_alloc_context3(d->codec);
  if (!d->avctx) return -1;
  d->avctx->width = d->avctx->coded_width = d->width;
  d->avctx->height = d->avctx->coded_height = d->height;
  d->avctx->flags |= AV_CODEC_FLAG_LOW_DELAY;  // a picture out for each packet in (the movies have no reordering)
  d->avctx->thread_count = 1;
  if (d->extradata_size > 0) {
    d->avctx->extradata = av_mallocz(d->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!d->avctx->extradata) return -1;
    memcpy(d->avctx->extradata, d->extradata, d->extradata_size);
    d->avctx->extradata_size = d->extradata_size;
  }
  int device_err = 0;
  if (hw) {
    device_err = av_hwdevice_ctx_create(&d->device, AV_HWDEVICE_TYPE_NVTEGRA, NULL, NULL, 0);
    if (device_err >= 0) {
      d->avctx->hw_device_ctx = av_buffer_ref(d->device);
      d->avctx->get_format = pick_nvtegra;
      d->hw = 1;
    }
  }
  const int err = avcodec_open2(d->avctx, d->codec, NULL);
  AVNVTegraDeviceContext *nv = err >= 0 ? nvctx(d) : NULL;
  if (nv && g_keep_max_clock) {
    mmuRequestSetAndWait(&nv->vic_channel.mmu_request, 0xFFFFFC18u, -1);  // as dfs_init asks for NVDEC's maximum
    mmuRequestGet(&nv->vic_channel.mmu_request, &nv->vic_channel.clock);
  }
  if (why && why_size > 0)
    snprintf(why, (size_t)why_size, "%s %dx%d on %s%s (%s)", d->codec->name, d->width, d->height,
             d->hw ? "NVDEC (nvtegra)" : "the CPU",
             hw && !d->hw ? ", the NVDEC device did not open" : "", nxv_version());
  return err;
}

NxvDecoder *nxv_open(int codec, const uint8_t *extradata, int extradata_size, int width, int height, int hw, char *why,
                     int why_size) {
  const AVCodec *c = find_codec(codec);
  if (!c) {
    if (why && why_size > 0) snprintf(why, (size_t)why_size, "no decoder for codec %d (%s)", codec, nxv_version());
    return NULL;
  }
  NxvDecoder *d = calloc(1, sizeof(*d));
  if (!d) return NULL;
  d->codec = c;
  d->width = width;
  d->height = height;
  if (extradata_size > 0 && extradata) {
    d->extradata = malloc((size_t)extradata_size);
    if (d->extradata) {
      memcpy(d->extradata, extradata, (size_t)extradata_size);
      d->extradata_size = extradata_size;
    }
  }
  d->packet = av_packet_alloc();
  d->frame = av_frame_alloc();
  d->sw_frame = av_frame_alloc();
  if (!d->packet || !d->frame || !d->sw_frame || open_codec(d, hw, why, why_size) < 0) {
    // the hardware path did not open: software
    close_codec(d);
    if (!hw || !d->packet || !d->frame || !d->sw_frame || open_codec(d, 0, why, why_size) < 0) {
      nxv_close(d);
      return NULL;
    }
    d->fell_back = 1;
  }
  return d;
}

#define NXV_ALIGN(x, a) (((x) + (a) - 1) & ~((size_t)(a) - 1))
static void nxv_no_free(void *opaque, uint8_t *data) {}

// `f` as a picture of w x h in `fmt` (NV12 or YUV420P) in `buf` + `offset`: pitches 256-aligned, planes 256-aligned
// from `buf` + `offset`, buf[0] a reference that frees nothing. The size it needs (whole pages) when `buf` is NULL.
static size_t aligned_picture(AVFrame *f, uint8_t *buf, size_t offset, int w, int h, int fmt) {
  const int nv12 = fmt == AV_PIX_FMT_NV12;
  const size_t ly = NXV_ALIGN((size_t)w, 256), lc = nv12 ? ly : NXV_ALIGN((size_t)(w + 1) / 2, 256);
  const size_t sy = NXV_ALIGN(ly * (size_t)h, 256), sc = NXV_ALIGN(lc * (size_t)((h + 1) / 2), 256);
  const size_t size = NXV_ALIGN(offset + sy + (nv12 ? 1 : 2) * sc, 0x1000);
  if (!buf) return size;
  av_frame_unref(f);
  f->buf[0] = av_buffer_create(buf, size, nxv_no_free, NULL, 0);
  if (!f->buf[0]) return 0;
  f->format = fmt;
  f->width = w;
  f->height = h;
  f->data[0] = buf + offset;
  f->data[1] = buf + offset + sy;
  f->data[2] = nv12 ? NULL : buf + offset + sy + sc;
  f->linesize[0] = (int)ly;
  f->linesize[1] = (int)lc;
  f->linesize[2] = nv12 ? 0 : (int)lc;
  f->extended_data = f->data;
  return size;
}

// d->sw_frame as the decoder's aligned NV12 picture of w x h (made again when the size changes).
static int vic_picture(NxvDecoder *d, int w, int h) {
  const int fmt = AV_PIX_FMT_NV12;
  AVFrame *s = d->sw_frame;
  if (d->vic_buf && d->vic_w == w && d->vic_h == h && s->buf[0] && s->format == fmt) return 0;
  av_frame_unref(s);
  free(d->vic_buf);
  d->vic_buf = NULL;
  const size_t size = aligned_picture(s, NULL, 0, w, h, fmt);
  d->vic_buf = aligned_alloc(0x1000, size);
  if (!d->vic_buf) return -1;
  memset(d->vic_buf, 0, size);
  armDCacheFlush(d->vic_buf, size);
  d->vic_size = size;
  if (!aligned_picture(s, d->vic_buf, 0, w, h, fmt)) return -1;
  d->vic_w = w;
  d->vic_h = h;
  return 0;
}

// The sample at (x, y) of plane k (0 Y, 1 U, 2 V) of an NV12 or YUV420P picture.
static int sample(const AVFrame *f, int k, int x, int y) {
  if (k == 0) return f->data[0][(size_t)y * f->linesize[0] + x];
  if (f->format == AV_PIX_FMT_NV12) return f->data[1][(size_t)y * f->linesize[1] + 2 * x + (k - 1)];
  return f->data[k][(size_t)y * f->linesize[k] + x];
}

// a against b, plane k: the samples that differ.
static long compare_plane(const AVFrame *a, const AVFrame *b, int k) {
  const int w = k ? (a->width + 1) / 2 : a->width, h = k ? (a->height + 1) / 2 : a->height;
  long count = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) count += sample(a, k, x, y) != sample(b, k, x, y);
  return count;
}

static void dump(const char *name, const AVFrame *f) {
  char path[256];
  snprintf(path, sizeof(path), "%s/%s_%dx%d.yuv", g_dump_dir, name, f->width, f->height);
  FILE *o = fopen(path, "wb");
  uint8_t *row = malloc((size_t)f->width);
  if (o && row) {
    // planar I420 whatever the picture's layout, a row at a time (a byte at a time held the first intro 20 s)
    for (int k = 0; k < 3; ++k) {
      const int w = k ? (f->width + 1) / 2 : f->width, h = k ? (f->height + 1) / 2 : f->height;
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) row[x] = (uint8_t)sample(f, k, x, y);
        fwrite(row, 1, (size_t)w, o);
      }
    }
  }
  free(row);
  if (o) fclose(o);
}

// verify: the VIC's picture (d->sw_frame) against FFmpeg's CPU fallback's copy of the same frame (an NV12 picture at an
// address that is not 256-aligned, so FFmpeg takes its CPU path), counted in d->checked*. With a dump folder, the first
// compared pair with something in the picture (not one flat colour) is written there as I420.
static void vic_compare(NxvDecoder *d) {
  const int w = d->frame->width, h = d->frame->height;
  AVFrame *c = av_frame_alloc();
  const size_t size = aligned_picture(c, NULL, 64, w, h, AV_PIX_FMT_NV12);
  uint8_t *mem = c ? aligned_alloc(0x1000, size) : NULL;
  if (mem && aligned_picture(c, mem, 64, w, h, AV_PIX_FMT_NV12) &&
      av_hwframe_transfer_data(c, d->frame, 0) >= 0) {
    long n = 0;
    for (int k = 0; k < 3; ++k) n += compare_plane(d->sw_frame, c, k);
    ++d->checked;
    if (n) {
      ++d->checked_differ;
      d->checked_samples += (uint64_t)n;
    }
    if (g_dump_dir[0] && !g_dumped) {
      int lo = 255, hi = 0;
      for (int y = 0; y < h; y += 8)
        for (int x = 0; x < w; x += 8) {
          const int v = sample(c, 0, x, y);
          lo = v < lo ? v : lo;
          hi = v > hi ? v : hi;
        }
      if (hi - lo > 32) {
        g_dumped = 1;
        dump("nxv_cpu", c);
        dump("nxv_vic", d->sw_frame);
      }
    }
  }
  av_frame_free(&c);
  free(mem);
}

static int fill_picture(NxvDecoder *d, NxvPicture *out) {
  const AVFrame *f = d->frame;
  if (f->format == AV_PIX_FMT_NVTEGRA) {
    const uint64_t t = armGetSystemTick();
    int r = -1;
    if (g_vic_copy && !d->vic_failed) {
      if (vic_picture(d, f->width, f->height) == 0) {
        r = av_hwframe_transfer_data(d->sw_frame, d->frame, 0);
        // nvtegra cleans and invalidates the picture's lines before the VIC writes it; invalidated again after, so no
        // line the CPU fetched meanwhile is read
        if (r >= 0) armDCacheFlush(d->vic_buf, d->vic_size);
      }
      if (r < 0)
        d->vic_failed = 1;  // FFmpeg's own picture from here on
      else
        ++d->vic_frames;
    }
    if (r < 0) {
      av_frame_unref(d->sw_frame);
      r = av_hwframe_transfer_data(d->sw_frame, d->frame, 0);
      if (r >= 0) ++d->cpu_frames;
    }
    d->t_xfer += armGetSystemTick() - t;
    if (r < 0) return -1;
    // verify: every 30th frame copied by the VIC (the CPU's copy takes 18 ms: a verify run is a test, not a play)
    if (g_verify && d->sw_frame->buf[0] && d->sw_frame->data[0] == d->vic_buf && d->vic_frames % 30 == 1)
      vic_compare(d);
    f = d->sw_frame;
  }
  if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P && f->format != AV_PIX_FMT_NV12)
    return -1;
  out->nv12 = f->format == AV_PIX_FMT_NV12;
  for (int k = 0; k < 3; ++k) {
    out->data[k] = f->data[k];
    out->linesize[k] = f->linesize[k];
  }
  out->width = f->width;
  out->height = f->height;
  return 1;
}

int nxv_decode(NxvDecoder *d, const uint8_t *data, int size, NxvPicture *out) {
  if (!d || !d->avctx || !data || size <= 0) return -1;
  av_packet_unref(d->packet);
  if (av_new_packet(d->packet, size) < 0) return -1;
  memcpy(d->packet->data, data, (size_t)size);
  const uint64_t t0 = armGetSystemTick();
  int err = avcodec_send_packet(d->avctx, d->packet);
  if (err < 0 && d->hw && !d->fell_back) {
    // NVDEC refused the stream: software from here on (the next keyframe shows again)
    close_codec(d);
    d->fell_back = 1;
    if (open_codec(d, 0, NULL, 0) < 0) return -1;
    err = avcodec_send_packet(d->avctx, d->packet);
  }
  if (err < 0) return -1;
  const uint64_t t1 = armGetSystemTick();
  err = avcodec_receive_frame(d->avctx, d->frame);
  d->t_send += t1 - t0;
  d->t_recv += armGetSystemTick() - t1;
  ++d->timed;
  // DFS has run (it runs at a frame's end): its clock put back to the maximum it started from
  AVNVTegraDeviceContext *nv = nvctx(d);
  if (nv) {
    const uint32_t c = nv->nvdec_channel.clock;
    if (c > d->nvdec_max) {
      d->nvdec_max = c;
    } else if (g_keep_max_clock && c < d->nvdec_max) {
      mmuRequestSetAndWait(&nv->nvdec_channel.mmu_request, d->nvdec_max, -1);
      mmuRequestGet(&nv->nvdec_channel.mmu_request, &nv->nvdec_channel.clock);
      ++d->restores;
    }
  }
  if (err < 0) return err == AVERROR(EAGAIN) ? 0 : -1;
  return fill_picture(d, out);
}

void nxv_stats(NxvDecoder *d, NxvStats *out) {
  memset(out, 0, sizeof(*out));
  if (!d) return;
  const double ms = 1e-6;
  out->frames = d->timed;
  out->send_ms = (double)armTicksToNs(d->t_send) * ms;
  out->receive_ms = (double)armTicksToNs(d->t_recv) * ms;
  out->transfer_ms = (double)armTicksToNs(d->t_xfer) * ms;
  AVNVTegraDeviceContext *nv = nvctx(d);
  out->nvdec_hz = nv ? nv->nvdec_channel.clock : 0;
  out->vic_hz = nv ? nv->vic_channel.clock : 0;
  out->nvdec_max_hz = d->nvdec_max;
  out->restores = d->restores;
  out->vic_frames = d->vic_frames;
  out->cpu_frames = d->cpu_frames;
  out->checked = d->checked;
  out->checked_differ = d->checked_differ;
  out->checked_samples = d->checked_samples;
  d->t_send = d->t_recv = d->t_xfer = d->timed = 0;
}

void nxv_flush(NxvDecoder *d) {
  if (d && d->avctx) avcodec_flush_buffers(d->avctx);
}

void nxv_close(NxvDecoder *d) {
  if (!d) return;
  close_codec(d);
  if (d->packet) av_packet_free(&d->packet);
  if (d->frame) av_frame_free(&d->frame);
  if (d->sw_frame) av_frame_free(&d->sw_frame);
  free(d->vic_buf);
  free(d->extradata);
  free(d);
}

int nxv_is_hw(const NxvDecoder *d) { return d && d->hw; }

const char *nxv_version(void) {
  static char text[64];
  if (!text[0])
    snprintf(text, sizeof(text), "FFmpeg %s, libavcodec %d.%d.%d", av_version_info(), LIBAVCODEC_VERSION_MAJOR,
             LIBAVCODEC_VERSION_MINOR, LIBAVCODEC_VERSION_MICRO);
  return text;
}
