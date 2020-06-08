//
// Created by wxc on 2020/6/8.
//
#include <jni.h>
#include <android/log.h>

#include "FFmpegCenter.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include <libavfilter/buffersrc.h>
}

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MEDIA_CORE", __VA_ARGS__)
#define LOG_RESOURCE(...) __android_log_print(ANDROID_LOG_ERROR, "MEDIA_RESOURCE", __VA_ARGS__)

AVFormatContext *ifmt_ctx;
AVFormatContext *ofmt_ctx;
AVFilterContext *buffersink_ctx;
AVFilterContext *buffersrc_ctx;
AVFilterGraph *filter_graph;
typedef struct StreamContext {
    AVCodecContext *dec_ctx;
    AVCodecContext *enc_ctx;
} StreamContext;
StreamContext *stream_ctx;
int videoIndex = -1;


int open_input_file(const char *path);
int open_output_file(const char *path);
int init_filters(const char *descr);
void encode_write_frame(AVFrame *inputFrame, unsigned int stream_index);
void log_ffmpeg(void *ptr, int level, const char *fmt, va_list vl);

void transcode_with_filter(JNIEnv *env, jclass c, jstring input, jstring output, jstring filter){
    int ret = -1;
    AVFrame *frame = NULL;
    AVFrame *filt_frame;
    AVPacket packet;
    unsigned int stream_index;

    const char *inputPath = env->GetStringUTFChars(input, 0);
    const char *outputPath = env->GetStringUTFChars(output, 0);
    const char *filterPath = env->GetStringUTFChars(filter, 0);

    av_log_set_callback(log_ffmpeg);

    LOGE("input path %s-%s-%s",inputPath, outputPath, filterPath);
    if(open_input_file(inputPath) < 0){
        LOGE("open input file failed, path:%s",inputPath);
        return;
    }

    if (open_output_file(outputPath) < 0){
        LOGE("open output file failed, path:%s",outputPath);
        return;
    }

    if ((init_filters(filterPath)) < 0){
        LOGE("init filters failed,");
        return;
    }

    while (true){
        if ((ret = av_read_frame(ifmt_ctx, &packet)) < 0)
            break;
        stream_index = packet.stream_index;
        LOGE("Demuxer gave frame of stream_index %u, time = %f", stream_index, packet.pts * av_q2d(ifmt_ctx->streams[packet.stream_index]->time_base));
        ret = avcodec_send_packet(stream_ctx[stream_index].dec_ctx, &packet);
        if (ret < 0) {
            LOGE("Error while sending a packet to the decoder\n");
            break;
        }
        while (ret >= 0){
            frame = av_frame_alloc();
            ret = avcodec_receive_frame(stream_ctx[stream_index].dec_ctx, frame);
            if (ret < 0) {
                av_frame_free(&frame);
                LOGE("Decoding failed");
                break;
            }

            LOGE("GOT FRAME ,%f" ,frame->pts * av_q2d(ifmt_ctx->streams[stream_index]->time_base));
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                LOGE("Error while receiving a frame from the decoder\n");
                goto end;
            }
            frame->pts = frame->best_effort_timestamp;

            if (stream_index == videoIndex) {
                /* push the decoded frame into the filtergraph */
                if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) <
                    0) {
                    LOGE("Error while feeding the filtergraph\n");
                    break;
                }

                /* pull filtered frames from the filtergraph */
                while (1) {
                    filt_frame = av_frame_alloc();
                    ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    if (ret < 0)
                        goto end;
                    filt_frame->pict_type = AV_PICTURE_TYPE_NONE;
                    encode_write_frame(filt_frame, stream_index);
                    av_frame_unref(filt_frame);
                }
            } else {
                encode_write_frame(frame, stream_index);
            }
            av_frame_free(&frame);
        }
        av_packet_unref(&packet);
    }
    av_write_trailer(ofmt_ctx);

end:
    av_packet_unref(&packet);
    av_frame_free(&frame);
    for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
        avcodec_free_context(&stream_ctx[i].dec_ctx);
        if (ofmt_ctx && ofmt_ctx->nb_streams > i && ofmt_ctx->streams[i] && stream_ctx[i].enc_ctx)
            avcodec_free_context(&stream_ctx[i].enc_ctx);
    }
    av_free(stream_ctx);
    avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);

    if (ret < 0)
        LOGE("Error occurred: %s\n", av_err2str(ret));
}

/**
 * open the input file
 * @param filename
 * @return
 */
