/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */ 

#include "driver.h"
#include "fdo.h"
#include "pdo.h"
#include "srbext.h"
#include "buffer.h"
#include "debug.h"
#include "assert.h"
#include "util.h"
#include <version.h>
#include <names.h>
#include <xencrsh_interface.h>
#include <xenvbd-ntstrsafe.h>

#define IS_NULL         ((ULONG)'llun')
#define IS_FDO          ((ULONG)'odf')
#define IS_PDO          ((ULONG)'odp')
//=============================================================================
XENVBD_PARAMETERS   DriverParameters;
HANDLE              DriverStatusKey;

#define XENVBD_POOL_TAG     'dbvX'

static FORCEINLINE BOOLEAN
__IsValid(
    __in WCHAR                  Char
    )
{
    return !(Char == 0 || Char == L' ' || Char == L'\t' || Char == L'\n' || Char == L'\r');
}
static DECLSPEC_NOINLINE BOOLEAN
__DriverGetOption(
    __in PWCHAR                 Options,
    __in PWCHAR                 Parameter,
    __out PWCHAR*               Value
    )
{
    PWCHAR  Ptr;
    PWCHAR  Buffer;
    ULONG   Index;
    ULONG   Length;

    *Value = NULL;
    Ptr = wcsstr(Options, Parameter);
    if (Ptr == NULL)
        return FALSE; // option not present

    // skip Parameter
    while (*Parameter) {
        ++Ptr;
        ++Parameter;
    }

    // find length of Value, up to next NULL or whitespace
    for (Length = 0; __IsValid(Ptr[Length]); ++Length) 
        ;
    if (Length == 0)
        return TRUE; // found the option, it had no value so *Value == NULL!

    Buffer = (PWCHAR)__AllocateNonPagedPoolWithTag(__FUNCTION__, __LINE__, (Length + 1) * sizeof(WCHAR), XENVBD_POOL_TAG);
    if (Buffer == NULL)
        return FALSE; // memory allocation failure, ignore option

    // copy Value
    for (Index = 0; Index < Length; ++Index)
        Buffer[Index] = Ptr[Index];
    Buffer[Length] = L'\0';

    *Value = Buffer;
    return TRUE;
}
static DECLSPEC_NOINLINE NTSTATUS
__DriverGetSystemStartParams(
    __out PWCHAR*               Options
    )
{
    UNICODE_STRING      Unicode;
    OBJECT_ATTRIBUTES   Attributes;
    HANDLE              Key;
    PKEY_VALUE_PARTIAL_INFORMATION  Value;
    ULONG               Size;
    NTSTATUS            Status;

    RtlInitUnicodeString(&Unicode, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control");
    InitializeObjectAttributes(&Attributes, &Unicode, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    Status = ZwOpenKey(&Key, KEY_READ, &Attributes);
    if (!NT_SUCCESS(Status))
        goto fail1;

    RtlInitUnicodeString(&Unicode, L"SystemStartOptions");
    Status = ZwQueryValueKey(Key, &Unicode, KeyValuePartialInformation, NULL, 0, &Size);
    if (Status != STATUS_BUFFER_TOO_SMALL &&
        Status != STATUS_BUFFER_OVERFLOW)
        goto fail2;

    Status = STATUS_NO_MEMORY;
#pragma prefast(suppress:6102)
    Value = (PKEY_VALUE_PARTIAL_INFORMATION)__AllocateNonPagedPoolWithTag(__FUNCTION__, __LINE__, Size, XENVBD_POOL_TAG);
    if (Value == NULL)
        goto fail3;

    Status = ZwQueryValueKey(Key, &Unicode, KeyValuePartialInformation, Value, Size, &Size);
    if (!NT_SUCCESS(Status))
        goto fail4;

    Status = STATUS_INVALID_PARAMETER;
    if (Value->Type != REG_SZ)
        goto fail5;

    Status = STATUS_NO_MEMORY;
    *Options = (PWCHAR)__AllocateNonPagedPoolWithTag(__FUNCTION__, __LINE__, Value->DataLength + sizeof(WCHAR), XENVBD_POOL_TAG);
    if (*Options == NULL)
        goto fail6;

    RtlCopyMemory(*Options, Value->Data, Value->DataLength);

    __FreePoolWithTag(Value, XENVBD_POOL_TAG);

    ZwClose(Key);
    return STATUS_SUCCESS;

fail6:
fail5:
fail4:
    __FreePoolWithTag(Value, XENVBD_POOL_TAG);
fail3:
fail2:
    ZwClose(Key);
fail1:
    *Options = NULL;
    return Status;
}
static DECLSPEC_NOINLINE VOID
__DriverParseParameterKey(
    )
{
    NTSTATUS    Status;
    PWCHAR      Options;
    PWCHAR      Value;

    // Set default parameters
    DriverParameters.SynthesizeInquiry = FALSE;
    DriverParameters.PVCDRom           = FALSE;

    // attempt to read registry for system start parameters
    Status = __DriverGetSystemStartParams(&Options);
    if (NT_SUCCESS(Status)) {
        Trace("Options = \"%ws\"\n", Options);

        // check each option
        if (__DriverGetOption(Options, L"XENVBD:SYNTH_INQ=", &Value)) {
            // Value may be NULL (it shouldnt be though!)
            if (Value) {
                if (wcscmp(Value, L"ON") == 0) {
                    DriverParameters.SynthesizeInquiry = TRUE;
                }
                __FreePoolWithTag(Value, XENVBD_POOL_TAG);
            }
        }

        if (__DriverGetOption(Options, L"XENVBD:PVCDROM=", &Value)) {
            // Value may be NULL (it shouldnt be though!)
            if (Value) {
                if (wcscmp(Value, L"ON") == 0) {
                    DriverParameters.PVCDRom = TRUE;
                }
                __FreePoolWithTag(Value, XENVBD_POOL_TAG);
            }
        }

        __FreePoolWithTag(Options, XENVBD_POOL_TAG);
    }

    Verbose("DriverParameters: %s%s\n", 
            DriverParameters.SynthesizeInquiry ? "SYNTH_INQ " : "",
            DriverParameters.PVCDRom ? "PV_CDROM " : "");
}

//=============================================================================
static PDRIVER_DISPATCH     StorPortDispatchPnp;
static PDRIVER_DISPATCH     StorPortDispatchPower;
static PDRIVER_UNLOAD       StorPortDriverUnload;

NTSTATUS
DriverDispatchPnp(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp
    )
{
    return StorPortDispatchPnp(DeviceObject, Irp);
}

NTSTATUS
DriverDispatchPower(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp
    )
{
    return StorPortDispatchPower(DeviceObject, Irp);
}

//=============================================================================
// Fdo Device Extension management
static PXENVBD_FDO              __XenvbdFdo;
static KSPIN_LOCK               __XenvbdLock;

VOID
DriverLinkFdo(
    __in PXENVBD_FDO             Fdo
    )
{
    KIRQL       Irql;

    KeAcquireSpinLock(&__XenvbdLock, &Irql);
    __XenvbdFdo = Fdo;
    KeReleaseSpinLock(&__XenvbdLock, Irql);
}

VOID
DriverUnlinkFdo(
    __in PXENVBD_FDO             Fdo
    )
{
    KIRQL       Irql;

    UNREFERENCED_PARAMETER(Fdo);

    KeAcquireSpinLock(&__XenvbdLock, &Irql);
    __XenvbdFdo = NULL;
    KeReleaseSpinLock(&__XenvbdLock, Irql);
}

__checkReturn
static FORCEINLINE ULONG
DriverGetFdoOrPdo(
    __in PDEVICE_OBJECT          DeviceObject,
    __out PXENVBD_FDO*           _Fdo,
    __out PXENVBD_PDO*           _Pdo
    )
{
    KIRQL       Irql;
    ULONG       Result = IS_NULL;
    
    *_Fdo = NULL;
    *_Pdo = NULL;

    KeAcquireSpinLock(&__XenvbdLock, &Irql);
    if (__XenvbdFdo) {
        PXENVBD_FDO Fdo = __XenvbdFdo;
        if (FdoReference(Fdo) > 0) {
            if (FdoGetDeviceObject(Fdo) == DeviceObject) {
                *_Fdo = Fdo;
                Result = IS_FDO;
            } else {
                KeReleaseSpinLock(&__XenvbdLock, Irql);

                *_Pdo = FdoGetPdoFromDeviceObject(Fdo, DeviceObject);
                FdoDereference(Fdo);
                return IS_PDO;
            }
        }
    }
    KeReleaseSpinLock(&__XenvbdLock, Irql);

    return Result;
}
__checkReturn
static FORCEINLINE NTSTATUS
DriverMapPdo(
    __in PDEVICE_OBJECT          DeviceObject, 
    __in PIRP                    Irp
    )
{
    KIRQL       Irql;
    NTSTATUS    Status;

    KeAcquireSpinLock(&__XenvbdLock, &Irql);
    if (__XenvbdFdo && FdoGetDeviceObject(__XenvbdFdo) != DeviceObject) {
        PXENVBD_FDO Fdo = __XenvbdFdo;
        if (FdoReference(Fdo) > 0) {
            KeReleaseSpinLock(&__XenvbdLock, Irql);
            Status = FdoMapDeviceObjectToPdo(Fdo, DeviceObject, Irp);
            FdoDereference(Fdo);
            goto done;
        }
    }
    KeReleaseSpinLock(&__XenvbdLock, Irql);
    Status = DriverDispatchPnp(DeviceObject, Irp);

done:
    return Status;
}

VOID
DriverNotifyInstaller(
    VOID
    )
{
    UNICODE_STRING                  Unicode;
    PKEY_VALUE_PARTIAL_INFORMATION  Partial;
    NTSTATUS                        status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    Partial = __AllocateNonPagedPoolWithTag(__FUNCTION__,
                                            __LINE__,
                                            FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) +
                                            sizeof (ULONG),
                                            XENVBD_POOL_TAG);
    status = STATUS_NO_MEMORY;
    if (Partial == NULL)
        goto fail1;

    Partial->TitleIndex = 0;
    Partial->Type = REG_DWORD;
    Partial->DataLength = sizeof (ULONG);
    *(PULONG)Partial->Data = 1;

    RtlInitUnicodeString(&Unicode, L"NeedReboot");

    status = ZwSetValueKey(DriverStatusKey,
                           &Unicode,
                           Partial->TitleIndex,
                           Partial->Type,
                           Partial->Data,
                           Partial->DataLength);
    if (!NT_SUCCESS(status))
        goto fail2;

    __FreePoolWithTag(Partial, XENVBD_POOL_TAG);

    return;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);
}

