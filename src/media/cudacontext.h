/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

struct AVBufferRef;

namespace CBridge {

/// Process-wide CUDA device shared by every decoder and filter graph.
///
/// One device context per process rather than per stream: at 24 concurrent streams
/// separate contexts would multiply GPU memory and context-switch overhead.
class CudaContext
{
public:
    static CudaContext &instance();

    /// Creates the device on first use. Safe to call repeatedly.
    bool ensureInitialized(int deviceIndex = 0);

    bool isAvailable() const { return m_device != nullptr; }

    /// Borrowed reference; callers must av_buffer_ref() to keep it.
    AVBufferRef *deviceContext() const { return m_device; }

    QString lastError() const { return m_lastError; }

    void shutdown();

private:
    CudaContext() = default;
    ~CudaContext();

    CudaContext(const CudaContext &) = delete;
    CudaContext &operator=(const CudaContext &) = delete;

    AVBufferRef *m_device = nullptr;
    bool m_attempted = false;
    int m_deviceIndex = 0;
    QString m_lastError;
};

} // namespace CBridge
