/*
 * NewTek NDI output
 * Copyright (c) 2017 Maksym Veremeyenko
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <pthread.h>
#include "libavformat/avformat.h"
#include "libavformat/internal.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavutil/time.h"
#include "libavutil/timecode.h"

#include "libndi_newtek_common.h"
#include "packet_queue.h"

struct NDIContext {
    const AVClass *cclass;

    /* Options */
    int reference_level;
    int clock_video, clock_audio;
    int64_t timecode_offset;
    int timecode_frame_tolerance_hard;
    int timecode_frame_tolerance_soft;
    int timecode_frames_buffer;

    NDIlib_video_frame_t *video;
    NDIlib_audio_frame_interleaved_16s_t *audio;
    NDIlib_send_instance_t ndi_send;
    AVFrame *last_avframe;
    int64_t pts_offset;
    int frame_diff;
    int frame_drop;

    int video_worker_thread_should_run;
    int audio_worker_thread_should_run;
    AVPacketQueue vqueue;
    AVPacketQueue aqueue;
};

static int ndi_write_trailer(AVFormatContext *avctx)
{
    struct NDIContext *ctx = avctx->priv_data;

    if (ctx->ndi_send) {
        NDIlib_send_destroy(ctx->ndi_send);
        av_frame_free(&ctx->last_avframe);
    }

    if (ctx->vqueue.max_size) {
        avpacket_queue_end(&ctx->vqueue);
    }
    if (ctx->aqueue.max_size) {
        avpacket_queue_end(&ctx->aqueue);
    }

    av_freep(&ctx->video);
    av_freep(&ctx->audio);

    ctx->video_worker_thread_should_run = 0;
    ctx->audio_worker_thread_should_run = 0;

    return 0;
}

