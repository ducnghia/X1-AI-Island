#include <cstdio>
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

// X1 AI Island v0.1
// Native Win32 overlay. NVIDIA telemetry is queried by dynamically loading
// nvml.dll from the installed NVIDIA driver: no CUDA SDK/NVML headers needed.
// UI rendering remains ordinary Win32/GDI and does not intentionally create
// a D3D context on the NVIDIA GPU.

typedef void* nvmlDevice_t;
typedef int nvmlReturn_t;
struct nvmlMemory_t { unsigned long long total, free, used; };
struct nvmlUtilization_t { unsigned int gpu, memory; };

typedef nvmlReturn_t (__cdecl *PFN_nvmlInit_v2)();
typedef nvmlReturn_t (__cdecl *PFN_nvmlShutdown)();
typedef nvmlReturn_t (__cdecl *PFN_nvmlDeviceGetCount_v2)(unsigned int*);
typedef nvmlReturn_t (__cdecl *PFN_nvmlDeviceGetHandleByIndex_v2)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (__cdecl *PFN_nvmlDeviceGetName)(nvmlDevice_t, char*, unsigned int);
typedef nvmlReturn_t (__cdecl *PFN_nvmlDeviceGetUtilizationRates)(nvmlDevice_t, nvmlUtilization_t*);
typedef nvmlReturn_t (__cdecl *PFN_nvmlDeviceGetMemoryInfo)(nvmlDevice_t, nvmlMemory_t*);
typedef nvmlReturn_t (__cdecl *PFN_nvmlDeviceGetTemperature)(nvmlDevice_t, unsigned int, unsigned int*);
typedef nvmlReturn_t (__cdecl *PFN_nvmlDeviceGetPowerUsage)(nvmlDevice_t, unsigned int*);

struct NvmlApi {
    HMODULE dll{};
    PFN_nvmlInit_v2 init{};
    PFN_nvmlShutdown shutdown{};
    PFN_nvmlDeviceGetCount_v2 count{};
    PFN_nvmlDeviceGetHandleByIndex_v2 handle{};
    PFN_nvmlDeviceGetName name{};
    PFN_nvmlDeviceGetUtilizationRates util{};
    PFN_nvmlDeviceGetMemoryInfo memory{};
    PFN_nvmlDeviceGetTemperature temp{};
    PFN_nvmlDeviceGetPowerUsage power{};
    nvmlDevice_t device{};
    std::string deviceName = "NVIDIA GPU";
    bool ready = false;

    template<class T> T sym(const char* n) { return reinterpret_cast<T>(GetProcAddress(dll, n)); }

    bool load() {
        const wchar_t* candidates[] = {
            L"nvml.dll",
            L"C:\\Windows\\System32\\nvml.dll",
            L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll"
        };
        for (auto p : candidates) { dll = LoadLibraryW(p); if (dll) break; }
        if (!dll) return false;
        init = sym<PFN_nvmlInit_v2>("nvmlInit_v2");
        shutdown = sym<PFN_nvmlShutdown>("nvmlShutdown");
        count = sym<PFN_nvmlDeviceGetCount_v2>("nvmlDeviceGetCount_v2");
        handle = sym<PFN_nvmlDeviceGetHandleByIndex_v2>("nvmlDeviceGetHandleByIndex_v2");
        name = sym<PFN_nvmlDeviceGetName>("nvmlDeviceGetName");
        util = sym<PFN_nvmlDeviceGetUtilizationRates>("nvmlDeviceGetUtilizationRates");
        memory = sym<PFN_nvmlDeviceGetMemoryInfo>("nvmlDeviceGetMemoryInfo");
        temp = sym<PFN_nvmlDeviceGetTemperature>("nvmlDeviceGetTemperature");
        power = sym<PFN_nvmlDeviceGetPowerUsage>("nvmlDeviceGetPowerUsage");
        if (!init || !count || !handle || !name || !util || !memory || !temp) return false;
        if (init() != 0) return false;
        unsigned int n = 0;
        if (count(&n) != 0 || n == 0) return false;

        // Prefer an RTX 3080 if present; otherwise first NVIDIA device.
        nvmlDevice_t first{};
        for (unsigned int i=0; i<n; ++i) {
            nvmlDevice_t d{};
            if (handle(i, &d) != 0) continue;
            if (!first) first = d;
            char buf[128]{};
            if (name(d, buf, sizeof(buf)) == 0) {
                std::string s(buf);
                if (s.find("RTX 3080") != std::string::npos) {
                    device = d; deviceName = s; break;
                }
                if (!device) { device = d; deviceName = s; }
            }
        }
        if (!device) device = first;
        ready = device != nullptr;
        return ready;
    }
    ~NvmlApi() {
        if (ready && shutdown) shutdown();
        if (dll) FreeLibrary(dll);
    }
} g_nvml;

