#pragma once
#include <d2d1.h>
#include <vector>

struct DayCycleState {
    bool initialized = false;
    ID2D1Bitmap* bgLeft = nullptr;
    ID2D1Bitmap* bgRight = nullptr;
    ID2D1Bitmap* moon = nullptr;

    // ✅ ПЕРЕНЕСЛИ КАДРЫ GIF ВНУТРЬ СТРУКТУРЫ ДЛЯ ПОТОКОБЕЗОПАСНОСТИ
    std::vector<ID2D1Bitmap*> sunFrames;
};

HRESULT InitDayCycle(ID2D1HwndRenderTarget* rt, DayCycleState* state);
void RenderDayCycle(ID2D1HwndRenderTarget* rt, float screenW, float screenH, const DayCycleState& state);
HRESULT LoadTextureFromResource(ID2D1HwndRenderTarget* rt, int resId, ID2D1Bitmap** ppBitmap, std::vector<ID2D1Bitmap*>* pGifFrames = nullptr);
void ShutdownDayCycle(DayCycleState& state);