static int ndi_write_video_packet(AVFormatContext *avctx, AVStream *st, AVPacket *pkt)
{
    struct NDIContext *ctx = avctx->priv_data;
    AVFrame *avframe, *tmp = (AVFrame *)pkt->data;
    int64_t final_pts;

    if (tmp->format != AV_PIX_FMT_UYVY422 && tmp->format != AV_PIX_FMT_BGRA &&
        tmp->format != AV_PIX_FMT_BGR0 && tmp->format != AV_PIX_FMT_RGBA &&
        tmp->format != AV_PIX_FMT_RGB0) {
        av_log(avctx, AV_LOG_ERROR, "Got a frame with invalid pixel format.\n");
        return AVERROR(EINVAL);
    }

    if (tmp->linesize[0] < 0) {
        av_log(avctx, AV_LOG_ERROR, "Got a frame with negative linesize.\n");
        return AVERROR(EINVAL);
    }

    if (tmp->width  != ctx->video->xres ||
        tmp->height != ctx->video->yres) {
        av_log(avctx, AV_LOG_ERROR, "Got a frame with invalid dimension.\n");
        av_log(avctx, AV_LOG_ERROR, "tmp->width=%d, tmp->height=%d, ctx->video->xres=%d, ctx->video->yres=%d\n",
            tmp->width, tmp->height, ctx->video->xres, ctx->video->yres);
        return AVERROR(EINVAL);
    }

    avframe = av_frame_clone(tmp);
    if (!avframe)
        return AVERROR(ENOMEM);

    if (ctx->timecode_offset > 0) {
        AVFrameSideData *sd = av_frame_get_side_data(avframe, AV_FRAME_DATA_S12M_TIMECODE);
        if (sd) {
            st = avctx->streams[pkt->stream_index];
            int32_t *tc_frame = (int32_t*)sd->data;
            int32_t frame_timecode = tc_frame[1];
            char frame_timecode_str[64];
            av_timecode_make_smpte_tc_string(frame_timecode_str, frame_timecode, 0);
            int frame_framenum = av_timecode_get_smpte_framenum(frame_timecode, st->avg_frame_rate, 0);

            AVTimecode local_timecode;
            av_timecode_init_from_now2(&local_timecode, st->avg_frame_rate, 0, -ctx->timecode_offset, NULL);
            char local_timecode_str[64];
            av_timecode_make_string(&local_timecode, local_timecode_str, 0);
            int local_framenum = av_timecode_get_framenum(&local_timecode);

            int64_t frame_diff = frame_framenum - local_framenum - ctx->pts_offset;
            av_log(avctx, AV_LOG_DEBUG,
                "frame_tc=%s local_tc=%s (frame_diff=%ld frame->pts=%ld pts_offset=%ld q.nb_packets=%d)\n", 
                frame_timecode_str, local_timecode_str, frame_diff, avframe->pts, ctx->pts_offset, ctx->vqueue.nb_packets
            );

            ctx->frame_diff = frame_diff;
        }
    }

    if (pkt->pts && avframe->pts % 30 == 0 && FFABS(ctx->frame_diff) > ctx->timecode_frame_tolerance_soft) {
        int offset = 0;
        if (ctx->frame_diff < 0) {
            int to_drop = -ctx->frame_diff;
            to_drop -= ctx->timecode_frame_tolerance_hard;
            if (to_drop <= 0) {
                to_drop = 1;
            }
            ctx->frame_drop = ctx->frame_drop + to_drop;
            offset = -to_drop;
        }
        if (ctx->frame_diff > 0) {
            int to_dup = ctx->frame_diff - ctx->timecode_frame_tolerance_hard;
            if (to_dup <= 0) {
                to_dup = 1;
            }
            ctx->frame_drop = FFMAX(0, ctx->frame_drop - to_dup);
            offset = to_dup;
        }
        if (offset != 0) {
            av_log(avctx, AV_LOG_WARNING, "Adjusting pts offset by %d (from %ld to %ld) [frame difference = %d]\n", offset, ctx->pts_offset, ctx->pts_offset + offset, ctx->frame_diff);
            ctx->pts_offset += offset;
        }
    }

    final_pts = pkt->pts + ctx->pts_offset;
    ctx->video->timecode = av_rescale_q(final_pts, st->time_base, NDI_TIME_BASE_Q);

    ctx->video->line_stride_in_bytes = avframe->linesize[0];
    ctx->video->p_data = (void *)(avframe->data[0]);

    av_log(
        avctx, AV_LOG_DEBUG, 
        "%s: pkt->pts=%"PRId64", pts_offset=%"PRId64", final_pts=%"PRId64", timecode=%"PRId64", st->time_base=%d/%d\n",
        __func__, pkt->pts, ctx->pts_offset, final_pts, ctx->video->timecode, st->time_base.num, st->time_base.den
    );

    /* asynchronous for one frame, but will block if a second frame
        is given before the first one has been sent */
    NDIlib_send_send_video_async(ctx->ndi_send, ctx->video);

    av_frame_free(&ctx->last_avframe);
    ctx->last_avframe = avframe;
    ctx->last_avframe->pts = pkt->pts;

    return 0;
}

static int ndi_write_audio_packet(AVFormatContext *avctx, AVStream *st, AVPacket *pkt)
{
    struct NDIContext *ctx = avctx->priv_data;
    int64_t final_pts = pkt->pts + ctx->pts_offset;

    ctx->audio->p_data = (short *)pkt->data;
    ctx->audio->timecode = av_rescale_q(pkt->pts, st->time_base, NDI_TIME_BASE_Q);
    ctx->audio->no_samples = pkt->size / (ctx->audio->no_channels << 1);

    av_log(
        avctx, AV_LOG_DEBUG, 
        "%s: pkt->pts=%"PRId64", pts_offset=%"PRId64", final_pts=%"PRId64", timecode=%"PRId64", st->time_base=%d/%d\n",
        __func__, pkt->pts, ctx->pts_offset, final_pts, ctx->audio->timecode, st->time_base.num, st->time_base.den
    );

    NDIlib_util_send_send_audio_interleaved_16s(ctx->ndi_send, ctx->audio);

    return 0;
}

