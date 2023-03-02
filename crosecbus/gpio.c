#include "driver.h"
#include "gpiowrapper.h"
#include <gpio.h>
#include <reshub.h>

static ULONG CrosEcBusDebugLevel = 100;
static ULONG CrosEcBusDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

NTSTATUS
GpioReadDataSynchronously(
	_In_ GPIO_CONTEXT *GpioContext,
	_In_reads_bytes_(Length) PVOID Data,
	_In_ ULONG Length
)
/*++

Routine Description:

This helper routine abstracts creating and sending an I/O
request (I2C Read) to the Spb I/O target.

Arguments:

SpbContext - Pointer to the current device context
Data       - A buffer to receive the data at at the above address
Length     - The amount of data to be read from the above address

Return Value:

NTSTATUS Status indicating success or failure

--*/
{
	WDF_OBJECT_ATTRIBUTES Attributes;
	WDFREQUEST IoctlRequest;
	WDFMEMORY WdfMemory;
	WDF_OBJECT_ATTRIBUTES RequestAttributes;
	WDF_REQUEST_SEND_OPTIONS SendOptions;
	NTSTATUS status;

	WdfWaitLockAcquire(GpioContext->GpioLock, NULL);

	status = STATUS_INVALID_PARAMETER;

	WDF_OBJECT_ATTRIBUTES_INIT(&RequestAttributes);
	status = WdfRequestCreate(&RequestAttributes, GpioContext->GpioIoTarget, &IoctlRequest);
	if (!NT_SUCCESS(status)) {
		CrosEcBusPrint(
			DEBUG_LEVEL_ERROR,
			DBG_IOCTL,
			"Error creating request - %x\n",
			status);
		goto exit;
	}

	WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
	Attributes.ParentObject = IoctlRequest;
	status = WdfMemoryCreatePreallocated(&Attributes, Data, Length, &WdfMemory);
	if (!NT_SUCCESS(status)) {
		CrosEcBusPrint(
			DEBUG_LEVEL_ERROR,
			DBG_IOCTL,
			"Error making preallocated memory - %x\n",
			status);
		goto exit;
	}

	status = WdfIoTargetFormatRequestForIoctl(GpioContext->GpioIoTarget,
		IoctlRequest,
		IOCTL_GPIO_READ_PINS,
		NULL,
		0,
		WdfMemory,
		0);
	if (!NT_SUCCESS(status))
	{
		CrosEcBusPrint(
			DEBUG_LEVEL_ERROR,
			DBG_IOCTL,
			"Error formatting request for ioctl - %x\n",
			status);
		goto exit;
	}

	WDF_REQUEST_SEND_OPTIONS_INIT(&SendOptions,
		WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);

	if (!WdfRequestSend(IoctlRequest, GpioContext->GpioIoTarget, &SendOptions)) {
		status = WdfRequestGetStatus(IoctlRequest);
	}

	if (!NT_SUCCESS(status))
	{
		CrosEcBusPrint(
			DEBUG_LEVEL_ERROR,
			DBG_IOCTL,
			"Error reading from Gpio - %x\n",
			status);
		goto exit;
	}
exit:
	if (IoctlRequest != NULL) {
		WdfObjectDelete(IoctlRequest);
	}

	WdfWaitLockRelease(GpioContext->GpioLock);

	return status;
}

VOID
GpioTargetDeinitialize(
	IN WDFDEVICE FxDevice,
	IN GPIO_CONTEXT *GpioContext
)
/*++

Routine Description:

This helper routine is used to free any members added to the SPB_CONTEXT,
note the SPB I/O target is parented to the device and will be
closed and free'd when the device is removed.

Arguments:

FxDevice   - Handle to the framework device object
SpbContext - Pointer to the current device context

Return Value:

NTSTATUS Status indicating success or failure

--*/
{
	UNREFERENCED_PARAMETER(FxDevice);
	UNREFERENCED_PARAMETER(GpioContext);

	//
	// Free any SPB_CONTEXT allocations here
	//
	if (GpioContext->GpioLock != NULL)
	{
		WdfObjectDelete(GpioContext->GpioLock);
	}
}

NTSTATUS
GpioTargetInitialize(
	IN WDFDEVICE FxDevice,
	IN GPIO_CONTEXT *GpioContext
)
/*++

Routine Description:

This helper routine opens the Spb I/O target and
initializes a request object used for the lifetime
of communication between this driver and Spb.

Arguments:

FxDevice   - Handle to the framework device object
SpbContext - Pointer to the current device context

Return Value:

NTSTATUS Status indicating success or failure

--*/
{
	WDF_OBJECT_ATTRIBUTES objectAttributes;
	WDF_IO_TARGET_OPEN_PARAMS openParams;
	UNICODE_STRING gpioDeviceName;
	WCHAR gpioDeviceNameBuffer[RESOURCE_HUB_PATH_SIZE];
	NTSTATUS status;

	WDF_OBJECT_ATTRIBUTES_INIT(&objectAttributes);
	objectAttributes.ParentObject = FxDevice;

	status = WdfIoTargetCreate(
		FxDevice,
		&objectAttributes,
		&GpioContext->GpioIoTarget);

	if (!NT_SUCCESS(status))
	{
		CrosEcBusPrint(
			DEBUG_LEVEL_ERROR,
			DBG_IOCTL,
			"Error creating IoTarget object - %!STATUS!",
			status);

		WdfObjectDelete(GpioContext->GpioIoTarget);
		goto exit;
	}

	RtlInitEmptyUnicodeString(
		&gpioDeviceName,
		gpioDeviceNameBuffer,
		sizeof(gpioDeviceNameBuffer));

	status = RESOURCE_HUB_CREATE_PATH_FROM_ID(
		&gpioDeviceName,
		GpioContext->GpioResHubId.LowPart,
		GpioContext->GpioResHubId.HighPart);

	if (!NT_SUCCESS(status))
	{
		CrosEcBusPrint(
			DEBUG_LEVEL_ERROR,
			DBG_IOCTL,
			"Error creating GPIO resource hub path string - %!STATUS!",
			status);
		goto exit;
	}

	WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
		&openParams,
		&gpioDeviceName,
		(FILE_GENERIC_READ));

	status = WdfIoTargetOpen(GpioContext->GpioIoTarget, &openParams);

	if (!NT_SUCCESS(status))
	{
		CrosEcBusPrint(
			DEBUG_LEVEL_ERROR,
			DBG_IOCTL,
			"Error opening GPIO target for communication - %!STATUS!",
			status);
		goto exit;
	}

	//
	// Allocate a waitlock to guard access to the default buffers
	//
	status = WdfWaitLockCreate(
		WDF_NO_OBJECT_ATTRIBUTES,
		&GpioContext->GpioLock);

	if (!NT_SUCCESS(status))
	{
		CrosEcBusPrint(
			DEBUG_LEVEL_ERROR,
			DBG_IOCTL,
			"Error creating Spb Waitlock - %!STATUS!",
			status);
		goto exit;
	}

exit:

	if (!NT_SUCCESS(status))
	{
		GpioTargetDeinitialize(FxDevice, GpioContext);
	}

	return status;
}