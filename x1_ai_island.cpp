#include <windows.h>
#include <windowsx.h>
#include <string>
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cmath>
#include "resource.h"
#include "fan_telemetry.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

// X1 AI Island v1.0
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
typedef nvmlReturn_t (__cdecl *PFN_nvmlDeviceGetPerformanceState)(nvmlDevice_t, unsigned int*);

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
    PFN_nvmlDeviceGetPerformanceState performanceState{};
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
        performanceState = sym<PFN_nvmlDeviceGetPerformanceState>("nvmlDeviceGetPerformanceState");
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
    unsigned pstate = 0;
    unsigned long long used = 0, total = 0;
    double watts = -1;
    bool utilOk = false, memoryOk = false, tempOk = false, pstateOk = false;
    bool ok = false;
} g_stats;

struct FanStats {
    DWORD fan1=0, fan2=0, status=X1_FAN_STARTING;
    DWORD mode=X1_FAN_MODE_BIOS_AUTO;
    LONG hottestTemp=-1;
    bool ok=false;
} g_fans;

struct FanTelemetryReader {
    HANDLE mapping{};
    X1FanTelemetry* view{};
    void update() {
        if(!view) {
            mapping=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,X1_FAN_MAPPING_NAME);
            if(mapping) view=static_cast<X1FanTelemetry*>(
                MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(X1FanTelemetry)));
        }
        g_fans={};
        if(!view || view->magic!=X1_FAN_MAGIC || view->version!=X1_FAN_VERSION) return;
        X1FanTelemetry copy{};
        for(int tries=0;tries<3;tries++) {
            LONG before=view->sequence;
            if(before&1) continue;
            MemoryBarrier();
            copy=*view;
            MemoryBarrier();
            if(before==view->sequence) break;
        }
        g_fans.status=copy.status;
        g_fans.fan1=copy.fan1Rpm;
        g_fans.fan2=copy.fan2Rpm;
        g_fans.mode=copy.activeMode;
        g_fans.hottestTemp=copy.hottestTempC;
        g_fans.ok=copy.status==X1_FAN_OK && GetTickCount64()-copy.updatedTick<5000;
    }
    bool requestMode(DWORD mode) {
        if(!view) update();
        if(!view || view->magic!=X1_FAN_MAGIC || view->version!=X1_FAN_VERSION ||
           mode>X1_FAN_MODE_AGGRESSIVE) return false;
        InterlockedExchange(&view->requestedMode,static_cast<LONG>(mode));
        return true;
    }
    ~FanTelemetryReader() {
        if(view) UnmapViewOfFile(view);
        if(mapping) CloseHandle(mapping);
    }
} g_fanReader;

HWND g_hwnd{};
HFONT g_font{}, g_metricsFont{}, g_smallFont{};
HBITMAP g_nvidiaLogo{};
HBRUSH g_backgroundBrush{};
std::array<std::wstring,8> g_compactParts{};
struct ExpandedDisplay {
    std::wstring status;
    std::wstring gpu;
    std::wstring temperature;
    std::wstring power;
    std::wstring performanceState;
    std::wstring vram;
    std::wstring fill;
    std::wstring cooling;
    std::wstring fan1;
    std::wstring fan2;
} g_expandedDisplay;
POINT g_dragStart{};
bool g_dragging=false;
bool g_expanded=false;
bool g_trackingMouse=false;
bool g_hoverConsumed=false;
bool g_contextOpen=false;
bool g_userHidden=false;
bool g_hoverHidden=false;
bool g_hotkeyRegistered=false;
bool g_fanHotkeyRegistered=false;
int g_hotkeyChoice=0;
const UINT_PTR STATS_TIMER_ID=1;
const UINT_PTR HOVER_TIMER_ID=2;
const UINT_PTR RESHOW_TIMER_ID=3;
const UINT_PTR ANIMATION_TIMER_ID=4;
const int HOTKEY_ID=1;
const int FAN_HOTKEY_ID=2;
const COLORREF NVIDIA_GREEN=RGB(119,185,1);
const BYTE ISLAND_OPACITY=217; // 85% keeps expanded telemetry clear while retaining translucency.
const UINT ANIMATION_INTERVAL_MS=100;
const wchar_t APP_VERSION[]=L"1.0.1";

enum class LoadLevel { Green, Yellow, Red };
enum class LoadSource { GPU, VRAM, Unavailable };

struct LoadDecision {
    unsigned gpu=0;
    unsigned vram=0;
    unsigned effective=0;
    LoadLevel level=LoadLevel::Green;
    LoadSource source=LoadSource::Unavailable;
};

