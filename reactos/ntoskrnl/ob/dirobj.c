/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ob/dirobj.c
 * PURPOSE:         Directory Object Implementation
 * PROGRAMMER:      David Welch (welch@mcmail.com)
 */

/* INCLUDES ***************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <internal/debug.h>

/* FUNCTIONS **************************************************************/

/*++
* @name NtOpenDirectoryObject
* @implemented NT4
*
*     The NtOpenDirectoryObject opens a namespace directory object.
*
* @param DirectoryHandle
*        Variable which receives the directory handle.
*
* @param DesiredAccess
*        Desired access to the directory.
*
* @param ObjectAttributes
*        Structure describing the directory.
*
* @return STATUS_SUCCESS or appropriate error value.
*
* @remarks None.
*
*--*/
NTSTATUS
NTAPI
NtOpenDirectoryObject (OUT PHANDLE DirectoryHandle,
                       IN ACCESS_MASK DesiredAccess,
                       IN POBJECT_ATTRIBUTES ObjectAttributes)
{
    HANDLE hDirectory;
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    NTSTATUS Status = STATUS_SUCCESS;
    PAGED_CODE();

    if(PreviousMode != KernelMode)
    {
        _SEH_TRY
        {
            ProbeForWriteHandle(DirectoryHandle);
        }
        _SEH_HANDLE
        {
            Status = _SEH_GetExceptionCode();
        }
        _SEH_END;

        if(!NT_SUCCESS(Status))
        {
            DPRINT1("NtOpenDirectoryObject failed, Status: 0x%x\n", Status);
            return Status;
        }
    }

    Status = ObOpenObjectByName(ObjectAttributes,
                                ObDirectoryType,
                                NULL,
                                PreviousMode,
                                DesiredAccess,
                                NULL,
                                &hDirectory);
    if(NT_SUCCESS(Status))
    {
        _SEH_TRY
        {
            *DirectoryHandle = hDirectory;
        }
        _SEH_HANDLE
        {
            Status = _SEH_GetExceptionCode();
        }
        _SEH_END;
    }

    return Status;
}

