#include "../x1_ai_island.cpp"
#include <cassert>
#include <cstdio>
int __cdecl failedUtil(nvmlDevice_t,nvmlUtilization_t*) { return 999; }
int __cdecl goodUtil(nvmlDevice_t,nvmlUtilization_t* u) { u->gpu=35; u->memory=12; return 0; }
int __cdecl goodMemory(nvmlDevice_t,nvmlMemory_t* m) { m->total=16ULL<<30; m->used=2ULL<<30; return 0; }
int __cdecl goodTemp(nvmlDevice_t,unsigned,unsigned* t) { *t=60; return 0; }
int __cdecl goodPower(nvmlDevice_t,unsigned* p) { *p=28000; return 0; }
int main() {
 g_nvml.ready=true; g_nvml.util=failedUtil; g_nvml.memory=goodMemory; g_nvml.temp=goodTemp; g_nvml.power=goodPower;
 updateStats(); assert(g_stats.ok && !g_stats.utilOk && g_stats.memoryOk && g_stats.tempOk);
 assert(g_stats.used==(2ULL<<30) && g_stats.temp==60 && g_stats.watts==28);
 assert(oneLine().find(L"N/A")!=std::wstring::npos && oneLine().find(L"2.0/16.0G")!=std::wstring::npos);
 g_nvml.util=goodUtil; updateStats(); assert(g_stats.utilOk && g_stats.gpu==35);
 assert(oneLine().find(L"35%")!=std::wstring::npos);
 g_nvml.ready=false; updateStats(); assert(!g_stats.ok && oneLine().find(L"NVML unavailable")!=std::wstring::npos);
 puts("PASS: partial readings survive utilization error 999; recovery and initialization failure handled.");
}