__checkReturn
__drv_allocatesMem(mem)
static FORCEINLINE PCHAR
#pragma warning(suppress: 28195)
__DriverFormatV(
    __in PCHAR       Fmt,
    __in va_list     Args
    )
{
    NTSTATUS    Status;
    PCHAR       Str;
    ULONG       Size = 32;

    for (;;) {
        Str = (PCHAR)__AllocateNonPagedPoolWithTag(__FUNCTION__, __LINE__, Size, XENVBD_POOL_TAG);
        if (!Str) {
            return NULL;
        }

        Status = RtlStringCchVPrintfA(Str, Size - 1, Fmt, Args);

        if (Status == STATUS_SUCCESS) {
            Str[Size - 1] = '\0';
            return Str;
        } 
        
        __FreePoolWithTag(Str, XENVBD_POOL_TAG);
        if (Status == STATUS_BUFFER_OVERFLOW) {
            Size *= 2;
        } else {
            return NULL;
        }
    }
}

__checkReturn
__drv_allocatesMem(mem)
PCHAR
DriverFormat(
    __in PCHAR       Format,
    ...
    )
{
    va_list Args;
    PCHAR   Str;

    va_start(Args, Format);
    Str = __DriverFormatV(Format, Args);
    va_end(Args);
    return Str;
}

