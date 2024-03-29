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

#include <ntddk.h>

#include "util.h"
#include "registry.h"
#include "assert.h"

#define REGISTRY_POOL 'GERX'

static UNICODE_STRING   RegistryPath;

static FORCEINLINE PVOID
__RegistryAllocate(
    IN  ULONG   Length
    )
{
    return __AllocateNonPagedPoolWithTag(__FUNCTION__,
                                         __LINE__,
                                         Length,
                                         REGISTRY_POOL);
}

static FORCEINLINE VOID
__RegistryFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, REGISTRY_POOL);
}

NTSTATUS
RegistryInitialize(
    IN PUNICODE_STRING  Path
    )
{
    NTSTATUS            status;

    ASSERT3P(RegistryPath.Buffer, ==, NULL);

    status = RtlUpcaseUnicodeString(&RegistryPath, Path, TRUE);
    if (!NT_SUCCESS(status))
        goto fail1;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
RegistryTeardown(
    VOID
    )
{
    RtlFreeUnicodeString(&RegistryPath);
    RegistryPath.Buffer = NULL;
    RegistryPath.MaximumLength = RegistryPath.Length = 0;
}

NTSTATUS
RegistryOpenKey(
    IN  HANDLE          Parent,
    IN  PUNICODE_STRING Path,
    IN  ACCESS_MASK     DesiredAccess,
    OUT PHANDLE         Key
    )
{
    OBJECT_ATTRIBUTES   Attributes;
    NTSTATUS            status;

    InitializeObjectAttributes(&Attributes,
                               Path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               Parent,
                               NULL);

    status = ZwOpenKey(Key,
                       DesiredAccess,
                       &Attributes);
    if (!NT_SUCCESS(status))
        goto fail1;

    return STATUS_SUCCESS;

fail1:
    return status;
}

NTSTATUS
RegistryOpenServiceKey(
    IN  ACCESS_MASK     DesiredAccess,
    OUT PHANDLE         Key
    )
{
    return RegistryOpenKey(NULL, &RegistryPath, DesiredAccess, Key);
}

NTSTATUS
RegistryOpenSoftwareKey(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  ACCESS_MASK     DesiredAccess,
    OUT PHANDLE         Key
    )
{
    NTSTATUS            status;

    status = IoOpenDeviceRegistryKey(DeviceObject,
                                     PLUGPLAY_REGKEY_DRIVER,
                                     DesiredAccess,
                                     Key);
    if (!NT_SUCCESS(status))
        goto fail1;

    return STATUS_SUCCESS;

fail1:
    return status;
}

NTSTATUS
RegistryOpenHardwareKey(
    IN  PDEVICE_OBJECT      DeviceObject,
    IN  ACCESS_MASK         DesiredAccess,
    OUT PHANDLE             Key
    )
{
    HANDLE                  SubKey;
    ULONG                   Length;
    PKEY_NAME_INFORMATION   Info;
    PWCHAR                  Cursor;
    UNICODE_STRING          Unicode;
    NTSTATUS                status;

    status = IoOpenDeviceRegistryKey(DeviceObject,
                                     PLUGPLAY_REGKEY_DEVICE,
                                     KEY_READ,
                                     &SubKey);
    if (!NT_SUCCESS(status))
        goto fail1;

    Length = 0;
    status = ZwQueryKey(SubKey,
                        KeyNameInformation,
                        NULL,
                        0,
                        &Length);
    if (status != STATUS_BUFFER_OVERFLOW &&
        status != STATUS_BUFFER_TOO_SMALL)
        goto fail2;

#pragma prefast(suppress:6102)
    Info = __RegistryAllocate(Length + sizeof (WCHAR));

    status = STATUS_NO_MEMORY;
    if (Info == NULL)
        goto fail3;

    status = ZwQueryKey(SubKey,
                        KeyNameInformation,
                        Info,
                        Length,
                        &Length);
    if (!NT_SUCCESS(status))
        goto fail4;

    Info->Name[Info->NameLength / sizeof (WCHAR)] = '\0';

    Cursor = wcsrchr(Info->Name, L'\\');
    ASSERT(Cursor != NULL);

    *Cursor = L'\0';
    
    RtlInitUnicodeString(&Unicode, Info->Name);

    status = RegistryOpenKey(NULL, &Unicode, DesiredAccess, Key);
    if (!NT_SUCCESS(status))
        goto fail5;

    __RegistryFree(Info);

    RegistryCloseKey(SubKey);

    return STATUS_SUCCESS;

fail5:
fail4:
    __RegistryFree(Info);

fail3:
fail2:
    RegistryCloseKey(SubKey);

fail1:
    return status;
}

NTSTATUS
RegistryOpenSubKey(
    IN  PHANDLE         Key,
    IN  PCHAR           Name,
    IN  ACCESS_MASK     DesiredAccess,
    OUT PHANDLE         SubKey
    )
{
    ANSI_STRING         Ansi;
    UNICODE_STRING      Unicode;
    NTSTATUS            status;

    RtlInitAnsiString(&Ansi, Name);

    status = RtlAnsiStringToUnicodeString(&Unicode, &Ansi, TRUE);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryOpenKey(Key, &Unicode, DesiredAccess, SubKey);
    if (!NT_SUCCESS(status))
        goto fail2;

    RtlFreeUnicodeString(&Unicode);

    return STATUS_SUCCESS;

fail2:
    RtlFreeUnicodeString(&Unicode);

fail1:
    return status;
}

NTSTATUS
RegistryCreateSubKey(
    IN  PHANDLE         Key,
    IN  PCHAR           Name,
    IN  ULONG           Options,
    OUT PHANDLE         SubKey
    )
{
    ANSI_STRING         Ansi;
    UNICODE_STRING      Unicode;
    OBJECT_ATTRIBUTES   Attributes;
    NTSTATUS            status;

    RtlInitAnsiString(&Ansi, Name);

    status = RtlAnsiStringToUnicodeString(&Unicode, &Ansi, TRUE);
    if (!NT_SUCCESS(status))
        goto fail1;

    InitializeObjectAttributes(&Attributes,
                               &Unicode,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               Key,
                               NULL);

    status = ZwCreateKey(SubKey,
                         KEY_ALL_ACCESS,
                         &Attributes,
                         0,
                         NULL,
                         Options,
                         NULL
                         );
    if (!NT_SUCCESS(status))
        goto fail2;

    RtlFreeUnicodeString(&Unicode);

    return STATUS_SUCCESS;

fail2:
    RtlFreeUnicodeString(&Unicode);

fail1:
    return status;
}

NTSTATUS
RegistryDeleteSubKey(
    IN  PHANDLE         Key,
    IN  PCHAR           Name
    )
{
    ANSI_STRING         Ansi;
    UNICODE_STRING      Unicode;
    HANDLE              SubKey;
    NTSTATUS            status;

    RtlInitAnsiString(&Ansi, Name);

    status = RtlAnsiStringToUnicodeString(&Unicode, &Ansi, TRUE);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryOpenKey(Key, &Unicode, KEY_ALL_ACCESS, &SubKey);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = ZwDeleteKey(SubKey);
    if (!NT_SUCCESS(status))
        goto fail3;

    ZwClose(SubKey);

    RtlFreeUnicodeString(&Unicode);

    return STATUS_SUCCESS;

fail3:
    ZwClose(SubKey);

fail2:
    RtlFreeUnicodeString(&Unicode);

fail1:
    return status;
}

NTSTATUS
RegistryEnumerateSubKeys(
    IN  HANDLE              Key,
    IN  NTSTATUS            (*Callback)(PVOID, HANDLE, PCHAR),
    IN  PVOID               Context
    )
{
    ULONG                   Size;
    NTSTATUS                status;
    PKEY_FULL_INFORMATION   Full;
    PKEY_BASIC_INFORMATION  Basic;
    ULONG                   Index;

    status = ZwQueryKey(Key,
                        KeyFullInformation,
                        NULL,
                        0,
                        &Size);
    if (status != STATUS_BUFFER_OVERFLOW &&
        status != STATUS_BUFFER_TOO_SMALL)
        goto fail1;

#pragma prefast(suppress:6102)
    Full = __RegistryAllocate(Size);

    status = STATUS_NO_MEMORY;
    if (Full == NULL)
        goto fail2;

    status = ZwQueryKey(Key,
                        KeyFullInformation,
                        Full,
                        Size,
                        &Size);
    if (!NT_SUCCESS(status))
        goto fail3;

    Size = FIELD_OFFSET(KEY_BASIC_INFORMATION, Name) +
           Full->MaxNameLen;

    Basic = __RegistryAllocate(Size);
    status = STATUS_NO_MEMORY;
    if (Basic == NULL)
        goto fail4;

    for (Index = 0; Index < Full->SubKeys; Index++) {
        UNICODE_STRING  Unicode;
        ANSI_STRING     Ansi;

        status = ZwEnumerateKey(Key,
                                Index,
                                KeyBasicInformation,
                                Basic,
                                Size,
                                &Size);
        if (!NT_SUCCESS(status))
            goto fail5;

        Unicode.MaximumLength = (USHORT)Basic->NameLength;
        Unicode.Buffer = Basic->Name;
        Unicode.Length = (USHORT)Basic->NameLength;

        Ansi.MaximumLength = (USHORT)((Basic->NameLength / sizeof (WCHAR)) + sizeof (CHAR));
        Ansi.Buffer = __RegistryAllocate(Ansi.MaximumLength);

        status = STATUS_NO_MEMORY;
        if (Ansi.Buffer == NULL)
            goto fail6;

        status = RtlUnicodeStringToAnsiString(&Ansi, &Unicode, FALSE);
        ASSERT(NT_SUCCESS(status));

        Ansi.Length = (USHORT)(strlen(Ansi.Buffer) * sizeof (CHAR));        

        status = Callback(Context, Key, Ansi.Buffer);

        __RegistryFree(Ansi.Buffer);
        Ansi.Buffer = NULL;

        if (!NT_SUCCESS(status))
            goto fail7;
    }

    __RegistryFree(Basic);

    __RegistryFree(Full);

    return STATUS_SUCCESS;

fail7:
fail6:
fail5:
    __RegistryFree(Basic);

fail4:
fail3:
    __RegistryFree(Full);
    
fail2:
fail1:
    return status;
}

NTSTATUS
RegistryEnumerateValues(
    IN  HANDLE                      Key,
    IN  NTSTATUS                    (*Callback)(PVOID, HANDLE, PCHAR),
    IN  PVOID                       Context
    )
{
    ULONG                           Size;
    NTSTATUS                        status;
    PKEY_FULL_INFORMATION           Full;
    PKEY_VALUE_BASIC_INFORMATION    Basic;
    ULONG                           Index;

    status = ZwQueryKey(Key,
                        KeyFullInformation,
                        NULL,
                        0,
                        &Size);
    if (status != STATUS_BUFFER_OVERFLOW &&
        status != STATUS_BUFFER_TOO_SMALL)
        goto fail1;

#pragma prefast(suppress:6102)
    Full = __RegistryAllocate(Size);

    status = STATUS_NO_MEMORY;
    if (Full == NULL)
        goto fail2;

    status = ZwQueryKey(Key,
                        KeyFullInformation,
                        Full,
                        Size,
                        &Size);
    if (!NT_SUCCESS(status))
        goto fail3;

    Size = FIELD_OFFSET(KEY_VALUE_BASIC_INFORMATION, Name) +
           Full->MaxValueNameLen;

    Basic = __RegistryAllocate(Size);
    status = STATUS_NO_MEMORY;
    if (Basic == NULL)
        goto fail4;

    for (Index = 0; Index < Full->Values; Index++) {
        UNICODE_STRING  Unicode;
        ANSI_STRING     Ansi;

        status = ZwEnumerateValueKey(Key,
                                     Index,
                                     KeyValueBasicInformation,
                                     Basic,
                                     Size,
                                     &Size);
        if (!NT_SUCCESS(status))
            goto fail5;

        Unicode.MaximumLength = (USHORT)Basic->NameLength;
        Unicode.Buffer = Basic->Name;
        Unicode.Length = (USHORT)Basic->NameLength;

        Ansi.MaximumLength = (USHORT)((Basic->NameLength / sizeof (WCHAR)) + sizeof (CHAR));
        Ansi.Buffer = __RegistryAllocate(Ansi.MaximumLength);

        status = RtlUnicodeStringToAnsiString(&Ansi, &Unicode, FALSE);
        ASSERT(NT_SUCCESS(status));

        Ansi.Length = (USHORT)(strlen(Ansi.Buffer) * sizeof (CHAR));        

        status = Callback(Context, Key, Ansi.Buffer);

        __RegistryFree(Ansi.Buffer);

        if (!NT_SUCCESS(status))
            goto fail6;
    }

    __RegistryFree(Basic);

    __RegistryFree(Full);

    return STATUS_SUCCESS;

fail6:
fail5:
    __RegistryFree(Basic);

fail4:
fail3:
    __RegistryFree(Full);
    
fail2:
fail1:
    return status;
}

NTSTATUS
RegistryDeleteValue(
    IN  PHANDLE         Key,
    IN  PCHAR           Name
    )
{
    ANSI_STRING         Ansi;
    UNICODE_STRING      Unicode;
    NTSTATUS            status;

    RtlInitAnsiString(&Ansi, Name);

    status = RtlAnsiStringToUnicodeString(&Unicode, &Ansi, TRUE);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = ZwDeleteValueKey(Key, &Unicode);
    if (!NT_SUCCESS(status))
        goto fail2;

    RtlFreeUnicodeString(&Unicode);

    return STATUS_SUCCESS;

fail2:
    RtlFreeUnicodeString(&Unicode);

fail1:
    return status;
}

NTSTATUS
RegistryQueryDwordValue(
    IN  HANDLE                      Key,
    IN  PCHAR                       Name,
    OUT PULONG                      Value
    )
{
    ANSI_STRING                     Ansi;
    UNICODE_STRING                  Unicode;
    PKEY_VALUE_PARTIAL_INFORMATION  Partial;
    ULONG                           Size;
    NTSTATUS                        status;

    RtlInitAnsiString(&Ansi, Name);

    status = RtlAnsiStringToUnicodeString(&Unicode, &Ansi, TRUE);
    if (!NT_SUCCESS(status))
        goto fail1;
        
    status = ZwQueryValueKey(Key,
                             &Unicode,
                             KeyValuePartialInformation,
                             NULL,
                             0,
                             &Size);
    if (status != STATUS_BUFFER_OVERFLOW &&
        status != STATUS_BUFFER_TOO_SMALL)
        goto fail2;

#pragma prefast(suppress:6102)
    Partial = __RegistryAllocate(Size);

    status = STATUS_NO_MEMORY;
    if (Partial == NULL)
        goto fail3;

    status = ZwQueryValueKey(Key,
                             &Unicode,
                             KeyValuePartialInformation,
                             Partial,
                             Size,
                             &Size);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = STATUS_INVALID_PARAMETER;
    if (Partial->Type != REG_DWORD ||
        Partial->DataLength != sizeof (ULONG))
        goto fail5;

    *Value = *(PULONG)Partial->Data;            

    __RegistryFree(Partial);

    RtlFreeUnicodeString(&Unicode);

    return STATUS_SUCCESS;

fail5:
fail4:
    __RegistryFree(Partial);

fail3:
fail2:
    RtlFreeUnicodeString(&Unicode);

fail1:
    return status;
}

NTSTATUS
RegistryUpdateDwordValue(
    IN  HANDLE                      Key,
    IN  PCHAR                       Name,
    IN  ULONG                       Value
    )
{
    ANSI_STRING                     Ansi;
    UNICODE_STRING                  Unicode;
    PKEY_VALUE_PARTIAL_INFORMATION  Partial;
    NTSTATUS                        status;

    RtlInitAnsiString(&Ansi, Name);

    status = RtlAnsiStringToUnicodeString(&Unicode, &Ansi, TRUE);
    if (!NT_SUCCESS(status))
        goto fail1;
        
    Partial = __RegistryAllocate(FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) +
                                 sizeof (ULONG));

    status = STATUS_NO_MEMORY;
    if (Partial == NULL)
        goto fail2;

    Partial->TitleIndex = 0;
    Partial->Type = REG_DWORD;
    Partial->DataLength = sizeof (ULONG);
    *(PULONG)Partial->Data = Value;            

    status = ZwSetValueKey(Key,
                           &Unicode,
                           Partial->TitleIndex,
                           Partial->Type,
                           Partial->Data,
                           Partial->DataLength);
    if (!NT_SUCCESS(status))
        goto fail3;

    __RegistryFree(Partial);

    RtlFreeUnicodeString(&Unicode);

    return STATUS_SUCCESS;

fail3:
    __RegistryFree(Partial);

fail2:
    RtlFreeUnicodeString(&Unicode);

fail1:

    return status;
}

