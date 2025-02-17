/*
 *  ReactOS Task Manager
 *
 *  perfdata.c
 *
 *  Copyright (C) 1999 - 2001  Brian Palmer  <brianp@reactos.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "stdafx.h"

#include <windows.h>
#include <aclapi.h>
#include <Winternl.h>
#include "UndocumentStruct.h"
#include "perfdata.h"
#include "FileDataStruct.h"

#pragma comment(lib,"ntdll.lib")

#define WIN32_LEAN_AND_MEAN
#define NTOS_MODE_USER
#define NANO_TO_SECOND	1/1000000000

CRITICAL_SECTION                           PerfDataCriticalSection;
PPERFDATA                                  pPerfDataOld = NULL;    /* Older perf data (saved to establish delta values) */
PPERFDATA                                  pPerfData = NULL;    /* Most recent copy of perf data */
ULONG                                      ProcessCountOld = 0;
ULONG                                      ProcessCount = 0;
double                                     dbIdleTime;
double                                     dbKernelTime;
double                                     dbSystemTime;
LARGE_INTEGER                              liOldIdleTime = {{0,0}};
double                                     OldKernelTime = 0;
LARGE_INTEGER                              liOldSystemTime = {{0,0}};
SYSTEM_PERFORMANCE_INFORMATION_UNDOC       SystemPerfInfo;
SYSTEM_BASIC_INFORMATION_UNDOC             SystemBasicInfo;
SYSTEM_FILECACHE_INFORMATION               SystemCacheInfo;
SYSTEM_HANDLE_INFORMATION                  SystemHandleInfo;
PSYSTEM_PROCESSOR_PERFORMANCE_INFORMATION_UNDOC  SystemProcessorTimeInfo = NULL;
PSID                                       SystemUserSid = NULL;

typedef struct _SIDTOUSERNAME
{
    LIST_ENTRY List;
    LPWSTR pszName;
    BYTE Data[0];
} SIDTOUSERNAME, *PSIDTOUSERNAME;

static LIST_ENTRY SidToUserNameHead = {&SidToUserNameHead, &SidToUserNameHead};

BOOL PerfDataInitialize(void)
{
    SID_IDENTIFIER_AUTHORITY NtSidAuthority = {SECURITY_NT_AUTHORITY};
    NTSTATUS    status;

    InitializeCriticalSection(&PerfDataCriticalSection);

    /*
     * Get number of processors in the system
     */
    status = NtQuerySystemInformation (SystemBasicInformation, &SystemBasicInfo, sizeof(SystemBasicInfo), NULL);
    if (!NT_SUCCESS(status))
        return FALSE;

    /*
     * Create the SYSTEM Sid
     */
    AllocateAndInitializeSid(&NtSidAuthority, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &SystemUserSid);
    return TRUE;
}

void PerfDataUninitialize(void)
{
    PLIST_ENTRY pCur;
    PSIDTOUSERNAME pEntry;

    if (pPerfData != NULL)
        HeapFree(GetProcessHeap(), 0, pPerfData);

    DeleteCriticalSection(&PerfDataCriticalSection);

    if (SystemUserSid != NULL)
    {
        FreeSid(SystemUserSid);
        SystemUserSid = NULL;
    }

    /* Free user names cache list */
    pCur = SidToUserNameHead.Flink;
    while (pCur != &SidToUserNameHead)
    {
        pEntry = CONTAINING_RECORD(pCur, SIDTOUSERNAME, List);
        pCur = pCur->Flink;
        HeapFree(GetProcessHeap(), 0, pEntry);
    }
}

static void SidToUserName(PSID Sid, LPWSTR szBuffer, DWORD BufferSize)
{
    static WCHAR szDomainNameUnused[255];
    DWORD DomainNameLen = sizeof(szDomainNameUnused) / sizeof(szDomainNameUnused[0]);
    SID_NAME_USE Use;

    if (Sid != NULL)
        LookupAccountSidW(NULL, Sid, szBuffer, &BufferSize, szDomainNameUnused, &DomainNameLen, &Use);
}