/*++
* @name NtQueryDirectoryObject
* @implemented NT4
*
*     The NtQueryDirectoryObject Reads information from a directory in
*     the system namespace.
*
* @param DirectoryHandle
*        Handle obtained with NtOpenDirectoryObject which
*        must grant DIRECTORY_QUERY access to the directory object.
*
* @param Buffer
*        Buffer to hold the data read.
*
* @param BufferLength
*        Size of the buffer in bytes.
*
* @param ReturnSingleEntry
*        When TRUE, only 1 entry is written in DirObjInformation;
*        otherwise as many as will fit in the buffer.
*
* @param RestartScan
*        If TRUE start reading at index 0.
*        If FALSE start reading at the index specified by *ObjectIndex.
*
* @param Context
*        Zero based index into the directory, interpretation
*        depends on RestartScan.
*
* @param ReturnLength
*        Caller supplied storage for the number of bytes
*        written (or NULL).
*
* @return STATUS_SUCCESS or appropriate error value.
*
* @remarks Although you can iterate over the directory by calling this
*          function multiple times, the directory is unlocked between
*          calls. This means that another thread can change the directory
*          and so iterating doesn't guarantee a consistent picture of the
*          directory. Best thing is to retrieve all directory entries in
*          one call.
*
*--*/
NTSTATUS
NTAPI
NtQueryDirectoryObject(IN HANDLE DirectoryHandle,
                       OUT PVOID Buffer,
                       IN ULONG BufferLength,
                       IN BOOLEAN ReturnSingleEntry,
                       IN BOOLEAN RestartScan,
                       IN OUT PULONG Context,
                       OUT PULONG ReturnLength OPTIONAL)
{
    PDIRECTORY_OBJECT Directory;
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    ULONG SkipEntries = 0;
    ULONG NextEntry = 0;
    ULONG CopyBytes = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    PAGED_CODE();

    if(PreviousMode != KernelMode)
    {
        _SEH_TRY
        {
            /* a test showed that the Buffer pointer just has to be 16 bit aligned,
            propably due to the fact that most information that needs to be copied
            is unicode strings */
            ProbeForWrite(Buffer, BufferLength, sizeof(WCHAR));
            ProbeForWriteUlong(Context);
            if(!RestartScan)
            {
                SkipEntries = *Context;
            }
            if(ReturnLength != NULL)
            {
                ProbeForWriteUlong(ReturnLength);
            }
        }
        _SEH_HANDLE
        {
            Status = _SEH_GetExceptionCode();
        }
        _SEH_END;

        if(!NT_SUCCESS(Status))
        {
            DPRINT1("NtQueryDirectoryObject failed, Status: 0x%x\n", Status);
            return Status;
        }
    }
    else if(!RestartScan)
    {
        SkipEntries = *Context;
    }

    Status = ObReferenceObjectByHandle(DirectoryHandle,
                                       DIRECTORY_QUERY,
                                       ObDirectoryType,
                                       PreviousMode,
                                       (PVOID*)&Directory,
                                       NULL);
    if(NT_SUCCESS(Status))
    {
        PVOID TemporaryBuffer = ExAllocatePool(NonPagedPool, BufferLength);
        if(TemporaryBuffer != NULL)
        {
            PROS_OBJECT_HEADER EntryHeader;
            PLIST_ENTRY ListEntry;
            KIRQL OldLevel;
            ULONG RequiredSize = sizeof(OBJECT_DIRECTORY_INFORMATION);
            ULONG nDirectories = 0;
            POBJECT_DIRECTORY_INFORMATION DirInfo = 
                (POBJECT_DIRECTORY_INFORMATION)TemporaryBuffer;

            Status = STATUS_NO_MORE_ENTRIES;

            KeAcquireSpinLock(&Directory->Lock, &OldLevel);

            for(ListEntry = Directory->head.Flink;
                ListEntry != &Directory->head;
                ListEntry = ListEntry->Flink)
            {
                NextEntry++;
                if(SkipEntries == 0)
                {
                    PUNICODE_STRING Name, Type;
                    ULONG EntrySize;

                    EntryHeader = CONTAINING_RECORD(ListEntry,
                                                    ROS_OBJECT_HEADER,
                                                    Entry);

                    /* calculate the size of the required buffer space for this entry */
                    Name = (HEADER_TO_OBJECT_NAME(EntryHeader)->Name.Length != 0 ?
                            &HEADER_TO_OBJECT_NAME(EntryHeader)->Name : NULL);
                    Type = &EntryHeader->Type->Name;
                    EntrySize = sizeof(OBJECT_DIRECTORY_INFORMATION) +
                                ((Name != NULL) ?
                                ((ULONG)Name->Length + sizeof(WCHAR)) : 0) +
                                (ULONG)EntryHeader->Type->Name.Length +
                                sizeof(WCHAR);

                    if(RequiredSize + EntrySize <= BufferLength)
                    {
                        /* the buffer is large enough to receive this entry. It would've
                        been much easier if the strings were directly appended to the
                        OBJECT_DIRECTORY_INFORMATION structured written into the buffer */
                        if(Name != NULL)
                            DirInfo->ObjectName = *Name;
                        else
                        {
                            DirInfo->ObjectName.Length = 
                                DirInfo->ObjectName.MaximumLength = 0;
                            DirInfo->ObjectName.Buffer = NULL;
                        }
                        DirInfo->ObjectTypeName = *Type;

                        nDirectories++;
                        RequiredSize += EntrySize;

                        Status = STATUS_SUCCESS;

                        if(ReturnSingleEntry)
                        {
                            /* we're only supposed to query one entry, so bail and copy the
                            strings to the buffer */
                            break;
                        }
                        DirInfo++;
                    }
                    else
                    {
                        if(ReturnSingleEntry)
                        {
                            /* the buffer is too small, so return the number of bytes that
                            would've been required for this query */
                            RequiredSize += EntrySize;
                            Status = STATUS_BUFFER_TOO_SMALL;
                        }

                        /* we couldn't query this entry, so leave the index that will be stored
                        in Context to this entry so the caller can query it the next time
                        he queries (hopefully with a buffer that is large enough then...) */
                        NextEntry--;

                        /* just copy the entries that fit into the buffer */
                        break;
                    }
                }
                else
                {
                    /* skip the entry */
                    SkipEntries--;
                }
            }

            if(!ReturnSingleEntry && ListEntry != &Directory->head)
            {
                /* there are more entries to enumerate but the buffer is already full.
                only tell this to the user if he queries multiple entries */
                Status = STATUS_MORE_ENTRIES;
            }

            if(NT_SUCCESS(Status) && nDirectories > 0)
            {
                PWSTR strbuf = 
                    (PWSTR)((POBJECT_DIRECTORY_INFORMATION)TemporaryBuffer +
                            nDirectories + 1);
                PWSTR deststrbuf =
                    (PWSTR)((POBJECT_DIRECTORY_INFORMATION)Buffer +
                            nDirectories + 1);

                memset((POBJECT_DIRECTORY_INFORMATION)TemporaryBuffer +
                       nDirectories,
                       0,
                       sizeof(OBJECT_DIRECTORY_INFORMATION));

                CopyBytes = (nDirectories + 1) *
                            sizeof(OBJECT_DIRECTORY_INFORMATION);

                /* copy the names from the objects and append them to the list of the
                objects. copy to the temporary buffer only because the directory
                lock can't be released and the buffer might be pagable memory! */
                for(DirInfo = (POBJECT_DIRECTORY_INFORMATION)TemporaryBuffer;
                    nDirectories > 0;
                    nDirectories--, DirInfo++)
                {
                    ULONG NameLength;

                    if(DirInfo->ObjectName.Length > 0)
                    {
                        RtlCopyMemory(strbuf,
                                      DirInfo->ObjectName.Buffer,
                                      DirInfo->ObjectName.Length);

                        /* change the buffer pointer to the buffer */
                        DirInfo->ObjectName.Buffer = deststrbuf;
                        NameLength = DirInfo->ObjectName.Length /
                                     sizeof(WCHAR);

                        /* NULL-terminate the string */
                        strbuf[NameLength] = L'\0';
                        strbuf += NameLength + 1;
                        deststrbuf += NameLength + 1;

                        CopyBytes += (NameLength + 1) * sizeof(WCHAR);
                    }

                    RtlCopyMemory(strbuf,
                                  DirInfo->ObjectTypeName.Buffer,
                                  DirInfo->ObjectTypeName.Length);

                    /* change the buffer pointer to the buffer */
                    DirInfo->ObjectTypeName.Buffer = deststrbuf;
                    NameLength = DirInfo->ObjectTypeName.Length /
                                 sizeof(WCHAR);

                    /* NULL-terminate the string */
                    strbuf[NameLength] = L'\0';
                    strbuf += NameLength + 1;
                    deststrbuf += NameLength + 1;

                    CopyBytes += (NameLength + 1) * sizeof(WCHAR);
                }
            }

            KeReleaseSpinLock(&Directory->Lock, OldLevel);
            ObDereferenceObject(Directory);

            if(NT_SUCCESS(Status) || ReturnSingleEntry)
            {
                _SEH_TRY
                {
                    if(CopyBytes != 0)
                    {
                        RtlCopyMemory(Buffer, TemporaryBuffer, CopyBytes);
                    }

                    *Context = NextEntry;

                    if(ReturnLength != NULL)
                    {
                        *ReturnLength = RequiredSize;
                    }
                }
                _SEH_HANDLE
                {
                    Status = _SEH_GetExceptionCode();
                }
                _SEH_END;
            }

            ExFreePool(TemporaryBuffer);
        }
        else
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    return Status;
}

/*++
* @name NtCreateDirectoryObject
* @implemented NT4
*
*     The NtOpenDirectoryObject creates or opens a directory object.
*
* @param DirectoryHandle
*        Variable which receives the directory handle.
*
* @param DesiredAccess
*        Desired access to the directory.
*
* @param ObjectAttributes
*        Structure describing the directory.
*
* @return STATUS_SUCCESS or appropriate error value.
*
* @remarks None.
*
*--*/
NTSTATUS
NTAPI
NtCreateDirectoryObject(OUT PHANDLE DirectoryHandle,
                        IN ACCESS_MASK DesiredAccess,
                        IN POBJECT_ATTRIBUTES ObjectAttributes)
{
    PDIRECTORY_OBJECT Directory;
    HANDLE hDirectory;
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    NTSTATUS Status = STATUS_SUCCESS;
    PAGED_CODE();

    DPRINT("NtCreateDirectoryObject(DirectoryHandle %x, "
            "DesiredAccess %x, ObjectAttributes %x\n",
            DirectoryHandle, DesiredAccess, ObjectAttributes);

    if(PreviousMode != KernelMode)
    {
        _SEH_TRY
        {
            ProbeForWriteHandle(DirectoryHandle);
        }
        _SEH_HANDLE
        {
            Status = _SEH_GetExceptionCode();
        }
        _SEH_END;

        if(!NT_SUCCESS(Status))
        {
            DPRINT1("NtCreateDirectoryObject failed, Status: 0x%x\n", Status);
            return Status;
        }
    }

    Status = ObCreateObject(PreviousMode,
                            ObDirectoryType,
                            ObjectAttributes,
                            PreviousMode,
                            NULL,
                            sizeof(DIRECTORY_OBJECT),
                            0,
                            0,
                            (PVOID*)&Directory);

    if(NT_SUCCESS(Status))
    {
        Status = ObInsertObject((PVOID)Directory,
                                NULL,
                                DesiredAccess,
                                0,
                                NULL,
                                &hDirectory);
        if (!NT_SUCCESS(Status))
        {
            ObMakeTemporaryObject(Directory);
        }
        ObDereferenceObject(Directory);

        if(NT_SUCCESS(Status))
        {
            _SEH_TRY
            {
                *DirectoryHandle = hDirectory;
            }
            _SEH_HANDLE
            {
                Status = _SEH_GetExceptionCode();
            }
            _SEH_END;
        }
    }

    return Status;
}

/* EOF */
