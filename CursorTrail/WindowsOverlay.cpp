#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "WindowsOverlay.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <cstdlib>  // For malloc/free
#include <vector>   // For std::vector

using namespace Gdiplus;

namespace
{
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

void ConfigureDpiAwareness()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }

    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto setProcessDpiAwarenessContext =
        reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext")
        );

    if (setProcessDpiAwarenessContext &&
        setProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return;
    }

    using SetProcessDPIAwareFn = BOOL(WINAPI*)();
    auto setProcessDPIAware =
        reinterpret_cast<SetProcessDPIAwareFn>(
            GetProcAddress(user32, "SetProcessDPIAware")
        );

    if (setProcessDPIAware) {
        setProcessDPIAware();
    }
}
}

WindowsOverlay::WindowsOverlay()
    : m_hwnd(nullptr)
    , m_hdc(nullptr)
    , m_memDC(nullptr)
    , m_hBitmap(nullptr)
    , m_hOldBitmap(nullptr)
    , m_screenX(0)
    , m_screenY(0)
    , m_screenWidth(0)
    , m_screenHeight(0)
    , m_currentIndex(0)
    , m_gdiplusToken(0)
{
    m_trailParts.reserve(g_config.maxParticles);
    for (int i = 0; i < g_config.maxParticles; ++i) {
        m_trailParts.emplace_back(0.0f, 0.0f, 0.0f);
    }
}

WindowsOverlay::~WindowsOverlay()
{
    Cleanup();
}

bool WindowsOverlay::Initialize()
{
    ConfigureDpiAwareness();

    // Initialize GDI+
    GdiplusStartupInput gdiplusStartupInput;
    if (GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, nullptr) != Ok) {
        std::cerr << "Failed to initialize GDI+" << std::endl;
        return false;
    }

    // Get the full virtual desktop bounds so every monitor is covered,
    // including displays positioned left or above the primary monitor.
    RefreshVirtualScreenBounds();

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"CursorTrailOverlay";
    
    if (!RegisterClassExW(&wc)) {
        std::cerr << "Failed to register window class" << std::endl;
        GdiplusShutdown(m_gdiplusToken);
        return false;
    }

    // Create layered window for transparent overlay
    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        L"CursorTrailOverlay",
        L"Cursor Trail",
        WS_POPUP,
        m_screenX, m_screenY, m_screenWidth, m_screenHeight,
        nullptr, nullptr, GetModuleHandle(nullptr), this
    );

    if (!m_hwnd) {
        std::cerr << "Failed to create overlay window" << std::endl;
        GdiplusShutdown(m_gdiplusToken);
        return false;
    }

    // Set window to be initially transparent - remove conflicting SetLayeredWindowAttributes call
    // We'll use UpdateLayeredWindow for all transparency control

    // Create memory DC for double buffering
    if (!CreateBackBuffer()) {
        std::cerr << "Failed to create overlay back buffer" << std::endl;
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
        GdiplusShutdown(m_gdiplusToken);
        return false;
    }

    // Load trail texture from configuration
    // Try multiple paths to find the texture file
    std::vector<std::wstring> texturePaths;
    
    // Convert config texture path to wide string and add to paths
    std::wstring configTexture;
    configTexture.assign(g_config.texturePath.begin(), g_config.texturePath.end());
    texturePaths.push_back(configTexture);
    
    // Also try relative paths for the configured texture
    std::wstring baseName = configTexture;
    size_t lastSlash = baseName.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        baseName = baseName.substr(lastSlash + 1);
    }
    
    texturePaths.push_back(baseName);
    texturePaths.push_back(L"CursorTrail\\" + baseName);
    texturePaths.push_back(L"..\\CursorTrail\\" + baseName);
    texturePaths.push_back(L"..\\..\\CursorTrail\\" + baseName);
    
    m_trailTexture = nullptr;
    for (const auto& path : texturePaths) {
        std::wcout << L"Trying to load texture from: " << path << std::endl;
        m_trailTexture = std::unique_ptr<Bitmap>(Bitmap::FromFile(path.c_str()));
        if (m_trailTexture && m_trailTexture->GetLastStatus() == Ok) {
            std::wcout << L"Successfully loaded texture from: " << path << std::endl;
            break;
        }
    }
    
    if (!m_trailTexture || m_trailTexture->GetLastStatus() != Ok) {
        std::cout << "Failed to load " << g_config.texturePath << " from all paths, creating fallback texture" << std::endl;
        // Fallback: Create texture same size as original (8x8) for consistency
        const int textureSize = 8; // Match original cursortrail.png dimensions
        m_trailTexture = std::make_unique<Bitmap>(textureSize, textureSize, PixelFormat32bppARGB);
        Graphics textureGraphics(m_trailTexture.get());
        textureGraphics.SetSmoothingMode(SmoothingModeAntiAlias);
        
        // Create a simple white circle matching the original texture design
        SolidBrush whiteBrush(Color(255, 255, 255, 255)); // Fully opaque white
        textureGraphics.FillEllipse(&whiteBrush, 0, 0, textureSize, textureSize);
        
        std::cout << "Created fallback white circle texture (" << textureSize << "x" << textureSize << ")" << std::endl;
    } else {
        std::cout << "Successfully loaded " << g_config.texturePath << " texture (" << m_trailTexture->GetWidth() << "x" << m_trailTexture->GetHeight() << ")" << std::endl;
    }

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    EnsureTopMost();
    
    std::cout << "Windows overlay window created and shown successfully!" << std::endl;
    std::cout << "Virtual screen bounds: " << m_screenX << "," << m_screenY
              << " " << m_screenWidth << "x" << m_screenHeight << std::endl;

    return true;
}