VOID
#pragma warning(suppress: 28197)
DriverFormatFree(
    __in __drv_freesMem(mem) PCHAR  Buffer
    )
{
    if (Buffer)
        __FreePoolWithTag(Buffer, XENVBD_POOL_TAG);
}

//=============================================================================
// StorPort redirections
static FORCEINLINE PCHAR
__ScsiAdapterControlTypeName(
    __in SCSI_ADAPTER_CONTROL_TYPE   ControlType
    )
{
    switch (ControlType) {
    case ScsiQuerySupportedControlTypes:    return "QuerySupportedControlTypes";
    case ScsiStopAdapter:                   return "StopAdapter";
    case ScsiRestartAdapter:                return "RestartAdapter";
    case ScsiSetBootConfig:                 return "SetBootConfig";
    case ScsiSetRunningConfig:              return "SetRunningConfig";
    default:                                return "UNKNOWN";
    }
}

static FORCEINLINE PCHAR
__ScsiAdapterControlStatus(
    __in SCSI_ADAPTER_CONTROL_STATUS Status
    )
{
    switch (Status) {
    case ScsiAdapterControlSuccess:         return "Success";
    case ScsiAdapterControlUnsuccessful:    return "Unsuccessful";
    default:                                return "UNKNOWN";
    }
}