struct HotkeyOption {
    UINT modifiers;
    UINT key;
    const wchar_t* label;
};

const HotkeyOption HOTKEYS[] = {
    {MOD_CONTROL, 'D', L"Ctrl+D"},
    {MOD_CONTROL|MOD_SHIFT, 'D', L"Ctrl+Shift+D"},
    {MOD_ALT, 'D', L"Alt+D"},
    {MOD_CONTROL|MOD_ALT, 'D', L"Ctrl+Alt+D"}
};

const wchar_t* SETTINGS_KEY=L"Software\\X1AIIsland";

int loadHotkeyChoice() {
    DWORD value=0, size=sizeof(value);
    if(RegGetValueW(HKEY_CURRENT_USER,SETTINGS_KEY,L"HotkeyChoice",RRF_RT_REG_DWORD,nullptr,&value,&size)==ERROR_SUCCESS
       && value < ARRAYSIZE(HOTKEYS)) return static_cast<int>(value);
    return 0;
}

void saveHotkeyChoice(int choice) {
    HKEY key{};
    if(RegCreateKeyExW(HKEY_CURRENT_USER,SETTINGS_KEY,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)==ERROR_SUCCESS) {
        DWORD value=static_cast<DWORD>(choice);
        RegSetValueExW(key,L"HotkeyChoice",0,REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value));
        RegCloseKey(key);
    }
}

bool registerToggleHotkey(HWND hwnd,int choice) {
    if(g_hotkeyRegistered) {
        UnregisterHotKey(hwnd,HOTKEY_ID);
        g_hotkeyRegistered=false;
    }
    const auto& option=HOTKEYS[choice];
    g_hotkeyRegistered=RegisterHotKey(hwnd,HOTKEY_ID,option.modifiers|MOD_NOREPEAT,option.key)!=FALSE;
    return g_hotkeyRegistered;
}

void selectHotkey(HWND hwnd,int choice) {
    if(choice<0 || choice>=static_cast<int>(ARRAYSIZE(HOTKEYS)) || choice==g_hotkeyChoice) return;
    int previous=g_hotkeyChoice;
    g_hotkeyChoice=choice;
    if(registerToggleHotkey(hwnd,choice)) {
        saveHotkeyChoice(choice);
    } else {
        g_hotkeyChoice=previous;
        registerToggleHotkey(hwnd,previous);
        MessageBoxW(hwnd,L"That shortcut is already in use by another application.",L"X1 AI Island",MB_OK|MB_ICONWARNING);
    }
}

void startAnimation(HWND hwnd) {
    if(IsWindowVisible(hwnd) && !g_userHidden && !g_hoverHidden)
        SetTimer(hwnd,ANIMATION_TIMER_ID,ANIMATION_INTERVAL_MS,nullptr);
}

void stopAnimation(HWND hwnd) {
    KillTimer(hwnd,ANIMATION_TIMER_ID);
}

