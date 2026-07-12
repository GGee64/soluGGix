#pragma once
#include <d2d1.h>

namespace Cal {
    void InitCalendarModule();
    void RenderCalendar(ID2D1HwndRenderTarget* rt, float screenW, float screenH);
    void ShutdownCalendarModule();
}