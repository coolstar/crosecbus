#define DESCRIPTOR_DEF
#include "driver.h"
#include "stdint.h"
#include "comm-host.h"

#define bool int
#define MS_IN_US 1000

static ULONG CrosEcBusDebugLevel = 100;
static ULONG CrosEcBusDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

NTSTATUS comm_init_lpc(void);

NTSTATUS
DriverEntry(
__in PDRIVER_OBJECT  DriverObject,
__in PUNICODE_STRING RegistryPath
)
{
	NTSTATUS               status = STATUS_SUCCESS;
	WDF_DRIVER_CONFIG      config;
	WDF_OBJECT_ATTRIBUTES  attributes;

	CrosEcBusPrint(DEBUG_LEVEL_INFO, DBG_INIT,
		"Driver Entry\n");

	WDF_DRIVER_CONFIG_INIT(&config, CrosEcBusEvtDeviceAdd);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

	//
	// Create a framework driver object to represent our driver.
	//

	status = WdfDriverCreate(DriverObject,
		RegistryPath,
		&attributes,
		&config,
		WDF_NO_HANDLE
		);

	if (!NT_SUCCESS(status))
	{
		CrosEcBusPrint(DEBUG_LEVEL_ERROR, DBG_INIT,
			"WdfDriverCreate failed with status 0x%x\n", status);
	}

	return status;
}

static NTSTATUS CrosEcCmdXferStatus(
	IN      PCROSECBUS_CONTEXT pDevice,
	OUT     PCROSEC_COMMAND Msg
)
{
	if (!Msg) {
		return STATUS_INVALID_PARAMETER;
	}

	if (ec_command_proto) {
		if (Msg->InSize > ec_max_insize) {
			CrosEcBusPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL, "Clamping message receive buffer\n");
			Msg->InSize = ec_max_insize;
		}

		if (Msg->OutSize > ec_max_outsize) {
			CrosEcBusPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL, "request of size %u is too big (max: %u)\n", Msg->OutSize, ec_max_outsize);
			return STATUS_INVALID_PARAMETER_3;
		}

		WdfWaitLockAcquire(pDevice->EcLock, NULL);

		int cmdstatus = ec_command_proto(Msg->Command, Msg->Version, Msg->Data, Msg->OutSize, Msg->Data, Msg->InSize);

		WdfWaitLockRelease(pDevice->EcLock, NULL);

		if (cmdstatus >= 0) {
			return STATUS_SUCCESS;
		}
		else {
			CrosEcBusPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL, "EC Returned Error: %d\n", cmdstatus);
			return STATUS_INTERNAL_ERROR;
		}
	}
	else {
		return STATUS_NOINTERFACE;
	}
}

static BOOLEAN CrosEcCheckFeatures(
	IN PCROSECBUS_CONTEXT pDevice,
	IN INT Feature
) {
	if (pDevice->EcFeatures[0] == -1 && pDevice->EcFeatures[1] == -1)
		return false; //Failed to get features

	return !!(pDevice->EcFeatures[Feature / 32] & EC_FEATURE_MASK_0(Feature));
}

static INT CrosEcReadMem(
	IN PCROSECBUS_CONTEXT pDevice,
	IN INT offset,
	IN INT bytes,
	OUT PVOID dest
)
{
	return ec_readmem(offset, bytes, dest);
}