HW_INITIALIZE       HwInitialize;

BOOLEAN 
HwInitialize(
    __in PVOID   HwDeviceExtension
    )
{
    Trace("(0x%p) @%d <---> TRUE\n", HwDeviceExtension, KeGetCurrentIrql());
    return TRUE;
}

HW_INTERRUPT        HwInterrupt;

BOOLEAN 
HwInterrupt(
    __in PVOID   HwDeviceExtension
    )
{
    UNREFERENCED_PARAMETER(HwDeviceExtension);
    return TRUE;
}

HW_RESET_BUS        HwResetBus;

BOOLEAN 
HwResetBus(
    __in PVOID   HwDeviceExtension,
    __in ULONG   PathId
    )
{
    BOOLEAN RetVal;
    Trace("(0x%p, %d) @%d --->\n", HwDeviceExtension, PathId, KeGetCurrentIrql());
    RetVal = FdoResetBus((PXENVBD_FDO)HwDeviceExtension);
    Trace("(0x%p, %d) @%d <--- %s\n", HwDeviceExtension, PathId, KeGetCurrentIrql(), RetVal ? "TRUE" : "FALSE");
    return RetVal;
}

HW_ADAPTER_CONTROL  HwAdapterControl;

SCSI_ADAPTER_CONTROL_STATUS
HwAdapterControl(
    __in PVOID                       HwDeviceExtension,
    __in SCSI_ADAPTER_CONTROL_TYPE   ControlType,
    __in PVOID                       Parameters
    )
{
    SCSI_ADAPTER_CONTROL_STATUS RetVal;
    Trace("(0x%p, %s, 0x%p) @%d --->\n", HwDeviceExtension, __ScsiAdapterControlTypeName(ControlType), Parameters, KeGetCurrentIrql());
    RetVal = FdoAdapterControl((PXENVBD_FDO)HwDeviceExtension, ControlType, Parameters);
    Trace("(0x%p, %s, 0x%p) @%d <--- %s\n", HwDeviceExtension, __ScsiAdapterControlTypeName(ControlType), Parameters, KeGetCurrentIrql(), __ScsiAdapterControlStatus(RetVal));
    return RetVal;
}

HW_FIND_ADAPTER     HwFindAdapter;

ULONG
HwFindAdapter(
    IN PVOID                               HwDeviceExtension,
    IN PVOID                               Context,
    IN PVOID                               BusInformation,
    IN PCHAR                               ArgumentString,
    IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    OUT PBOOLEAN                           Again
    )
{
    ULONG RetVal;
    Trace("(0x%p, 0x%p, 0x%p, %s, 0x%p, 0x%p) @%d --->\n", HwDeviceExtension, 
                Context, BusInformation, ArgumentString, ConfigInfo, Again, KeGetCurrentIrql());
    RetVal = FdoFindAdapter((PXENVBD_FDO)HwDeviceExtension, ConfigInfo);
    Trace("(0x%p, 0x%p, 0x%p, %s, 0x%p, 0x%p) @%d <--- %d\n", HwDeviceExtension, 
                Context, BusInformation, ArgumentString, ConfigInfo, Again, KeGetCurrentIrql(), RetVal);
    return RetVal;
}

