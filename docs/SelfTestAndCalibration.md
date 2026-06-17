# Self-Test and Factory Calibration

## Self-test interface

`TchSelfTestInitialize` creates a raw child PDO under the touch device and
publishes `GUID_TOUCH_SELFTEST_INTERFACE`
(`{3a0ac59a-4d8a-4875-b7ea-304771ff9b9a}`). A user-mode factory or debug tool
opens this interface directly instead of talking through the HID path.

Opening the interface increments `TestSessionRefCnt`; closing it decrements the
counter. IOCTL dispatch asserts that a self-test session is active before it
touches the controller.

The original self-test IOCTLs are:

- `IOCTL_TOUCH_SELFTEST_READ`: read an arbitrary controller register over I2C.
- `IOCTL_TOUCH_SELFTEST_WRITE`: write arbitrary bytes to a controller register.
- `IOCTL_TOUCH_SELFTEST_MODE`: set `DiagnosticMode` in the device context.
- `IOCTL_TOUCH_SELFTEST_CHANGE_PAGE`: call `Ft5xChangePage` for paged register
  access.

`src/selftest/enoselftest.c` is a parallel test interface with its own GUID and
IOCTL names. It exposes the same raw I2C, diagnostic mode, and page switching
shape, but is kept separate from the main FocalTech self-test interface.

## FT5x06 factory calibration

The FT5x06 manual describes the device mode register at `0x00`. Bits `[6:4]`
select the controller mode; `0x40` enters factory/test mode and `0x00` returns
to work mode. The global state register `0xA7` reports states such as work,
calibration, and factory, while `0xA9` reports controller error codes.

The driver now exposes:

- `IOCTL_TOUCH_SELFTEST_CALIBRATE`
- `TOUCH_SELFTEST_CALIBRATION`
- `TOUCH_SELFTEST_CALIBRATION_FLAG_STORE`

The kernel flow is:

1. Validate the buffered `TOUCH_SELFTEST_CALIBRATION` request.
2. Write `0x40` to register `0x00` to enter factory mode.
3. Write `0x04` to register `0x02` to start calibration.
4. Poll `0x00`, `0xA7`, and `0xA9` until calibration completes, an error is
   reported, or the timeout expires.
5. If `TOUCH_SELFTEST_CALIBRATION_FLAG_STORE` is set, re-enter factory mode and
   write `0x05` to register `0x02` to store the calibration result.
6. Always attempt to write `0x00` to register `0x00` before returning so the
   controller is left in work mode.

The returned structure includes:

- `OperationStatus`: the NTSTATUS from the calibration sequence.
- `PollCount`: number of status polls completed.
- `FinalDeviceMode`: last value read from `0x00`.
- `FinalState`: last value read from `0xA7`.
- `ErrorCode`: last value read from `0xA9`.

## Calibration test program

`tools/CalibrationTool` is a small console program that locates
`GUID_TOUCH_SELFTEST_INTERFACE`, opens it, and sends
`IOCTL_TOUCH_SELFTEST_CALIBRATE`.

Usage:

```text
CalibrationTool.exe [--timeout-ms N] [--poll-ms N] [--no-store]
```

By default it waits up to 5000 ms, polls every 100 ms, and stores the calibration
result. Use `--no-store` when validating the sequence without committing the
factory data.

Build the ARM64 Debug driver:

```powershell
& 'D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' contrib\FocalTechTouch.sln /m /t:Rebuild '/p:Configuration=Debug;Platform=ARM64;WindowsTargetPlatformVersion=10.0.22621.0;VisualStudioVersion=17.0;SkipPackageVerification=true;ApiValidator_Enable=false;EnableInf2cat=false' /v:minimal
```

Build the ARM64 Debug calibration tool:

```powershell
& 'D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' tools\CalibrationTool\CalibrationTool.vcxproj /m /t:Rebuild '/p:Configuration=Debug;Platform=ARM64;WindowsTargetPlatformVersion=10.0.22621.0;VisualStudioVersion=17.0' /v:minimal
```