void showIsland(HWND hwnd) {
    g_userHidden=false;
    g_hoverHidden=false;
    KillTimer(hwnd,RESHOW_TIMER_ID);
    ShowWindow(hwnd,SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    startAnimation(hwnd);
}

void toggleIsland(HWND hwnd) {
    if(g_userHidden || !IsWindowVisible(hwnd)) {
        showIsland(hwnd);
    } else {
        g_userHidden=true;
        g_hoverHidden=false;
        KillTimer(hwnd,HOVER_TIMER_ID);
        KillTimer(hwnd,RESHOW_TIMER_ID);
        stopAnimation(hwnd);
        ShowWindow(hwnd,SW_HIDE);
    }
}

void cancelHover(HWND hwnd) {
    KillTimer(hwnd,HOVER_TIMER_ID);
    g_trackingMouse=false;
    g_hoverConsumed=false;
}

bool cursorInside(HWND hwnd) {
    POINT p{};
    RECT r{};
    return GetCursorPos(&p) && GetWindowRect(hwnd,&r) && PtInRect(&r,p);
}

LoadLevel levelForPercent(unsigned value) {
    if(value>=80) return LoadLevel::Red;
    if(value>=50) return LoadLevel::Yellow;
    return LoadLevel::Green;
}

LoadDecision assessLoad(const Stats& s) {
    LoadDecision result{};
    result.gpu=s.utilOk ? (std::min)(s.gpu,100U) : 0;
    bool vramAvailable=s.memoryOk && s.total;
    result.vram=vramAvailable
        ? (std::min)(static_cast<unsigned>((s.used*100ULL)/s.total),100U)
        : 0;

    // The higher available metric drives the single status border.
    // GPU wins ties.
    if(s.utilOk && (!vramAvailable || result.gpu>=result.vram)) {
        result.source=LoadSource::GPU;
        result.effective=result.gpu;
    } else if(vramAvailable) {
        result.source=LoadSource::VRAM;
        result.effective=result.vram;
    }

    result.level=levelForPercent(result.effective);
    return result;
}

LoadLevel loadLevel(const Stats& s) {
    return assessLoad(s).level;
}

const wchar_t* fanModeName(DWORD mode) {
    if(mode==X1_FAN_MODE_COOL) return L"Cool";
    if(mode==X1_FAN_MODE_AGGRESSIVE) return L"Aggressive";
    return L"BIOS Auto";
}

std::wstring formatText(const wchar_t* format,...) {
    wchar_t buffer[256]{};
    va_list args;
    va_start(args,format);
    _vsnwprintf_s(buffer,ARRAYSIZE(buffer),_TRUNCATE,format,args);
    va_end(args);
    return buffer;
}

INT_PTR CALLBACK AboutDialogProc(HWND dialog,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_INITDIALOG: {
        HBITMAP logo=LoadBitmapW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDB_3S_LOGO));
        SetWindowLongPtrW(dialog,DWLP_USER,reinterpret_cast<LONG_PTR>(logo));
        SendDlgItemMessageW(dialog,IDC_ABOUT_LOGO,STM_SETIMAGE,IMAGE_BITMAP,
                            reinterpret_cast<LPARAM>(logo));
        SetDlgItemTextW(dialog,IDC_ABOUT_VERSION,formatText(L"X1 AI Island v%s",APP_VERSION).c_str());
        SendDlgItemMessageW(dialog,IDC_ABOUT_VERSION,WM_SETFONT,
                            reinterpret_cast<WPARAM>(g_metricsFont),TRUE);

        HWND owner=GetWindow(dialog,GW_OWNER);
        HMONITOR monitor=MonitorFromWindow(owner ? owner : dialog,MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        RECT window{};
        if(GetMonitorInfoW(monitor,&info) && GetWindowRect(dialog,&window)) {
            int width=window.right-window.left;
            int height=window.bottom-window.top;
            int x=info.rcWork.left+(info.rcWork.right-info.rcWork.left-width)/2;
            int y=info.rcWork.top+(info.rcWork.bottom-info.rcWork.top-height)/2;
            SetWindowPos(dialog,HWND_TOP,x,y,0,0,SWP_NOSIZE|SWP_NOACTIVATE);
        }
        return TRUE;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(GetStockObject(WHITE_BRUSH));
    case WM_CTLCOLORSTATIC: {
        HDC dc=reinterpret_cast<HDC>(wp);
        HWND control=reinterpret_cast<HWND>(lp);
        SetBkMode(dc,TRANSPARENT);
        int id=GetDlgCtrlID(control);
        if(id==IDC_ABOUT_VERSION || id==IDC_ABOUT_TITLE) SetTextColor(dc,RGB(52,148,245));
        else if(id==IDC_ABOUT_SUBTITLE) SetTextColor(dc,RGB(86,104,120));
        else if(id==IDC_ABOUT_CREDIT) SetTextColor(dc,RGB(105,105,105));
        return reinterpret_cast<INT_PTR>(GetStockObject(WHITE_BRUSH));
    }
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL) {
            EndDialog(dialog,LOWORD(wp));
            return TRUE;
        }
        break;
    case WM_DESTROY: {
        HBITMAP logo=reinterpret_cast<HBITMAP>(GetWindowLongPtrW(dialog,DWLP_USER));
        if(logo) DeleteObject(logo);
        return TRUE;
    }
    }
    return FALSE;
}

void showAbout(HWND hwnd) {
    DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_ABOUT_DIALOG),
                    hwnd,AboutDialogProc,0);
}

BYTE channel(double value) {
    return static_cast<BYTE>((std::max)(0.0,(std::min)(255.0,value)));
}