NTSTATUS
OnPrepareHardware(
_In_  WDFDEVICE     FxDevice,
_In_  WDFCMRESLIST  FxResourcesRaw,
_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

This routine caches the SPB resource connection ID.

Arguments:

FxDevice - a handle to the framework device object
FxResourcesRaw - list of translated hardware resources that
the PnP manager has assigned to the device
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PCROSECBUS_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;

	UNREFERENCED_PARAMETER(FxResourcesRaw);
	UNREFERENCED_PARAMETER(FxResourcesTranslated);

	status = WdfWaitLockCreate(
		WDF_NO_OBJECT_ATTRIBUTES,
		&pDevice->EcLock);
	if (!NT_SUCCESS(status))
	{
		CrosEcBusPrint(
			DEBUG_LEVEL_ERROR,
			DBG_IOCTL,
			"Error creating Waitlock - %x\n",
			status);
		return status;
	}

	status = comm_init_lpc();
	if (!NT_SUCCESS(status)) {
		return status;
	}

	struct ec_response_get_version r;
	int rv = ec_command_proto(EC_CMD_GET_VERSION, 0, NULL, 0, &r, sizeof(struct ec_response_get_version));
	if (rv >= 0) {
		/* Ensure versions are null-terminated before we print them */
		r.version_string_ro[sizeof(r.version_string_ro) - 1] = '\0';
		r.version_string_rw[sizeof(r.version_string_rw) - 1] = '\0';

		DbgPrint("EC RO Version: %s\n", r.version_string_ro);
		DbgPrint("EC RW Version: %s\n", r.version_string_rw);
	}
	else {
		DbgPrint("Error: Could not get version\n");
		return STATUS_DEVICE_CONFIGURATION_ERROR;
	}

	struct ec_response_get_features f;

	rv = ec_command_proto(EC_CMD_GET_FEATURES, 0, NULL, 0, &f, sizeof(struct ec_response_get_features));
	if (rv >= 0) {
		pDevice->EcFeatures[0] = f.flags[0];
		pDevice->EcFeatures[1] = f.flags[1];

		DbgPrint("EC Features: %08x %08x\n", pDevice->EcFeatures[0], pDevice->EcFeatures[1]);
	}
	else {
		DbgPrint("Warning: Couldn't get device features\n");
		pDevice->EcFeatures[0] = -1;
		pDevice->EcFeatures[1] = -1;
	}

	return status;
}

NTSTATUS
OnReleaseHardware(
_In_  WDFDEVICE     FxDevice,
_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

Arguments:

FxDevice - a handle to the framework device object
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	NTSTATUS status = STATUS_SUCCESS;

	PCROSECBUS_CONTEXT pDevice = GetDeviceContext(FxDevice);
	UNREFERENCED_PARAMETER(FxResourcesTranslated);

	if (pDevice->EcLock != NULL)
	{
		WdfObjectDelete(pDevice->EcLock);
	}

	return status;
}

NTSTATUS
OnD0Entry(
_In_  WDFDEVICE               FxDevice,
_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine allocates objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxDevice);
	UNREFERENCED_PARAMETER(FxPreviousState);
	NTSTATUS status = STATUS_SUCCESS;

	return status;
}

