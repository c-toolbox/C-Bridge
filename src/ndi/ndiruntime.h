#pragma once

#include <QString>

// NDIlib_v5 is a typedef of a versioned struct, so it cannot be forward-declared.
#ifdef CBRIDGE_NDI_SUPPORT
#include <Processing.NDI.Lib.h>
#endif

namespace CBridge {

/// Loads Processing.NDI.Lib.x64.dll at runtime.
///
/// Deliberately not linked at build time: a machine without the NDI runtime should
/// still start and report the problem in the UI, with the multicast sinks unaffected.
class NdiRuntime
{
public:
    static NdiRuntime &instance();

    /// Loads and initialises the library on first call. Safe to call repeatedly.
    bool ensureLoaded();

    bool isLoaded() const { return m_lib != nullptr; }
#ifdef CBRIDGE_NDI_SUPPORT
    const NDIlib_v5 *lib() const { return m_lib; }
#endif

    QString version() const;
    QString lastError() const { return m_lastError; }

private:
    NdiRuntime() = default;
    ~NdiRuntime();

    NdiRuntime(const NdiRuntime &) = delete;
    NdiRuntime &operator=(const NdiRuntime &) = delete;

    QString locateLibrary() const;

#ifdef CBRIDGE_NDI_SUPPORT
    const NDIlib_v5 *m_lib = nullptr;
#else
    const void *m_lib = nullptr;
#endif
    void *m_module = nullptr;
    bool m_attempted = false;
    QString m_lastError;
};

} // namespace CBridge