static FORCEINLINE BOOLEAN
__FailStorageRequest(
    __in PVOID               HwDeviceExtension,
    __in PSCSI_REQUEST_BLOCK Srb
    )
{
    if (Srb->Function == SRB_FUNCTION_STORAGE_REQUEST_BLOCK) {
        // Win8 and above storport request. not supported
        // complete the request (with fail code)
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        StorPortNotification(RequestComplete, HwDeviceExtension, Srb);
        Error("(0x%p) STORAGE_REQUEST_BLOCK not supported\n", HwDeviceExtension);
        return TRUE;
    }

    return FALSE;
}

HW_BUILDIO          HwBuildIo;

BOOLEAN 
HwBuildIo(
    __in PVOID               HwDeviceExtension,
    __in PSCSI_REQUEST_BLOCK Srb
    )
{
    if (__FailStorageRequest(HwDeviceExtension, Srb))
        return FALSE; // dont pass to HwStartIo

    return FdoBuildIo((PXENVBD_FDO)HwDeviceExtension, Srb);
}

HW_STARTIO          HwStartIo;

BOOLEAN 
HwStartIo(
    __in PVOID               HwDeviceExtension,
    __in PSCSI_REQUEST_BLOCK Srb
    )
{
    if (__FailStorageRequest(HwDeviceExtension, Srb))
        return TRUE; // acknowledge the srb

    return FdoStartIo((PXENVBD_FDO)HwDeviceExtension, Srb);
}

//=============================================================================
// Driver Redirections

__drv_dispatchType(IRP_MJ_PNP)
DRIVER_DISPATCH             DispatchPnp;

NTSTATUS 
DispatchPnp(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
    )
{
    NTSTATUS            Status;
    ULONG               IsFdo;
    PXENVBD_FDO         Fdo;
    PXENVBD_PDO         Pdo;

    IsFdo = DriverGetFdoOrPdo(DeviceObject, &Fdo, &Pdo);

    switch (IsFdo) {
    case IS_FDO:
        Status = FdoDispatchPnp(Fdo, DeviceObject, Irp); // drops Fdo reference
        break;

    case IS_PDO:
        if (Pdo) {
            Status = PdoDispatchPnp(Pdo, DeviceObject, Irp); // drops Pdo reference
        } else {
            Status = DriverMapPdo(DeviceObject, Irp);
        }
        break;

    case IS_NULL:
    default:
        Warning("DeviceObject 0x%p is not FDO (0x%p) or a PDO\n", DeviceObject, __XenvbdFdo);
        Status = DriverDispatchPnp(DeviceObject, Irp);
        break;
    }

    return Status;
}

__drv_dispatchType(IRP_MJ_POWER)
DRIVER_DISPATCH             DispatchPower;

NTSTATUS 
DispatchPower(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
    )
{
    NTSTATUS            Status;
    ULONG               IsFdo;
    PXENVBD_FDO         Fdo;
    PXENVBD_PDO         Pdo;

    IsFdo = DriverGetFdoOrPdo(DeviceObject, &Fdo, &Pdo);

    switch (IsFdo) {
    case IS_FDO:
        ASSERT3P(Fdo, !=, NULL);
        ASSERT3P(Pdo, ==, NULL);
        Status = FdoDispatchPower(Fdo, DeviceObject, Irp); // drops Fdo reference
        break;

    case IS_PDO:
        if (Pdo) {
            PdoDereference(Pdo); // drops Pdo reference
        }
        Status = DriverDispatchPower(DeviceObject, Irp);
        break;

    case IS_NULL:
    default:
        Warning("DeviceObject 0x%p is not FDO (0x%p) or a PDO\n", DeviceObject, __XenvbdFdo);
        Status = DriverDispatchPower(DeviceObject, Irp);
        break;
    }

    return Status;
}

DRIVER_UNLOAD               DriverUnload;