int open_input_file(const char *filename)
{
    int ret;
    AVCodec *dec;

    ifmt_ctx = NULL;
    if ((ret = avformat_open_input(&ifmt_ctx, filename, NULL, NULL)) < 0) {
        LOGE("Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
        LOGE("Cannot find stream information\n");
        return ret;
    }

    /* select the video stream */
    ret = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (ret < 0) {
        LOGE("Cannot find a video stream in the input file\n");
        return ret;
    }
    videoIndex = ret;

    stream_ctx = static_cast<StreamContext *>(av_mallocz_array(ifmt_ctx->nb_streams,
                                                               sizeof(*stream_ctx)));
    for (int i = 0; i < ifmt_ctx->nb_streams; i++){
        AVStream *stream = ifmt_ctx->streams[i];
        AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
        AVCodecContext *codec_ctx;
        if (!dec) {
            LOGE("Failed to find decoder for stream #%u\n", i);
            return AVERROR_DECODER_NOT_FOUND;
        }
        codec_ctx = avcodec_alloc_context3(dec);
        if (!codec_ctx) {
            LOGE("Failed to allocate the decoder context for stream #%u\n", i);
            return AVERROR(ENOMEM);
        }
        ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar);
        if (ret < 0) {
            LOGE("Failed to copy decoder parameters to input decoder context "
                 "for stream #%u\n", i);
            return ret;
        }

        if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
            || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
                codec_ctx->framerate = av_guess_frame_rate(ifmt_ctx, stream, NULL);
            /* Open decoder */
            ret = avcodec_open2(codec_ctx, dec, NULL);
            if (ret < 0) {
                LOGE("Failed to open decoder for stream #%u\n", i);
                return ret;
            }
        }
        stream_ctx[i].dec_ctx = codec_ctx;
    }
    av_dump_format(ifmt_ctx, 0, filename, 0);
    LOGE("FINISH OPEN INPUT FILE");
    return 0;
}

int open_output_file(const char *filename)
{
    AVStream *out_stream;
    AVStream *in_stream;
    AVCodecContext *dec_ctx, *enc_ctx;
    AVCodec *encoder;
    int ret;
    unsigned int i;

    ofmt_ctx = NULL;
    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, filename);
    if (!ofmt_ctx) {
        LOGE("Could not create output context\n -- %d",ifmt_ctx->nb_streams);
        return AVERROR_UNKNOWN;
    }

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            LOGE("Failed allocating output stream\n");
            return AVERROR_UNKNOWN;
        }

        in_stream = ifmt_ctx->streams[i];
        dec_ctx = stream_ctx[i].dec_ctx;

        if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
            || dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            /* in this example, we choose transcoding to same codec */
            if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO){
                encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
            } else {
                encoder = avcodec_find_encoder(dec_ctx->codec_id);
            }
            if (!encoder) {
                LOGE("Necessary encoder not found\n");
                return AVERROR_INVALIDDATA;
            }
            enc_ctx = avcodec_alloc_context3(encoder);
            if (!enc_ctx) {
                LOGE("Failed to allocate the encoder context\n");
                return AVERROR(ENOMEM);
            }

            /* In this example, we transcode to same properties (picture size,
             * sample rate etc.). These properties can be changed for output
             * streams easily using filters */
            if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
                enc_ctx->height = dec_ctx->height;
                enc_ctx->width = dec_ctx->width;
                if (dec_ctx->sample_aspect_ratio.num == 0){
                    LOGE("can't get sample aspect ratio ,use default");
                    enc_ctx->sample_aspect_ratio = av_make_q(1, 1);
                } else {
                    enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
                }
                /* take first format from list of supported formats */
                if (encoder->pix_fmts)
                    enc_ctx->pix_fmt = encoder->pix_fmts[0];
                else
                    enc_ctx->pix_fmt = dec_ctx->pix_fmt;
                /* video time_base can be set to whatever is handy and supported by encoder */
                /* frames per second */
                enc_ctx->time_base = av_inv_q(dec_ctx->framerate);
                if (enc_ctx->codec_id == AV_CODEC_ID_MPEG4 && (enc_ctx->time_base.den > 65535 || enc_ctx->time_base.num > 65536)){
                    // the maximum admitted value for the timebase denominator is 65535
                    LOGE("time_base error, time_base-den:%d,time_base:%d,", enc_ctx->time_base.den, enc_ctx->time_base.num);
                    double f = enc_ctx->time_base.den / (double)enc_ctx->time_base.num;
                    enc_ctx->time_base = av_make_q(65535/f ,65535);
                    LOGE("New frame rate---time_base-den:%d,time_base:%d,", enc_ctx->time_base.den, enc_ctx->time_base.num);
                }
                LOGE("width = %d, height = %d, sample_aspect_ratio.den=%d, sample_aspect_ratio.num=%d, pix_fmt= %d, timebase.den=%d, timebase.num=%d", enc_ctx->width, enc_ctx->height, enc_ctx->sample_aspect_ratio.den, enc_ctx->sample_aspect_ratio.num, enc_ctx->pix_fmt, enc_ctx->time_base.den, enc_ctx->time_base.num);
