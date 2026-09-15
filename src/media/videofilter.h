/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "media/avwrappers.h"

#include <QString>

#include <functional>

namespace CBridge {

/// Converts decoded frames into a packed host format suitable for NDI.
///
/// For hardware frames the chain is scale_cuda -> hwdownload -> format, so the
/// colour conversion and any scaling happen on the GPU and only the final packed
/// frame crosses PCIe. Software frames take the same graph minus the GPU stages.
class VideoFilter
{
public:
    using FrameCallback = std::function<void(AVFrame *frame)>;

    VideoFilter();
    ~VideoFilter();

    VideoFilter(const VideoFilter &) = delete;
    VideoFilter &operator=(const VideoFilter &) = delete;

    /// targetWidth/Height of 0 keeps the source size. fpsNum of 0 keeps the source rate.
    bool open(const AVFrame *templateFrame, AVRational timeBase,
              int targetWidth, int targetHeight,
              int fpsNum, int fpsDen,
              AVPixelFormat outputFormat,
              QString *error);
    void close();
    bool isOpen() const { return m_graph != nullptr; }

    int outputWidth() const { return m_outputWidth; }
    int outputHeight() const { return m_outputHeight; }

    void setFrameCallback(FrameCallback callback) { m_onFrame = std::move(callback); }

    bool push(AVFrame *frame, QString *error);

private:
    FilterGraphPtr m_graph;
    AVFilterContext *m_source = nullptr;
    AVFilterContext *m_sink = nullptr;
    FramePtr m_output;

    FrameCallback m_onFrame;

    int m_outputWidth = 0;
    int m_outputHeight = 0;
};

} // namespace CBridge