VOID
WINAPI
CachedGetUserFromSid(
    PSID pSid,
    LPWSTR pUserName,
    PULONG pcwcUserName)
{
    PLIST_ENTRY pCur;
    PSIDTOUSERNAME pEntry;
    ULONG cbSid, cwcUserName;

    cwcUserName = *pcwcUserName;

    /* Walk through the list */
    for(pCur = SidToUserNameHead.Flink;
        pCur != &SidToUserNameHead;
        pCur = pCur->Flink)
    {
        pEntry = CONTAINING_RECORD(pCur, SIDTOUSERNAME, List);
        if (EqualSid((PSID)&pEntry->Data, pSid))
        {
            wcsncpy(pUserName, pEntry->pszName, cwcUserName);
            *pcwcUserName = cwcUserName;
            return;
        }
    }

    /* We didn't find the SID in the list, get the name conventional */
    SidToUserName(pSid, pUserName, cwcUserName);

    /* Allocate a new entry */
    *pcwcUserName = wcslen(pUserName);
    cwcUserName = *pcwcUserName + 1;
    cbSid = GetLengthSid(pSid);
    pEntry = (PSIDTOUSERNAME) HeapAlloc(GetProcessHeap(), 0, sizeof(SIDTOUSERNAME) + cbSid + cwcUserName * sizeof(WCHAR));

    /* Copy the Sid and name to our entry */
    CopySid(cbSid, (PSID)&pEntry->Data, pSid);
    pEntry->pszName = (LPWSTR)(pEntry->Data + cbSid);
    wcsncpy(pEntry->pszName, pUserName, cwcUserName);

    /* Insert the new entry */
    pEntry->List.Flink = &SidToUserNameHead;
    pEntry->List.Blink = SidToUserNameHead.Blink;
    SidToUserNameHead.Blink->Flink = &pEntry->List;
    SidToUserNameHead.Blink = &pEntry->List;

    return;
}