//                enc_ctx->framerate = dec_ctx->framerate;
//                enc_ctx->bit_rate = dec_ctx->bit_rate;
            } else {
                enc_ctx->sample_rate = dec_ctx->sample_rate;
                enc_ctx->channel_layout = dec_ctx->channel_layout;
                enc_ctx->channels = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
                /* take first format from list of supported formats */
                enc_ctx->sample_fmt = encoder->sample_fmts[0];
                enc_ctx->time_base = (AVRational){1, enc_ctx->sample_rate};
            }

            if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
                enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            /* Third parameter can be used to pass settings to encoder */
            ret = avcodec_open2(enc_ctx, encoder, NULL);
            if (ret < 0) {
                LOGE("Cannot open video encoder for codec #%s, error = %s", encoder->long_name, av_err2str(ret));
                return ret;
            }
            ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
            if (ret < 0) {
                LOGE("Failed to copy encoder parameters to output stream #%u\n", i);
                return ret;
            }

            out_stream->time_base = enc_ctx->time_base;
            stream_ctx[i].enc_ctx = enc_ctx;
        } else if (dec_ctx->codec_type == AVMEDIA_TYPE_UNKNOWN) {
            LOGE("Elementary stream #%d is of unknown type, cannot proceed\n", i);
            return AVERROR_INVALIDDATA;
        } else {
            /* if this stream must be remuxed */
            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0) {
                LOGE("Copying parameters for stream #%u failed\n", i);
                return ret;
            }
            out_stream->time_base = in_stream->time_base;
        }

    }
    av_dump_format(ofmt_ctx, 0, filename, 1);

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOGE("Could not open output file '%s'", filename);
            return ret;
        }
    }

    /* init muxer, write output file header */
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        LOGE("Error occurred when opening output file\n");
        return ret;
    }
    LOGE("FINISH OPEN OUTPUT FILE");
    return 0;
}

int init_filters(const char *filters_descr)
{
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVRational time_base = ifmt_ctx->streams[videoIndex]->time_base;
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE };

    filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             stream_ctx[videoIndex].dec_ctx->width, stream_ctx[videoIndex].dec_ctx->height, stream_ctx[videoIndex].dec_ctx->pix_fmt,
             time_base.num, time_base.den,
             stream_ctx[videoIndex].dec_ctx->sample_aspect_ratio.num, stream_ctx[videoIndex].dec_ctx->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                       args, NULL, filter_graph);
    if (ret < 0) {
        LOGE("Cannot create buffer source\n");
        goto end;
    }

    /* buffer video sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                       NULL, NULL, filter_graph);
    if (ret < 0) {
        LOGE("Cannot create buffer sink\n");
        goto end;
    }

//    ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts,
//                              AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    ret = av_opt_set_bin(buffersink_ctx, "pix_fmts",
                         (uint8_t*)&stream_ctx[videoIndex].dec_ctx->pix_fmt, sizeof(stream_ctx[videoIndex].dec_ctx->pix_fmt),
                         AV_OPT_SEARCH_CHILDREN);

    if (ret < 0) {
        LOGE("Cannot set output pixel format\n");
        goto end;
    }

    /*
     * Set the endpoints for the filter graph. The filter_graph will
     * be linked to the graph described by filters_descr.
     */

    /*
     * The buffer source output must be connected to the input pad of
     * the first filter described by filters_descr; since the first
     * filter input label is not specified, it is set to "in" by
     * default.
     */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    /*
     * The buffer sink input must be connected to the output pad of
     * the last filter described by filters_descr; since the last
     * filter output label is not specified, it is set to "out" by
     * default.
     */
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr,
                                        &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

    end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    LOGE("FINISH INIT FILTER, result = %d",ret);
    return ret;
}

void encode_write_frame(AVFrame *inputFrame, unsigned int stream_index){
    int ret;
    AVPacket *pkt;


    pkt = av_packet_alloc();
    if (!pkt)
        return;
    LOGE("START FRAME ENCODE pts = %3" PRId64" ，seconds = %d",inputFrame->pts, inputFrame->pts * av_q2d(ifmt_ctx->streams[stream_index]->time_base));
    ret = avcodec_send_frame(stream_ctx[stream_index].enc_ctx, inputFrame);
    while (ret >= 0) {
        ret = avcodec_receive_packet(stream_ctx[stream_index].enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            LOGE("FAIL during encoding %s",av_err2str(ret));
            return;
        } else if (ret < 0) {
            LOGE("Error during encoding %s",av_err2str(ret));
        }

        (*pkt).stream_index = stream_index;
        av_packet_rescale_ts(pkt,
                             ifmt_ctx->streams[stream_index]->time_base,
                             ofmt_ctx->streams[stream_index]->time_base);
        LOGE("START WRITE PACKET,  pts = %3" PRId64" ，pkt.pts seconds = %f", pkt->pts, pkt->pts * av_q2d(ofmt_ctx->streams[stream_index]->time_base));
        ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        if(ret < 0){
            LOGE("write pkt fail\n");
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
}

void log_ffmpeg(void *ptr, int level, const char *fmt, va_list vl)
{
    va_list vl2;
    char *line = static_cast<char *>(malloc(1280 * sizeof(char)));
    static int print_prefix = 1;
    va_copy(vl2, vl);
    av_log_format_line(ptr, level, fmt, vl2, line, 1280, &print_prefix);
    va_end(vl2);
    line[1279] = '\0';
    LOG_RESOURCE("%s", line);
    free(line);
}