static void *ndi_video_worker_thread(void *arg) {
    AVFormatContext *avctx = (AVFormatContext *)arg;
    struct NDIContext *ctx = avctx->priv_data;
    AVStream *st;

    while (ctx->video_worker_thread_should_run) {
        AVPacket pkt;
        int ret = avpacket_queue_get(&ctx->vqueue, &pkt, 0);
        if (!ret) {
            st = avctx->streams[pkt.stream_index];
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                ndi_write_video_packet(avctx, st, &pkt);
                av_packet_unref(&pkt);
            } else {
                av_log(avctx, AV_LOG_ERROR, "Non video packet in video queue.\n");
            }
        } else if (ctx->clock_video) {
            av_log(avctx, AV_LOG_WARNING, "Video packet queue is empty.\n");
            if (st && ctx->last_avframe) {
                av_log(avctx, AV_LOG_WARNING, "Duplicating video frame.\n");
                ctx->pts_offset += 1;
                ctx->video->timecode = av_rescale_q(ctx->last_avframe->pts + ctx->pts_offset, st->time_base, NDI_TIME_BASE_Q);
                NDIlib_send_send_video_async(ctx->ndi_send, ctx->video);
            } else {
                av_log(avctx, AV_LOG_WARNING, "Unknown stream or no video frame available. Skipping frame duplication.\n");
                av_usleep(30000);
            }
        }
    }
    return NULL;
}

static void *ndi_audio_worker_thread(void *arg) {
    AVFormatContext *avctx = (AVFormatContext *)arg;
    struct NDIContext *ctx = avctx->priv_data;

    while (ctx->audio_worker_thread_should_run) {
        AVPacket pkt;
        int ret = avpacket_queue_get(&ctx->aqueue, &pkt, 1);
        if (!ret) {
            AVStream *st = avctx->streams[pkt.stream_index];
            if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                ndi_write_audio_packet(avctx, st, &pkt);
                av_packet_unref(&pkt);
            } else {
                av_log(avctx, AV_LOG_ERROR, "Non audio packet in audio queue.\n");
            }
        } else {
            av_log(avctx, AV_LOG_WARNING, "Audio packet queue is empty.\n");
        }
    }
    return NULL;
}

static int ndi_write_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct NDIContext *ctx = avctx->priv_data;
    AVStream *st = avctx->streams[pkt->stream_index];
    AVPacketQueue *q;
    int is_video = 0;

    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        q = &ctx->vqueue;
        is_video = 1;
    } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        q = &ctx->aqueue;
    } else {
        return AVERROR_BUG;
    }
    if (avpacket_queue_put(q, pkt)) {
        av_log(
            avctx, AV_LOG_WARNING, 
            "%s packet queue is full (%lu bytes, %d packets). Dropping packet.\n", 
            is_video ? "Video" : "Audio", avpacket_queue_size(q), q->nb_packets
        );
    }
    return 0;
}

static int ndi_setup_audio(AVFormatContext *avctx, AVStream *st)
{
    struct NDIContext *ctx = avctx->priv_data;
    AVCodecParameters *c = st->codecpar;
    pthread_t worker_thread_id;

    if (ctx->audio) {
        av_log(avctx, AV_LOG_ERROR, "Only one audio stream is supported!\n");
        return AVERROR(EINVAL);
    }

    ctx->audio = av_mallocz(sizeof(NDIlib_audio_frame_interleaved_16s_t));
    if (!ctx->audio)
        return AVERROR(ENOMEM);

    ctx->audio->sample_rate = c->sample_rate;
    ctx->audio->no_channels = c->channels;
    ctx->audio->reference_level = ctx->reference_level;

    avpriv_set_pts_info(st, 64, 1, NDI_TIME_BASE);

    if (!ctx->audio_worker_thread_should_run) {
        int64_t q_size = 64*1024*1024; // 64 MB
        avpacket_queue_init(&ctx->aqueue, q_size, avctx);
        ctx->audio_worker_thread_should_run = 1;
        pthread_create(&worker_thread_id, NULL, ndi_audio_worker_thread, (void *)avctx);
    }

    return 0;
}