COLORREF pulseColor(LoadLevel level,double phase) {
    double rWave=(std::sin)(phase)+1.0;
    double gWave=(std::sin)(phase+0.55)+1.0;
    double bWave=(std::sin)(phase+1.10)+1.0;
    rWave*=0.5; gWave*=0.5; bWave*=0.5;

    if(level==LoadLevel::Green) {
        // NVIDIA-green breathing: hue remains green while brightness rises
        // and falls gently.
        double breath=0.45+0.55*gWave;
        return RGB(channel(119.0*breath),channel(185.0*breath),channel(1.0+4.0*bWave));
    }
    if(level==LoadLevel::Yellow) {
        // Yellow-dominant RGB pulse: red and green breathe with a small
        // blue phase offset, while the overall hue remains yellow.
        return RGB(channel(205.0+50.0*rWave),channel(145.0+100.0*gWave),channel(4.0+24.0*bWave));
    }
    // Red-biased RGB breathing: a strong red channel with subtle green/blue
    // phase shifts creates the red-shifted accent-line effect.
    return RGB(channel(170.0+85.0*rWave),channel(5.0+42.0*gWave),channel(5.0+30.0*bWave));
}

COLORREF animatedBorderColor(LoadLevel level,ULONGLONG now) {
    double period=level==LoadLevel::Green ? 2400.0 : (level==LoadLevel::Yellow ? 1400.0 : 1050.0);
    double phase=6.283185307179586*(static_cast<double>(now%static_cast<ULONGLONG>(period))/period);
    return pulseColor(level,phase);
}

COLORREF gpuTextColor(const Stats& s,ULONGLONG now) {
    if(!s.utilOk) return RGB(110,110,116);
    LoadLevel level=levelForPercent((std::min)(s.gpu,100U));
    // Keep the normal state visually steady; only warning and critical GPU
    // load levels animate.
    if(level==LoadLevel::Green) return NVIDIA_GREEN;
    return animatedBorderColor(level,now);
}

void updateStats() {
    Stats s{};
    if (g_nvml.ready) {
        nvmlUtilization_t u{};
        nvmlMemory_t m{};
        unsigned t=0, p=0, ps=0;
        // A laptop driver may temporarily reject utilization queries while
        // still providing memory, temperature and power. Read independently.
        s.utilOk = g_nvml.util(g_nvml.device,&u)==0;
        s.memoryOk = g_nvml.memory(g_nvml.device,&m)==0;
        s.tempOk = g_nvml.temp(g_nvml.device,0,&t)==0;
        if(s.utilOk) { s.gpu=u.gpu; s.memUtil=u.memory; }
        if(s.memoryOk) { s.used=m.used; s.total=m.total; }
        if(s.tempOk) s.temp=t;
        if (g_nvml.power && g_nvml.power(g_nvml.device,&p)==0) s.watts=p/1000.0;
        s.pstateOk = g_nvml.performanceState && g_nvml.performanceState(g_nvml.device,&ps)==0;
        if(s.pstateOk) s.pstate=ps;
        s.ok = s.utilOk || s.memoryOk || s.tempOk || s.watts>=0 || s.pstateOk;
    }
    g_stats=s;
    g_fanReader.update();
}

