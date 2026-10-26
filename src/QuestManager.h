#pragma once

#include "pch.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

struct QuestStageInfo {
    std::uint16_t index = 0;
    bool isExecuted = false;
    bool isSetupStage = false;
};

struct QuestAliasInfo {
    std::uint32_t aliasID = 0;
    std::string aliasName;
    RE::FormID refFormID = 0;
    std::string refName;
    bool isBound = false;
    bool isStaticFallback = false;
};

struct QuestItemLink {
    enum class LinkType {
        kPrerequisite,
        kRewardOrItem
    };

    LinkType type = LinkType::kPrerequisite;
    std::string itemName;
    std::string itemTypeStr;
    RE::FormID itemFormID = 0;
    std::string conditionDetails;

    RE::FormID questFormID = 0;
    std::string questName;
    std::string questEditorID;
    std::string sourcePlugin;
};

struct QuestConditionNodeInfo {
    std::string functionName;
    std::string opStr;
    std::string paramSummary;
    std::string currentValueStr;
    bool evaluatesTrue = false;
    bool isOR = false;
    RE::TESConditionItem* rawNode = nullptr;
};

struct QuestGlobalVarInfo {
    RE::TESGlobal* globalForm = nullptr;
    RE::FormID formID = 0;
    std::string editorID;
    float currentValue = 0.0f;
};

struct PapyrusVarLiveInfo {
    enum class VarType {
        kInt,
        kFloat,
        kBool,
        kString,
        kGlobalVar,
        kObjectOrOther
    };

    std::string scriptName;
    std::string varName;
    std::uint32_t rawIndex = 0;
    VarType type = VarType::kObjectOrOther;
    std::int32_t intVal = 0;
    float floatVal = 0.0f;
    bool boolVal = false;
    std::string strVal;
    std::string displayTypeStr;
    RE::TESGlobal* globalForm = nullptr;
};

struct QuestScriptStatus {
    RE::TESQuest* questPtr = nullptr;
    bool deepDetailsLoaded = false;

    RE::FormID questFormID = 0;
    std::string questName;
    std::string questEditorID;
    std::string sourcePlugin;
    std::string questTypeName;
    std::vector<std::string> attachedScripts;
    std::uint64_t vmHandle = 0;
    std::uint8_t priority = 0;
    std::uint16_t rawFlags = 0;
    std::string flagsDescription;
    std::string eventTypeStr;

    bool isEngineRunning = false;
    bool isActiveInJournal = false;
    bool isBackgroundRunning = false;
    bool isCompleted = false;
    bool isWaitingToStart = false;
    bool isBlockedByConditions = false;
    std::string blockReason;
    std::uint16_t currentStage = 0;

    std::uint32_t totalAliases = 0;
    std::uint32_t boundOrResolvableAliases = 0;
    std::uint32_t totalObjectives = 0;

    bool hasTargetCoords = false;
    RE::FormID targetRefFormID = 0;
    std::string targetRefName;
    float targetX = 0.0f;
    float targetY = 0.0f;
    float targetZ = 0.0f;
    std::string targetCellName;
    std::string targetCellEditorID;
    RE::FormID targetCellFormID = 0;
    std::string targetWorldspaceName;
    RE::FormID targetWorldspaceFormID = 0;

    std::vector<QuestConditionNodeInfo> startConditions;
    std::vector<QuestGlobalVarInfo> referencedGlobals;
    std::vector<PapyrusVarLiveInfo> papyrusVars;
};

struct ActorRefEntry {
    RE::FormID questFormID = 0;
    std::string questName;
    std::string questEditorID;
    std::uint32_t aliasID = 0;
    std::string aliasName;
    RE::FormID refFormID = 0;
    std::string refName;
};

struct ActorMoveTrack {
    RE::NiPoint3 lastPos{};
    double sinceTime = 0.0;
};

struct QuestEntry {
    RE::TESQuest* quest = nullptr;
    RE::FormID formID = 0;
    std::string name;
    std::string editorID;
    std::string sourcePlugin;
    std::string typeName;
    std::uint16_t currentStage = 0;
    bool isPinned = false;
    bool isActiveInJournal = false;
    bool isBackgroundRunning = false;
    bool isCompleted = false;
    std::vector<QuestStageInfo> stages;
    std::vector<QuestAliasInfo> cachedAliases;
    bool aliasesCached = false;
};

class QuestManager {
public:
    static QuestManager* GetSingleton() {
        static QuestManager singleton;
        return &singleton;
    }

    void LoadOrCreateIni();
    void SaveIni();
    std::uint32_t GetToggleKeyScanCode() const { return m_toggleKeyScanCode; }
    const std::string& GetToggleKeyName() const { return m_toggleKeyName; }
    void RequestRefresh() { m_needsRefresh.store(true); }
    void RefreshQuests();
    void RenderUI();

private:
    QuestManager() = default;

