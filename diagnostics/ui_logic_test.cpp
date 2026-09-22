#include "../x1_ai_island.cpp"
#include <cassert>
#include <cstdio>
int main() {
    Stats s{};
    s.utilOk=true; s.gpu=49; s.memoryOk=true; s.total=100; s.used=10;
    assert(loadLevel(s)==LoadLevel::Green);
    s.gpu=50; assert(loadLevel(s)==LoadLevel::Yellow);
    s.gpu=10; s.used=80; assert(loadLevel(s)==LoadLevel::Red);

    s.gpu=60; s.used=60;
    auto tied=assessLoad(s);
    assert(tied.effective==60 && tied.source==LoadSource::GPU);
    s.gpu=55; s.used=70;
    auto vramLed=assessLoad(s);
    assert(vramLed.effective==70 && vramLed.source==LoadSource::VRAM);

    auto green=pulseColor(LoadLevel::Green,0.0);
    auto yellow=pulseColor(LoadLevel::Yellow,0.0);
    auto red=pulseColor(LoadLevel::Red,0.0);
    assert(GetGValue(green)>GetRValue(green) && GetGValue(green)>GetBValue(green));
    assert(GetRValue(yellow)>GetBValue(yellow)*4 && GetGValue(yellow)>GetBValue(yellow)*4);
    assert(GetRValue(red)>GetGValue(red)*3 && GetRValue(red)>GetBValue(red)*3);

    Stats gpuText{};
    gpuText.utilOk=true;
    gpuText.gpu=49;
    auto gpuTextGreen=gpuTextColor(gpuText,0);
    gpuText.gpu=50;
    auto gpuTextYellow=gpuTextColor(gpuText,0);
    gpuText.gpu=80;
    auto gpuTextRed=gpuTextColor(gpuText,0);
    assert(gpuTextGreen==NVIDIA_GREEN);
    Stats unavailableGpuText{};
    assert(gpuTextColor(unavailableGpuText,1234)==RGB(110,110,116));
    assert(GetRValue(gpuTextYellow)>GetBValue(gpuTextYellow)*4);
    assert(GetRValue(gpuTextRed)>GetGValue(gpuTextRed)*3);

    g_nvml.ready=true; g_stats=s; g_stats.ok=true; g_stats.tempOk=true; g_stats.temp=61;
    g_stats.utilOk=true; g_stats.gpu=70;
    g_stats.pstateOk=true; g_stats.pstate=8; g_stats.watts=28; g_stats.total=16ULL<<30; g_stats.used=2ULL<<30;
    g_fans.ok=true; g_fans.fan1=3816; g_fans.fan2=3540;
    auto text=compactMetrics();
    assert(text.find(L"GPU")==std::wstring::npos);
    assert(text.find(L"RAM")==std::wstring::npos);
    assert(text.find(L"2.0/16G")!=std::wstring::npos);
    assert(text.find(L"P8")!=std::wstring::npos);
    assert(text.find(L"F1 3816")!=std::wstring::npos);
    assert(text.find(L"F2 3540")!=std::wstring::npos);
    assert(text.find(L"COOL")==std::wstring::npos);
    g_fans.mode=X1_FAN_MODE_COOL;
    assert(compactMetrics().find(L"COOL")!=std::wstring::npos);
    g_fans.mode=X1_FAN_MODE_AGGRESSIVE;
    assert(compactMetrics().find(L"AGGR")!=std::wstring::npos);
    puts("PASS: max(GPU, VRAM) border, GPU-only name color, RGB pulse bias, compact telemetry.");
}
