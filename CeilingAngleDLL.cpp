#define IMGUI_IMPL_WIN32_DISABLE_GAMEPAD

#include <windows.h>
#include "avatar.h"
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <random>
#include <cmath>
#include <cstdarg>
#include <string>
#include <vector>
#include <d3d9.h>
#include <d3dx9.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx9.h"

#include "MinHook.h"

constexpr uintptr_t VIEWY_OFFSET = 0x04268EC;
constexpr uintptr_t VIEWX_OFFSET = 0x4AAC;

static bool autoCeilingEnabled = true;
static int ceilingSmoothing = 1;

static float targetPitch = 0.0f;
static std::mt19937 rng{ std::random_device{}() };

static bool autoClickerEnabled = true;
static int autoClickerCPS = 10;
static int customKey = 'B';
static int menuKey = 'K';
static auto lastClickTime = std::chrono::steady_clock::now();

static std::atomic_bool autoClickOnCeiling{ false };
static float ceilingThreshold = 1.0f;
static int ceilingHoldMs = 150;

static bool showMenu = true;
static bool bindingHotkey = false;
static bool bindingMenuKey = false;

static std::chrono::steady_clock::time_point bindingStartHotkey = std::chrono::steady_clock::time_point();
static std::chrono::steady_clock::time_point bindingStartMenuKey = std::chrono::steady_clock::time_point();
static constexpr int BIND_DEBOUNCE_MS = 200;

static bool prevShowMenu = false;

static bool debugEnabled = false;
static bool cursorReleased = false;
static std::chrono::steady_clock::time_point lastDebugPrint = std::chrono::steady_clock::now();

static uintptr_t g_engineBase = 0;

LPDIRECT3DDEVICE9 g_pd3dDevice = nullptr;
HWND g_hwnd = nullptr;
WNDPROC oWndProc = nullptr;

LPDIRECT3DTEXTURE9 g_pAvatarTexture = nullptr;

typedef BOOL(WINAPI* tSetCursorPos)(int X, int Y);
static tSetCursorPos oSetCursorPos = nullptr;
static void* g_pSetCursorPosAddr = nullptr;

typedef BOOL(WINAPI* tGetCursorPos)(LPPOINT lpPoint);
static tGetCursorPos oGetCursorPos = nullptr;
static void* g_pGetCursorPosAddr = nullptr;

typedef UINT(WINAPI* tGetRawInputData)(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);
static tGetRawInputData oGetRawInputData = nullptr;
static void* g_pGetRawInputDataAddr = nullptr;

typedef UINT(WINAPI* tGetRawInputBuffer)(PRAWINPUT pData, PUINT pcbSize, UINT cbSizeHeader);
static tGetRawInputBuffer oGetRawInputBuffer = nullptr;
static void* g_pGetRawInputBufferAddr = nullptr;

typedef UINT(WINAPI* tSendInput)(UINT nInputs, LPINPUT pInputs, int cbSize);
static tSendInput oSendInput = nullptr;
static void* g_pSendInputAddr = nullptr;

typedef void (WINAPI* tMouseEvent)(DWORD dwFlags, DWORD dx, DWORD dy, DWORD dwData, ULONG_PTR dwExtraInfo);
static tMouseEvent oMouseEvent = nullptr;
static void* g_pMouseEventAddr = nullptr;

void DebugLog(const char* fmt, ...) {
    if (!debugEnabled) return;
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDebugPrint).count();
    if (ms < 200) return;
    lastDebugPrint = now;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