void PerfDataRefresh(void)
{
    ULONG                                      ulSize;
    NTSTATUS                                   status;
    LPBYTE                                     pBuffer;
    ULONG                                      BufferSize;
    PSYSTEM_PROCESS_INFORMATION_UNDOC          pSPI;
    PPERFDATA                                  pPDOld;
    ULONG                                      Idx, Idx2;
    HANDLE                                     hProcess;
    HANDLE                                     hProcessToken;
    SYSTEM_PERFORMANCE_INFORMATION_UNDOC       SysPerfInfo;
    SYSTEM_TIMEOFDAY_INFORMATION_UNDOC         SysTimeInfo;
    SYSTEM_FILECACHE_INFORMATION_UNDOC         SysCacheInfo;
    LPBYTE                                     SysHandleInfoData;
    PSYSTEM_PROCESSOR_PERFORMANCE_INFORMATION_UNDOC  SysProcessorTimeInfo;
    double                                     CurrentKernelTime;
    PSECURITY_DESCRIPTOR                       ProcessSD;
    PSID                                       ProcessUser;
    ULONG                                      Buffer[64]; /* must be 4 bytes aligned! */
    ULONG                                      cwcUserName;

    /* Get new system time */
    status = NtQuerySystemInformation(SystemTimeOfDayInformation, &SysTimeInfo, sizeof(SysTimeInfo), NULL);
    if (!NT_SUCCESS(status))
        return;

    /* Get new CPU's idle time */
    status = NtQuerySystemInformation(SystemPerformanceInformation, &SysPerfInfo, sizeof(SysPerfInfo), NULL);
    if (!NT_SUCCESS(status))
        return;

    /* Get system cache information */
    status = NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS) 0x0015, &SysCacheInfo, sizeof(SysCacheInfo), NULL);
    if (!NT_SUCCESS(status))
        return;

    /* Get processor time information */
    SysProcessorTimeInfo = (PSYSTEM_PROCESSOR_PERFORMANCE_INFORMATION_UNDOC)HeapAlloc(GetProcessHeap(), 0, sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION) * SystemBasicInfo.NumberOfProcessors);
    status = NtQuerySystemInformation(SystemProcessorPerformanceInformation, SysProcessorTimeInfo, sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION) * SystemBasicInfo.NumberOfProcessors, &ulSize);

    if (!NT_SUCCESS(status))
    {
        if (SysProcessorTimeInfo != NULL)
            HeapFree(GetProcessHeap(), 0, SysProcessorTimeInfo);
        return;
    }

    /* Get handle information
     * We don't know how much data there is so just keep
     * increasing the buffer size until the call succeeds
     */
    BufferSize = 0;
    do
    {
        BufferSize += 0x10000;
        SysHandleInfoData = (LPBYTE)HeapAlloc(GetProcessHeap(), 0, BufferSize);

        status = NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS) 0x0010, SysHandleInfoData, BufferSize, &ulSize);

        if (status == STATUS_INFO_LENGTH_MISMATCH) {
            HeapFree(GetProcessHeap(), 0, SysHandleInfoData);
        }

    } while (status == STATUS_INFO_LENGTH_MISMATCH);

    /* Get process information
     * We don't know how much data there is so just keep
     * increasing the buffer size until the call succeeds
     */
    BufferSize = 0;
    do
    {
        BufferSize += 0x10000;
        pBuffer = (LPBYTE)HeapAlloc(GetProcessHeap(), 0, BufferSize);

        status = NtQuerySystemInformation(SystemProcessInformation, pBuffer, BufferSize, &ulSize);

        if (status == STATUS_INFO_LENGTH_MISMATCH) {
            HeapFree(GetProcessHeap(), 0, pBuffer);
        }

    } while (status == STATUS_INFO_LENGTH_MISMATCH);

    EnterCriticalSection(&PerfDataCriticalSection);

    /*
     * Save system performance info
     */
    memcpy(&SystemPerfInfo, &SysPerfInfo, sizeof(SYSTEM_PERFORMANCE_INFORMATION));

    /*
     * Save system cache info
     */
    memcpy(&SystemCacheInfo, &SysCacheInfo, sizeof(SYSTEM_FILECACHE_INFORMATION));

    /*
     * Save system processor time info
     */
    if (SystemProcessorTimeInfo) {
        HeapFree(GetProcessHeap(), 0, SystemProcessorTimeInfo);
    }
    SystemProcessorTimeInfo = SysProcessorTimeInfo;

    /*
     * Save system handle info
     */
    memcpy(&SystemHandleInfo, SysHandleInfoData, sizeof(SYSTEM_HANDLE_INFORMATION));
    HeapFree(GetProcessHeap(), 0, SysHandleInfoData);

    for (CurrentKernelTime=0, Idx=0; Idx<(ULONG)SystemBasicInfo.NumberOfProcessors; Idx++) {
        CurrentKernelTime += Li2Double(SystemProcessorTimeInfo[Idx].KernelTime);
        CurrentKernelTime += Li2Double(SystemProcessorTimeInfo[Idx].DpcTime);
        CurrentKernelTime += Li2Double(SystemProcessorTimeInfo[Idx].InterruptTime);
    }

    /* If it's a first call - skip idle time calcs */
    if (liOldIdleTime.QuadPart != 0) {
        /*  CurrentValue = NewValue - OldValue */
        dbIdleTime = Li2Double(SysPerfInfo.IdleProcessTime) - Li2Double(liOldIdleTime);
        dbKernelTime = CurrentKernelTime - OldKernelTime;
        dbSystemTime = Li2Double(SysTimeInfo.CurrentTime) - Li2Double(liOldSystemTime);

        /*  CurrentCpuIdle = IdleTime / SystemTime */
        dbIdleTime = dbIdleTime / dbSystemTime;
        dbKernelTime = dbKernelTime / dbSystemTime;

        /*  CurrentCpuUsage% = 100 - (CurrentCpuIdle * 100) / NumberOfProcessors */
        dbIdleTime = 100.0 - dbIdleTime * 100.0 / (double)SystemBasicInfo.NumberOfProcessors; /* + 0.5; */
        dbKernelTime = 100.0 - dbKernelTime * 100.0 / (double)SystemBasicInfo.NumberOfProcessors; /* + 0.5; */
    }

    /* Store new CPU's idle and system time */
    liOldIdleTime = SysPerfInfo.IdleProcessTime;
    liOldSystemTime = SysTimeInfo.CurrentTime;
    OldKernelTime = CurrentKernelTime;

    /* Determine the process count
     * We loop through the data we got from NtQuerySystemInformation
     * and count how many structures there are (until RelativeOffset is 0)
     */
    ProcessCountOld = ProcessCount;
    ProcessCount = 0;
    pSPI = (PSYSTEM_PROCESS_INFORMATION_UNDOC)pBuffer;
    while (pSPI) {
        ProcessCount++;
        if (pSPI->NextEntryOffset == 0)
            break;
        pSPI = (PSYSTEM_PROCESS_INFORMATION_UNDOC)((LPBYTE)pSPI + pSPI->NextEntryOffset);
    }

    /* Now alloc a new PERFDATA array and fill in the data */
    /* Clear out process perf data structures with HEAP_ZERO_MEMORY flag: */
    pPerfData = (PPERFDATA)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PERFDATA) * ProcessCount);
    pSPI = (PSYSTEM_PROCESS_INFORMATION_UNDOC)pBuffer;
    for (Idx=0; Idx<ProcessCount; Idx++) {
        /* Get the old perf data for this process (if any) */
        /* so that we can establish delta values */
        pPDOld = NULL;
        for (Idx2=0; Idx2<ProcessCountOld; Idx2++) {
            if (pPerfDataOld[Idx2].ProcessId == pSPI->UniqueProcessId) {
                pPDOld = &pPerfDataOld[Idx2];
                break;
            }
        }

        if (pSPI->ImageName.Buffer) {
            /* Don't assume a UNICODE_STRING Buffer is zero terminated: */
            int len = pSPI->ImageName.Length / 2;
            /* Check against max size and allow for terminating zero (already zeroed): */
            if(len >= MAX_PATH)len=MAX_PATH - 1;
            wcsncpy(pPerfData[Idx].ImageName, pSPI->ImageName.Buffer, len);
        } else {
			//
            wcscpy(pPerfData[Idx].ImageName, L"Idle");
        }

        pPerfData[Idx].ProcessId = pSPI->UniqueProcessId;

        if (pPDOld)    {
            double    CurTime = Li2Double(pSPI->KernelTime) + Li2Double(pSPI->UserTime);
            double    OldTime = Li2Double(pPDOld->KernelTime) + Li2Double(pPDOld->UserTime);
            double    CpuTime = (CurTime - OldTime) / dbSystemTime;
            CpuTime = CpuTime * 100.0 / (double)SystemBasicInfo.NumberOfProcessors + 0.5; 
            pPerfData[Idx].CPUUsage = (ULONG)CpuTime;
        }
        pPerfData[Idx].CPUTime.QuadPart = pSPI->UserTime.QuadPart + pSPI->KernelTime.QuadPart;
        pPerfData[Idx].WorkingSetSizeBytes = pSPI->WorkingSetSize;
        pPerfData[Idx].PeakWorkingSetSizeBytes = pSPI->PeakWorkingSetSize;
        if (pPDOld)
            pPerfData[Idx].WorkingSetSizeDelta = labs((LONG)pSPI->WorkingSetSize - (LONG)pPDOld->WorkingSetSizeBytes);
        else
            pPerfData[Idx].WorkingSetSizeDelta = 0;
        pPerfData[Idx].PageFaultCount = pSPI->PageFaultCount;
        if (pPDOld)
            pPerfData[Idx].PageFaultCountDelta = labs((LONG)pSPI->PageFaultCount - (LONG)pPDOld->PageFaultCount);
        else
            pPerfData[Idx].PageFaultCountDelta = 0;
        pPerfData[Idx].VirtualMemorySizeBytes = pSPI->VirtualSize;
        pPerfData[Idx].PagedPoolUsagePages = pSPI->QuotaPeakPagedPoolUsage;
        pPerfData[Idx].NonPagedPoolUsagePages = pSPI->QuotaPeakNonPagedPoolUsage;
        pPerfData[Idx].BasePriority = pSPI->BasePriority;
        pPerfData[Idx].HandleCount = pSPI->HandleCount;
        pPerfData[Idx].ThreadCount = pSPI->NumberOfThreads;
        pPerfData[Idx].SessionId = pSPI->SessionId;
        pPerfData[Idx].UserName[0] = L'\0';
        pPerfData[Idx].USERObjectCount = 0;
        pPerfData[Idx].GDIObjectCount = 0;
        ProcessUser = SystemUserSid;
        ProcessSD = NULL;

        if (pSPI->UniqueProcessId != NULL) {
            hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | READ_CONTROL, FALSE, PtrToUlong(pSPI->UniqueProcessId));
            if (hProcess) {
                /* don't query the information of the system process. It's possible but
                   returns Administrators as the owner of the process instead of SYSTEM */
                if (pSPI->UniqueProcessId != (HANDLE)0x4)
                {
                    if (OpenProcessToken(hProcess, TOKEN_QUERY, &hProcessToken))
                    {
                        DWORD RetLen = 0;
                        BOOL Ret;

                        Ret = GetTokenInformation(hProcessToken, TokenUser, (LPVOID)Buffer, sizeof(Buffer), &RetLen);
                        CloseHandle(hProcessToken);

                        if (Ret)
                            ProcessUser = ((PTOKEN_USER)Buffer)->User.Sid;
                        else
                            goto ReadProcOwner;
                    }
                    else
                    {
ReadProcOwner:
                        GetSecurityInfo(hProcess, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &ProcessUser, NULL, NULL, NULL, &ProcessSD);
                    }

                    pPerfData[Idx].USERObjectCount = GetGuiResources(hProcess, GR_USEROBJECTS);
                    pPerfData[Idx].GDIObjectCount = GetGuiResources(hProcess, GR_GDIOBJECTS);
                }

                GetProcessIoCounters(hProcess, &pPerfData[Idx].IOCounters);
                CloseHandle(hProcess);

				// caculte io speed
				if (pPDOld)
				{
					pPerfData[Idx].ReadOperationsPerSecond = (pPerfData[Idx].IOCounters.ReadOperationCount - pPDOld->IOCounters.ReadOperationCount)/(dbSystemTime*100*NANO_TO_SECOND);
					pPerfData[Idx].WriteOperationsPerSecond = (pPerfData[Idx].IOCounters.WriteOperationCount - pPDOld->IOCounters.WriteOperationCount)/(dbSystemTime*100*NANO_TO_SECOND);
					pPerfData[Idx].OtherOperationsPerSecond = (pPerfData[Idx].IOCounters.OtherOperationCount - pPDOld->IOCounters.OtherOperationCount)/(dbSystemTime*100*NANO_TO_SECOND);
					pPerfData[Idx].ReadTransferPerSecond = (pPerfData[Idx].IOCounters.ReadTransferCount - pPDOld->IOCounters.ReadTransferCount)/(dbSystemTime*100*NANO_TO_SECOND);
					pPerfData[Idx].WriteTransferPerSecond = (pPerfData[Idx].IOCounters.WriteTransferCount - pPDOld->IOCounters.WriteTransferCount)/(dbSystemTime*100*NANO_TO_SECOND);
					pPerfData[Idx].OtherTransferPerSecond = (pPerfData[Idx].IOCounters.OtherTransferCount - pPDOld->IOCounters.OtherTransferCount)/(dbSystemTime*100*NANO_TO_SECOND);
				}
				
            } else {
                goto ClearInfo;
            }
        } else {
ClearInfo:
            /* clear information we were unable to fetch */
            ZeroMemory(&pPerfData[Idx].IOCounters, sizeof(IO_COUNTERS));
        }

        cwcUserName = sizeof(pPerfData[0].UserName) / sizeof(pPerfData[0].UserName[0]);
        CachedGetUserFromSid(ProcessUser, pPerfData[Idx].UserName, &cwcUserName);

        if (ProcessSD != NULL)
        {
            LocalFree((HLOCAL)ProcessSD);
        }

        pPerfData[Idx].UserTime.QuadPart = pSPI->UserTime.QuadPart;
        pPerfData[Idx].KernelTime.QuadPart = pSPI->KernelTime.QuadPart;
        pSPI = (PSYSTEM_PROCESS_INFORMATION_UNDOC)((LPBYTE)pSPI + pSPI->NextEntryOffset);
    }

	if (pPerfDataOld) {
		HeapFree(GetProcessHeap(), 0, pPerfDataOld);
	}
	pPerfDataOld = pPerfData;

    HeapFree(GetProcessHeap(), 0, pBuffer);
    LeaveCriticalSection(&PerfDataCriticalSection);
}

