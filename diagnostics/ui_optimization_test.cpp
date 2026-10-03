#include <windows.h>
#include <cassert>
#include <cstdio>

static int measurements=0;
static HDC paintDc{};
enum class Failure { None, Dc, Bitmap, SelectionNull, SelectionError };
static Failure failure=Failure::None;
static int injectedFailures=0;
static int dcAllocations=0, bitmapAllocations=0;
static HDC trackedDc{};
static HBITMAP trackedBitmap{};
static HGDIOBJ trackedOriginal{};
static bool restoredBeforeDelete=false;

static HDC WINAPI testCreateCompatibleDC(HDC dc) {
    ++dcAllocations;
    if(failure==Failure::Dc) {
        failure=Failure::None; ++injectedFailures;
        return nullptr;
    }
    trackedDc=CreateCompatibleDC(dc);
    trackedOriginal=trackedDc ? GetCurrentObject(trackedDc,OBJ_BITMAP) : nullptr;
    restoredBeforeDelete=false;
    return trackedDc;
}
static HBITMAP WINAPI testCreateCompatibleBitmap(HDC dc,int width,int height) {
    ++bitmapAllocations;
    if(failure==Failure::Bitmap) {
        failure=Failure::None; ++injectedFailures;
        return nullptr;
    }
    trackedBitmap=CreateCompatibleBitmap(dc,width,height);
    return trackedBitmap;
}
static HGDIOBJ WINAPI testSelectObject(HDC dc,HGDIOBJ object) {
    if(dc==trackedDc && object==trackedBitmap && trackedBitmap &&
       (failure==Failure::SelectionNull || failure==Failure::SelectionError)) {
        HGDIOBJ result=failure==Failure::SelectionError ? HGDI_ERROR : nullptr;
        failure=Failure::None; ++injectedFailures;
        return result;
    }
    return SelectObject(dc,object);
}
static BOOL WINAPI testDeleteDC(HDC dc) {
    if(dc==trackedDc) {
        restoredBeforeDelete=GetCurrentObject(dc,OBJ_BITMAP)==trackedOriginal;
        assert(restoredBeforeDelete);
        trackedDc=nullptr; trackedOriginal=nullptr;
    }
    return DeleteDC(dc);
}
static BOOL WINAPI testDeleteObject(HGDIOBJ object) {
    BOOL result=DeleteObject(object);
    if(object==trackedBitmap && trackedBitmap) {
        assert(result);
        trackedBitmap=nullptr;
    }
    return result;
}
static BOOL WINAPI measuredExtent(HDC dc,LPCWSTR text,int length,LPSIZE size) {
    ++measurements;
    return GetTextExtentPoint32W(dc,text,length,size);
}
static HDC WINAPI testBeginPaint(HWND,LPPAINTSTRUCT) { return paintDc; }
static BOOL WINAPI testEndPaint(HWND,const PAINTSTRUCT*) { return TRUE; }
#define GetTextExtentPoint32W measuredExtent
#define BeginPaint testBeginPaint
#define EndPaint testEndPaint
#define CreateCompatibleDC testCreateCompatibleDC
#define CreateCompatibleBitmap testCreateCompatibleBitmap
#define SelectObject testSelectObject
#define DeleteDC testDeleteDC
#define DeleteObject testDeleteObject
#include "../x1_ai_island.cpp"
#undef CreateCompatibleDC
#undef CreateCompatibleBitmap
#undef SelectObject
#undef DeleteDC
#undef DeleteObject
#undef GetTextExtentPoint32W
#undef BeginPaint
#undef EndPaint

static DWORD gdiHandles() {
    GdiFlush();
    return GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
}

