#include "daycycle.h"
#include "resource.h"
#include <windows.h>
#include <cmath>
#include <wincodec.h>
#include <vector>
#include <shlwapi.h>

#include <d2d1_1.h> // Добавлен для флага COMPOSITE_MODE

#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "Shlwapi.lib")

extern HWND hOverlayWnd;

std::vector<ID2D1Bitmap*> g_sunFrames;
static ID2D1Bitmap* g_moonFlippedBitmap = nullptr;

float cached_sun_sw = 0.0f;
float cached_sun_sh = 0.0f;
float cached_moon_mw = 0.0f;
float cached_moon_mh = 0.0f;

HRESULT LoadTextureFromResource(ID2D1HwndRenderTarget* rt, int resId, ID2D1Bitmap** ppBitmap, std::vector<ID2D1Bitmap*>* pGifFrames) {
    if (!rt) return E_POINTER;
    if (ppBitmap) *ppBitmap = nullptr;

    HMODULE hModule = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&LoadTextureFromResource, &hModule);

    HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCEW(resId), L"PNG");
    if (!hRes) hRes = FindResourceW(hModule, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hRes) hRes = FindResourceW(hModule, MAKEINTRESOURCEW(resId), L"IMAGE");

    if (!hRes) return HRESULT_FROM_WIN32(GetLastError());

    HGLOBAL hGlob = LoadResource(hModule, hRes);
    if (!hGlob) return HRESULT_FROM_WIN32(GetLastError());

    LPVOID pData = LockResource(hGlob);
    DWORD sz = SizeofResource(hModule, hRes);
    if (!pData || sz == 0) return E_FAIL;

    IStream* pStream = SHCreateMemStream(static_cast<const BYTE*>(pData), sz);
    if (!pStream) return E_OUTOFMEMORY;

    IWICImagingFactory* pWICFactory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pWICFactory));
    if (FAILED(hr)) {
        pStream->Release();
        return hr;
    }

    IWICBitmapDecoder* pDecoder = nullptr;
    hr = pWICFactory->CreateDecoderFromStream(pStream, nullptr, WICDecodeMetadataCacheOnLoad, &pDecoder);
    if (SUCCEEDED(hr)) {
        UINT frameCount = 1;
        pDecoder->GetFrameCount(&frameCount);

        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );

        // 🟢 ЛОГИКА ДЛЯ АНИМИРОВАННОГО СОЛНЦА (GIF)
        if (pGifFrames) {
            UINT globalWidth = 0, globalHeight = 0;
            IWICBitmapFrameDecode* pFirstFrame = nullptr;
            if (SUCCEEDED(pDecoder->GetFrame(0, &pFirstFrame))) {
                pFirstFrame->GetSize(&globalWidth, &globalHeight);
                pFirstFrame->Release();
            }

            if (globalWidth == 0 || globalHeight == 0) {
                globalWidth = 512; globalHeight = 512;
            }

            ID2D1BitmapRenderTarget* pGpuCanvas = nullptr;
            D2D1_SIZE_U canvasSize = D2D1::SizeU(globalWidth, globalHeight);
            if (SUCCEEDED(rt->CreateCompatibleRenderTarget(nullptr, &canvasSize, &props.pixelFormat, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &pGpuCanvas)) && pGpuCanvas) {
                pGpuCanvas->BeginDraw();
                pGpuCanvas->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
                pGpuCanvas->EndDraw();
            }

            for (UINT i = 0; i < frameCount; ++i) {
                IWICBitmapFrameDecode* pSource = nullptr;
                if (SUCCEEDED(pDecoder->GetFrame(i, &pSource))) {
                    UINT offsetX = 0, offsetY = 0;

                    IWICMetadataQueryReader* pMetadataReader = nullptr;
                    if (SUCCEEDED(pSource->GetMetadataQueryReader(&pMetadataReader)) && pMetadataReader) {
                        PROPVARIANT propLeft, propTop;
                        PropVariantInit(&propLeft); PropVariantInit(&propTop);
                        if (SUCCEEDED(pMetadataReader->GetMetadataByName(L"/imgdesc/Left", &propLeft)) && propLeft.vt == VT_UI2) {
                            offsetX = propLeft.uiVal;
                        }
                        if (SUCCEEDED(pMetadataReader->GetMetadataByName(L"/imgdesc/Top", &propTop)) && propTop.vt == VT_UI2) {
                            offsetY = propTop.uiVal;
                        }
                        PropVariantClear(&propLeft); PropVariantClear(&propTop);
                        pMetadataReader->Release();
                    }

                    IWICFormatConverter* pConverter = nullptr;
                    if (SUCCEEDED(pWICFactory->CreateFormatConverter(&pConverter))) {
                        if (SUCCEEDED(pConverter->Initialize(pSource, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom))) {
                            ID2D1Bitmap* pRawWicBitmap = nullptr;
                            if (SUCCEEDED(rt->CreateBitmapFromWicBitmap(pConverter, &props, &pRawWicBitmap)) && pRawWicBitmap) {
                                if (pGpuCanvas) {
                                    pGpuCanvas->BeginDraw();
                                    D2D1_RECT_F frameDestRect = D2D1::RectF(
                                        (float)offsetX, (float)offsetY,
                                        (float)offsetX + pRawWicBitmap->GetSize().width,
                                        (float)offsetY + pRawWicBitmap->GetSize().height
                                    );
                                    pGpuCanvas->DrawBitmap(pRawWicBitmap, frameDestRect);
                                    pGpuCanvas->EndDraw();

                                    ID2D1Bitmap* pLiveCanvasBitmap = nullptr;
                                    if (SUCCEEDED(pGpuCanvas->GetBitmap(&pLiveCanvasBitmap)) && pLiveCanvasBitmap) {
                                        ID2D1Bitmap* pStaticFrameClone = nullptr;
                                        if (SUCCEEDED(rt->CreateBitmap(canvasSize, props, &pStaticFrameClone)) && pStaticFrameClone) {
                                            pStaticFrameClone->CopyFromBitmap(nullptr, pLiveCanvasBitmap, nullptr);
                                            pGifFrames->push_back(pStaticFrameClone);
                                        }
                                        pLiveCanvasBitmap->Release();
                                    }
                                }
                                pRawWicBitmap->Release();
                            }
                        }
                        pConverter->Release();
                    }
                    pSource->Release();
                }
            }
            if (pGpuCanvas) pGpuCanvas->Release();
        }
        // 🟢 ЛОГИКА ДЛЯ ОБЫЧНЫХ ОДИНОЧНЫХ КАРТИНОК (PNG: bgLeft, bgRight, moon)
        else if (ppBitmap) {
            IWICBitmapFrameDecode* pSource = nullptr;
            if (SUCCEEDED(pDecoder->GetFrame(0, &pSource))) {
                IWICFormatConverter* pConverter = nullptr;
                if (SUCCEEDED(pWICFactory->CreateFormatConverter(&pConverter))) {
                    if (SUCCEEDED(pConverter->Initialize(pSource, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom))) {
                        rt->CreateBitmapFromWicBitmap(pConverter, &props, ppBitmap);
                    }
                    pConverter->Release();
                }
                pSource->Release();
            }
        }
        pDecoder->Release();
    }
    pWICFactory->Release();
    pStream->Release();
    return hr;
}

