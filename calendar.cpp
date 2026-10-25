#include "calendar.h"
#include "resource.h"
#include <wincodec.h>
#include <cstdio>
#include <dwrite.h>
#include <shlwapi.h>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "dwrite.lib")

namespace Cal {
    // ✅ ИСПРАВЛЕНО: Вынесены в верхнюю часть namespace для корректной области видимости
    static BYTE g_alphaCurrent = 0;
    static BYTE g_textBrushAlpha = 255;

    static void LogMsg(const wchar_t* text) {
        OutputDebugStringW(text);
        OutputDebugStringW(L"\n");
    }

    HRESULT LoadTextureFromResourceToD2D(ID2D1HwndRenderTarget* rt, int resId, ID2D1Bitmap** ppOut) {
        if (!rt || !ppOut) return E_POINTER;
        *ppOut = nullptr;

        // ✅ ИСПРАВЛЕНИЕ: Получаем дескриптор того модуля, где находится ЭТОТ код 
        // (работает корректно и в EXE, и в DLL)
        HMODULE hModule = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&LoadTextureFromResourceToD2D, &hModule);

        // Ищем ресурс, используя правильный hModule
        HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCEW(resId), L"PNG");
        if (!hRes) {
            hRes = FindResourceW(hModule, MAKEINTRESOURCEW(resId), RT_RCDATA);
        }
        if (!hRes) {
            hRes = FindResourceW(hModule, MAKEINTRESOURCEW(resId), L"IMAGE");
        }

        if (!hRes) {
            LogMsg(L"[Error] Ресурс текстуры не найден в текущем модуле!");
            return HRESULT_FROM_WIN32(GetLastError());
        }

        HGLOBAL hGlob = LoadResource(hModule, hRes);
        if (!hGlob) return HRESULT_FROM_WIN32(GetLastError());

        LPVOID pData = LockResource(hGlob);
        DWORD sz = SizeofResource(hModule, hRes);
        if (!pData || sz == 0) return E_FAIL;

        IStream* pStream = SHCreateMemStream(static_cast<const BYTE*>(pData), sz);
        if (!pStream) return E_OUTOFMEMORY;

        IWICImagingFactory* pWIC = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pWIC));
        if (SUCCEEDED(hr)) {
            IWICBitmapDecoder* pDecoder = nullptr;
            hr = pWIC->CreateDecoderFromStream(pStream, nullptr, WICDecodeMetadataCacheOnLoad, &pDecoder);
            if (SUCCEEDED(hr)) {
                IWICFormatConverter* pConv = nullptr;
                hr = pWIC->CreateFormatConverter(&pConv);
                if (SUCCEEDED(hr)) {
                    IWICBitmapFrameDecode* pSrc = nullptr;
                    hr = pDecoder->GetFrame(0, &pSrc);
                    if (SUCCEEDED(hr)) {
                        hr = pConv->Initialize(pSrc, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);
                        if (SUCCEEDED(hr)) {
                            hr = rt->CreateBitmapFromWicBitmap(pConv, NULL, ppOut);
                        }
                        pSrc->Release();
                    }
                    pConv->Release();
                }
                pDecoder->Release();
            }
            else {
                LogMsg(L"[Error] WIC не смог декодировать данные из ресурса.");
            }
            pWIC->Release();
        }

        pStream->Release();
        return hr;
    }

    static const wchar_t* MONTHS_ELVISH[12] = {
        L"Aravanya",   L"Aramathie",  L"Erelase",    L"Yavannia",   L"Narie",      L"Miriel",
        L"Nelye",      L"Entulie",    L"Tuliqe",     L"Calie",      L"Lotharia",   L"Elbe"
    };
    static const wchar_t* DAYS_OF_WEEK[7] = {
        L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday", L"Sunday"
    };

    class MemoryFontFileStream : public IDWriteFontFileStream {
        ULONG m_refCount; const void* m_pData; UINT32 m_size;
    public:
        MemoryFontFileStream(const void* pData, UINT32 size) : m_refCount(1), m_pData(pData), m_size(size) {}
        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteFontFileStream)) { *ppvObject = this; AddRef(); return S_OK; }
            *ppvObject = nullptr; return E_NOINTERFACE;
        }
        virtual ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
        virtual ULONG STDMETHODCALLTYPE Release() override { ULONG c = InterlockedDecrement(&m_refCount); if (c == 0) delete this; return c; }
        virtual HRESULT STDMETHODCALLTYPE ReadFileFragment(const void** ppFragmentStart, UINT64 fileOffset, UINT64 fragmentSize, void** pContext) override {
            if (fileOffset + fragmentSize > m_size) return E_FAIL;
            *ppFragmentStart = (const BYTE*)m_pData + fileOffset; *pContext = nullptr; return S_OK;
        }
        virtual void STDMETHODCALLTYPE ReleaseFileFragment(void* context) override {}
        virtual HRESULT STDMETHODCALLTYPE GetFileSize(UINT64* fileSize) override { *fileSize = m_size; return S_OK; }
        virtual HRESULT STDMETHODCALLTYPE GetLastWriteTime(UINT64* lastWriteTime) override { *lastWriteTime = 0; return S_OK; }
    };

    class MemoryFontFileLoader : public IDWriteFontFileLoader {
        ULONG m_refCount; const void* m_pData; UINT32 m_size;
    public:
        MemoryFontFileLoader(const void* pData, UINT32 size) : m_refCount(1), m_pData(pData), m_size(size) {}
        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteFontFileLoader)) { *ppvObject = this; AddRef(); return S_OK; }
            *ppvObject = nullptr; return E_NOINTERFACE;
        }
        virtual ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
        virtual ULONG STDMETHODCALLTYPE Release() override { ULONG c = InterlockedDecrement(&m_refCount); if (c == 0) delete this; return c; }
        virtual HRESULT STDMETHODCALLTYPE CreateStreamFromKey(const void* fontFileReferenceKey, UINT32 fontFileReferenceKeySize, IDWriteFontFileStream** fontFileStream) override {
            *fontFileStream = new MemoryFontFileStream(m_pData, m_size); return S_OK;
        }
    };

    static MemoryFontFileLoader* g_pLoader1 = nullptr;
    static MemoryFontFileLoader* g_pLoader2 = nullptr;

    struct ModuleState {
        IDWriteFactory* DWrite = nullptr;
        IDWriteFontCollection* CustomCollection1 = nullptr;
        IDWriteFontCollection* CustomCollection2 = nullptr;
        ID2D1SolidColorBrush* textBrush = nullptr;
        bool initialized = false;
    };
    static ModuleState g_state;

    struct AnimationState {
        int lastMonth = -1, lastDay = -1;
        enum Phase { PH_INIT, PH_FADEIN, PH_HOLD, PH_FADEOUT } phase = PH_INIT;
        DWORD64 phaseStart = 0;
    };
    static AnimationState g_anim;

    IDWriteFontCollection* CreateCollectionFromResource(IDWriteFactory* factory, MemoryFontFileLoader*& loader, int resId) {
        HRSRC hRes = FindResourceW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(resId), L"IDR_MY_FONT");
        if (!hRes) return nullptr;
        HGLOBAL hGlob = LoadResource(GetModuleHandleW(NULL), hRes);
        if (!hGlob) return nullptr;
        LPVOID pData = LockResource(hGlob);
        DWORD sz = SizeofResource(GetModuleHandleW(NULL), hRes);

        loader = new MemoryFontFileLoader(pData, sz);
        factory->RegisterFontFileLoader(loader);

        static int keyElf = 101;
        static int keyHobbit = 102;
        void* pKey = (resId == 101) ? &keyElf : &keyHobbit;

        IDWriteFontFile* pFontFile = nullptr;
        IDWriteFontCollection* pCollection = nullptr;
        if (SUCCEEDED(factory->CreateCustomFontFileReference(pKey, sizeof(int), loader, &pFontFile))) {
            pFontFile->Release();
        }
        return pCollection;
    }

    void InitCalendarModule() {
        if (g_state.initialized) return;
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

        wchar_t exeDir[MAX_PATH]; GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
        wchar_t* pSlash = wcsrchr(exeDir, L'\\'); if (pSlash) *(pSlash + 1) = '\0';

        bool elvLoaded = false;
        HRSRC hRes = FindResourceW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(ELVISH_FONT_ID), L"IDR_MY_FONT");
        if (hRes) {
            HGLOBAL hGlob = LoadResource(GetModuleHandleW(NULL), hRes);
            if (hGlob) { LPVOID pData = LockResource(hGlob); DWORD sz = SizeofResource(GetModuleHandleW(NULL), hRes); ULONG c = 0; HANDLE f = AddFontMemResourceEx(pData, sz, nullptr, &c); elvLoaded = (f != nullptr && c > 0); }
        }
        if (!elvLoaded) { wchar_t path1[MAX_PATH]; wcscpy_s(path1, MAX_PATH, exeDir); wcscat_s(path1, MAX_PATH, L"elvish.ttf"); AddFontResourceW(path1); }

        bool dayLoaded = false;
        HRSRC hResD = FindResourceW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(107), L"IDR_MY_FONT");
        if (hResD) {
            HGLOBAL hGlobD = LoadResource(GetModuleHandleW(NULL), hResD);
            if (hGlobD) { LPVOID pDataD = LockResource(hGlobD); DWORD szD = SizeofResource(GetModuleHandleW(NULL), hResD); ULONG c = 0; HANDLE f2 = AddFontMemResourceEx(pDataD, szD, nullptr, &c); dayLoaded = (f2 != nullptr && c > 0); }
        }
        if (!dayLoaded) { wchar_t path2[MAX_PATH]; wcscpy_s(path2, MAX_PATH, exeDir); wcscat_s(path2, MAX_PATH, L"dayfont.ttf"); AddFontResourceW(path2); }

        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_state.DWrite));
        if (FAILED(hr)) g_state.DWrite = nullptr;

        g_state.initialized = true;
    }

    void RenderCalendar(ID2D1HwndRenderTarget* rt, float screenW, float screenH) {
        if (!rt || !g_state.initialized || !g_state.DWrite) return;

        // ✅ ИСПРАВЛЕНО: Декларация только один раз в области видимости функции
        SYSTEMTIME st; GetLocalTime(&st);

        bool dateChanged = (g_anim.lastMonth != -1 && (st.wMonth != g_anim.lastMonth || st.wDay != g_anim.lastDay));
        if (!g_anim.phaseStart) { g_anim.phase = AnimationState::PH_FADEIN; g_anim.phaseStart = GetTickCount64(); }
        if (dateChanged) { g_anim.phase = AnimationState::PH_FADEIN; g_anim.phaseStart = GetTickCount64(); }

        DWORD64 now = GetTickCount64();
        float elapsed = static_cast<float>(now - g_anim.phaseStart);

        switch (g_anim.phase) {
        case AnimationState::PH_FADEIN:
            g_alphaCurrent = (BYTE)(min(1.0f, elapsed / 2800.0f) * 255.0f);
            if (elapsed >= 2800.0f) { g_anim.phase = AnimationState::PH_HOLD; g_anim.phaseStart = now; }
            break;
        case AnimationState::PH_HOLD:
            g_alphaCurrent = 255;
            if (elapsed >= 5000.0f) { g_anim.phase = AnimationState::PH_FADEOUT; g_anim.phaseStart = now; }
            break;
        case AnimationState::PH_FADEOUT:
            g_alphaCurrent = (BYTE)(max(0.0f, min(1.0f, (5000.0f - elapsed) / 400.0f)) * 255.0f);
            if (elapsed >= 5000.0f) { g_anim.phase = AnimationState::PH_INIT; g_alphaCurrent = 0; }
            break;
        }

        if (g_alphaCurrent == 0 && g_anim.phase == AnimationState::PH_INIT) return;

        float a = g_alphaCurrent / 255.0f;

        if (!g_state.textBrush || g_textBrushAlpha != g_alphaCurrent) {
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, a), &g_state.textBrush);
            g_textBrushAlpha = g_alphaCurrent;
        }

        const float PANEL_W = 380.0f;
        const float PANEL_H = 200.0f;
        const float panelX = (screenW - PANEL_W) * 0.5f;
        const float panelY = screenH * 0.3f;

        float alpha = g_alphaCurrent / 255.0f;

        ID2D1Bitmap* pBg = nullptr;
        HRESULT hrTex = LoadTextureFromResourceToD2D(rt, IDB_PNG3, &pBg);

        HRSRC hResD = FindResourceW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(107), L"RCDATA");

        if (SUCCEEDED(hrTex) && pBg) {
            // 5-й параметр bitmapOpacity автоматически умножает альфа-канал текстуры на alpha
            rt->DrawBitmap(
                pBg,
                D2D1::RectF(panelX, panelY, panelX + PANEL_W, panelY + PANEL_H),
                alpha,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            pBg->Release();
        }
        else {
            // Fallback-кисть уже умножает 0.95f * a, поэтому она тоже тускнеет синхронно
            static ID2D1SolidColorBrush* s_bgBrush = nullptr;
            if (!s_bgBrush || g_textBrushAlpha != g_alphaCurrent) {
                rt->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.06f, 0.12f, 0.95f * alpha), &s_bgBrush);
            }
            rt->FillRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(panelX + 4.f, panelY + 4.f, panelX + PANEL_W - 4.f, panelY + PANEL_H - 4.f), 6.f, 6.f), s_bgBrush);
        }

        IDWriteTextFormat* pFormatMonth = nullptr;
        HRESULT hr = g_state.DWrite->CreateTextFormat(L"Tengwar Annatar", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 54.0f, L"", &pFormatMonth);
        if (SUCCEEDED(hr) && pFormatMonth) {
            pFormatMonth->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            pFormatMonth->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            rt->DrawTextW(MONTHS_ELVISH[(st.wMonth >= 1 && st.wMonth <= 12) ? st.wMonth - 1 : 0], (UINT32)wcslen(MONTHS_ELVISH[st.wMonth - 1]), pFormatMonth, D2D1::RectF(panelX, panelY + 60.f, panelX + PANEL_W, panelY + 45.f), g_state.textBrush);
            pFormatMonth->Release();
        }

        IDWriteTextFormat* pFormatDay = nullptr;
        hr = g_state.DWrite->CreateTextFormat(L"Hobbiton Brushhand", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 30.0f, L"", &pFormatDay);
        if (SUCCEEDED(hr) && pFormatDay) {
            pFormatDay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            pFormatDay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const wchar_t* dayStr = DAYS_OF_WEEK[(st.wDayOfWeek == 0) ? 6 : st.wDayOfWeek - 1];
            rt->DrawTextW(dayStr, (UINT32)wcslen(dayStr), pFormatDay, D2D1::RectF(panelX, panelY + 130.f, panelX + PANEL_W, panelY + 80.f), g_state.textBrush);
            pFormatDay->Release();
        }

        IDWriteTextFormat* pFormatNum = nullptr;
        hr = g_state.DWrite->CreateTextFormat(L"Hobbiton Brushhand", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 48.0f, L"", &pFormatNum);
        if (SUCCEEDED(hr) && pFormatNum) {
            pFormatNum->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            pFormatNum->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            wchar_t numBuf[8]; swprintf_s(numBuf, L"%d", st.wDay);
            rt->DrawTextW(numBuf, (UINT32)wcslen(numBuf), pFormatNum, D2D1::RectF(panelX, panelY + 200.f, panelX + PANEL_W, panelY + 115.f), g_state.textBrush);
            pFormatNum->Release();
        }
    }

    void ShutdownCalendarModule() {
        if (g_state.textBrush) g_state.textBrush->Release();
        g_state.textBrush = nullptr;
        if (g_state.DWrite) g_state.DWrite->Release();
        g_state.DWrite = nullptr;

        wchar_t exeDir[MAX_PATH]; GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
        wchar_t* pSlash = wcsrchr(exeDir, L'\\'); if (pSlash) *(pSlash + 1) = 0;

        wchar_t pathElv[MAX_PATH]; wcscpy_s(pathElv, MAX_PATH, exeDir); wcscat_s(pathElv, MAX_PATH, L"elvish.ttf");
        wchar_t pathDay[MAX_PATH]; wcscpy_s(pathDay, MAX_PATH, exeDir); wcscat_s(pathDay, MAX_PATH, L"dayfont.ttf");

        RemoveFontResourceW(pathElv); RemoveFontResourceW(pathDay);
        PostMessageW(HWND_BROADCAST, WM_FONTCHANGE, 0, 0);
        g_state.initialized = false;
    }
}
