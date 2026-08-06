// Desktop window hierarchy diagnostic for Windows 11
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <stdio.h>
#define LVM_GETITEMPOSITION (0x1000 + 16)
#define LVM_SETITEMPOSITION (0x1000 + 15)

void PrintWindowChain(HWND hwnd, int depth = 0) {
    WCHAR cls[256] = {}, txt[256] = {};
    GetClassNameW(hwnd, cls, 256);
    GetWindowTextW(hwnd, txt, 256);
    RECT r; GetWindowRect(hwnd, &r);
    DWORD pid; GetWindowThreadProcessId(hwnd, &pid);
    printf("%*sHWND=%p Class=[%ls] Text=[%ls] Rect=(%d,%d %dx%d) PID=%lu\n",
        depth*2, "", hwnd, cls, txt, r.left, r.top, r.right-r.left, r.bottom-r.top, pid);
}

// Enumerate child windows
BOOL CALLBACK EnumChildProc(HWND hwnd, LPARAM lp) {
    int* depth = (int*)lp;
    PrintWindowChain(hwnd, *depth);
    int d2 = *depth + 1;
    EnumChildWindows(hwnd, EnumChildProc, (LPARAM)&d2);
    return TRUE;
}

// Enumerate top-level SysListView32 windows
BOOL CALLBACK EnumTopProc(HWND hwnd, LPARAM) {
    WCHAR cls[256];
    GetClassNameW(hwnd, cls, 256);
    if (wcscmp(cls, L"SysListView32") == 0) {
        printf("\n=== Found top-level SysListView32 ===\n");
        PrintWindowChain(hwnd);
        int d = 1;
        EnumChildWindows(hwnd, EnumChildProc, (LPARAM)&d);
        // Also show parent
        HWND parent = GetParent(hwnd);
        while (parent) {
            printf("  Parent: "); PrintWindowChain(parent);
            parent = GetParent(parent);
        }
    }
    return TRUE;
}

int main() {
    printf("=== Desktop Window Hierarchy Diagnostic ===\n\n");

    // Check Progman
    HWND progman = FindWindowW(L"Progman", nullptr);
    printf("Progman: %p\n", progman);
    if (progman) {
        PrintWindowChain(progman);
        int d = 1;
        EnumChildWindows(progman, EnumChildProc, (LPARAM)&d);

        // Send the refresh message
        printf("\nSending 0x052C to Progman...\n");
        SendMessageW(progman, 0x052C, 0xD, 0);
        Sleep(500);
        printf("After refresh, Progman children:\n");
        d = 1;
        EnumChildWindows(progman, EnumChildProc, (LPARAM)&d);
    }

    // Check WorkerW
    printf("\n=== WorkerW windows ===\n");
    HWND worker = FindWindowExW(nullptr, nullptr, L"WorkerW", nullptr);
    while (worker) {
        PrintWindowChain(worker);
        int d = 1;
        EnumChildWindows(worker, EnumChildProc, (LPARAM)&d);
        worker = FindWindowExW(nullptr, worker, L"WorkerW", nullptr);
    }

    // Check all top-level SysListView32
    printf("\n=== All top-level SysListView32 ===\n");
    EnumWindows(EnumTopProc, 0);

    // Check SHELLDLL_DefView
    printf("\n=== SHELLDLL_DefView search ===\n");
    HWND defview = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    printf("Under Progman: %p\n", defview);
    if (defview) {
        HWND lv = FindWindowExW(defview, nullptr, L"SysListView32", nullptr);
        printf("  -> SysListView32: %p\n", lv);
        if (lv) {
            int count = (int)SendMessageW(lv, 0x1004, 0, 0); // LVM_GETITEMCOUNT
            printf("  -> Icon count: %d\n", count);
        }
    }

    // ── 测试 LVM_SETITEMPOSITION 跨进程 ──
    printf("\n=== Testing LVM_SETITEMPOSITION cross-process ===\n");
    HWND lv = FindWindowExW(defview, nullptr, L"SysListView32", nullptr);
    if (lv) {
        // 读取第一个图标的位置
        POINT pt = {};
        LRESULT r = SendMessageW(lv, LVM_GETITEMPOSITION, 0, (LPARAM)&pt);
        printf("Icon 0 position: (%ld, %ld)  [GetResult=%d]\n", pt.x, pt.y, (int)r);

        // 尝试移动到新位置
        int newX = pt.x + 100, newY = pt.y + 100;
        printf("Moving icon 0 to (%d, %d)...\n", newX, newY);
        LRESULT setResult = SendMessageW(lv, LVM_SETITEMPOSITION, 0, MAKELPARAM(newX, newY));
        printf("SetItemPosition result: %d\n", (int)setResult);
        Sleep(500);

        // 读回验证
        POINT pt2 = {};
        SendMessageW(lv, LVM_GETITEMPOSITION, 0, (LPARAM)&pt2);
        printf("Icon 0 after move: (%ld, %ld)\n", pt2.x, pt2.y);
        printf("Move %s\n", (pt2.x == newX && pt2.y == newY) ? "SUCCESS!" : "FAILED (ignored or auto-arranged back)");
    }

    printf("\nPress Enter to exit...\n");
    getchar();
    return 0;
}
