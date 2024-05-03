#define DESCRIPTOR_DEF
#include "driver.h"
#pragma warning(disable:4005)
#pragma warning(disable:4083)
#include <stdint.h>
#include "comm-host.h"
#include "userspaceQueue.h"

#define bool int
#define MS_IN_US 1000

BOOLEAN OnInterruptIsr(
	WDFINTERRUPT Interrupt,
	ULONG MessageID);

VOID
CrosEcBusS0ixNotifyCallback(
	PCROSECBUS_CONTEXT pDevice,
	ULONG NotifyCode);

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

		InterlockedIncrement64(&pDevice->KernelAccessesWaiting);
		WdfWaitLockAcquire(pDevice->EcLock, NULL);

		int cmdstatus = ec_command_proto((UINT16)Msg->Command, (UINT8)Msg->Version, Msg->Data, Msg->OutSize, Msg->Data, Msg->InSize);

		InterlockedDecrement64(&pDevice->KernelAccessesWaiting);
		WdfWaitLockRelease(pDevice->EcLock);

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
	UNREFERENCED_PARAMETER(pDevice);
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
	WDF_INTERRUPT_CONFIG interruptConfig;

	UNREFERENCED_PARAMETER(FxResourcesRaw);

	pDevice->FoundSyncGPIO = FALSE;

	//
	// Parse the peripheral's resources.
	//

	ULONG resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);

	for (ULONG i = 0; i < resourceCount; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR pDescriptor, pDescriptorRaw;
		UCHAR Class;
		UCHAR Type;

		pDescriptor = WdfCmResourceListGetDescriptor(
			FxResourcesTranslated, i);
		pDescriptorRaw = WdfCmResourceListGetDescriptor(
			FxResourcesRaw, i);

		switch (pDescriptor->Type)
		{
		case CmResourceTypeInterrupt:
			//
			// Create an interrupt object for hardware notifications
			//
			WDF_INTERRUPT_CONFIG_INIT(
				&interruptConfig,
				OnInterruptIsr,
				NULL);
			interruptConfig.PassiveHandling = TRUE;
			interruptConfig.InterruptRaw = pDescriptorRaw;
			interruptConfig.InterruptTranslated = pDescriptor;

			status = WdfInterruptCreate(
				pDevice->FxDevice,
				&interruptConfig,
				WDF_NO_OBJECT_ATTRIBUTES,
				&pDevice->Interrupt);

			if (!NT_SUCCESS(status))
			{
				CrosEcBusPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
					"Error creating WDF interrupt object - %!STATUS!",
					status);
			}

			DbgPrint("Found Sync GPIO!\n");
		default:
			//
			// Ignoring all other resource types.
			//
			break;
		}
	}

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

	struct ec_response_get_version r = { 0 };
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

	struct ec_response_get_features f = { 0 };

	rv = ec_command_proto(EC_CMD_GET_FEATURES, 0, NULL, 0, &f, sizeof(struct ec_response_get_features));
	if (rv >= 0) {
		pDevice->EcFeatures[0] = f.flags[0];
		pDevice->EcFeatures[1] = f.flags[1];

		DbgPrint("EC Features: %08x %08x\n", pDevice->EcFeatures[0], pDevice->EcFeatures[1]);
	}
	else {
		DbgPrint("Warning: Couldn't get device features\n");
		pDevice->EcFeatures[0] = (UINT32)-1;
		pDevice->EcFeatures[1] = (UINT32)-1;
	}

	pDevice->hostSleepV1 = FALSE;

	NTSTATUS acpiNotifyStatus = WdfFdoQueryForInterface(FxDevice,
		&GUID_ACPI_INTERFACE_STANDARD2,
		(PINTERFACE)&pDevice->S0ixNotifyAcpiInterface,
		sizeof(ACPI_INTERFACE_STANDARD2),
		1,
		NULL);

	if (NT_SUCCESS(acpiNotifyStatus)) {
		struct ec_params_get_cmd_versions_v1 req_v1 = { 0 };
		struct ec_response_get_cmd_versions resp = { 0 };
		req_v1.cmd = EC_CMD_HOST_SLEEP_EVENT;
		rv = ec_command_proto(EC_CMD_GET_CMD_VERSIONS, 1, &req_v1, sizeof(req_v1), &resp, sizeof(resp));
		if (rv >= 0) {
			pDevice->hostSleepV1 = (resp.version_mask & EC_VER_MASK(1)) != 0;
		}

		acpiNotifyStatus = pDevice->S0ixNotifyAcpiInterface.RegisterForDeviceNotifications(
			pDevice->S0ixNotifyAcpiInterface.Context,
			(PDEVICE_NOTIFY_CALLBACK2)CrosEcBusS0ixNotifyCallback,
			pDevice);
		if (!NT_SUCCESS(acpiNotifyStatus)) {
			DbgPrint("Warning: Failed to register notifications on ACPI device\n");
		}
	}
	else {
		DbgPrint("Warning: Failed to get ACPI device\n");
	}

	return status;
}

