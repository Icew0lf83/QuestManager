#include "pch.h"
#include "D3DHook.h"
#include "QuestManager.h"

namespace {
    void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg) {
        if (!a_msg) {
            return;
        }

        switch (a_msg->type) {
            case SKSE::MessagingInterface::kDataLoaded:
                // Sobald alle Mods/Quests und das D3D11-SwapChain bereit sind, Hook installieren
                D3DHook::Install();
                QuestManager::GetSingleton()->RefreshQuests();
                break;

            case SKSE::MessagingInterface::kPostLoadGame:
            case SKSE::MessagingInterface::kNewGame:
                // Nach dem Laden eines Spielstands Quest-Liste aktualisieren
                QuestManager::GetSingleton()->RefreshQuests();
                break;
        }
    }
}

extern "C" [[maybe_unused]] __declspec(dllexport) const SKSE::PluginVersionData SKSEPlugin_Version = []() noexcept {
    SKSE::PluginVersionData data{};
    data.PluginVersion({ 1, 0, 0, 0 });
    data.PluginName("SkyrimQuestManager");
    data.AuthorName("Icew0lf");
    data.UsesAddressLibrary();
    data.UsesUpdatedStructs();
    data.UsesNoStructs();
    return data;
}();

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);

    if (auto* messaging = SKSE::GetMessagingInterface()) {
        messaging->RegisterListener(OnSKSEMessage);
    }

    return true;
}