template<typename T>
T ReadMem(uintptr_t addr) {
    T ret{};
    if (!addr) return ret;
    __try { ret = *reinterpret_cast<T*>(addr); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ret;
}

template<typename T>
void WriteMem(uintptr_t addr, T value) {
    if (!addr) return;
    __try { *reinterpret_cast<T*>(addr) = value; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static std::string KeyToString(int vk) {
    if (vk >= 'A' && vk <= 'Z') { char s[2] = { static_cast<char>(vk), 0 }; return std::string(s); }
    if (vk >= '0' && vk <= '9') { char s[2] = { static_cast<char>(vk), 0 }; return std::string(s); }
    switch (vk) {
    case VK_SPACE: return "SPACE";
    case VK_TAB: return "TAB";
    case VK_SHIFT: return "SHIFT";
    case VK_CONTROL: return "CTRL";
    case VK_MENU: return "ALT";
    case VK_LBUTTON: return "MOUSE1";
    case VK_RBUTTON: return "MOUSE2";
    case VK_MBUTTON: return "MOUSE3";
    case VK_ESCAPE: return "ESC";
    default: { char buf[16]; sprintf_s(buf, "VK_%02X", vk); return std::string(buf); }
    }
}

bool IsLocalPlayerHunter() { return true; }

void RunAutoCeiling(uintptr_t engineBase) {
    if (!autoCeilingEnabled) { autoClickOnCeiling.store(false); return; }
    static std::chrono::steady_clock::time_point touchStart = std::chrono::steady_clock::time_point();
    try {
        bool keyPressed = (GetAsyncKeyState(customKey) & 0x8000) != 0;
        bool isHunter = IsLocalPlayerHunter();
        bool allowActions = isHunter;
        if (keyPressed && allowActions) {
            uintptr_t angleBasePtr = ReadMem<uintptr_t>(engineBase + VIEWY_OFFSET);
            if (angleBasePtr == 0) { autoClickOnCeiling.store(false); DebugLog("angleBasePtr == 0"); return; }
            float currentPitch = ReadMem<float>(angleBasePtr + VIEWX_OFFSET);
            if (targetPitch == 0.0f) {
                std::uniform_real_distribution<float> dist(11.0f, 13.0f);
                targetPitch = dist(rng);
                DebugLog("Nuevo targetPitch: %.3f", targetPitch);
            }
            float newPitch;
            if (ceilingSmoothing <= 1) newPitch = targetPitch;
            else {
                float t = static_cast<float>(ceilingSmoothing) / 60.0f;
                newPitch = currentPitch + (targetPitch - currentPitch) * (1.0f - t);
            }
            WriteMem<float>(angleBasePtr + VIEWX_OFFSET, newPitch);
            DebugLog("anglePtr=0x%p current=%.3f new=%.3f target=%.3f", (void*)angleBasePtr, currentPitch, newPitch, targetPitch);
            if (std::fabs(newPitch - targetPitch) <= ceilingThreshold) {
                if (touchStart == std::chrono::steady_clock::time_point()) {
                    touchStart = std::chrono::steady_clock::now(); DebugLog("touchStart establecido");
                }
                else {
                    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - touchStart).count();
                    if (dur >= ceilingHoldMs) {
                        if (!autoClickOnCeiling.load()) { autoClickOnCeiling.store(true); DebugLog("autoClickOnCeiling = true (duracion %.0f ms)", (double)dur); }
                    }
                }
            }
            else { touchStart = std::chrono::steady_clock::time_point(); autoClickOnCeiling.store(false); }
        }
        else { targetPitch = 0.0f; autoClickOnCeiling.store(false); }
    }
    catch (...) { autoClickOnCeiling.store(false); }
}

void SendLeftClick() {
    INPUT input[2] = {};
    input[0].type = INPUT_MOUSE; input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].type = INPUT_MOUSE; input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, input, sizeof(INPUT));
}

void RunAutoClicker() {
    if (autoClickerCPS <= 0) return;
    if (autoClickerCPS > 20) autoClickerCPS = 20;

    if (!autoClickerEnabled && !autoClickOnCeiling.load()) return;

    try {
        bool keyPressed = (GetAsyncKeyState(customKey) & 0x8000) != 0;
        bool isHunter = IsLocalPlayerHunter();
        bool allowActions = isHunter;

        bool shouldClick = (keyPressed && allowActions && autoClickerEnabled) ||
            (autoClickOnCeiling.load() && autoClickerEnabled);

        if (shouldClick && autoClickerCPS > 0) {
            auto now = std::chrono::steady_clock::now();
            double interval = 1000.0 / static_cast<double>(autoClickerCPS);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastClickTime).count();
            if (elapsed >= interval) {
                SendLeftClick();
                lastClickTime = now;
                DebugLog("Clicked (autoClickOnCeiling=%d elapsed=%lld interval=%.2f)",
                    (int)autoClickOnCeiling.load(), (long long)elapsed, interval);
            }
        }
    }
    catch (...) {}
}

