#include <windows.h>
#include <cstdio>
BOOL CALLBACK inspect(HWND w, LPARAM) {
    wchar_t cls[128]{}; GetClassNameW(w,cls,128);
    if (wcscmp(cls,L"X1AIIslandClass")) return TRUE;
    RECT r{}; GetWindowRect(w,&r); DWORD pid{}; GetWindowThreadProcessId(w,&pid);
    printf("pid=%lu visible=%d rect=%ld,%ld,%ld,%ld hung=%d\n",pid,IsWindowVisible(w),r.left,r.top,r.right,r.bottom,IsHungAppWindow(w));
    return TRUE;
}
int main() { EnumWindows(inspect,0); }
