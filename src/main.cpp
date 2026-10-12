// Entry point: single-instance hand-off, then the viewer.
#include "app/App.h"

#include <shellapi.h>

#include <exception>
#include <string>

using namespace fiv;

namespace {

std::wstring FirstArgument() {
    // Kernel parser, not __wargv: correct for Unicode paths and paths with spaces (see AGENTS.md).
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring path;
    if (argv && argc > 1) path = argv[1];
    if (argv) LocalFree(argv);
    return path;
}

// Hands the path to the running viewer and brings it forward. Returns false if
// no window could be found (e.g. it is still starting), in which case we run.
bool ForwardToRunningInstance(const std::wstring& path) {
    HWND other = FindWindowW(kWindowClass, nullptr);
    if (!other) return false;
    if (!path.empty()) {
        COPYDATASTRUCT cds{};
        cds.dwData = kCopyDataOpen;
        cds.cbData = DWORD((path.size() + 1) * sizeof(wchar_t));
        cds.lpData = const_cast<wchar_t*>(path.c_str());
        DWORD_PTR result = 0;
        SendMessageTimeoutW(other, WM_COPYDATA, 0, LPARAM(&cds), SMTO_ABORTIFHUNG, 3000, &result);
    }
    if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
    SetForegroundWindow(other);
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    const std::wstring path = FirstArgument();

    // Clear stale error state first, otherwise ERROR_ALREADY_EXISTS can be misread (see AGENTS.md).
    SetLastError(0);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\FastImageViewer.Instance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS && ForwardToRunningInstance(path)) {
        CloseHandle(mutex);
        return 0;
    }

    // No "insert disk" / critical-error popups from removable or network drives.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    int rc = 1;
    ComApartment com(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    {  // App (and every ComPtr it owns) is destroyed before CoUninitialize
        try {
            App app;
            rc = app.Run(inst, path);
        } catch (const std::exception& e) {
            MessageBoxA(nullptr, e.what(), "FastImageViewer: unexpected error", MB_OK | MB_ICONERROR);
        } catch (...) {
            MessageBoxA(nullptr, "Unknown error.", "FastImageViewer: unexpected error", MB_OK | MB_ICONERROR);
        }
    }
    if (mutex) CloseHandle(mutex);
    return rc;
}
