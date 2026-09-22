#pragma once
#include <windows.h>

constexpr wchar_t X1_FAN_MAPPING_NAME[] = L"Global\\X1FanTelemetryV1";
constexpr DWORD X1_FAN_MAGIC = 0x31463158; // "X1F1"
constexpr DWORD X1_FAN_VERSION = 2;

enum X1FanStatus : DWORD {
    X1_FAN_STARTING = 0,
    X1_FAN_OK = 1,
    X1_FAN_DRIVER_MISSING = 2,
    X1_FAN_MODULE_MISSING = 3,
    X1_FAN_EC_UNAVAILABLE = 4,
    X1_FAN_UNSUPPORTED_MODEL = 5
};
enum X1FanMode : DWORD {
    X1_FAN_MODE_BIOS_AUTO = 0,
    X1_FAN_MODE_COOL = 1,
    X1_FAN_MODE_AGGRESSIVE = 2
};

struct X1FanTelemetry {
    DWORD magic;
    DWORD version;
    volatile LONG sequence;
    DWORD status;
    DWORD fan1Rpm;
    DWORD fan2Rpm;
    ULONGLONG updatedTick;
    DWORD lastError;
    volatile LONG requestedMode;
    DWORD activeMode;
    LONG hottestTempC;
};
