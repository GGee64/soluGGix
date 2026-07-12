#include "daycycle.h"
#include "resource.h"
#include <windows.h>
#include <cmath>
#include <wincodec.h>
#include <vector>
#include <shlwapi.h> // Добавлен этот инклюд
#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "Shlwapi.lib") // Добавлена эта линковка

static std::vector<ID2D1Bitmap*> g_sunFrames;

HRESULT LoadTextureFromResource(ID2D1HwndRenderTarget* rt, int resId, ID2D1Bitmap** ppBitmap, std::vector<ID2D1Bitmap*>* pGifFrames = nullptr) {
    if (!rt) return E_POINTER;
    if (ppBitmap) *ppBitmap = nullptr;

    // 1. Авто-определение HMODULE для поддержки EXE и DLL
    HMODULE hModule = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&LoadTextureFromResource, &hModule);

    // 2. Ищем ресурс по цепочке приоритетов типов: PNG -> RCDATA -> IMAGE
    HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCEW(resId), L"PNG");
    if (!hRes) hRes = FindResourceW(hModule, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hRes) hRes = FindResourceW(hModule, MAKEINTRESOURCEW(resId), L"IMAGE");

    if (!hRes) return HRESULT_FROM_WIN32(GetLastError());

    HGLOBAL hGlob = LoadResource(hModule, hRes);
    if (!hGlob) return HRESULT_FROM_WIN32(GetLastError());

    LPVOID pData = LockResource(hGlob);
    DWORD sz = SizeofResource(hModule, hRes);
    if (!pData || sz == 0) return E_FAIL;

    // 3. Создаем IStream из байт памяти
    IStream* pStream = SHCreateMemStream(static_cast<const BYTE*>(pData), sz);
    if (!pStream) return E_OUTOFMEMORY;

    IWICImagingFactory* pWICFactory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pWICFactory));
    if (FAILED(hr)) {
        pStream->Release();
        return hr;
    }

    IWICBitmapDecoder* pDecoder = nullptr;
    // ✅ ИЗМЕНЕНО: Загрузка из потока памяти, а не из файла
    hr = pWICFactory->CreateDecoderFromStream(pStream, nullptr, WICDecodeMetadataCacheOnLoad, &pDecoder);
    if (SUCCEEDED(hr)) {
        UINT frameCount = 1;
        if (pGifFrames) {
            pDecoder->GetFrameCount(&frameCount);
        }

        for (UINT i = 0; i < frameCount; ++i) {
            IWICBitmapFrameDecode* pSource = nullptr;
            hr = pDecoder->GetFrame(i, &pSource);
            if (SUCCEEDED(hr)) {
                IWICFormatConverter* pConverter = nullptr;
                hr = pWICFactory->CreateFormatConverter(&pConverter);
                if (SUCCEEDED(hr)) {
                    hr = pConverter->Initialize(pSource, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom);
                    if (SUCCEEDED(hr)) {
                        ID2D1Bitmap* pBitmap = nullptr;
                        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
                        );
                        hr = rt->CreateBitmapFromWicBitmap(pConverter, &props, &pBitmap);
                        if (SUCCEEDED(hr) && pBitmap) {
                            if (pGifFrames) {
                                pGifFrames->push_back(pBitmap);
                            }
                            else if (ppBitmap) {
                                *ppBitmap = pBitmap;
                            }
                        }
                    }
                    pConverter->Release();
                }
                pSource->Release();
            }
        }
        pDecoder->Release();
    }
    pWICFactory->Release();
    pStream->Release(); // Важно освободить поток
    return hr;
}

static const float PI_F = 3.14159265f;

HRESULT InitDayCycle(ID2D1HwndRenderTarget* rt, DayCycleState* state) {
    if (!rt || !state) return E_POINTER;
    state->initialized = true;
    return S_OK;
}