    void ApplyOrStartQuestStage(const QuestEntry& a_entry, std::uint16_t a_targetStage, bool a_isLastStage);
    void AbortAndResetQuest(RE::TESQuest* a_quest, const std::string& a_editorID);
    void TeleportToQuestTarget(RE::TESQuest* a_quest, const std::string& a_editorID);
    void TeleportToSpecificAlias(RE::TESQuest* a_quest, std::uint32_t a_aliasID, const std::string& a_aliasName);
    void ExportDebugReportToFile();
    void GiveOrUnlockFormForPlayer(RE::FormID a_formID, const std::string& a_itemName);
    bool ApplyPapyrusVariableEdit(std::uint64_t a_vmHandle, const PapyrusVarLiveInfo& a_varInfo);

    void SelectDefaultStageInCombo(const QuestEntry& a_entry);
    void RenderActiveQuestsTab();
    std::unordered_set<RE::FormID> m_multiActiveQuests;
    bool m_multiDragging = false;
    bool m_multiDragAdd = true;
    void RenderAllQuestsTab();
    void RenderItemsAndRewardsTab();
    void RenderScriptMonitorTab();
    void RenderActorRefsTab();
    void ScanActorRefs();
    void QueueActorVerify(RE::FormID a_refID, std::string a_label, std::function<bool(RE::Actor*)> a_check);
    void OpenInActorsTab(const QuestEntry& a_entry, std::uint32_t a_aliasID);
    void RenderQuestDetailsPanel(QuestEntry& a_entry);

    void TogglePinQuest(RE::FormID a_formID, const std::string& a_editorID);
    void SortQuestsWithPins();
    void FulfillSingleConditionNode(QuestScriptStatus& a_st, QuestConditionNodeInfo& a_nodeInfo);

    static RE::TESObjectREFR* ResolveStaticAliasReference(RE::TESQuest* a_quest, RE::BGSBaseAlias* a_alias, int a_depth = 0);
    static void ForceFillQuestAliases(RE::TESQuest* a_quest);

    static std::vector<QuestStageInfo> ExtractAllStages(RE::TESQuest* a_quest, bool a_isActuallyStarted);
    static std::vector<QuestAliasInfo> ExtractQuestAliases(RE::TESQuest* a_quest);
    static void ExtractQuestItemsAndConditions(RE::TESQuest* a_quest, const QuestEntry& a_entry, std::vector<QuestItemLink>& a_outLinks);
    static QuestScriptStatus AnalyzeQuestScriptAndState(RE::TESQuest* a_quest, const QuestEntry& a_entry, bool a_loadDeepDetails = false);
    static void PopulateScriptDeepDetails(QuestScriptStatus& status);

    static bool HasActiveJournalObjectives(RE::TESQuest* a_quest);
    static void ForceDisplayObjectiveInJournal(RE::TESQuest* a_quest, RE::BGSQuestObjective* a_obj);
    static void EnsureQuestTargetMarkersEnabled(RE::TESQuest* a_quest);
    static void ForceQuestWhiteInJournal(RE::TESQuest* a_quest);
    static RE::TESObjectREFR* FindBestTeleportTarget(RE::TESQuest* a_quest);
    static std::string GetObjectiveStatusString(const RE::BGSQuestObjective* a_obj);
    static std::string GetQuestTypeString(RE::QUEST_DATA::Type a_type);
    static bool MatchesFilter(const QuestEntry& a_entry, const char* a_searchBuffer);
    bool MatchesScriptMonitorFilter(const QuestScriptStatus& a_st) const;
    bool m_iniLoaded = false;

    std::uint32_t m_toggleKeyScanCode = 0xD2; 
    std::string m_toggleKeyName = "VK_INSERT";
    std::unordered_set<std::string> m_pinnedQuests;

    std::mutex m_mutex;
    std::atomic<bool> m_needsRefresh{ true };
    std::vector<QuestEntry> m_allQuests;
    std::vector<QuestItemLink> m_itemLinks;
    std::vector<QuestScriptStatus> m_scriptStatuses;
    std::vector<std::string> m_pluginList;

    RE::FormID m_selectedActiveQuestID = 0;
    RE::FormID m_selectedAllQuestID = 0;
    int m_selectedItemLinkIdx = -1;
    int m_selectedScriptStatusIdx = -1;

    int m_selectedStageComboIdx = 0;
    int m_selectedPluginIdx = 0;
    std::string m_selectedCategory = "All Categories";

    bool m_showTechnicalSetupStages = false;
    bool m_switchToAllQuestsTab = false;
    bool m_switchToActorsTab = false;
    bool m_scrollActorToSelected = false;
    bool m_scrollAllQuestsToSelected = false;

    std::vector<ActorRefEntry> m_actorRefs;
    std::unordered_map<RE::FormID, ActorMoveTrack> m_actorTracks;
    bool m_actorRefsScanned = false;
    int m_selectedActorRefIdx = -1;
    bool m_actorOnlyJournalQuests = true;
    bool m_actorOnlyProblems = false;
    bool m_actorResetInventory = false;
    char m_searchActorRefs[128] = "";

    bool m_showRewardsFilter = true;
    bool m_showPrerequisitesFilter = true;
    int m_scriptMonitorFilterMode = 0;

    char m_searchActive[128] = "";
    char m_searchAll[128] = "";
    char m_searchItems[128] = "";
    char m_searchScripts[128] = "";
    char m_searchPapyrusVars[128] = "";
    std::string m_statusMessage;
};