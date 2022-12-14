#include "stdafx.h"
#include <atlbase.h>
#include <atlcomcli.h>
#include <BazisLib/bzshlp/Win32/process.h>
#include <BazisLib/bzscore/datetime.h>
#include <BazisLib/bzscore/file.h>
#include <BazisLib/bzscore/string.h>
#include <atlsafe.h>

#include "VMAPI/VixC.h"
#ifndef VIX_C_SUPPORTED
#pragma message("WARNING: Missing VIX header. VMware Workstation Pro must be installed during build for snapshot support.")
#include "VMAPI/VixCOM.h"
#endif

extern HINSTANCE g_hThisDll;

HRESULT StartRevertingCurrentVMToLastSnapshot_VMWare()
{
    TCHAR tszDll[MAX_PATH];
    GetModuleFileName(g_hThisDll, tszDll, _countof(tszDll));

    size_t len = _tcslen(tszDll);
    if (len < 6)
        return E_FAIL;

    if (!_memicmp(tszDll + len - 6, L"64.dll", 12))
        memcpy(tszDll + len - 6, L"32.dll", 10);


    BazisLib::String vmFile;

    int count = 0;
    LPWSTR *args = CommandLineToArgvW(GetCommandLineW(), &count);

    for (int i = 0; i < count; i++)
    {
        wchar_t *pArg = args[i];
        size_t len = wcslen(pArg);
        if (len < 4)
            continue;
        if (_memicmp(pArg + len - 4, L".vmx", 8))
            continue;

        vmFile = pArg;
        break;
    }
    LocalFree(args);

    BazisLib::Win32::Process process(BazisLib::String::sFormat(L"rundll32 \"%s\",VMSnapshotRevertingEntry_VMWare /VMXFILE:%s", tszDll, vmFile.c_str()).c_str(), (WORD)SW_SHOW);
    return S_OK;
}

#if !defined(_WIN64)
#if !defined(VIX_C_SUPPORTED)
template <class _ReturnedInterface> static HRESULT WaitForJobResult(CComPtr<IVixLib> pVix, CComPtr<IJob> pJob, CComPtr<_ReturnedInterface> &pResult)
{
    CComVariant results;
    CComSafeArray<int> propertyID;
    propertyID.Add(VIX_PROPERTY_JOB_RESULT_HANDLE);
    ULONGLONG resultHandle = 0;
    HRESULT hR = pJob->Wait(CComVariant(propertyID), &results, &resultHandle);

    if (!SUCCEEDED(hR))
        return hR;
    VARIANT_BOOL success = 0;
    hR = pVix->ErrorIndicatesSuccess(resultHandle, &success);
    if (!SUCCEEDED(hR))
        return hR;
    if (!success)
        return E_FAIL;
    CComSafeArray<VARIANT> resultArray = results.parray;
    if (resultArray.GetCount() != 1)
        return E_FAIL;
    hR = resultArray.GetAt(0).punkVal->QueryInterface(&pResult);
    if (!SUCCEEDED(hR))
        return hR;
    return S_OK;
}