ULONG PerfDataGetProcessIndex(ULONG pid)
{
    ULONG idx;

    EnterCriticalSection(&PerfDataCriticalSection);

    for (idx = 0; idx < ProcessCount; idx++)
    {
        if (PtrToUlong(pPerfData[idx].ProcessId) == pid)
        {
            break;
        }
    }

    LeaveCriticalSection(&PerfDataCriticalSection);

    if (idx == ProcessCount)
    {
        return -1;
    }
    return idx;
}

ULONG PerfDataGetProcessCount(void)
{
    return ProcessCount;
}

ULONG PerfDataGetProcessorUsage(void)
{
	ULONG ulCpuUsage = 0;
	ulCpuUsage = (ULONG)(dbIdleTime + 0.5);

	if (ulCpuUsage > 100)
		ulCpuUsage = 100;

	return ulCpuUsage;
}

ULONG PerfDataGetProcessorSystemUsage(void)
{
    return (ULONG)dbKernelTime;
}

BOOL PerfDataGetImageName(ULONG Index, LPWSTR lpImageName, int nMaxCount)
{
    BOOL  bSuccessful;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount) {
        wcsncpy(lpImageName, pPerfData[Index].ImageName, nMaxCount);
        bSuccessful = TRUE;
    } else {
        bSuccessful = FALSE;
    }
    LeaveCriticalSection(&PerfDataCriticalSection);
    return bSuccessful;
}

