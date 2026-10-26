#include "D3DHook.h"
#include "QuestManager.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace D3DHook {
    namespace {
        using Present_t = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
        Present_t g_OriginalPresent = nullptr;
        WNDPROC g_OriginalWndProc = nullptr;

        ID3D11Device* g_Device = nullptr;
        ID3D11DeviceContext* g_Context = nullptr;
        ID3D11RenderTargetView* g_RenderTargetView = nullptr;
        HWND g_Hwnd = nullptr;

        bool g_Initialized = false;
        bool g_MenuOpen = false;

        void ToggleGameControls(bool a_enableGameControls) {
            if (auto* controlMap = RE::ControlMap::GetSingleton()) {
                controlMap->ToggleControls(RE::ControlMap::UEFlag::kAll, a_enableGameControls, false);
            }
        }

        // Verarbeitet NUR Sondertasten (Backspace, Delete, Pfeiltasten, Enter),
        // da normale Buchstaben/Zahlen bereits über Skyrims RE::CharEvent kommen!
        void ForwardSpecialKeysToImGui(std::uint32_t a_scanCode, bool a_isDown) {
            ImGuiIO& io = ImGui::GetIO();
            switch (a_scanCode) {
            case 0x0E: io.AddKeyEvent(ImGuiKey_Backspace, a_isDown); break;
            case 0xD3: io.AddKeyEvent(ImGuiKey_Delete, a_isDown);    break;
            case 0x1C: io.AddKeyEvent(ImGuiKey_Enter, a_isDown);     break;
            case 0xCB: io.AddKeyEvent(ImGuiKey_LeftArrow, a_isDown); break;
            case 0xCD: io.AddKeyEvent(ImGuiKey_RightArrow, a_isDown); break;
            case 0xC7: io.AddKeyEvent(ImGuiKey_Home, a_isDown);      break;
            case 0xCF: io.AddKeyEvent(ImGuiKey_End, a_isDown);       break;
            default: break;
            }
        }

        class InputListener : public RE::BSTEventSink<RE::InputEvent*> {
        public:
            static InputListener* GetSingleton() {
                static InputListener singleton;
                return &singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override {
                if (!a_event || !g_Initialized) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                ImGuiIO& io = ImGui::GetIO();

                for (auto* event = *a_event; event; event = event->next) {               
                    if (auto* charEvent = event->AsCharEvent()) {
                        if (g_MenuOpen && charEvent->keyCode >= 32) {
                            io.AddInputCharacter(charEvent->keyCode);
                        }
                    }
                    else if (auto* button = event->AsButtonEvent()) {
                        std::uint32_t hotkeyScan = QuestManager::GetSingleton()->GetToggleKeyScanCode();
                        if (button->GetDevice() == RE::INPUT_DEVICE::kKeyboard && button->GetIDCode() == hotkeyScan && button->IsDown()) {
                            SetMenuOpen(!g_MenuOpen);
                            return RE::BSEventNotifyControl::kStop;
                        }

                        if (g_MenuOpen) {
                            if (button->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
                                ForwardSpecialKeysToImGui(button->GetIDCode(), button->IsPressed());
                            }

                            if (button->GetDevice() == RE::INPUT_DEVICE::kMouse) {
                                if (button->GetIDCode() == 8) {
                                    io.MouseWheel += button->Value();
                                }
                                else if (button->GetIDCode() == 9) {
                                    io.MouseWheel -= button->Value();
                                }
                            }
                        }
                    }
                }

                return RE::BSEventNotifyControl::kContinue;
            }
        };

        LRESULT CALLBACK WndProcHook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
            if (uMsg == WM_ACTIVATEAPP || uMsg == WM_SETFOCUS || uMsg == WM_KILLFOCUS) {
                if (g_MenuOpen) {
                    bool isActivated = (uMsg == WM_SETFOCUS) || (uMsg == WM_ACTIVATEAPP && wParam == TRUE);
                    if (isActivated) {
                        ClipCursor(nullptr);
                        ReleaseCapture();
                        ToggleGameControls(false);
                    }
                    else {
                        ClipCursor(nullptr);
                        ReleaseCapture();
                    }
                }
            }

            if (g_MenuOpen) {
                // WM_CHAR in WndProc ignorieren, falls Windows sie doch sendet, damit nichts doppelt zu RE::CharEvent ankommt
                if (uMsg == WM_CHAR) {
                    return 0;
                }
                ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
                if (uMsg >= WM_KEYFIRST && uMsg <= WM_KEYLAST) {
                    return 0;
                }
            }

            return CallWindowProcA(g_OriginalWndProc, hWnd, uMsg, wParam, lParam);
        }

        void SetupImGuiStyle() {
            ImGuiStyle& style = ImGui::GetStyle();
            style.WindowRounding = 6.0f;
            style.FrameRounding = 4.0f;
            style.GrabRounding = 4.0f;
            style.ScrollbarRounding = 4.0f;
            style.ScrollbarSize = 16.0f;

            ImVec4* colors = style.Colors;
            colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.13f, 0.96f);
            colors[ImGuiCol_Header] = ImVec4(0.22f, 0.26f, 0.34f, 0.80f);
            colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.34f, 0.45f, 0.90f);
            colors[ImGuiCol_HeaderActive] = ImVec4(0.32f, 0.40f, 0.55f, 1.00f);
            colors[ImGuiCol_Button] = ImVec4(0.20f, 0.25f, 0.34f, 1.00f);
            colors[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.35f, 0.48f, 1.00f);
            colors[ImGuiCol_ButtonActive] = ImVec4(0.35f, 0.44f, 0.60f, 1.00f);
            colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
            colors[ImGuiCol_Tab] = ImVec4(0.16f, 0.20f, 0.26f, 0.90f);
            colors[ImGuiCol_TabHovered] = ImVec4(0.28f, 0.35f, 0.48f, 1.00f);
            colors[ImGuiCol_TabActive] = ImVec4(0.24f, 0.31f, 0.43f, 1.00f);
        }

        void InitImGui(IDXGISwapChain* a_swapChain) {
            if (FAILED(a_swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_Device)))) {
                return;
            }
            g_Device->GetImmediateContext(&g_Context);

            DXGI_SWAP_CHAIN_DESC sd{};
            a_swapChain->GetDesc(&sd);
            g_Hwnd = sd.OutputWindow;

            ID3D11Texture2D* backBuffer = nullptr;
            if (SUCCEEDED(a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
                g_Device->CreateRenderTargetView(backBuffer, nullptr, &g_RenderTargetView);
                backBuffer->Release();
            }

            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            io.IniFilename = "Data/SKSE/Plugins/QuestManager_ImGui.ini";

            SetupImGuiStyle();

            ImGui_ImplWin32_Init(g_Hwnd);
            ImGui_ImplDX11_Init(g_Device, g_Context);

            g_OriginalWndProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrA(g_Hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHook))
                );

            if (auto* inputMgr = RE::BSInputDeviceManager::GetSingleton()) {
                inputMgr->AddEventSink(InputListener::GetSingleton());
            }

            QuestManager::GetSingleton()->LoadOrCreateIni();
            g_Initialized = true;
        }

        HRESULT WINAPI PresentHook(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags) {
            if (!g_Initialized) {
                InitImGui(a_swapChain);
            }

            if (g_Initialized && g_MenuOpen) {
                ToggleGameControls(false);

                if (GetForegroundWindow() == g_Hwnd) {
                    ClipCursor(nullptr);

                    ImGuiIO& io = ImGui::GetIO();
                    POINT cursorPos{};
                    if (GetCursorPos(&cursorPos) && ScreenToClient(g_Hwnd, &cursorPos)) {
                        io.MousePos = ImVec2(static_cast<float>(cursorPos.x), static_cast<float>(cursorPos.y));
                    }

                    io.MouseDrawCursor = true;
                    io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                    io.MouseDown[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
                    io.MouseDown[2] = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
                }

                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();

                QuestManager::GetSingleton()->RenderUI();

                ImGui::Render();
                g_Context->OMSetRenderTargets(1, &g_RenderTargetView, nullptr);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            }
            else if (g_Initialized) {
                ImGui::GetIO().MouseDrawCursor = false;
            }

            return g_OriginalPresent(a_swapChain, a_syncInterval, a_flags);
        }
    }

    bool IsMenuOpen() {
        return g_MenuOpen;
    }

    void SetMenuOpen(bool a_open) {
        if (g_MenuOpen == a_open) {
            return;
        }
        g_MenuOpen = a_open;
        ToggleGameControls(!g_MenuOpen);

        if (g_MenuOpen) {
            ClipCursor(nullptr);
            QuestManager::GetSingleton()->RequestRefresh();
        }
    }

    void Install() {
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer) {
            return;
        }

        auto* swapChain = reinterpret_cast<IDXGISwapChain*>(renderer->GetRuntimeData().renderWindows[0].swapChain);
        if (!swapChain) {
            return;
        }

        void** vtable = *reinterpret_cast<void***>(swapChain);
        g_OriginalPresent = reinterpret_cast<Present_t>(vtable[8]);

        REL::safe_write(reinterpret_cast<uintptr_t>(&vtable[8]), reinterpret_cast<uintptr_t>(&PresentHook));
    }
}