bool WindowsOverlay::RefreshVirtualScreenBounds()
{
    m_screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    m_screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    m_screenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_screenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (m_screenWidth <= 0 || m_screenHeight <= 0) {
        m_screenX = 0;
        m_screenY = 0;
        m_screenWidth = GetSystemMetrics(SM_CXSCREEN);
        m_screenHeight = GetSystemMetrics(SM_CYSCREEN);
    }

    return m_screenWidth > 0 && m_screenHeight > 0;
}

bool WindowsOverlay::CreateBackBuffer()
{
    if (!m_hwnd || m_screenWidth <= 0 || m_screenHeight <= 0) {
        return false;
    }

    ReleaseBackBuffer();

    m_hdc = GetDC(m_hwnd);
    if (!m_hdc) {
        return false;
    }

    m_memDC = CreateCompatibleDC(m_hdc);
    if (!m_memDC) {
        ReleaseDC(m_hwnd, m_hdc);
        m_hdc = nullptr;
        return false;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = m_screenWidth;
    bmi.bmiHeader.biHeight = -m_screenHeight; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    m_hBitmap = CreateDIBSection(m_memDC, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    if (!m_hBitmap) {
        ReleaseBackBuffer();
        return false;
    }

    m_hOldBitmap = (HBITMAP)SelectObject(m_memDC, m_hBitmap);
    if (!m_hOldBitmap) {
        ReleaseBackBuffer();
        return false;
    }

    return true;
}

void WindowsOverlay::ReleaseBackBuffer()
{
    if (m_hOldBitmap && m_memDC) {
        SelectObject(m_memDC, m_hOldBitmap);
        m_hOldBitmap = nullptr;
    }

    if (m_hBitmap) {
        DeleteObject(m_hBitmap);
        m_hBitmap = nullptr;
    }

    if (m_memDC) {
        DeleteDC(m_memDC);
        m_memDC = nullptr;
    }

    if (m_hdc && m_hwnd) {
        ReleaseDC(m_hwnd, m_hdc);
        m_hdc = nullptr;
    }
}

void WindowsOverlay::EnsureTopMost()
{
    if (!m_hwnd) {
        return;
    }

    SetWindowPos(
        m_hwnd,
        HWND_TOPMOST,
        m_screenX,
        m_screenY,
        m_screenWidth,
        m_screenHeight,
        SWP_NOACTIVATE | SWP_SHOWWINDOW
    );
}

void WindowsOverlay::Update(double frameScale)
{
    if (!m_hwnd) return;
    frameScale = (std::max)(0.0, (std::min)(4.0, frameScale));

    // Get global cursor position
    POINT cursorPos;
    if (GetCursorPos(&cursorPos)) {
        TrailPart currentTrail(static_cast<float>(cursorPos.x), static_cast<float>(cursorPos.y), g_config.fadeTime);
        
        // Calculate previous index BEFORE adding current trail
        size_t prevIndex = (m_currentIndex == 0) ? m_trailParts.size() - 1 : m_currentIndex - 1;
        
        // Add the current cursor position to trail (match OpenGL version exactly)
        AddTrailPart(currentTrail);
        
        // Interpolate trail between current and previous position ONLY (like OpenGL Game.cpp)
        const TrailPart& previousTrail = m_trailParts[prevIndex];
        
        float dx = currentTrail.x - previousTrail.x;
        float dy = currentTrail.y - previousTrail.y;
        float distance = std::sqrt(dx * dx + dy * dy);
        
        // Avoid division by zero and match OpenGL logic exactly
        if (distance > 0.0f) {
            float dirX = dx / distance;
            float dirY = dy / distance;
            
            // Use configurable interpolation interval
            float interval = g_config.spawnFrequency;
            float stopAt = distance;
            
            for (float d = interval; d < stopAt; d += interval) {
                float interpX = previousTrail.x + dirX * d;
                float interpY = previousTrail.y + dirY * d;
                AddTrailPart(TrailPart(interpX, interpY, g_config.fadeTime));
            }
        }
        
    }

    // Update trail fade times - use configurable fade rate
    for (auto& part : m_trailParts) {
        if (part.time > 0.0f) {
            part.time -= static_cast<float>(g_config.fadeRate * frameScale);
            if (part.time < 0.0f) {
                part.time = 0.0f;
            }
        }
    }
}

void WindowsOverlay::AddTrailPart(const TrailPart& part)
{
    m_trailParts[m_currentIndex] = part;
    m_currentIndex = (m_currentIndex + 1) % g_config.maxParticles;
}

void WindowsOverlay::Render()
{
    if (!m_hwnd || !m_memDC) return;

    int oldX = m_screenX;
    int oldY = m_screenY;
    int oldWidth = m_screenWidth;
    int oldHeight = m_screenHeight;

    if (RefreshVirtualScreenBounds() &&
        (oldX != m_screenX || oldY != m_screenY ||
         oldWidth != m_screenWidth || oldHeight != m_screenHeight)) {
        EnsureTopMost();

        if (!CreateBackBuffer()) {
            std::cerr << "Failed to recreate overlay back buffer after display change" << std::endl;
            return;
        }
    } else {
        EnsureTopMost();
    }

    // Clear the memory DC with fully transparent pixels using Graphics
    Graphics clearGraphics(m_memDC);
    clearGraphics.Clear(Color(0, 0, 0, 0)); // Fully transparent

    // Draw trail using GDI+
    Graphics graphics(m_memDC);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetCompositingMode(CompositingModeSourceOver);
    graphics.SetCompositingQuality(CompositingQualityHighQuality);
    
    DrawTrail(graphics);

    // Update the layered window
    POINT ptDst = { m_screenX, m_screenY };
    POINT ptSrc = { 0, 0 };
    SIZE sizeWnd = { m_screenWidth, m_screenHeight };
    BLENDFUNCTION bf = {};
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(m_hwnd, nullptr, &ptDst, &sizeWnd, m_memDC, &ptSrc, RGB(0, 0, 0), &bf, ULW_ALPHA);
}

void WindowsOverlay::DrawTrail(Graphics& graphics)
{
    for (const auto& part : m_trailParts) {
        if (part.time > 0.0f) {
            // Calculate alpha to match OpenGL version exactly (use time directly as alpha)
            float alpha = (std::max)(0.0f, (std::min)(1.0f, part.time));
            
            // Use configurable sprite size
            float spriteSize = g_config.spriteSize;
            float textureWidth = static_cast<float>(m_trailTexture->GetWidth());
            float textureHeight = static_cast<float>(m_trailTexture->GetHeight());
            
            // Create a color matrix for alpha blending
            ColorMatrix colorMatrix = {
                {
                    {1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
                    {0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
                    {0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
                    {0.0f, 0.0f, 0.0f, alpha, 0.0f},
                    {0.0f, 0.0f, 0.0f, 0.0f, 1.0f}
                }
            };
            
            ImageAttributes imageAttributes;
            imageAttributes.SetColorMatrix(&colorMatrix);
            
            // Draw the trail sprite at fixed size (same as OpenGL version)
            // Position sprite centered on the trail point
            RectF destRect(
                part.x - static_cast<float>(m_screenX) - spriteSize / 2.0f,
                part.y - static_cast<float>(m_screenY) - spriteSize / 2.0f,
                spriteSize,
                spriteSize
            );
            
            graphics.DrawImage(
                m_trailTexture.get(),
                destRect,
                0, 0, textureWidth, textureHeight,
                UnitPixel,
                &imageAttributes
            );
        }
    }
}

void WindowsOverlay::Cleanup()
{
    ReleaseBackBuffer();
    
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    
    if (m_gdiplusToken) {
        GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
    }
}

LRESULT CALLBACK WindowsOverlay::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
        case WM_CREATE:
            {
                CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
                WindowsOverlay* pOverlay = reinterpret_cast<WindowsOverlay*>(pCreate->lpCreateParams);
                SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pOverlay));
            }
            return 0;
            
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
            
        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

#endif // _WIN32
