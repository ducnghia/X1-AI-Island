#include <windows.h>
#include <cstdio>
#include "../fan_telemetry.h"

int main() {
    HANDLE mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,X1_FAN_MAPPING_NAME);
    if(!mapping) { printf("FAIL: mapping unavailable (%lu)\n",GetLastError()); return 1; }
    auto data=static_cast<const X1FanTelemetry*>(
        MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(X1FanTelemetry)));
    if(!data) { printf("FAIL: mapping unreadable (%lu)\n",GetLastError()); CloseHandle(mapping); return 1; }
    printf("status=%lu mode=%lu fan1=%lu fan2=%lu hottest=%ld age=%llu ms error=%lu\n",
        data->status,data->activeMode,data->fan1Rpm,data->fan2Rpm,data->hottestTempC,
        static_cast<unsigned long long>(GetTickCount64()-data->updatedTick),data->lastError);
    bool ok=data->magic==X1_FAN_MAGIC && data->version==X1_FAN_VERSION &&
            data->status==X1_FAN_OK && GetTickCount64()-data->updatedTick<5000;
    UnmapViewOfFile(data); CloseHandle(mapping);
    return ok ? 0 : 2;
}