static const float PI_F = 3.14159265f;

HRESULT InitDayCycle(ID2D1HwndRenderTarget* rt, DayCycleState* state) {
    if (!rt || !state) return E_POINTER;
    state->initialized = true;
    return S_OK;
}

void RenderDayCycle(ID2D1HwndRenderTarget* rt, float screenW, float screenH, const DayCycleState& state) {
    // 🟢 ВОЗВРАЩАЕМ РОДНОЙ И СТАБИЛЬНЫЙ КЛИП ПАНЕЛИ ЗАДАЧ (Луна больше не залезет в трей)
    HWND hTaskbarWnd = ::FindWindowW(L"Shell_TrayWnd", NULL);
    bool isClipPushed = false;

    if (hTaskbarWnd && ::IsWindowVisible(hTaskbarWnd)) {
        RECT rectTaskbar;
        if (::GetWindowRect(hTaskbarWnd, &rectTaskbar)) {
            POINT pt = { rectTaskbar.left, rectTaskbar.top };
            ::ScreenToClient(hOverlayWnd, &pt);

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
        LoadTextureFromResource(rt, IDB_GIF1, nullptr, &g_sunFrames);
    }
    if (mutableState.moon == nullptr) {
        LoadTextureFromResource(rt, IDB_PNG4, &mutableState.moon);
    }
    if (g_moonFlippedBitmap == nullptr) {
        LoadTextureFromResource(rt, IDB_PNG6, &g_moonFlippedBitmap);
    }

    SYSTEMTIME st; GetLocalTime(&st);

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

    // 🟢 ВОЗВРАЩАЕМ ВАШУ ОРИГИНАЛЬНУЮ МАТЕМАТИКУ (Поминутный расчет, никаких шлейфов!)
    static int lastCachedMinute = -1;
    static float cached_sx1 = 0.0f, cached_sy1 = 0.0f;
    static float cached_sx2 = 0.0f, cached_sy2 = 0.0f;
    static float cached_mx1 = 0.0f, cached_my1 = 0.0f;
    static float cached_mx2 = 0.0f, cached_my2 = 0.0f;

    if (st.wMinute != lastCachedMinute) {
        int secGlobal = st.wHour * 3600 + st.wMinute * 60;
        float tDayGlobal = static_cast<float>(secGlobal) / 86400.0f;

        float leftPivotX = 0.0f; float leftPivotY = baselineY;
        float rightPivotX = screenW; float rightPivotY = baselineY;

        float angleSun = 2.0f * PI_F * (tDayGlobal - 0.5f);
        float cosSun = cosf(angleSun); float sinSun = sinf(angleSun);

        cached_sx1 = leftPivotX + R * sinSun; cached_sy1 = leftPivotY - R * cosSun;
        cached_sx2 = rightPivotX + R * sinSun; cached_sy2 = rightPivotY - R * cosSun;

        float angleMoon = 2.0f * PI_F * (tDayGlobal - (tDayGlobal > 0.5f ? 1.0f : 0.0f));
        float cosMoon = cosf(angleMoon); float sinMoon = sinf(angleMoon);

        cached_mx1 = leftPivotX + R * sinMoon; cached_my1 = leftPivotY - R * cosMoon;
        cached_mx2 = rightPivotX + R * sinMoon; cached_my2 = rightPivotY - R * cosMoon;

        lastCachedMinute = st.wMinute;
    }

    // 🟢 ВОЗВРАЩАЕМ ПРАВИЛЬНЫЙ РАСЧЕТ ПРОПОРЦИЙ ИЗ РАЗМЕРА ТЕКСТУРЫ
    if (!g_sunFrames.empty()) {
        D2D1_SIZE_F sunSize = g_sunFrames[0]->GetSize();
        cached_sun_sw = sunSize.width * 0.35f;
            cached_sun_sh = sunSize.height * 0.35f;
    }
    if (mutableState.moon) {
        D2D1_SIZE_F moonSize = mutableState.moon->GetSize();
        cached_moon_mw = moonSize.width * 0.35f;
        cached_moon_mh = moonSize.height * 0.35f;
    }
    // ✅ ИСПРАВЛЕННЫЙ РОТАТОР КАДРОВ С ЛОКАЛЬНЫМ СТАБИЛЬНЫМ ТАЙМЕРОМ (0% ДРЕДАНИЯ КАДРОВ)
    // ✅ ИСПРАВЛЕННЫЙ ВЫВОД: Аппаратный Clip панели задач без деформации текстуры
    if (!g_sunFrames.empty() && cached_sun_sw > 0.0f) {
        ULONGLONG currentTimeMs = GetTickCount64();
        int frameDelayMs = 60;

        // 🟢 ИСПРАВЛЕННЫЙ РАСЧЕТ ИНДЕКСА ДЛЯ РЕЖИМА "ПИНГ-ПОНГ" (Туда и обратно)
       // 1. Вычисляем полный цикл виртуальных кадров (туда + обратно минус стыковочные кадры)
        UINT totalVirtualFrames = static_cast<UINT>((g_sunFrames.size() - 1) * 2);

        if (totalVirtualFrames > 0) {
            // 2. Получаем сквозной шаг времени
            UINT step = static_cast<UINT>((currentTimeMs / frameDelayMs) % totalVirtualFrames);

            // 3. Математическое отражение: если шаг зашел дальше середины цикла, 
            // мы начинаем вычитать его из конца, пуская гифку реверсом назад!
            UINT currentFrameIdx = (step < g_sunFrames.size()) ? step : (totalVirtualFrames - step);

            // 🟢 Весь остальной ваш защищенный код отрисовки солнца остается прежним:
            if (currentFrameIdx < g_sunFrames.size()) {
                ID2D1Bitmap* pCurrentSunBitmap = g_sunFrames[currentFrameIdx];
                if (pCurrentSunBitmap) {
                    D2D1_RECT_F rectSun1 = D2D1::RectF(cached_sx1 - cached_sun_sw / 2.0f, cached_sy1 - cached_sun_sh / 2.0f, cached_sx1 + cached_sun_sw / 2.0f, cached_sy1 + cached_sun_sh / 2.0f);
                    D2D1_RECT_F rectSun2 = D2D1::RectF(cached_sx2 - cached_sun_sw / 2.0f, cached_sy2 - cached_sun_sh / 2.0f, cached_sx2 + cached_sun_sw / 2.0f, cached_sy2 + cached_sun_sh / 2.0f);

                    if (rectSun1.bottom > baselineY) rectSun1.bottom = baselineY;
                    if (rectSun2.bottom > baselineY) rectSun2.bottom = baselineY;

                    rt->DrawBitmap(pCurrentSunBitmap, rectSun1);
                    rt->DrawBitmap(pCurrentSunBitmap, rectSun2);
                }
            }
        }
    }
    
    //Отрисовка луны
    // ✅ ФИНАЛЬНЫЙ ТАЙМЕР ЛУНЫ С РАНДОМОМ И 100 % ПЛАВНЫМ 3D - ФЛИПОМ
        if (mutableState.moon && g_moonFlippedBitmap && cached_moon_mw > 0.0f) {

            // Длительность самого процесса переворота (в секундах)
            const float FLIP_DURATION_SEC = 1.2f;
            // Время удержания перевернутой луны на экране (в секундах)
            const float HOLD_DURATION_SEC = 10.0f;
            // Полное время всей анимации (флип туда + удержание + флип обратно)
            float totalAnimDuration = FLIP_DURATION_SEC * 2.0f + HOLD_DURATION_SEC;

            // Статические переменные для защиты от рассинхронизации Windows
            static bool isAnimating = false;
            static ULONGLONG animStartTimeMs = 0;
            static int lastCheckedMinute = -1;

            // Случайный интервал: инициализируем при первом старте (например, 5 минут)
            static int nextIntervalMinutes = 5;
            static int minutesPassedInState = 0;

            // Каждую новую минуту проверяем прогресс таймера
            if (st.wMinute != lastCheckedMinute) {
                if (lastCheckedMinute != -1) {
                    minutesPassedInState++;

                    // Если прошло случайное количество минут — взводим триггер анимации
                    if (minutesPassedInState >= nextIntervalMinutes && !isAnimating) {
                        isAnimating = true;
                        animStartTimeMs = GetTickCount64();
                        minutesPassedInState = 0;

                        // 🟢 РАНДОМИЗАТОР: Сразу выбираем случайный следующий интервал от 5 до 15 минут
                        // Используем системный GetTickCount64 в качестве сида для честного рандома
                        srand(static_cast<unsigned int>(GetTickCount64()));
                        nextIntervalMinutes = 5 + (rand() % 11); // Генерирует число от 5 до 15
                    }
                }
                lastCheckedMinute = st.wMinute;
            }

            bool isFlippedPeriod = false;
            float scaleX = 1.0f; // Коэффициент сжатия по ширине (1.0 = норма, 0.0 = ребро)

            // Логика плавной отрисовки флипа по чистому локальному счетчику миллисекунд
            if (isAnimating) {
                ULONGLONG elapsedMs = GetTickCount64() - animStartTimeMs;
                float currentSeconds = static_cast<float>(elapsedMs) / 1000.0f;

                if (currentSeconds < totalAnimDuration) {
                    // 1. ЭТАП: Переворот туда (Обычная луна схлопывается в 0, перевернутая разворачивается)
                    if (currentSeconds < FLIP_DURATION_SEC) {
                        float progress = currentSeconds / FLIP_DURATION_SEC;
                        scaleX = fabsf(cosf(progress * PI_F));
                        isFlippedPeriod = (progress > 0.5f);
                    }
                    // 2. ЭТАП: Удержание (Перевернутая луна стабильно висит на экране)
                    else if (currentSeconds >= FLIP_DURATION_SEC && currentSeconds < (FLIP_DURATION_SEC + HOLD_DURATION_SEC)) {
                        isFlippedPeriod = true;
                        scaleX = 1.0f;
                    }
                    // 3. ЭТАП: Обратный переворот (Перевернутая схлопывается в 0, обычная возвращается)
                    else if (currentSeconds >= (FLIP_DURATION_SEC + HOLD_DURATION_SEC) && currentSeconds < totalAnimDuration) {
                        float timeInBackFlip = currentSeconds - (FLIP_DURATION_SEC + HOLD_DURATION_SEC);
                        float progress = timeInBackFlip / FLIP_DURATION_SEC;
                        scaleX = fabsf(cosf(progress * PI_F));
                        isFlippedPeriod = (progress <= 0.5f);
                    }
                }
                else {
                    // Анимация полностью завершилась — сбрасываем флаги
                    isAnimating = false;
                    isFlippedPeriod = false;
                    scaleX = 1.0f;
                }
            }

            // Выбираем текстуру
            ID2D1Bitmap* pActiveMoon = isFlippedPeriod ? g_moonFlippedBitmap : mutableState.moon;

            // Плавное сжатие ширины
            float currentWidth = cached_moon_mw * scaleX;

            // Координаты из вашего бэкапа
            D2D1_RECT_F rectMoon1 = D2D1::RectF(
                cached_mx1 - currentWidth / 2.0f,
                cached_my1 - cached_moon_mh / 2.0f,
                cached_mx1 + currentWidth / 2.0f,
                cached_my1 + cached_moon_mh / 2.0f
            );

            D2D1_RECT_F rectMoon2 = D2D1::RectF(
                cached_mx2 - currentWidth / 2.0f,
                cached_my2 - cached_moon_mh / 2.0f,
                cached_mx2 + currentWidth / 2.0f,
                cached_my2 + cached_moon_mh / 2.0f
            );

            if (rectMoon1.bottom > baselineY) rectMoon1.bottom = baselineY;
            if (rectMoon2.bottom > baselineY) rectMoon2.bottom = baselineY;

            rt->DrawBitmap(pActiveMoon, rectMoon1);
            rt->DrawBitmap(pActiveMoon, rectMoon2);
        }

    if (isClipPushed) {
        rt->PopAxisAlignedClip();
    }
}
void ShutdownDayCycle(DayCycleState& state) {
    if (state.bgLeft) { state.bgLeft->Release(); state.bgLeft = nullptr; }
    if (state.bgRight) { state.bgRight->Release(); state.bgRight = nullptr; }
    for (auto frame : g_sunFrames) {
        if (frame) frame->Release();
    }
    g_sunFrames.clear();
    if (state.moon) { state.moon->Release(); state.moon = nullptr; }
    if (g_moonFlippedBitmap) {
        g_moonFlippedBitmap->Release();
        g_moonFlippedBitmap = nullptr;
    }
    state.initialized = false;
}