NTSTATUS
OnD0Exit(
_In_  WDFDEVICE               FxDevice,
_In_  WDF_POWER_DEVICE_STATE  FxTargetState
)
/*++

Routine Description:

This routine destroys objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxTargetState - target power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxDevice);
	UNREFERENCED_PARAMETER(FxTargetState);

	NTSTATUS status = STATUS_SUCCESS;

	return status;
}

NTSTATUS
CrosEcBusEvtDeviceAdd(
IN WDFDRIVER       Driver,
IN PWDFDEVICE_INIT DeviceInit
)
{
	NTSTATUS                      status = STATUS_SUCCESS;
	WDF_OBJECT_ATTRIBUTES         attributes;
	WDFDEVICE                     device;
	UCHAR                         minorFunction;
	PCROSECBUS_CONTEXT               devContext;
	WDF_QUERY_INTERFACE_CONFIG  qiConfig;

	UNREFERENCED_PARAMETER(Driver);

	PAGED_CODE();

	CrosEcBusPrint(DEBUG_LEVEL_INFO, DBG_PNP,
		"CrosEcBusEvtDeviceAdd called\n");

	{
		WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
		WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

		pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
		pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
		pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
		pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;

		WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);
	}

	//
	// Setup the device context
	//

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, CROSECBUS_CONTEXT);

	// Set DeviceType
	WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_CONTROLLER);

	//
	// Create a framework device object.This call will in turn create
	// a WDM device object, attach to the lower stack, and set the
	// appropriate flags and attributes.
	//

	status = WdfDeviceCreate(&DeviceInit, &attributes, &device);

	if (!NT_SUCCESS(status))
	{
		CrosEcBusPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfDeviceCreate failed with status code 0x%x\n", status);

		return status;
	}

	{
		WDF_DEVICE_STATE deviceState;
		WDF_DEVICE_STATE_INIT(&deviceState);

		deviceState.NotDisableable = WdfFalse;
		WdfDeviceSetDeviceState(device, &deviceState);
	}

	//
	// Create manual I/O queue to take care of hid report read requests
	//

	devContext = GetDeviceContext(device);

	DECLARE_CONST_UNICODE_STRING(dosDeviceName, SYMBOLIC_NAME_STRING);

	status = WdfDeviceCreateSymbolicLink(device,
		&dosDeviceName
	);
	if (!NT_SUCCESS(status)) {
		CrosEcBusPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfDeviceCreateSymbolicLink failed 0x%x\n", status);
		return status;
	}

	{ // V1
		CROSEC_INTERFACE_STANDARD CrosEcInterface;
		RtlZeroMemory(&CrosEcInterface, sizeof(CrosEcInterface));

		CrosEcInterface.InterfaceHeader.Size = sizeof(CrosEcInterface);
		CrosEcInterface.InterfaceHeader.Version = 1;
		CrosEcInterface.InterfaceHeader.Context = (PVOID)devContext;

		//
		// Let the framework handle reference counting.
		//
		CrosEcInterface.InterfaceHeader.InterfaceReference = WdfDeviceInterfaceReferenceNoOp;
		CrosEcInterface.InterfaceHeader.InterfaceDereference = WdfDeviceInterfaceDereferenceNoOp;

		CrosEcInterface.CheckFeatures = CrosEcCheckFeatures;
		CrosEcInterface.CmdXferStatus = CrosEcCmdXferStatus;

		WDF_QUERY_INTERFACE_CONFIG_INIT(&qiConfig,
			(PINTERFACE)&CrosEcInterface,
			&GUID_CROSEC_INTERFACE_STANDARD,
			NULL);

		status = WdfDeviceAddQueryInterface(device, &qiConfig);
		if (!NT_SUCCESS(status)) {
			CrosEcBusPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
				"WdfDeviceAddQueryInterface failed 0x%x\n", status);

			return status;
		}
	}

	{ // V2
		CROSEC_INTERFACE_STANDARD_V2 CrosEcInterface;
		RtlZeroMemory(&CrosEcInterface, sizeof(CrosEcInterface));

		CrosEcInterface.InterfaceHeader.Size = sizeof(CrosEcInterface);
		CrosEcInterface.InterfaceHeader.Version = 2;
		CrosEcInterface.InterfaceHeader.Context = (PVOID)devContext;

		//
		// Let the framework handle reference counting.
		//
		CrosEcInterface.InterfaceHeader.InterfaceReference = WdfDeviceInterfaceReferenceNoOp;
		CrosEcInterface.InterfaceHeader.InterfaceDereference = WdfDeviceInterfaceDereferenceNoOp;

		CrosEcInterface.CheckFeatures = CrosEcCheckFeatures;
		CrosEcInterface.CmdXferStatus = CrosEcCmdXferStatus;
		CrosEcInterface.ReadEcMem = CrosEcReadMem;

		WDF_QUERY_INTERFACE_CONFIG_INIT(&qiConfig,
			(PINTERFACE)&CrosEcInterface,
			&GUID_CROSEC_INTERFACE_STANDARD_V2,
			NULL);

		status = WdfDeviceAddQueryInterface(device, &qiConfig);
		if (!NT_SUCCESS(status)) {
			CrosEcBusPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
				"WdfDeviceAddQueryInterface failed 0x%x\n", status);

			return status;
		}
	}

	devContext->FxDevice = device;

	return status;
}