void setWindowSize() {
    int w = 560;
    int h = g_expanded ? 128 : 46;
    RECT r{}; GetWindowRect(g_hwnd,&r);
    SetWindowPos(g_hwnd,HWND_TOPMOST,r.left,r.top,w,h,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    HRGN region=CreateRoundRectRgn(0,0,w+1,h+1,24,24);
    SetWindowRgn(g_hwnd,region,TRUE);
}

std::array<std::wstring,8> compactSegments() {
    std::array<std::wstring,8> parts{};
    parts[0]=g_stats.utilOk ? formatText(L"%u%%",g_stats.gpu) : L"N/A";
    if(g_stats.memoryOk) {
        constexpr double GIB=1073741824.0;
        double usedGB=g_stats.used/GIB;
        double totalGB=g_stats.total/GIB;
        if(g_stats.total%1073741824ULL==0)
            parts[1]=formatText(L"%.1f/%lluG",usedGB,g_stats.total/1073741824ULL);
        else
            parts[1]=formatText(L"%.1f/%.1fG",usedGB,totalGB);
    } else {
        parts[1]=L"VRAM N/A";
    }
    parts[2]=g_stats.pstateOk ? formatText(L"P%u",g_stats.pstate) : L"P?";
    parts[3]=g_stats.tempOk ? formatText(L"%u\u00B0C",g_stats.temp) : L"Temp N/A";
    parts[4]=g_stats.watts>=0 ? formatText(L"%.0fW",g_stats.watts) : L"--W";
    if(g_fans.ok && g_fans.mode==X1_FAN_MODE_COOL) parts[5]=L"COOL";
    if(g_fans.ok && g_fans.mode==X1_FAN_MODE_AGGRESSIVE) parts[5]=L"AGGR";
    parts[6]=g_fans.ok ? formatText(L"F1 %lu",g_fans.fan1) : L"F1 --";
    parts[7]=g_fans.ok ? formatText(L"F2 %lu",g_fans.fan2) : L"F2 --";
    return parts;
}

std::wstring compactMetrics() {
    auto parts=compactSegments();
    std::wstring result;
    for(const auto& part:parts) {
        if(part.empty()) continue;
        if(!result.empty()) result.push_back(L' ');
        result+=part;
    }
    return result;
}

void refreshDisplayCache() {
    g_compactParts=compactSegments();
    g_expandedDisplay={};
    LoadDecision decision=assessLoad(g_stats);

    if(g_stats.ok) {
        g_expandedDisplay.gpu=g_stats.utilOk
            ? formatText(L"GPU Load  %u%%",decision.gpu)
            : L"GPU Load  N/A";
        g_expandedDisplay.temperature=g_stats.tempOk
            ? formatText(L"Temperature  %u\u00B0C",g_stats.temp)
            : L"Temperature  N/A";
        g_expandedDisplay.power=g_stats.watts>=0
            ? formatText(L"Power  %.1fW",g_stats.watts)
            : L"Power  N/A";
        g_expandedDisplay.performanceState=g_stats.pstateOk
            ? formatText(L"Performance State  P%u",g_stats.pstate)
            : L"Performance State  P?";
        if(g_stats.memoryOk) {
            g_expandedDisplay.vram=formatText(
                L"VRAM  %.2f / %.2f GB",
                g_stats.used/1073741824.0,g_stats.total/1073741824.0);
            g_expandedDisplay.fill=formatText(L"Fill  %u%%",decision.vram);
        } else {
            g_expandedDisplay.vram=L"VRAM  N/A";
            g_expandedDisplay.fill=L"Fill  N/A";
        }
    } else {
        g_expandedDisplay.status=g_nvml.ready
            ? L"GPU readings temporarily unavailable; retrying every second."
            : L"NVIDIA NVML could not be initialized. Check NVIDIA driver.";
    }

    g_expandedDisplay.cooling=g_fans.ok
        ? formatText(L"Cooling  %s",fanModeName(g_fans.mode))
        : L"Cooling  Service unavailable";
    if(g_fans.ok) {
        g_expandedDisplay.fan1=formatText(L"Fan 1  %lu RPM",g_fans.fan1);
        g_expandedDisplay.fan2=formatText(L"Fan 2  %lu RPM",g_fans.fan2);
    }
}

void requestFanMode(HWND hwnd,DWORD mode) {
    if(!g_fanReader.requestMode(mode))
        MessageBoxW(hwnd,L"X1FanService is unavailable.",L"X1 AI Island",
                    MB_OK|MB_ICONERROR);
}

void showFanModeMenu(HWND hwnd) {
    cancelHover(hwnd);
    HMENU menu=CreatePopupMenu();
    AppendMenuW(menu,MF_STRING,200,L"BIOS Auto (default)");
    AppendMenuW(menu,MF_STRING,201,L"Cool");
    AppendMenuW(menu,MF_STRING,202,L"Aggressive");
    CheckMenuRadioItem(menu,200,202,200+(std::min)(g_fans.mode,DWORD{2}),MF_BYCOMMAND);

    RECT r{}; GetWindowRect(hwnd,&r);
    POINT p{r.left+(r.right-r.left)/2,r.bottom};
    SetForegroundWindow(hwnd);
    g_contextOpen=true;
    int cmd=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,hwnd,nullptr);
    g_contextOpen=false;
    DestroyMenu(menu);
    PostMessageW(hwnd,WM_NULL,0,0);
    if(cmd>=200 && cmd<=202) requestFanMode(hwnd,static_cast<DWORD>(cmd-200));
}

void drawNvidiaLogo(HDC dc,int x,int y) {
    if(!g_nvidiaLogo) return;
    HDC logoDc=CreateCompatibleDC(dc);
    HGDIOBJ old=SelectObject(logoDc,g_nvidiaLogo);
    BitBlt(dc,x,y,32,22,logoDc,0,0,SRCCOPY);
    SelectObject(logoDc,old);
    DeleteDC(logoDc);
}