ULONG PerfDataGetProcessId(ULONG Index)
{
    ULONG  ProcessId;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        ProcessId = PtrToUlong(pPerfData[Index].ProcessId);
    else
        ProcessId = 0;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return ProcessId;
}

BOOL PerfDataGetUserName(ULONG Index, LPWSTR lpUserName, int nMaxCount)
{
    BOOL  bSuccessful;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount) {
        wcsncpy(lpUserName, pPerfData[Index].UserName, nMaxCount);
        bSuccessful = TRUE;
    } else {
        bSuccessful = FALSE;
    }

    LeaveCriticalSection(&PerfDataCriticalSection);

    return bSuccessful;
}

ULONG PerfDataGetSessionId(ULONG Index)
{
    ULONG  SessionId;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        SessionId = pPerfData[Index].SessionId;
    else
        SessionId = 0;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return SessionId;
}

ULONG PerfDataGetCPUUsage(ULONG Index)
{
    ULONG  CpuUsage;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        CpuUsage = pPerfData[Index].CPUUsage;
    else
        CpuUsage = 0;

    LeaveCriticalSection(&PerfDataCriticalSection);

	if (CpuUsage > 100)
		CpuUsage = 100;

    return CpuUsage;
}

LARGE_INTEGER PerfDataGetCPUTime(ULONG Index)
{
    LARGE_INTEGER  CpuTime = {{0,0}};

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        CpuTime = pPerfData[Index].CPUTime;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return CpuTime;
}

