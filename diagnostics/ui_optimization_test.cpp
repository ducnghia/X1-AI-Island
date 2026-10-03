#include <windows.h>
#include <cassert>
#include <cstdio>

static int measurements=0;
static HDC paintDc{};
static BOOL WINAPI measuredExtent(HDC dc,LPCWSTR text,int length,LPSIZE size) {
    ++measurements;
    return GetTextExtentPoint32W(dc,text,length,size);
}
static HDC WINAPI testBeginPaint(HWND,LPPAINTSTRUCT) { return paintDc; }
static BOOL WINAPI testEndPaint(HWND,const PAINTSTRUCT*) { return TRUE; }
#define GetTextExtentPoint32W measuredExtent
#define BeginPaint testBeginPaint
#define EndPaint testEndPaint
#include "../x1_ai_island.cpp"
#undef GetTextExtentPoint32W
#undef BeginPaint
#undef EndPaint

int main() {
    HDC dc=CreateCompatibleDC(nullptr);
    assert(dc);
    HFONT font=CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    assert(font);
    g_stats={}; g_fans={};
    refreshDisplayCache();
    g_compactMeasurements.update(dc,96);
    assert(g_compactMeasurements.valid && measurements>0);
    int count=measurements;
    refreshDisplayCache();
    g_compactMeasurements.update(dc,96);
    assert(measurements==count);
    g_compactMeasurements.update(dc,144);
    assert(measurements>count);
    count=measurements;
    HGDIOBJ original=SelectObject(dc,font);
    g_compactMeasurements.update(dc,144);
    assert(measurements>count);
    count=measurements;
    g_fans.ok=true; g_fans.fan1=3210;
    refreshDisplayCache();
    assert(!g_compactMeasurements.valid);
    g_compactMeasurements.update(dc,144);
    assert(measurements>count);
    for(size_t i=0;i<g_compactParts.size();++i) {
        if(g_compactParts[i].empty()) continue;
        SIZE expected{};
        assert(GetTextExtentPoint32W(dc,g_compactParts[i].c_str(),
            static_cast<int>(g_compactParts[i].size()),&expected));
        assert(expected.cx==g_compactMeasurements.sizes[i].cx);
        assert(expected.cy==g_compactMeasurements.sizes[i].cy);
    }
    SelectObject(dc,original);

    // Use a memory DC to verify restoration even when the primary font is absent.
    HWND window=CreateWindowExW(0,L"STATIC",L"Optimization test",WS_POPUP,
        0,0,560,124,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    assert(window);
    paintDc=dc;
    g_font=nullptr; g_metricsFont=font; g_smallFont=font;
    g_backgroundBrush=CreateSolidBrush(RGB(18,18,20));
    assert(g_backgroundBrush);
    HGDIOBJ originalPen=GetCurrentObject(dc,OBJ_PEN);
    HGDIOBJ originalBrush=GetCurrentObject(dc,OBJ_BRUSH);
    SetDCPenColor(dc,RGB(12,34,56));
    for(bool expanded : {false,true}) {
        g_expanded=expanded;
        paint(window);
        assert(GetCurrentObject(dc,OBJ_FONT)==original);
        assert(GetCurrentObject(dc,OBJ_PEN)==originalPen);
        assert(GetCurrentObject(dc,OBJ_BRUSH)==originalBrush);
        assert(GetDCPenColor(dc)==RGB(12,34,56));
    }
    DWORD handlesBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(int frame=0;frame<1000;++frame) paint(window);
    assert(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==handlesBefore);
    DestroyWindow(window);
    cleanupBackbuffer();
    DeleteObject(g_backgroundBrush); g_backgroundBrush=nullptr;
    g_metricsFont=nullptr; g_smallFont=nullptr;
    DeleteObject(font); DeleteDC(dc);

    X1FanTelemetry telemetry{};
    telemetry.magic=X1_FAN_MAGIC; telemetry.version=X1_FAN_VERSION;
    telemetry.status=X1_FAN_OK; telemetry.fan1Rpm=3000; telemetry.fan2Rpm=3100;
    telemetry.activeMode=X1_FAN_MODE_COOL; telemetry.updatedTick=GetTickCount64();
    FanTelemetryReader reader;
    reader.view=&telemetry;
    reader.update();
    assert(g_fans.ok && g_fans.fan1==3000 && g_fans.mode==X1_FAN_MODE_COOL);
    telemetry.sequence=1;
    reader.update();
    assert(!g_fans.ok && g_fans.fan1==0 && g_fans.fan2==0);
    telemetry.sequence=2;
    reader.update();
    assert(g_fans.ok && g_fans.fan2==3100);
    telemetry.updatedTick=GetTickCount64()-6000;
    reader.update();
    assert(!g_fans.ok);
    reader.view=nullptr;
    puts("PASS: text cache reuse/invalidation, font restoration, snapshot rejection/recovery/staleness.");
}
