#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "setupapi.lib")

// {3a0ac59a-4d8a-4875-b7ea-304771ff9b9a}
static const GUID GUID_TOUCH_SELFTEST_INTERFACE =
{ 0x3a0ac59a, 0x4d8a, 0x4875, { 0xb7, 0xea, 0x30, 0x47, 0x71, 0xff, 0x9b, 0x9a } };

#ifndef FILE_DEVICE_KEYBOARD
#define FILE_DEVICE_KEYBOARD 0x0000000b
#endif

#define TOUCH_TEST_BUFFER_CTL_CODE(id) \
    CTL_CODE(FILE_DEVICE_KEYBOARD, (id), METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_TOUCH_SELFTEST_CALIBRATE TOUCH_TEST_BUFFER_CTL_CODE(104)
#define TOUCH_SELFTEST_CALIBRATION_FLAG_STORE 0x00000001

typedef LONG NTSTATUS;

typedef struct _TOUCH_SELFTEST_CALIBRATION
{
    ULONG Size;
    ULONG TimeoutMs;
    ULONG PollIntervalMs;
    ULONG Flags;
    ULONG PollCount;
    NTSTATUS OperationStatus;
    UCHAR FinalDeviceMode;
    UCHAR FinalState;
    UCHAR ErrorCode;
    UCHAR Reserved;
} TOUCH_SELFTEST_CALIBRATION;

static void PrintUsage()
{
    wprintf(L"Usage: CalibrationTool.exe [--timeout-ms N] [--poll-ms N] [--no-store]\n");
}

static BOOL TryParseUlong(_In_ const wchar_t* Text, _Out_ ULONG* Value)
{
    wchar_t* end = nullptr;
    unsigned long parsed = wcstoul(Text, &end, 10);
    if (end == Text || *end != L'\0')
    {
        return FALSE;
    }

    *Value = parsed;
    return TRUE;
}

static BOOL GetSelfTestDevicePath(_Out_writes_(PathCch) wchar_t* Path, _In_ DWORD PathCch)
{
    HDEVINFO devInfo;
    SP_DEVICE_INTERFACE_DATA interfaceData;
    DWORD requiredLength = 0;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detailData = nullptr;
    BOOL ok = FALSE;

    devInfo = SetupDiGetClassDevsW(
        &GUID_TOUCH_SELFTEST_INTERFACE,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }

    interfaceData.cbSize = sizeof(interfaceData);
    if (!SetupDiEnumDeviceInterfaces(
            devInfo,
            nullptr,
            &GUID_TOUCH_SELFTEST_INTERFACE,
            0,
            &interfaceData))
    {
        goto exit;
    }

    SetupDiGetDeviceInterfaceDetailW(
        devInfo,
        &interfaceData,
        nullptr,
        0,
        &requiredLength,
        nullptr);

    detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)LocalAlloc(LMEM_FIXED, requiredLength);
    if (detailData == nullptr)
    {
        goto exit;
    }

    detailData->cbSize = sizeof(*detailData);
    if (!SetupDiGetDeviceInterfaceDetailW(
            devInfo,
            &interfaceData,
            detailData,
            requiredLength,
            nullptr,
            nullptr))
    {
        goto exit;
    }

    ok = wcsncpy_s(Path, PathCch, detailData->DevicePath, _TRUNCATE) == 0;

exit:
    if (detailData != nullptr)
    {
        LocalFree(detailData);
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return ok;
}

int wmain(int argc, wchar_t** argv)
{
    TOUCH_SELFTEST_CALIBRATION calibration = {};
    wchar_t devicePath[1024] = {};
    HANDLE device;
    DWORD bytesReturned = 0;
    BOOL ok;

    calibration.Size = sizeof(calibration);
    calibration.TimeoutMs = 5000;
    calibration.PollIntervalMs = 100;
    calibration.Flags = TOUCH_SELFTEST_CALIBRATION_FLAG_STORE;

    for (int i = 1; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"--timeout-ms") == 0 && i + 1 < argc)
        {
            if (!TryParseUlong(argv[++i], &calibration.TimeoutMs))
            {
                PrintUsage();
                return 2;
            }
        }
        else if (wcscmp(argv[i], L"--poll-ms") == 0 && i + 1 < argc)
        {
            if (!TryParseUlong(argv[++i], &calibration.PollIntervalMs))
            {
                PrintUsage();
                return 2;
            }
        }
        else if (wcscmp(argv[i], L"--no-store") == 0)
        {
            calibration.Flags &= ~TOUCH_SELFTEST_CALIBRATION_FLAG_STORE;
        }
        else
        {
            PrintUsage();
            return 2;
        }
    }

    if (!GetSelfTestDevicePath(devicePath, ARRAYSIZE(devicePath)))
    {
        fwprintf(stderr, L"Touch self-test interface was not found. Win32 error=%lu\n", GetLastError());
        return 1;
    }

    wprintf(L"Opening %ls\n", devicePath);
    device = CreateFileW(
        devicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (device == INVALID_HANDLE_VALUE)
    {
        fwprintf(stderr, L"CreateFile failed. Win32 error=%lu\n", GetLastError());
        return 1;
    }

    ok = DeviceIoControl(
        device,
        IOCTL_TOUCH_SELFTEST_CALIBRATE,
        &calibration,
        sizeof(calibration),
        &calibration,
        sizeof(calibration),
        &bytesReturned,
        nullptr);

    CloseHandle(device);

    wprintf(L"DeviceIoControl: %ls, Win32 error=%lu, bytes=%lu\n",
            ok ? L"success" : L"failed",
            ok ? 0 : GetLastError(),
            bytesReturned);
    wprintf(L"OperationStatus=0x%08lx PollCount=%lu DeviceMode=0x%02x State=0x%02x ErrorCode=0x%02x\n",
            calibration.OperationStatus,
            calibration.PollCount,
            calibration.FinalDeviceMode,
            calibration.FinalState,
            calibration.ErrorCode);

    return ok ? 0 : 1;
}
