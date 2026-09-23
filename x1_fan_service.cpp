#include <windows.h>
#include <sddl.h>
#include <string>
#include <cstring>
#include "fan_telemetry.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace {
constexpr wchar_t SERVICE_NAME[] = L"X1FanService";
constexpr wchar_t PAWN_DEVICE[] = L"\\\\?\\GLOBALROOT\\Device\\PawnIO";
constexpr DWORD IOCTL_LOAD_BINARY = 0xA1B22084;
constexpr DWORD IOCTL_EXECUTE_FN = 0xA1B22104;
constexpr WORD EC_DATA = 0x62, EC_STATUS = 0x66;
constexpr BYTE OBF = 0x01, IBF = 0x02, CMD_READ = 0x80, CMD_WRITE = 0x81;
constexpr BYTE REG_FAN_SELECT = 0x31, REG_FAN_LO = 0x84, REG_FAN_HI = 0x85;
constexpr BYTE REG_FAN_CTRL = 0x2f, REG_TEMP0 = 0x78, REG_TEMP1 = 0xc0;
constexpr BYTE FAN_BIOS = 0x80;

SERVICE_STATUS_HANDLE g_statusHandle{};
SERVICE_STATUS g_serviceStatus{};
HANDLE g_stopEvent{}, g_commandEvent{}, g_mapping{};
X1FanTelemetry* g_shared{};
bool g_consoleMode=false;

void publish(DWORD status,DWORD fan1=0,DWORD fan2=0,DWORD error=0) {
    if(!g_shared) return;
    InterlockedIncrement(&g_shared->sequence);
    MemoryBarrier();
    g_shared->status=status;
    g_shared->fan1Rpm=fan1;
    g_shared->fan2Rpm=fan2;
    g_shared->updatedTick=GetTickCount64();
    g_shared->lastError=error;
    MemoryBarrier();
    InterlockedIncrement(&g_shared->sequence);
}

bool supportedModel() {
    HKEY key{};
    if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"HARDWARE\\DESCRIPTION\\System\\BIOS",0,KEY_READ,&key)!=ERROR_SUCCESS) return false;
    wchar_t value[256]{}; DWORD type=0,size=sizeof(value);
    LONG rc=RegQueryValueExW(key,L"SystemProductName",nullptr,&type,reinterpret_cast<BYTE*>(value),&size);
    RegCloseKey(key);
    return rc==ERROR_SUCCESS && type==REG_SZ && wcsncmp(value,L"20Y6",4)==0;
}

std::wstring exeDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr,path,MAX_PATH);
    std::wstring p(path);
    auto slash=p.find_last_of(L"\\/");
    return slash==std::wstring::npos ? L"." : p.substr(0,slash);
}

class PawnEc {
    HANDLE device_=INVALID_HANDLE_VALUE;
    HANDLE mutex_{};

