#if !defined(_CROSECBUS_H_)
#define _CROSECBUS_H_

#pragma warning(disable:4200)  // suppress nameless struct/union warning
#pragma warning(disable:4201)  // suppress nameless struct/union warning
#pragma warning(disable:4214)  // suppress bit field types other than int warning
#include <initguid.h>
#include <wdm.h>

#pragma warning(default:4200)
#pragma warning(default:4201)
#pragma warning(default:4214)
#include <wdf.h>
#include <wdmguid.h>

#pragma warning(disable:4201)  // suppress nameless struct/union warning
#pragma warning(disable:4214)  // suppress bit field types other than int warning
#include <hidport.h>

#include <acpiioct.h>

//
// String definitions
//

#define DRIVERNAME                 "crosecbus.sys: "

#define CROSECBUS_POOL_TAG            (ULONG) 'CREC'
#define CROSECBUS_HARDWARE_IDS        L"CoolStar\\GOOG0004\0\0"
#define CROSECBUS_HARDWARE_IDS_LENGTH sizeof(CROSECBUS_HARDWARE_IDS)

#define NTDEVICE_NAME_STRING       L"\\Device\\CrosEC"
#define SYMBOLIC_NAME_STRING       L"\\DosDevices\\GOOG0004"

#define true 1
#define false 0

typedef struct _CROSEC_COMMAND {
    UINT32 Version;
    UINT32 Command;
    UINT32 OutSize;
    UINT32 InSize;
    UINT32 Result;
#pragma warning(disable:4200)
    UINT8 Data[];
} CROSEC_COMMAND, *PCROSEC_COMMAND;

typedef
NTSTATUS
(*PCROSEC_CMD_XFER_STATUS)(
    IN      PVOID Context,
    OUT     PCROSEC_COMMAND Msg
    );

typedef
BOOLEAN
(*PCROSEC_CHECK_FEATURES)(
    IN PVOID Context,
    IN INT Feature
    );

typedef
INT
(*PCROSEC_READ_MEM) (
    IN PVOID Context,
    IN INT offset,
    IN INT bytes,
    OUT PVOID dest
    );

DEFINE_GUID(GUID_CROSEC_INTERFACE_STANDARD,
    0xd7062676, 0xe3a4, 0x11ec, 0xa6, 0xc4, 0x24, 0x4b, 0xfe, 0x99, 0x46, 0xd0);

DEFINE_GUID(GUID_CROSEC_INTERFACE_STANDARD_V2,
    0xad8649fa, 0x7c71, 0x11ed, 0xb6, 0x3c, 0x00, 0x15, 0x5d, 0xa4, 0x49, 0xad);

typedef enum {
    CSVivaldiRequestUpdateButton = 0x101
} CSVivaldiRequest;

#include <pshpack1.h>
typedef struct CSVivaldiSettingsArg {
    UINT32 argSz;
    CSVivaldiRequest settingsRequest;
    union args {
        struct {
            UINT8 button;
        } button;
    } args;
} CSVivaldiSettingsArg, * PCSVivaldiSettingsArg;
#include <poppack.h>

//
// Interface for getting and setting power level etc.,
//
typedef struct _CROSEC_INTERFACE_STANDARD {
    INTERFACE                        InterfaceHeader;
    PCROSEC_CMD_XFER_STATUS          CmdXferStatus;
    PCROSEC_CHECK_FEATURES           CheckFeatures;
} CROSEC_INTERFACE_STANDARD, * PCROSEC_INTERFACE_STANDARD;

typedef struct _CROSEC_INTERFACE_STANDARD_V2 {
    INTERFACE                        InterfaceHeader;
    PCROSEC_CMD_XFER_STATUS          CmdXferStatus;
    PCROSEC_CHECK_FEATURES           CheckFeatures;
    PCROSEC_READ_MEM                 ReadEcMem;
} CROSEC_INTERFACE_STANDARD_V2, * PCROSEC_INTERFACE_STANDARD_V2;

typedef struct _CROSECBUS_CONTEXT
{

	//
	// Handle back to the WDFDEVICE
	//

	WDFDEVICE FxDevice;

    UINT32 EcFeatures[2];

    LONG64 KernelAccessesWaiting;
    WDFWAITLOCK EcLock;

    WDFINTERRUPT Interrupt;
    BOOLEAN FoundSyncGPIO;
    PCALLBACK_OBJECT CSButtonsCallback;

    //S0IX Notify
    ACPI_INTERFACE_STANDARD2 S0ixNotifyAcpiInterface;
    BOOLEAN isInS0ix;
    BOOLEAN hostSleepV1;

} CROSECBUS_CONTEXT, *PCROSECBUS_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CROSECBUS_CONTEXT, GetDeviceContext)

//
// Function definitions
//

DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_UNLOAD CrosEcBusDriverUnload;

EVT_WDF_DRIVER_DEVICE_ADD CrosEcBusEvtDeviceAdd;

EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL CrosEcBusEvtInternalDeviceControl;

//
// Helper macros
//

#define DEBUG_LEVEL_ERROR   1
#define DEBUG_LEVEL_INFO    2
#define DEBUG_LEVEL_VERBOSE 3

#define DBG_INIT  1
#define DBG_PNP   2
#define DBG_IOCTL 4

#if 0
#define CrosEcBusPrint(dbglevel, dbgcatagory, fmt, ...) {          \
    if (CrosEcBusDebugLevel >= dbglevel &&                         \
        (CrosEcBusDebugCatagories && dbgcatagory))                 \
		    {                                                           \
        DbgPrint(DRIVERNAME);                                   \
        DbgPrint(fmt, __VA_ARGS__);                             \
		    }                                                           \
}
#else
#define CrosEcBusPrint(dbglevel, fmt, ...) {                       \
}
#endif
#endif