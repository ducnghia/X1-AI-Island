#include "../x1_ai_island.cpp"
#include <cstdio>
int main(){ if(!g_nvml.load()) return 1; for(int i=0;i<4;++i){ updateStats(); wprintf(L"%ls\n",oneLine().c_str()); fflush(stdout); Sleep(1000); } }
