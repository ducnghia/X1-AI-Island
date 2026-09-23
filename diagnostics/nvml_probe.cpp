#include "../x1_ai_island.cpp"
#include <cstdio>
int main() {
    puts("Loading NVML..."); fflush(stdout);
    bool loaded = g_nvml.load();
    printf("loaded=%d device=%p\n", loaded, g_nvml.device); fflush(stdout);
    if (loaded) for(int i=0; i<8; ++i) { Sleep(1000);
        nvmlUtilization_t u{}; nvmlMemory_t m{}; unsigned t=0,p=0;
        printf("util=%d\n", g_nvml.util(g_nvml.device,&u)); fflush(stdout);
        printf("memory=%d\n", g_nvml.memory(g_nvml.device,&m)); fflush(stdout);
        printf("temp=%d\n", g_nvml.temp(g_nvml.device,0,&t)); fflush(stdout);
        printf("power=%d\n", g_nvml.power ? g_nvml.power(g_nvml.device,&p) : -1);
        printf("GPU=%u VRAM=%llu/%llu temp=%u power=%u\n",u.gpu,m.used,m.total,t,p);
    }
    return loaded ? 0 : 1;
}