void paint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc=BeginPaint(hwnd,&ps);
    RECT rc{}; GetClientRect(hwnd,&rc);
    FillRect(dc,&rc,g_backgroundBrush);

    LoadDecision decision=assessLoad(g_stats);
    ULONGLONG now=GetTickCount64();
    HPEN border=CreatePen(PS_SOLID,1,animatedBorderColor(decision.level,now));
    HGDIOBJ oldPen=SelectObject(dc,border);
    HGDIOBJ oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
    RoundRect(dc,1,1,rc.right-1,rc.bottom-1,24,24);
    SelectObject(dc,oldBrush); SelectObject(dc,oldPen);
    DeleteObject(border);

    SetBkMode(dc,TRANSPARENT);
    SelectObject(dc,g_font);

    drawNvidiaLogo(dc,10,12);
    // The GPU name reflects GPU utilization only. The border continues to
    // reflect the higher pressure of GPU utilization and VRAM fill.
    SetTextColor(dc,gpuTextColor(g_stats,now));
    RECT nameLine={47,4,121,42};
    DrawTextW(dc,L"RTX 3080",-1,&nameLine,DT_LEFT|DT_VCENTER|DT_SINGLELINE);

    SetTextColor(dc,RGB(242,242,245));
    SelectObject(dc,g_metricsFont);
    const auto& parts=g_compactParts;
    std::array<SIZE,8> sizes{};
    int totalWidth=0;
    int activeSegments=0;
    for(size_t i=0;i<parts.size();++i) {
        if(parts[i].empty()) continue;
        GetTextExtentPoint32W(dc,parts[i].c_str(),static_cast<int>(parts[i].size()),&sizes[i]);
        totalWidth+=sizes[i].cx;
        activeSegments++;
    }
    const int left=126;
    const int right=rc.right-4;
    int extra=(std::max)(0,right-left-totalWidth);
    int gapCount=(std::max)(1,activeSegments-1);
    int gap=extra/gapCount;
    int remainder=extra%gapCount;
    int x=left;
    int drawn=0;
    for(size_t i=0;i<parts.size();++i) {
        if(parts[i].empty()) continue;
        if(i==5 && g_fans.mode==X1_FAN_MODE_COOL) {
            SetTextColor(dc,NVIDIA_GREEN);
        } else if(i==5 && g_fans.mode==X1_FAN_MODE_AGGRESSIVE) {
            LoadLevel badgeLevel=decision.level==LoadLevel::Red ? LoadLevel::Red : LoadLevel::Yellow;
            SetTextColor(dc,animatedBorderColor(badgeLevel,now));
        } else {
            SetTextColor(dc,RGB(242,242,245));
        }
        int y=(46-sizes[i].cy)/2;
        TextOutW(dc,x,y,parts[i].c_str(),static_cast<int>(parts[i].size()));
        drawn++;
        if(drawn<activeSegments)
            x+=sizes[i].cx+gap+(drawn<=remainder?1:0);
    }

    if(g_expanded) {
        SelectObject(dc,g_smallFont);
        SetTextColor(dc,RGB(190,190,198));
        constexpr UINT LEFT_CELL=DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS;
        constexpr UINT RIGHT_CELL=DT_RIGHT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS;
        RECT gpuCell={16,45,120,72};
        RECT tempCell={120,45,274,72};
        RECT powerCell={274,45,376,72};
        RECT stateCell={376,45,rc.right-16,72};
        RECT vramCell={16,72,392,99};
        RECT fillCell={392,72,rc.right-16,99};
        RECT coolingCell={16,99,220,124};
        RECT fan1Cell={220,99,384,124};
        RECT fan2Cell={384,99,rc.right-16,124};

        if(!g_expandedDisplay.status.empty()) {
            RECT statusCell={16,45,rc.right-16,72};
            DrawTextW(dc,g_expandedDisplay.status.c_str(),-1,&statusCell,LEFT_CELL);
        } else {
            DrawTextW(dc,g_expandedDisplay.gpu.c_str(),-1,&gpuCell,LEFT_CELL);
            DrawTextW(dc,g_expandedDisplay.temperature.c_str(),-1,&tempCell,LEFT_CELL);
            DrawTextW(dc,g_expandedDisplay.power.c_str(),-1,&powerCell,LEFT_CELL);
            DrawTextW(dc,g_expandedDisplay.performanceState.c_str(),-1,&stateCell,RIGHT_CELL);
            DrawTextW(dc,g_expandedDisplay.vram.c_str(),-1,&vramCell,LEFT_CELL);
            DrawTextW(dc,g_expandedDisplay.fill.c_str(),-1,&fillCell,RIGHT_CELL);
        }
        DrawTextW(dc,g_expandedDisplay.cooling.c_str(),-1,&coolingCell,LEFT_CELL);
        DrawTextW(dc,g_expandedDisplay.fan1.c_str(),-1,&fan1Cell,LEFT_CELL);
        DrawTextW(dc,g_expandedDisplay.fan2.c_str(),-1,&fan2Cell,RIGHT_CELL);
    }
    EndPaint(hwnd,&ps);
}

LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE:
        SetTimer(hwnd,STATS_TIMER_ID,1000,nullptr);
        SetTimer(hwnd,ANIMATION_TIMER_ID,ANIMATION_INTERVAL_MS,nullptr);
        g_hotkeyChoice=loadHotkeyChoice();
        registerToggleHotkey(hwnd,g_hotkeyChoice);
        g_fanHotkeyRegistered=RegisterHotKey(
            hwnd,FAN_HOTKEY_ID,MOD_CONTROL|MOD_SHIFT|MOD_NOREPEAT,'F')!=FALSE;
        return 0;
    case WM_TIMER:
        if(wp==STATS_TIMER_ID) {
            updateStats();
            refreshDisplayCache();
            InvalidateRect(hwnd,nullptr,FALSE);
        } else if(wp==ANIMATION_TIMER_ID) {
            InvalidateRect(hwnd,nullptr,FALSE);
        } else if(wp==HOVER_TIMER_ID) {
            KillTimer(hwnd,HOVER_TIMER_ID);
            if(!g_contextOpen && !g_dragging && !g_userHidden && cursorInside(hwnd)) {
                g_hoverConsumed=true;
                g_hoverHidden=true;
                g_trackingMouse=false;
                stopAnimation(hwnd);
                ShowWindow(hwnd,SW_HIDE);
                SetTimer(hwnd,RESHOW_TIMER_ID,5000,nullptr);
            }
        } else if(wp==RESHOW_TIMER_ID) {
            KillTimer(hwnd,RESHOW_TIMER_ID);
            if(g_hoverHidden && !g_userHidden) {
                showIsland(hwnd);
                if(cursorInside(hwnd)) {
                    TRACKMOUSEEVENT tme{sizeof(tme),TME_LEAVE,hwnd,0};
                    TrackMouseEvent(&tme);
                    g_trackingMouse=true;
                } else {
                    g_hoverConsumed=false;
                }
            }
        }
        return 0;
    case WM_HOTKEY:
        if(wp==HOTKEY_ID) toggleIsland(hwnd);
        if(wp==FAN_HOTKEY_ID) showFanModeMenu(hwnd);
        return 0;
    case WM_LBUTTONDBLCLK:
        g_expanded=!g_expanded; setWindowSize(); return 0;
    case WM_LBUTTONDOWN:
        cancelHover(hwnd);
        g_dragging=true; SetCapture(hwnd);
        g_dragStart={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        return 0;
    case WM_MOUSEMOVE:
        if(g_dragging && (wp & MK_LBUTTON)) {
            POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; ClientToScreen(hwnd,&p);
            SetWindowPos(hwnd,HWND_TOPMOST,p.x-g_dragStart.x,p.y-g_dragStart.y,0,0,SWP_NOSIZE|SWP_NOACTIVATE);
        } else if(!g_trackingMouse && !g_contextOpen) {
            TRACKMOUSEEVENT tme{sizeof(tme),TME_LEAVE,hwnd,0};
            TrackMouseEvent(&tme);
            g_trackingMouse=true;
            if(!g_hoverConsumed) SetTimer(hwnd,HOVER_TIMER_ID,1000,nullptr);
        }
        return 0;
    case WM_MOUSELEAVE:
        cancelHover(hwnd);
        return 0;
    case WM_LBUTTONUP:
        g_dragging=false; ReleaseCapture(); return 0;
    case WM_RBUTTONDOWN:
        cancelHover(hwnd);
        return 0;
    case WM_RBUTTONUP: {
        cancelHover(hwnd);
        HMENU m=CreatePopupMenu();
        HMENU fanModes=CreatePopupMenu();
        HMENU shortcuts=CreatePopupMenu();
        AppendMenuW(m,MF_STRING,1,L"Expand / Collapse");
        AppendMenuW(fanModes,MF_STRING,200,L"BIOS Auto (default)");
        AppendMenuW(fanModes,MF_STRING,201,L"Cool");
        AppendMenuW(fanModes,MF_STRING,202,L"Aggressive");
        CheckMenuRadioItem(fanModes,200,202,200+(std::min)(g_fans.mode,DWORD{2}),MF_BYCOMMAND);
        std::wstring fanLabel=L"Fan Control";
        if(g_fans.ok) {
            fanLabel+=L" - ";
            fanLabel+=fanModeName(g_fans.mode);
        }
        fanLabel+=L"\tCtrl+Shift+F";
        AppendMenuW(m,MF_POPUP|(g_fans.ok?0:MF_GRAYED),
                    reinterpret_cast<UINT_PTR>(fanModes),fanLabel.c_str());
        std::wstring hideLabel=L"Hide Island\t";
        hideLabel+=HOTKEYS[g_hotkeyChoice].label;
        AppendMenuW(m,MF_STRING,3,hideLabel.c_str());
        AppendMenuW(m,MF_SEPARATOR,0,nullptr);
        for(int i=0;i<static_cast<int>(ARRAYSIZE(HOTKEYS));++i) {
            UINT flags=MF_STRING|(i==g_hotkeyChoice?MF_CHECKED:0);
            AppendMenuW(shortcuts,flags,100+i,HOTKEYS[i].label);
        }
        AppendMenuW(m,MF_POPUP,reinterpret_cast<UINT_PTR>(shortcuts),L"Hide / show shortcut");
        AppendMenuW(m,MF_SEPARATOR,0,nullptr);
        AppendMenuW(m,MF_STRING,5,L"About X1 AI Island");
        AppendMenuW(m,MF_STRING,2,L"Exit");
        POINT p{}; GetCursorPos(&p);
        SetForegroundWindow(hwnd);
        g_contextOpen=true;
        int cmd=TrackPopupMenu(m,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,hwnd,nullptr);
        g_contextOpen=false;
        DestroyMenu(m);
        if(cmd==1){g_expanded=!g_expanded;setWindowSize();}
        if(cmd>=200 && cmd<=202) {
            requestFanMode(hwnd,static_cast<DWORD>(cmd-200));
        }
        if(cmd==3)toggleIsland(hwnd);
        if(cmd>=100 && cmd<100+static_cast<int>(ARRAYSIZE(HOTKEYS))) selectHotkey(hwnd,cmd-100);
        if(cmd==5)showAbout(hwnd);
        if(cmd==2)DestroyWindow(hwnd);
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wp)>=200 && LOWORD(wp)<=202) {
            requestFanMode(hwnd,static_cast<DWORD>(LOWORD(wp)-200));
            return 0;
        }
        if(LOWORD(wp)==5) {
            showAbout(hwnd);
            return 0;
        }
        break;
    case WM_PAINT: paint(hwnd); return 0;
    case WM_DESTROY:
        KillTimer(hwnd,STATS_TIMER_ID);
        KillTimer(hwnd,HOVER_TIMER_ID);
        KillTimer(hwnd,RESHOW_TIMER_ID);
        KillTimer(hwnd,ANIMATION_TIMER_ID);
        if(g_hotkeyRegistered) UnregisterHotKey(hwnd,HOTKEY_ID);
        if(g_fanHotkeyRegistered) UnregisterHotKey(hwnd,FAN_HOTKEY_ID);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

