#pragma once
#include <d2d1.h>

namespace Stars {
    void InitStarsModule();
    void UpdateStars(double targetDelta);
    void UpdateStarTimers(double realDeltaTime); // <-- ДОБАВИТЬ СЮДА
    void RenderStars(ID2D1HwndRenderTarget* pRenderTarget);
}