NTSTATUS
OnSelfManagedIoInit(
	_In_
	WDFDEVICE FxDevice
) {
	PCROSECBUS_CONTEXT pDevice;
	pDevice = GetDeviceContext(FxDevice);

	NTSTATUS status = STATUS_SUCCESS;

	// CS Keyboard Callback

	UNICODE_STRING CSKeyboardSettingsCallbackAPI;
	RtlInitUnicodeString(&CSKeyboardSettingsCallbackAPI, L"\\CallBack\\CsKeyboardSettingsCallbackAPI");


	OBJECT_ATTRIBUTES attributes;
	InitializeObjectAttributes(&attributes,
		&CSKeyboardSettingsCallbackAPI,
		OBJ_KERNEL_HANDLE | OBJ_OPENIF | OBJ_CASE_INSENSITIVE | OBJ_PERMANENT,
		NULL,
		NULL
	);
	status = ExCreateCallback(&pDevice->CSButtonsCallback, &attributes, TRUE, TRUE);
	if (!NT_SUCCESS(status)) {

		return status;
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

	if (pDevice->S0ixNotifyAcpiInterface.Context) { //Used for S0ix notifications
		pDevice->S0ixNotifyAcpiInterface.UnregisterForDeviceNotifications(pDevice->S0ixNotifyAcpiInterface.Context);
	}

	if (pDevice->EcLock != NULL)
	{
		WdfObjectDelete(pDevice->EcLock);
	}

	if (pDevice->CSButtonsCallback) {
		ObfDereferenceObject(pDevice->CSButtonsCallback);
		pDevice->CSButtonsCallback = NULL;
	}

	return status;
}

static NTSTATUS send_ec_command(
	_In_ PCROSECBUS_CONTEXT pDevice,
	UINT32 cmd,
	UINT32 version,
	UINT8* out,
	size_t outSize,
	UINT8* in,
	size_t inSize)
{
	PCROSEC_COMMAND msg = (PCROSEC_COMMAND)ExAllocatePoolWithTag(NonPagedPool, sizeof(CROSEC_COMMAND) + max(outSize, inSize), CROSECBUS_POOL_TAG);
	if (!msg) {
		return STATUS_NO_MEMORY;
	}
	msg->Version = version;
	msg->Command = cmd;
	msg->OutSize = (UINT32)outSize;
	msg->InSize = (UINT32)inSize;

	if (outSize)
		memcpy(msg->Data, out, outSize);

	NTSTATUS status = CrosEcCmdXferStatus(pDevice, msg);
	if (!NT_SUCCESS(status)) {
		goto exit;
	}

	if (in && inSize) {
		memcpy(in, msg->Data, inSize);
	}

exit:
	ExFreePoolWithTag(msg, CROSECBUS_POOL_TAG);
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
	UNREFERENCED_PARAMETER(FxPreviousState);
	NTSTATUS status = STATUS_SUCCESS;

	PCROSECBUS_CONTEXT pDevice = GetDeviceContext(FxDevice);

	struct ec_params_motion_sense params = { 0 };
	struct ec_response_motion_sense resp;

	params.cmd = MOTIONSENSE_CMD_FIFO_INT_ENABLE;
	params.fifo_int_enable.enable = 0;

	send_ec_command(pDevice, EC_CMD_MOTION_SENSE_CMD, 1, (UINT8*)&params, sizeof(params), (UINT8*)&resp, sizeof(resp)); //Ignore response as device may not have sensors

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
	UNREFERENCED_PARAMETER(FxTargetState);

	NTSTATUS status = STATUS_SUCCESS;
	PCROSECBUS_CONTEXT pDevice = GetDeviceContext(FxDevice);

	return status;
}

BOOLEAN OnInterruptIsr(
	WDFINTERRUPT Interrupt,
	ULONG MessageID) {
	UNREFERENCED_PARAMETER(MessageID);

	WDFDEVICE Device = WdfInterruptGetDevice(Interrupt);
	PCROSECBUS_CONTEXT pDevice = GetDeviceContext(Device);

	const uint32_t mkbp_mask =
		EC_HOST_EVENT_MASK(EC_HOST_EVENT_MKBP);
	struct ec_response_host_event_mask r;

	NTSTATUS status = send_ec_command(pDevice, EC_CMD_HOST_EVENT_GET_B, 0, NULL, 0, (UINT8*)&r, sizeof(r));
	if (!NT_SUCCESS(status)) {
		goto out;
	}

	if (r.mask & EC_HOST_EVENT_MASK(EC_HOST_EVENT_INVALID)) {
		goto out;
	}

	if (r.mask & mkbp_mask) {
		struct ec_params_host_event_mask p;
		p.mask = mkbp_mask;

		status = send_ec_command(pDevice, EC_CMD_HOST_EVENT_CLEAR_B, 0, (UINT8*)&p, sizeof(p), NULL, 0);
		if (!NT_SUCCESS(status)) {
			goto out;
		}

		struct ec_response_get_next_event event = { 0 };
		status = send_ec_command(pDevice, EC_CMD_GET_NEXT_EVENT, 0, NULL, 0, (UINT8*)&event, sizeof(event));
		if (!NT_SUCCESS(status)) {
			goto out;
		}

		if (event.event_type == EC_MKBP_EVENT_BUTTON) {
			if (pDevice->CSButtonsCallback) {
				CSVivaldiSettingsArg newArg;
				RtlZeroMemory(&newArg, sizeof(CSVivaldiSettingsArg));
				newArg.argSz = sizeof(CSVivaldiSettingsArg);
				newArg.settingsRequest = CSVivaldiRequestUpdateButton;
				newArg.args.button.button = (UINT8)event.data.buttons;
				ExNotifyCallback(pDevice->CSButtonsCallback, &newArg, NULL);
			}
		}
		return TRUE;
	}

out:
	return FALSE;
}

NTSTATUS CrosEcBusSleepEvent(
	PCROSECBUS_CONTEXT pDevice,
	UINT8 sleepEvent
) {
	if (!pDevice->hostSleepV1) {
		DbgPrint("Warning: EC does not support S0ix!\n");
		return STATUS_NOT_SUPPORTED;
	}

	struct ec_params_host_sleep_event_v1 req1 = { 0 };
	struct ec_response_host_sleep_event_v1 resp1 = { 0 };

	req1.sleep_event = sleepEvent;
	req1.suspend_params.sleep_timeout_ms = EC_HOST_SLEEP_TIMEOUT_DEFAULT;

	return send_ec_command(pDevice, EC_CMD_HOST_SLEEP_EVENT, 1, (UINT8 *)&req1, sizeof(req1), (UINT8 *)&resp1, sizeof(resp1));
}

VOID
CrosEcBusS0ixNotifyCallback(
	PCROSECBUS_CONTEXT pDevice,
	ULONG NotifyCode) {
	if (NotifyCode == 2 && pDevice->isInS0ix) {
		if (NT_SUCCESS(CrosEcBusSleepEvent(pDevice, HOST_SLEEP_EVENT_S0IX_RESUME))) {
			pDevice->isInS0ix = FALSE;
		}
	}
	else if (NotifyCode == 1 && !pDevice->isInS0ix) {
		if (NT_SUCCESS(CrosEcBusSleepEvent(pDevice, HOST_SLEEP_EVENT_S0IX_SUSPEND))) {
			pDevice->isInS0ix = TRUE;
		}
	}
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
		pnpCallbacks.EvtDeviceSelfManagedIoInit = OnSelfManagedIoInit;
		pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
		pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;

		WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);
	}

	{
		DECLARE_CONST_UNICODE_STRING(Name, NTDEVICE_NAME_STRING);
		status = WdfDeviceInitAssignName(DeviceInit,
			&Name
		);
		if (!NT_SUCCESS(status)) {
			CrosEcBusPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
				"WdfDeviceInitAssignName failed 0x%x\n", status);
			return status;
		}
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
		WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS IdleSettings;

		WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&IdleSettings, IdleCanWakeFromS0);
		IdleSettings.IdleTimeoutType = SystemManagedIdleTimeoutWithHint;
		IdleSettings.IdleTimeout = 1000;
		IdleSettings.UserControlOfIdleSettings = IdleDoNotAllowUserControl;

		WdfDeviceAssignS0IdleSettings(device, &IdleSettings);
	}

	{
		WDF_DEVICE_STATE deviceState;
		WDF_DEVICE_STATE_INIT(&deviceState);

		deviceState.NotDisableable = WdfFalse;
		WdfDeviceSetDeviceState(device, &deviceState);
	}

	status = CrosECQueueInitialize(device);
	if (!NT_SUCCESS(status)) {
		CrosEcBusPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"CrosECQueueInitialize failed 0x%x\n", status);
		return status;
	}

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

	devContext->KernelAccessesWaiting = 0;
	devContext->FxDevice = device;

	return status;
}