ULONG PerfDataGetWorkingSetSizeBytes(ULONG Index)
{
    ULONG  WorkingSetSizeBytes;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        WorkingSetSizeBytes = pPerfData[Index].WorkingSetSizeBytes;
    else
        WorkingSetSizeBytes = 0;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return WorkingSetSizeBytes;
}

ULONG PerfDataGetPeakWorkingSetSizeBytes(ULONG Index)
{
    ULONG  PeakWorkingSetSizeBytes;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        PeakWorkingSetSizeBytes = pPerfData[Index].PeakWorkingSetSizeBytes;
    else
        PeakWorkingSetSizeBytes = 0;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return PeakWorkingSetSizeBytes;
}

ULONG PerfDataGetWorkingSetSizeDelta(ULONG Index)
{
    ULONG  WorkingSetSizeDelta;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        WorkingSetSizeDelta = pPerfData[Index].WorkingSetSizeDelta;
    else
        WorkingSetSizeDelta = 0;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return WorkingSetSizeDelta;
}

ULONG PerfDataGetPageFaultCount(ULONG Index)
{
    ULONG  PageFaultCount;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        PageFaultCount = pPerfData[Index].PageFaultCount;
    else
        PageFaultCount = 0;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return PageFaultCount;
}

ULONG PerfDataGetPageFaultCountDelta(ULONG Index)
{
    ULONG  PageFaultCountDelta;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        PageFaultCountDelta = pPerfData[Index].PageFaultCountDelta;
    else
        PageFaultCountDelta = 0;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return PageFaultCountDelta;
}

ULONG PerfDataGetVirtualMemorySizeBytes(ULONG Index)
{
    ULONG  VirtualMemorySizeBytes;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        VirtualMemorySizeBytes = pPerfData[Index].VirtualMemorySizeBytes;
    else
        VirtualMemorySizeBytes = 0;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return VirtualMemorySizeBytes;
}

ULONG PerfDataGetPagedPoolUsagePages(ULONG Index)
{
    ULONG  PagedPoolUsage;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        PagedPoolUsage = pPerfData[Index].PagedPoolUsagePages;
    else
        PagedPoolUsage = 0;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return PagedPoolUsage;
}

ULONG PerfDataGetNonPagedPoolUsagePages(ULONG Index)
{
    ULONG  NonPagedPoolUsage;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        NonPagedPoolUsage = pPerfData[Index].NonPagedPoolUsagePages;
    else
        NonPagedPoolUsage = 0;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return NonPagedPoolUsage;
}

ULONG PerfDataGetBasePriority(ULONG Index)
{
    ULONG  BasePriority;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        BasePriority = pPerfData[Index].BasePriority;
    else
        BasePriority = 0;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return BasePriority;
}

ULONG PerfDataGetHandleCount(ULONG Index)
{
    ULONG  HandleCount;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        HandleCount = pPerfData[Index].HandleCount;
    else
        HandleCount = 0;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return HandleCount;
}

ULONG PerfDataGetThreadCount(ULONG Index)
{
    ULONG  ThreadCount;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        ThreadCount = pPerfData[Index].ThreadCount;
    else
        ThreadCount = 0;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return ThreadCount;
}

ULONG PerfDataGetUSERObjectCount(ULONG Index)
{
    ULONG  USERObjectCount;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        USERObjectCount = pPerfData[Index].USERObjectCount;
    else
        USERObjectCount = 0;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return USERObjectCount;
}

ULONG PerfDataGetGDIObjectCount(ULONG Index)
{
    ULONG  GDIObjectCount;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
        GDIObjectCount = pPerfData[Index].GDIObjectCount;
    else
        GDIObjectCount = 0;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return GDIObjectCount;
}

BOOL PerfDataGetIOCounters(ULONG Index, PIO_COUNTERS pIoCounters)
{
    BOOL  bSuccessful;

    EnterCriticalSection(&PerfDataCriticalSection);

    if (Index < ProcessCount)
    {
        memcpy(pIoCounters, &pPerfData[Index].IOCounters, sizeof(IO_COUNTERS));
        bSuccessful = TRUE;
    }
    else
        bSuccessful = FALSE;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return bSuccessful;
}

ULONG PerfDataGetCommitChargeTotalK(void)
{
    ULONG  Total;
    ULONG  PageSize;

    EnterCriticalSection(&PerfDataCriticalSection);

    Total = SystemPerfInfo.CommittedPages;
    PageSize = SystemBasicInfo.PageSize;

    LeaveCriticalSection(&PerfDataCriticalSection);

    Total = Total * (PageSize / 1024);

    return Total;
}

ULONG PerfDataGetCommitChargeLimitK(void)
{
    ULONG  Limit;
    ULONG  PageSize;

    EnterCriticalSection(&PerfDataCriticalSection);

    Limit = SystemPerfInfo.CommitLimit;
    PageSize = SystemBasicInfo.PageSize;

    LeaveCriticalSection(&PerfDataCriticalSection);

    Limit = Limit * (PageSize / 1024);

    return Limit;
}

