// Compiled into every test executable on MSVC (see the top-level CMakeLists).
//
// A Debug build's failed assert, abort() or crash opens a dialog and waits for
// someone to click it, which hangs an unattended test run and interrupts
// whoever is at the machine. This sends the message to stderr instead and lets
// the process exit with a failing status. It runs before main, as a static
// initializer; a test that fails earlier than that still gets the dialog.

#ifdef _MSC_VER

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <crtdbg.h>
#include <cstdlib>
#include <initializer_list>

namespace {

struct NoDialogs {
    NoDialogs() {
        // assert() in a Release CRT, and the "abort() has been called" box.
        _set_error_mode(_OUT_TO_STDERR);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        // assert() and the library's own checks in a Debug CRT.
        for (const int kind : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
            _CrtSetReportMode(kind, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
        }
        // The system's own box for a crash or a missing DLL.
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    }
};

const NoDialogs no_dialogs;

} // namespace

#endif