    bool execute(const char* name,const ULONGLONG* input,DWORD inputCount,
                 ULONGLONG* output,DWORD outputCount) {
        if(inputCount>2) return false;
        BYTE packet[32+2*sizeof(ULONGLONG)]{};
        size_t n=strlen(name);
        if(n>=32) return false;
        memcpy(packet,name,n);
        if(inputCount) memcpy(packet+32,input,inputCount*sizeof(ULONGLONG));
        DWORD returned=0;
        return DeviceIoControl(device_,IOCTL_EXECUTE_FN,packet,
            32+inputCount*sizeof(ULONGLONG),
            output,outputCount*sizeof(ULONGLONG),&returned,nullptr)!=FALSE;
    }
    bool readPort(WORD port,BYTE& value) {
        ULONGLONG in=port,out=0;
        if(!execute("ioctl_pio_read",&in,1,&out,1)) return false;
        value=static_cast<BYTE>(out); return true;
    }
    bool writePort(WORD port,BYTE value) {
        ULONGLONG in[2]={port,value};
        return execute("ioctl_pio_write",in,2,nullptr,0);
    }
    bool waitBit(BYTE mask,bool set,DWORD timeout=750) {
        ULONGLONG end=GetTickCount64()+timeout;
        do {
            BYTE status=0;
            if(!readPort(EC_STATUS,status)) return false;
            if(((status&mask)!=0)==set) return true;
            Sleep(2);
        } while(GetTickCount64()<end);
        SetLastError(ERROR_TIMEOUT); return false;
    }
    void drain() {
        for(int i=0;i<16;i++) {
            BYTE status=0,junk=0;
            if(!readPort(EC_STATUS,status) || !(status&OBF)) break;
            readPort(EC_DATA,junk);
        }
    }
    bool begin() {
        drain();
        return waitBit(IBF,false);
    }
    bool readRegister(BYTE address,BYTE& value) {
        if(!begin() || !writePort(EC_STATUS,CMD_READ) || !waitBit(IBF,false) ||
           !writePort(EC_DATA,address) || !waitBit(OBF,true)) return false;
        return readPort(EC_DATA,value);
    }
    bool writeRegister(BYTE address,BYTE value) {
        return begin() && writePort(EC_STATUS,CMD_WRITE) && waitBit(IBF,false) &&
               writePort(EC_DATA,address) && waitBit(IBF,false) &&
               writePort(EC_DATA,value) && waitBit(IBF,false);
    }
public:
    ~PawnEc() {
        if(device_!=INVALID_HANDLE_VALUE) CloseHandle(device_);
        if(mutex_) CloseHandle(mutex_);
    }
    DWORD open() {
        device_=CreateFileW(PAWN_DEVICE,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
                           nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(device_==INVALID_HANDLE_VALUE) return X1_FAN_DRIVER_MISSING;

        std::wstring module=exeDirectory()+L"\\LpcACPIEC.bin";
        HANDLE file=CreateFileW(module.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,
                                OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) return X1_FAN_MODULE_MISSING;
        LARGE_INTEGER size{};
        if(!GetFileSizeEx(file,&size) || size.QuadPart<=0 || size.QuadPart>16*1024*1024) {
            CloseHandle(file);
            SetLastError(ERROR_BAD_LENGTH);
            return X1_FAN_EC_UNAVAILABLE;
        }
        DWORD blobSize=static_cast<DWORD>(size.QuadPart);
        void* blob=HeapAlloc(GetProcessHeap(),0,blobSize);
        DWORD read=0;
        bool loaded=blob && ReadFile(file,blob,blobSize,&read,nullptr) && read==blobSize;
        CloseHandle(file);
        DWORD returned=0;
        if(loaded)
            loaded=DeviceIoControl(device_,IOCTL_LOAD_BINARY,blob,blobSize,
                                   nullptr,0,&returned,nullptr)!=FALSE;
        if(blob) HeapFree(GetProcessHeap(),0,blob);
        if(!loaded)
            return X1_FAN_EC_UNAVAILABLE;
        mutex_=CreateMutexW(nullptr,FALSE,L"Global\\Access_EC");
        return mutex_ ? X1_FAN_OK : X1_FAN_EC_UNAVAILABLE;
    }
    bool sample(DWORD& fan1,DWORD& fan2) {
        DWORD wait=WaitForSingleObject(mutex_,1000);
        if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED) return false;
        bool ok=true;
        DWORD rpm[2]{};
        for(BYTE fan=0;fan<2 && ok;fan++) {
            BYTE lo=0,hi=0;
            ok=writeRegister(REG_FAN_SELECT,fan) &&
               readRegister(REG_FAN_LO,lo) && readRegister(REG_FAN_HI,hi);
            DWORD value=(static_cast<DWORD>(hi)<<8)|lo;
            rpm[fan]=value<=0x1fff ? value : 0;
        }
        writeRegister(REG_FAN_SELECT,0);
        ReleaseMutex(mutex_);
        if(ok) { fan1=rpm[0]; fan2=rpm[1]; }
        return ok;
    }
    bool readHottestTemp(LONG& hottest) {
        DWORD wait=WaitForSingleObject(mutex_,1000);
        if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED) return false;
        bool ok=true;
        hottest=-127;
        for(int i=0;i<12 && ok;i++) {
            BYTE raw=0;
            BYTE reg=i<8 ? static_cast<BYTE>(REG_TEMP0+i) : static_cast<BYTE>(REG_TEMP1+i-8);
            ok=readRegister(reg,raw);
            signed char temp=static_cast<signed char>(raw);
            if(ok && temp>0 && temp<120 && temp>hottest) hottest=temp;
        }
        ReleaseMutex(mutex_);
        return ok && hottest>-127;
    }
    bool setFanLevel(BYTE level) {
        DWORD wait=WaitForSingleObject(mutex_,1000);
        if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED) return false;
        bool ok=true;
        for(BYTE fan=0;fan<2 && ok;fan++) {
            BYTE verify=0;
            ok=writeRegister(REG_FAN_SELECT,fan) && writeRegister(REG_FAN_CTRL,level) &&
               readRegister(REG_FAN_CTRL,verify) && (verify&0xc7)==(level&0xc7);
        }
        writeRegister(REG_FAN_SELECT,0);
        ReleaseMutex(mutex_);
        return ok;
    }
};