struct Stats {
    unsigned gpu = 0, memUtil = 0, temp = 0;
    unsigned long long used = 0, total = 0;
    double watts = -1;
    bool ok = false;
} g_stats;

HWND g_hwnd{};
HFONT g_font{}, g_smallFont{};
POINT g_dragStart{};
bool g_dragging=false;
bool g_expanded=false;
const UINT_PTR TIMER_ID=1;

void updateStats() {
    Stats s{};
    if (g_nvml.ready) {
        nvmlUtilization_t u{};
        nvmlMemory_t m{};
        unsigned t=0, p=0;
        if (g_nvml.util(g_nvml.device,&u)==0 &&
            g_nvml.memory(g_nvml.device,&m)==0 &&
            g_nvml.temp(g_nvml.device,0,&t)==0) {
            s.gpu=u.gpu; s.memUtil=u.memory; s.used=m.used; s.total=m.total; s.temp=t; s.ok=true;
            if (g_nvml.power && g_nvml.power(g_nvml.device,&p)==0) s.watts=p/1000.0;
        }
    }
    g_stats=s;
}

std::wstring widen(const std::string& s) {
    if(s.empty()) return L"";
    int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);
    std::wstring w(n? n-1:0,L'\0');
    if(n>1) MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,w.data(),n);
    return w;
}

