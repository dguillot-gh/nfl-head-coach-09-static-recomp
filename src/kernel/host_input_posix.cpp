// XInput calls for hosts without XInput: one controller (player 1) whose state the host app
// sets through SetHostPad. Real-controller keystrokes are not reported; xam_input.cpp makes
// keystrokes from button transitions itself.

#include "kernel/host_input.h"

#include <mutex>

namespace
{
    std::mutex g_mutex;
    bool g_connected = false;
    XINPUT_GAMEPAD g_pad{};
    DWORD g_packet = 0;
    XINPUT_VIBRATION g_rumble{};
}

namespace hc09::host
{
    void SetHostPad(bool connected, const XINPUT_GAMEPAD& pad)
    {
        std::lock_guard lock(g_mutex);
        const auto& a = g_pad;
        const bool changed = connected != g_connected || a.wButtons != pad.wButtons || a.bLeftTrigger != pad.bLeftTrigger ||
            a.bRightTrigger != pad.bRightTrigger || a.sThumbLX != pad.sThumbLX || a.sThumbLY != pad.sThumbLY ||
            a.sThumbRX != pad.sThumbRX || a.sThumbRY != pad.sThumbRY;
        g_connected = connected;
        g_pad = pad;
        if (changed)
            g_packet++;
    }

    XINPUT_VIBRATION HostRumble()
    {
        std::lock_guard lock(g_mutex);
        return g_rumble;
    }
}

DWORD XInputGetState(DWORD userIndex, XINPUT_STATE* state)
{
    std::lock_guard lock(g_mutex);
    if (userIndex != 0 || !g_connected)
        return ERROR_DEVICE_NOT_CONNECTED;
    state->dwPacketNumber = g_packet;
    state->Gamepad = g_pad;
    return ERROR_SUCCESS;
}

DWORD XInputSetState(DWORD userIndex, XINPUT_VIBRATION* vibration)
{
    std::lock_guard lock(g_mutex);
    if (userIndex != 0 || !g_connected)
        return ERROR_DEVICE_NOT_CONNECTED;
    g_rumble = *vibration;
    return ERROR_SUCCESS;
}

DWORD XInputGetCapabilities(DWORD userIndex, DWORD /*flags*/, XINPUT_CAPABILITIES* capabilities)
{
    std::lock_guard lock(g_mutex);
    if (userIndex != 0 || !g_connected)
        return ERROR_DEVICE_NOT_CONNECTED;
    *capabilities = {};
    capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
    capabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    capabilities->Gamepad.wButtons = 0xF3FF;
    capabilities->Gamepad.bLeftTrigger = capabilities->Gamepad.bRightTrigger = 0xFF;
    capabilities->Gamepad.sThumbLX = capabilities->Gamepad.sThumbLY = -64;
    capabilities->Gamepad.sThumbRX = capabilities->Gamepad.sThumbRY = -64;
    capabilities->Vibration = { 0xFFFF, 0xFFFF };
    return ERROR_SUCCESS;
}

DWORD XInputGetKeystroke(DWORD userIndex, DWORD /*reserved*/, XINPUT_KEYSTROKE* /*keystroke*/)
{
    std::lock_guard lock(g_mutex);
    if ((userIndex != 0 && userIndex != XUSER_INDEX_ANY) || !g_connected)
        return ERROR_DEVICE_NOT_CONNECTED;
    return ERROR_EMPTY;
}