static PANSI_STRING
RegistrySzToAnsi(
    IN  PWCHAR      Buffer
    )
{
    PANSI_STRING    Ansi;
    ULONG           Length;
    UNICODE_STRING  Unicode;
    NTSTATUS        status;

    Ansi = __RegistryAllocate(sizeof (ANSI_STRING) * 2);

    status = STATUS_NO_MEMORY;
    if (Ansi == NULL)
        goto fail1;

    Length = (ULONG)wcslen(Buffer);
    Ansi[0].MaximumLength = (USHORT)(Length + 1) * sizeof (CHAR);
    Ansi[0].Buffer = __RegistryAllocate(Ansi[0].MaximumLength);

    status = STATUS_NO_MEMORY;
    if (Ansi[0].Buffer == NULL)
        goto fail2;

    RtlInitUnicodeString(&Unicode, Buffer);
    status = RtlUnicodeStringToAnsiString(&Ansi[0], &Unicode, FALSE);
    ASSERT(NT_SUCCESS(status));

    Ansi[0].Length = (USHORT)Length * sizeof (CHAR);

    return Ansi;

fail2:
    __RegistryFree(Ansi);

fail1:
    return NULL;
}

static PANSI_STRING
RegistryMultiSzToAnsi(
    IN  PWCHAR      Buffer
    )
{
    PANSI_STRING    Ansi;
    LONG            Index;
    LONG            Count;
    NTSTATUS        status;

    Index = 0;
    Count = 0;
    for (;;) {
        ULONG   Length;

        Length = (ULONG)wcslen(&Buffer[Index]);
        if (Length == 0)
            break;

        Index += Length + 1;
        Count++;
    }

    Ansi = __RegistryAllocate(sizeof (ANSI_STRING) * (Count + 1));

    status = STATUS_NO_MEMORY;
    if (Ansi == NULL)
        goto fail1;

    for (Index = 0; Index < Count; Index++) {
        ULONG           Length;
        UNICODE_STRING  Unicode;

        Length = (ULONG)wcslen(Buffer);
        Ansi[Index].MaximumLength = (USHORT)(Length + 1) * sizeof (CHAR);
        Ansi[Index].Buffer = __RegistryAllocate(Ansi[Index].MaximumLength);

        status = STATUS_NO_MEMORY;
        if (Ansi[Index].Buffer == NULL)
            goto fail2;

        RtlInitUnicodeString(&Unicode, Buffer);

        status = RtlUnicodeStringToAnsiString(&Ansi[Index], &Unicode, FALSE);
        ASSERT(NT_SUCCESS(status));

        Ansi[Index].Length = (USHORT)Length * sizeof (CHAR);
        Buffer += Length + 1;
    }

    return Ansi;

fail2:
    while (--Index >= 0)
        __RegistryFree(Ansi[Index].Buffer);

    __RegistryFree(Ansi);

fail1:
    return NULL;
}

