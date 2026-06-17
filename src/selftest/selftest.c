/*++
    Copyright (c) Microsoft Corporation. All Rights Reserved.
    Copyright (c) Bingxing Wang. All Rights Reserved.
    Copyright (c) LumiaWoA authors. All Rights Reserved.

    Module Name:

        selftest.c

    Abstract:

        Implements internal interfaces exposing functionality needed for 
        controller self-tests, which can be used to run HW testing 
        on a device in the manufacturing line, etc.

    Environment:

        Kernel mode

    Revision History:

--*/

#include <internal.h>
#include <controller.h>
#include <ft5x\ftinternal.h>
#include <spb.h>
#include <initguid.h>
#include <devguid.h>
#include <selftest\selftest.h>
#include <selftest.tmh>

#define FT5X06_REG_DEVICE_MODE        0x00
#define FT5X06_REG_FACTORY_COMMAND    0x02
#define FT5X06_REG_STATE              0xA7
#define FT5X06_REG_ERROR              0xA9

#define FT5X06_DEVICE_MODE_MASK       0x70
#define FT5X06_DEVICE_MODE_WORK       0x00
#define FT5X06_DEVICE_MODE_FACTORY    0x40

#define FT5X06_STATE_WORK             0x01
#define FT5X06_STATE_CALIBRATION      0x02
#define FT5X06_STATE_FACTORY          0x03

#define FT5X06_FACTORY_CMD_CALIBRATE  0x04
#define FT5X06_FACTORY_CMD_STORE      0x05

#define TOUCH_CALIBRATION_TIMEOUT_MS       5000
#define TOUCH_CALIBRATION_POLL_INTERVAL_MS 100