int WINAPI wWinMain(HINSTANCE h,HINSTANCE,LPWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_nvml.load();
    updateStats();
    refreshDisplayCache();

    g_font=CreateFontW(-16,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    g_metricsFont=CreateFontW(-14,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    g_smallFont=CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    g_nvidiaLogo=LoadBitmapW(h,MAKEINTRESOURCEW(IDB_NVIDIA_LOGO));
    g_backgroundBrush=CreateSolidBrush(RGB(18,18,20));

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style=CS_HREDRAW|CS_VREDRAW|CS_DBLCLKS;
    wc.lpfnWndProc=WndProc; wc.hInstance=h; wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    wc.lpszClassName=L"X1AIIslandClass";
    RegisterClassExW(&wc);

    int sw=GetSystemMetrics(SM_CXSCREEN);
    const int initialWidth=560;
    const int initialX=(std::max)(0,(sw-initialWidth)/2);
    g_hwnd=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_LAYERED,
        wc.lpszClassName,L"X1 AI Island",WS_POPUP,
        initialX,18,initialWidth,46,nullptr,nullptr,h,nullptr);
    if(!g_hwnd) return 1;
    SetLayeredWindowAttributes(g_hwnd,0,ISLAND_OPACITY,LWA_ALPHA);
    setWindowSize();
    ShowWindow(g_hwnd,SW_SHOWNOACTIVATE);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    DeleteObject(g_font); DeleteObject(g_metricsFont); DeleteObject(g_smallFont);
    if(g_nvidiaLogo) DeleteObject(g_nvidiaLogo);
    if(g_backgroundBrush) DeleteObject(g_backgroundBrush);
    return 0;
}