static int ndi_setup_video(AVFormatContext *avctx, AVStream *st)
{
    struct NDIContext *ctx = avctx->priv_data;
    AVCodecParameters *c = st->codecpar;
    pthread_t worker_thread_id;

    if (ctx->video) {
        av_log(avctx, AV_LOG_ERROR, "Only one video stream is supported!\n");
        return AVERROR(EINVAL);
    }

    if (c->codec_id != AV_CODEC_ID_WRAPPED_AVFRAME) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported codec format!"
               " Only AV_CODEC_ID_WRAPPED_AVFRAME is supported (-vcodec wrapped_avframe).\n");
        return AVERROR(EINVAL);
    }

    if (c->format != AV_PIX_FMT_UYVY422 && c->format != AV_PIX_FMT_BGRA &&
        c->format != AV_PIX_FMT_BGR0 && c->format != AV_PIX_FMT_RGBA &&
        c->format != AV_PIX_FMT_RGB0) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format!"
               " Only AV_PIX_FMT_UYVY422, AV_PIX_FMT_BGRA, AV_PIX_FMT_BGR0,"
               " AV_PIX_FMT_RGBA, AV_PIX_FMT_RGB0 is supported.\n");
        return AVERROR(EINVAL);
    }

    if (c->field_order == AV_FIELD_BB || c->field_order == AV_FIELD_BT) {
        av_log(avctx, AV_LOG_ERROR, "Lower field-first disallowed");
        return AVERROR(EINVAL);
    }

    ctx->video = av_mallocz(sizeof(NDIlib_video_frame_t));
    if (!ctx->video)
        return AVERROR(ENOMEM);

    switch(c->format) {
        case AV_PIX_FMT_UYVY422:
            ctx->video->FourCC = NDIlib_FourCC_type_UYVY;
            break;
        case AV_PIX_FMT_BGRA:
            ctx->video->FourCC = NDIlib_FourCC_type_BGRA;
            break;
        case AV_PIX_FMT_BGR0:
            ctx->video->FourCC = NDIlib_FourCC_type_BGRX;
            break;
        case AV_PIX_FMT_RGBA:
            ctx->video->FourCC = NDIlib_FourCC_type_RGBA;
            break;
        case AV_PIX_FMT_RGB0:
            ctx->video->FourCC = NDIlib_FourCC_type_RGBX;
            break;
    }

    ctx->video->xres = c->width;
    ctx->video->yres = c->height;
    ctx->video->frame_rate_N = st->avg_frame_rate.num;
    ctx->video->frame_rate_D = st->avg_frame_rate.den;
    ctx->video->frame_format_type = c->field_order == AV_FIELD_PROGRESSIVE
        ? NDIlib_frame_format_type_progressive
        : NDIlib_frame_format_type_interleaved;

    if (st->sample_aspect_ratio.num) {
        AVRational display_aspect_ratio;
        av_reduce(&display_aspect_ratio.num, &display_aspect_ratio.den,
                  st->codecpar->width  * (int64_t)st->sample_aspect_ratio.num,
                  st->codecpar->height * (int64_t)st->sample_aspect_ratio.den,
                  1024 * 1024);
        ctx->video->picture_aspect_ratio = av_q2d(display_aspect_ratio);
    }
    else
        ctx->video->picture_aspect_ratio = (double)st->codecpar->width/st->codecpar->height;

    avpriv_set_pts_info(st, 64, 1, NDI_TIME_BASE);

    if (!ctx->video_worker_thread_should_run) {
        int64_t q_size = 1024*1024*1024; // 1 GB
        avpacket_queue_init(&ctx->vqueue, q_size, avctx);
        ctx->video_worker_thread_should_run = 1;
        pthread_create(&worker_thread_id, NULL, ndi_video_worker_thread, (void *)avctx);
    }

    return 0;
}

