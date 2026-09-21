#include "../x1_ai_island.cpp"
#include <cstdio>
int main(){
  bool loaded=g_nvml.load(); updateStats();
  wprintf(L"NVML=%d GPU_OK=%d GPU=%u VRAM_OK=%d VRAM=%llu/%llu PSTATE_OK=%d P%u TEMP=%u TEXT=%ls\n",
    loaded,g_stats.utilOk,g_stats.gpu,g_stats.memoryOk,g_stats.used,g_stats.total,g_stats.pstateOk,g_stats.pstate,g_stats.temp,compactMetrics().c_str());
  return (loaded && g_stats.memoryOk && g_stats.pstateOk) ? 0 : 1;
}
