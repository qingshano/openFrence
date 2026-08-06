// openFences Shell Service Object
// 注册到 ShellServiceObjectDelayLoad，explorer 启动时自动通过 CoCreateInstance 加载

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <vector>
#include <algorithm>

// {F3E8C2A1-B4D5-6789-ABCD-EF0123456789}
static const GUID CLSID_OpenFences =
{ 0xf3e8c2a1, 0xb4d5, 0x6789, { 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89 } };

static WCHAR g_dllPath[MAX_PATH] = {};

// ── 图标管理 ──
struct FenceRect { int x, y, w, h; };
static std::vector<FenceRect> g_fences;
static WNDPROC g_origProc = nullptr;

HWND FindDesktopListView() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) SendMessageW(progman, 0x052C, 0xD, 0);
    HWND defview = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (!defview) {
        HWND w = FindWindowExW(nullptr, nullptr, L"WorkerW", nullptr);
        while (w) {
            defview = FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr);
            if (defview) break;
            w = FindWindowExW(nullptr, w, L"WorkerW", nullptr);
        }
    }
    return defview ? FindWindowExW(defview, nullptr, L"SysListView32", nullptr) : nullptr;
}

void ConstrainToFences(int& x, int& y) {
    for (auto& f : g_fences) {
        if (x >= f.x - 30 && x <= f.x + f.w + 30 &&
            y >= f.y - 30 && y <= f.y + f.h + 30) {
            x = (std::max)(f.x, (std::min)(x, f.x + f.w - 40));
            y = (std::max)(f.y + 24, (std::min)(y, f.y + f.h - 20));
            return;
        }
    }
}

LRESULT CALLBACK ListViewSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COPYDATA) {
        auto* cds = (COPYDATASTRUCT*)lp;
        if (cds->dwData == 1 && cds->cbData > 0) {
            size_t n = cds->cbData / sizeof(FenceRect);
            g_fences.assign((FenceRect*)cds->lpData, (FenceRect*)cds->lpData + n);
        }
        return TRUE;
    }
    if (msg == 0x100F) { // LVM_SETITEMPOSITION
        int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
        ConstrainToFences(x, y);
        lp = MAKELPARAM(x, y);
    }
    return CallWindowProcW(g_origProc, hwnd, msg, wp, lp);
}

void InstallSubclass() {
    HWND lv = FindDesktopListView();
    if (lv) {
        g_origProc = (WNDPROC)(LONG_PTR)SetWindowLongPtrW(
            lv, GWLP_WNDPROC, (LONG_PTR)ListViewSubclass);
    }
}

// ── COM 对象 ──
class OpenFencesObj : public IUnknown {
    volatile LONG m_ref = 1;
public:
    OpenFencesObj() { InstallSubclass(); }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown) { *ppv = static_cast<IUnknown*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) { delete this; }
        return r;
    }
};

// ── COM DLL 导出 ──
static volatile LONG g_lockCount = 0;

class OpenFencesCF : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* obj = new OpenFencesObj();
        HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) InterlockedIncrement(&g_lockCount);
        else InterlockedDecrement(&g_lockCount);
        return S_OK;
    }
};

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (rclsid != CLSID_OpenFences) return CLASS_E_CLASSNOTAVAILABLE;
    static OpenFencesCF factory;
    return factory.QueryInterface(riid, ppv);
}
STDAPI DllCanUnloadNow() { return g_lockCount == 0 ? S_OK : S_FALSE; }

// ── 注册 ──
STDAPI DllRegisterServer() {
    WCHAR clsid[64], key[512];
    swprintf_s(clsid, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        CLSID_OpenFences.Data1, CLSID_OpenFences.Data2, CLSID_OpenFences.Data3,
        CLSID_OpenFences.Data4[0],CLSID_OpenFences.Data4[1],CLSID_OpenFences.Data4[2],
        CLSID_OpenFences.Data4[3],CLSID_OpenFences.Data4[4],CLSID_OpenFences.Data4[5],
        CLSID_OpenFences.Data4[6],CLSID_OpenFences.Data4[7]);

    HKEY hk;

    // CLSID\{...}
    swprintf_s(key, L"CLSID\\%s", clsid);
    RegCreateKeyExW(HKEY_CLASSES_ROOT, key, 0,0,0,KEY_WRITE,0,&hk,0);
    RegSetValueExW(hk, 0,0,REG_SZ,(BYTE*)L"openFences Shell Object", 44);
    RegCloseKey(hk);

    // InprocServer32
    swprintf_s(key, L"CLSID\\%s\\InprocServer32", clsid);
    RegCreateKeyExW(HKEY_CLASSES_ROOT, key, 0,0,0,KEY_WRITE,0,&hk,0);
    RegSetValueExW(hk, 0,0,REG_SZ,(BYTE*)g_dllPath,(DWORD)(wcslen(g_dllPath)+1)*2);
    RegSetValueExW(hk, L"ThreadingModel",0,REG_SZ,(BYTE*)L"Apartment",20);
    RegCloseKey(hk);

    // ShellServiceObjectDelayLoad
    RegCreateKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\ShellServiceObjectDelayLoad",
        0,0,0,KEY_WRITE,0,&hk,0);
    RegSetValueExW(hk, clsid,0,REG_SZ,(BYTE*)L"openFences Desktop",36);
    RegCloseKey(hk);

    // Approved
    RegCreateKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
        0,0,0,KEY_WRITE,0,&hk,0);
    RegSetValueExW(hk, clsid,0,REG_SZ,(BYTE*)L"openFences Desktop Manager",52);
    RegCloseKey(hk);

    return S_OK;
}

STDAPI DllUnregisterServer() {
    WCHAR clsid[64], key[512];
    swprintf_s(clsid, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        CLSID_OpenFences.Data1, CLSID_OpenFences.Data2, CLSID_OpenFences.Data3,
        CLSID_OpenFences.Data4[0],CLSID_OpenFences.Data4[1],CLSID_OpenFences.Data4[2],
        CLSID_OpenFences.Data4[3],CLSID_OpenFences.Data4[4],CLSID_OpenFences.Data4[5],
        CLSID_OpenFences.Data4[6],CLSID_OpenFences.Data4[7]);

    swprintf_s(key, L"CLSID\\%s", clsid);
    RegDeleteTreeW(HKEY_CLASSES_ROOT, key);

    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\ShellServiceObjectDelayLoad",
        0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
        RegDeleteValueW(hk, clsid);
        RegCloseKey(hk);
    }

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
        0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
        RegDeleteValueW(hk, clsid);
        RegCloseKey(hk);
    }
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        GetModuleFileNameW(hInst, g_dllPath, MAX_PATH);
    }
    return TRUE;
}
