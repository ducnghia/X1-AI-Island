#include <windows.h>
#include <sddl.h>
#include <string>
#include <vector>
#include <fstream>
#include "fan_telemetry.h"

#pragma comment(lib, "advapi32.lib")

namespace {
constexpr wchar_t SERVICE_NAME[] = L"X1FanService";
constexpr wchar_t PAWN_DEVICE[] = L"\\\\?\\GLOBALROOT\\Device\\PawnIO";
constexpr DWORD IOCTL_LOAD_BINARY = 0xA1B22084;
constexpr DWORD IOCTL_EXECUTE_FN = 0xA1B22104;
constexpr WORD EC_DATA = 0x62, EC_STATUS = 0x66;
constexpr BYTE OBF = 0x01, IBF = 0x02, CMD_READ = 0x80, CMD_WRITE = 0x81;
constexpr BYTE REG_FAN_SELECT = 0x31, REG_FAN_LO = 0x84, REG_FAN_HI = 0x85;

SERVICE_STATUS_HANDLE g_statusHandle{};
SERVICE_STATUS g_serviceStatus{};
HANDLE g_stopEvent{}, g_mapping{};
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
        std::vector<BYTE> packet(32+inputCount*sizeof(ULONGLONG),0);
        size_t n=strlen(name);
        if(n>=32) return false;
        memcpy(packet.data(),name,n);
        if(inputCount) memcpy(packet.data()+32,input,inputCount*sizeof(ULONGLONG));
        DWORD returned=0;
        return DeviceIoControl(device_,IOCTL_EXECUTE_FN,packet.data(),static_cast<DWORD>(packet.size()),
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
        std::ifstream file(module,std::ios::binary);
        if(!file) return X1_FAN_MODULE_MISSING;
        std::vector<char> blob((std::istreambuf_iterator<char>(file)),{});
        DWORD returned=0;
        if(blob.empty() || !DeviceIoControl(device_,IOCTL_LOAD_BINARY,blob.data(),
                static_cast<DWORD>(blob.size()),nullptr,0,&returned,nullptr))
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
};

bool createTelemetry() {
    PSECURITY_DESCRIPTOR sd{};
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(A;;GR;;;WD)(A;;GA;;;SY)(A;;GA;;;BA)",SDDL_REVISION_1,&sd,nullptr);
    SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
    const wchar_t* name=g_consoleMode ? L"Local\\X1FanTelemetryProbe" : X1_FAN_MAPPING_NAME;
    g_mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,sd?&sa:nullptr,PAGE_READWRITE,0,
                                sizeof(X1FanTelemetry),name);
    if(sd) LocalFree(sd);
    if(!g_mapping) return false;
    g_shared=static_cast<X1FanTelemetry*>(MapViewOfFile(g_mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(X1FanTelemetry)));
    if(!g_shared) return false;
    ZeroMemory(g_shared,sizeof(*g_shared));
    g_shared->magic=X1_FAN_MAGIC; g_shared->version=X1_FAN_VERSION;
    publish(X1_FAN_STARTING);
    return true;
}

void runWorker() {
    if(!createTelemetry()) return;
    if(!supportedModel()) { publish(X1_FAN_UNSUPPORTED_MODEL); return; }
    PawnEc ec;
    DWORD opened=ec.open();
    if(opened!=X1_FAN_OK) { publish(opened,0,0,GetLastError()); return; }
    while(WaitForSingleObject(g_stopEvent,1000)==WAIT_TIMEOUT) {
        DWORD f1=0,f2=0;
        if(ec.sample(f1,f2)) publish(X1_FAN_OK,f1,f2);
        else publish(X1_FAN_EC_UNAVAILABLE,0,0,GetLastError());
    }
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
    setServiceState(SERVICE_STOPPED);
}
}

int wmain(int argc,wchar_t** argv) {
    if(argc>1 && wcscmp(argv[1],L"--console")==0) {
        g_consoleMode=true;
        g_stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        runWorker();
        if(g_shared) wprintf(L"status=%lu fan1=%lu fan2=%lu error=%lu\n",
            g_shared->status,g_shared->fan1Rpm,g_shared->fan2Rpm,g_shared->lastError);
        return g_shared && g_shared->status==X1_FAN_OK ? 0 : 1;
    }
    SERVICE_TABLE_ENTRYW table[]={{const_cast<LPWSTR>(SERVICE_NAME),serviceMain},{nullptr,nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : static_cast<int>(GetLastError());
}
