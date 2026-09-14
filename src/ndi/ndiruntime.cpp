#include "ndi/ndiruntime.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>

#ifdef CBRIDGE_NDI_SUPPORT
#include <Processing.NDI.Lib.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

NdiRuntime &NdiRuntime::instance()
{
    static NdiRuntime runtime;
    return runtime;
}

NdiRuntime::~NdiRuntime()
{
#ifdef CBRIDGE_NDI_SUPPORT
    if (m_lib) {
        m_lib->destroy();
        m_lib = nullptr;
    }
#endif
#ifdef Q_OS_WIN
    if (m_module) {
        FreeLibrary(static_cast<HMODULE>(m_module));
        m_module = nullptr;
    }
#endif
}

QString NdiRuntime::locateLibrary() const
{
#ifdef CBRIDGE_NDI_SUPPORT
    const QString fileName = QString::fromLatin1(NDILIB_LIBRARY_NAME);
#else
    const QString fileName = u"Processing.NDI.Lib.x64.dll"_s;
#endif

    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();

    QStringList directories;
#ifdef CBRIDGE_NDI_SUPPORT
    directories << environment.value(QString::fromLatin1(NDILIB_REDIST_FOLDER));
#endif
    directories << environment.value(u"NDI_RUNTIME_DIR_V6"_s)
                << environment.value(u"NDI_RUNTIME_DIR_V5"_s);

    const QString sdkDir = environment.value(u"NDI_SDK_DIR"_s);
    if (!sdkDir.isEmpty()) {
        directories << sdkDir + u"/Bin/x64"_s << sdkDir + u"/Redist"_s;
    }

    for (const QString &directory : directories) {
        if (directory.isEmpty()) {
            continue;
        }
        const QString candidate = QDir(directory).filePath(fileName);
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }

    // Fall back to the normal search path, which covers a system-wide install.
    return fileName;
}

bool NdiRuntime::ensureLoaded()
{
#ifndef CBRIDGE_NDI_SUPPORT
    m_lastError = u"This build was compiled without NDI support"_s;
    return false;
#else
    if (m_lib) {
        return true;
    }
    if (m_attempted) {
        return false;
    }
    m_attempted = true;

    const QString path = locateLibrary();

#ifdef Q_OS_WIN
    const QByteArray localPath = QDir::toNativeSeparators(path).toLocal8Bit();
    HMODULE module = LoadLibraryA(localPath.constData());
    if (!module) {
        m_lastError = u"Could not load %1. Install the NDI runtime from "
                      "https://ndi.video/"_s.arg(path);
        qWarning("%s", qUtf8Printable(m_lastError));
        return false;
    }
    m_module = module;

    using LoadFn = const NDIlib_v5 *(*)();
    auto loader = reinterpret_cast<LoadFn>(
        reinterpret_cast<void *>(GetProcAddress(module, "NDIlib_v5_load")));
    if (!loader) {
        m_lastError = u"%1 does not export NDIlib_v5_load"_s.arg(path);
        qWarning("%s", qUtf8Printable(m_lastError));
        FreeLibrary(module);
        m_module = nullptr;
        return false;
    }

    const NDIlib_v5 *lib = loader();
#else
    const NDIlib_v5 *lib = nullptr;
    m_lastError = u"Dynamic NDI loading is only implemented for Windows"_s;
    return false;
#endif

    if (!lib) {
        m_lastError = u"NDIlib_v5_load returned no interface"_s;
        return false;
    }
    if (!lib->is_supported_CPU()) {
        m_lastError = u"This CPU does not meet the NDI requirements (SSE4.1)"_s;
        qWarning("%s", qUtf8Printable(m_lastError));
        return false;
    }
    if (!lib->initialize()) {
        m_lastError = u"NDIlib initialize() failed"_s;
        qWarning("%s", qUtf8Printable(m_lastError));
        return false;
    }

    m_lib = lib;
    qInfo("NDI runtime loaded: %s", qUtf8Printable(version()));
    return true;
#endif
}

QString NdiRuntime::version() const
{
#ifdef CBRIDGE_NDI_SUPPORT
    if (m_lib) {
        return QString::fromUtf8(m_lib->version());
    }
#endif
    return u"not loaded"_s;
}

} // namespace CBridge