BOOL WINAPI hkSetCursorPos(int X, int Y) {
    if (showMenu) {
        __try {
            RECT rect;
            if (GetClientRect(g_hwnd, &rect)) {
                POINT center = { rect.right / 2, rect.bottom / 2 };
                ClientToScreen(g_hwnd, &center);

                if (abs(X - center.x) < 10 && abs(Y - center.y) < 10) {
                    DebugLog("Bloqueado SetCursorPos al centro: (%d, %d)", X, Y);
                    return TRUE;
                }
            }

            ImGuiIO& io = ImGui::GetIO();
            if (io.WantCaptureMouse) {
                if (oSetCursorPos) return oSetCursorPos(X, Y);
                return SetCursorPos(X, Y);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        return TRUE;
    }

    if (oSetCursorPos) return oSetCursorPos(X, Y);
    return SetCursorPos(X, Y);
}

BOOL WINAPI hkGetCursorPos(LPPOINT lpPoint) {
    if (!lpPoint) return FALSE;
    if (showMenu) {
        __try {
            ImGuiIO& io = ImGui::GetIO();
            if (io.WantCaptureMouse) {
                POINT p; BOOL ok = oGetCursorPos ? oGetCursorPos(&p) : GetCursorPos(&p);
                if (ok) { lpPoint->x = p.x; lpPoint->y = p.y; return TRUE; }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (oGetCursorPos) return oGetCursorPos(lpPoint);
    return GetCursorPos(lpPoint);
}

UINT WINAPI hkGetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader) {
    UINT ret = 0;
    if (oGetRawInputData) ret = oGetRawInputData(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);

    if (showMenu && uiCommand == RID_INPUT && pData != nullptr && pcbSize != nullptr && *pcbSize >= sizeof(RAWINPUT)) {
        __try {
            RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(pData);
            if (ri->header.dwType == RIM_TYPEMOUSE) {
                // Zerear movimiento y botones del mouse
                ri->data.mouse.lLastX = 0;
                ri->data.mouse.lLastY = 0;
                ri->data.mouse.ulRawButtons = 0;
                ri->data.mouse.usButtonFlags = 0;
                ri->data.mouse.usButtonData = 0;
                DebugLog("Bloqueado RawInput MOUSE");
            }
            else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
                // Bloquear tambien teclado excepto la tecla del menu
                if (ri->data.keyboard.VKey != menuKey && ri->data.keyboard.VKey != VK_END) {
                    ri->data.keyboard.VKey = 0;
                    ri->data.keyboard.MakeCode = 0;
                    ri->data.keyboard.Flags = 0;
                    DebugLog("Bloqueado RawInput KEYBOARD");
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return ret;
}

UINT WINAPI hkGetRawInputBuffer(PRAWINPUT pData, PUINT pcbSize, UINT cbSizeHeader) {
    if (showMenu && pData != nullptr && pcbSize != nullptr && *pcbSize > 0) {
        __try {
            UINT ret = 0;
            if (oGetRawInputBuffer) ret = oGetRawInputBuffer(pData, pcbSize, cbSizeHeader);
            UINT count = *pcbSize / sizeof(RAWINPUT);
            for (UINT i = 0; i < count; ++i) {
                RAWINPUT* ri = &pData[i];
                if (ri->header.dwType == RIM_TYPEMOUSE) {
                    ri->data.mouse.lLastX = 0;
                    ri->data.mouse.lLastY = 0;
                    ri->data.mouse.ulRawButtons = 0;
                    ri->data.mouse.usButtonFlags = 0;
                    ri->data.mouse.usButtonData = 0;
                }
                else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
                    if (ri->data.keyboard.VKey != menuKey && ri->data.keyboard.VKey != VK_END) {
                        ri->data.keyboard.VKey = 0;
                        ri->data.keyboard.MakeCode = 0;
                        ri->data.keyboard.Flags = 0;
                    }
                }
            }
            DebugLog("Bloqueado RawInputBuffer (%d entries)", count);
            return ret;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (oGetRawInputBuffer) return oGetRawInputBuffer(pData, pcbSize, cbSizeHeader);
    return 0;
}

UINT WINAPI hkSendInput(UINT nInputs, LPINPUT pInputs, int cbSize) {
    if (showMenu && pInputs != nullptr) {
        try {
            std::vector<INPUT> copy;
            copy.reserve(nInputs);

            for (UINT i = 0; i < nInputs; ++i) {
                INPUT in = pInputs[i];
                if (in.type == INPUT_MOUSE && (in.mi.dwFlags & MOUSEEVENTF_MOVE)) {
                    in.mi.dx = 0;
                    in.mi.dy = 0;
                    in.mi.dwFlags &= ~MOUSEEVENTF_MOVE;
                }
                copy.push_back(in);
            }

            return oSendInput ? oSendInput(nInputs, copy.data(), cbSize) : 0;
        }
        catch (...) {
            return 0;
        }
    }

    if (oSendInput)
        return oSendInput(nInputs, pInputs, cbSize);

    return 0;
}

void WINAPI hkMouseEvent(DWORD dwFlags, DWORD dx, DWORD dy, DWORD dwData, ULONG_PTR dwExtraInfo) {
    if (showMenu) {
        __try {
            ImGuiIO& io = ImGui::GetIO();
            if (io.WantCaptureMouse && (dwFlags & MOUSEEVENTF_MOVE)) {
                DebugLog("Bloqueado mouse_event MOVE");
                return;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (oMouseEvent) { oMouseEvent(dwFlags, dx, dy, dwData, dwExtraInfo); }
    else { mouse_event(dwFlags, dx, dy, dwData, dwExtraInfo); }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    bool windowFocused = (GetForegroundWindow() == hWnd);

    if (showMenu && windowFocused && msg == WM_INPUT) {
        DebugLog("Bloqueado WM_INPUT (ventana activa)");
        return 0;
    }

    if (showMenu && windowFocused) {
        ImGuiIO& io = ImGui::GetIO();

        if (io.WantCaptureMouse) {
            if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK ||
                msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_RBUTTONDBLCLK ||
                msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP || msg == WM_MBUTTONDBLCLK ||
                msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL || msg == WM_MOUSEMOVE) {
                DebugLog("Bloqueado mensaje de mouse: 0x%X", msg);
                return 0;
            }
        }

        if (io.WantCaptureKeyboard) {
            if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_CHAR) {
                if (wParam != menuKey && wParam != VK_END) {
                    DebugLog("Bloqueado mensaje de teclado: 0x%X", msg);
                    return 0;
                }
            }
        }
    }

    return CallWindowProc(oWndProc, hWnd, msg, wParam, lParam);
}

void HandleKeyBindingStates() {
    static int lastBoundKey = 0;
    static bool waitingForRelease = false;
    static bool delayActive = false;
    static auto bindingDelayStart = std::chrono::steady_clock::now();

    if (bindingHotkey) {
        if (!delayActive) {
            bindingDelayStart = std::chrono::steady_clock::now();
            delayActive = true;
            waitingForRelease = false;
            lastBoundKey = 0;
            DebugLog("Esperando 50ms antes de permitir binding (hotkey)...");
            return;
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - bindingDelayStart).count() < 50)
            return;

        for (int vk = 0x08; vk <= 0xFE; vk++) {
            if (vk == VK_ESCAPE || vk == menuKey) continue;

            SHORT state = GetAsyncKeyState(vk);
            bool pressed = (state & 0x8000) != 0;

            if (pressed && !waitingForRelease) {
                customKey = vk;
                bindingHotkey = false;
                delayActive = false;
                waitingForRelease = true;
                lastBoundKey = vk;
                DebugLog("Nueva activation key: %s", KeyToString(customKey).c_str());
                return;
            }
        }

        if (waitingForRelease && !(GetAsyncKeyState(lastBoundKey) & 0x8000)) {
            waitingForRelease = false;
            lastBoundKey = 0;
        }
    }

    if (bindingMenuKey) {
        if (!delayActive) {
            bindingDelayStart = std::chrono::steady_clock::now();
            delayActive = true;
            waitingForRelease = false;
            lastBoundKey = 0;
            DebugLog("Esperando 50ms antes de permitir binding (menu)...");
            return;
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - bindingDelayStart).count() < 50)
            return;

        for (int vk = 0x08; vk <= 0xFE; vk++) {
            if (vk == VK_ESCAPE || vk == customKey) continue;

            SHORT state = GetAsyncKeyState(vk);
            bool pressed = (state & 0x8000) != 0;

            if (pressed && !waitingForRelease) {
                menuKey = vk;
                bindingMenuKey = false;
                delayActive = false;
                waitingForRelease = true;
                lastBoundKey = vk;
                DebugLog("Nueva menu key: %s", KeyToString(menuKey).c_str());
                return;
            }
        }

        if (waitingForRelease && !(GetAsyncKeyState(lastBoundKey) & 0x8000)) {
            waitingForRelease = false;
            lastBoundKey = 0;
        }
    }
}

void ApplyBlueStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.WindowPadding = ImVec2(12, 12);
    style.FramePadding = ImVec2(8, 6);
    style.ItemSpacing = ImVec2(8, 6);
    style.ScrollbarRounding = 8.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;

    ImVec4* colors = style.Colors;

    colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.08f, 0.18f, 0.95f); 
    colors[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.12f, 0.25f, 0.95f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.18f, 0.35f, 1.00f);

    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.45f, 0.80f, 0.90f);        
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.60f, 0.95f, 0.95f); 
    colors[ImGuiCol_HeaderActive] = ImVec4(0.35f, 0.65f, 1.00f, 1.00f);  

    colors[ImGuiCol_Button] = ImVec4(0.15f, 0.35f, 0.80f, 0.95f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.50f, 0.95f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.10f, 0.30f, 0.70f, 1.00f);

    colors[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.15f, 0.30f, 0.85f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.25f, 0.50f, 0.95f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.60f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.50f, 0.75f, 1.00f, 1.00f);

    colors[ImGuiCol_Text] = ImVec4(0.90f, 0.95f, 1.00f, 1.00f);  
}

bool LoadTextureFromMemory(const unsigned char* image_data, size_t image_size, LPDIRECT3DTEXTURE9* out_texture, int* out_width, int* out_height)
{
    if (!image_data || !out_texture || !out_width || !out_height)
        return false;

    LPDIRECT3DTEXTURE9 texture = nullptr;
    D3DXIMAGE_INFO info;

    HRESULT hr = D3DXCreateTextureFromFileInMemoryEx(
        g_pd3dDevice,
        image_data,
        static_cast<UINT>(image_size),
        D3DX_DEFAULT,
        D3DX_DEFAULT,
        D3DX_DEFAULT,
        0,
        D3DFMT_UNKNOWN,
        D3DPOOL_MANAGED,
        D3DX_DEFAULT,
        D3DX_DEFAULT,
        0,
        &info,
        nullptr,
        &texture
    );

    if (FAILED(hr))
        return false;

    *out_texture = texture;
    *out_width = info.Width;
    *out_height = info.Height;
    return true;
}

bool LoadTextureFromFile(const char* filename, LPDIRECT3DTEXTURE9* out_texture, int* out_width, int* out_height)
{
    HRESULT hr = D3DXCreateTextureFromFileExA(
        g_pd3dDevice,
        filename,
        D3DX_DEFAULT_NONPOW2,
        D3DX_DEFAULT_NONPOW2,
        D3DX_DEFAULT,
        0,
        D3DFMT_UNKNOWN,
        D3DPOOL_MANAGED,
        D3DX_DEFAULT,
        D3DX_DEFAULT,
        0,
        nullptr,
        nullptr,
        out_texture
    );

    if (FAILED(hr)) {
        DebugLog("Failed to load texture: %s", filename);
        return false;
    }

    if (*out_texture)
    {
        D3DSURFACE_DESC desc;
        (*out_texture)->GetLevelDesc(0, &desc);
        if (out_width) *out_width = desc.Width;
        if (out_height) *out_height = desc.Height;
        return true;
    }

    return false;
}

static ImFont* s_fontTitle = nullptr;
static ImFont* s_fontCredits = nullptr;

void InitImGui(HWND hwnd, LPDIRECT3DDEVICE9 device)
{
    g_hwnd = hwnd;
    g_pd3dDevice = device;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    const char* fontPath = "C:\\Windows\\Fonts\\segoeui.ttf";
    FILE* f = nullptr;
    if (fopen_s(&f, fontPath, "rb") == 0) {
        fclose(f);
        io.Fonts->AddFontFromFileTTF(fontPath, 15.0f);
        s_fontTitle = io.Fonts->AddFontFromFileTTF(fontPath, 48.0f);
        s_fontCredits = io.Fonts->AddFontFromFileTTF(fontPath, 28.0f);
    }
    else {
        io.Fonts->AddFontDefault();
        s_fontTitle = nullptr;
        s_fontCredits = nullptr;
    }

    ApplyBlueStyle();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX9_Init(g_pd3dDevice);

    oWndProc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
    DebugLog("WndProc hooked: oWndProc=0x%p", oWndProc);

    int avatar_width = 0, avatar_height = 0;
    if (!LoadTextureFromMemory(avatarhd_png, avatarhd_png_len, &g_pAvatarTexture, &avatar_width, &avatar_height)) {
        DebugLog("No se pudo cargar la imagen del avatar desde memoria");
    }
    else {
        DebugLog("Avatar cargado desde memoria: %dx%d", avatar_width, avatar_height);
    }
}

void CleanupImGui() {
    if (g_pAvatarTexture) {
        g_pAvatarTexture->Release();
        g_pAvatarTexture = nullptr;
        DebugLog("Avatar texture released");
    }

    if (g_hwnd && oWndProc) {
        SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);
        oWndProc = nullptr;
        DebugLog("WndProc restored");
    }

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    DebugLog("ImGui cleanup complete");
}

void RenderImGui()
{
    HandleKeyBindingStates();

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (showMenu != prevShowMenu) {
        ImGuiIO& io = ImGui::GetIO();
        if (showMenu) {
            io.MouseDrawCursor = true;
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

            ClipCursor(NULL);
            ShowCursor(TRUE);

            RAWINPUTDEVICE rid;
            rid.usUsagePage = 0x01;
            rid.usUsage = 0x02; 
            rid.dwFlags = 0;
            rid.hwndTarget = nullptr;
            RegisterRawInputDevices(&rid, 1, sizeof(rid));

            cursorReleased = true;
            DebugLog("Menu abierto: ClipCursor(NULL), cursor liberado, ShowCursor(TRUE)");
        }
        else {
            io.MouseDrawCursor = false;
            ShowCursor(FALSE);
            cursorReleased = false;
            DebugLog("Menu cerrado");
        }
        prevShowMenu = showMenu;
    }

    if (showMenu) {
        static auto lastClipCheck = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastClipCheck).count() > 50) {
            ClipCursor(NULL);
            lastClipCheck = now;
        }
    }

    if (showMenu) {
        ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
        ImGui::Begin("AutoCeiling v3.0 - https://discord.gg/jGFZDzksfv", &showMenu, ImGuiWindowFlags_NoCollapse);

        if (g_pAvatarTexture) {
            ImGui::Image((void*)g_pAvatarTexture, ImVec2(256, 256));
            ImGui::SameLine();
            ImGui::BeginGroup();

            if (s_fontTitle) ImGui::PushFont(s_fontTitle);
            ImGui::TextColored(ImVec4(0.85f, 0.60f, 1.00f, 1.0f), "AutoCeiling v3.0");
            if (s_fontTitle) ImGui::PopFont();

            ImGui::Spacing();

            if (s_fontCredits) ImGui::PushFont(s_fontCredits);
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Desarrollado por el Agente 308 y Aika");
            if (s_fontCredits) ImGui::PopFont();

            ImGui::EndGroup();
        }
        else {
            if (s_fontTitle) ImGui::PushFont(s_fontTitle);
            ImGui::TextColored(ImVec4(0.85f, 0.60f, 1.00f, 1.0f), "AutoCeiling v3.0 - Desarrollado por el Agente 308 y Aika");
            if (s_fontTitle) ImGui::PopFont();
        }

        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Columns(2, nullptr, false);

        ImGui::Text("Ceiling");
        ImGui::Checkbox("AutoCeiling", &autoCeilingEnabled);
        ImGui::SliderInt("Smoothing", &ceilingSmoothing, 1, 60);
        ImGui::SliderFloat("Ceiling threshold (deg)", &ceilingThreshold, 0.1f, 5.0f);
        ImGui::SliderInt("Ceiling hold ms", &ceilingHoldMs, 0, 1000);
        ImGui::Separator();

        ImGui::NextColumn();
        ImGui::Text("AutoClicker");
        ImGui::Checkbox("AutoClicker (enabled)", &autoClickerEnabled);
        ImGui::SliderInt("CPS", &autoClickerCPS, 1, 20);
        if (autoClickerCPS > 20) autoClickerCPS = 20;
        ImGui::Spacing();

        ImGui::Text("Activation key:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.85f, 0.75f, 1.00f, 1.0f), "%s", KeyToString(customKey).c_str());
        ImGui::SameLine();
        if (bindingHotkey) ImGui::Button("Press any key...");
        else { if (ImGui::Button("Bind")) { bindingHotkey = true; bindingStartHotkey = std::chrono::steady_clock::now(); } }

        ImGui::Spacing();
        ImGui::Text("Menu key:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.85f, 0.75f, 1.00f, 1.0f), "%s", KeyToString(menuKey).c_str());
        ImGui::SameLine();
        if (bindingMenuKey) ImGui::Button("Press any key...");
        else { if (ImGui::Button("Bind##menu")) { bindingMenuKey = true; bindingStartMenuKey = std::chrono::steady_clock::now(); } }

        ImGui::NextColumn();
        ImGui::Columns(1);
        ImGui::Separator();
        ImGui::TextWrapped("Controls: Hold the activation key to enable features manually, or let AutoCeiling trigger AutoClick when touching the ceiling.");
        ImGui::Separator();
        ImGui::TextDisabled("Press END to unload.");

        ImGui::End();
    }

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

typedef HRESULT(__stdcall* tEndScene)(LPDIRECT3DDEVICE9);
tEndScene oEndScene = nullptr;
static bool imguiInitialized = false;

HRESULT __stdcall hkEndScene(LPDIRECT3DDEVICE9 pDevice)
{
    if (!imguiInitialized)
    {
        HWND hwnd = FindWindowA(NULL, "Left 4 Dead 2 - Direct3D 9");
        if (!hwnd) hwnd = GetForegroundWindow();
        InitImGui(hwnd, pDevice);
        imguiInitialized = true;
        DebugLog("ImGui inicializado");
    }

    RenderImGui();
    return oEndScene ? oEndScene(pDevice) : S_OK;
}

DWORD WINAPI MainThread(LPVOID lpParam) {
    g_engineBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("engine.dll"));
    if (!g_engineBase) {
        g_engineBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("client.dll"));
        DebugLog("engine.dll no encontrada usando client.dll como fallback");
    }
    else { DebugLog("engine.dll encontrada en 0x%p", (void*)g_engineBase); }

    LPDIRECT3DDEVICE9 pDevice = nullptr;
    while (!pDevice) {
        HWND hwnd = FindWindowA(NULL, "Left 4 Dead 2 - Direct3D 9");
        if (!hwnd) { Sleep(100); continue; }

        IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
        if (!pD3D) { Sleep(100); continue; }

        D3DPRESENT_PARAMETERS d3dpp = {};
        d3dpp.Windowed = TRUE;
        d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        d3dpp.hDeviceWindow = hwnd;

        if (SUCCEEDED(pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &pDevice)))
        {
            pD3D->Release();
            break;
        }

        pD3D->Release();
        Sleep(50);
    }

    if (!pDevice) { FreeLibraryAndExitThread((HMODULE)lpParam, 0); return 0; }

    MH_Initialize();
    void** vTable = *reinterpret_cast<void***>(pDevice);
    void* pEndScene = vTable[42];
    if (MH_CreateHook(pEndScene, &hkEndScene, reinterpret_cast<void**>(&oEndScene)) != MH_OK) DebugLog("MH_CreateHook fallo en vTable[42]");
    else DebugLog("MH_CreateHook OK");
    if (MH_EnableHook(pEndScene) != MH_OK) DebugLog("MH_EnableHook fallo en vTable[42]");
    else DebugLog("MH_EnableHook OK");

    g_pSetCursorPosAddr = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("user32.dll"), "SetCursorPos"));
    if (g_pSetCursorPosAddr) {
        if (MH_CreateHook(g_pSetCursorPosAddr, &hkSetCursorPos, reinterpret_cast<void**>(&oSetCursorPos)) == MH_OK) {
            MH_EnableHook(g_pSetCursorPosAddr);
            DebugLog("Hook SetCursorPos ok");
        }
        else DebugLog("CreateHook SetCursorPos fallo");
    }

    g_pGetCursorPosAddr = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("user32.dll"), "GetCursorPos"));
    if (g_pGetCursorPosAddr) {
        if (MH_CreateHook(g_pGetCursorPosAddr, &hkGetCursorPos, reinterpret_cast<void**>(&oGetCursorPos)) == MH_OK) {
            MH_EnableHook(g_pGetCursorPosAddr);
            DebugLog("Hook GetCursorPos ok");
        }
        else DebugLog("CreateHook GetCursorPos fallo");
    }

    g_pGetRawInputDataAddr = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("user32.dll"), "GetRawInputData"));
    if (g_pGetRawInputDataAddr) {
        if (MH_CreateHook(g_pGetRawInputDataAddr, &hkGetRawInputData, reinterpret_cast<void**>(&oGetRawInputData)) == MH_OK) {
            MH_EnableHook(g_pGetRawInputDataAddr);
            DebugLog("Hook GetRawInputData ok");
        }
        else DebugLog("CreateHook GetRawInputData fallo");
    }

    g_pGetRawInputBufferAddr = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("user32.dll"), "GetRawInputBuffer"));
    if (g_pGetRawInputBufferAddr) {
        if (MH_CreateHook(g_pGetRawInputBufferAddr, &hkGetRawInputBuffer, reinterpret_cast<void**>(&oGetRawInputBuffer)) == MH_OK) {
            MH_EnableHook(g_pGetRawInputBufferAddr);
            DebugLog("Hook GetRawInputBuffer ok");
        }
        else DebugLog("CreateHook GetRawInputBuffer fallo");
    }

    g_pSendInputAddr = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("user32.dll"), "SendInput"));
    if (g_pSendInputAddr) {
        if (MH_CreateHook(g_pSendInputAddr, &hkSendInput, reinterpret_cast<void**>(&oSendInput)) == MH_OK) {
            MH_EnableHook(g_pSendInputAddr);
            DebugLog("Hook SendInput ok");
        }
        else DebugLog("CreateHook SendInput fallo");
    }

    g_pMouseEventAddr = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("user32.dll"), "mouse_event"));
    if (g_pMouseEventAddr) {
        if (MH_CreateHook(g_pMouseEventAddr, &hkMouseEvent, reinterpret_cast<void**>(&oMouseEvent)) == MH_OK) {
            MH_EnableHook(g_pMouseEventAddr);
            DebugLog("Hook mouse_event ok");
        }
        else DebugLog("CreateHook mouse_event fallo");
    }

    while (true) {
        RunAutoCeiling(g_engineBase);
        RunAutoClicker();

        if (!bindingMenuKey && (GetAsyncKeyState(menuKey) & 1)) showMenu = !showMenu;
        if (GetAsyncKeyState(VK_END) & 1) break;

        if (showMenu) {
            ClipCursor(NULL); 

            static int cursorCheckCount = 0;
            if (++cursorCheckCount % 10 == 0) { 
                int cursorCount = ShowCursor(TRUE);
                while (cursorCount < 0) {
                    cursorCount = ShowCursor(TRUE);
                }
            }
        }

        Sleep(8);
    }

    if (g_pMouseEventAddr) { MH_DisableHook(g_pMouseEventAddr); MH_RemoveHook(g_pMouseEventAddr); }
    if (g_pSendInputAddr) { MH_DisableHook(g_pSendInputAddr); MH_RemoveHook(g_pSendInputAddr); }
    if (g_pGetRawInputBufferAddr) { MH_DisableHook(g_pGetRawInputBufferAddr); MH_RemoveHook(g_pGetRawInputBufferAddr); }
    if (g_pGetRawInputDataAddr) { MH_DisableHook(g_pGetRawInputDataAddr); MH_RemoveHook(g_pGetRawInputDataAddr); }
    if (g_pGetCursorPosAddr) { MH_DisableHook(g_pGetCursorPosAddr); MH_RemoveHook(g_pGetCursorPosAddr); }
    if (g_pSetCursorPosAddr) { MH_DisableHook(g_pSetCursorPosAddr); MH_RemoveHook(g_pSetCursorPosAddr); }

    MH_DisableHook(pEndScene);
    MH_RemoveHook(pEndScene);
    MH_Uninitialize();

    if (imguiInitialized) CleanupImGui();

    DebugLog("MainThread finalizando...");
    FreeLibraryAndExitThread((HMODULE)lpParam, 0);
    return 0;
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD dwReason, LPVOID) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
    }
    return TRUE;
}