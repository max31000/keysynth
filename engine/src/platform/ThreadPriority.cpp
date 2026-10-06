#include "platform/ThreadPriority.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <avrt.h>
#include <processthreadsapi.h>
#endif

namespace ks::platform {

#if defined(_WIN32)
namespace {
thread_local HANDLE t_mmcssHandle = nullptr;
}

AudioThreadInfo promoteCurrentThreadForAudio() noexcept {
    AudioThreadInfo info;
    if (t_mmcssHandle == nullptr) {
        DWORD taskIndex = 0;
        t_mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (t_mmcssHandle != nullptr) {
            AvSetMmThreadPriority(t_mmcssHandle, AVRT_PRIORITY_HIGH);
            info.mmcss = true;
        } else {
            info.errorCode = GetLastError();
            if (info.errorCode == ERROR_THREAD_ALREADY_IN_TASK) {
                info.mmcss = true; // the driver (typical for ASIO) already registered this thread with MMCSS
                info.error = "thread already in an MMCSS task (registered by the driver)";
            } else {
                info.error = "AvSetMmThreadCharacteristicsW failed";
            }
        }
    } else {
        info.mmcss = true;
    }
    // Opt the thread out of EcoQoS / power throttling (keeps it on P-cores at full clock on hybrid CPUs).
    THREAD_POWER_THROTTLING_STATE state{};
    state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0; // 0 = throttling disabled
    info.powerThrottlingOff = SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state)) != 0;
    return info;
}

void demoteCurrentThread() noexcept {
    if (t_mmcssHandle != nullptr) {
        AvRevertMmThreadCharacteristics(t_mmcssHandle);
        t_mmcssHandle = nullptr;
    }
}
#else
AudioThreadInfo promoteCurrentThreadForAudio() noexcept { return {}; }
void demoteCurrentThread() noexcept {}
#endif

} // namespace ks::platform