BYTE curveLevel(DWORD mode,LONG temp,BYTE current,bool sameMode) {
    const LONG* thresholds=nullptr;
    static const LONG cool[]={45,52,59,66,74};
    static const LONG aggressive[]={38,45,52,59,68};
    thresholds=mode==X1_FAN_MODE_AGGRESSIVE ? aggressive : cool;
    const BYTE levels[]={2,3,5,7,FAN_BIOS};
    int desired=0;
    while(desired<4 && temp>=thresholds[desired]) desired++;
    BYTE target=levels[desired];

    // Three-degree hysteresis prevents level hunting while cooling down.
    if(sameMode && current!=FAN_BIOS) {
        int currentIndex=0;
        while(currentIndex<4 && levels[currentIndex]!=current) currentIndex++;
        if(desired<currentIndex && temp>=thresholds[currentIndex-1]-3) return current;
    } else if(sameMode && target!=FAN_BIOS && temp>=thresholds[3]-5) {
        return FAN_BIOS;
    }
    return target;
}

bool createTelemetry() {
    PSECURITY_DESCRIPTOR sd{};
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(A;;GRGW;;;IU)(A;;GA;;;SY)(A;;GA;;;BA)",SDDL_REVISION_1,&sd,nullptr);
    SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
    const wchar_t* name=g_consoleMode ? L"Local\\X1FanTelemetryProbe" : X1_FAN_MAPPING_NAME;
    g_mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,sd?&sa:nullptr,PAGE_READWRITE,0,
                                sizeof(X1FanTelemetry),name);
    g_commandEvent=CreateEventW(g_consoleMode ? nullptr : (sd?&sa:nullptr),FALSE,FALSE,
                                g_consoleMode ? nullptr : X1_FAN_COMMAND_EVENT_NAME);
    if(sd) LocalFree(sd);
    if(!g_mapping || !g_commandEvent) return false;
    g_shared=static_cast<X1FanTelemetry*>(MapViewOfFile(g_mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(X1FanTelemetry)));
    if(!g_shared) return false;
    ZeroMemory(g_shared,sizeof(*g_shared));
    g_shared->magic=X1_FAN_MAGIC; g_shared->version=X1_FAN_VERSION;
    g_shared->requestedMode=X1_FAN_MODE_BIOS_AUTO;
    g_shared->activeMode=X1_FAN_MODE_BIOS_AUTO;
    g_shared->hottestTempC=-1;
    publish(X1_FAN_STARTING);
    return true;
}

bool runningOnBattery() {
    SYSTEM_POWER_STATUS status{};
    return GetSystemPowerStatus(&status) && status.ACLineStatus==0;
}

DWORD pollInterval(DWORD requestedMode) {
    if(requestedMode!=X1_FAN_MODE_BIOS_AUTO) return 1000;
    return runningOnBattery() ? 3000 : 2000;
}