static int ndi_write_header(AVFormatContext *avctx)
{
    int ret = 0;
    unsigned int n;
    struct NDIContext *ctx = avctx->priv_data;
    const NDIlib_send_create_t ndi_send_desc = { .p_ndi_name = avctx->url,
        .p_groups = NULL, .clock_video = ctx->clock_video, .clock_audio = ctx->clock_audio };

    if (!NDIlib_initialize()) {
        av_log(avctx, AV_LOG_ERROR, "NDIlib_initialize failed.\n");
        return AVERROR_EXTERNAL;
    }

    /* check if streams compatible */
    for (n = 0; n < avctx->nb_streams; n++) {
        AVStream *st = avctx->streams[n];
        AVCodecParameters *c = st->codecpar;
        if        (c->codec_type == AVMEDIA_TYPE_AUDIO) {
            if ((ret = ndi_setup_audio(avctx, st)))
                goto error;
        } else if (c->codec_type == AVMEDIA_TYPE_VIDEO) {
            if ((ret = ndi_setup_video(avctx, st)))
                goto error;
        } else {
            av_log(avctx, AV_LOG_ERROR, "Unsupported stream type.\n");
            ret = AVERROR(EINVAL);
            goto error;
        }
    }

    ctx->ndi_send = NDIlib_send_create(&ndi_send_desc);
    if (!ctx->ndi_send) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create NDI output %s\n", avctx->url);
        ret = AVERROR_EXTERNAL;
    }

error:
    return ret;
}

#define OFFSET(x) offsetof(struct NDIContext, x)
static const AVOption options[] = {
    { "reference_level", "The audio reference level in dB"  , OFFSET(reference_level), AV_OPT_TYPE_INT, { .i64 = 0 }, -20, 20, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM},
    { "clock_video", "These specify whether video 'clock' themselves"  , OFFSET(clock_video), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM },
    { "clock_audio", "These specify whether audio 'clock' themselves"  , OFFSET(clock_audio), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM },
    { "timecode_offset", "how long in microseconds the video should be delayed relative to current time (0 is disabled)", OFFSET(timecode_offset), AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, INT64_MAX, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM },
    { "timecode_frame_tolerance_hard", "how many frames the video should be offset from the target before adjusting abruptly", OFFSET(timecode_frame_tolerance_hard), AV_OPT_TYPE_INT, { .i64 = 30 }, 0, 120, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM },
    { "timecode_frame_tolerance_soft", "how many frames the video should be offset from the target before adjusting slowly", OFFSET(timecode_frame_tolerance_soft), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 30, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM },
    { "timecode_frames_buffer", "the max amount of frames that can be buffered before dropping", OFFSET(timecode_frames_buffer), AV_OPT_TYPE_INT, { .i64 = 60 }, 0, 600, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM },
    { NULL },
};

static const AVClass libndi_newtek_muxer_class = {
    .class_name = "NDI muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT,
};

AVOutputFormat ff_libndi_newtek_muxer = {
    .name           = "libndi_newtek",
    .long_name      = NULL_IF_CONFIG_SMALL("Network Device Interface (NDI) output using NewTek library"),
    .audio_codec    = AV_CODEC_ID_PCM_S16LE,
    .video_codec    = AV_CODEC_ID_WRAPPED_AVFRAME,
    .subtitle_codec = AV_CODEC_ID_NONE,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &libndi_newtek_muxer_class,
    .priv_data_size = sizeof(struct NDIContext),
    .write_header   = ndi_write_header,
    .write_packet   = ndi_write_packet,
    .write_trailer  = ndi_write_trailer,
};