void RenderDayCycle(ID2D1HwndRenderTarget* rt, float screenW, float screenH, const DayCycleState& state) {
    // ✅ ХАК: Находим реальное окно панели задач Windows в реальном времени
    HWND hTaskbarWnd = ::FindWindowW(L"Shell_TrayWnd", NULL);
    bool isClipPushed = false;

    if (hTaskbarWnd && ::IsWindowVisible(hTaskbarWnd)) {
        RECT rectTaskbar;
        if (::GetWindowRect(hTaskbarWnd, &rectTaskbar)) {
            POINT pt = { rectTaskbar.left, rectTaskbar.top };
            ::ScreenToClient(rt->GetHwnd(), &pt);

            if (pt.y > 0 && pt.y < screenH) {
                D2D1_RECT_F safeRenderArea = D2D1::RectF(0.0f, 0.0f, screenW, (float)pt.y);
                rt->PushAxisAlignedClip(safeRenderArea, D2D1_ANTIALIAS_MODE_ALIASED);
                isClipPushed = true;
            }
        }
    }

    if (!rt || !state.initialized) {
        if (isClipPushed) rt->PopAxisAlignedClip();
        return;
    }

    DayCycleState& mutableState = const_cast<DayCycleState&>(state);

    if (mutableState.bgLeft == nullptr) {
        LoadTextureFromResource(rt, IDB_PNG1, &mutableState.bgLeft);
    }
    if (mutableState.bgRight == nullptr) {
        LoadTextureFromResource(rt, IDB_PNG2, &mutableState.bgRight);
    }
    if (g_sunFrames.empty()) {
        LoadTextureFromResource(rt, IDB_PNG5, nullptr, &g_sunFrames);
    }
    if (mutableState.moon == nullptr) {
        LoadTextureFromResource(rt, IDB_PNG4, &mutableState.moon);
    }

    SYSTEMTIME st; GetLocalTime(&st);

    static int lastCachedMinute = -1;
    static float cached_sx1 = 0.0f, cached_sy1 = 0.0f;
    static float cached_sx2 = 0.0f, cached_sy2 = 0.0f;
    static float cached_mx1 = 0.0f, cached_my1 = 0.0f;
    static float cached_mx2 = 0.0f, cached_my2 = 0.0f;

    static float cached_sun_sw = 0.0f;
    static float cached_sun_sh = 0.0f;

    static float cached_moon_mw = 0.0f;
    static float cached_moon_mh = 0.0f;

    RECT workArea;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);
    int screenHeightPhysical = GetSystemMetrics(SM_CYSCREEN);
    float taskbarHeight = static_cast<float>(screenHeightPhysical - workArea.bottom);
    float baselineY = screenH - taskbarHeight;

    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

    float textureScale = 0.35f;

    if (state.bgLeft) {
        D2D1_SIZE_F originalSize = state.bgLeft->GetSize();
        float scaledWidth = originalSize.width * textureScale;
        float scaledHeight = originalSize.height * textureScale;
        D2D1_RECT_F rectLeft = D2D1::RectF(0.0f, baselineY - scaledHeight, scaledWidth, baselineY);
        rt->DrawBitmap(state.bgLeft, rectLeft);
    }

    if (state.bgRight) {
        D2D1_SIZE_F originalSize = state.bgRight->GetSize();
        float scaledWidth = originalSize.width * textureScale;
        float scaledHeight = originalSize.height * textureScale;
        D2D1_RECT_F rectRight = D2D1::RectF(screenW - scaledWidth, baselineY - scaledHeight, screenW, baselineY);
        rt->DrawBitmap(state.bgRight, rectRight);
    }

    float R = fminf(250.0f, screenH * 0.15f);
    if (R < 30.0f) R = 30.0f;

    if (st.wMinute != lastCachedMinute) {
        int sec = st.wHour * 3600 + st.wMinute * 60 + st.wSecond;

        float leftPivotX = 0.0f;
        float leftPivotY = baselineY;
        float rightPivotX = screenW;
        float rightPivotY = baselineY;

        float tDayGlobal = sec / 86400.0f;
        float angleSun = 2.0f * PI_F * (tDayGlobal - 0.5f);
        float cosSun = cosf(angleSun);
        float sinSun = sinf(angleSun);

        cached_sx1 = leftPivotX + R * sinSun;
        cached_sy1 = leftPivotY - R * cosSun;
        cached_sx2 = rightPivotX + R * sinSun;
        cached_sy2 = rightPivotY - R * cosSun;

        float angleMoon = 2.0f * PI_F * (tDayGlobal - (tDayGlobal > 0.5f ? 1.0f : 0.0f));
        float cosMoon = cosf(angleMoon);
        float sinMoon = sinf(angleMoon);

        cached_mx1 = leftPivotX + R * sinMoon;
        cached_my1 = leftPivotY - R * cosMoon;
        cached_mx2 = rightPivotX + R * sinMoon;
        cached_my2 = rightPivotY - R * cosMoon;

        if (!g_sunFrames.empty()) {
            D2D1_SIZE_F sunSize = g_sunFrames[0]->GetSize();
            float sunScale = 0.35f;
            cached_sun_sw = sunSize.width * sunScale;
            cached_sun_sh = sunSize.height * sunScale;
        }

        if (mutableState.moon) {
            D2D1_SIZE_F moonSize = mutableState.moon->GetSize();
            float moonScale = 0.35f; // Держим единый масштаб со светилами
            cached_moon_mw = moonSize.width * moonScale;
            cached_moon_mh = moonSize.height * moonScale;
        }

        lastCachedMinute = st.wMinute;
    }

    // ✅ ПОЛНЫЙ РОТАТОР КАДРОВ СОЛНЦА (Восстановлен и закрыт)
    if (!g_sunFrames.empty() && cached_sun_sw > 0.0f) {
        ULONGLONG currentTimeMs = GetTickCount64();
        int frameDelayMs = 60;
        UINT currentFrameIdx = (currentTimeMs / frameDelayMs) % g_sunFrames.size();
        ID2D1Bitmap* pCurrentSunBitmap = g_sunFrames[currentFrameIdx];

        if (pCurrentSunBitmap) {
            D2D1_RECT_F rectSun1 = D2D1::RectF(cached_sx1 - cached_sun_sw / 2.0f, cached_sy1 - cached_sun_sh / 2.0f, cached_sx1 + cached_sun_sw / 2.0f, cached_sy1 + cached_sun_sh / 2.0f);
            D2D1_RECT_F rectSun2 = D2D1::RectF(cached_sx2 - cached_sun_sw / 2.0f, cached_sy2 - cached_sun_sh / 2.0f, cached_sx2 + cached_sun_sw / 2.0f, cached_sy2 + cached_sun_sh / 2.0f);
            rt->DrawBitmap(pCurrentSunBitmap, rectSun1);
            rt->DrawBitmap(pCurrentSunBitmap, rectSun2);
        }
    }

    // ✅ ВОТ ЭТОТ КРИТИЧЕСКИЙ КУСОК ТЫ ПОТЕРЯЛ: ОТРИСОВКА ЛУНЫ ТЕКСТУРОЙ ИЗ MOON.PNG
    if (mutableState.moon && cached_moon_mw > 0.0f) {
        D2D1_RECT_F rectMoon1 = D2D1::RectF(cached_mx1 - cached_moon_mw / 2.0f, cached_my1 - cached_moon_mh / 2.0f, cached_mx1 + cached_moon_mw / 2.0f, cached_my1 + cached_moon_mh / 2.0f);
        D2D1_RECT_F rectMoon2 = D2D1::RectF(cached_mx2 - cached_moon_mw / 2.0f, cached_my2 - cached_moon_mh / 2.0f, cached_mx2 + cached_moon_mw / 2.0f, cached_my2 + cached_moon_mh / 2.0f);

        rt->DrawBitmap(mutableState.moon, rectMoon1);
        rt->DrawBitmap(mutableState.moon, rectMoon2);
    }

    if (isClipPushed) {
        rt->PopAxisAlignedClip();
    }
}

void ShutdownDayCycle(DayCycleState& state) {
    if (state.bgLeft) {
        state.bgLeft->Release();
        state.bgLeft = nullptr;
    }
    if (state.bgRight) {
        state.bgRight->Release();
        state.bgRight = nullptr;
    }

    for (auto frame : g_sunFrames) {
        if (frame) frame->Release();
    }
    g_sunFrames.clear();

    if (state.moon) { state.moon->Release(); state.moon = nullptr; }

    state.initialized = false;
}