NTSTATUS
RegistryQuerySzValue(
    IN  HANDLE                      Key,
    IN  PCHAR                       Name,
    OUT PANSI_STRING                *Array
    )
{
    ANSI_STRING                     Ansi;
    UNICODE_STRING                  Unicode;
    PKEY_VALUE_PARTIAL_INFORMATION  Value;
    ULONG                           Size;
    NTSTATUS                        status;

    RtlInitAnsiString(&Ansi, Name);

    status = RtlAnsiStringToUnicodeString(&Unicode, &Ansi, TRUE);
    if (!NT_SUCCESS(status))
        goto fail1;
        
    status = ZwQueryValueKey(Key,
                             &Unicode,
                             KeyValuePartialInformation,
                             NULL,
                             0,
                             &Size);
    if (status != STATUS_BUFFER_OVERFLOW &&
        status != STATUS_BUFFER_TOO_SMALL)
        goto fail2;

#pragma prefast(suppress:6102)
    Value = __RegistryAllocate(Size);

    status = STATUS_NO_MEMORY;
    if (Value == NULL)
        goto fail3;

    status = ZwQueryValueKey(Key,
                             &Unicode,
                             KeyValuePartialInformation,
                             Value,
                             Size,
                             &Size);
    if (!NT_SUCCESS(status))
        goto fail4;

    switch (Value->Type) {
    case REG_SZ:
        status = STATUS_NO_MEMORY;
        *Array = RegistrySzToAnsi((PWCHAR)Value->Data);
        break;

    case REG_MULTI_SZ:
        status = STATUS_NO_MEMORY;
        *Array = RegistryMultiSzToAnsi((PWCHAR)Value->Data);
        break;

    default:
        status = STATUS_INVALID_PARAMETER;
        *Array = NULL;
        break;
    }

    if (*Array == NULL)
        goto fail5;

    __RegistryFree(Value);

    RtlFreeUnicodeString(&Unicode);

    return STATUS_SUCCESS;

fail5:
fail4:
    __RegistryFree(Value);

fail3:
fail2:
    RtlFreeUnicodeString(&Unicode);

fail1:
    return status;
}

