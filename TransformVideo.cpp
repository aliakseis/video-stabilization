#include "TransformVideo.h"

#include <stdint.h>

extern "C"
{
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <opencv2/opencv.hpp>

#include <vector>

#include "makeguard.h"

struct AVFrameDeleter
{
    void operator()(AVFrame *frame) const { av_frame_free(&frame); };
};

typedef std::unique_ptr<AVFrame, AVFrameDeleter> AVFramePtr;

void ReportError(int ret)
{
    char errBuf[AV_ERROR_MAX_STRING_SIZE]{};
    fprintf(stderr, "Error occurred: %s\n", av_make_error_string(errBuf, AV_ERROR_MAX_STRING_SIZE, ret));
}


int TransformVideo(const char *in_filename, const char *out_filename, std::function<void(cv::Mat&)>  callback)
{
    AVFormatContext *input_format_context = NULL, *output_format_context = NULL;

    int ret;
    int stream_index = 0;

    AVDictionary* opts = NULL;


    if ((ret = avformat_open_input(&input_format_context, in_filename, NULL, NULL)) < 0) {
        fprintf(stderr, "Could not open input file '%s'", in_filename);
        return 1;
    }

    auto input_format_context_guard = MakeGuard(&input_format_context, avformat_close_input);


    if ((ret = avformat_find_stream_info(input_format_context, NULL)) < 0) {
        fprintf(stderr, "Failed to retrieve input stream information");
        return 1;
    }

    avformat_alloc_output_context2(&output_format_context, NULL, NULL, out_filename);
    if (!output_format_context) {
        fprintf(stderr, "Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        return 1;
    }

    auto output_format_context_guard = MakeGuard(output_format_context, avformat_free_context);

    const auto number_of_streams = input_format_context->nb_streams;
    std::vector<int> streams_list(number_of_streams);


    int videoStreamNumber = -1;
    AVStream* videoStream = nullptr;
    AVStream* outputVideoStream = nullptr;

    for (int i = 0; i < input_format_context->nb_streams; i++) {
        AVStream *out_stream;
        AVStream *in_stream = input_format_context->streams[i];
        AVCodecParameters *in_codecpar = in_stream->codecpar;

        const bool isVideoStream = in_codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        if (isVideoStream)
        {
            videoStreamNumber = i;
            videoStream = in_stream;
        }
        else if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            streams_list[i] = -1;
            continue;
        }
        streams_list[i] = stream_index++;
        out_stream = avformat_new_stream(output_format_context, NULL);
        if (!out_stream) {
            fprintf(stderr, "Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            return 1;
        }
        ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy codec parameters\n");
            return 1;
        }

        if (isVideoStream)
        {
            outputVideoStream = out_stream;
        }
    }
    // https://ffmpeg.org/doxygen/trunk/group__lavf__misc.html#gae2645941f2dc779c307eb6314fd39f10
    av_dump_format(output_format_context, 0, out_filename, 1);

    // unless it's a no file (we'll talk later about that) write to the disk (FLAG_WRITE)
    // but basically it's a way to save the file to a buffer so you can store it
    // wherever you want.
    if (!(output_format_context->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&output_format_context->pb, out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Could not open output file '%s'", out_filename);
            return 1;
        }
    }


    // input video context
    auto videoCodecContext = avcodec_alloc_context3(nullptr);
    if (!videoCodecContext)
        return 1;

    auto videoCodecContextGuard = MakeGuard(&videoCodecContext, avcodec_free_context);

    if (avcodec_parameters_to_context(videoCodecContext, videoStream->codecpar) < 0)
        return 1;

    auto videoCodec = avcodec_find_decoder(videoCodecContext->codec_id);
    if (videoCodec == nullptr)
    {
        fprintf(stderr, "No such codec found");
        return 1;  // Codec not found
    }

    // Open codec
    if (avcodec_open2(videoCodecContext, videoCodec, nullptr) < 0)
    {
        fprintf(stderr, "Error on codec opening");
        return 1;  // Could not open codec
    }



    // output
/* in this example, we choose transcoding to same codec */
    auto encoder = avcodec_find_encoder(videoCodecContext->codec_id);
    if (!encoder) {
        av_log(NULL, AV_LOG_FATAL, "Necessary encoder not found\n");
        return 1;
    }
    auto enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) {
        av_log(NULL, AV_LOG_FATAL, "Failed to allocate the encoder context\n");
        return 1;
    }

    /* In this example, we transcode to same properties (picture size,
     * sample rate etc.). These properties can be changed for output
     * streams easily using filters */
     // if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
    enc_ctx->height = videoCodecContext->height;
    enc_ctx->width = videoCodecContext->width;
    enc_ctx->sample_aspect_ratio = videoCodecContext->sample_aspect_ratio;

    /* take first format from list of supported formats */
    enc_ctx->pix_fmt = (encoder->pix_fmts != nullptr)? encoder->pix_fmts[0] : videoCodecContext->pix_fmt;
    /* video time_base can be set to whatever is handy and supported by encoder */
    //enc_ctx->time_base = av_inv_q(m_videoCodecContext->framerate);
    enc_ctx->time_base = videoStream->time_base;

    if (output_format_context->oformat->flags & AVFMT_GLOBALHEADER)
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    /* Third parameter can be used to pass settings to encoder */
    ret = avcodec_open2(enc_ctx, encoder, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open video encoder for stream #%u\n", videoStreamNumber);
        ReportError(ret);
        return 1;
    }
    ret = avcodec_parameters_from_context(outputVideoStream->codecpar, enc_ctx);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to copy encoder parameters to output stream #%u\n", videoStreamNumber);
        return 1;
    }
    outputVideoStream->time_base = enc_ctx->time_base;


    //if (fragmented_mp4_options) {
    //    // https://developer.mozilla.org/en-US/docs/Web/API/Media_Source_Extensions_API/Transcoding_assets_for_MSE
    //    av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
    //}
    // https://ffmpeg.org/doxygen/trunk/group__lavf__encoding.html#ga18b7b10bb5b94c4842de18166bc677cb
    ret = avformat_write_header(output_format_context, &opts);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        return 1;
    }

    AVFramePtr videoFrame(av_frame_alloc());

    AVFramePtr videoFrameOut(av_frame_alloc());
    videoFrameOut->format = videoCodecContext->pix_fmt;
    videoFrameOut->width = videoCodecContext->width;
    videoFrameOut->height = videoCodecContext->height;
    av_frame_get_buffer(videoFrameOut.get(), 16);

    while (true) {
        AVPacket packet;

        ret = av_read_frame(input_format_context, &packet);
        if (ret < 0)
            break;
        const auto in_stream = input_format_context->streams[packet.stream_index];
        if (packet.stream_index >= number_of_streams || streams_list[packet.stream_index] < 0) {
            av_packet_unref(&packet);
            continue;
        }
        packet.stream_index = streams_list[packet.stream_index];
        const auto out_stream = output_format_context->streams[packet.stream_index];

        if (packet.stream_index == videoStreamNumber)
        {
            const int ret = avcodec_send_packet(videoCodecContext, &packet);
            if (ret < 0)
                return false;

            while (avcodec_receive_frame(videoCodecContext, videoFrame.get()) == 0)
            {
                // transformation

                AVPacket avEncodedPacket;

                av_init_packet(&avEncodedPacket);
                avEncodedPacket.data = NULL;
                avEncodedPacket.size = 0;


                cv::Mat img(videoFrame->height, videoFrame->width, CV_8UC3);// , pFrameRGB->data[0]); //dst->data[0]);

                int stride = img.step[0];


                auto img_convert_ctx = sws_getCachedContext(
                    NULL,
                    videoCodecContext->width,
                    videoCodecContext->height,
                    videoCodecContext->pix_fmt,
                    videoCodecContext->width,
                    videoCodecContext->height,
                    AV_PIX_FMT_BGR24,
                    SWS_FAST_BILINEAR, NULL, NULL, NULL);
                sws_scale(img_convert_ctx, videoFrame->data, videoFrame->linesize, 0, videoCodecContext->height, //pFrameRGB->data, pFrameRGB->linesize);
                    //(uint8_t*)
                    &img.data, //&videoFrame->width);
                    &stride);


                callback(img);


                auto reverse_convert_ctx = sws_getCachedContext(
                    NULL,
                    videoCodecContext->width,
                    videoCodecContext->height,
                    AV_PIX_FMT_BGR24,
                    videoCodecContext->width,
                    videoCodecContext->height,
                    videoCodecContext->pix_fmt,
                    SWS_FAST_BILINEAR, NULL, NULL, NULL);

                sws_scale(reverse_convert_ctx,
                    &img.data,
                    &stride,
                    //&videoFrame->width,
                    0, videoCodecContext->height, //pFrameRGB->data, pFrameRGB->linesize);
                    //(uint8_t*)
                    videoFrameOut->data, videoFrameOut->linesize
                );



                videoFrameOut->pts = videoFrame->pts;
                videoFrameOut->pkt_dts = videoFrame->pkt_dts;


                auto ret = avcodec_send_frame(enc_ctx, videoFrameOut.get());
                if (ret >= 0)
                {
                    while (!(ret = avcodec_receive_packet(enc_ctx, &avEncodedPacket)))
                        //if (!ret)
                    {
                        if (avEncodedPacket.pts != AV_NOPTS_VALUE)
                            avEncodedPacket.pts = av_rescale_q(avEncodedPacket.pts, enc_ctx->time_base, outputVideoStream->time_base);
                        if (avEncodedPacket.dts != AV_NOPTS_VALUE)
                            avEncodedPacket.dts = av_rescale_q(avEncodedPacket.dts, enc_ctx->time_base, outputVideoStream->time_base);

                        // outContainer is "mp4"
                        av_write_frame(output_format_context, &avEncodedPacket);

                        //av_free_packet(&encodedPacket);
                    }
                }


            }
        }
        else
        {
            /* copy packet */
            packet.pts = av_rescale_q_rnd(packet.pts, in_stream->time_base, out_stream->time_base, AVRounding(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            packet.dts = av_rescale_q_rnd(packet.dts, in_stream->time_base, out_stream->time_base, AVRounding(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            packet.duration = av_rescale_q(packet.duration, in_stream->time_base, out_stream->time_base);
            // https://ffmpeg.org/doxygen/trunk/structAVPacket.html#ab5793d8195cf4789dfb3913b7a693903
            packet.pos = -1;

            //https://ffmpeg.org/doxygen/trunk/group__lavf__encoding.html#ga37352ed2c63493c38219d935e71db6c1
            ret = av_interleaved_write_frame(output_format_context, &packet);
            if (ret < 0) {
                fprintf(stderr, "Error muxing packet\n");
                break;
            }
        }
        av_packet_unref(&packet);
    }

    // flush encoder
    if (videoCodec->capabilities & AV_CODEC_CAP_DELAY)
    {
        AVPacket avEncodedPacket;

        av_init_packet(&avEncodedPacket);
        avEncodedPacket.data = NULL;
        avEncodedPacket.size = 0;

        while ((ret = avcodec_send_frame(enc_ctx, nullptr)) >= 0)
        {
            //auto ret = avcodec_send_frame(enc_ctx, nullptr);
            //if (ret >= 0)
            {
                //ret = avcodec_receive_packet(enc_ctx, &avEncodedPacket);
                //if (!ret)
                while (!(ret = avcodec_receive_packet(enc_ctx, &avEncodedPacket)))
                {
                    if (avEncodedPacket.pts != AV_NOPTS_VALUE)
                        avEncodedPacket.pts = av_rescale_q(avEncodedPacket.pts, enc_ctx->time_base, outputVideoStream->time_base);
                    if (avEncodedPacket.dts != AV_NOPTS_VALUE)
                        avEncodedPacket.dts = av_rescale_q(avEncodedPacket.dts, enc_ctx->time_base, outputVideoStream->time_base);

                    // outContainer is "mp4"
                    av_write_frame(output_format_context, &avEncodedPacket);

                    //av_free_packet(&encodedPacket);
                }
            }

        }
    }

    //https://ffmpeg.org/doxygen/trunk/group__lavf__encoding.html#ga7f14007e7dc8f481f054b21614dfec13
    av_write_trailer(output_format_context);

    if (output_format_context && !(output_format_context->oformat->flags & AVFMT_NOFILE))
        avio_closep(&output_format_context->pb);

    return 0;
}