void runWorker() {
    if(!createTelemetry()) return;
    if(!supportedModel()) { publish(X1_FAN_UNSUPPORTED_MODEL); return; }
    PawnEc ec;
    DWORD opened=ec.open();
    if(opened!=X1_FAN_OK) { publish(opened,0,0,GetLastError()); return; }
    // A previous process could have terminated while holding a manual EC
    // level. Every fresh service start tries to hand both fans to BIOS.
    // Lenovo firmware may briefly own the EC during startup, so a failed
    // first attempt must not terminate the service; the main loop retries.
    bool initialBios=false;
    for(int attempt=0;attempt<3 && !initialBios;attempt++) {
        initialBios=ec.setFanLevel(FAN_BIOS);
        if(!initialBios) Sleep(300);
    }
    unsigned consecutiveFailures=0;
    DWORD activeMode=initialBios ? X1_FAN_MODE_BIOS_AUTO : 0xffffffff;
    BYTE appliedLevel=initialBios ? FAN_BIOS : 0xff;
    DWORD requested=X1_FAN_MODE_BIOS_AUTO;
    HANDLE waits[]={g_stopEvent,g_commandEvent};
    for(;;) {
        DWORD f1=0,f2=0;
        if(ec.sample(f1,f2)) {
            consecutiveFailures=0;
            publish(X1_FAN_OK,f1,f2);
        } else if(++consecutiveFailures>=3) {
            // Lenovo firmware and other well-behaved EC clients can briefly
            // own the controller. Do not flash N/A for a single missed pass.
            publish(X1_FAN_EC_UNAVAILABLE,0,0,GetLastError());
        }

        requested=static_cast<DWORD>(
            InterlockedCompareExchange(&g_shared->requestedMode,0,0));
        if(requested>X1_FAN_MODE_AGGRESSIVE) requested=X1_FAN_MODE_BIOS_AUTO;
        LONG hottest=-1;
        bool modeOk=true;
        if(requested==X1_FAN_MODE_BIOS_AUTO) {
            if(activeMode!=requested || appliedLevel!=FAN_BIOS)
                modeOk=ec.setFanLevel(FAN_BIOS);
            if(modeOk) { activeMode=requested; appliedLevel=FAN_BIOS; }
        } else if(ec.readHottestTemp(hottest)) {
            BYTE target=curveLevel(requested,hottest,
                appliedLevel,activeMode==requested);
            if(activeMode!=requested || target!=appliedLevel)
                modeOk=ec.setFanLevel(target);
            if(modeOk) { activeMode=requested; appliedLevel=target; }
        }
        g_shared->activeMode=activeMode;
        g_shared->hottestTempC=hottest;

        DWORD wait=WaitForMultipleObjects(ARRAYSIZE(waits),waits,FALSE,pollInterval(requested));
        if(wait==WAIT_OBJECT_0) break;
        if(wait!=WAIT_OBJECT_0+1 && wait!=WAIT_TIMEOUT) break;
    }
    ec.setFanLevel(FAN_BIOS);
    g_shared->activeMode=X1_FAN_MODE_BIOS_AUTO;
}

void cleanupServiceResources() {
    if(g_shared) UnmapViewOfFile(g_shared);
    if(g_mapping) CloseHandle(g_mapping);
    if(g_commandEvent) CloseHandle(g_commandEvent);
    if(g_stopEvent) CloseHandle(g_stopEvent);
    g_shared=nullptr;
    g_mapping=nullptr;
    g_commandEvent=nullptr;
    g_stopEvent=nullptr;
}

void setServiceState(DWORD state,DWORD error=NO_ERROR) {
    g_serviceStatus.dwServiceType=SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwCurrentState=state;
    g_serviceStatus.dwControlsAccepted=state==SERVICE_RUNNING ? SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN : 0;
    g_serviceStatus.dwWin32ExitCode=error;
    SetServiceStatus(g_statusHandle,&g_serviceStatus);
}

void WINAPI serviceControl(DWORD control) {
    if(control==SERVICE_CONTROL_STOP || control==SERVICE_CONTROL_SHUTDOWN) {
        setServiceState(SERVICE_STOP_PENDING);
        SetEvent(g_stopEvent);
    }
}
void WINAPI serviceMain(DWORD,LPWSTR*) {
    g_statusHandle=RegisterServiceCtrlHandlerW(SERVICE_NAME,serviceControl);
    if(!g_statusHandle) return;
    g_stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    setServiceState(SERVICE_RUNNING);
    runWorker();
    cleanupServiceResources();
    setServiceState(SERVICE_STOPPED);
}