static
VOID
TchSelfTestDelayMs(
    IN ULONG Milliseconds
    )
{
    LARGE_INTEGER delay;

    delay.QuadPart = WDF_REL_TIMEOUT_IN_MS(Milliseconds);
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

static
NTSTATUS
TchSelfTestWriteRegister(
    IN PDEVICE_EXTENSION DevContext,
    IN UCHAR Register,
    IN UCHAR Value
    )
{
    return SpbWriteDataSynchronously(
        &DevContext->I2CContext,
        Register,
        &Value,
        sizeof(Value));
}

static
NTSTATUS
TchSelfTestReadRegister(
    IN PDEVICE_EXTENSION DevContext,
    IN UCHAR Register,
    OUT UCHAR* Value
    )
{
    return SpbReadDataSynchronously(
        &DevContext->I2CContext,
        Register,
        Value,
        sizeof(*Value));
}

static
NTSTATUS
TchSelfTestReadCalibrationStatus(
    IN PDEVICE_EXTENSION DevContext,
    IN PTOUCH_SELFTEST_CALIBRATION Calibration
    )
{
    NTSTATUS status;

    status = TchSelfTestReadRegister(
        DevContext,
        FT5X06_REG_DEVICE_MODE,
        &Calibration->FinalDeviceMode);

    if (NT_SUCCESS(status))
    {
        status = TchSelfTestReadRegister(
            DevContext,
            FT5X06_REG_STATE,
            &Calibration->FinalState);
    }

    if (NT_SUCCESS(status))
    {
        status = TchSelfTestReadRegister(
            DevContext,
            FT5X06_REG_ERROR,
            &Calibration->ErrorCode);
    }

    return status;
}

static
NTSTATUS
TchSelfTestRunFactoryCalibration(
    IN PDEVICE_EXTENSION DevContext,
    IN OUT PTOUCH_SELFTEST_CALIBRATION Calibration
    )
{
    NTSTATUS status;
    ULONG timeoutMs;
    ULONG pollIntervalMs;
    ULONG elapsedMs;

    timeoutMs = Calibration->TimeoutMs == 0 ?
        TOUCH_CALIBRATION_TIMEOUT_MS : Calibration->TimeoutMs;
    pollIntervalMs = Calibration->PollIntervalMs == 0 ?
        TOUCH_CALIBRATION_POLL_INTERVAL_MS : Calibration->PollIntervalMs;

    if (pollIntervalMs == 0 || pollIntervalMs > timeoutMs)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Calibration->PollCount = 0;
    Calibration->OperationStatus = STATUS_UNSUCCESSFUL;
    Calibration->FinalDeviceMode = 0;
    Calibration->FinalState = 0;
    Calibration->ErrorCode = 0;
    Calibration->Reserved = 0;

    status = TchSelfTestWriteRegister(
        DevContext,
        FT5X06_REG_DEVICE_MODE,
        FT5X06_DEVICE_MODE_FACTORY);
    if (!NT_SUCCESS(status))
    {
        goto exit;
    }

    TchSelfTestDelayMs(100);

    status = TchSelfTestWriteRegister(
        DevContext,
        FT5X06_REG_FACTORY_COMMAND,
        FT5X06_FACTORY_CMD_CALIBRATE);
    if (!NT_SUCCESS(status))
    {
        goto exit;
    }

    TchSelfTestDelayMs(300);

    for (elapsedMs = 0; elapsedMs < timeoutMs; elapsedMs += pollIntervalMs)
    {
        status = TchSelfTestReadCalibrationStatus(DevContext, Calibration);
        if (!NT_SUCCESS(status))
        {
            goto exit;
        }

        Calibration->PollCount++;

        if (Calibration->ErrorCode != 0)
        {
            status = STATUS_DEVICE_DATA_ERROR;
            goto exit;
        }

        if (((Calibration->FinalDeviceMode & FT5X06_DEVICE_MODE_MASK) ==
             FT5X06_DEVICE_MODE_WORK) ||
            Calibration->FinalState == FT5X06_STATE_FACTORY ||
            Calibration->FinalState == FT5X06_STATE_WORK)
        {
            status = STATUS_SUCCESS;
            break;
        }

        TchSelfTestDelayMs(pollIntervalMs);
    }

    if (!NT_SUCCESS(status))
    {
        goto exit;
    }

    if (elapsedMs >= timeoutMs)
    {
        status = STATUS_IO_TIMEOUT;
        goto exit;
    }

    if (Calibration->Flags & TOUCH_SELFTEST_CALIBRATION_FLAG_STORE)
    {
        status = TchSelfTestWriteRegister(
            DevContext,
            FT5X06_REG_DEVICE_MODE,
            FT5X06_DEVICE_MODE_FACTORY);
        if (!NT_SUCCESS(status))
        {
            goto exit;
        }

        TchSelfTestDelayMs(100);

        status = TchSelfTestWriteRegister(
            DevContext,
            FT5X06_REG_FACTORY_COMMAND,
            FT5X06_FACTORY_CMD_STORE);
        if (!NT_SUCCESS(status))
        {
            goto exit;
        }

        TchSelfTestDelayMs(300);
    }

exit:
    Calibration->OperationStatus = status;
    (VOID)TchSelfTestReadCalibrationStatus(DevContext, Calibration);
    (VOID)TchSelfTestWriteRegister(
        DevContext,
        FT5X06_REG_DEVICE_MODE,
        FT5X06_DEVICE_MODE_WORK);

    return status;
}

VOID
TchSelfTestOnDeviceControl(
    IN WDFQUEUE Queue,
    IN WDFREQUEST Request,
    IN size_t OutputBufferLength,
    IN size_t InputBufferLength,
    IN ULONG IoControlCode
    )
/*++

Routine Description:

    This dispatch routine allows a user-mode application to issue test
    requests to the driver for execution on the chip and reporting of 
    results. 

Arguments:

    Queue - Framework queue object handle
    Request - Framework request object handle
    OutputBufferLength - self-explanatory
    InputBufferLength - self-explanatory
    IoControlCode - Specifies what is being requested

Return Value:

    NTSTATUS indicating success or failure

--*/
{
    PDEVICE_EXTENSION devContext;
    UCHAR* readBuffer = NULL;
    TOUCH_TEST_I2C_HEADER* headerIn = NULL;
    TOUCH_TEST_I2C_HEADER headerTemp = {0};
    size_t bytesReturned;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    BOOLEAN *requestedDiagnosticMode;
    UCHAR *requestedPage;
    PTOUCH_SELFTEST_CALIBRATION calibration;


    devContext = GetDeviceContext(WdfPdoGetParent(WdfIoQueueGetDevice(Queue)));

    //
    // Ensure we're in a test session (framework should prevent this)
    //
    ASSERT(0 != devContext->TestSessionRefCnt);

    //
    // Process the test request
    //
    switch (IoControlCode)
    {
        case IOCTL_TOUCH_SELFTEST_READ:
        {
            //
            // Validate parameters and memory
            //
            if (InputBufferLength != sizeof(TOUCH_TEST_I2C_HEADER))
            {
                status = STATUS_INVALID_PARAMETER;
                goto exit;
            }

            status = WdfRequestRetrieveInputBuffer(
                Request,
                sizeof(TOUCH_TEST_I2C_HEADER),
                (PVOID) &headerIn,
                NULL);

            if ((!NT_SUCCESS(status)) || 
                (headerIn->AddressLength != sizeof(headerIn->Address)) ||
                (headerIn->RequestedTransferLength < 1))
            {
                status = STATUS_INVALID_PARAMETER;
                goto exit;
            }

            //
            // Create a copy of headerIn since in and out buffers point to the
            // same memory and so SpbReadDataSync will overwrite it
            //
            headerTemp = *headerIn;

            status = WdfRequestRetrieveOutputBuffer(
                Request,
                headerTemp.RequestedTransferLength,
                (PVOID) &readBuffer,
                NULL);

            if ((!NT_SUCCESS(status)) ||
                (headerTemp.RequestedTransferLength > OutputBufferLength))
            {
                status = STATUS_INVALID_PARAMETER;
                goto exit;
            }

            //
            // Perform read
            //
            status = SpbReadDataSynchronously(
                &devContext->I2CContext,
                headerTemp.Address,
                readBuffer,
                headerTemp.RequestedTransferLength);
            if (!NT_SUCCESS(status))
            {
                goto exit;
            }

            WdfRequestSetInformation(Request, headerTemp.RequestedTransferLength);

            break;
        }

        case IOCTL_TOUCH_SELFTEST_WRITE:
        {
            //
            // Validate parameters and memory
            //
            if (InputBufferLength < sizeof(TOUCH_TEST_I2C_HEADER))
            {
                status = STATUS_INVALID_PARAMETER;
                goto exit;
            }

            status = WdfRequestRetrieveInputBuffer(
                Request,
                sizeof(TOUCH_TEST_I2C_HEADER),
                (PVOID) &headerIn,
                &bytesReturned);

            if ((!NT_SUCCESS(status)) ||
                (headerIn->AddressLength != sizeof(headerIn->Address)) ||
                (bytesReturned != (sizeof(TOUCH_TEST_I2C_HEADER) + headerIn->RequestedTransferLength)))
            {
                status = STATUS_INVALID_PARAMETER;
                goto exit;
            }

            //
            // Perform write
            //
            status = SpbWriteDataSynchronously(
                &devContext->I2CContext,
                headerIn->Address,
                (PVOID) (headerIn+1),
                headerIn->RequestedTransferLength);

            if (!NT_SUCCESS(status))
            {
                goto exit;
            }

            WdfRequestSetInformation(Request, headerIn->RequestedTransferLength);

            break;
        }

        case IOCTL_TOUCH_SELFTEST_MODE:
        {
            //
            // Validate parameters and memory
            //
            if (InputBufferLength != sizeof(BOOLEAN))
            {
                status = STATUS_INVALID_PARAMETER;
                goto exit;
            }

            status = WdfRequestRetrieveInputBuffer(
                Request,
                sizeof(BOOLEAN),
                (PVOID) &requestedDiagnosticMode,
                NULL);

            if (!NT_SUCCESS(status))
            {
                status = STATUS_INVALID_PARAMETER;
                goto exit;
            }

            if (*requestedDiagnosticMode != devContext->DiagnosticMode)
            {
                devContext->DiagnosticMode = *requestedDiagnosticMode;
            }

            WdfRequestSetInformation(Request, sizeof(*requestedDiagnosticMode));

            break;
        }
        
        case IOCTL_TOUCH_SELFTEST_CHANGE_PAGE:
        {
            //
            // Validate parameters and memory
            //
            if (InputBufferLength != sizeof(UCHAR))
            {
                status = STATUS_INVALID_PARAMETER;
                goto exit;
            }

            status = WdfRequestRetrieveInputBuffer(
                Request,
                sizeof(UCHAR),
                (PVOID) &requestedPage,
                NULL);

            if (!NT_SUCCESS(status))
            {
                status = STATUS_INVALID_PARAMETER;
                goto exit;
            }

            status = Ft5xChangePage(
                devContext->TouchContext,
                &devContext->I2CContext,
                *requestedPage);
            if (!NT_SUCCESS(status))
            {
                goto exit;
            }

            WdfRequestSetInformation(Request, sizeof(*requestedPage));

            break;
        }

        case IOCTL_TOUCH_SELFTEST_CALIBRATE:
        {
            if (InputBufferLength < sizeof(TOUCH_SELFTEST_CALIBRATION) ||
                OutputBufferLength < sizeof(TOUCH_SELFTEST_CALIBRATION))
            {
                status = STATUS_INVALID_PARAMETER;
                goto exit;
            }

            status = WdfRequestRetrieveInputBuffer(
                Request,
                sizeof(TOUCH_SELFTEST_CALIBRATION),
                (PVOID*)&calibration,
                NULL);

            if (!NT_SUCCESS(status) ||
                calibration->Size != sizeof(TOUCH_SELFTEST_CALIBRATION))
            {
                status = STATUS_INVALID_PARAMETER;
                goto exit;
            }

            status = TchSelfTestRunFactoryCalibration(
                devContext,
                calibration);

            WdfRequestSetInformation(
                Request,
                sizeof(TOUCH_SELFTEST_CALIBRATION));

            break;
        }

        default:
        {
            status = STATUS_NOT_IMPLEMENTED;
            break;
        } 
    }

exit:

    WdfRequestComplete(
        Request,
        status);
}

VOID 
TchSelfTestOnCreate(
    IN WDFDEVICE Device,
    IN WDFREQUEST Request,
    IN WDFFILEOBJECT FileObject
    )
/*++

Routine Description:

    This dispatch routine is invoked when a user-mode application is
    opening a test session. We simply reference count the number of 
    creates and put the device into test mode.

Arguments:

    Device - Framework device object representing the test device
    Request - Can be used to get create information, ununsed here.
    FileObject - Unused here.

Return Value:

    None

--*/

{
    PDEVICE_EXTENSION devContext;
    LONG testSessionCount;

    UNREFERENCED_PARAMETER(FileObject);

    devContext = GetDeviceContext(WdfPdoGetParent(Device));
    testSessionCount = InterlockedIncrement(&(devContext->TestSessionRefCnt));

    WdfRequestComplete(
        Request,
        STATUS_SUCCESS);
}

VOID 
TchSelfTestOnClose(
    IN WDFFILEOBJECT FileObject
    )
/*++

Routine Description:

    This dispatch routine is invoked when a user-mode application is
    closing a test session. We simply reference count the number of 
    closes and restore the driver to normal operation if necessary.

Arguments:

    FileObject - Unused here.

Return Value:

    None

--*/

{
    PDEVICE_EXTENSION devContext;
    LONG testSessionCount;

    devContext = GetDeviceContext(WdfPdoGetParent(WdfFileObjectGetDevice(FileObject)));

    testSessionCount = InterlockedDecrement(&(devContext->TestSessionRefCnt));
}

NTSTATUS
TchSelfTestInitialize(
    IN WDFDEVICE Device
    )
/*++

Routine Description:

    This function creates a PDO that will be accessed directly by a user-mode
    application in order to initiate controller testing, gather results, etc.

    The test PDO is associated with the touch device's FDO and given a
    device interface so a user-mode application to find the test device. 

    The function registers EvtIoDeviceControl, EvtIoRead, and EvtIoWrite 
    dispatch routines which comprise the user-mode device driver interface for 
    test functionality.

Arguments:

    Device - Framework device object representing the actual touch device

Return Value:

    NTSTATUS indicating success or failure

--*/
{
    NTSTATUS status;
    PDEVICE_EXTENSION devContext;
    PWDFDEVICE_INIT deviceInit = NULL;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDFDEVICE childDevice = NULL;
    WDF_OBJECT_ATTRIBUTES objectAttributes;
    WDF_IO_QUEUE_CONFIG queueConfig;

    DECLARE_CONST_UNICODE_STRING(deviceId, L"{3a0ac59a-4d8a-4875-b7ea-304771ff9b9a}\\NokiaTouch\0");
    DECLARE_CONST_UNICODE_STRING(hardwareId, L"NOKIA_TOUCH");
    DECLARE_CONST_UNICODE_STRING(instanceId, L"0\0");

    devContext = GetDeviceContext(Device);

    //
    // Create a child test PDO, the touch device is the parent
    //
    deviceInit = WdfPdoInitAllocate(Device);

    if (NULL == deviceInit)
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_INIT,
            "Error allocating WDFDEVICE_INIT structure");

        status = STATUS_INVALID_DEVICE_STATE;

        goto exit;
    }

    //
    // NOTE: Default driver security should suffice for this interface
    //

    //
    // Indicate this PDO runs in "raw mode", so the framework doesn't
    // attempt to load a function driver -- we just want an interface
    // and non-HID stack for passing test data
    //
    status = WdfPdoInitAssignRawDevice(
        deviceInit,
        &GUID_DEVCLASS_HIDCLASS);

    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_INIT,
            "Error assigning test device object as raw device - %!STATUS!",
            status);

        goto exit;
    }

    //
    // Assign device, hardware, instance IDs
    //
    status = WdfPdoInitAssignDeviceID(
        deviceInit,
        &deviceId);

    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_INIT,
            "Error assigning test device ID - %!STATUS!",
            status);

        goto exit;
    }

    status = WdfPdoInitAddHardwareID(
        deviceInit,
        &hardwareId);

    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_INIT,
            "Error assigning test device hardware ID - %!STATUS!",
            status);

        goto exit;
    }

    status = WdfPdoInitAssignInstanceID(
        deviceInit,
        &instanceId);

    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_INIT,
            "Error assigning test device instance ID - %!STATUS!",
            status);

        goto exit;
    }

    //
    // We will want to know when a test application has opened
    // or closed a handle to the test device -- during this time
    // the touch driver's normal operation is interrupted.
    //
    WDF_FILEOBJECT_CONFIG_INIT(
        &fileConfig,
        TchSelfTestOnCreate,
        TchSelfTestOnClose,
        WDF_NO_EVENT_CALLBACK);

    WdfDeviceInitSetFileObjectConfig(
        deviceInit,
        &fileConfig,
        WDF_NO_OBJECT_ATTRIBUTES);

    //
    // Create the touch test device
    //
    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttributes);

    status = WdfDeviceCreate(
        &deviceInit,
        &objectAttributes,
        &childDevice);

    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_INIT,
            "Error creating test device - %!STATUS!",
            status);

        goto exit;
    }

    //
    // Set up an I/O request queue so that calling application
    // may send test requests to the device.
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &queueConfig,
        WdfIoQueueDispatchParallel);

    queueConfig.EvtIoDeviceControl = TchSelfTestOnDeviceControl;

    status = WdfIoQueueCreate(
        childDevice,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &devContext->TestQueue);

    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_INIT,
            "Error creating test device request queue - %!STATUS!",
            status);

        goto exit;
    }

    //
    // Expose a device interface for a user-mode test application
    // to access this test device
    //
    status = WdfDeviceCreateDeviceInterface(
        childDevice,
        &GUID_TOUCH_SELFTEST_INTERFACE,
        NULL);

    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_INIT,
            "Error creating test device interface - %!STATUS!",
            status);

        goto exit;
    }

    //
    // This must be called in addition to WdfPdoInitAllocate to
    // associate the test PDO just created as child of the touch
    // driver FDO
    //
    status = WdfFdoAddStaticChild(Device, childDevice);

    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_INIT,
            "Error adding test PDO as child of touch FDO - %!STATUS!",
            status);

        goto exit;
    }

exit:

    if (!NT_SUCCESS(status))
    {
        if (deviceInit != NULL)
        {
            WdfDeviceInitFree(deviceInit);
        }

        if (childDevice != NULL)
        {
            WdfObjectDelete(childDevice);
        }
    }

    return status;
}