NTSTATUS
RegistryQueryKeyName(
    IN  HANDLE              Key,
    OUT PANSI_STRING        *Array
    )
{
    PKEY_NAME_INFORMATION   Value;
    ULONG                   Size;
    NTSTATUS                status;

    status = ZwQueryKey(Key,
                        KeyNameInformation,
                        NULL,
                        0,
                        &Size);
    if (status != STATUS_BUFFER_OVERFLOW &&
        status != STATUS_BUFFER_TOO_SMALL)
        goto fail1;

    // Name information is not intrinsically NULL terminated
#pragma prefast(suppress:6102)
    Value = __RegistryAllocate(Size + sizeof (WCHAR));

    status = STATUS_NO_MEMORY;
    if (Value == NULL)
        goto fail2;

    status = ZwQueryKey(Key,
                        KeyNameInformation,
                        Value,
                        Size,
                        &Size);
    if (!NT_SUCCESS(status))
        goto fail3;

    Value->Name[Value->NameLength / sizeof (WCHAR)] = L'\0';
    *Array = RegistrySzToAnsi((PWCHAR)Value->Name);

    status = STATUS_NO_MEMORY;
    if (*Array == NULL)
        goto fail4;

    __RegistryFree(Value);

    return STATUS_SUCCESS;

fail4:
fail3:
    __RegistryFree(Value);

fail2:
fail1:
    return status;
}

