#include <windows.h>
#include <cstdio>
#include <cwchar>
#include "../fan_telemetry.h"

int wmain(int argc,wchar_t** argv) {
    DWORD mode=X1_FAN_MODE_BIOS_AUTO;
    if(argc>1 && !_wcsicmp(argv[1],L"cool")) mode=X1_FAN_MODE_COOL;
    if(argc>1 && !_wcsicmp(argv[1],L"aggressive")) mode=X1_FAN_MODE_AGGRESSIVE;
    HANDLE mapping=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,X1_FAN_MAPPING_NAME);
    if(!mapping) return 1;
    auto data=static_cast<X1FanTelemetry*>(
        MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(X1FanTelemetry)));
    if(!data) { CloseHandle(mapping); return 1; }
    InterlockedExchange(&data->requestedMode,mode);
    ULONGLONG deadline=GetTickCount64()+5000;
    while(GetTickCount64()<deadline && data->activeMode!=mode) Sleep(100);
    printf("requested=%lu active=%lu fan1=%lu fan2=%lu hottest=%ld\n",
           mode,data->activeMode,data->fan1Rpm,data->fan2Rpm,data->hottestTempC);
    bool ok=data->activeMode==mode;
    UnmapViewOfFile(data); CloseHandle(mapping);
    return ok ? 0 : 2;
}