void setWindowSize() {
    int w = g_expanded ? 460 : 430;
    int h = g_expanded ? 150 : 58;
    RECT r{}; GetWindowRect(g_hwnd,&r);
    SetWindowPos(g_hwnd,HWND_TOPMOST,r.left,r.top,w,h,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    HRGN region=CreateRoundRectRgn(0,0,w+1,h+1,28,28);
    SetWindowRgn(g_hwnd,region,TRUE);
}

std::wstring oneLine() {
    if(!g_stats.ok) return L"RTX 3080   NVML unavailable";
    double usedGB=g_stats.used/1073741824.0, totalGB=g_stats.total/1073741824.0;
    std::wstringstream ss;
    ss << L"RTX 3080   " << g_stats.gpu << L"%   "
       << std::fixed << std::setprecision(1) << usedGB << L"/" << totalGB << L"G   "
       << g_stats.temp << L"\u00B0C";
    if(g_stats.watts>=0) ss << L"   " << std::setprecision(0) << g_stats.watts << L"W";
    return ss.str();
}

void paint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc=BeginPaint(hwnd,&ps);
    RECT rc{}; GetClientRect(hwnd,&rc);
    HBRUSH bg=CreateSolidBrush(RGB(18,18,20));
    FillRect(dc,&rc,bg); DeleteObject(bg);
    SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,RGB(242,242,245));
    SelectObject(dc,g_font);

    RECT line={20,10,rc.right-20,48};
    auto text=oneLine(); static bool logged=false; if(!logged){ FILE* f=nullptr; fopen_s(&f,"diagnostics/render.log","w"); if(f){fwprintf(f,L"ready=%d ok=%d font=%p dc=%p rect=%ld,%ld text=%ls\n",g_nvml.ready,g_stats.ok,g_font,dc,rc.right,rc.bottom,text.c_str()); fclose(f);} logged=true;}
    DrawTextW(dc,text.c_str(),-1,&line,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);

    if(g_expanded) {
        SelectObject(dc,g_smallFont);
        SetTextColor(dc,RGB(190,190,198));
        std::wstringstream a,b;
        if(g_stats.ok) {
            a << L"GPU utilization    " << g_stats.gpu << L"%        GPU memory engine    " << g_stats.memUtil << L"%";
            b << L"Dedicated VRAM     " << std::fixed << std::setprecision(2)
              << g_stats.used/1073741824.0 << L" / " << g_stats.total/1073741824.0
              << L" GB        Temperature    " << g_stats.temp << L"\u00B0C";
            if(g_stats.watts>=0) b << L"        Power    " << std::setprecision(1) << g_stats.watts << L"W";
        } else {
            a << L"NVIDIA NVML could not be loaded. Check NVIDIA driver / nvml.dll.";
            b << L"The overlay itself does not require CUDA Toolkit.";
        }
        RECT r1={20,58,rc.right-20,88}, r2={20,91,rc.right-20,130};
        DrawTextW(dc,a.str().c_str(),-1,&r1,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawTextW(dc,b.str().c_str(),-1,&r2,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    }
    EndPaint(hwnd,&ps);
}

LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE:
        SetTimer(hwnd,TIMER_ID,1000,nullptr);
        return 0;
    case WM_TIMER:
        updateStats(); InvalidateRect(hwnd,nullptr,FALSE); return 0;
    case WM_LBUTTONDBLCLK:
        g_expanded=!g_expanded; setWindowSize(); return 0;
    case WM_LBUTTONDOWN:
        g_dragging=true; SetCapture(hwnd);
        g_dragStart={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        return 0;
    case WM_MOUSEMOVE:
        if(g_dragging && (wp & MK_LBUTTON)) {
            POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; ClientToScreen(hwnd,&p);
            SetWindowPos(hwnd,HWND_TOPMOST,p.x-g_dragStart.x,p.y-g_dragStart.y,0,0,SWP_NOSIZE|SWP_NOACTIVATE);
        }
        return 0;
    case WM_LBUTTONUP:
        g_dragging=false; ReleaseCapture(); return 0;
    case WM_RBUTTONUP: {
        HMENU m=CreatePopupMenu();
        AppendMenuW(m,MF_STRING,1,L"Expand / Collapse");
        AppendMenuW(m,MF_SEPARATOR,0,nullptr);
        AppendMenuW(m,MF_STRING,2,L"Exit");
        POINT p{}; GetCursorPos(&p);
        SetForegroundWindow(hwnd);
        int cmd=TrackPopupMenu(m,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,hwnd,nullptr);
        DestroyMenu(m);
        if(cmd==1){g_expanded=!g_expanded;setWindowSize();}
        if(cmd==2)DestroyWindow(hwnd);
        return 0;
    }
    case WM_PAINT: paint(hwnd); return 0;
    case WM_DESTROY:
        KillTimer(hwnd,TIMER_ID); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

int WINAPI wWinMain(HINSTANCE h,HINSTANCE,LPWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_nvml.load();
    updateStats();

    g_font=CreateFontW(-19,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    g_smallFont=CreateFontW(-15,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style=CS_HREDRAW|CS_VREDRAW|CS_DBLCLKS;
    wc.lpfnWndProc=WndProc; wc.hInstance=h; wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    wc.lpszClassName=L"X1AIIslandClass";
    RegisterClassExW(&wc);

    int sw=GetSystemMetrics(SM_CXSCREEN);
    g_hwnd=CreateWindowExW(WS_EX_TOPMOST,
        wc.lpszClassName,L"X1 AI Island",WS_POPUP,
        sw-450,18,430,58,nullptr,nullptr,h,nullptr);
    if(!g_hwnd) return 1;
    setWindowSize();
    ShowWindow(g_hwnd,SW_SHOWNOACTIVATE);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    DeleteObject(g_font); DeleteObject(g_smallFont);
    return 0;
}

