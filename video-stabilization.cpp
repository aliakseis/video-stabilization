// based on https://ffmpeg.org/doxygen/trunk/remuxing_8c-example.html

#include <stdint.h>

extern "C"
{
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <opencv2/imgproc/imgproc.hpp>

#include "makeguard.h"

#include <vector>

#include <memory>

#include <opencv2/opencv.hpp>
#include <iostream>
#include <cassert>
#include <cmath>
#include <fstream>

#include <functional>


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


    int m_videoStreamNumber = -1;
    AVStream* m_videoStream = nullptr;
    AVStream* outputVideoStream = nullptr;

    for (int i = 0; i < input_format_context->nb_streams; i++) {
        AVStream *out_stream;
        AVStream *in_stream = input_format_context->streams[i];
        AVCodecParameters *in_codecpar = in_stream->codecpar;

        const bool isVideoStream = in_codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        if (isVideoStream)
        {
            m_videoStreamNumber = i;
            m_videoStream = in_stream;
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
    auto m_videoCodecContext = avcodec_alloc_context3(nullptr);
    if (!m_videoCodecContext)
        return 1;

    auto videoCodecContextGuard = MakeGuard(&m_videoCodecContext, avcodec_free_context);

    if (avcodec_parameters_to_context(m_videoCodecContext, m_videoStream->codecpar) < 0)
        return 1;

    auto m_videoCodec = avcodec_find_decoder(m_videoCodecContext->codec_id);
    if (m_videoCodec == nullptr)
    {
        fprintf(stderr, "No such codec found");
        return 1;  // Codec not found
    }

    // Open codec
    if (avcodec_open2(m_videoCodecContext, m_videoCodec, nullptr) < 0)
    {
        fprintf(stderr, "Error on codec opening");
        return 1;  // Could not open codec
    }



    // output
/* in this example, we choose transcoding to same codec */
    auto encoder = avcodec_find_encoder(m_videoCodecContext->codec_id);
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
    enc_ctx->height = m_videoCodecContext->height;
    enc_ctx->width = m_videoCodecContext->width;
    enc_ctx->sample_aspect_ratio = m_videoCodecContext->sample_aspect_ratio;
    /* take first format from list of supported formats */
    if (encoder->pix_fmts)
        enc_ctx->pix_fmt = encoder->pix_fmts[0];
    else
        enc_ctx->pix_fmt = m_videoCodecContext->pix_fmt;
    /* video time_base can be set to whatever is handy and supported by encoder */
    //enc_ctx->time_base = av_inv_q(m_videoCodecContext->framerate);
    enc_ctx->time_base = m_videoStream->time_base;

    if (output_format_context->oformat->flags & AVFMT_GLOBALHEADER)
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    /* Third parameter can be used to pass settings to encoder */
    ret = avcodec_open2(enc_ctx, encoder, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open video encoder for stream #%u\n", m_videoStreamNumber);
        ReportError(ret);
        return 1;
    }
    ret = avcodec_parameters_from_context(outputVideoStream->codecpar, enc_ctx);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to copy encoder parameters to output stream #%u\n", m_videoStreamNumber);
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
    videoFrameOut->format = m_videoCodecContext->pix_fmt;
    videoFrameOut->width = m_videoCodecContext->width;
    videoFrameOut->height = m_videoCodecContext->height;
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

        if (packet.stream_index == m_videoStreamNumber)
        {
            const int ret = avcodec_send_packet(m_videoCodecContext, &packet);
            if (ret < 0)
                return false;

            while (avcodec_receive_frame(m_videoCodecContext, videoFrame.get()) == 0)
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
                    m_videoCodecContext->width,
                    m_videoCodecContext->height,
                    m_videoCodecContext->pix_fmt,
                    m_videoCodecContext->width,
                    m_videoCodecContext->height,
                    AV_PIX_FMT_BGR24,
                    SWS_FAST_BILINEAR, NULL, NULL, NULL);
                sws_scale(img_convert_ctx, videoFrame->data, videoFrame->linesize, 0, m_videoCodecContext->height, //pFrameRGB->data, pFrameRGB->linesize);
                    //(uint8_t*)
                    &img.data, //&videoFrame->width);
                    &stride);


                callback(img);


                auto reverse_convert_ctx = sws_getCachedContext(
                    NULL,
                    m_videoCodecContext->width,
                    m_videoCodecContext->height,
                    AV_PIX_FMT_BGR24,
                    m_videoCodecContext->width,
                    m_videoCodecContext->height,
                    m_videoCodecContext->pix_fmt,
                    SWS_FAST_BILINEAR, NULL, NULL, NULL);

                sws_scale(reverse_convert_ctx,
                    &img.data,
                    &stride,
                    //&videoFrame->width,
                    0, m_videoCodecContext->height, //pFrameRGB->data, pFrameRGB->linesize);
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
    if (m_videoCodec->capabilities & AV_CODEC_CAP_DELAY)
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


//int main(int argc, char **argv)
//{
//    if (argc < 3) {
//        printf("You need to pass at least two parameters.\n");
//        return -1;
//    }
//
//    const char *in_filename = argv[1];
//    const char *out_filename = argv[2];
//
//    return TransformVideo(in_filename, out_filename);
//}

/*
Thanks Nghia Ho for his excellent code.
And,I modified the smooth step using a simple kalman filter .
So,It can processes live video streaming.
modified by chen jia.
email:chenjia2013@foxmail.com
*/


// This video stablisation smooths the global trajectory using a sliding average window

//const int SMOOTHING_RADIUS = 15; // In frames. The larger the more stable the video, but less reactive to sudden panning
const int HORIZONTAL_BORDER_CROP = 20; // In pixels. Crops the border to reduce the black borders from stabilisation being too noticeable.

// 1. Get previous to current frame transformation (dx, dy, da) for all frames
// 2. Accumulate the transformations to get the image trajectory
// 3. Smooth out the trajectory using an averaging window
// 4. Generate new set of previous to current transform, such that the trajectory ends up being the same as the smoothed trajectory
// 5. Apply the new transformation to the video

struct TransformParam
{
    TransformParam() = default;
    TransformParam(double _dx, double _dy, double _da) {
        dx = _dx;
        dy = _dy;
        da = _da;
    }

    double dx;
    double dy;
    double da; // angle
};

struct Trajectory
{
    Trajectory() = default;
    Trajectory(double _x, double _y, double _a) {
        x = _x;
        y = _y;
        a = _a;
    }
    // "+"
    friend Trajectory operator+(const Trajectory &c1, const Trajectory  &c2) {
        return Trajectory(c1.x + c2.x, c1.y + c2.y, c1.a + c2.a);
    }
    //"-"
    friend Trajectory operator-(const Trajectory &c1, const Trajectory  &c2) {
        return Trajectory(c1.x - c2.x, c1.y - c2.y, c1.a - c2.a);
    }
    //"*"
    friend Trajectory operator*(const Trajectory &c1, const Trajectory  &c2) {
        return Trajectory(c1.x*c2.x, c1.y*c2.y, c1.a*c2.a);
    }
    //"/"
    friend Trajectory operator/(const Trajectory &c1, const Trajectory  &c2) {
        return Trajectory(c1.x / c2.x, c1.y / c2.y, c1.a / c2.a);
    }
    //"="
    Trajectory operator =(const Trajectory &rx) {
        x = rx.x;
        y = rx.y;
        a = rx.a;
        return Trajectory(x, y, a);
    }

    double x;
    double y;
    double a; // angle
};

//////////////////////////////////////////////////////////////////////////////

using namespace std;
using namespace cv;

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("You need to pass at least two parameters.\n");
        return -1;
    }
    
    const char *in_filename = argv[1];
    const char *out_filename = argv[2];
    


    //if (argc < 2) {
    //    cout << "./VideoStab [video.avi]" << endl;
    //    return 0;
    //}
    // For further analysis
    ofstream out_transform("prev_to_cur_transformation.txt");
    ofstream out_trajectory("trajectory.txt");
    ofstream out_smoothed_trajectory("smoothed_trajectory.txt");
    ofstream out_new_transform("new_prev_to_cur_transformation.txt");

    //VideoCapture cap(argv[1]);
    //assert(cap.isOpened());

    //Mat cur;
    Mat cur_grey;
    Mat prev;
    Mat prev_grey;

    //cap >> prev;//get the first frame.ch
    //cvtColor(prev, prev_grey, COLOR_BGR2GRAY);

    // Step 1 - Get previous to current frame transformation (dx, dy, da) for all frames
    vector <TransformParam> prev_to_cur_transform; // previous to current
    // Accumulated frame to frame transform
    double a = 0;
    double x = 0;
    double y = 0;
    // Step 2 - Accumulate the transformations to get the image trajectory
    vector <Trajectory> trajectory; // trajectory at all frames
    //
    // Step 3 - Smooth out the trajectory using an averaging window
    vector <Trajectory> smoothed_trajectory; // trajectory at all frames
    Trajectory X;//posteriori state estimate
    Trajectory	X_;//priori estimate
    Trajectory P;// posteriori estimate error covariance
    Trajectory P_;// priori estimate error covariance
    Trajectory K;//gain
    Trajectory	z;//actual measurement
    double pstd = 4e-3;//can be changed
    double cstd = 0.25;//can be changed
    Trajectory Q(pstd, pstd, pstd);// process noise covariance
    Trajectory R(cstd, cstd, cstd);// measurement noise covariance 
    // Step 4 - Generate new set of previous to current transform, such that the trajectory ends up being the same as the smoothed trajectory
    vector <TransformParam> new_prev_to_cur_transform;
    //
    // Step 5 - Apply the new transformation to the video
    //cap.set(CV_CAP_PROP_POS_FRAMES, 0);
    Mat T(2, 3, CV_64F);

    //int vert_border = HORIZONTAL_BORDER_CROP * prev.rows / prev.cols; // get the aspect ratio correct
    //VideoWriter outputVideo;
    //outputVideo.open("compare.avi", CV_FOURCC('X', 'V', 'I', 'D'), 24, cvSize(cur.rows, cur.cols * 2 + 10), true);
    //
    int k = 1;
    //int max_frames = cap.get(CV_CAP_PROP_FRAME_COUNT);
    Mat last_T;
    Mat prev_grey_;
    Mat cur_grey_;

    bool first = true;

    auto lam = [&](Mat& cur) {

        if (first) 
        {
            first = false;
            prev = cur;
            cvtColor(prev, prev_grey, COLOR_BGR2GRAY);
            return;
        }


        const int vert_border = HORIZONTAL_BORDER_CROP * cur.rows / cur.cols; // get the aspect ratio correct

        cvtColor(cur, cur_grey, COLOR_BGR2GRAY);

        // vector from prev to cur
        vector <Point2f> prev_corner;
        vector <Point2f> cur_corner;
        vector <Point2f> prev_corner2;
        vector <Point2f> cur_corner2;
        vector <uchar> status;
        vector <float> err;

        goodFeaturesToTrack(prev_grey, prev_corner, 200, 0.01, 30);
        calcOpticalFlowPyrLK(prev_grey, cur_grey, prev_corner, cur_corner, status, err);

        // weed out bad matches
        for (size_t i = 0; i < status.size(); i++) {
            if (status[i] != 0u) {
                prev_corner2.push_back(prev_corner[i]);
                cur_corner2.push_back(cur_corner[i]);
            }
        }

        // translation + rotation only
        Mat T = estimateRigidTransform(prev_corner2, cur_corner2, false); // false = rigid transform, no scaling/shearing

        // in rare cases no transform is found. We'll just use the last known good transform.
        if (T.data == nullptr) {
            last_T.copyTo(T);
        }

        T.copyTo(last_T);

        // decompose T
        double dx = T.at<double>(0, 2);
        double dy = T.at<double>(1, 2);
        double da = atan2(T.at<double>(1, 0), T.at<double>(0, 0));
        //
        //prev_to_cur_transform.push_back(TransformParam(dx, dy, da));

        out_transform << k << " " << dx << " " << dy << " " << da << endl;
        //
        // Accumulated frame to frame transform
        x += dx;
        y += dy;
        a += da;
        //trajectory.push_back(Trajectory(x,y,a));
        //
        out_trajectory << k << " " << x << " " << y << " " << a << endl;
        //
        z = Trajectory(x, y, a);
        //
        if (k == 1) {
            // intial guesses
            X = Trajectory(0, 0, 0); //Initial estimate,  set 0
            P = Trajectory(1, 1, 1); //set error variance,set 1
        }
        else
        {
            //time update（prediction）
            X_ = X; //X_(k) = X(k-1);
            P_ = P + Q; //P_(k) = P(k-1)+Q;
            // measurement update（correction）
            K = P_ / (P_ + R); //gain;K(k) = P_(k)/( P_(k)+R );
            X = X_ + K * (z - X_); //z-X_ is residual,X(k) = X_(k)+K(k)*(z(k)-X_(k)); 
            P = (Trajectory(1, 1, 1) - K)*P_; //P(k) = (1-K(k))*P_(k);
        }
        //smoothed_trajectory.push_back(X);
        out_smoothed_trajectory << k << " " << X.x << " " << X.y << " " << X.a << endl;
        //-
        // target - current
        double diff_x = X.x - x;//
        double diff_y = X.y - y;
        double diff_a = X.a - a;

        dx = dx + diff_x;
        dy = dy + diff_y;
        da = da + diff_a;

        //new_prev_to_cur_transform.push_back(TransformParam(dx, dy, da));
        //
        out_new_transform << k << " " << dx << " " << dy << " " << da << endl;
        //
        T.at<double>(0, 0) = cos(da);
        T.at<double>(0, 1) = -sin(da);
        T.at<double>(1, 0) = sin(da);
        T.at<double>(1, 1) = cos(da);

        T.at<double>(0, 2) = dx;
        T.at<double>(1, 2) = dy;

        Mat cur2;

        warpAffine(prev, cur2, T, cur.size());

        cur2 = cur2(Range(vert_border, cur2.rows - vert_border), Range(HORIZONTAL_BORDER_CROP, cur2.cols - HORIZONTAL_BORDER_CROP));

        // Resize cur2 back to cur size, for better side by side comparison
        resize(cur2, cur2, cur.size());

        // Now draw the original and stablised side by side for coolness
        //Mat canvas = Mat::zeros(cur.rows, cur.cols * 2 + 10, cur.type());

        //prev.copyTo(canvas(Range::all(), Range(0, cur2.cols)));
        //cur2.copyTo(canvas(Range::all(), Range(cur2.cols + 10, cur2.cols * 2 + 10)));

        //// If too big to fit on the screen, then scale it down by 2, hopefully it'll fit :)
        //if (canvas.cols > 1920) {
        //    resize(canvas, canvas, Size(canvas.cols / 2, canvas.rows / 2));
        //}
        //outputVideo<<canvas;
        //imshow("before and after", canvas);

        //waitKey(10);
        //

        prev = cur.clone();//cur.copyTo(prev);
        cur_grey.copyTo(prev_grey);

        cur = cur2;

        //cout << "Frame: " << k << "/" << max_frames << " - good optical flow: " << prev_corner2.size() << endl;
        k++;

    };


    return TransformVideo(in_filename, out_filename, lam);
}