static void runTests() {
    g_compactMeasurements.valid=false;
    HDC dc=CreateCompatibleDC(nullptr);
    assert(dc);
    BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=800;
    info.bmiHeader.biHeight=-160;
    info.bmiHeader.biPlanes=1;
    info.bmiHeader.biBitCount=32;
    info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;
    HBITMAP output=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    assert(output && pixels);
    HGDIOBJ originalBitmap=SelectObject(dc,output);
    assert(originalBitmap && originalBitmap!=HGDI_ERROR);
    HFONT font=CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    assert(font);
    g_stats={}; g_fans={};
    refreshDisplayCache();
    g_compactMeasurements.update(dc,96);
    assert(g_compactMeasurements.valid && measurements>0);
    int count=measurements;
    assert(!refreshDisplayCache());
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

    g_expanded=false;
    g_stats.watts=10.11;
    assert(refreshDisplayCache());
    g_stats.watts=10.14;
    assert(!refreshDisplayCache());
    g_stats.watts=10.24;
    assert(!refreshDisplayCache()); // Expanded precision is invisible while compact.
    g_expanded=true;
    g_stats.watts=10.34;
    assert(refreshDisplayCache());
    assert(!refreshDisplayCache());
    for(int row=0;row<3;++row) {
        int previousRight=0;
        for(int column=0;column<6;++column) {
            RECT cell=expandedCell(ISLAND_WIDTH,row,column);
            assert(cell.left>=previousRight && cell.right>cell.left);
            assert(cell.top==45+row*27 && cell.bottom<=128);
            assert(cell.right<=ISLAND_WIDTH-16);
            previousRight=cell.right;
        }
    }

    SelectObject(dc,font);
    for(int index=0;index<9;++index) {
        RECT cell=expandedCell(ISLAND_WIDTH,index/3,(index%3)*2);
        SIZE size{};
        assert(GetTextExtentPoint32W(dc,EXPANDED_LABELS[index],
            lstrlenW(EXPANDED_LABELS[index]),&size));
        assert(size.cx<=cell.right-cell.left);
    }
    SelectObject(dc,original);
    g_expanded=false;
    g_fans.fan1=4000; g_fans.fan2=3000;
    refreshDisplayCache();
    g_fans.fan2=3100;
    assert(!refreshDisplayCache());
    g_expanded=true;
    g_fans.fan2=3200;
    assert(refreshDisplayCache());
    g_stats.memoryOk=true;
    g_stats.used=9985798963ULL;
    g_stats.total=16ULL<<30;
    refreshDisplayCache();
    assert(g_expandedDisplay.values[3]==L"9.3GB");
    assert(g_compactParts[1]==L"9.3/16G");
    g_stats.memoryOk=false;
    refreshDisplayCache();
    assert(g_expandedDisplay.values[3]==L"N/A");

    // A real color surface catches accidental rendering into the default 1x1 bitmap.
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
    auto assertEmpty=[] {
        assert(!g_backbufferDc && !g_backbufferBitmap && !g_backbufferOldBitmap);
        assert(g_backbufferWidth==0 && g_backbufferHeight==0);
        assert(!trackedDc && !trackedBitmap);
    };
    auto assertRestored=[&] {
        assert(GetCurrentObject(dc,OBJ_BITMAP)==output);
        assert(GetCurrentObject(dc,OBJ_FONT)==original);
        assert(GetCurrentObject(dc,OBJ_PEN)==originalPen);
        assert(GetCurrentObject(dc,OBJ_BRUSH)==originalBrush);
        assert(GetDCPenColor(dc)==RGB(12,34,56));
    };
    auto render=[&](int width,int height) {
        assert(SetWindowPos(window,nullptr,0,0,width,height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE));
        RECT rect{};
        assert(GetClientRect(window,&rect));
        assert(rect.right==width && rect.bottom==height);
        assert(PatBlt(dc,0,0,800,160,WHITENESS));
        paint(window);
        // Corners are outside the rounded border and text, but inside the fill.
        assert(GetPixel(dc,0,0)==RGB(18,18,20));
        assert(GetPixel(dc,width-1,height-1)==RGB(18,18,20));
        assertRestored();
    };
    auto assertBuffer=[&](int width,int height) {
        assert(g_backbufferDc && g_backbufferBitmap);
        assert(g_backbufferOldBitmap && g_backbufferOldBitmap!=HGDI_ERROR);
        assert(g_backbufferWidth==width && g_backbufferHeight==height);
        assert(GetCurrentObject(g_backbufferDc,OBJ_BITMAP)==g_backbufferBitmap);
        BITMAP bitmap{};
        assert(GetObjectW(g_backbufferBitmap,sizeof(bitmap),&bitmap)==sizeof(bitmap));
        assert(bitmap.bmWidth==width && bitmap.bmHeight==height);
        assert(bitmap.bmBitsPixel*bitmap.bmPlanes>=24);
    };
    // First text rendering initializes process-owned GDI resources. Warm both
    // rendering paths before measuring steady-state resource counts.
    for(bool expanded : {false,true}) {
        g_expanded=expanded;
        render(560,124);
        cleanupBackbuffer();
        assertEmpty();
    }
    const DWORD baseline=gdiHandles();
    for(bool expanded : {false,true}) {
        g_expanded=expanded;
        for(bool primaryFont : {false,true}) {
            g_font=primaryFont ? font : nullptr;
            render(560,124);
            assertBuffer(560,124);
            HDC cachedDc=g_backbufferDc;
            HBITMAP cachedBitmap=g_backbufferBitmap;
            int allocations=dcAllocations+bitmapAllocations;
            render(560,124);
            assert(g_backbufferDc==cachedDc && g_backbufferBitmap==cachedBitmap);
            assert(dcAllocations+bitmapAllocations==allocations);
            render(720,150);
            assertBuffer(720,150);
            cleanupBackbuffer();
            assert(restoredBeforeDelete);
            assertEmpty();
            assert(gdiHandles()==baseline);

            for(Failure fault : {Failure::Dc,Failure::Bitmap,
                                  Failure::SelectionNull,Failure::SelectionError}) {
                // Exercise both first allocation and replacement of an existing buffer.
                for(bool resize : {false,true}) {
                    if(resize) {
                        render(560,124);
                        assertBuffer(560,124);
                    }
                    int failuresBefore=injectedFailures;
                    failure=fault;
                    render(720,150);
                    assert(injectedFailures==failuresBefore+1 && failure==Failure::None);
                    assertEmpty();
                    assert(gdiHandles()==baseline);
                    render(720,150);
                    assertBuffer(720,150);
                    cleanupBackbuffer();
                    assert(restoredBeforeDelete);
                    assertEmpty();
                    assert(gdiHandles()==baseline);
                }
            }
        }
    }
    render(560,124);
    DWORD handlesBefore=gdiHandles();
    int allocations=dcAllocations+bitmapAllocations;
    for(int frame=0;frame<1000;++frame) paint(window);
    assert(dcAllocations+bitmapAllocations==allocations);
    assert(gdiHandles()==handlesBefore);
    assertRestored();
    for(int frame=0;frame<100;++frame) {
        render(frame%2 ? 560 : 720,frame%2 ? 124 : 150);
        assert(gdiHandles()==handlesBefore);
    }
    assert(!ensureBackbuffer(dc,0,124));
    assertEmpty();
    assert(ensureBackbuffer(dc,560,124));
    assert(!ensureBackbuffer(dc,560,-1));
    assertEmpty();
    assert(ensureBackbuffer(dc,560,124));
    assert(!ensureBackbuffer(nullptr,560,124));
    assertEmpty();
    cleanupBackbuffer(); // Repeated cleanup must be harmless.
    assertEmpty();
    assert(gdiHandles()==baseline);
    assert(DestroyWindow(window));

    assert(DeleteObject(g_backgroundBrush)); g_backgroundBrush=nullptr;
    g_font=nullptr; g_metricsFont=nullptr; g_smallFont=nullptr;
    assert(DeleteObject(font));
    assert(SelectObject(dc,originalBitmap)==output);
    assert(DeleteObject(output));
    assert(DeleteDC(dc));
    paintDc=nullptr;


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
}

int main() {
    runTests();
    // Repeat creation and destruction too: one-time process initialization is
    // excluded, but a leak per fixture must still fail the resource assertion.
    const DWORD warmedHandles=gdiHandles();
    runTests();
    assert(gdiHandles()==warmedHandles);
    puts("PASS: text cache, color output, resize/reuse, DC/bitmap/selection failures, GDI restoration/leaks, telemetry snapshots.");
}