NTSTATUS
RegistryQuerySystemStartOption(
    IN  PCHAR                       Prefix,
    OUT PANSI_STRING                *Value
    )
{
    UNICODE_STRING                  Unicode;
    HANDLE                          Key;
    PANSI_STRING                    Ansi;
    ULONG                           Length;
    PCHAR                           Option;
    PCHAR                           Context;
    NTSTATUS                        status;

    RtlInitUnicodeString(&Unicode, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control");
    
    status = RegistryOpenKey(NULL, &Unicode, KEY_READ, &Key);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryQuerySzValue(Key, "SystemStartOptions", &Ansi);
    if (!NT_SUCCESS(status))
        goto fail2;

    // SystemStartOptions is a space separated list of options.
    // Scan it looking for the one we want.
    Length = (ULONG)strlen(Prefix);

    Option = __strtok_r(Ansi[0].Buffer, " ", &Context);
    if (strncmp(Prefix, Option, Length) == 0)
        goto found;

    while ((Option = __strtok_r(NULL, " ", &Context)) != NULL)
        if (strncmp(Prefix, Option, Length) == 0)
            goto found;

    status = STATUS_OBJECT_NAME_NOT_FOUND;
    goto fail3;

found:
    *Value = __RegistryAllocate(sizeof (ANSI_STRING) * 2);

    status = STATUS_NO_MEMORY;
    if (*Value == NULL)
        goto fail4;

    Length = (ULONG)strlen(Option);
    (*Value)[0].MaximumLength = (USHORT)(Length + 1) * sizeof (CHAR);
    (*Value)[0].Buffer = __RegistryAllocate((*Value)[0].MaximumLength);

    status = STATUS_NO_MEMORY;
    if ((*Value)[0].Buffer == NULL)
        goto fail5;

    RtlCopyMemory((*Value)[0].Buffer, Option, Length * sizeof (CHAR));

    (*Value)[0].Length = (USHORT)Length * sizeof (CHAR);

    RegistryFreeSzValue(Ansi);

    ZwClose(Key);

    return STATUS_SUCCESS;

fail5:
    __RegistryFree(*Value);

fail4:
fail3:
    RegistryFreeSzValue(Ansi);

fail2:
    ZwClose(Key);

fail1:
    return status;
}

static PKEY_VALUE_PARTIAL_INFORMATION
RegistryAnsiToSz(
    PANSI_STRING                    Ansi
    )
{
    ULONG                           Length;
    PKEY_VALUE_PARTIAL_INFORMATION  Partial;
    UNICODE_STRING                  Unicode;
    NTSTATUS                        status;

    Length = Ansi->Length + 1;
    Partial = __RegistryAllocate(FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) +
                                 Length * sizeof (WCHAR));

    status = STATUS_NO_MEMORY;
    if (Partial == NULL)
        goto fail1;

    Partial->TitleIndex = 0;
    Partial->Type = REG_SZ;
    Partial->DataLength = Length * sizeof (WCHAR);

    Unicode.MaximumLength = (UCHAR)Partial->DataLength;
    Unicode.Buffer = (PWCHAR)Partial->Data;
    Unicode.Length = 0;

    status = RtlAnsiStringToUnicodeString(&Unicode, Ansi, FALSE);
    if (!NT_SUCCESS(status))
        goto fail2;

    return Partial;

fail2:
    __RegistryFree(Partial);

fail1:
    return NULL;
}

