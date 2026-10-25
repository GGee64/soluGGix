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
    static BYTE g_alphaCurrent = 0;
    static BYTE g_textBrushAlpha = 255;

    static void LogMsg(const wchar_t* text) {
        OutputDebugStringW(text);
        OutputDebugStringW(L"\n");
    }

    HRESULT LoadTextureFromResourceToD2D(ID2D1HwndRenderTarget* rt, int resId, ID2D1Bitmap** ppOut) {
        if (!rt || !ppOut) return E_POINTER;
        *ppOut = nullptr;

        HMODULE hModule = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&LoadTextureFromResourceToD2D, &hModule);

        HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCEW(resId), RT_RCDATA);
        if (!hRes) hRes = FindResourceW(hModule, MAKEINTRESOURCEW(resId), L"PNG");
        if (!hRes) hRes = FindResourceW(hModule, MAKEINTRESOURCEW(resId), L"IMAGE");

        if (!hRes) {
            LogMsg(L"[Error] Texture resource not found!");
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

    class MFontEnumerator : public IDWriteFontFileEnumerator {
        ULONG m_refCount; IDWriteFontFile* m_pFontFile; bool m_hasCurrent;
    public:
        MFontEnumerator(IDWriteFontFile* pFile) : m_refCount(1), m_pFontFile(pFile), m_hasCurrent(false) { if (m_pFontFile) m_pFontFile->AddRef(); }
        ~MFontEnumerator() { if (m_pFontFile) m_pFontFile->Release(); }
        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteFontFileEnumerator)) { *ppvObject = this; AddRef(); return S_OK; }
            *ppvObject = nullptr; return E_NOINTERFACE;
        }
        virtual ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
        virtual ULONG STDMETHODCALLTYPE Release() override { ULONG c = InterlockedDecrement(&m_refCount); if (c == 0) delete this; return c; }
        virtual HRESULT STDMETHODCALLTYPE MoveNext(BOOL* hasCurrentView) override { *hasCurrentView = !m_hasCurrent; m_hasCurrent = true; return S_OK; }
        virtual HRESULT STDMETHODCALLTYPE GetCurrentFontFile(IDWriteFontFile** fontFile) override { if (!m_hasCurrent || !m_pFontFile) return E_FAIL; *fontFile = m_pFontFile; (*fontFile)->AddRef(); return S_OK; }
    };

    // 🟢 ИСПРАВЛЕННЫЙ ЛОАДЕР: Наследует сразу оба интерфейса (и файлы, и коллекции)
    class MemoryFontFileLoader : public IDWriteFontFileLoader, public IDWriteFontCollectionLoader {
        ULONG m_refCount; const void* m_pData; UINT32 m_size;
    public:
        MemoryFontFileLoader(const void* pData, UINT32 size) : m_refCount(1), m_pData(pData), m_size(size) {}

        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
            if (riid == __uuidof(IUnknown)) { *ppvObject = static_cast<IDWriteFontFileLoader*>(this); AddRef(); return S_OK; }
            if (riid == __uuidof(IDWriteFontFileLoader)) { *ppvObject = static_cast<IDWriteFontFileLoader*>(this); AddRef(); return S_OK; }
            if (riid == __uuidof(IDWriteFontCollectionLoader)) { *ppvObject = static_cast<IDWriteFontCollectionLoader*>(this); AddRef(); return S_OK; }
            *ppvObject = nullptr; return E_NOINTERFACE;
        }
        virtual ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
        virtual ULONG STDMETHODCALLTYPE Release() override { ULONG c = InterlockedDecrement(&m_refCount); if (c == 0) delete this; return c; }

        // IDWriteFontFileLoader
        virtual HRESULT STDMETHODCALLTYPE CreateStreamFromKey(const void* fontFileReferenceKey, UINT32 fontFileReferenceKeySize, IDWriteFontFileStream** fontFileStream) override {
            *fontFileStream = new MemoryFontFileStream(m_pData, m_size); return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE CreateEnumeratorFromKey(IDWriteFactory* factory, const void* collectionKey, UINT32 collectionKeySize, IDWriteFontFileEnumerator** fontFileEnumerator) override {
            if (!fontFileEnumerator) return E_POINTER;
            IDWriteFontFile* pFontFile = nullptr;
            HRESULT hr = factory->CreateCustomFontFileReference(collectionKey, collectionKeySize, this, &pFontFile);
            if (SUCCEEDED(hr) && pFontFile) {
                // 🟢 ЖЕЛЕЗНЫЙ ФИКС: Возвращаем наш собственный энумератор, вообще не дергая методы фабрики!
                *fontFileEnumerator = new MFontEnumerator(pFontFile);
                pFontFile->Release();
                return S_OK;
            }
            *fontFileEnumerator = nullptr; return hr;
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
        HRSRC hRes = FindResourceW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(resId), RT_RCDATA);
        if (!hRes) return nullptr;
        HGLOBAL hGlob = LoadResource(GetModuleHandleW(NULL), hRes);
        if (!hGlob) return nullptr;
        LPVOID pData = LockResource(hGlob);
        DWORD sz = SizeofResource(GetModuleHandleW(NULL), hRes);

        loader = new MemoryFontFileLoader(pData, sz);

        // Регистрируем лоадер в фабрике в обеих ролях
        factory->RegisterFontFileLoader(loader);
        factory->RegisterFontCollectionLoader(loader);

        static int keyElf = 501;
        static int keyHobbit = 502;
        void* pKey = (resId == ELVISH_FONT_ID) ? &keyElf : &keyHobbit;

        IDWriteFontCollection* pCollection = nullptr;

        // 🟢 ЧИСТЫЙ СТАНДАРТНЫЙ ВЫЗОВ: loader теперь легально приводится к IDWriteFontCollectionLoader!
        HRESULT hrColl = factory->CreateCustomFontCollection(
            static_cast<IDWriteFontCollectionLoader*>(loader),
            pKey,
            sizeof(int),
            &pCollection
        );

        char collLog[128];
        sprintf_s(collLog, "[FontDebug] CreateCustomFontCollection for ID %d returned HRESULT: 0x%08X\n", resId, hrColl);
        OutputDebugStringA(collLog);

        return pCollection;
    }

    void InitCalendarModule() {
        if (g_state.initialized) return;

        HRESULT hrInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        (void)hrInit;

        HMODULE hMod = GetModuleHandleW(NULL);
        char modLog[128]; sprintf_s(modLog, "[FontDebug] Target hMod address: 0x%p\n", hMod);
        OutputDebugStringA(modLog);

        HRSRC hRes1 = FindResourceW(hMod, MAKEINTRESOURCEW(ELVISH_FONT_ID), RT_RCDATA);
        char resLog1[128]; sprintf_s(resLog1, "[FontDebug] FindResourceW for Elvish (501) status: %s\n", hRes1 ? "FOUND" : "NULL");
        OutputDebugStringA(resLog1);

        HRSRC hRes2 = FindResourceW(hMod, MAKEINTRESOURCEW(HOBBIT_FONT_ID), RT_RCDATA);
        char resLog2[128]; sprintf_s(resLog2, "[FontDebug] FindResourceW for Hobbit (502) status: %s\n", hRes2 ? "FOUND" : "NULL");
        OutputDebugStringA(resLog2);

            HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_state.DWrite));
        char factoryLog[128]; sprintf_s(factoryLog, "[FontDebug] DWriteCreateFactory SHARED returned HRESULT: 0x%08X\n", hr);
        OutputDebugStringA(factoryLog);
        if (FAILED(hr) || !g_state.DWrite) { g_state.DWrite = nullptr; return; }
        g_state.CustomCollection1 = CreateCollectionFromResource(g_state.DWrite, g_pLoader1, ELVISH_FONT_ID);
        g_state.CustomCollection2 = CreateCollectionFromResource(g_state.DWrite, g_pLoader2, HOBBIT_FONT_ID);
        char collCheck1[128]; sprintf_s(collCheck1, "[FontDebug] CustomCollection1 final pointer: 0x%p\n", g_state.CustomCollection1);
        OutputDebugStringA(collCheck1);
        char collCheck2[128]; sprintf_s(collCheck2, "[FontDebug] CustomCollection2 final pointer: 0x%p\n", g_state.CustomCollection2);
        OutputDebugStringA(collCheck2);
        g_state.initialized = true;
    }
    void RenderCalendar(ID2D1HwndRenderTarget* rt, float screenW, float screenH) {
        if (!rt || !g_state.initialized || !g_state.DWrite) return;
        SYSTEMTIME st; GetLocalTime(&st);

        // 🔍 ОТЛАДОЧНЫЙ ТЕСТ: Вытаскиваем точное паспортное имя хоббитского шрифта из созданной коллекции
        static bool nameLogged = false;
        if (!nameLogged && g_state.CustomCollection2) {
            UINT32 familyCount = g_state.CustomCollection2->GetFontFamilyCount();
            for (UINT32 i = 0; i < familyCount; ++i) {
                IDWriteFontFamily* pFamily = nullptr;
                if (SUCCEEDED(g_state.CustomCollection2->GetFontFamily(i, &pFamily)) && pFamily) {
                    IDWriteLocalizedStrings* pNames = nullptr;
                    if (SUCCEEDED(pFamily->GetFamilyNames(&pNames)) && pNames) {
                        wchar_t actualName[64] = { 0 };
                        if (SUCCEEDED(pNames->GetString(0, actualName, 64))) {
                            OutputDebugStringA("[FontDebug] !!! REAL HOBBIT FAMILY NAME IN COLLECTION IS: ");
                            OutputDebugStringW(actualName);
                            OutputDebugStringA(" !!!\n");
                        }
                        pNames->Release();
                    }
                    pFamily->Release();
                }
            }
            nameLogged = true;
        }

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
        float alpha = g_alphaCurrent / 255.0f;
        if (!g_state.textBrush || g_textBrushAlpha != g_alphaCurrent) {
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha), &g_state.textBrush);
            g_textBrushAlpha = g_alphaCurrent;
        }
        const float PANEL_W = 380.0f;
        const float PANEL_H = 200.0f;
        const float panelX = (screenW - PANEL_W) * 0.5f;
        const float panelY = screenH * 0.3f;
        ID2D1Bitmap* pBg = nullptr;
        HRESULT hrTex = LoadTextureFromResourceToD2D(rt, IDB_PNG3, &pBg);
        if (SUCCEEDED(hrTex) && pBg) {
            rt->DrawBitmap(
                pBg,
                D2D1::RectF(panelX, panelY, panelX + PANEL_W, panelY + PANEL_H),
                alpha,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            pBg->Release();
        }
        else {
            static ID2D1SolidColorBrush* s_bgBrush = nullptr;
            if (!s_bgBrush || g_textBrushAlpha != g_alphaCurrent) {
                rt->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.06f, 0.12f, 0.95f * alpha), &s_bgBrush);
            }
            if (s_bgBrush) {
                rt->FillRoundedRectangle(D2D1::RoundedRect(
                    D2D1::RectF(panelX + 4.f, panelY + 4.f, panelX + PANEL_W - 4.f, panelY + PANEL_H - 4.f), 6.f, 6.f), s_bgBrush);
            }
        }
        // 1. Эльфийский месяц
        IDWriteTextFormat* pFormatMonth = nullptr;
        HRESULT hr = g_state.DWrite->CreateTextFormat(L"Tengwar Annatar", g_state.CustomCollection1, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 54.0f, L"", &pFormatMonth);
        if (SUCCEEDED(hr) && pFormatMonth) {
            pFormatMonth->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            pFormatMonth->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            int mIdx = st.wMonth;
            if (mIdx < 1 || mIdx > 12) mIdx = 1;
            rt->DrawTextW(MONTHS_ELVISH[mIdx - 1], (UINT32)wcslen(MONTHS_ELVISH[mIdx - 1]), pFormatMonth, D2D1::RectF(panelX, panelY + 60.f, panelX + PANEL_W, panelY + 45.f), g_state.textBrush);
            pFormatMonth->Release();
        }
        // 2. Хоббитский день недели
        IDWriteTextFormat* pFormatDay = nullptr;
        hr = g_state.DWrite->CreateTextFormat(L"Hobbiton Brushhand Hobbiton brush", g_state.CustomCollection2, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 30.0f, L"", &pFormatDay);
        if (SUCCEEDED(hr) && pFormatDay) {
            pFormatDay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            pFormatDay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const wchar_t* dayStr = DAYS_OF_WEEK[(st.wDayOfWeek == 0) ? 6 : st.wDayOfWeek - 1];
            rt->DrawTextW(dayStr, (UINT32)wcslen(dayStr), pFormatDay, D2D1::RectF(panelX, panelY + 130.f, panelX + PANEL_W, panelY + 80.f), g_state.textBrush);
            pFormatDay->Release();
        }
        // 3. Хоббитское число
        IDWriteTextFormat* pFormatNum = nullptr;
        hr = g_state.DWrite->CreateTextFormat(L"Hobbiton Brushhand Hobbiton brush", g_state.CustomCollection2, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 48.0f, L"", &pFormatNum);
        if (SUCCEEDED(hr) && pFormatNum) {
            pFormatNum->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            pFormatNum->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            wchar_t numBuf[8]; swprintf_s(numBuf, L"%d", st.wDay);
            rt->DrawTextW(numBuf, (UINT32)wcslen(numBuf), pFormatNum, D2D1::RectF(panelX, panelY + 200.f, panelX + PANEL_W, panelY + 115.f), g_state.textBrush);
            pFormatNum->Release();
        }
        g_anim.lastMonth = st.wMonth;
        g_anim.lastDay = st.wDay;
    }
    void ShutdownCalendarModule() {
        if (g_state.textBrush) g_state.textBrush->Release();
        g_state.textBrush = nullptr;
        if (g_state.CustomCollection1) { g_state.CustomCollection1->Release(); g_state.CustomCollection1 = nullptr; }
        if (g_state.CustomCollection2) { g_state.CustomCollection2->Release(); g_state.CustomCollection2 = nullptr; }
        if (g_state.DWrite) {
            if (g_pLoader1) g_state.DWrite->UnregisterFontFileLoader(g_pLoader1);
            if (g_pLoader1) g_state.DWrite->UnregisterFontCollectionLoader(g_pLoader1);
            if (g_pLoader2) g_state.DWrite->UnregisterFontFileLoader(g_pLoader2);
            if (g_pLoader2) g_state.DWrite->UnregisterFontCollectionLoader(g_pLoader2);
            g_state.DWrite->Release();
        }
        g_state.DWrite = nullptr;
        if (g_pLoader1) { delete g_pLoader1; g_pLoader1 = nullptr; }
        if (g_pLoader2) { delete g_pLoader2; g_pLoader2 = nullptr; }
        g_state.initialized = false;
    }
}