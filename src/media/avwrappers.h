#pragma once

#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavutil/frame.h>
}

#include <memory>

namespace CBridge {

struct AVFrameDeleter {
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};

struct AVPacketDeleter {
    void operator()(AVPacket *packet) const { av_packet_free(&packet); }
};

struct AVCodecContextDeleter {
    void operator()(AVCodecContext *context) const { avcodec_free_context(&context); }
};

struct AVFilterGraphDeleter {
    void operator()(AVFilterGraph *graph) const { avfilter_graph_free(&graph); }
};

using FramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
using FilterGraphPtr = std::unique_ptr<AVFilterGraph, AVFilterGraphDeleter>;

inline FramePtr makeFrame() { return FramePtr(av_frame_alloc()); }
inline PacketPtr makePacket() { return PacketPtr(av_packet_alloc()); }

QString avErrorString(int code);

} // namespace CBridge