static PKEY_VALUE_PARTIAL_INFORMATION
RegistryAnsiToMultiSz(
    PANSI_STRING                    Ansi
    )
{
    ULONG                           Length;
    ULONG                           Index;
    PKEY_VALUE_PARTIAL_INFORMATION  Partial;
    UNICODE_STRING                  Unicode;
    NTSTATUS                        status;

    Length = 1;
    for (Index = 0; Ansi[Index].Buffer != NULL; Index++)
        Length += Ansi[Index].Length + 1;

    Partial = __RegistryAllocate(FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) +
                               Length * sizeof (WCHAR));

    status = STATUS_NO_MEMORY;
    if (Partial == NULL)
        goto fail1;

    Partial->TitleIndex = 0;
    Partial->Type = REG_MULTI_SZ;
    Partial->DataLength = Length * sizeof (WCHAR);

    Unicode.MaximumLength = (USHORT)Partial->DataLength;
    Unicode.Buffer = (PWCHAR)Partial->Data;
    Unicode.Length = 0;

    for (Index = 0; Ansi[Index].Buffer != NULL; Index++) {
        status = RtlAnsiStringToUnicodeString(&Unicode, &Ansi[Index], FALSE);
        if (!NT_SUCCESS(status))
            goto fail2;

        Length = Unicode.Length / sizeof (WCHAR);

        ASSERT3U(Unicode.MaximumLength, >=, (Length + 1) * sizeof (WCHAR));
        Unicode.MaximumLength -= (USHORT)((Length + 1) * sizeof (WCHAR));
        Unicode.Buffer += Length + 1;
        Unicode.Length = 0;
    }
    *Unicode.Buffer = L'\0';

    return Partial;

