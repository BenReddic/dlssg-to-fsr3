#pragma once
// Experimental native NGX evaluation limiter for Windows DX12.
// Standalone header: add alongside NvNGXDirectX12.cpp; no CMake edit required.
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <mutex>

namespace NativeFramePacer {
namespace detail {
struct State {
    std::mutex mutex;
    LARGE_INTEGER frequency{};
    int64_t nextDeadline = 0;
    uint64_t remainder = 0;
    uint32_t targetMilli = 29973;
    bool enabled = true;
    DWORD lastConfigRead = 0;
    wchar_t iniPath[MAX_PATH]{};
    State() { QueryPerformanceFrequency(&frequency); }
};
inline void RefreshConfig(State& s) {
    if (!s.iniPath[0]) {
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&RefreshConfig), &module)) {
            DWORD n = GetModuleFileNameW(module, s.iniPath, MAX_PATH);
            if (n && n < MAX_PATH) {
                wchar_t* slash = wcsrchr(s.iniPath, L'\\');
                if (slash && (slash - s.iniPath) + 22 < MAX_PATH)
                    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - s.iniPath), L'NativeFramePacer.ini');
                else s.iniPath[0] = 0;
            }
        }
    }
    if (!s.iniPath[0]) return;
    // Read integer thousandths; no locale-dependent decimal parsing.
    int newTarget = GetPrivateProfileIntW(L'Pacer', L'FPSMilli', 29973, s.iniPath);
    int newEnabled = GetPrivateProfileIntW(L'Pacer', L'Enabled', 1, s.iniPath);
    if (newTarget < 1000 || newTarget > 240000) newTarget = 29973;
    if (s.targetMilli != static_cast<uint32_t>(newTarget) || s.enabled != (newEnabled != 0)) {
        s.nextDeadline = 0;
        s.remainder = 0;
    }
    s.targetMilli = static_cast<uint32_t>(newTarget);
    s.enabled = (newEnabled != 0);
}
} // namespace detail

inline void WaitBeforeEvaluation() {
    static detail::State state;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.frequency.QuadPart) return;
    DWORD nowMs = GetTickCount();
    if (!state.lastConfigRead || nowMs - state.lastConfigRead >= 1000) {
        state.lastConfigRead = nowMs;
        detail::RefreshConfig(state);
    }
    if (!state.enabled) { state.nextDeadline = 0; state.remainder = 0; return; }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    const int64_t now = qpc.QuadPart;
    const uint64_t numerator = static_cast<uint64_t>(state.frequency.QuadPart) * 1000ULL;
    const uint64_t denominator = state.targetMilli;
    const int64_t period = static_cast<int64_t>(numerator / denominator);
    if (!state.nextDeadline || now - state.nextDeadline > period * 3 ||
        state.nextDeadline - now > period * 2) {
        state.nextDeadline = now;
        state.remainder = 0;
    }
    while (true) {
        QueryPerformanceCounter(&qpc);
        int64_t remaining = state.nextDeadline - qpc.QuadPart;
        if (remaining <= 0) break;
        // Coarse sleep only when >2ms remain. Spin/yield for the tail.
        if (remaining > state.frequency.QuadPart / 500)
            Sleep(1);
        else if (remaining > state.frequency.QuadPart / 5000)
            Sleep(0);
        else YieldProcessor();
    }
    state.nextDeadline += period;
    state.remainder += numerator % denominator;
    if (state.remainder >= denominator) {
        ++state.nextDeadline;
        state.remainder -= denominator;
    }
}
} // namespace NativeFramePacer
