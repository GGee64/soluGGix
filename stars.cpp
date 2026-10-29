#include "stars.h"
#include <cmath>
#include <random>
#include <atomic>
#include <algorithm>

#undef min
#undef max

extern HWND hOverlayWnd;
extern int screenWidth;
extern int screenHeight;

namespace Stars {
    static float speed = 8.0f;
    static float angle = 25.0f;

    // Состояния желтой звезды
    static float starX = -100.0f;
    static float starY = -100.0f;
    static bool isStarVisible = false;

    // Состояния фиолетовой звезды
    static float star2X = -100.0f;
    static float star2Y = -100.0f;
    static bool isStar2Visible = false;
    static float star2Speed = 20.0f;
    static bool star2Active = false;

    // Таймеры
    static double delayCounter = 0;
    static bool isWaiting = true;
    static float currentDelay = 0.0f;

    static double star2DelayCounter = 0;
    static bool isStar2Waiting = true;
    static float star2CurrentDelay = 0.0f;

    namespace Random {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> delayDist(3000, 15000);
        static std::uniform_int_distribution<> star2ChanceDist(1, 10);
        static std::uniform_int_distribution<> edgeDist(0, 1); // 0 - старт слева, 1 - старт сверху

        inline int GetDelay() { return delayDist(gen); }
        inline bool ShouldSpawnSecondStar() { return star2ChanceDist(gen) <= 3; }
        inline int ChooseStartEdge() { return edgeDist(gen); }
    }

    void InitStarsModule() {
        currentDelay = static_cast<float>(Random::GetDelay());
        star2CurrentDelay = static_cast<float>(Random::GetDelay());

        starX = -100.0f; starY = -100.0f; isStarVisible = false;
        star2X = -100.0f; star2Y = -100.0f; isStar2Visible = false; star2Active = false;
        delayCounter = 0; isWaiting = true;
    }

    void UpdateStars(double targetDelta) {
        float timeFactor = static_cast<float>(targetDelta) / 16.666f;
        const float RAD = 3.14159265f / 180.0f;

        // Движение звезд идет вправо-вниз (угол 25 градусов от горизонтали)
    // В экранных координатах Windows: вправо (+X), вниз (+Y)
        float stepX = speed * cosf(angle * RAD) * timeFactor;
        float stepY = speed * sinf(angle * RAD) * timeFactor; // ПЛЮС, чтобы лететь СВЕРХУ ВНИЗ

        // 1. ДВИЖЕНИЕ ЖЕЛТОЙ ЗВЕЗДЫ
        if (!isWaiting) {
            starX += stepX;
            starY += stepY; // Летит вниз

            // Проверка выхода за правый или НИЖНИЙ край экрана
            if (starX > screenWidth + 30 || starY > screenHeight + 30) {
                isStarVisible = false;
                isWaiting = true;
                delayCounter = 0;
                currentDelay = static_cast<float>(Random::GetDelay());
            }
        }

        // 2. ДВИЖЕНИЕ ВТОРОЙ ЗВЕЗДЫ (ФИОЛЕТОВОЙ)
        if (star2Active && !isStar2Waiting) {
            float step2X = star2Speed * cosf(angle * RAD) * timeFactor;
            float step2Y = star2Speed * sinf(angle * RAD) * timeFactor;

            star2X += step2X;
            star2Y += step2Y; // Летит вниз

            if (star2X > screenWidth + 30 || star2Y > screenHeight + 30) {
                isStar2Visible = false;
                star2Active = false;
            }
        }
    }

    void UpdateStarTimers(double realDeltaTime) {
        // 1. Таймер желтой звезды
        if (isWaiting) {
            delayCounter += realDeltaTime;
            if (delayCounter >= currentDelay) {
                isWaiting = false;
                isStarVisible = true;

                if (Random::ChooseStartEdge() == 0) {
                    // ВАРИАНТ 1: Спавн на левой границе (X = -30)
                    // Y выбирается от середины экрана до самого низа
                    starX = -30.0f;
                    starY = static_cast<float>(rand() % (screenHeight / 2) + (screenHeight / 2 - 50));
                }
                else {
                    // ВАРИАНТ 2: Спавн на верхней границе (Y = -30)
                    // X выбирается в диапазоне от 0 до ширины экрана
                    starX = static_cast<float>(rand() % screenWidth);
                    starY = -30.0f;
                }

                if (!star2Active && Random::ShouldSpawnSecondStar()) {
                    star2Active = true;
                    isStar2Waiting = true;
                    star2DelayCounter = 0;
                    star2CurrentDelay = static_cast<float>((Random::GetDelay() % 3000) + 1000);
                }
            }
        }

        // 2. Таймер фиолетовой звезды
        if (star2Active && isStar2Waiting) {
            star2DelayCounter += realDeltaTime;
            if (star2DelayCounter >= star2CurrentDelay) {
                isStar2Waiting = false;
                isStar2Visible = true;

                if (Random::ChooseStartEdge() == 0) {
                    star2X = -30.0f;
                    star2Y = static_cast<float>(rand() % (screenHeight / 2) + (screenHeight / 2 - 50));
                }
                else {
                    star2X = static_cast<float>(rand() % screenWidth);
                    star2Y = -30.0f;
                }
            }
        }
    }

    static void DrawSingleGeometry(float x, float y, ID2D1HwndRenderTarget* rt, ID2D1Brush* brush, float sizeMultiplier) {
        const float outerRadius = 6.0f * sizeMultiplier;
        const float innerRadius = 1.0f * sizeMultiplier;

        ID2D1PathGeometry* pStarGeometry = nullptr;
        ID2D1Factory* pFactory = nullptr;
        rt->GetFactory(&pFactory);

        if (pFactory && SUCCEEDED(pFactory->CreatePathGeometry(&pStarGeometry))) {
            ID2D1GeometrySink* pSink = nullptr;
            if (SUCCEEDED(pStarGeometry->Open(&pSink))) {
                D2D1_POINT_2F points[8] = {
                    { x, y - outerRadius },
                    { x + outerRadius, y },
                    { x, y + outerRadius },
                    { x - outerRadius, y },
                    { x + innerRadius, y - innerRadius },
                    { x + innerRadius, y + innerRadius },
                    { x - innerRadius, y + innerRadius },
                    { x - innerRadius, y - innerRadius }
                };

                pSink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);
                pSink->AddLine(points[4]);
                pSink->AddLine(points[1]);
                pSink->AddLine(points[5]);
                pSink->AddLine(points[2]);
                pSink->AddLine(points[6]);
                pSink->AddLine(points[3]);
                pSink->AddLine(points[7]);
                pSink->AddLine(points[0]);
                pSink->EndFigure(D2D1_FIGURE_END_CLOSED);
                pSink->Close();
                pSink->Release();

                rt->FillGeometry(pStarGeometry, brush);
            }
            pStarGeometry->Release();
        }
    }

    void RenderStars(ID2D1HwndRenderTarget* pRenderTarget) {
        if (!pRenderTarget) return;

        ID2D1SolidColorBrush* pBrush = nullptr;
        if (isStarVisible && SUCCEEDED(pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.7f, 0.2f, 1.0f), &pBrush))) {
            DrawSingleGeometry(starX, starY, pRenderTarget, pBrush, 1.0f);
            pBrush->Release();
        }

        if (isStar2Visible && SUCCEEDED(pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.2f, 1.0f, 1.0f), &pBrush))) {
            DrawSingleGeometry(star2X, star2Y, pRenderTarget, pBrush, 0.7f);
            pBrush->Release();
        }
    }
}