ULONG PerfDataGetCommitChargePeakK(void)
{
    ULONG  Peak;
    ULONG  PageSize;

    EnterCriticalSection(&PerfDataCriticalSection);

    Peak = SystemPerfInfo.PeakCommitment;
    PageSize = SystemBasicInfo.PageSize;

    LeaveCriticalSection(&PerfDataCriticalSection);

    Peak = Peak * (PageSize / 1024);

    return Peak;
}

ULONG PerfDataGetKernelMemoryTotalK(void)
{
    ULONG  Total;
    ULONG  Paged;
    ULONG  NonPaged;
    ULONG  PageSize;

    EnterCriticalSection(&PerfDataCriticalSection);

    Paged = SystemPerfInfo.PagedPoolPages;
    NonPaged = SystemPerfInfo.NonPagedPoolPages;
    PageSize = SystemBasicInfo.PageSize;

    LeaveCriticalSection(&PerfDataCriticalSection);

    Paged = Paged * (PageSize / 1024);
    NonPaged = NonPaged * (PageSize / 1024);

    Total = Paged + NonPaged;

    return Total;
}

ULONG PerfDataGetKernelMemoryPagedK(void)
{
    ULONG  Paged;
    ULONG  PageSize;

    EnterCriticalSection(&PerfDataCriticalSection);

    Paged = SystemPerfInfo.PagedPoolPages;
    PageSize = SystemBasicInfo.PageSize;

    LeaveCriticalSection(&PerfDataCriticalSection);

    Paged = Paged * (PageSize / 1024);

    return Paged;
}

ULONG PerfDataGetKernelMemoryNonPagedK(void)
{
    ULONG  NonPaged;
    ULONG  PageSize;

    EnterCriticalSection(&PerfDataCriticalSection);

    NonPaged = SystemPerfInfo.NonPagedPoolPages;
    PageSize = SystemBasicInfo.PageSize;

    LeaveCriticalSection(&PerfDataCriticalSection);

    NonPaged = NonPaged * (PageSize / 1024);

    return NonPaged;
}

ULONG PerfDataGetPhysicalMemoryTotalK(void)
{
    ULONG  Total;
    ULONG  PageSize;

    EnterCriticalSection(&PerfDataCriticalSection);

    Total = SystemBasicInfo.NumberOfPhysicalPages;
    PageSize = SystemBasicInfo.PageSize;

    LeaveCriticalSection(&PerfDataCriticalSection);

    Total = Total * (PageSize / 1024);

    return Total;
}

ULONG PerfDataGetPhysicalMemoryAvailableK(void)
{
    ULONG  Available;
    ULONG  PageSize;

    EnterCriticalSection(&PerfDataCriticalSection);

    Available = SystemPerfInfo.AvailablePages;
    PageSize = SystemBasicInfo.PageSize;

    LeaveCriticalSection(&PerfDataCriticalSection);

    Available = Available * (PageSize / 1024);

    return Available;
}

ULONG PerfDataGetPhysicalMemorySystemCacheK(void)
{
    ULONG  SystemCache;
    ULONG  PageSize;

    EnterCriticalSection(&PerfDataCriticalSection);

    PageSize = SystemBasicInfo.PageSize;
    SystemCache = SystemCacheInfo.CurrentSizeIncludingTransitionInPages * PageSize;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return SystemCache / 1024;
}

ULONG PerfDataGetSystemHandleCount(void)
{
    ULONG  HandleCount;

    EnterCriticalSection(&PerfDataCriticalSection);

    HandleCount = SystemHandleInfo.NumberOfHandles;

    LeaveCriticalSection(&PerfDataCriticalSection);

    return HandleCount;
}

ULONG PerfDataGetTotalThreadCount(void)
{
    ULONG  ThreadCount = 0;
    ULONG  i;

    EnterCriticalSection(&PerfDataCriticalSection);

    for (i=0; i<ProcessCount; i++)
    {
        ThreadCount += pPerfData[i].ThreadCount;
    }

    LeaveCriticalSection(&PerfDataCriticalSection);

    return ThreadCount;
}

BOOL PerfDataGet(ULONG Index, PPERFDATA *lppData)
{
    BOOL  bSuccessful = FALSE;

    EnterCriticalSection(&PerfDataCriticalSection);
    if (Index < ProcessCount)
    {
        *lppData = pPerfData + Index;
        bSuccessful = TRUE;
    }
    LeaveCriticalSection(&PerfDataCriticalSection);
    return bSuccessful;
}

