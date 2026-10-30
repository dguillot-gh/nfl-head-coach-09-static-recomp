#pragma once

// Host controller input for XAM. On Windows this is XInput itself. Elsewhere the same structures,
// constants and calls are provided here (host_input_posix.cpp), fed by the host app through
// SetHostPad (the Android on-screen pad and Android gamepads), so xam_input.cpp is shared.

#ifdef _WIN32
#include <windows.h>
#include <Xinput.h>
#else
#include <cstdint>

using BYTE = uint8_t;
using WORD = uint16_t;
using SHORT = int16_t;
using DWORD = uint32_t;
using WCHAR = char16_t;

constexpr DWORD ERROR_SUCCESS = 0;
constexpr DWORD ERROR_DEVICE_NOT_CONNECTED = 1167;
constexpr DWORD ERROR_EMPTY = 4306;

constexpr DWORD XUSER_MAX_COUNT = 4;
constexpr DWORD XUSER_INDEX_ANY = 0xFF;

constexpr WORD XINPUT_GAMEPAD_DPAD_UP = 0x0001;
constexpr WORD XINPUT_GAMEPAD_DPAD_DOWN = 0x0002;
constexpr WORD XINPUT_GAMEPAD_DPAD_LEFT = 0x0004;
constexpr WORD XINPUT_GAMEPAD_DPAD_RIGHT = 0x0008;
constexpr WORD XINPUT_GAMEPAD_START = 0x0010;
constexpr WORD XINPUT_GAMEPAD_BACK = 0x0020;
constexpr WORD XINPUT_GAMEPAD_LEFT_THUMB = 0x0040;
constexpr WORD XINPUT_GAMEPAD_RIGHT_THUMB = 0x0080;
constexpr WORD XINPUT_GAMEPAD_LEFT_SHOULDER = 0x0100;
constexpr WORD XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200;
constexpr WORD XINPUT_GAMEPAD_A = 0x1000;
constexpr WORD XINPUT_GAMEPAD_B = 0x2000;
constexpr WORD XINPUT_GAMEPAD_X = 0x4000;
constexpr WORD XINPUT_GAMEPAD_Y = 0x8000;

constexpr BYTE XINPUT_DEVTYPE_GAMEPAD = 0x01;
constexpr BYTE XINPUT_DEVSUBTYPE_GAMEPAD = 0x01;

constexpr WORD XINPUT_KEYSTROKE_KEYDOWN = 0x0001;
constexpr WORD XINPUT_KEYSTROKE_KEYUP = 0x0002;
constexpr WORD XINPUT_KEYSTROKE_REPEAT = 0x0004;

constexpr WORD VK_PAD_A = 0x5800;
constexpr WORD VK_PAD_B = 0x5801;
constexpr WORD VK_PAD_X = 0x5802;
constexpr WORD VK_PAD_Y = 0x5803;
constexpr WORD VK_PAD_RSHOULDER = 0x5804;
constexpr WORD VK_PAD_LSHOULDER = 0x5805;
constexpr WORD VK_PAD_LTRIGGER = 0x5806;
constexpr WORD VK_PAD_RTRIGGER = 0x5807;
constexpr WORD VK_PAD_DPAD_UP = 0x5810;
constexpr WORD VK_PAD_DPAD_DOWN = 0x5811;
constexpr WORD VK_PAD_DPAD_LEFT = 0x5812;
constexpr WORD VK_PAD_DPAD_RIGHT = 0x5813;
constexpr WORD VK_PAD_START = 0x5814;
constexpr WORD VK_PAD_BACK = 0x5815;
constexpr WORD VK_PAD_LTHUMB_PRESS = 0x5816;
constexpr WORD VK_PAD_RTHUMB_PRESS = 0x5817;

// Keyboard keys used by the Windows virtual pad; there is no keyboard path here (KeyDown is false).
constexpr int VK_RETURN = 0x0D;
constexpr int VK_ESCAPE = 0x1B;
constexpr int VK_SPACE = 0x20;
constexpr int VK_LEFT = 0x25;
constexpr int VK_UP = 0x26;
constexpr int VK_RIGHT = 0x27;
constexpr int VK_DOWN = 0x28;

struct XINPUT_GAMEPAD
{
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};

struct XINPUT_STATE
{
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
};

struct XINPUT_VIBRATION
{
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
};

struct XINPUT_CAPABILITIES
{
    BYTE Type;
    BYTE SubType;
    WORD Flags;
    XINPUT_GAMEPAD Gamepad;
    XINPUT_VIBRATION Vibration;
};

struct XINPUT_KEYSTROKE
{
    WORD VirtualKey;
    WCHAR Unicode;
    WORD Flags;
    BYTE UserIndex;
    BYTE HidCode;
};

DWORD XInputGetState(DWORD userIndex, XINPUT_STATE* state);
DWORD XInputSetState(DWORD userIndex, XINPUT_VIBRATION* vibration);
DWORD XInputGetCapabilities(DWORD userIndex, DWORD flags, XINPUT_CAPABILITIES* capabilities);
DWORD XInputGetKeystroke(DWORD userIndex, DWORD reserved, XINPUT_KEYSTROKE* keystroke);

namespace hc09::host
{
    // Player 1's controller as the host app sees it (on-screen pad merged with any real gamepad).
    void SetHostPad(bool connected, const XINPUT_GAMEPAD& pad);
    // Last rumble the game asked for (0-65535 per motor), for the host app to play.
    XINPUT_VIBRATION HostRumble();
}
#endif