fail2:
    __RegistryFree(Partial);

fail1:
    return NULL;
}

NTSTATUS
RegistryUpdateSzValue(
    IN  HANDLE                      Key,
    IN  PCHAR                       Name,
    IN  ULONG                       Type,
    ...
    )
{
    ANSI_STRING                     Ansi;
    UNICODE_STRING                  Unicode;
    va_list                         Arguments;
    PKEY_VALUE_PARTIAL_INFORMATION  Partial;
    NTSTATUS                        status;

    RtlInitAnsiString(&Ansi, Name);

    status = RtlAnsiStringToUnicodeString(&Unicode, &Ansi, TRUE);
    if (!NT_SUCCESS(status))
        goto fail1;
        
    va_start(Arguments, Type);
    switch (Type) {
    case REG_SZ: {
        PANSI_STRING    Argument;

        Argument = va_arg(Arguments, PANSI_STRING);

        status = STATUS_NO_MEMORY;
        Partial = RegistryAnsiToSz(Argument);        
        break;
    }
    case REG_MULTI_SZ: {
        PANSI_STRING    Argument;

        Argument = va_arg(Arguments, PANSI_STRING);

        status = STATUS_NO_MEMORY;
        Partial = RegistryAnsiToMultiSz(Argument);        
        break;
    }
    default:
        status = STATUS_INVALID_PARAMETER;
        Partial = NULL;
        break;
    }
    va_end(Arguments);

    if (Partial == NULL)
        goto fail2;

    status = ZwSetValueKey(Key,
                           &Unicode,
                           Partial->TitleIndex,
                           Partial->Type,
                           Partial->Data,
                           Partial->DataLength);
    if (!NT_SUCCESS(status))
        goto fail3;

    __RegistryFree(Partial);

    RtlFreeUnicodeString(&Unicode);

    return STATUS_SUCCESS;

fail3:
    __RegistryFree(Partial);

fail2:
    RtlFreeUnicodeString(&Unicode);

fail1:
    return status;
}

VOID
RegistryFreeSzValue(
    IN  PANSI_STRING    Array
    )
{
    ULONG               Index;

    if (Array == NULL)
        return;

    for (Index = 0; Array[Index].Buffer != NULL; Index++)
        __RegistryFree(Array[Index].Buffer);

    __RegistryFree(Array);
}

VOID
RegistryCloseKey(
    IN  HANDLE  Key
    )
{
    ZwClose(Key);
}
