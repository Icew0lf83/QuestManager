#pragma once

#include "pch.h"

namespace D3DHook {
    // Installiert den IDXGISwapChain::Present Hook & den Input-Listener
    void Install();

    // Gibt zurück, ob das ImGui-Menü aktuell sichtbar ist
    bool IsMenuOpen();
    void SetMenuOpen(bool a_open);
}