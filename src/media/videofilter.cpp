/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/videofilter.h"

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <QStringList>

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {

QString describeFormat(AVPixelFormat format)
{
    const char *name = av_get_pix_fmt_name(format);
    return name ? QString::fromUtf8(name) : u"unknown"_s;
}

} // namespace

VideoFilter::VideoFilter() = default;

VideoFilter::~VideoFilter()
{
    close();
}

void VideoFilter::close()
{
    m_output.reset();
    m_graph.reset();
    m_source = nullptr;
    m_sink = nullptr;
    m_outputWidth = 0;
    m_outputHeight = 0;
}

bool VideoFilter::open(const AVFrame *templateFrame, AVRational timeBase,
                       int targetWidth, int targetHeight,
                       int fpsNum, int fpsDen,
                       AVPixelFormat outputFormat,
                       QString *error)
{
    close();

    if (!templateFrame || templateFrame->width <= 0 || templateFrame->height <= 0) {
        if (error) {
            *error = u"Cannot build a filter graph without a sample frame"_s;
        }
        return false;
    }

    const auto inputFormat = AVPixelFormat(templateFrame->format);
    const bool hardware = inputFormat == AV_PIX_FMT_CUDA;

    m_outputWidth = targetWidth > 0 ? targetWidth : templateFrame->width;
    m_outputHeight = targetHeight > 0 ? targetHeight : templateFrame->height;

    m_graph.reset(avfilter_graph_alloc());
    if (!m_graph) {
        if (error) {
            *error = u"Out of memory allocating the filter graph"_s;
        }
        return false;
    }

    AVBufferSrcParameters *parameters = av_buffersrc_parameters_alloc();
    if (!parameters) {
        if (error) {
            *error = u"Out of memory allocating buffersrc parameters"_s;
        }
        close();
        return false;
    }

    // Allocated uninitialised: buffersrc rejects a hardware pix_fmt until
    // hw_frames_ctx is attached, which only av_buffersrc_parameters_set can do.
    m_source = avfilter_graph_alloc_filter(m_graph.get(), avfilter_get_by_name("buffer"), "in");
    if (!m_source) {
        av_free(parameters);
        if (error) {
            *error = u"Could not allocate the filter source"_s;
        }
        close();
        return false;
    }

    parameters->format = inputFormat;
    parameters->width = templateFrame->width;
    parameters->height = templateFrame->height;
    parameters->time_base = timeBase;
    parameters->sample_aspect_ratio = AVRational { 1, 1 };
    parameters->hw_frames_ctx = templateFrame->hw_frames_ctx;
    int ret = av_buffersrc_parameters_set(m_source, parameters);
    av_free(parameters);
    if (ret < 0) {
        if (error) {
            *error = u"Could not set buffersrc parameters: %1"_s.arg(avErrorString(ret));
        }
        close();
        return false;
    }

    ret = avfilter_init_str(m_source, nullptr);
    if (ret < 0) {
        if (error) {
            *error = u"Could not initialise the filter source: %1"_s.arg(avErrorString(ret));
        }
        close();
        return false;
    }

    // Allocated uninitialised as well: buffersink only accepts its format list
    // before init, and FFmpeg 7+ renamed the option to "pixel_formats".
    m_sink = avfilter_graph_alloc_filter(m_graph.get(), avfilter_get_by_name("buffersink"), "out");
    if (!m_sink) {
        if (error) {
            *error = u"Could not allocate the filter sink"_s;
        }
        close();
        return false;
    }

    const AVPixelFormat sinkFormats[] = { outputFormat, AV_PIX_FMT_NONE };
    ret = av_opt_set(m_sink, "pixel_formats", av_get_pix_fmt_name(outputFormat),
                     AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        ret = av_opt_set_int_list(m_sink, "pix_fmts", sinkFormats, AV_PIX_FMT_NONE,
                                  AV_OPT_SEARCH_CHILDREN);
    }
    if (ret < 0) {
        if (error) {
            *error = u"Could not constrain the sink format: %1"_s.arg(avErrorString(ret));
        }
        close();
        return false;
    }

    ret = avfilter_init_str(m_sink, nullptr);
    if (ret < 0) {
        if (error) {
            *error = u"Could not initialise the filter sink: %1"_s.arg(avErrorString(ret));
        }
        close();
        return false;
    }

    QStringList chain;
    if (hardware) {
        chain << u"scale_cuda=w=%1:h=%2:format=nv12"_s.arg(m_outputWidth).arg(m_outputHeight);
        chain << u"hwdownload"_s;
        chain << u"format=nv12"_s;
    } else if (targetWidth > 0 || targetHeight > 0) {
        chain << u"scale=w=%1:h=%2"_s.arg(m_outputWidth).arg(m_outputHeight);
    }
    if (fpsNum > 0) {
        chain << u"fps=%1/%2"_s.arg(fpsNum).arg(fpsDen > 0 ? fpsDen : 1);
    }
    chain << u"format=%1"_s.arg(describeFormat(outputFormat));

    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    if (!outputs || !inputs) {
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        if (error) {
            *error = u"Out of memory allocating filter endpoints"_s;
        }
        close();
        return false;
    }

    outputs->name = av_strdup("in");
    outputs->filter_ctx = m_source;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = m_sink;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    const QByteArray description = chain.join(u","_s).toUtf8();
    ret = avfilter_graph_parse_ptr(m_graph.get(), description.constData(),
                                   &inputs, &outputs, nullptr);
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);

    if (ret < 0) {
        if (error) {
            *error = u"Could not parse the filter chain \"%1\": %2"_s
                         .arg(QString::fromUtf8(description), avErrorString(ret));
        }
        close();
        return false;
    }

    ret = avfilter_graph_config(m_graph.get(), nullptr);
    if (ret < 0) {
        if (error) {
            *error = u"Could not configure the filter graph: %1"_s.arg(avErrorString(ret));
        }
        close();
        return false;
    }

    m_output = makeFrame();
    if (!m_output) {
        if (error) {
            *error = u"Out of memory allocating the filter output frame"_s;
        }
        close();
        return false;
    }

    m_outputWidth = av_buffersink_get_w(m_sink);
    m_outputHeight = av_buffersink_get_h(m_sink);

    qInfo("Filter graph: %s -> %dx%d %s", description.constData(),
          m_outputWidth, m_outputHeight, qUtf8Printable(describeFormat(outputFormat)));
    return true;
}

bool VideoFilter::push(AVFrame *frame, QString *error)
{
    if (!m_graph || !m_source || !m_sink) {
        return false;
    }

    int ret = av_buffersrc_add_frame_flags(m_source, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) {
        if (error) {
            *error = u"Filter rejected a frame: %1"_s.arg(avErrorString(ret));
        }
        return false;
    }

    while (true) {
        ret = av_buffersink_get_frame(m_sink, m_output.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }
        if (ret < 0) {
            if (error) {
                *error = u"Filter output failed: %1"_s.arg(avErrorString(ret));
            }
            return false;
        }

        if (m_onFrame) {
            m_onFrame(m_output.get());
        }
        av_frame_unref(m_output.get());
    }
}

} // namespace CBridge