const wchar_t* modeName(DWORD mode) {
    if(mode==X1_FAN_MODE_COOL) return L"Cool";
    if(mode==X1_FAN_MODE_AGGRESSIVE) return L"Aggressive";
    return L"BIOS Auto";
}

int showController() {
    HANDLE mapping=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,X1_FAN_MAPPING_NAME);
    if(!mapping) {
        MessageBoxW(nullptr,L"X1FanService is not running or its telemetry is unavailable.",
                    L"X1 Fan Control",MB_OK|MB_ICONERROR);
        return 1;
    }
    auto shared=static_cast<X1FanTelemetry*>(
        MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(X1FanTelemetry)));
    if(!shared || shared->magic!=X1_FAN_MAGIC || shared->version!=X1_FAN_VERSION) {
        if(shared) UnmapViewOfFile(shared);
        CloseHandle(mapping);
        MessageBoxW(nullptr,L"X1FanService telemetry version does not match this controller.",
                    L"X1 Fan Control",MB_OK|MB_ICONERROR);
        return 1;
    }

    std::wstring first=L"Current mode: ";
    first+=modeName(shared->activeMode);
    first+=L"\n\nSelect Yes for BIOS Auto (default and safest).\n"
            L"Select No to choose a custom cooling mode.\n"
            L"Select Cancel to leave the mode unchanged.";
    int answer=MessageBoxW(nullptr,first.c_str(),L"X1 Fan Control",
                           MB_YESNOCANCEL|MB_ICONINFORMATION|MB_DEFBUTTON1);
    DWORD requested=X1_FAN_MODE_BIOS_AUTO;
    bool selected=answer==IDYES;
    if(answer==IDNO) {
        int custom=MessageBoxW(nullptr,
            L"Select Yes for Cool.\n"
            L"Select No for Aggressive.\n"
            L"Select Cancel to leave the mode unchanged.\n\n"
            L"Both custom modes return control to BIOS at high temperature.",
            L"Choose custom fan mode",MB_YESNOCANCEL|MB_ICONWARNING|MB_DEFBUTTON1);
        if(custom==IDYES) { requested=X1_FAN_MODE_COOL; selected=true; }
        if(custom==IDNO) { requested=X1_FAN_MODE_AGGRESSIVE; selected=true; }
    }
    if(selected) {
        InterlockedExchange(&shared->requestedMode,requested);
        HANDLE command=OpenEventW(EVENT_MODIFY_STATE,FALSE,X1_FAN_COMMAND_EVENT_NAME);
        if(command) {
            SetEvent(command);
            CloseHandle(command);
        }
        ULONGLONG deadline=GetTickCount64()+4000;
        while(GetTickCount64()<deadline && shared->activeMode!=requested) Sleep(100);
        std::wstring result=L"Requested mode: ";
        result+=modeName(requested);
        result+=shared->activeMode==requested ? L"\n\nMode is active." :
                                               L"\n\nRequest sent; the service is still applying it.";
        MessageBoxW(nullptr,result.c_str(),L"X1 Fan Control",
                    MB_OK|(shared->activeMode==requested?MB_ICONINFORMATION:MB_ICONWARNING));
    }
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    return 0;
}
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int) {
    int argc=0;
    LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    bool console=argc>1 && wcscmp(argv[1],L"--console")==0;
    if(argv) LocalFree(argv);
    if(console) {
        g_consoleMode=true;
        g_stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        runWorker();
        int result=g_shared && g_shared->status==X1_FAN_OK ? 0 : 1;
        cleanupServiceResources();
        return result;
    }
    SERVICE_TABLE_ENTRYW table[]={{const_cast<LPWSTR>(SERVICE_NAME),serviceMain},{nullptr,nullptr}};
    if(StartServiceCtrlDispatcherW(table)) return 0;
    return GetLastError()==ERROR_FAILED_SERVICE_CONTROLLER_CONNECT ? showController()
                                                                   : static_cast<int>(GetLastError());
}