static HRESULT DoRevertVMToSnapshot(const wchar_t *pVMXFile)
{
    CComPtr<IVixLib> pVixLib;
    HRESULT hR = pVixLib.CoCreateInstance(__uuidof(VixLib));
    if (!SUCCEEDED(hR))
        return hR;

    CComBSTR emptyStr(L"");

    CComPtr<IJob> pJob;

    hR = pVixLib->Connect(VIX_API_VERSION, VIX_SERVICEPROVIDER_DEFAULT, emptyStr, 0, emptyStr, emptyStr, 0, NULL, NULL, &pJob);
    if (!SUCCEEDED(hR))
        return hR;

    CComPtr<IHost> pHost;

    hR = WaitForJobResult(pVixLib, pJob, pHost);
    if (!SUCCEEDED(hR))
        return hR;

    pJob = NULL;
    hR = pHost->OpenVM(CComBSTR(pVMXFile), NULL, &pJob);
    if (!SUCCEEDED(hR))
        return hR;

    CComPtr<IVM> pVM;
    hR = WaitForJobResult(pVixLib, pJob, pVM);
    if (!SUCCEEDED(hR))
        return hR;

    CComPtr<ISnapshot> pSnapshot;
    ULONGLONG error = 0;

    hR = pVM->GetCurrentSnapshot(&pSnapshot, &error);
    if (!SUCCEEDED(hR))
        return hR;
    VARIANT_BOOL success = 0;
    hR = pVixLib->ErrorIndicatesSuccess(error, &success);
    if (!SUCCEEDED(hR))
        return hR;
    if (!success)
        return E_FAIL;

    pJob = NULL;
    hR = pVM->RevertToSnapshot(pSnapshot, VIX_VMPOWEROP_LAUNCH_GUI, NULL, NULL, &pJob);
    if (!SUCCEEDED(hR))
        return hR;

    CComVariant results;
    CComSafeArray<int> args;
    args.Add(VIX_PROPERTY_NONE);
    hR = pJob->Wait(CComVariant(args), &results, &error);
    if (!SUCCEEDED(hR))
        return hR;

    return S_OK;
}
#else
static HRESULT DoRevertVMToSnapshot(const wchar_t* pVMXFile)
{
    VixDLL dll;
    VixError err;
    VixHandle hostHandle = VIX_INVALID_HANDLE;
    VixHandle jobHandle = VIX_INVALID_HANDLE;
    VixHandle vmHandle = VIX_INVALID_HANDLE;
    VixHandle snapshotHandle = VIX_INVALID_HANDLE;

    if (!dll.VixDLLIsValid())
    {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    jobHandle = dll.VixHost_Connect(VIX_API_VERSION, VIX_SERVICEPROVIDER_DEFAULT, "", 0, "", "", 0, NULL, NULL, NULL);
    err = dll.VixJob_Wait(jobHandle,
        VIX_PROPERTY_JOB_RESULT_HANDLE,
        &hostHandle,
        VIX_PROPERTY_NONE);

    if (VIX_FAILED(err))
    {
        goto abort;
    }

    dll.Vix_ReleaseHandle(jobHandle);

    jobHandle = dll.VixVM_Open(hostHandle,
        BazisLib::StringToANSIString(BazisLib::String(pVMXFile)).c_str(),
        NULL, 
        NULL);
    err = dll.VixJob_Wait(jobHandle,
        VIX_PROPERTY_JOB_RESULT_HANDLE,
        &vmHandle,
        VIX_PROPERTY_NONE);
    
    if (VIX_FAILED(err))
    {
        goto abort;
    }

    dll.Vix_ReleaseHandle(jobHandle);

    err = dll.VixVM_GetCurrentSnapshot(vmHandle, &snapshotHandle);
    
    if (VIX_FAILED(err))
    {
        goto abort;
    }

    jobHandle = dll.VixVM_RevertToSnapshot(vmHandle,
        snapshotHandle,
        VIX_VMPOWEROP_LAUNCH_GUI,
        VIX_INVALID_HANDLE,
        NULL,
        NULL);
    err = dll.VixJob_Wait(jobHandle, VIX_PROPERTY_NONE);
    
    if (VIX_FAILED(err))
    {
        goto abort;
    }

    err = VIX_OK;

abort:
    dll.Vix_ReleaseHandle(jobHandle);
    dll.Vix_ReleaseHandle(vmHandle);
    dll.Vix_ReleaseHandle(snapshotHandle);

    dll.VixHost_Disconnect(hostHandle);

    return err;
}
#endif
extern "C" void __declspec(dllexport) VMSnapshotRevertingEntry_VMWare()
{
    wchar_t* pVmxFile = wcsstr(GetCommandLineW(), L"/VMXFILE:");
    if (!pVmxFile)
        ExitProcess(-1);

    pVmxFile += 9;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hR = DoRevertVMToSnapshot(pVmxFile);
    CoUninitialize();
    ExitProcess(hR);
}
#endif
