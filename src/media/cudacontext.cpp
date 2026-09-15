/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/cudacontext.h"

#include "media/avwrappers.h"

extern "C" {
#include <libavutil/hwcontext.h>
}

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

QString avErrorString(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] {};
    av_strerror(code, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

CudaContext &CudaContext::instance()
{
    static CudaContext context;
    return context;
}

CudaContext::~CudaContext()
{
    shutdown();
}

bool CudaContext::ensureInitialized(int deviceIndex)
{
    if (m_device && deviceIndex == m_deviceIndex) {
        return true;
    }
    if (m_attempted && deviceIndex == m_deviceIndex) {
        return false;
    }

    shutdown();
    m_attempted = true;
    m_deviceIndex = deviceIndex;

    const QByteArray device = QByteArray::number(deviceIndex);
    const int ret = av_hwdevice_ctx_create(&m_device, AV_HWDEVICE_TYPE_CUDA,
                                           device.constData(), nullptr, 0);
    if (ret < 0) {
        m_device = nullptr;
        m_lastError = u"Could not create a CUDA device context: %1"_s.arg(avErrorString(ret));
        qWarning("%s", qUtf8Printable(m_lastError));
        return false;
    }

    qInfo("CUDA device %d ready for hardware decoding", deviceIndex);
    return true;
}

void CudaContext::shutdown()
{
    if (m_device) {
        av_buffer_unref(&m_device);
        m_device = nullptr;
    }
    m_attempted = false;
}

} // namespace CBridge
