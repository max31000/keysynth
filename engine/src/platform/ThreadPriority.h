#pragma once
// OS-specific audio-thread tuning (ARCHITECTURE §6). Only this directory may call Win32.

#include <string>

namespace ks::platform {

struct AudioThreadInfo {
    bool mmcss = false;           // registered with MMCSS "Pro Audio"
    bool powerThrottlingOff = false;
    std::string error;
    unsigned long errorCode = 0; // GetLastError() of the MMCSS call
};

// Call once from the audio thread (first callback). Not RT-safe in the strict sense (syscalls), but bounded and
// one-shot. No-op on non-Windows platforms.
AudioThreadInfo promoteCurrentThreadForAudio() noexcept;

// Undo MMCSS registration for the current thread (call from the same thread when the device stops; optional).
void demoteCurrentThread() noexcept;

} // namespace ks::platform