ULONGLONG PerfDataGetReadOperationsPerSecond(ULONG Index)
{
	ULONGLONG  ulRetValue;

	EnterCriticalSection(&PerfDataCriticalSection);

	if (Index < ProcessCount)
		ulRetValue = pPerfData[Index].ReadOperationsPerSecond;
	else
		ulRetValue = 0;

	LeaveCriticalSection(&PerfDataCriticalSection);

	return ulRetValue;
}

ULONGLONG PerfDataGetWriteOperationsPerSecond(ULONG Index)
{
	ULONGLONG  ulRetValue;

	EnterCriticalSection(&PerfDataCriticalSection);

	if (Index < ProcessCount)
		ulRetValue = pPerfData[Index].WriteOperationsPerSecond;
	else
		ulRetValue = 0;

	LeaveCriticalSection(&PerfDataCriticalSection);

	return ulRetValue;
}

ULONGLONG PerfDataGetOtherOperationsPerSecond(ULONG Index)
{
	ULONGLONG  ulRetValue;

	EnterCriticalSection(&PerfDataCriticalSection);

	if (Index < ProcessCount)
		ulRetValue = pPerfData[Index].OtherOperationsPerSecond;
	else
		ulRetValue = 0;

	LeaveCriticalSection(&PerfDataCriticalSection);

	return ulRetValue;
}

ULONGLONG PerfDataGetReadTransferPerSecond(ULONG Index)
{
	ULONGLONG  ulRetValue;

	EnterCriticalSection(&PerfDataCriticalSection);

	if (Index < ProcessCount)
		ulRetValue = pPerfData[Index].ReadTransferPerSecond;
	else
		ulRetValue = 0;

	LeaveCriticalSection(&PerfDataCriticalSection);

	return ulRetValue;
}

ULONGLONG PerfDataGetWriteTransferPerSecond(ULONG Index)
{
	ULONGLONG  ulRetValue;

	EnterCriticalSection(&PerfDataCriticalSection);

	if (Index < ProcessCount)
		ulRetValue = pPerfData[Index].WriteTransferPerSecond;
	else
		ulRetValue = 0;

	LeaveCriticalSection(&PerfDataCriticalSection);

	return ulRetValue;
}

ULONGLONG PerfDataGetOtherTransferPerSecond(ULONG Index)
{
	ULONGLONG  ulRetValue;

	EnterCriticalSection(&PerfDataCriticalSection);

	if (Index < ProcessCount)
		ulRetValue = pPerfData[Index].OtherTransferPerSecond;
	else
		ulRetValue = 0;

	LeaveCriticalSection(&PerfDataCriticalSection);

	return ulRetValue;
}

BOOL PerfDataTakeSnapshot(PVOID * pbuffer)
{
	BOOL bRet = FALSE;
	*pbuffer = NULL;
	PNTF_HEADER pHeader = NULL;

	EnterCriticalSection(&PerfDataCriticalSection);

	pHeader = (PNTF_HEADER) HeapAlloc(GetProcessHeap(), 0, sizeof(NTF_HEADER) + ProcessCount * sizeof(PERFDATA));
	if (NULL == pHeader)
	{
		goto RET;
	}
	
	// copy data to our snapshot
	pHeader->wSignature = NTF_SIGNATURE;
	pHeader->ProcessCount = ProcessCount;
	pHeader->dbIdleTime = dbIdleTime;		
	pHeader->dbKernelTime = dbKernelTime;
	pHeader->dbSystemTime = dbSystemTime;
	memcpy(&pHeader->SystemPerfInfo, &SystemPerfInfo, sizeof(SYSTEM_PERFORMANCE_INFORMATION_UNDOC));
	memcpy(&pHeader->SystemBasicInfo, &SystemBasicInfo, sizeof(SYSTEM_BASIC_INFORMATION_UNDOC));
	memcpy(&pHeader->SystemCacheInfo, &SystemCacheInfo, sizeof(SYSTEM_FILECACHE_INFORMATION));
	memcpy(&pHeader->SystemHandleInfo, &SystemHandleInfo, sizeof(SYSTEM_HANDLE_INFORMATION));
	pHeader->SystemUserSid = SystemUserSid;

	PPERFDATA pbody = (PPERFDATA) pHeader->pData;

	if (!pHeader)
		goto RET;

	memcpy(pbody, pPerfData, ProcessCount * sizeof(PERFDATA));
	bRet = TRUE;

RET:
	LeaveCriticalSection(&PerfDataCriticalSection);

	if (FALSE == bRet)
		HeapFree(GetProcessHeap(), 0, pHeader);
	else 
		*pbuffer = pHeader;

	return bRet;
}

ULONG PerfDataGetMemoryUsage(void)
{
	double douRamUsage = 0;
	douRamUsage = 100.0 * PerfDataGetPhysicalMemoryAvailableK() / PerfDataGetPhysicalMemoryTotalK() ;
	return (ULONG) (douRamUsage + 0.5);
}