VOID
DriverUnload(
    IN PDRIVER_OBJECT  _DriverObject
    )
{
    Trace("===> (Irql=%d)\n", KeGetCurrentIrql());
    Verbose("%s (%s)\n",
         MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
         DAY_STR "/" MONTH_STR "/" YEAR_STR);
    StorPortDriverUnload(_DriverObject);
    BufferTerminate();
    ZwClose(DriverStatusKey);
    Trace("<=== (Irql=%d)\n", KeGetCurrentIrql());
}

DRIVER_INITIALIZE           DriverEntry;

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT  _DriverObject,
    IN PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS                Status;
    OBJECT_ATTRIBUTES       Attributes;
    UNICODE_STRING          Unicode;
    HW_INITIALIZATION_DATA  InitData;
    HANDLE                  ServiceKey;

    // RegistryPath == NULL if crashing!
    if (RegistryPath == NULL) {
        return XencrshEntryPoint(_DriverObject);
    }

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
    
    Trace("===> (Irql=%d)\n", KeGetCurrentIrql());
    Verbose("%s (%s)\n",
         MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
         DAY_STR "/" MONTH_STR "/" YEAR_STR);

    InitializeObjectAttributes(&Attributes,
                               RegistryPath,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    Status = ZwOpenKey(&ServiceKey,
                       KEY_ALL_ACCESS,
                       &Attributes);
    if (!NT_SUCCESS(Status))
        goto done;

    RtlInitUnicodeString(&Unicode, L"Status");

    InitializeObjectAttributes(&Attributes,
                               &Unicode,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               ServiceKey,
                               NULL);

    Status = ZwCreateKey(&DriverStatusKey,
                         KEY_ALL_ACCESS,
                         &Attributes,
                         0,
                         NULL,
                         REG_OPTION_VOLATILE,
                         NULL
                         );

    ZwClose(ServiceKey);

    if (!NT_SUCCESS(Status))
        goto done;

    KeInitializeSpinLock(&__XenvbdLock);
    __XenvbdFdo = NULL;
    BufferInitialize();
    __DriverParseParameterKey();

    RtlZeroMemory(&InitData, sizeof(InitData));

    InitData.HwInitializationDataSize   =   sizeof(InitData);
    InitData.AdapterInterfaceType       =   Internal;
    InitData.HwInitialize               =   HwInitialize;
    InitData.HwStartIo                  =   HwStartIo;
    InitData.HwInterrupt                =   HwInterrupt;
#pragma warning(suppress : 4152)
    InitData.HwFindAdapter              =   HwFindAdapter;
    InitData.HwResetBus                 =   HwResetBus;
    InitData.HwDmaStarted               =   NULL;
    InitData.HwAdapterState             =   NULL;
    InitData.DeviceExtensionSize        =   FdoSizeofXenvbdFdo();
    InitData.SpecificLuExtensionSize    =   sizeof (ULONG); // not actually used
    InitData.SrbExtensionSize           =   sizeof(XENVBD_SRBEXT);
    InitData.NumberOfAccessRanges       =   2;
    InitData.MapBuffers                 =   STOR_MAP_NON_READ_WRITE_BUFFERS;
    InitData.NeedPhysicalAddresses      =   TRUE;
    InitData.TaggedQueuing              =   TRUE;
    InitData.AutoRequestSense           =   TRUE;
    InitData.MultipleRequestPerLu       =   TRUE;
    InitData.HwAdapterControl           =   HwAdapterControl;
    InitData.HwBuildIo                  =   HwBuildIo;

    Status = StorPortInitialize(_DriverObject, RegistryPath, &InitData, NULL);
    if (NT_SUCCESS(Status)) {
        StorPortDispatchPnp     = _DriverObject->MajorFunction[IRP_MJ_PNP];
        StorPortDispatchPower   = _DriverObject->MajorFunction[IRP_MJ_POWER];
        StorPortDriverUnload    = _DriverObject->DriverUnload;

        _DriverObject->MajorFunction[IRP_MJ_PNP]    = DispatchPnp;
        _DriverObject->MajorFunction[IRP_MJ_POWER]  = DispatchPower;
        _DriverObject->DriverUnload                 = DriverUnload;
    }

done:
    Trace("<=== (%08x) (Irql=%d)\n", Status, KeGetCurrentIrql());
    return Status;
}
