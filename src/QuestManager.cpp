#include "QuestManager.h"
#include "D3DHook.h"
#include "imgui.h"
#include "RE/A/ActorValues.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

namespace {
    void ExecuteConsoleCommand(const std::string& a_command) {
        RE::TESObjectREFR* targetRef = nullptr;
        std::string command = a_command;

        // "<8 hex digits>.<command>" cannot be parsed by the script compiler, so run the command on the target reference instead.
        if (a_command.size() > 9 && a_command[8] == '.' &&
            std::all_of(a_command.begin(), a_command.begin() + 8, [](unsigned char c) { return std::isxdigit(c) != 0; })) {
            const auto refID = static_cast<RE::FormID>(std::stoul(a_command.substr(0, 8), nullptr, 16));
            targetRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(refID);
            if (!targetRef) {
                return;
            }
            command = a_command.substr(9);
        }

        auto* scriptFactory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
        auto* script = scriptFactory ? scriptFactory->Create() : nullptr;
        if (script) {
            script->SetCommand(command);
            script->CompileAndRun(targetRef);
            delete script;
        }
    }

    void CallNativeSetObjectiveDisplayed(RE::TESQuest* a_quest, std::uint16_t a_objectiveIndex, bool a_displayed, bool a_force = false) {
        if (!a_quest) {
            return;
        }
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return;
        }
        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) {
            return;
        }

        RE::VMHandle handle = policy->GetHandleForObject(RE::FormType::Quest, a_quest);
        if (handle == policy->EmptyHandle()) {
            return;
        }

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        auto args = RE::MakeFunctionArguments(
            static_cast<std::int32_t>(a_objectiveIndex),
            static_cast<bool>(a_displayed),
            static_cast<bool>(a_force)
        );
        vm->DispatchMethodCall(handle, "Quest"sv, "SetObjectiveDisplayed"sv, args, callback);
    }

    void CallNativeSetObjectiveCompleted(RE::TESQuest* a_quest, std::uint16_t a_objectiveIndex, bool a_completed = true) {
        if (!a_quest) {
            return;
        }
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return;
        }
        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) {
            return;
        }

        RE::VMHandle handle = policy->GetHandleForObject(RE::FormType::Quest, a_quest);
        if (handle == policy->EmptyHandle()) {
            return;
        }

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        auto args = RE::MakeFunctionArguments(
            static_cast<std::int32_t>(a_objectiveIndex),
            static_cast<bool>(a_completed)
        );
        vm->DispatchMethodCall(handle, "Quest"sv, "SetObjectiveCompleted"sv, args, callback);
    }

    void CallNativeSetQuestStage(RE::TESQuest* a_quest, std::uint16_t a_stage) {
        if (!a_quest) {
            return;
        }
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return;
        }
        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) {
            return;
        }

        RE::VMHandle handle = policy->GetHandleForObject(RE::FormType::Quest, a_quest);
        if (handle == policy->EmptyHandle()) {
            return;
        }

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        auto args = RE::MakeFunctionArguments(static_cast<std::int32_t>(a_stage));
        vm->DispatchMethodCall(handle, "Quest"sv, "SetCurrentStageID"sv, args, callback);
    }

    void CallNativeSetQuestActive(RE::TESQuest* a_quest, bool a_active) {
        if (!a_quest) {
            return;
        }
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return;
        }
        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) {
            return;
        }

        RE::VMHandle handle = policy->GetHandleForObject(RE::FormType::Quest, a_quest);
        if (handle == policy->EmptyHandle()) {
            return;
        }

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        auto args = RE::MakeFunctionArguments(static_cast<bool>(a_active));
        vm->DispatchMethodCall(handle, "Quest"sv, "SetActive"sv, args, callback);
    }

    void CallNativeEnableObjectReference(RE::TESObjectREFR* a_ref) {
        if (!a_ref || !a_ref->IsDisabled()) {
            return;
        }
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return;
        }
        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) {
            return;
        }

        RE::VMHandle handle = policy->GetHandleForObject(RE::FormType::Reference, a_ref);
        if (handle == policy->EmptyHandle()) {
            return;
        }

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        auto args = RE::MakeFunctionArguments(false);
        vm->DispatchMethodCall(handle, "ObjectReference"sv, "Enable"sv, args, callback);
    }

    void CallNativeAddToMap(RE::TESObjectREFR* a_ref) {
        if (!a_ref) {
            return;
        }
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return;
        }
        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) {
            return;
        }

        RE::VMHandle handle = policy->GetHandleForObject(RE::FormType::Reference, a_ref);
        if (handle == policy->EmptyHandle()) {
            return;
        }

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        auto args = RE::MakeFunctionArguments(false);
        vm->DispatchMethodCall(handle, "ObjectReference"sv, "AddToMap"sv, args, callback);
    }

    std::unordered_map<RE::FormID, RE::TESObjectREFR*> g_baseToRefCache;
    bool g_refCacheBuilt = false;

    void RebuildBaseToRefCache() {
        g_baseToRefCache.clear();
        const auto& [allForms, lock] = RE::TESForm::GetAllForms();
        RE::BSReadLockGuard guard(lock);
        if (!allForms) {
            return;
        }
        g_baseToRefCache.reserve(16384);
        for (const auto& [formID, form] : *allForms) {
            if (!form) {
                continue;
            }
            auto fType = form->GetFormType();
            if (fType == RE::FormType::ActorCharacter || fType == RE::FormType::Reference) {
                auto* refr = static_cast<RE::TESObjectREFR*>(form);
                if (auto* baseObj = refr->GetBaseObject()) {
                    RE::FormID baseID = baseObj->GetFormID();
                    auto it = g_baseToRefCache.find(baseID);
                    if (it == g_baseToRefCache.end() || (it->second->IsDisabled() && !refr->IsDisabled())) {
                        g_baseToRefCache[baseID] = refr;
                    }
                }
            }
        }
        g_refCacheBuilt = true;
    }

    RE::TESObjectREFR* FindActorReferenceByBaseNPC(RE::TESNPC* a_npc) {
        if (!a_npc) {
            return nullptr;
        }
        if (!g_refCacheBuilt) {
            RebuildBaseToRefCache();
        }
        auto it = g_baseToRefCache.find(a_npc->GetFormID());
        return it != g_baseToRefCache.end() ? it->second : nullptr;
    }

    RE::TESObjectREFR* FindPlacedReferenceByBaseForm(RE::TESForm* a_baseForm) {
        if (!a_baseForm) {
            return nullptr;
        }
        if (auto* ref = a_baseForm->As<RE::TESObjectREFR>()) {
            return ref;
        }
        if (!g_refCacheBuilt) {
            RebuildBaseToRefCache();
        }
        auto it = g_baseToRefCache.find(a_baseForm->GetFormID());
        return it != g_baseToRefCache.end() ? it->second : nullptr;
    }

    RE::TESObjectREFR* FindReferenceInLocation(RE::BGSLocation* a_loc) {
        if (!a_loc) {
            return nullptr;
        }
        for (const auto& specRef : a_loc->specialRefs) {
            if (specRef.refData.refID != 0) {
                if (auto* form = RE::TESForm::LookupByID(specRef.refData.refID)) {
                    if (auto* refr = form->As<RE::TESObjectREFR>()) {
                        return refr;
                    }
                }
            }
        }
        if (auto markerHandle = a_loc->worldLocMarker.get()) {
            return markerHandle.get();
        }
        return nullptr;
    }

    RE::TESObjectREFR* ResolveFromConditionChain(const RE::TESCondition& a_cond) {
        for (auto* node = a_cond.head; node != nullptr; node = node->next) {
            if (auto refPtr = node->data.runOnRef.get()) {
                return refPtr.get();
            }

            auto fn = node->data.functionData.function.get();
            void* param1 = node->data.functionData.params[0];
            if (!param1) {
                continue;
            }

            switch (fn) {
            case RE::FUNCTION_DATA::FunctionID::kGetIsID:
            case RE::FUNCTION_DATA::FunctionID::kGetIsReference:
            case RE::FUNCTION_DATA::FunctionID::kGetWithinDistance:
            case RE::FUNCTION_DATA::FunctionID::kGetDistance:
            case RE::FUNCTION_DATA::FunctionID::kGetInContainer: {
                auto* form = static_cast<RE::TESForm*>(param1);
                if (auto* refr = FindPlacedReferenceByBaseForm(form)) {
                    return refr;
                }
                break;
            }
            case RE::FUNCTION_DATA::FunctionID::kGetInCurrentLoc:
            case RE::FUNCTION_DATA::FunctionID::kGetIsEditorLocation: {
                auto* form = static_cast<RE::TESForm*>(param1);
                if (auto* loc = form ? form->As<RE::BGSLocation>() : nullptr) {
                    if (auto* refr = FindReferenceInLocation(loc)) {
                        return refr;
                    }
                }
                break;
            }
            default:
                break;
            }
        }
        return nullptr;
    }

    bool ContainsCaseInsensitive(std::string_view a_haystack, std::string_view a_needle) {
        if (a_needle.empty()) {
            return true;
        }
        auto it = std::search(
            a_haystack.begin(), a_haystack.end(),
            a_needle.begin(), a_needle.end(),
            [](char ch1, char ch2) {
                return std::toupper(static_cast<unsigned char>(ch1)) == std::toupper(static_cast<unsigned char>(ch2));
            }
        );
        return it != a_haystack.end();
    }

    std::string DescribeForm(RE::TESForm* a_form, std::string& a_outTypeStr) {
        if (!a_form) {
            a_outTypeStr = "None";
            return "";
        }

        switch (a_form->GetFormType()) {
        case RE::FormType::Weapon:      a_outTypeStr = "Weapon"; break;
        case RE::FormType::Armor:       a_outTypeStr = "Armor"; break;
        case RE::FormType::Ammo:        a_outTypeStr = "Ammo"; break;
        case RE::FormType::Book:        a_outTypeStr = "Book / Note"; break;
        case RE::FormType::Ingredient:  a_outTypeStr = "Ingredient"; break;
        case RE::FormType::Misc:        a_outTypeStr = "Misc Item"; break;
        case RE::FormType::KeyMaster:   a_outTypeStr = "Key"; break;
        case RE::FormType::AlchemyItem: a_outTypeStr = "Potion / Food"; break;
        case RE::FormType::SoulGem:     a_outTypeStr = "Soul Gem"; break;
        case RE::FormType::Spell:       a_outTypeStr = "Spell"; break;
        case RE::FormType::Scroll:      a_outTypeStr = "Scroll"; break;
        case RE::FormType::Shout:       a_outTypeStr = "Shout"; break;
        case RE::FormType::Perk:        a_outTypeStr = "Perk"; break;
        case RE::FormType::Faction:     a_outTypeStr = "Faction"; break;
        case RE::FormType::Global:      a_outTypeStr = "Global Var"; break;
        case RE::FormType::NPC:         a_outTypeStr = "NPC / ActorBase"; break;
        case RE::FormType::Location:    a_outTypeStr = "Location"; break;
        case RE::FormType::Keyword:     a_outTypeStr = "Keyword"; break;
        case RE::FormType::Quest:       a_outTypeStr = "Quest"; break;
        case RE::FormType::LeveledItem: a_outTypeStr = "Leveled Item (Reward Pool)"; break;
        default:                        a_outTypeStr = "Form"; break;
        }

        std::string name = a_form->GetName() ? a_form->GetName() : "";
        std::string edid = a_form->GetFormEditorID() ? a_form->GetFormEditorID() : "";

        if (!name.empty() && !edid.empty()) {
            return std::format("{} [{}]", name, edid);
        }
        if (!name.empty()) {
            return name;
        }
        if (!edid.empty()) {
            return edid;
        }
        return std::format("FormID 0x{:08X}", a_form->GetFormID());
    }

    bool IsInventoryOrRewardFormType(RE::FormType a_type) {
        switch (a_type) {
        case RE::FormType::Weapon:
        case RE::FormType::Armor:
        case RE::FormType::Ammo:
        case RE::FormType::Book:
        case RE::FormType::Ingredient:
        case RE::FormType::Misc:
        case RE::FormType::KeyMaster:
        case RE::FormType::AlchemyItem:
        case RE::FormType::SoulGem:
        case RE::FormType::Spell:
        case RE::FormType::Scroll:
        case RE::FormType::Shout:
        case RE::FormType::Perk:
        case RE::FormType::LeveledItem:
            return true;
        default:
            return false;
        }
    }



    struct HotkeyOption {
        const char* keyName;
        std::uint32_t scanCode;
        const char* description;
    };

    constexpr HotkeyOption kHotkeyCatalog[] = {
          { "VK_INSERT",       0xD2, "Insert key (Default)" },
          { "VK_DELETE",       0xD3, "Delete key" },
          { "VK_HOME",         0xC7, "Home key" },
          { "VK_END",          0xCF, "End key" },
          { "VK_PRIOR",        0xC9, "Page Up key" },
          { "VK_NEXT",         0xD1, "Page Down key" },
          { "VK_PAUSE",        0xC5, "Pause / Break key" },
          { "VK_F1",           0x3B, "Function key F1" },
          { "VK_F2",           0x3C, "Function key F2" },
          { "VK_F3",           0x3D, "Function key F3" },
          { "VK_F4",           0x3E, "Function key F4" },
          { "VK_F5",           0x3F, "Function key F5" },
          { "VK_F6",           0x40, "Function key F6" },
          { "VK_F7",           0x41, "Function key F7" },
          { "VK_F8",           0x42, "Function key F8" },
          { "VK_F9",           0x43, "Function key F9" },
          { "VK_F10",          0x44, "Function key F10" },
          { "VK_F11",          0x57, "Function key F11" },
          { "VK_F12",          0x58, "Function key F12" },
          { "VK_NUMPAD0",      0x52, "Numpad 0 key" },
          { "VK_NUMPAD1",      0x4F, "Numpad 1 key" },
          { "VK_NUMPAD2",      0x50, "Numpad 2 key" },
          { "VK_NUMPAD3",      0x51, "Numpad 3 key" },
          { "VK_NUMPAD4",      0x4B, "Numpad 4 key" },
          { "VK_NUMPAD5",      0x4C, "Numpad 5 key" },
          { "VK_NUMPAD6",      0x4D, "Numpad 6 key" },
          { "VK_NUMPAD7",      0x47, "Numpad 7 key" },
          { "VK_NUMPAD8",      0x48, "Numpad 8 key" },
          { "VK_NUMPAD9",      0x49, "Numpad 9 key" },
          { "VK_MULTIPLY",     0x37, "Numpad * (Multiply key)" },
          { "VK_ADD",          0x4E, "Numpad + (Add / Plus key)" },
          { "VK_SUBTRACT",     0x4A, "Numpad - (Subtract / Minus key)" },
          { "VK_DIVIDE",       0xB5, "Numpad / (Divide key)" },
          { "VK_DECIMAL",      0x53, "Numpad . (Decimal / Del key)" },
          { "VK_NUMPAD_ENTER", 0x9C, "Numpad Enter key" }
    };

    std::string TrimString(std::string_view a_str) {
        const auto first = a_str.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) {
            return "";
        }
        const auto last = a_str.find_last_not_of(" \t\r\n");
        return std::string(a_str.substr(first, (last - first + 1)));
    }

    std::string GetComparisonOperatorString(RE::CONDITION_ITEM_DATA::OpCode a_op) {
        using Op = RE::CONDITION_ITEM_DATA::OpCode;
        switch (a_op) {
        case Op::kEqualTo:              return "==";
        case Op::kNotEqualTo:           return "!=";
        case Op::kGreaterThan:          return ">";
        case Op::kGreaterThanOrEqualTo: return ">=";
        case Op::kLessThan:             return "<";
        case Op::kLessThanOrEqualTo:    return "<=";
        default:                        return "==";
        }
    }

    std::string GetConditionFunctionName(RE::FUNCTION_DATA::FunctionID a_fn) {
        const auto rawId = static_cast<std::uint16_t>(a_fn);
        switch (rawId) {
        case 1:   return "GetWantBlocking";
        case 5:   return "GetLocked";
        case 6:   return "GetPos";
        case 8:   return "GetAngle";
        case 10:  return "GetStartingPos";
        case 12:  return "GetSecondsPassed";
        case 14:  return "GetActorValue";
        case 18:  return "GetCurrentTime";
        case 24:  return "GetScale";
        case 25:  return "IsMoving";
        case 26:  return "IsTurning";
        case 27:  return "GetLineOfSight";
        case 32:  return "GetInSameCell";
        case 35:  return "GetDisabled";
        case 36:  return "MenuMode";
        case 39:  return "GetDisease";
        case 41:  return "GetClothingValue";
        case 42:  return "SameFaction";
        case 43:  return "SameRace";
        case 44:  return "SameSex";
        case 45:  return "GetDetected";
        case 46:  return "GetDead";
        case 47:  return "GetItemCount";
        case 48:  return "GetGold";
        case 49:  return "GetSleeping";
        case 50:  return "GetTalkedToPC";
        case 53:  return "GetScriptVariable";
        case 56:  return "GetQuestRunning";
        case 58:  return "GetStage";
        case 59:  return "GetStageDone";
        case 60:  return "GetFactionRankDifference";
        case 61:  return "GetAlarmed";
        case 62:  return "IsRaining";
        case 63:  return "GetAttacked";
        case 64:  return "GetIsCreature";
        case 65:  return "GetLockLevel";
        case 66:  return "GetShouldAttack";
        case 67:  return "GetInCell";
        case 68:  return "GetIsClass";
        case 69:  return "GetIsRace";
        case 70:  return "GetIsSex";
        case 71:  return "GetInFaction";
        case 72:  return "GetIsID";
        case 73:  return "GetFactionRank";
        case 74:  return "GetGlobalValue";
        case 75:  return "IsSnowing";
        case 77:  return "GetRandomPercent";
        case 79:  return "GetQuestVariable";
        case 80:  return "GetLevel";
        case 84:  return "GetDeadCount";
        case 91:  return "GetIsAlerted";
        case 98:  return "GetPlayerControlsDisabled";
        case 99:  return "GetHeadingAngle";
        case 101: return "IsWeaponOut";
        case 106: return "IsFacingUp";
        case 107: return "GetKnockedState";
        case 109: return "IsWeaponSkillType";
        case 110: return "GetCurrentAIPackage";
        case 111: return "IsWaiting";
        case 112: return "IsIdlePlaying";
        case 116: return "IsIntimidatedbyPlayer";
        case 117: return "IsPlayerInRegion";
        case 122: return "GetCrime";
        case 123: return "IsGreetingPlayer";
        case 127: return "HasBeenEaten";
        case 130: return "GetPCIsClass";
        case 131: return "GetPCIsRace";
        case 132: return "GetPCIsSex";
        case 133: return "GetPCInFaction";
        case 136: return "GetIsReference";
        case 141: return "GetDestroyed";
        case 148: return "GetCurrentWeatherPercent";
        case 149: return "GetIsCurrentWeather";
        case 150: return "IsContinuingPackagePCNear";
        case 161: return "GetIsCurrentPackage";
        case 172: return "GetCombatState";
        case 180: return "GetRelativeAngle";
        case 182: return "GetEquipped";
        case 193: return "GetPCExpelled";
        case 195: return "GetPCFactionMurder";
        case 197: return "GetPCEnemyofFaction";
        case 199: return "GetPCFactionAttack";
        case 214: return "HasMagicEffect";
        case 223: return "IsSpellTarget";
        case 242: return "IsScenePackageRunning";
        case 244: return "IsInInterior";
        case 246: return "GetIsObjectType";
        case 249: return "GetTotalPersuasionNumber";
        case 254: return "GetInWorldspace";
        case 258: return "GetPCMiscStat";
        case 264: return "HasSameEditorLocAsRef";
        case 265: return "HasSameEditorLocAsRefAlias";
        case 285: return "IsSwimming";
        case 289: return "IsSneaking";
        case 300: return "IsInCombat";
        case 304: return "IsWaterObject";
        case 310: return "GetInCurrentLoc";
        case 315: return "IsBribedbyPlayer";
        case 359: return "IsInInteriorCell";
        case 360: return "GetInCurrentLocAlias";
        case 362: return "HasLinkedRef";
        case 366: return "GetStolenItemValueNoCrime";
        case 372: return "IsInList";
        case 397: return "GetIsEditorLocation";
        case 402: return "IsInSameCurrentLocAsRef";
        case 403: return "IsInSameCurrentLocAsRefAlias";
        case 408: return "LocAliasIsLocation";
        case 410: return "IsEditorLocation";
        case 432: return "IsWardState";
        case 448: return "HasPerk";
        case 449: return "GetFactionRelation";
        case 470: return "GetThreatRatio";
        case 474: return "GetIsAlignment";
        case 490: return "IsProtected";
        case 500: return "GetWeaponAnimType";
        case 513: return "IsVoiceType";
        case 515: return "GetInCurrentLocFormList";
        case 528: return "IsUndead";
        case 533: return "GetIsUsedItemType";
        case 536: return "IsScenePlaying";
        case 546: return "GetQuestCompleted";
        case 550: return "IsBribeSuccess";
        case 551: return "IsIntimidateSuccess";
        case 552: return "IsPersuadeSuccess";
        case 560: return "HasKeyword";
        case 561: return "HasRefType";
        case 562: return "LocationHasKeyword";
        case 563: return "LocationHasRefType";
        case 565: return "GetIsEditorLocationAlias";
        case 566: return "GetIsAliasRef";
        case 567: return "GetIsEditorLocAlias";
        case 584: return "GetInContainer";
        case 589: return "GetLocationCleared";
        case 595: return "HasAssociationType";
        case 596: return "HasFamilyRelationship";
        case 600: return "HasParentRelationship";
        case 610: return "GetWithinDistance";
        case 611: return "GetDistance";
        case 629: return "GetVMQuestVariable";
        case 630: return "GetVMScriptVariable";
        case 639: return "GetInSharedCrimeFaction";
        case 644: return "GetKeywordDataForCurrentLocation";
        case 654: return "HasSpell";
        case 655: return "HasShout";
        case 672: return "GetAttackState";
        case 681: return "GetHasNote";
        case 691: return "EPAlchemyGetMakingPoison";
        default:  return std::format("FuncID({})", rawId);
        }
    }

    void CollectGlobalsFromCondition(const RE::TESCondition& a_cond, std::vector<QuestGlobalVarInfo>& a_outGlobals) {
        auto addGlobal = [&](RE::TESGlobal* a_glob) {
            if (!a_glob) {
                return;
            }
            bool exists = std::any_of(a_outGlobals.begin(), a_outGlobals.end(), [&](const QuestGlobalVarInfo& g) {
                return g.formID == a_glob->GetFormID();
                });
            if (!exists) {
                QuestGlobalVarInfo info{};
                info.globalForm = a_glob;
                info.formID = a_glob->GetFormID();
                info.editorID = a_glob->GetFormEditorID() ? a_glob->GetFormEditorID() : std::format("Global_0x{:08X}", a_glob->GetFormID());
                info.currentValue = a_glob->value;
                a_outGlobals.push_back(std::move(info));
            }
            };

        for (auto* node = a_cond.head; node != nullptr; node = node->next) {
            if (node->data.flags.global && node->data.comparisonValue.g) {
                addGlobal(node->data.comparisonValue.g);
            }
            if (node->data.functionData.function.get() == RE::FUNCTION_DATA::FunctionID::kGetGlobalValue) {
                if (auto* form = static_cast<RE::TESForm*>(node->data.functionData.params[0])) {
                    if (auto* glob = form->As<RE::TESGlobal>()) {
                        addGlobal(glob);
                    }
                }
            }
        }
    }
}

RE::TESObjectREFR* QuestManager::ResolveStaticAliasReference(RE::TESQuest* a_quest, RE::BGSBaseAlias* a_alias, int a_depth) {
    if (!a_quest || !a_alias || a_depth > 4) {
        return nullptr;
    }

    auto resolveAliasByID = [&](std::uint32_t a_targetAliasID) -> RE::TESObjectREFR* {
        {
            RE::BSReadLockGuard lock(a_quest->aliasAccessLock);
            auto it = a_quest->refAliasMap.find(a_targetAliasID);
            if (it != a_quest->refAliasMap.end()) {
                if (auto ptr = it->second.get()) {
                    return ptr.get();
                }
            }
        }
        for (auto* other : a_quest->aliases) {
            if (other && other != a_alias && other->aliasID == a_targetAliasID) {
                return ResolveStaticAliasReference(a_quest, other, a_depth + 1);
            }
        }
        return nullptr;
        };

    if (auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(a_alias)) {
        using FT = RE::BGSBaseAlias::FILL_TYPE;
        switch (refAlias->fillType.get()) {
        case FT::kForced: {
            if (auto forcedPtr = refAlias->fillData.forced.forcedRef.get()) {
                return forcedPtr.get();
            }
            break;
        }
        case FT::kUniqueActor: {
            if (auto* npc = refAlias->fillData.uniqueActor.uniqueActor) {
                if (auto* actorRef = FindActorReferenceByBaseNPC(npc)) {
                    return actorRef;
                }
            }
            break;
        }
        case FT::kCreated: {
            std::uint16_t targetAliasIdx = refAlias->fillData.created.alias.alias;
            if (targetAliasIdx < 0x8000) {
                if (auto* containerRef = resolveAliasByID(targetAliasIdx)) {
                    return containerRef;
                }
            }
            if (auto* createdObj = refAlias->fillData.created.object) {
                if (auto* existingRef = FindPlacedReferenceByBaseForm(createdObj)) {
                    return existingRef;
                }
            }
            break;
        }
        case FT::kFromAlias: {
            if (auto* parentRef = resolveAliasByID(refAlias->fillData.fromAlias.forcedFromAlias)) {
                return parentRef;
            }
            break;
        }
        case FT::kConditions:
        case FT::kFromEvent:
        case FT::kFromExternal:
        default:
            break;
        }

        if (refAlias->conditions && refAlias->conditions->head) {
            if (auto* condRef = ResolveFromConditionChain(*refAlias->conditions)) {
                return condRef;
            }
        }
    }
    else if (auto* locAlias = skyrim_cast<RE::BGSLocAlias*>(a_alias)) {
        if (locAlias->conditions && locAlias->conditions->head) {
            if (auto* condRef = ResolveFromConditionChain(*locAlias->conditions)) {
                return condRef;
            }
        }
    }
    else if (auto* locAlias = skyrim_cast<RE::BGSLocAlias*>(a_alias)) {
        if (locAlias->conditions && locAlias->conditions->head) {
            if (auto* condRef = ResolveFromConditionChain(*locAlias->conditions)) {
                return condRef;
            }
        }
    }

    return nullptr;
}

void QuestManager::ForceFillQuestAliases(RE::TESQuest* a_quest) {
    if (!a_quest) {
        return;
    }

    for (auto* alias : a_quest->aliases) {
        if (!alias) {
            continue;
        }
        // Make all aliases optional so Skyrim never aborts quest start due to missing previous quest states
        alias->flags.set(RE::BGSBaseAlias::FLAGS::kOptional);

        RE::ObjectRefHandle existingHandle;
        {
            RE::BSReadLockGuard lock(a_quest->aliasAccessLock);
            auto it = a_quest->refAliasMap.find(alias->aliasID);
            if (it != a_quest->refAliasMap.end()) {
                existingHandle = it->second;
            }
        }

        if (!existingHandle.get()) {
            if (RE::TESObjectREFR* staticRef = ResolveStaticAliasReference(a_quest, alias)) {
                RE::ObjectRefHandle newHandle(staticRef);
                RE::BSWriteLockGuard writeLock(a_quest->aliasAccessLock);
                auto it = a_quest->refAliasMap.find(alias->aliasID);
                if (it != a_quest->refAliasMap.end()) {
                    it->second = newHandle;
                }
                else {
                    a_quest->refAliasMap.emplace(alias->aliasID, newHandle);
                }
            }
        }
    }
}

void QuestManager::EnsureQuestTargetMarkersEnabled(RE::TESQuest* a_quest) {
    if (!a_quest) {
        return;
    }

    ForceFillQuestAliases(a_quest);

    for (auto* obj : a_quest->objectives) {
        if (!obj || !obj->targets || obj->numTargets == 0) {
            continue;
        }

        for (std::uint32_t i = 0; i < obj->numTargets; ++i) {
            auto* target = obj->targets[i];
            if (!target) {
                continue;
            }

            RE::ObjectRefHandle refHandle;
            {
                RE::BSReadLockGuard aliasLock(a_quest->aliasAccessLock);
                auto it = a_quest->refAliasMap.find(target->alias);
                if (it != a_quest->refAliasMap.end()) {
                    refHandle = it->second;
                }
            }

            auto targetRefPtr = refHandle.get();
            if (targetRefPtr) {
                if (targetRefPtr->IsDisabled()) {
                    targetRefPtr->Enable(false);
                    CallNativeEnableObjectReference(targetRefPtr.get());
                }

                if (targetRefPtr->extraList.HasType(RE::ExtraDataType::kMapMarker)) {
                    CallNativeAddToMap(targetRefPtr.get());
                }
            }
        }
    }
}

// Schaltet die Quest im Journal auf WeiÃŸ & Aktiv, OHNE bereits sichtbare Objectives erneut auf dem HUD aufzublenden!
void QuestManager::ForceQuestWhiteInJournal(RE::TESQuest* a_quest) {
    if (!a_quest) {
        return;
    }

    a_quest->data.flags.reset(RE::QuestFlag::kCompleted);
    a_quest->data.flags.set(
        RE::QuestFlag::kEnabled,
        RE::QuestFlag::kActive,
        RE::QuestFlag::kDisplayedInHUD
    );

    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
        auto& playerObjectives = player->GetPlayerRuntimeData().objectives;
        for (auto& objInst : playerObjectives) {
            if (objInst.Objective && objInst.Objective->ownerQuest == a_quest) {
                if (objInst.Objective->state.any(RE::QUEST_OBJECTIVE_STATE::kDisplayed) &&
                    objInst.Objective->state.none(RE::QUEST_OBJECTIVE_STATE::kCompleted, RE::QUEST_OBJECTIVE_STATE::kFailed)) {
                    objInst.InstanceState = RE::QUEST_OBJECTIVE_STATE::kDisplayed;
                }
            }
        }
    }

    CallNativeSetQuestActive(a_quest, true);
}

RE::TESObjectREFR* QuestManager::FindBestTeleportTarget(RE::TESQuest* a_quest) {
    if (!a_quest) {
        return nullptr;
    }

    auto resolveAlias = [&](std::uint32_t a_aliasID) -> RE::TESObjectREFR* {
        RE::ObjectRefHandle refHandle;
        {
            RE::BSReadLockGuard aliasLock(a_quest->aliasAccessLock);
            auto it = a_quest->refAliasMap.find(a_aliasID);
            if (it != a_quest->refAliasMap.end()) {
                refHandle = it->second;
            }
        }
        if (auto ptr = refHandle.get()) {
            return ptr.get();
        }
        for (auto* al : a_quest->aliases) {
            if (al && al->aliasID == a_aliasID) {
                return ResolveStaticAliasReference(a_quest, al);
            }
        }
        return nullptr;
        };

    for (auto* obj : a_quest->objectives) {
        if (!obj || !obj->targets || obj->numTargets == 0) {
            continue;
        }
        if (obj->state.any(RE::QUEST_OBJECTIVE_STATE::kDisplayed) &&
            obj->state.none(RE::QUEST_OBJECTIVE_STATE::kCompleted, RE::QUEST_OBJECTIVE_STATE::kFailed)) {
            for (std::uint32_t i = 0; i < obj->numTargets; ++i) {
                if (obj->targets[i]) {
                    if (auto* ref = resolveAlias(obj->targets[i]->alias)) {
                        return ref;
                    }
                }
            }
        }
    }

    for (auto* obj : a_quest->objectives) {
        if (!obj || !obj->targets || obj->numTargets == 0) {
            continue;
        }
        for (std::uint32_t i = 0; i < obj->numTargets; ++i) {
            if (obj->targets[i]) {
                if (auto* ref = resolveAlias(obj->targets[i]->alias)) {
                    return ref;
                }
            }
        }
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    {
        RE::BSReadLockGuard aliasLock(a_quest->aliasAccessLock);
        for (const auto& [aliasID, handle] : a_quest->refAliasMap) {
            auto ptr = handle.get();
            if (ptr && ptr.get() != player) {
                return ptr.get();
            }
        }
    }

    for (auto* al : a_quest->aliases) {
        if (auto* ref = ResolveStaticAliasReference(a_quest, al)) {
            if (ref != player) {
                return ref;
            }
        }
    }

    if (a_quest->objConditions.head) {
        if (auto* condRef = ResolveFromConditionChain(a_quest->objConditions)) {
            if (condRef != player) {
                return condRef;
            }
        }
    }

    return nullptr;
}

void QuestManager::TeleportToQuestTarget(RE::TESQuest* a_quest, const std::string& a_editorID) {
    if (!a_quest) {
        return;
    }

    m_statusMessage = std::format("Searching target marker for quest '{}'...", a_editorID);

    SKSE::GetTaskInterface()->AddTask([this, a_quest, a_editorID]() {
        EnsureQuestTargetMarkersEnabled(a_quest);
        RE::TESObjectREFR* targetRef = FindBestTeleportTarget(a_quest);

        if (!targetRef) {
            m_statusMessage = std::format("Could not find an active 3D target marker/alias for quest '{}'.", a_editorID);
            return;
        }

        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            D3DHook::SetMenuOpen(false);
            player->MoveTo(targetRef);
            m_statusMessage = std::format("Teleported to quest target (0x{:08X}) for '{}'!", targetRef->GetFormID(), a_editorID);
        }
        });
}

void QuestManager::TeleportToSpecificAlias(RE::TESQuest* a_quest, std::uint32_t a_aliasID, const std::string& a_aliasName) {
    if (!a_quest) {
        return;
    }

    m_statusMessage = std::format("Teleporting to alias '{}' (ID {})...", a_aliasName, a_aliasID);

    SKSE::GetTaskInterface()->AddTask([this, a_quest, a_aliasID, a_aliasName]() {
        RE::TESObjectREFR* targetRef = nullptr;

        RE::ObjectRefHandle refHandle;
        {
            RE::BSReadLockGuard aliasLock(a_quest->aliasAccessLock);
            auto it = a_quest->refAliasMap.find(a_aliasID);
            if (it != a_quest->refAliasMap.end()) {
                refHandle = it->second;
            }
        }

        if (auto targetRefPtr = refHandle.get()) {
            targetRef = targetRefPtr.get();
        }
        else {
            for (auto* al : a_quest->aliases) {
                if (al && al->aliasID == a_aliasID) {
                    targetRef = ResolveStaticAliasReference(a_quest, al);
                    break;
                }
            }
        }

        // If still unbound (e.g. pure dynamic radiant alias on an unstarted quest), try force-filling aliases or using best quest target
        if (!targetRef) {
            ForceFillQuestAliases(a_quest);
            {
                RE::BSReadLockGuard aliasLock(a_quest->aliasAccessLock);
                auto it = a_quest->refAliasMap.find(a_aliasID);
                if (it != a_quest->refAliasMap.end()) {
                    if (auto ptr = it->second.get()) {
                        targetRef = ptr.get();
                    }
                }
            }
            if (!targetRef) {
                targetRef = FindBestTeleportTarget(a_quest);
            }
        }

        if (!targetRef) {
            m_statusMessage = std::format("Alias '{}' (ID {}) has no bound or static 3D reference.", a_aliasName, a_aliasID);
            return;
        }

        if (targetRef->IsDisabled()) {
            targetRef->Enable(false);
            CallNativeEnableObjectReference(targetRef);
        }

        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            D3DHook::SetMenuOpen(false);
            player->MoveTo(targetRef);
            m_statusMessage = std::format("Teleported to alias '{}' (Ref 0x{:08X})!", a_aliasName, targetRef->GetFormID());
        }
        });
}

std::string QuestManager::GetObjectiveStatusString(const RE::BGSQuestObjective* a_obj) {
    if (!a_obj) {
        return "Unknown";
    }
    if (a_obj->state.any(RE::QUEST_OBJECTIVE_STATE::kCompleted) || a_obj->state.any(RE::QUEST_OBJECTIVE_STATE::kCompletedDisplayed)) {
        return "Completed";
    }
    if (a_obj->state.any(RE::QUEST_OBJECTIVE_STATE::kFailed) || a_obj->state.any(RE::QUEST_OBJECTIVE_STATE::kFailedDisplayed)) {
        return "Failed";
    }
    if (a_obj->state.any(RE::QUEST_OBJECTIVE_STATE::kDisplayed)) {
        return "Displayed (Active)";
    }
    return "Dormant / Hidden";
}

bool QuestManager::HasActiveJournalObjectives(RE::TESQuest* a_quest) {
    if (!a_quest) {
        return false;
    }

    bool hasDisplayed = false;
    bool hasUnfinishedDisplayed = false;

    for (const auto* obj : a_quest->objectives) {
        if (!obj) {
            continue;
        }
        if (obj->state.any(RE::QUEST_OBJECTIVE_STATE::kDisplayed)) {
            hasDisplayed = true;
            if (obj->state.none(
                RE::QUEST_OBJECTIVE_STATE::kCompleted,
                RE::QUEST_OBJECTIVE_STATE::kCompletedDisplayed,
                RE::QUEST_OBJECTIVE_STATE::kFailed,
                RE::QUEST_OBJECTIVE_STATE::kFailedDisplayed)) {
                hasUnfinishedDisplayed = true;
            }
        }
    }

    return hasDisplayed && (hasUnfinishedDisplayed || !a_quest->IsCompleted());
}

void QuestManager::ForceDisplayObjectiveInJournal(RE::TESQuest* a_quest, RE::BGSQuestObjective* a_obj) {
    if (!a_quest || !a_obj) {
        return;
    }

    a_quest->data.flags.reset(RE::QuestFlag::kCompleted);
    a_quest->data.flags.set(
        RE::QuestFlag::kEnabled,
        RE::QuestFlag::kActive,
        RE::QuestFlag::kDisplayedInHUD
    );

    bool wasAlreadyDisplayed = a_obj->state.any(RE::QUEST_OBJECTIVE_STATE::kDisplayed) &&
        a_obj->state.none(RE::QUEST_OBJECTIVE_STATE::kCompleted, RE::QUEST_OBJECTIVE_STATE::kFailed);

    a_obj->state.reset(
        RE::QUEST_OBJECTIVE_STATE::kCompleted,
        RE::QUEST_OBJECTIVE_STATE::kCompletedDisplayed,
        RE::QUEST_OBJECTIVE_STATE::kFailed,
        RE::QUEST_OBJECTIVE_STATE::kFailedDisplayed
    );
    a_obj->state.set(RE::QUEST_OBJECTIVE_STATE::kDisplayed);

    EnsureQuestTargetMarkersEnabled(a_quest);

    if (!wasAlreadyDisplayed) {
        // Trigger HUD objective notification only ONCE if it wasn't already displayed by the engine
        CallNativeSetObjectiveDisplayed(a_quest, a_obj->index, true, true);
    }
    ForceQuestWhiteInJournal(a_quest);
}

std::vector<QuestStageInfo> QuestManager::ExtractAllStages(RE::TESQuest* a_quest, bool a_isActuallyStarted) {
    std::vector<QuestStageInfo> allStages;
    if (!a_quest) {
        return allStages;
    }

    std::uint16_t minObjectiveIdx = 0xFFFF;
    for (const auto* obj : a_quest->objectives) {
        if (obj && obj->index > 0 && obj->index < minObjectiveIdx) {
            minObjectiveIdx = obj->index;
        }
    }

    auto isTechnicalSetupStage = [&](std::uint16_t a_idx) -> bool {
        if (minObjectiveIdx != 0xFFFF) {
            return a_idx < minObjectiveIdx && a_idx < 10;
        }
        return a_idx < 10;
        };

    std::uint16_t curStage = a_quest->GetCurrentStageID();

    auto addUniqueStage = [&](std::uint16_t a_stageIndex) {
        bool alreadyInList = std::any_of(allStages.begin(), allStages.end(), [&](const QuestStageInfo& info) {
            return info.index == a_stageIndex;
            });
        if (!alreadyInList) {
            bool isDone = a_isActuallyStarted && (a_stageIndex <= curStage);
            allStages.push_back({ a_stageIndex, isDone, isTechnicalSetupStage(a_stageIndex) });
        }
        };

    if (a_quest->executedStages) {
        for (const auto& stage : *a_quest->executedStages) {
            addUniqueStage(stage.data.index);
        }
    }

    if (a_quest->waitingStages) {
        for (const auto* stage : *a_quest->waitingStages) {
            if (stage) {
                addUniqueStage(stage->data.index);
            }
        }
    }

    std::sort(allStages.begin(), allStages.end(), [](const QuestStageInfo& a, const QuestStageInfo& b) {
        return a.index < b.index;
        });

    if (allStages.size() == 1) {
        allStages[0].isSetupStage = false;
    }

    return allStages;
}

std::vector<QuestAliasInfo> QuestManager::ExtractQuestAliases(RE::TESQuest* a_quest) {
    std::vector<QuestAliasInfo> result;
    if (!a_quest) {
        return result;
    }

    auto describeRef = [](RE::TESObjectREFR* a_ref) -> std::string {
        if (!a_ref) {
            return "Unnamed Reference";
        }
        std::string rName = a_ref->GetDisplayFullName() ? a_ref->GetDisplayFullName() : "";
        if (rName.empty() && a_ref->GetBaseObject() && a_ref->GetBaseObject()->GetName()) {
            rName = a_ref->GetBaseObject()->GetName();
        }
        if (rName.empty() && a_ref->GetFormEditorID()) {
            rName = a_ref->GetFormEditorID();
        }
        if (rName.empty() && a_ref->GetBaseObject() && a_ref->GetBaseObject()->GetFormEditorID()) {
            rName = a_ref->GetBaseObject()->GetFormEditorID();
        }
        return rName.empty() ? "Unnamed Reference" : rName;
        };

    RE::TESObjectREFR* questBestFallback = FindBestTeleportTarget(a_quest);

    for (auto* alias : a_quest->aliases) {
        if (!alias) {
            continue;
        }

        QuestAliasInfo info{};
        info.aliasID = alias->aliasID;
        info.aliasName = alias->aliasName.empty() ? std::format("Alias #{}", alias->aliasID) : std::string(alias->aliasName.c_str());

        RE::ObjectRefHandle handle;
        {
            RE::BSReadLockGuard lock(a_quest->aliasAccessLock);
            auto it = a_quest->refAliasMap.find(alias->aliasID);
            if (it != a_quest->refAliasMap.end()) {
                handle = it->second;
            }
        }

        if (auto refPtr = handle.get()) {
            info.isBound = true;
            info.isStaticFallback = false;
            info.refFormID = refPtr->GetFormID();
            info.refName = describeRef(refPtr.get());
        }
        else if (RE::TESObjectREFR* staticRef = ResolveStaticAliasReference(a_quest, alias)) {
            info.isBound = true;
            info.isStaticFallback = true;
            info.refFormID = staticRef->GetFormID();
            info.refName = std::format("{} [Static Target]", describeRef(staticRef));
        }
        else if (questBestFallback) {
            info.isBound = true;
            info.isStaticFallback = true;
            info.refFormID = questBestFallback->GetFormID();
            info.refName = std::format("{} [Quest Target Fallback]", describeRef(questBestFallback));
        }
        else {
            info.isBound = false;
            info.isStaticFallback = false;
            info.refName = "Unbound (Dynamic / Radiant)";
        }

        result.push_back(std::move(info));
    }

    {
        RE::BSReadLockGuard lock(a_quest->aliasAccessLock);
        for (const auto& [aliasID, handle] : a_quest->refAliasMap) {
            bool exists = std::any_of(result.begin(), result.end(), [&](const QuestAliasInfo& a) {
                return a.aliasID == aliasID;
                });
            if (!exists) {
                QuestAliasInfo info{};
                info.aliasID = aliasID;
                info.aliasName = std::format("Alias #{}", aliasID);
                if (auto refPtr = handle.get()) {
                    info.isBound = true;
                    info.refFormID = refPtr->GetFormID();
                    info.refName = describeRef(refPtr.get());
                }
                else if (questBestFallback) {
                    info.isBound = true;
                    info.isStaticFallback = true;
                    info.refFormID = questBestFallback->GetFormID();
                    info.refName = std::format("{} [Quest Target Fallback]", describeRef(questBestFallback));
                }
                else {
                    info.isBound = false;
                    info.refName = "Unbound";
                }
                result.push_back(std::move(info));
            }
        }
    }

    std::sort(result.begin(), result.end(), [](const QuestAliasInfo& a, const QuestAliasInfo& b) {
        return a.aliasID < b.aliasID;
        });

    return result;
}

void QuestManager::ExtractQuestItemsAndConditions(RE::TESQuest* a_quest, const QuestEntry& a_entry, std::vector<QuestItemLink>& a_outLinks) {
    if (!a_quest) {
        return;
    }

    auto addUniqueLink = [&](QuestItemLink::LinkType a_type, RE::TESForm* a_form, const std::string& a_details) {
        if (!a_form) {
            return;
        }
        std::string typeStr;
        std::string desc = DescribeForm(a_form, typeStr);
        if (desc.empty()) {
            return;
        }

        bool duplicate = std::any_of(a_outLinks.begin(), a_outLinks.end(), [&](const QuestItemLink& existing) {
            return existing.questFormID == a_entry.formID &&
                existing.itemFormID == a_form->GetFormID() &&
                existing.type == a_type;
            });
        if (duplicate) {
            return;
        }

        QuestItemLink link{};
        link.type = a_type;
        link.itemName = std::move(desc);
        link.itemTypeStr = std::move(typeStr);
        link.itemFormID = a_form->GetFormID();
        link.conditionDetails = a_details;
        link.questFormID = a_entry.formID;
        link.questName = a_entry.name.empty() ? a_entry.editorID : a_entry.name;
        link.questEditorID = a_entry.editorID;
        link.sourcePlugin = a_entry.sourcePlugin;
        a_outLinks.push_back(std::move(link));
        };

    auto inspectConditionChain = [&](const RE::TESCondition& a_cond, const std::string& a_contextLabel, bool a_isStartCondition) {
        for (auto* node = a_cond.head; node != nullptr; node = node->next) {
            auto fn = node->data.functionData.function.get();
            void* param1 = node->data.functionData.params[0];
            if (!param1) {
                continue;
            }

            switch (fn) {
            case RE::FUNCTION_DATA::FunctionID::kGetItemCount:
            case RE::FUNCTION_DATA::FunctionID::kGetEquipped:
            case RE::FUNCTION_DATA::FunctionID::kHasSpell:
            case RE::FUNCTION_DATA::FunctionID::kHasPerk:
            case RE::FUNCTION_DATA::FunctionID::kGetInFaction:
            case RE::FUNCTION_DATA::FunctionID::kGetGlobalValue:
            case RE::FUNCTION_DATA::FunctionID::kGetStageDone:
            case RE::FUNCTION_DATA::FunctionID::kGetStage:
            case RE::FUNCTION_DATA::FunctionID::kGetQuestCompleted:
            case RE::FUNCTION_DATA::FunctionID::kGetDeadCount:
            case RE::FUNCTION_DATA::FunctionID::kHasKeyword: {
                auto* form = static_cast<RE::TESForm*>(param1);
                if (form && form->GetFormID() != 0) {
                    auto linkType = (!a_isStartCondition && IsInventoryOrRewardFormType(form->GetFormType()))
                        ? QuestItemLink::LinkType::kRewardOrItem
                        : QuestItemLink::LinkType::kPrerequisite;
                    addUniqueLink(linkType, form, a_contextLabel);
                }
                break;
            }
            default:
                break;
            }
        }
        };

    // 1. Quest Start & Event Conditions (Prerequisites)
    inspectConditionChain(a_quest->objConditions, "Quest Start Condition (objConditions)", true);

    // 2. Quest Aliases (Created Items / Unique Actors / Forced References)
    for (auto* alias : a_quest->aliases) {
        if (!alias) {
            continue;
        }
        if (auto* refAlias = skyrim_cast<RE::BGSRefAlias*>(alias)) {
            std::string aliasLabel = refAlias->aliasName.empty()
                ? std::format("Alias #{}", refAlias->aliasID)
                : std::string(refAlias->aliasName.c_str());

            if (refAlias->fillType == RE::BGSBaseAlias::FILL_TYPE::kCreated && refAlias->fillData.created.object) {
                addUniqueLink(
                    QuestItemLink::LinkType::kRewardOrItem,
                    refAlias->fillData.created.object,
                    std::format("Created by Quest Alias '{}'", aliasLabel)
                );
            }
            else if (refAlias->fillType == RE::BGSBaseAlias::FILL_TYPE::kUniqueActor && refAlias->fillData.uniqueActor.uniqueActor) {
                addUniqueLink(
                    QuestItemLink::LinkType::kPrerequisite,
                    refAlias->fillData.uniqueActor.uniqueActor,
                    std::format("Required Unique Actor in Alias '{}'", aliasLabel)
                );
            }
            else if (refAlias->fillType == RE::BGSBaseAlias::FILL_TYPE::kForced) {
                if (auto forcedRef = refAlias->fillData.forced.forcedRef.get()) {
                    if (auto* baseObj = forcedRef->GetBaseObject()) {
                        auto linkType = IsInventoryOrRewardFormType(baseObj->GetFormType())
                            ? QuestItemLink::LinkType::kRewardOrItem
                            : QuestItemLink::LinkType::kPrerequisite;
                        addUniqueLink(linkType, baseObj, std::format("Forced Reference in Alias '{}'", aliasLabel));
                    }
                }
            }
        }
    }

    // 3. Currently Bound Alias References (e.g. Weapons, Books, Quest Items or Boss Chests)
    {
        RE::BSReadLockGuard lock(a_quest->aliasAccessLock);
        for (const auto& [aliasID, handle] : a_quest->refAliasMap) {
            if (auto refPtr = handle.get()) {
                if (auto* baseObj = refPtr->GetBaseObject()) {
                    if (IsInventoryOrRewardFormType(baseObj->GetFormType())) {
                        addUniqueLink(
                            QuestItemLink::LinkType::kRewardOrItem,
                            baseObj,
                            std::format("Bound Quest Item / Reward in Alias #{}", aliasID)
                        );
                    }
                }
            }
        }
    }
}

bool QuestManager::ApplyPapyrusVariableEdit(std::uint64_t a_vmHandle, const PapyrusVarLiveInfo& a_varInfo) {
    if (a_vmHandle == 0) {
        return false;
    }
    auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    if (!vm) {
        return false;
    }

    RE::BSSpinLockGuard vmLock(vm->attachedScriptsLock);
    auto it = vm->attachedScripts.find(static_cast<RE::VMHandle>(a_vmHandle));
    if (it == vm->attachedScripts.end()) {
        return false;
    }

    for (auto& scriptObj : it->second) {
        if (!scriptObj || !scriptObj->type) {
            continue;
        }
        if (a_varInfo.scriptName != scriptObj->type->GetName()) {
            continue;
        }
        std::uint32_t totalVars = scriptObj->type->GetTotalNumVariables();
        if (a_varInfo.rawIndex >= totalVars) {
            return false;
        }

        auto& dstVar = scriptObj->variables[a_varInfo.rawIndex];
        switch (a_varInfo.type) {
        case PapyrusVarLiveInfo::VarType::kInt:
            dstVar.SetSInt(a_varInfo.intVal);
            return true;
        case PapyrusVarLiveInfo::VarType::kFloat:
            dstVar.SetFloat(a_varInfo.floatVal);
            return true;
        case PapyrusVarLiveInfo::VarType::kBool:
            dstVar.SetBool(a_varInfo.boolVal);
            return true;
        case PapyrusVarLiveInfo::VarType::kString:
            dstVar.SetString(a_varInfo.strVal);
            return true;
        default:
            return false;
        }
    }
    return false;
}

QuestScriptStatus QuestManager::AnalyzeQuestScriptAndState(RE::TESQuest* a_quest, const QuestEntry& a_entry, bool a_loadDeepDetails) {
    QuestScriptStatus status{};
    status.questPtr = a_quest;
    status.questFormID = a_entry.formID;
    status.questName = a_entry.name.empty() ? a_entry.editorID : a_entry.name;
    status.questEditorID = a_entry.editorID;
    status.sourcePlugin = a_entry.sourcePlugin;
    status.questTypeName = a_entry.typeName;
    status.currentStage = a_entry.currentStage;
    status.isActiveInJournal = a_entry.isActiveInJournal;
    status.isBackgroundRunning = a_entry.isBackgroundRunning;
    status.isCompleted = a_entry.isCompleted;

    if (!a_quest) {
        return status;
    }

    status.isEngineRunning = a_quest->IsRunning();
    status.isWaitingToStart = a_quest->IsStarting();
    status.priority = a_quest->data.priority;
    status.rawFlags = a_quest->data.flags.underlying();
    status.totalAliases = a_quest->aliases.size();
    status.totalObjectives = a_quest->objectives.size();

    // Fast VM script name check
    if (auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton()) {
        if (auto* policy = vm->GetObjectHandlePolicy()) {
            RE::VMHandle handle = policy->GetHandleForObject(RE::FormType::Quest, a_quest);
            if (handle != policy->EmptyHandle()) {
                status.vmHandle = static_cast<std::uint64_t>(handle);
                RE::BSSpinLockGuard vmLock(vm->attachedScriptsLock);
                auto it = vm->attachedScripts.find(handle);
                if (it != vm->attachedScripts.end()) {
                    for (const auto& scriptPtr : it->second) {
                        if (scriptPtr && scriptPtr->type) {
                            status.attachedScripts.emplace_back(scriptPtr->type->GetName());
                        }
                    }
                }
            }
        }
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    bool startConditionsMet = true;
    if (a_quest->objConditions.head && player) {
        startConditionsMet = a_quest->objConditions.IsTrue(player, player);
    }

    if (!status.isEngineRunning && !a_entry.isCompleted) {
        if (!startConditionsMet) {
            status.isBlockedByConditions = true;
            status.blockReason = "Blocked: Quest start conditions (objConditions) evaluate to FALSE.";
        }
        else if (status.isWaitingToStart) {
            status.isBlockedByConditions = true;
            status.blockReason = "Waiting: Quest is in kStarting state (waiting for Alias fill or Story Manager).";
        }
        else {
            status.blockReason = "Dormant: Start conditions met, waiting for trigger event / dialogue / script.";
        }
    }
    else if (status.isEngineRunning) {
        if (!startConditionsMet) {
            status.blockReason = "Running (Note: Initial start conditions currently evaluate to FALSE).";
        }
        else {
            status.blockReason = status.attachedScripts.empty()
                ? "Running normally (No Papyrus scripts attached)."
                : std::format("Active: {} Papyrus script(s) bound & listening for events.", status.attachedScripts.size());
        }
    }
    else if (a_entry.isCompleted) {
        status.blockReason = "Completed: Quest has finished execution.";
    }

    if (a_loadDeepDetails) {
        PopulateScriptDeepDetails(status);
    }

    return status;
}

void QuestManager::PopulateScriptDeepDetails(QuestScriptStatus& status) {
    if (status.deepDetailsLoaded || !status.questPtr) {
        return;
    }
    status.deepDetailsLoaded = true;
    RE::TESQuest* a_quest = status.questPtr;

    std::vector<std::string> flagNames;
    if (a_quest->data.flags.any(RE::QuestFlag::kEnabled))        flagNames.emplace_back("Enabled");
    if (a_quest->data.flags.any(RE::QuestFlag::kCompleted))      flagNames.emplace_back("Completed");
    if (a_quest->data.flags.any(RE::QuestFlag::kAddIdleToHello)) flagNames.emplace_back("AddIdleToHello");
    if (a_quest->data.flags.any(RE::QuestFlag::kAllowRepeatStages)) flagNames.emplace_back("AllowRepeatStages");
    if (a_quest->data.flags.any(RE::QuestFlag::kStartsEnabled))  flagNames.emplace_back("StartsEnabled");
    if (a_quest->data.flags.any(RE::QuestFlag::kDisplayedInHUD)) flagNames.emplace_back("DisplayedInHUD");
    if (a_quest->data.flags.any(RE::QuestFlag::kFailed))         flagNames.emplace_back("Failed");
    if (a_quest->data.flags.any(RE::QuestFlag::kStageWait))      flagNames.emplace_back("StageWait");
    if (a_quest->data.flags.any(RE::QuestFlag::kRunOnce))        flagNames.emplace_back("RunOnce");
    if (a_quest->data.flags.any(RE::QuestFlag::kExcludeFromExport)) flagNames.emplace_back("ExcludeFromExport");
    if (a_quest->data.flags.any(RE::QuestFlag::kWarnOnAliasFillFailure)) flagNames.emplace_back("WarnOnAliasFillFailure");
    if (a_quest->data.flags.any(RE::QuestFlag::kActive))         flagNames.emplace_back("Active");

    if (flagNames.empty()) {
        status.flagsDescription = "None (0x0000)";
    }
    else {
        std::string joined;
        for (std::size_t i = 0; i < flagNames.size(); ++i) {
            joined += flagNames[i] + (i + 1 < flagNames.size() ? " | " : "");
        }
        status.flagsDescription = std::format("0x{:04X} ({})", status.rawFlags, joined);
    }

    const auto eventValue = a_quest->eventID;
    if (eventValue == RE::QuestEvent::kNone) {
        status.eventTypeStr = "None (Standard / Script / Dialogue Start)";
    }
    else {
        const auto ev = static_cast<std::uint32_t>(eventValue);
        std::string eventName;
        switch (eventValue) {
        case RE::QuestEvent::kKillActor:             eventName = "KillActor (KILL)"; break;
        case RE::QuestEvent::kAssaultActor:          eventName = "AssaultActor (ASSU)"; break;
        case RE::QuestEvent::kChangeLocation:        eventName = "ChangeLocation (CLOC)"; break;
        case RE::QuestEvent::kScript:                eventName = "Script (SCPT)"; break;
        case RE::QuestEvent::kActorDialogue:         eventName = "ActorDialogue (ADIA)"; break;
        case RE::QuestEvent::kActorHello:            eventName = "ActorHello (AHEL)"; break;
        case RE::QuestEvent::kActivateActor:         eventName = "ActivateActor (AFAV)"; break;
        case RE::QuestEvent::kPlayerAddItem:         eventName = "PlayerAddItem (AIPL)"; break;
        case RE::QuestEvent::kPlayerRemoveItem:      eventName = "PlayerRemoveItem (REMP)"; break;
        case RE::QuestEvent::kCraftItem:             eventName = "CraftItem (CRFT)"; break;
        case RE::QuestEvent::kPickLock:              eventName = "PickLock (LOCK)"; break;
        case RE::QuestEvent::kInfection:             eventName = "Infection (INFC)"; break;
        case RE::QuestEvent::kCure:                  eventName = "Cure (CURE)"; break;
        case RE::QuestEvent::kNewVoicePower:         eventName = "NewVoicePower (NVPE)"; break;
        case RE::QuestEvent::kDeadBody:              eventName = "DeadBody (DEAD)"; break;
        case RE::QuestEvent::kSkillIncrease:         eventName = "SkillIncrease (SKIL)"; break;
        case RE::QuestEvent::kIncreaseLevel:         eventName = "IncreaseLevel (LEVL)"; break;
        case RE::QuestEvent::kChangeRelationshipRank:eventName = "ChangeRelationshipRank (CHBR)"; break;
        case RE::QuestEvent::kIntimidateNPC:         eventName = "IntimidateNPC (INTM)"; break;
        case RE::QuestEvent::kBribeNPC:              eventName = "BribeNPC (BRIB)"; break;
        case RE::QuestEvent::kFlatterNPC:            eventName = "FlatterNPC (FLAT)"; break;
        case RE::QuestEvent::kPlayerGetsFavor:       eventName = "PlayerGetsFavor (PRFV)"; break;
        case RE::QuestEvent::kPayFine:               eventName = "PayFine (PFIN)"; break;
        default:                                     eventName = std::format("Unknown (0x{:08X})", ev); break;
        }
        status.eventTypeStr = std::format("Story Manager Event '{}' (0x{:08X})", eventName, ev);
    }

    status.boundOrResolvableAliases = 0;
    for (auto* al : a_quest->aliases) {
        if (!al) continue;
        bool bound = false;
        {
            RE::BSReadLockGuard lock(a_quest->aliasAccessLock);
            auto it = a_quest->refAliasMap.find(al->aliasID);
            if (it != a_quest->refAliasMap.end() && it->second.get()) {
                bound = true;
            }
        }
        if (!bound && ResolveStaticAliasReference(a_quest, al) != nullptr) {
            bound = true;
        }
        if (bound) {
            status.boundOrResolvableAliases++;
        }
    }

    CollectGlobalsFromCondition(a_quest->objConditions, status.referencedGlobals);
    for (auto* al : a_quest->aliases) {
        if (auto* refAl = skyrim_cast<RE::BGSRefAlias*>(al)) {
            if (refAl->conditions) {
                CollectGlobalsFromCondition(*refAl->conditions, status.referencedGlobals);
            }
        }
    }

    if (status.vmHandle != 0) {
        if (auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton()) {
            RE::BSSpinLockGuard vmLock(vm->attachedScriptsLock);
            auto it = vm->attachedScripts.find(static_cast<RE::VMHandle>(status.vmHandle));
            if (it != vm->attachedScripts.end()) {
                for (const auto& scriptPtr : it->second) {
                    if (scriptPtr && scriptPtr->type) {
                        std::string scName = scriptPtr->type->GetName();
                        std::vector<RE::BSScript::ObjectTypeInfo*> hierarchy;
                        for (auto* t = scriptPtr->type.get(); t != nullptr; t = t->GetParent()) {
                            hierarchy.push_back(t);
                        }
                        std::reverse(hierarchy.begin(), hierarchy.end());

                        std::uint32_t varBaseIdx = 0;
                        std::uint32_t totalVars = scriptPtr->type->GetTotalNumVariables();

                        for (auto* t : hierarchy) {
                            auto* varIter = t->GetVariableIter();
                            std::uint32_t numLocalVars = t->GetNumVariables();
                            for (std::uint32_t v = 0; v < numLocalVars; ++v) {
                                std::uint32_t rawIdx = varBaseIdx + v;
                                if (!varIter || rawIdx >= totalVars) {
                                    break;
                                }

                                PapyrusVarLiveInfo pVar{};
                                pVar.scriptName = scName;
                                pVar.varName = varIter[v].name.c_str();
                                pVar.rawIndex = rawIdx;

                                const auto& val = scriptPtr->variables[rawIdx];
                                if (val.IsInt()) {
                                    pVar.type = PapyrusVarLiveInfo::VarType::kInt;
                                    pVar.intVal = val.GetSInt();
                                    pVar.displayTypeStr = "Int";
                                }
                                else if (val.IsFloat()) {
                                    pVar.type = PapyrusVarLiveInfo::VarType::kFloat;
                                    pVar.floatVal = val.GetFloat();
                                    pVar.displayTypeStr = "Float";
                                }
                                else if (val.IsBool()) {
                                    pVar.type = PapyrusVarLiveInfo::VarType::kBool;
                                    pVar.boolVal = val.GetBool();
                                    pVar.displayTypeStr = "Bool";
                                }
                                else if (val.IsString()) {
                                    pVar.type = PapyrusVarLiveInfo::VarType::kString;
                                    pVar.strVal = val.GetString().data() ? val.GetString().data() : "";
                                    pVar.displayTypeStr = "String";
                                }
                                else if (val.IsObject()) {
                                    pVar.type = PapyrusVarLiveInfo::VarType::kObjectOrOther;
                                    pVar.displayTypeStr = "Object";
                                    #ifdef GetObject
                                    #undef GetObject
                                    #endif
                                    auto objPtr = val.GetObject();
                                    if (objPtr && objPtr->type) {
                                        std::string objTypeName = objPtr->type->GetName();
                                        std::uint64_t objHandle = static_cast<std::uint64_t>(objPtr->GetHandle());
                                        pVar.strVal = std::format("{} (Handle 0x{:X})", objTypeName, objHandle);

                                        if (ContainsCaseInsensitive(objTypeName, "GlobalVariable")) {
                                            RE::FormID globFormID = static_cast<RE::FormID>(objHandle & 0xFFFFFFFF);
                                            if (auto* globForm = RE::TESForm::LookupByID<RE::TESGlobal>(globFormID)) {
                                                pVar.type = PapyrusVarLiveInfo::VarType::kGlobalVar;
                                                pVar.globalForm = globForm;
                                                pVar.floatVal = globForm->value;
                                                pVar.displayTypeStr = "Global";
                                                pVar.strVal = std::format("0x{:08X}", globFormID);

                                                bool alreadyInGlobals = std::any_of(status.referencedGlobals.begin(), status.referencedGlobals.end(),
                                                    [&](const QuestGlobalVarInfo& g) { return g.formID == globFormID; });
                                                if (!alreadyInGlobals) {
                                                    QuestGlobalVarInfo gInfo{};
                                                    gInfo.globalForm = globForm;
                                                    gInfo.formID = globFormID;
                                                    const char* edid = globForm->GetFormEditorID();
                                                    gInfo.editorID = (edid && edid[0] != '\0') ? edid : pVar.varName;
                                                    gInfo.currentValue = globForm->value;
                                                    status.referencedGlobals.push_back(std::move(gInfo));
                                                }
                                            }
                                        }
                                    }
                                    else {
                                        pVar.strVal = "None (Null Object)";
                                    }
                                }
                                else {
                                    pVar.type = PapyrusVarLiveInfo::VarType::kObjectOrOther;
                                    pVar.strVal = "None / Array";
                                    pVar.displayTypeStr = "Other";
                                }

                                status.papyrusVars.push_back(std::move(pVar));
                            }
                            varBaseIdx += numLocalVars;
                        }
                    }
                }
            }
        }
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (a_quest->objConditions.head && player) {
        for (auto* node = a_quest->objConditions.head; node != nullptr; node = node->next) {
            QuestConditionNodeInfo cInfo{};
            cInfo.rawNode = node;
            auto fn = node->data.functionData.function.get();
            cInfo.functionName = GetConditionFunctionName(fn);
            cInfo.opStr = GetComparisonOperatorString(node->data.flags.opCode);
            cInfo.isOR = node->data.flags.isOR;

            RE::TESObjectREFR* subj = player;
            if (auto runRef = node->data.runOnRef.get()) {
                subj = runRef.get();
            }
            RE::ConditionCheckParams checkParams(subj, player);
            cInfo.evaluatesTrue = node->IsTrue(checkParams);

            void* param1 = node->data.functionData.params[0];
            void* param2 = node->data.functionData.params[1];
            float compVal = node->data.flags.global && node->data.comparisonValue.g
                ? node->data.comparisonValue.g->value
                : node->data.comparisonValue.f;

            const auto rawId = static_cast<std::uint16_t>(fn);
            if (rawId == 629 || rawId == 630) { // GetVMQuestVariable / GetVMScriptVariable
                auto* form = static_cast<RE::TESForm*>(param1);
                std::string dummyType;
                std::string formDesc = DescribeForm(form, dummyType);
                const char* varName = param2 ? reinterpret_cast<const char*>(&param2) : "";
                if (param2 && !std::isprint(static_cast<unsigned char>(*reinterpret_cast<const char*>(&param2)))) {
                    varName = static_cast<const char*>(param2);
                }
                cInfo.paramSummary = std::format("{} [Var: {}] {} {:.1f}",
                    formDesc, (varName && varName[0]) ? varName : "VMVar", cInfo.opStr, compVal);
            }
            else if (param1) {
                switch (fn) {
                case RE::FUNCTION_DATA::FunctionID::kGetItemCount:
                case RE::FUNCTION_DATA::FunctionID::kGetEquipped:
                case RE::FUNCTION_DATA::FunctionID::kHasSpell:
                case RE::FUNCTION_DATA::FunctionID::kHasPerk:
                case RE::FUNCTION_DATA::FunctionID::kGetInFaction:
                case RE::FUNCTION_DATA::FunctionID::kGetPCInFaction:
                case RE::FUNCTION_DATA::FunctionID::kGetGlobalValue:
                case RE::FUNCTION_DATA::FunctionID::kGetStageDone:
                case RE::FUNCTION_DATA::FunctionID::kGetStage:
                case RE::FUNCTION_DATA::FunctionID::kGetQuestCompleted:
                case RE::FUNCTION_DATA::FunctionID::kGetQuestRunning:
                case RE::FUNCTION_DATA::FunctionID::kGetDeadCount:
                case RE::FUNCTION_DATA::FunctionID::kHasKeyword:
                case RE::FUNCTION_DATA::FunctionID::kGetIsID:
                case RE::FUNCTION_DATA::FunctionID::kGetIsRace:
                case RE::FUNCTION_DATA::FunctionID::kGetPCIsRace:
                case RE::FUNCTION_DATA::FunctionID::kGetInCurrentLoc: {
                    auto* form = static_cast<RE::TESForm*>(param1);
                    std::string dummyType;
                    std::string formDesc = DescribeForm(form, dummyType);
                    if (fn == RE::FUNCTION_DATA::FunctionID::kGetStageDone) {
                        auto stageIdx = static_cast<std::uint16_t>(reinterpret_cast<std::uintptr_t>(param2) & 0xFFFF);
                        cInfo.paramSummary = std::format("{} [Stage {}] {} {:.1f}", formDesc, stageIdx, cInfo.opStr, compVal);
                    }
                    else {
                        cInfo.paramSummary = std::format("{} (0x{:08X}) {} {:.1f}", formDesc, form ? form->GetFormID() : 0, cInfo.opStr, compVal);
                    }
                    break;
                }
                default:
                    cInfo.paramSummary = std::format("(Param: 0x{:X}) {} {:.1f}", reinterpret_cast<std::uintptr_t>(param1), cInfo.opStr, compVal);
                    break;
                }
            }
            else {
                cInfo.paramSummary = std::format("{} {:.1f}", cInfo.opStr, compVal);
            }

            switch (fn) {
            case RE::FUNCTION_DATA::FunctionID::kGetStage:
                if (auto* q = static_cast<RE::TESForm*>(param1) ? static_cast<RE::TESForm*>(param1)->As<RE::TESQuest>() : nullptr) {
                    cInfo.currentValueStr = std::format("Stage {}", q->GetCurrentStageID());
                }
                break;
            case RE::FUNCTION_DATA::FunctionID::kGetQuestCompleted:
                if (auto* q = static_cast<RE::TESForm*>(param1) ? static_cast<RE::TESForm*>(param1)->As<RE::TESQuest>() : nullptr) {
                    cInfo.currentValueStr = q->IsCompleted() ? "1 (completed)" : "0 (not completed)";
                }
                break;
            case RE::FUNCTION_DATA::FunctionID::kGetQuestRunning:
                if (auto* q = static_cast<RE::TESForm*>(param1) ? static_cast<RE::TESForm*>(param1)->As<RE::TESQuest>() : nullptr) {
                    cInfo.currentValueStr = q->IsRunning() ? "1 (running)" : "0 (not running)";
                }
                break;
            case RE::FUNCTION_DATA::FunctionID::kGetGlobalValue:
                if (auto* g = static_cast<RE::TESForm*>(param1) ? static_cast<RE::TESForm*>(param1)->As<RE::TESGlobal>() : nullptr) {
                    cInfo.currentValueStr = std::format("{:.2f}", g->value);
                }
                break;
            case RE::FUNCTION_DATA::FunctionID::kGetItemCount:
                if (auto* o = static_cast<RE::TESForm*>(param1) ? static_cast<RE::TESForm*>(param1)->As<RE::TESBoundObject>() : nullptr) {
                    cInfo.currentValueStr = std::format("{}", subj->GetInventoryCounts([o](const RE::TESBoundObject& b) { return &b == o; })[o]);
                }
                break;
            default:
                break;
            }
            if (cInfo.currentValueStr.empty()) {
                cInfo.currentValueStr = cInfo.evaluatesTrue ? "(condition true)" : "(condition false)";
            }

            status.startConditions.push_back(std::move(cInfo));
        }
    }

    if (RE::TESObjectREFR* bestRef = FindBestTeleportTarget(a_quest)) {
        status.hasTargetCoords = true;
        status.targetRefFormID = bestRef->GetFormID();
        std::string rName = bestRef->GetDisplayFullName() ? bestRef->GetDisplayFullName() : "";
        if (rName.empty() && bestRef->GetBaseObject() && bestRef->GetBaseObject()->GetName()) {
            rName = bestRef->GetBaseObject()->GetName();
        }
        if (rName.empty() && bestRef->GetFormEditorID()) {
            rName = bestRef->GetFormEditorID();
        }
        status.targetRefName = rName.empty() ? "Unnamed Reference" : rName;

        auto pos = bestRef->GetPosition();
        status.targetX = pos.x;
        status.targetY = pos.y;
        status.targetZ = pos.z;

        if (auto* cell = bestRef->GetParentCell()) {
            status.targetCellFormID = cell->GetFormID();
            status.targetCellName = cell->GetFullName() ? cell->GetFullName() : "";
            status.targetCellEditorID = cell->GetFormEditorID() ? cell->GetFormEditorID() : "";
        }
        if (auto* ws = bestRef->GetWorldspace()) {
            status.targetWorldspaceFormID = ws->GetFormID();
            status.targetWorldspaceName = ws->GetFullName() ? ws->GetFullName() : (ws->GetFormEditorID() ? ws->GetFormEditorID() : "Worldspace");
        }
    }
}

std::string QuestManager::GetQuestTypeString(RE::QUEST_DATA::Type a_type) {
    using Type = RE::QUEST_DATA::Type;
    switch (a_type) {
    case Type::kMainQuest:        return "Main Quest";
    case Type::kMagesGuild:       return "College of Winterhold";
    case Type::kThievesGuild:     return "Thieves Guild";
    case Type::kDarkBrotherhood:  return "Dark Brotherhood";
    case Type::kCompanionsQuest:  return "Companions";
    case Type::kMiscellaneous:    return "Miscellaneous";
    case Type::kDaedric:          return "Daedric Quest";
    case Type::kSideQuest:        return "Side Quest";
    case Type::kCivilWar:         return "Civil War";
    case Type::kDLC01_Vampire:    return "Dawnguard / Vampire";
    case Type::kDLC02_Dragonborn: return "Dragonborn (DLC2)";
    default:                      return "Other / System";
    }
}

void QuestManager::RefreshQuests() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_needsRefresh.store(false);
    RebuildBaseToRefCache(); 

    if (!m_iniLoaded) {
        LoadOrCreateIni();
    }

    m_allQuests.clear();
    m_itemLinks.clear();
    m_scriptStatuses.clear();
    m_pluginList.clear();
    m_pluginList.push_back("All Plugins (Game, DLCs & Mods)");

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
        return;
    }

    const auto& questArray = dataHandler->GetFormArray<RE::TESQuest>();
    m_allQuests.reserve(questArray.size());
    m_scriptStatuses.reserve(questArray.size());

    for (auto* quest : questArray) {
        if (!quest) {
            continue;
        }

        std::string name = quest->GetName() ? quest->GetName() : "";
        std::string editorID = quest->GetFormEditorID() ? quest->GetFormEditorID() : "";
        auto qType = quest->data.questType.get();

        bool isInternalName = name.empty() ||
            name.starts_with("-") ||
            name.starts_with(" ") ||
            name.starts_with("\"") ||
            name.starts_with("<") ||
            name.starts_with("(") ||
            ContainsCaseInsensitive(name, "CONDITIONED NOT TO START");

        bool isInternalEditorID = ContainsCaseInsensitive(editorID, "Dialogue") ||
            ContainsCaseInsensitive(editorID, "Scene") ||
            ContainsCaseInsensitive(editorID, "Radiant") ||
            editorID.starts_with("WI") ||
            editorID.starts_with("WE");

        bool isRadiantOrSystem = isInternalName || (isInternalEditorID && qType == RE::QUEST_DATA::Type::kNone);

        if (isRadiantOrSystem) {
            continue;
        }

        QuestEntry entry{};
        entry.quest = quest;
        entry.formID = quest->GetFormID();
        entry.name = std::move(name);
        entry.editorID = std::move(editorID);

        std::string pinKey = !entry.editorID.empty() ? entry.editorID : std::format("0x{:08X}", entry.formID);
        entry.isPinned = m_pinnedQuests.contains(pinKey);

        if (const auto* file = quest->GetFile(0)) {
            entry.sourcePlugin = std::string(file->GetFilename());
            if (std::find(m_pluginList.begin(), m_pluginList.end(), entry.sourcePlugin) == m_pluginList.end()) {
                m_pluginList.push_back(entry.sourcePlugin);
            }
        }
        else {
            entry.sourcePlugin = "Unknown";
        }

        entry.typeName = GetQuestTypeString(qType);
        entry.currentStage = quest->GetCurrentStageID();
        entry.isCompleted = quest->IsCompleted();

        bool engineRunning = quest->IsRunning() || quest->IsStarting();
        bool hasJournalObjectives = HasActiveJournalObjectives(quest);

        entry.isActiveInJournal = engineRunning && !entry.isCompleted && !isInternalName && hasJournalObjectives;
        entry.isBackgroundRunning = engineRunning && !entry.isActiveInJournal && !entry.isCompleted;

        entry.stages = ExtractAllStages(quest, entry.isActiveInJournal || entry.isCompleted);

        ExtractQuestItemsAndConditions(quest, entry, m_itemLinks);
        m_scriptStatuses.push_back(AnalyzeQuestScriptAndState(quest, entry));

        m_allQuests.push_back(std::move(entry));
    }

    SortQuestsWithPins();

   /* std::sort(m_allQuests.begin(), m_allQuests.end(), [](const QuestEntry& a, const QuestEntry& b) {
        if (a.name.empty() != b.name.empty()) {
            return !a.name.empty();
        }
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.editorID < b.editorID;
        });*/

    // Sort Rewards & Prerequisites by Quest Name / EditorID so all items of the same quest are grouped together
    std::sort(m_itemLinks.begin(), m_itemLinks.end(), [](const QuestItemLink& a, const QuestItemLink& b) {
        if (a.questName != b.questName) {
            return a.questName < b.questName;
        }
        if (a.questEditorID != b.questEditorID) {
            return a.questEditorID < b.questEditorID;
        }
        if (a.type != b.type) {
            return a.type < b.type;
        }
        return a.itemName < b.itemName;
        });

    // Sort Script & Condition Monitor by Quest Name / EditorID
    std::sort(m_scriptStatuses.begin(), m_scriptStatuses.end(), [](const QuestScriptStatus& a, const QuestScriptStatus& b) {
        if (a.questName != b.questName) {
            return a.questName < b.questName;
        }
        return a.questEditorID < b.questEditorID;
        });
}

void QuestManager::SelectDefaultStageInCombo(const QuestEntry& a_entry) {
    m_selectedStageComboIdx = 0;
    if (a_entry.stages.empty()) {
        return;
    }

    if (a_entry.isActiveInJournal) {
        for (int i = 0; i < static_cast<int>(a_entry.stages.size()); ++i) {
            if (a_entry.stages[i].index > a_entry.currentStage) {
                m_selectedStageComboIdx = i;
                return;
            }
        }
        m_selectedStageComboIdx = static_cast<int>(a_entry.stages.size()) - 1;
    }
    else {
        // Default to the first real progression stage (non-setup stage >= 10 or with objective)
        for (int i = 0; i < static_cast<int>(a_entry.stages.size()); ++i) {
            if (!a_entry.stages[i].isSetupStage) {
                m_selectedStageComboIdx = i;
                return;
            }
        }
        m_selectedStageComboIdx = 0;
    }
}

bool QuestManager::MatchesFilter(const QuestEntry& a_entry, const char* a_searchBuffer) {
    if (!a_searchBuffer || a_searchBuffer[0] == '\0') {
        return true;
    }
    std::string_view needle(a_searchBuffer);

    return ContainsCaseInsensitive(a_entry.name, needle) ||
        ContainsCaseInsensitive(a_entry.editorID, needle);
}

void QuestManager::ApplyOrStartQuestStage(const QuestEntry& a_entry, std::uint16_t a_targetStage, bool a_isLastStage) {
    RE::TESQuest* quest = a_entry.quest;
    if (!quest) {
        return;
    }

    std::string edID = a_entry.editorID;
    bool wasActive = a_entry.isActiveInJournal;
    std::vector<QuestStageInfo> allQuestStages = a_entry.stages;

    m_statusMessage = a_isLastStage
        ? std::format("Completing quest '{}' at final stage ({})...", edID, a_targetStage)
        : std::format("Setting quest '{}' to stage {}...", edID, a_targetStage);

    SKSE::GetTaskInterface()->AddTask([this, quest, edID, allQuestStages, a_targetStage, a_isLastStage, wasActive]() {
        std::vector<RE::BGSQuestObjective*> sortedObjectives;
        for (auto* obj : quest->objectives) {
            if (obj) {
                sortedObjectives.push_back(obj);
            }
        }
        std::sort(sortedObjectives.begin(), sortedObjectives.end(), [](const RE::BGSQuestObjective* a, const RE::BGSQuestObjective* b) {
            return a->index < b->index;
            });

        // 1. Pre-fill static/unique aliases and make all aliases optional so startquest cannot fail
        ForceFillQuestAliases(quest);

        if (!a_isLastStage) {
            quest->data.flags.reset(RE::QuestFlag::kCompleted);
            quest->data.flags.set(RE::QuestFlag::kEnabled, RE::QuestFlag::kActive, RE::QuestFlag::kDisplayedInHUD);
        }

        // 2. When advancing to a higher stage, mark older active objectives (< a_targetStage) as Completed
        //    so old objectives like "Retrieve the Golden Claw" don't stay open or re-trigger
        if (wasActive && !a_isLastStage) {
            bool hasNewerObjective = std::any_of(sortedObjectives.begin(), sortedObjectives.end(), [&](const RE::BGSQuestObjective* o) {
                return o->index >= a_targetStage;
                });
            if (hasNewerObjective) {
                for (auto* obj : sortedObjectives) {
                    if (obj->index < a_targetStage &&
                        obj->state.any(RE::QUEST_OBJECTIVE_STATE::kDisplayed) &&
                        obj->state.none(RE::QUEST_OBJECTIVE_STATE::kCompleted, RE::QUEST_OBJECTIVE_STATE::kFailed)) {
                        obj->state.set(RE::QUEST_OBJECTIVE_STATE::kCompleted, RE::QUEST_OBJECTIVE_STATE::kCompletedDisplayed);
                        CallNativeSetObjectiveCompleted(quest, obj->index, true);
                    }
                }
            }
        }

        // 3. Start quest and execute target stage
        if (!edID.empty()) {
            if (!wasActive && !quest->IsRunning()) {
                ExecuteConsoleCommand(std::format("startquest {}", edID));
            }
            if (!quest->IsRunning()) {
                bool started = false;
                quest->EnsureQuestStarted(started, true);
            }

            ExecuteConsoleCommand(std::format("setstage {} {}", edID, a_targetStage));
        }
        else {
            bool started = false;
            quest->EnsureQuestStarted(started, true);
        }

        // 4. Fallback if Skyrim's setstage was blocked by unfulfilled previous quest conditions (e.g. TG09 "Darkness Returns")
        if (quest->GetCurrentStageID() != a_targetStage) {
            CallNativeSetQuestStage(quest, a_targetStage);
            quest->currentStage = a_targetStage;
        }

        EnsureQuestTargetMarkersEnabled(quest);
        std::uint16_t actualStageAfterExec = quest->GetCurrentStageID();

        // CASE A: Final stage selected -> Complete Quest
        if (a_isLastStage) {
            if (!edID.empty()) {
                ExecuteConsoleCommand(std::format("completeallobjectives {}", edID));
                ExecuteConsoleCommand(std::format("completequest {}", edID));
            }
            quest->data.flags.set(RE::QuestFlag::kCompleted);
            quest->data.flags.reset(RE::QuestFlag::kActive);
        }
        // CASE B: Normal stage -> Ensure proper objective is displayed once and quest is active (white) in journal
        else {
            RE::BGSQuestObjective* matchingObj = nullptr;
            for (auto* obj : sortedObjectives) {
                if (obj->index == actualStageAfterExec) {
                    matchingObj = obj;
                    break;
                }
            }

            if (matchingObj && matchingObj->state.none(RE::QUEST_OBJECTIVE_STATE::kDisplayed)) {
                ForceDisplayObjectiveInJournal(quest, matchingObj);
            }
            else if (!HasActiveJournalObjectives(quest) && !sortedObjectives.empty()) {
                RE::BGSQuestObjective* bestObj = nullptr;
                for (auto* obj : sortedObjectives) {
                    if (obj->index <= actualStageAfterExec) {
                        bestObj = obj;
                    }
                }
                if (!bestObj) {
                    bestObj = sortedObjectives.front();
                }
                ForceDisplayObjectiveInJournal(quest, bestObj);
            }
            else {
                ForceQuestWhiteInJournal(quest);
            }

            std::thread([this, quest]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                SKSE::GetTaskInterface()->AddTask([this, quest]() {
                    EnsureQuestTargetMarkersEnabled(quest);
                    ForceQuestWhiteInJournal(quest);
                    RequestRefresh();
                    });
                }).detach();
        }

        RefreshQuests();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& entry : m_allQuests) {
                if (entry.formID == quest->GetFormID()) {
                    SelectDefaultStageInCombo(entry);
                    break;
                }
            }
        }

        if (a_isLastStage) {
            m_statusMessage = std::format("Quest '{}' successfully completed at stage {}!", edID, actualStageAfterExec);
        }
        else {
            m_statusMessage = std::format("Quest '{}' is now active at stage {}!", edID, actualStageAfterExec);
        }
        });
}

void QuestManager::AbortAndResetQuest(RE::TESQuest* a_quest, const std::string& a_editorID) {
    if (!a_quest) {
        return;
    }

    m_statusMessage = std::format("Removing quest '{}' from journal...", a_editorID);

    SKSE::GetTaskInterface()->AddTask([this, a_quest, a_editorID]() {
        for (auto* obj : a_quest->objectives) {
            if (obj) {
                CallNativeSetObjectiveDisplayed(a_quest, obj->index, false);
            }
        }

        if (!a_editorID.empty()) {
            ExecuteConsoleCommand(std::format("resetquest {}", a_editorID));
            ExecuteConsoleCommand(std::format("stopquest {}", a_editorID));
        }

        a_quest->Reset();
        a_quest->Stop();

        for (auto* obj : a_quest->objectives) {
            if (obj) {
                obj->state.reset(
                    RE::QUEST_OBJECTIVE_STATE::kDisplayed,
                    RE::QUEST_OBJECTIVE_STATE::kCompleted,
                    RE::QUEST_OBJECTIVE_STATE::kCompletedDisplayed,
                    RE::QUEST_OBJECTIVE_STATE::kFailed,
                    RE::QUEST_OBJECTIVE_STATE::kFailedDisplayed
                );
            }
        }

        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            auto& playerObjectives = player->GetPlayerRuntimeData().objectives;
            for (auto& objInst : playerObjectives) {
                if (objInst.Objective && objInst.Objective->ownerQuest == a_quest) {
                    objInst.InstanceState = RE::QUEST_OBJECTIVE_STATE::kDormant;
                }
            }
        }

        a_quest->data.flags.reset(
            RE::QuestFlag::kActive,
            RE::QuestFlag::kDisplayedInHUD,
            RE::QuestFlag::kCompleted
        );
        a_quest->currentStage = 0;

        RefreshQuests();
        m_statusMessage = std::format("Quest '{}' has been stopped and completely removed from your journal!", a_editorID);
        });
}

bool QuestManager::MatchesScriptMonitorFilter(const QuestScriptStatus& st) const {
    switch (m_scriptMonitorFilterMode) {
    case 1: // All Quests With Attached Papyrus Scripts
        if (st.attachedScripts.empty()) return false;
        break;
    case 2: // Active Journal Quests With Scripts Only
        if (!st.isActiveInJournal || st.attachedScripts.empty()) return false;
        break;
    case 3: // Background Running Quests With Scripts Only
        if (!st.isBackgroundRunning || st.attachedScripts.empty()) return false;
        break;
    case 4: // Blocked by Start Conditions / Waiting Only
        if (!st.isBlockedByConditions) return false;
        break;
    case 5: // Quests Without Any Attached Scripts
        if (!st.attachedScripts.empty()) return false;
        break;
    case 6: // Completed Quests With Scripts
        if (!st.isCompleted || st.attachedScripts.empty()) return false;
        break;
    default: // 0: Show All
        break;
    }

    std::string_view needle(m_searchScripts);
    if (!needle.empty()) {
        bool match = ContainsCaseInsensitive(st.questName, needle) ||
            ContainsCaseInsensitive(st.questEditorID, needle) ||
            ContainsCaseInsensitive(st.sourcePlugin, needle) ||
            ContainsCaseInsensitive(st.targetCellEditorID, needle) ||
            ContainsCaseInsensitive(st.targetCellName, needle);
        if (!match) {
            for (const auto& sc : st.attachedScripts) {
                if (ContainsCaseInsensitive(sc, needle)) {
                    match = true;
                    break;
                }
            }
        }
        if (!match) {
            return false;
        }
    }
    return true;
}

void QuestManager::ExportDebugReportToFile() {
    std::filesystem::path reportPath = "Data/SKSE/Plugins/QuestManager_DebugReport.txt";
    std::error_code ec;
    std::filesystem::create_directories(reportPath.parent_path(), ec);

    std::ofstream out(reportPath, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        m_statusMessage = "ERROR: Failed to write debug report to Data/SKSE/Plugins/QuestManager_DebugReport.txt!";
        return;
    }

    static const char* filterModeNames[] = {
        "Show All Quests (With & Without Scripts)",
        "All Quests With Attached Papyrus Scripts",
        "Active Journal Quests With Scripts Only",
        "Background Running Quests With Scripts Only",
        "Blocked by Start Conditions / Waiting Only",
        "Quests Without Any Attached Scripts",
        "Completed Quests With Scripts"
    };
    const char* activeFilterName = (m_scriptMonitorFilterMode >= 0 && m_scriptMonitorFilterMode < 7)
        ? filterModeNames[m_scriptMonitorFilterMode]
        : "Custom";

    auto now = std::chrono::system_clock::now();
    std::time_t nowTime = std::chrono::system_clock::to_time_t(now);

    out << "================================================================================\n";
    out << " SKYRIM QUEST MANAGER - DIAGNOSTIC & DEBUG REPORT\n";
    out << " Generated:     " << std::put_time(std::localtime(&nowTime), "%Y-%m-%d %H:%M:%S") << "\n";
    out << " Active Filter: " << activeFilterName << "\n";
    if (m_searchScripts[0] != '\0') {
        out << " Search Query:  '" << m_searchScripts << "'\n";
    }
    out << "================================================================================\n\n";

    int exportedCount = 0;
    int activeJournalCount = 0;
    int bgRunningCount = 0;

    for (auto& scriptInfo : m_scriptStatuses) {
        if (!MatchesScriptMonitorFilter(scriptInfo)) {
            continue;
        }

        PopulateScriptDeepDetails(scriptInfo);
        exportedCount++;

        if (scriptInfo.isActiveInJournal) {
            activeJournalCount++;
        }
        else if (scriptInfo.isEngineRunning) {
            bgRunningCount++;
        }

        const QuestEntry* matchingEntry = nullptr;
        for (const auto& q : m_allQuests) {
            if (q.formID == scriptInfo.questFormID) {
                matchingEntry = &q;
                break;
            }
        }

        std::string stateStr = "DORMANT / INACTIVE";
        if (scriptInfo.isActiveInJournal) stateStr = "ACTIVE IN JOURNAL";
        else if (scriptInfo.isBackgroundRunning) stateStr = "RUNNING IN BACKGROUND";
        else if (scriptInfo.isCompleted) stateStr = "COMPLETED";
        else if (scriptInfo.isBlockedByConditions) stateStr = "BLOCKED BY CONDITIONS";

        out << "--------------------------------------------------------------------------------\n";
        out << std::format("QUEST: {} [{}]\n", scriptInfo.questName.empty() ? "<Unnamed>" : scriptInfo.questName, scriptInfo.questEditorID);
        out << std::format("  FormID:        0x{:08X}\n", scriptInfo.questFormID);
        out << std::format("  VMHandle:      0x{:016X}\n", scriptInfo.vmHandle);
        out << std::format("  Source Plugin: {}\n", scriptInfo.sourcePlugin);
        out << std::format("  Category:      {}\n", scriptInfo.questTypeName);
        out << std::format("  Priority:      {}\n", scriptInfo.priority);
        out << std::format("  Quest Flags:   {}\n", scriptInfo.flagsDescription);
        out << std::format("  Event Type:    {}\n", scriptInfo.eventTypeStr);
        out << std::format("  State:         {}\n", stateStr);
        out << std::format("  Diagnostic:    {}\n", scriptInfo.blockReason);
        out << std::format("  Current Stage: {}\n", scriptInfo.currentStage);

        if (scriptInfo.hasTargetCoords) {
            out << std::format("  Target Ref:    {} (0x{:08X})\n", scriptInfo.targetRefName, scriptInfo.targetRefFormID);
            out << std::format("  Coordinates:   X: {:.1f}, Y: {:.1f}, Z: {:.1f}\n", scriptInfo.targetX, scriptInfo.targetY, scriptInfo.targetZ);
            out << std::format("  Cell / World:  {} [{}] (0x{:08X}) | Worldspace: {} (0x{:08X})\n",
                scriptInfo.targetCellName, scriptInfo.targetCellEditorID, scriptInfo.targetCellFormID,
                scriptInfo.targetWorldspaceName, scriptInfo.targetWorldspaceFormID);
        }

        if (matchingEntry && !matchingEntry->stages.empty()) {
            out << "  Stages:        ";
            for (const auto& st : matchingEntry->stages) {
                out << std::format("{}{} ", st.index, st.isExecuted ? "[Done]" : "");
            }
            out << "\n";
        }

        if (scriptInfo.questPtr && !scriptInfo.questPtr->objectives.empty()) {
            out << "  Objectives:\n";
            for (const auto* obj : scriptInfo.questPtr->objectives) {
                if (!obj) continue;
                std::string objText = obj->displayText.data() ? obj->displayText.data() : "<No Text>";
                out << std::format("    - [Index {}] ({}): {}\n", obj->index, GetObjectiveStatusString(obj), objText);
            }
        }

        if (scriptInfo.questPtr) {
            auto aliases = ExtractQuestAliases(scriptInfo.questPtr);
            if (!aliases.empty()) {
                out << "  Aliases (refAliasMap & Static Fallbacks):\n";
                for (const auto& al : aliases) {
                    if (al.isBound) {
                        out << std::format("    - [ID {}] {}: Bound to '{}' (0x{:08X})\n", al.aliasID, al.aliasName, al.refName, al.refFormID);
                    }
                    else {
                        out << std::format("    - [ID {}] {}: UNBOUND / EMPTY\n", al.aliasID, al.aliasName);
                    }
                }
            }
        }

        if (!scriptInfo.attachedScripts.empty()) {
            out << "  Attached Papyrus Scripts: ";
            for (std::size_t i = 0; i < scriptInfo.attachedScripts.size(); ++i) {
                out << scriptInfo.attachedScripts[i] << (i + 1 < scriptInfo.attachedScripts.size() ? ", " : "");
            }
            out << "\n";
        }
        if (!scriptInfo.papyrusVars.empty()) {
            out << "  Papyrus Properties & Variables:\n";
            for (const auto& pv : scriptInfo.papyrusVars) {
                std::string valStr;
                if (pv.type == PapyrusVarLiveInfo::VarType::kInt) valStr = std::to_string(pv.intVal);
                else if (pv.type == PapyrusVarLiveInfo::VarType::kFloat) valStr = std::format("{:.2f}", pv.floatVal);
                else if (pv.type == PapyrusVarLiveInfo::VarType::kBool) valStr = pv.boolVal ? "True" : "False";
                else valStr = pv.strVal;
                out << std::format("    - {}.{} ({}) = {}\n", pv.scriptName, pv.varName, pv.displayTypeStr, valStr);
            }
        }
        out << "\n";
    }

    out << "================================================================================\n";
    out << std::format("SUMMARY: Exported {} Filtered Quests ({} Active in Journal | {} Running in Background)\n",
        exportedCount, activeJournalCount, bgRunningCount);
    out << "================================================================================\n";
    out.close();

    m_statusMessage = std::format(
        "Exported {} filtered quest(s) ({}) to Data/SKSE/Plugins/QuestManager_DebugReport.txt!",
        exportedCount, activeFilterName
    );
}

void QuestManager::RenderQuestDetailsPanel(QuestEntry& a_entry) {
    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.30f, 1.0f), "%s",
        a_entry.name.empty() ? "[Unnamed Quest]" : a_entry.name.c_str());
    ImGui::SameLine();
    std::string pinBtnLabel = a_entry.isPinned ? "[*] Unpin Quest" : "[ ] Pin to Top";
    if (ImGui::SmallButton(pinBtnLabel.c_str())) {
        TogglePinQuest(a_entry.formID, a_entry.editorID);
    }

    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Editor ID:      %s", a_entry.editorID.empty() ? "<None>" : a_entry.editorID.c_str());
    ImGui::Text("Form ID:        0x%08X", a_entry.formID);
    ImGui::Text("Source Plugin:  %s", a_entry.sourcePlugin.c_str());
    ImGui::Text("Category:       %s", a_entry.typeName.c_str());

    if (a_entry.isCompleted) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Status:         Completed");
    }
    else if (a_entry.isActiveInJournal) {
        ImGui::TextColored(ImVec4(0.3f, 0.85f, 1.0f, 1.0f), "Status:         ACTIVE (In Player Journal)");
    }
    else if (a_entry.isBackgroundRunning) {
        ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.3f, 1.0f), "Status:         Running in Background (No HUD Objective)");
    }
    else {
        ImGui::TextDisabled("Status:         Inactive (Not in Journal)");
    }

    ImGui::Text("Current Stage:  %u", a_entry.currentStage);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Quest Stages (%zu total):", a_entry.stages.size());
    if (ImGui::Checkbox("Show technical setup stages (< 10 / Pre-Objective)", &m_showTechnicalSetupStages)) {
        SelectDefaultStageInCombo(a_entry);
    }

    if (!a_entry.stages.empty()) {
        std::vector<int> visibleStageIndices;
        for (int i = 0; i < static_cast<int>(a_entry.stages.size()); ++i) {
            if (m_showTechnicalSetupStages || !a_entry.stages[i].isSetupStage) {
                visibleStageIndices.push_back(i);
            }
        }
        if (visibleStageIndices.empty()) {
            for (int i = 0; i < static_cast<int>(a_entry.stages.size()); ++i) {
                visibleStageIndices.push_back(i);
            }
        }

        if (std::find(visibleStageIndices.begin(), visibleStageIndices.end(), m_selectedStageComboIdx) == visibleStageIndices.end()) {
            m_selectedStageComboIdx = visibleStageIndices.front();
        }

        int lastIdx = static_cast<int>(a_entry.stages.size()) - 1;
        int firstNonSetupIdx = visibleStageIndices.front();

        auto formatStageText = [&](int idx, const QuestStageInfo& st) -> std::string {
            bool isLast = (idx == lastIdx && a_entry.stages.size() > 1);
            std::string setupTag = st.isSetupStage ? " [Tech Setup]" : "";

            if (!a_entry.isActiveInJournal && !a_entry.isCompleted) {
                if (idx == firstNonSetupIdx && !st.isSetupStage) {
                    return std::format("Stage {}{} [Recommended Start Stage]", st.index, setupTag);
                }
                if (isLast) {
                    return std::format("Stage {}{} [Final Stage - Completes Quest]", st.index, setupTag);
                }
                return std::format("Stage {}{} [Available]", st.index, setupTag);
            }

            if (isLast) {
                return std::format("Stage {}{} [Final Stage - Completes Quest]", st.index, setupTag);
            }
            if (st.index == a_entry.currentStage) {
                return std::format("Stage {}{} [Current Stage]", st.index, setupTag);
            }
            if (st.index < a_entry.currentStage) {
                return std::format("Stage {}{} [Already Completed]", st.index, setupTag);
            }
            return std::format("Stage {}{} [Next Step / Available]", st.index, setupTag);
            };

        std::string previewLabel = formatStageText(m_selectedStageComboIdx, a_entry.stages[m_selectedStageComboIdx]);

        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##StageDropdown", previewLabel.c_str())) {
            for (int realIdx : visibleStageIndices) {
                ImGui::PushID(realIdx);
                const auto& st = a_entry.stages[realIdx];
                bool isSelected = (m_selectedStageComboIdx == realIdx);
                std::string itemLabel = formatStageText(realIdx, st);

                if (ImGui::Selectable(itemLabel.c_str(), isSelected)) {
                    m_selectedStageComboIdx = realIdx;
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();

        std::uint16_t chosenStage = a_entry.stages[m_selectedStageComboIdx].index;
        bool isLastStageSelected = (m_selectedStageComboIdx == lastIdx && a_entry.stages.size() > 1);

        if (!a_entry.isActiveInJournal && !a_entry.isCompleted) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.58f, 0.29f, 1.0f));
            std::string btnText = isLastStageSelected
                ? std::format("Complete Quest Immediately (Final Stage {})", chosenStage)
                : std::format("Start Quest at Stage {}", chosenStage);

            if (ImGui::Button(btnText.c_str(), ImVec2(-1.0f, 38.0f))) {
                ApplyOrStartQuestStage(a_entry, chosenStage, isLastStageSelected);
            }
            ImGui::PopStyleColor(2);
        }
        else if (a_entry.isActiveInJournal) {
            if (chosenStage > a_entry.currentStage) {
                if (isLastStageSelected) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.25f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.65f, 0.34f, 1.0f));
                    std::string btnText = std::format("Finish / Complete Quest (Final Stage {})", chosenStage);
                    if (ImGui::Button(btnText.c_str(), ImVec2(-1.0f, 38.0f))) {
                        ApplyOrStartQuestStage(a_entry, chosenStage, true);
                    }
                    ImGui::PopStyleColor(2);
                }
                else {
                    std::string btnText = std::format("Advance to Selected Stage (Stage {})", chosenStage);
                    if (ImGui::Button(btnText.c_str(), ImVec2(-1.0f, 38.0f))) {
                        ApplyOrStartQuestStage(a_entry, chosenStage, false);
                    }
                }
            }
            else {
                ImGui::TextDisabled("Select a higher stage (> %u) in the dropdown\nto advance this quest.", a_entry.currentStage);
            }
        }
    }
    else {
        ImGui::TextDisabled("This quest has no defined quest stages.");
    }

    // --- Quest Objectives & Status ---
    if (a_entry.quest && !a_entry.quest->objectives.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        int objCount = 0;
        for (auto* obj : a_entry.quest->objectives) {
            if (obj) objCount++;
        }

        ImGui::Text("Quest Objectives (%d):", objCount);
        ImGui::BeginChild("ObjectivesListChild", ImVec2(0.0f, 105.0f), true, ImGuiWindowFlags_HorizontalScrollbar);

        for (auto* obj : a_entry.quest->objectives) {
            if (!obj) continue;

            std::string objText = obj->displayText.data() ? obj->displayText.data() : "<No Text>";
            std::string statusStr = GetObjectiveStatusString(obj);

            ImGui::BulletText("Index %u - %s", obj->index, statusStr.c_str());
            ImGui::Indent(15.0f);
            ImGui::TextWrapped("Text: %s", objText.c_str());
            ImGui::Unindent(15.0f);
            ImGui::Spacing();
        }
        ImGui::EndChild();
    }

    // --- Submenu: Quest Aliases (refAliasMap + Static Fallback) & Direct Teleport ---
    if (a_entry.quest) {
        ImGui::Spacing();
        if (!a_entry.aliasesCached) {
            a_entry.cachedAliases = ExtractQuestAliases(a_entry.quest);
            a_entry.aliasesCached = true;
        }
        const auto& aliases = a_entry.cachedAliases;

        std::string aliasHeader = std::format("Quest Aliases & Bound References ({})###AliasesHeader", aliases.size());
        if (ImGui::CollapsingHeader(aliasHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (aliases.empty()) {
                ImGui::TextDisabled("No aliases defined for this quest.");
            }
            else {
                ImGui::BeginChild("AliasesListChild", ImVec2(0.0f, 140.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
                for (const auto& al : aliases) {
                    ImGui::PushID(static_cast<int>(al.aliasID));
                    if (ImGui::SmallButton("Teleport")) {
                        TeleportToSpecificAlias(a_entry.quest, al.aliasID, al.aliasName);
                    }
                    ImGui::SameLine();

                    if (al.isBound) {
                        if (auto* aliasActor = RE::TESForm::LookupByID<RE::Actor>(al.refFormID); aliasActor && !aliasActor->IsPlayerRef()) {
                            if (ImGui::SmallButton("Actor Tools")) {
                                OpenInActorsTab(a_entry, al.aliasID);
                            }
                            ImGui::SameLine();
                            if (aliasActor->IsDead()) {
                                ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.30f, 1.0f), "[DEAD]");
                                ImGui::SameLine();
                            }
                            else if (aliasActor->IsDisabled()) {
                                ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f), "[DISABLED]");
                                ImGui::SameLine();
                            }
                        }
                        ImVec4 aliasColor = al.isStaticFallback
                            ? ImVec4(0.4f, 0.85f, 1.0f, 1.0f)
                            : ImVec4(0.4f, 0.95f, 0.5f, 1.0f);
                        ImGui::TextColored(aliasColor, "[ID %u] %s", al.aliasID, al.aliasName.c_str());
                        ImGui::SameLine();
                        ImGui::Text("-> %s (0x%08X)", al.refName.c_str(), al.refFormID);
                    }
                    else {
                        ImGui::TextColored(ImVec4(0.85f, 0.70f, 0.40f, 1.0f), "[ID %u] %s", al.aliasID, al.aliasName.c_str());
                        ImGui::SameLine();
                        ImGui::TextDisabled("-> %s (Auto-Resolves on Teleport)", al.refName.c_str());
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();
            }
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("Teleport to Best Active Quest Target", ImVec2(-1.0f, 34.0f))) {
        TeleportToQuestTarget(a_entry.quest, a_entry.editorID);
    }

    if (a_entry.isActiveInJournal || a_entry.isCompleted || a_entry.currentStage > 0) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "Danger Zone (Use at your own risk):");

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.20f, 0.20f, 1.0f));
        const bool multiAbort = m_multiActiveQuests.size() > 1 && m_multiActiveQuests.contains(a_entry.formID);
        const std::string abortLabel = multiAbort
            ? std::format("Abort & Remove {} Selected Quests from Journal###AbortBtn", m_multiActiveQuests.size())
            : std::string("Abort & Remove Quest from Journal###AbortBtn");
        if (ImGui::Button(abortLabel.c_str(), ImVec2(-1.0f, 36.0f))) {
            ImGui::OpenPopup("ConfirmAbortQuest");
        }
        ImGui::PopStyleColor(2);

        if (ImGui::BeginPopupModal("ConfirmAbortQuest", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (multiAbort) {
                ImGui::Text("Are you sure you want to completely abort and reset\n%d selected quests?",
                    static_cast<int>(m_multiActiveQuests.size()));
            }
            else {
                ImGui::Text("Are you sure you want to completely abort and reset\n'%s' [%s]?",
                    a_entry.name.empty() ? "Unnamed" : a_entry.name.c_str(),
                    a_entry.editorID.c_str());
            }
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                "WARNING: All quest objectives will be removed from your journal,\n"
                "all stages will be reset to 0, and the quest will be stopped immediately!");
            ImGui::Spacing();
            ImGui::Separator();

            if (ImGui::Button("Yes, Abort & Reset!", ImVec2(210.0f, 0.0f))) {
                if (multiAbort) {
                    for (auto& e : m_allQuests) {
                        if (e.isActiveInJournal && m_multiActiveQuests.contains(e.formID)) {
                            AbortAndResetQuest(e.quest, e.editorID);
                        }
                    }
                    m_multiActiveQuests.clear();
                }
                else {
                    AbortAndResetQuest(a_entry.quest, a_entry.editorID);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(140.0f, 0.0f))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

void QuestManager::RenderActiveQuestsTab() {
    ImGui::SetNextItemWidth(450.0f);
    ImGui::InputTextWithHint("##SearchActive", "Search active quests (Name or Quest ID)...", m_searchActive, sizeof(m_searchActive));
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        RequestRefresh();
    }

    std::erase_if(m_multiActiveQuests, [this](RE::FormID a_id) {
        return std::none_of(m_allQuests.begin(), m_allQuests.end(),
            [a_id](const QuestEntry& e) { return e.formID == a_id && e.isActiveInJournal; });
    });

    ImGui::Spacing();

    if (!ImGui::IsMouseDown(0)) {
        m_multiDragging = false;
    }

    ImGui::BeginChild("ActiveQuestList", ImVec2(680.0f, -35.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
    int activeCount = 0;
    QuestEntry* selectedEntry = nullptr;

    for (auto& entry : m_allQuests) {
        if (!entry.isActiveInJournal) {
            continue;
        }
        if (!MatchesFilter(entry, m_searchActive)) {
            continue;
        }

        activeCount++;
        ImGui::PushID(static_cast<int>(entry.formID));
        ImGui::PushStyleColor(ImGuiCol_Text, entry.isPinned ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) : ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
        if (ImGui::SmallButton(entry.isPinned ? "[*]" : "[ ]")) {
            TogglePinQuest(entry.formID, entry.editorID);
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(entry.isPinned ? "Unpin quest from top" : "Pin quest to top of list");
        }
        ImGui::SameLine();

        std::string displayTitle = std::format("{}{} [{}] (Stage: {})",
            entry.isPinned ? "[*] " : "",
            entry.name.empty() ? entry.editorID : entry.name,
            entry.editorID,
            entry.currentStage);

        bool isSelected = (m_selectedActiveQuestID == entry.formID);
        const bool inMulti = m_multiActiveQuests.contains(entry.formID);
        if (ImGui::Selectable(displayTitle.c_str(), isSelected || inMulti)) {
            m_selectedActiveQuestID = entry.formID;
            SelectDefaultStageInCombo(entry);
        }
        if (ImGui::IsItemClicked(0)) {
            m_multiDragging = true;
            if (ImGui::GetIO().KeyCtrl) {
                m_multiDragAdd = !inMulti;
            }
            else {
                m_multiActiveQuests.clear();
                m_multiDragAdd = true;
            }
        }
        if (m_multiDragging && ImGui::IsMouseDown(0) &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
            if (m_multiDragAdd) {
                m_multiActiveQuests.insert(entry.formID);
            }
            else {
                m_multiActiveQuests.erase(entry.formID);
            }
        }
        if (isSelected) {
            selectedEntry = &entry;
        }
        ImGui::PopID();
    }

    if (activeCount == 0) {
        ImGui::TextDisabled("There are currently no active quests in your journal.");
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("ActiveQuestDetails", ImVec2(0.0f, -35.0f), true);
    if (selectedEntry) {
        RenderQuestDetailsPanel(*selectedEntry);
    }
    else {
        ImGui::TextDisabled("Select an active quest on the left to advance its stage\nor remove it from your journal.");
    }
    ImGui::EndChild();
}

void QuestManager::RenderAllQuestsTab() {
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##SearchAll", "Search (Name or Quest ID)...", m_searchAll, sizeof(m_searchAll));
    ImGui::SameLine();

    ImGui::SetNextItemWidth(290.0f);
    if (m_selectedPluginIdx >= static_cast<int>(m_pluginList.size())) {
        m_selectedPluginIdx = 0;
    }
    const char* pluginPreview = m_pluginList.empty() ? "All" : m_pluginList[m_selectedPluginIdx].c_str();
    if (ImGui::BeginCombo("##PluginFilter", pluginPreview)) {
        for (int i = 0; i < static_cast<int>(m_pluginList.size()); ++i) {
            bool isSelected = (m_selectedPluginIdx == i);
            if (ImGui::Selectable(m_pluginList[i].c_str(), isSelected)) {
                m_selectedPluginIdx = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();

    static const std::vector<std::string> allKnownCategories = {
        "Main Quest",
        "Companions",
        "College of Winterhold",
        "Thieves Guild",
        "Dark Brotherhood",
        "Civil War",
        "Daedric Quest",
        "Side Quest",
        "Miscellaneous",
        "Dawnguard / Vampire",
        "Dragonborn (DLC2)",
        "Other / System"
    };

    std::vector<std::string> availableCategories;
    availableCategories.push_back("All Categories");

    for (const auto& cat : allKnownCategories) {
        bool hasQuestsInCat = false;
        for (const auto& entry : m_allQuests) {
            if (m_selectedPluginIdx > 0 && entry.sourcePlugin != m_pluginList[m_selectedPluginIdx]) continue;

            if (entry.typeName == cat) {
                hasQuestsInCat = true;
                break;
            }
        }
        if (hasQuestsInCat) {
            availableCategories.push_back(cat);
        }
    }

    if (std::find(availableCategories.begin(), availableCategories.end(), m_selectedCategory) == availableCategories.end()) {
        m_selectedCategory = "All Categories";
    }

    ImGui::SetNextItemWidth(210.0f);
    if (ImGui::BeginCombo("##CategoryFilter", m_selectedCategory.c_str())) {
        for (const auto& cat : availableCategories) {
            bool isSelected = (m_selectedCategory == cat);
            if (ImGui::Selectable(cat.c_str(), isSelected)) {
                m_selectedCategory = cat;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();

    ImGui::BeginChild("AllQuestsList", ImVec2(680.0f, -35.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
    QuestEntry* selectedEntry = nullptr;

    for (auto& entry : m_allQuests) {
        if (m_selectedPluginIdx > 0 && entry.sourcePlugin != m_pluginList[m_selectedPluginIdx]) {
            continue;
        }
        if (m_selectedCategory != "All Categories" && entry.typeName != m_selectedCategory) {
            continue;
        }
        if (!MatchesFilter(entry, m_searchAll)) {
            continue;
        }

        std::string statusTag;
        if (entry.isActiveInJournal) {
            statusTag = " [ACTIVE]";
        }
        else if (entry.isCompleted) {
            statusTag = " [Completed]";
        }

        ImGui::PushID(static_cast<int>(entry.formID));
        ImGui::PushStyleColor(ImGuiCol_Text, entry.isPinned ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) : ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
        if (ImGui::SmallButton(entry.isPinned ? "[*]" : "[ ]")) {
            TogglePinQuest(entry.formID, entry.editorID);
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(entry.isPinned ? "Unpin quest from top" : "Pin quest to top of list");
        }
        ImGui::SameLine();

        std::string label = entry.name.empty()
            ? std::format("{}[{}] ({}){}", entry.isPinned ? "[*] " : "", entry.editorID, entry.sourcePlugin, statusTag)
            : std::format("{}{} [{}]{}", entry.isPinned ? "[*] " : "", entry.name, entry.editorID, statusTag);

        bool isSelected = (m_selectedAllQuestID == entry.formID);
        if (isSelected && m_scrollAllQuestsToSelected) {
            ImGui::SetScrollHereY(0.0f);
            m_scrollAllQuestsToSelected = false;
        }
        if (ImGui::Selectable(label.c_str(), isSelected)) {
            m_selectedAllQuestID = entry.formID;
            SelectDefaultStageInCombo(entry);
        }
        if (isSelected) {
            selectedEntry = &entry;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("AllQuestsDetails", ImVec2(0.0f, -35.0f), true);
    if (selectedEntry) {
        RenderQuestDetailsPanel(*selectedEntry);
    }
    else {
        ImGui::TextDisabled("Select a quest from the game, DLC, or a mod on the left.");
    }
    ImGui::EndChild();
}

void QuestManager::RenderItemsAndRewardsTab() {
    ImGui::SetNextItemWidth(340.0f);
    ImGui::InputTextWithHint("##SearchItems", "Search Quest, Item, Spell, Perk, Faction or Keyword...", m_searchItems, sizeof(m_searchItems));
    ImGui::SameLine();
    ImGui::Checkbox("Show Rewards / Quest Items", &m_showRewardsFilter);
    ImGui::SameLine();
    ImGui::Checkbox("Show Prerequisites / Conditions", &m_showPrerequisitesFilter);

    ImGui::Spacing();

    ImGui::BeginChild("ItemLinksList", ImVec2(680.0f, -35.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
    std::string_view needle(m_searchItems);

    for (int i = 0; i < static_cast<int>(m_itemLinks.size()); ++i) {
        const auto& link = m_itemLinks[i];

        if (link.type == QuestItemLink::LinkType::kRewardOrItem && !m_showRewardsFilter) {
            continue;
        }
        if (link.type == QuestItemLink::LinkType::kPrerequisite && !m_showPrerequisitesFilter) {
            continue;
        }

        if (!needle.empty()) {
            bool match = ContainsCaseInsensitive(link.itemName, needle) ||
                ContainsCaseInsensitive(link.itemTypeStr, needle) ||
                ContainsCaseInsensitive(link.questName, needle) ||
                ContainsCaseInsensitive(link.questEditorID, needle);
            if (!match) {
                continue;
            }
        }

        ImGui::PushID(i);
        const char* tag = (link.type == QuestItemLink::LinkType::kRewardOrItem) ? "[REWARD / ITEM]" : "[PREREQUISITE]";
        ImVec4 tagColor = (link.type == QuestItemLink::LinkType::kRewardOrItem)
            ? ImVec4(0.35f, 0.95f, 0.45f, 1.0f)
            : ImVec4(0.95f, 0.70f, 0.30f, 1.0f);

        ImGui::TextColored(tagColor, "%s", tag);
        ImGui::SameLine();

        std::string rowLabel = std::format("[{} | {}] -> {} ({})", link.questName, link.questEditorID, link.itemName, link.itemTypeStr);
        bool isSelected = (m_selectedItemLinkIdx == i);
        if (ImGui::Selectable(rowLabel.c_str(), isSelected)) {
            m_selectedItemLinkIdx = i;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("ItemLinkDetails", ImVec2(0.0f, -35.0f), true);
    if (m_selectedItemLinkIdx >= 0 && m_selectedItemLinkIdx < static_cast<int>(m_itemLinks.size())) {
        const auto& sel = m_itemLinks[m_selectedItemLinkIdx];

        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.30f, 1.0f), "%s", sel.itemName.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Entry Type:     %s", sel.type == QuestItemLink::LinkType::kRewardOrItem ? "Quest Reward / Alias Item" : "Start Prerequisite / Condition");
        ImGui::Text("Form Category:  %s", sel.itemTypeStr.c_str());
        ImGui::Text("Item FormID:    0x%08X", sel.itemFormID);
        ImGui::TextWrapped("Role in Quest:  %s", sel.conditionDetails.c_str());

        ImGui::Spacing();

        if (sel.type == QuestItemLink::LinkType::kPrerequisite) {
            ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "What this quest needs to start:");
            const QuestEntry* qe = nullptr;
            for (const auto& q : m_allQuests) {
                if (q.formID == sel.questFormID) {
                    qe = &q;
                    break;
                }
            }
            if (qe && qe->quest) {
                const auto st = AnalyzeQuestScriptAndState(qe->quest, *qe, false);
                if (st.startConditions.empty()) {
                    ImGui::TextDisabled("This quest has no explicit start conditions (objConditions).");
                    ImGui::TextWrapped("This entry is a reference/alias prerequisite, not an objConditions entry. The quest can still depend on it through its alias, script, dialogue or Story Manager logic.");
                }
                else {
                    int passCount = 0;
                    for (const auto& sc : st.startConditions) {
                        passCount += sc.evaluatesTrue ? 1 : 0;
                    }
                    const bool allMet = passCount == static_cast<int>(st.startConditions.size());
                    ImGui::TextColored(allMet ? ImVec4(0.35f, 0.95f, 0.45f, 1.0f) : ImVec4(0.95f, 0.75f, 0.30f, 1.0f),
                        "%d of %zu conditions met%s", passCount, st.startConditions.size(),
                        allMet ? " - quest can start." : ".");
                    ImGui::BeginChild("ExplorerConds", ImVec2(0.0f, 190.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
                    for (const auto& sc : st.startConditions) {
                        ImGui::TextColored(sc.evaluatesTrue ? ImVec4(0.35f, 0.95f, 0.45f, 1.0f) : ImVec4(0.95f, 0.40f, 0.40f, 1.0f),
                            "[%s]", sc.evaluatesTrue ? "PASS" : "FAIL");
                        ImGui::SameLine();
                        ImGui::Text("%s (%s)", sc.functionName.c_str(), sc.isOR ? "OR" : "AND");
                        ImGui::TextDisabled("      Required: %s | Current: %s", sc.paramSummary.c_str(), sc.currentValueStr.c_str());
                    }
                    ImGui::EndChild();
                }
            }
            else {
                ImGui::TextDisabled("Quest data not available.");
            }
            ImGui::Spacing();
        }

        RE::TESForm* itemForm = RE::TESForm::LookupByID(sel.itemFormID);
        bool canGiveToPlayer = itemForm && IsInventoryOrRewardFormType(itemForm->GetFormType());

        std::string giveBtnLabel = "Add Item / Reward to Inventory";
        if (itemForm) {
            if (itemForm->GetFormType() == RE::FormType::Spell) giveBtnLabel = "Learn Spell (Add to Player)";
            else if (itemForm->GetFormType() == RE::FormType::Perk) giveBtnLabel = "Unlock Perk (Add to Player)";
            else if (itemForm->GetFormType() == RE::FormType::Shout) giveBtnLabel = "Unlock Shout Words (Add to Player)";
        }
        else {
            giveBtnLabel = "Not an Inventory Item / Cannot Add";
        }
        if (!canGiveToPlayer) {
            giveBtnLabel = std::format("Cannot Add '{}' to Inventory", sel.itemTypeStr);
        }

        if (canGiveToPlayer || sel.type == QuestItemLink::LinkType::kRewardOrItem) {
        ImGui::BeginDisabled(!canGiveToPlayer);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.50f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.65f, 0.32f, 1.0f));
        if (ImGui::Button(giveBtnLabel.c_str(), ImVec2(-1.0f, 36.0f))) {
            GiveOrUnlockFormForPlayer(sel.itemFormID, sel.itemName);
        }
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "Associated Quest Information:");
        ImGui::Text("Quest Name:     %s", sel.questName.c_str());
        ImGui::Text("Quest EditorID: %s", sel.questEditorID.c_str());
        ImGui::Text("Quest FormID:   0x%08X", sel.questFormID);
        ImGui::Text("Source Plugin:  %s", sel.sourcePlugin.c_str());

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.70f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.58f, 0.88f, 1.0f));
        if (ImGui::Button("Jump to Quest in 'All Quests' Tab", ImVec2(-1.0f, 38.0f))) {
            m_selectedAllQuestID = sel.questFormID;
            m_selectedPluginIdx = 0;
            m_selectedCategory = "All Categories";
            m_searchAll[0] = '\0';

            for (const auto& q : m_allQuests) {
                if (q.formID == sel.questFormID) {
                    SelectDefaultStageInCombo(q);
                    break;
                }
            }
            m_switchToAllQuestsTab = true;
            m_scrollAllQuestsToSelected = true;
        }
        ImGui::PopStyleColor(2);
    }
    else {
        ImGui::TextDisabled("Select a reward or prerequisite entry on the left\nto inspect its associated quest.");
    }
    ImGui::EndChild();
}

void QuestManager::GiveOrUnlockFormForPlayer(RE::FormID a_formID, const std::string& a_itemName) {
    if (a_formID == 0) {
        return;
    }

    SKSE::GetTaskInterface()->AddTask([this, a_formID, a_itemName]() {
        auto* form = RE::TESForm::LookupByID(a_formID);
        if (!form) {
            m_statusMessage = std::format("Could not find Form 0x{:08X}!", a_formID);
            return;
        }

        auto fType = form->GetFormType();
        if (fType == RE::FormType::Spell) {
            ExecuteConsoleCommand(std::format("player.addspell {:08X}", a_formID));
            m_statusMessage = std::format("Added Spell '{}' (0x{:08X}) to player!", a_itemName, a_formID);
        }
        else if (fType == RE::FormType::Perk) {
            ExecuteConsoleCommand(std::format("player.addperk {:08X}", a_formID));
            m_statusMessage = std::format("Unlocked Perk '{}' (0x{:08X}) for player!", a_itemName, a_formID);
        }
        else if (fType == RE::FormType::Shout) {
            if (auto* shout = form->As<RE::TESShout>()) {
                for (int i = 0; i < 3; ++i) {
                    if (shout->variations[i].word) {
                        ExecuteConsoleCommand(std::format("player.teachword {:08X}", shout->variations[i].word->GetFormID()));
                        ExecuteConsoleCommand(std::format("player.unlockword {:08X}", shout->variations[i].word->GetFormID()));
                    }
                }
            }
            m_statusMessage = std::format("Unlocked Shout '{}' (0x{:08X}) for player!", a_itemName, a_formID);
        }
        else if (IsInventoryOrRewardFormType(fType)) {
            ExecuteConsoleCommand(std::format("player.additem {:08X} 1", a_formID));
            m_statusMessage = std::format("Added 1x '{}' (0x{:08X}) to your inventory!", a_itemName, a_formID);
        }
        else {
            m_statusMessage = std::format("'{}' (0x{:08X}) is not a valid inventory or ability form.", a_itemName, a_formID);
        }
        });
}

void QuestManager::RenderScriptMonitorTab() {
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##SearchScripts", "Search Quest, Script, Cell or Plugin...", m_searchScripts, sizeof(m_searchScripts));
    ImGui::SameLine();

    ImGui::SetNextItemWidth(390.0f);
    const char* filterModes[] = {
        "Show All Quests (With & Without Scripts)",
        "All Quests With Attached Papyrus Scripts",
        "Active Journal Quests With Scripts Only",
        "Background Running Quests With Scripts Only",
        "Blocked by Start Conditions / Waiting Only",
        "Quests Without Any Attached Scripts",
        "Completed Quests With Scripts"
    };
    ImGui::Combo("##ScriptMonitorFilter", &m_scriptMonitorFilterMode, filterModes, IM_ARRAYSIZE(filterModes));
    ImGui::SameLine();
    if (ImGui::Button("Refresh Status")) {
        RequestRefresh();
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.45f, 0.30f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.58f, 0.38f, 1.0f));
    if (ImGui::Button("Export Filtered List (.txt)")) {
        ExportDebugReportToFile();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Exports only the quests currently matching the selected filter and search bar above.");
    }
    ImGui::PopStyleColor(2);

    ImGui::Spacing();

    ImGui::BeginChild("ScriptMonitorList", ImVec2(680.0f, -35.0f), true, ImGuiWindowFlags_HorizontalScrollbar);

    for (int i = 0; i < static_cast<int>(m_scriptStatuses.size()); ++i) {
        const auto& st = m_scriptStatuses[i];
        if (!MatchesScriptMonitorFilter(st)) {
            continue;
        }

        ImGui::PushID(i);
        if (st.isActiveInJournal && !st.attachedScripts.empty()) {
            ImGui::TextColored(ImVec4(0.3f, 0.95f, 0.4f, 1.0f), "[JOURNAL + SCRIPT]");
        }
        else if (st.isEngineRunning && !st.attachedScripts.empty()) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.8f, 1.0f), "[BG SCRIPT]");
        }
        else if (st.isBlockedByConditions) {
            ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f), "[BLOCKED]");
        }
        else if (st.isEngineRunning) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "[RUNNING (NO SCRIPT)]");
        }
        else if (st.isCompleted) {
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 0.6f, 1.0f), "[COMPLETED]");
        }
        else {
            ImGui::TextDisabled("[DORMANT]");
        }
        ImGui::SameLine();

        std::string itemLabel = std::format("{} [{}] (Scripts: {})", st.questName, st.questEditorID, st.attachedScripts.size());
        bool isSelected = (m_selectedScriptStatusIdx == i);
        if (ImGui::Selectable(itemLabel.c_str(), isSelected)) {
            m_selectedScriptStatusIdx = i;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("ScriptMonitorDetails", ImVec2(0.0f, -35.0f), true);
    if (m_selectedScriptStatusIdx >= 0 && m_selectedScriptStatusIdx < static_cast<int>(m_scriptStatuses.size())) {
        auto& st = m_scriptStatuses[m_selectedScriptStatusIdx];
        PopulateScriptDeepDetails(st);

        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.30f, 1.0f), "%s [%s]", st.questName.c_str(), st.questEditorID.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        // --- Quick Modder Copy Utilities ---
        if (ImGui::SmallButton("Copy FormID")) {
            std::string hexID = std::format("0x{:08X}", st.questFormID);
            ImGui::SetClipboardText(hexID.c_str());
            m_statusMessage = std::format("Copied FormID {} to clipboard!", hexID);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy EditorID")) {
            ImGui::SetClipboardText(st.questEditorID.c_str());
            m_statusMessage = std::format("Copied EditorID '{}' to clipboard!", st.questEditorID);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy 'sqv' Command")) {
            std::string cmd = std::format("sqv {}", st.questEditorID);
            ImGui::SetClipboardText(cmd.c_str());
            m_statusMessage = std::format("Copied console command '{}' to clipboard!", cmd);
        }

        ImGui::Spacing();

        ImGui::Text("Quest FormID:   0x%08X (Dec: %u)", st.questFormID, st.questFormID);
        ImGui::Text("VM Handle:      0x%016llX", static_cast<unsigned long long>(st.vmHandle));
        ImGui::Text("Source Plugin:  %s", st.sourcePlugin.c_str());
        ImGui::Text("Quest Type:     %s (Priority: %u)", st.questTypeName.c_str(), st.priority);
        ImGui::TextWrapped("Quest Flags:    %s", st.flagsDescription.c_str());
        ImGui::TextWrapped("Event Trigger:  %s", st.eventTypeStr.c_str());
        ImGui::Text("Engine Running: %s | Stage: %u", st.isEngineRunning ? "YES (Active in VM)" : "NO", st.currentStage);
        ImGui::Text("Aliases/Objs:   %u/%u Aliases Bound/Resolvable | %u Objectives",
            st.boundOrResolvableAliases, st.totalAliases, st.totalObjectives);
        ImGui::TextWrapped("Diagnostic:     %s", st.blockReason.c_str());

        // --- 3D World Coordinates & Location Inspector ---
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "3D Target Reference, Cell & Coordinates:");
        if (st.hasTargetCoords) {
            ImGui::Text("Target Ref:     %s (0x%08X)", st.targetRefName.c_str(), st.targetRefFormID);
            ImGui::Text("World Pos:      X: %.1f,  Y: %.1f,  Z: %.1f", st.targetX, st.targetY, st.targetZ);
            ImGui::Text("Parent Cell:    %s [%s] (0x%08X)",
                st.targetCellName.empty() ? "<Interior/Unnamed>" : st.targetCellName.c_str(),
                st.targetCellEditorID.empty() ? "NoEditorID" : st.targetCellEditorID.c_str(),
                st.targetCellFormID);
            if (st.targetWorldspaceFormID != 0) {
                ImGui::Text("Worldspace:     %s (0x%08X)", st.targetWorldspaceName.c_str(), st.targetWorldspaceFormID);
            }

            if (ImGui::SmallButton("Teleport to Target Ref")) {
                for (const auto& q : m_allQuests) {
                    if (q.formID == st.questFormID && q.quest) {
                        TeleportToQuestTarget(q.quest, q.editorID);
                        break;
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy 'player.moveto'")) {
                std::string cmd = std::format("player.moveto {:08X}", st.targetRefFormID);
                ImGui::SetClipboardText(cmd.c_str());
                m_statusMessage = std::format("Copied '{}' to clipboard!", cmd);
            }
            if (!st.targetCellEditorID.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Copy 'coc' Cell")) {
                    std::string cmd = std::format("coc {}", st.targetCellEditorID);
                    ImGui::SetClipboardText(cmd.c_str());
                    m_statusMessage = std::format("Copied '{}' to clipboard!", cmd);
                }
            }
        }
        else {
            ImGui::TextDisabled("No 3D target reference associated with this quest.");
        }

        // --- Live Quest Global Variables Editor (TESGlobal) ---
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        std::string globHeader = std::format("Live Quest Globals Editor (TESGlobal) ({})###GlobalsHeader", st.referencedGlobals.size());
        if (ImGui::CollapsingHeader(globHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (st.referencedGlobals.empty()) {
                ImGui::TextDisabled("No TESGlobal variables directly referenced in this quest's conditions.");
            }
            else {
                for (std::size_t gIdx = 0; gIdx < st.referencedGlobals.size(); ++gIdx) {
                    auto& glob = st.referencedGlobals[gIdx];
                    if (glob.globalForm) {
                        glob.currentValue = glob.globalForm->value;
                    }
                    ImGui::PushID(static_cast<int>(gIdx));
                    ImGui::SetNextItemWidth(135.0f);
                    if (ImGui::InputFloat("##GlobVal", &glob.currentValue, 1.0f, 10.0f, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                        if (glob.globalForm) {
                            glob.globalForm->value = glob.currentValue;
                            m_statusMessage = std::format("Updated TESGlobal '{}' to {:.2f}!", glob.editorID, glob.currentValue);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.35f, 1.0f), "%s (0x%08X)", glob.editorID.c_str(), glob.formID);
                    ImGui::PopID();
                }
            }
        }

        // --- Live Papyrus Script Variables & Properties Editor ---
        ImGui::Spacing();
        std::string papyrusHeader = std::format("Live Papyrus Script Variables & Properties ({})###PapyrusVarsHeader", st.papyrusVars.size());
        if (ImGui::CollapsingHeader(papyrusHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (st.attachedScripts.empty()) {
                ImGui::TextDisabled("No Papyrus scripts bound to this quest in the Virtual Machine.");
            }
            else {
                for (const auto& sc : st.attachedScripts) {
                    ImGui::BulletText("%s.pex", sc.c_str());
                    ImGui::SameLine();
                    ImGui::PushID(sc.c_str());
                    if (ImGui::SmallButton("Copy Script Name")) {
                        ImGui::SetClipboardText(sc.c_str());
                        m_statusMessage = std::format("Copied script name '{}' to clipboard!", sc);
                    }
                    ImGui::PopID();
                }

                if (!st.papyrusVars.empty()) {
                    ImGui::Spacing();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputTextWithHint("##FilterPapyrusVars", "Filter properties/variables by name or type...", m_searchPapyrusVars, sizeof(m_searchPapyrusVars));

                    ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

                    if (ImGui::BeginTable("PapyrusVarsTable", 3, tableFlags, ImVec2(0.0f, 195.0f))) {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 68.0f);
                        ImGui::TableSetupColumn("Script.Property / Variable", ImGuiTableColumnFlags_WidthStretch, 0.52f);
                        ImGui::TableSetupColumn("Live VM Value / Handle", ImGuiTableColumnFlags_WidthStretch, 0.48f);
                        ImGui::TableHeadersRow();

                        std::string_view varNeedle(m_searchPapyrusVars);
                        for (std::size_t vIdx = 0; vIdx < st.papyrusVars.size(); ++vIdx) {
                            auto& pv = st.papyrusVars[vIdx];
                            if (!varNeedle.empty()) {
                                if (!ContainsCaseInsensitive(pv.varName, varNeedle) &&
                                    !ContainsCaseInsensitive(pv.scriptName, varNeedle) &&
                                    !ContainsCaseInsensitive(pv.displayTypeStr, varNeedle) &&
                                    !ContainsCaseInsensitive(pv.strVal, varNeedle)) {
                                    continue;
                                }
                            }

                            ImGui::TableNextRow();
                            ImGui::PushID(static_cast<int>(vIdx));

                            // Column 0: Type
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f), "[%s]", pv.displayTypeStr.c_str());

                            // Column 1: Script & Variable Name (No overlap + full tooltip on hover)
                            ImGui::TableSetColumnIndex(1);
                            std::string fullVarName = std::format("{}.{}", pv.scriptName, pv.varName);
                            ImGui::TextUnformatted(fullVarName.c_str());
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("%s", fullVarName.c_str());
                            }

                            // Column 2: Editable Value or Object Handle
                            ImGui::TableSetColumnIndex(2);
                            if (pv.type == PapyrusVarLiveInfo::VarType::kInt) {
                                ImGui::SetNextItemWidth(-1.0f);
                                if (ImGui::InputInt("##PInt", &pv.intVal, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue)) {
                                    if (ApplyPapyrusVariableEdit(st.vmHandle, pv)) {
                                        m_statusMessage = std::format("Updated {}.{} (Int) to {} in VM!", pv.scriptName, pv.varName, pv.intVal);
                                    }
                                }
                            }
                            else if (pv.type == PapyrusVarLiveInfo::VarType::kFloat) {
                                ImGui::SetNextItemWidth(-1.0f);
                                if (ImGui::InputFloat("##PFloat", &pv.floatVal, 1.0f, 10.0f, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                                    if (ApplyPapyrusVariableEdit(st.vmHandle, pv)) {
                                        m_statusMessage = std::format("Updated {}.{} (Float) to {:.2f} in VM!", pv.scriptName, pv.varName, pv.floatVal);
                                    }
                                }
                            }
                            else if (pv.type == PapyrusVarLiveInfo::VarType::kBool) {
                                if (ImGui::Checkbox("##PBool", &pv.boolVal)) {
                                    if (ApplyPapyrusVariableEdit(st.vmHandle, pv)) {
                                        m_statusMessage = std::format("Toggled {}.{} (Bool) to {} in VM!", pv.scriptName, pv.varName, pv.boolVal ? "True" : "False");
                                    }
                                }
                                ImGui::SameLine();
                                ImGui::TextDisabled("%s", pv.boolVal ? "True" : "False");
                            }
                            else if (pv.type == PapyrusVarLiveInfo::VarType::kString) {
                                char strBuf[256]{};
                                std::strncpy(strBuf, pv.strVal.c_str(), sizeof(strBuf) - 1);
                                ImGui::SetNextItemWidth(-1.0f);
                                if (ImGui::InputText("##PStr", strBuf, sizeof(strBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                                    pv.strVal = strBuf;
                                    if (ApplyPapyrusVariableEdit(st.vmHandle, pv)) {
                                        m_statusMessage = std::format("Updated {}.{} (String) to '{}' in VM!", pv.scriptName, pv.varName, pv.strVal);
                                    }
                                }
                            }
                            else if (pv.type == PapyrusVarLiveInfo::VarType::kGlobalVar && pv.globalForm) {
                                pv.floatVal = pv.globalForm->value;
                                ImGui::SetNextItemWidth(130.0f);
                                if (ImGui::InputFloat("##PGlob", &pv.floatVal, 1.0f, 5.0f, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                                    pv.globalForm->value = pv.floatVal;
                                    m_statusMessage = std::format("Updated GlobalVariable '{}' ({}) to {:.2f}!", pv.varName, pv.strVal, pv.floatVal);
                                }
                                ImGui::SameLine();
                                ImGui::TextDisabled("(%s)", pv.strVal.c_str());
                            }
                            else {
                                ImGui::TextColored(ImVec4(0.72f, 0.76f, 0.82f, 1.0f), "%s", pv.strVal.c_str());
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("%s", pv.strVal.c_str());
                                }
                            }

                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
            }
        }

       // --- Start Conditions Breakdown (objConditions) & Bypass Controls ---
        if (!st.startConditions.empty()) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("Quest Start Conditions Breakdown (%zu):", st.startConditions.size());
            {
                int passCount = 0;
                std::string missing;
                for (const auto& sc : st.startConditions) {
                    if (sc.evaluatesTrue) {
                        ++passCount;
                    }
                    else {
                        missing += std::format("\n  - {}: {}{}", sc.functionName, sc.paramSummary, sc.isOR ? " (OR: alternative may suffice)" : "");
                    }
                }
                const bool allMet = passCount == static_cast<int>(st.startConditions.size());
                ImGui::TextColored(allMet ? ImVec4(0.35f, 0.95f, 0.45f, 1.0f) : ImVec4(0.95f, 0.75f, 0.30f, 1.0f),
                    "%d of %zu conditions met%s%s", passCount, st.startConditions.size(),
                    allMet ? " - quest can start." : ". Missing:", missing.c_str());
            }

            ImGui::BeginChild("StartCondChild", ImVec2(0.0f, 170.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
            for (std::size_t idx = 0; idx < st.startConditions.size(); ++idx) {
                auto& c = st.startConditions[idx];
                ImGui::PushID(static_cast<int>(idx));

                if (!c.evaluatesTrue) {
                    if (ImGui::SmallButton("Fulfill / Bypass")) {
                        FulfillSingleConditionNode(st, c);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Automatically fulfills this requirement (adds faction/item/global/stage) or patches this condition node to PASS.");
                    }
                    ImGui::SameLine();
                }

                ImVec4 condColor = c.evaluatesTrue ? ImVec4(0.35f, 0.95f, 0.45f, 1.0f) : ImVec4(0.95f, 0.40f, 0.40f, 1.0f);
                ImGui::TextColored(condColor, "[%s]", c.evaluatesTrue ? "PASS" : "FAIL");
                ImGui::SameLine();
                ImGui::Text("%s: %s (%s)", c.functionName.c_str(), c.paramSummary.c_str(), c.isOR ? "OR" : "AND");
                ImGui::TextDisabled("      Required: %s | Current: %s", c.paramSummary.c_str(), c.currentValueStr.c_str());
                ImGui::PopID();
            }
            ImGui::EndChild();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Inspect Quest in 'All Quests' Tab", ImVec2(-1.0f, 36.0f))) {
            m_selectedAllQuestID = st.questFormID;
            m_selectedPluginIdx = 0;
            m_selectedCategory = "All Categories";
            m_searchAll[0] = '\0';
            m_switchToAllQuestsTab = true;
            m_scrollAllQuestsToSelected = true;
        }
    }
    else {
        ImGui::TextDisabled("Select a quest on the left to inspect & live-edit its Papyrus VM script properties,\nTESGlobal variables, 3D target coordinates, and start condition status.");
    }
    ImGui::EndChild();
}

void QuestManager::LoadOrCreateIni() {
    m_iniLoaded = true;
    std::filesystem::path iniPath = "Data/SKSE/Plugins/QuestManager_Config.ini";
    std::error_code ec;
    std::filesystem::create_directories(iniPath.parent_path(), ec);

    if (!std::filesystem::exists(iniPath, ec)) {
        SaveIni();
        return;
    }

    std::ifstream in(iniPath);
    if (!in.is_open()) {
        return;
    }

    std::string line;
    std::string currentSection;
    while (std::getline(in, line)) {
        auto commentPos = line.find_first_of(";#");
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }
        line = TrimString(line);
        if (line.empty()) {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            currentSection = TrimString(line.substr(1, line.size() - 2));
            continue;
        }

        auto eqPos = line.find('=');
        if (eqPos == std::string::npos) {
            continue;
        }

        std::string key = TrimString(line.substr(0, eqPos));
        std::string val = TrimString(line.substr(eqPos + 1));

        if (ContainsCaseInsensitive(currentSection, "Hotkey") && ContainsCaseInsensitive(key, "ToggleKey")) {
            for (const auto& opt : kHotkeyCatalog) {
                if (ContainsCaseInsensitive(val, opt.keyName) && val.size() == std::strlen(opt.keyName)) {
                    m_toggleKeyName = opt.keyName;
                    m_toggleKeyScanCode = opt.scanCode;
                    break;
                }
            }
        }
        else if (ContainsCaseInsensitive(currentSection, "PinnedQuests") && ContainsCaseInsensitive(key, "Pinned")) {
            m_pinnedQuests.clear();
            std::stringstream ss(val);
            std::string item;
            while (std::getline(ss, item, ',')) {
                std::string trimmed = TrimString(item);
                if (!trimmed.empty()) {
                    m_pinnedQuests.insert(trimmed);
                }
            }
        }
    }
}

void QuestManager::SaveIni() {
    std::filesystem::path iniPath = "Data/SKSE/Plugins/QuestManager_Config.ini";
    std::error_code ec;
    std::filesystem::create_directories(iniPath.parent_path(), ec);

    std::ofstream out(iniPath, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return;
    }

    out << "; ============================================================================\n";
    out << "; Skyrim SKSE64 Quest Manager - Configuration File\n";
    out << "; ============================================================================\n";
    out << "; HOW TO CHANGE THE MENU HOTKEY:\n";
    out << "; Below are 35 ready-to-use hotkey options. Only ONE line should be active\n";
    out << "; (without a ';' in front). To use another key, remove the ';' in front of\n";
    out << "; your desired key and put a ';' in front of the previously active line.\n";
    out << "; ============================================================================\n\n";
    out << "[Hotkey]\n";

    for (const auto& opt : kHotkeyCatalog) {
        bool isCurrent = (m_toggleKeyScanCode == opt.scanCode);
        out << std::format("{}ToggleKey = {:<14} ; {}\n", isCurrent ? "" : "; ", opt.keyName, opt.description);
    }

    out << "\n[PinnedQuests]\n";
    out << "; Automatically managed list of pinned quests (EditorIDs or FormIDs)\n";
    out << "Pinned = ";
    bool first = true;
    for (const auto& q : m_pinnedQuests) {
        if (!first) out << ",";
        out << q;
        first = false;
    }
    out << "\n";
}

void QuestManager::SortQuestsWithPins() {
    std::sort(m_allQuests.begin(), m_allQuests.end(), [](const QuestEntry& a, const QuestEntry& b) {
        if (a.isPinned != b.isPinned) {
            return a.isPinned > b.isPinned;
        }
        if (a.name.empty() != b.name.empty()) {
            return !a.name.empty();
        }
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.editorID < b.editorID;
        });
}

void QuestManager::TogglePinQuest(RE::FormID a_formID, const std::string& a_editorID) {
    std::string key = !a_editorID.empty() ? a_editorID : std::format("0x{:08X}", a_formID);
    bool nowPinned = false;

    if (m_pinnedQuests.contains(key)) {
        m_pinnedQuests.erase(key);
        nowPinned = false;
    }
    else {
        m_pinnedQuests.insert(key);
        nowPinned = true;
    }

    for (auto& q : m_allQuests) {
        if (q.formID == a_formID) {
            q.isPinned = nowPinned;
            break;
        }
    }

    SortQuestsWithPins();
    SaveIni();
    m_statusMessage = nowPinned
        ? std::format("Pinned quest '{}' [*] to the top of the quest list!", key)
        : std::format("Unpinned quest '{}' from top.", key);
}

void QuestManager::FulfillSingleConditionNode(QuestScriptStatus& a_st, QuestConditionNodeInfo& a_nodeInfo) {
    auto* node = a_nodeInfo.rawNode;
    if (!node) {
        return;
    }

    auto fn = node->data.functionData.function.get();
    auto* form = static_cast<RE::TESForm*>(node->data.functionData.params[0]);
    float targetVal = (node->data.flags.global && node->data.comparisonValue.g)
        ? node->data.comparisonValue.g->value
        : node->data.comparisonValue.f;

    using Fn = RE::FUNCTION_DATA::FunctionID;
    if ((fn == Fn::kGetInFaction || fn == Fn::kGetPCInFaction) && form && form->GetFormType() == RE::FormType::Faction) {
        RE::FormID facID = form->GetFormID();
        SKSE::GetTaskInterface()->AddTask([this, facID]() {
            ExecuteConsoleCommand(std::format("player.addtofaction {:08X} 1", facID));
            RequestRefresh();
            });
        m_statusMessage = std::format("Added player to required Faction (0x{:08X})!", facID);
        return;
    }
    if (fn == Fn::kGetItemCount && form) {
        int count = std::max(1, static_cast<int>(targetVal));
        RE::FormID itemID = form->GetFormID();
        SKSE::GetTaskInterface()->AddTask([this, itemID, count]() {
            ExecuteConsoleCommand(std::format("player.additem {:08X} {}", itemID, count));
            RequestRefresh();
            });
        m_statusMessage = std::format("Added {}x required item (0x{:08X}) to player!", count, itemID);
        return;
    }
    if (fn == Fn::kGetGlobalValue && form) {
        if (auto* glob = form->As<RE::TESGlobal>()) {
            glob->value = targetVal;
            a_st.deepDetailsLoaded = false;
            PopulateScriptDeepDetails(a_st);
            m_statusMessage = std::format("Set Global (0x{:08X}) to required value {:.1f}!", glob->GetFormID(), targetVal);
            return;
        }
    }
    if ((fn == Fn::kGetStageDone || fn == Fn::kGetStage || fn == Fn::kGetQuestCompleted) && form) {
        if (auto* prereqQuest = form->As<RE::TESQuest>()) {
            std::string qEdID = prereqQuest->GetFormEditorID() ? prereqQuest->GetFormEditorID() : "";
            std::uint16_t stage = static_cast<std::uint16_t>(std::uintptr_t(node->data.functionData.params[1]) & 0xFFFF);
            if (stage == 0 && targetVal > 0.0f) stage = static_cast<std::uint16_t>(targetVal);

            SKSE::GetTaskInterface()->AddTask([this, prereqQuest, qEdID, stage, fn]() {
                if (!qEdID.empty()) {
                    if (fn == Fn::kGetQuestCompleted) {
                        ExecuteConsoleCommand(std::format("completequest {}", qEdID));
                    }
                    else if (stage > 0) {
                        ExecuteConsoleCommand(std::format("setstage {} {}", qEdID, stage));
                    }
                }
                if (fn == Fn::kGetQuestCompleted) {
                    prereqQuest->data.flags.set(RE::QuestFlag::kCompleted);
                }
                RequestRefresh();
                });
            m_statusMessage = std::format("Fulfilled prerequisite quest '{}'!", qEdID);
            return;
        }
    }

    // Universal fallback: Force this specific condition node to PASS in memory
    node->data.functionData.function = Fn::kGetLevel;
    node->data.functionData.params[0] = nullptr;
    node->data.functionData.params[1] = nullptr;
    node->data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kGreaterThanOrEqualTo;
    node->data.comparisonValue.f = 0.0f;
    node->data.flags.global = false;
    node->data.runOnRef.reset();

    a_nodeInfo.evaluatesTrue = true;
    a_st.deepDetailsLoaded = false;
    PopulateScriptDeepDetails(a_st);
    m_statusMessage = std::format("Forced condition '{}' to PASS in memory!", a_nodeInfo.functionName);
}

namespace {
    constexpr double kStuckSeconds = 60.0;

    void QueueRefCommand(RE::FormID a_refID, const std::string& a_command) {
        SKSE::GetTaskInterface()->AddTask([a_refID, a_command]() {
            ExecuteConsoleCommand(std::format("{:08X}.{}", a_refID, a_command));
            });
    }

    void QueueActorMoveToPlayer(RE::FormID a_refID) {
        SKSE::GetTaskInterface()->AddTask([a_refID]() {
            auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_refID);
            if (!ref) {
                return;
            }
            if (ref->IsDisabled()) {
                ref->Enable(false);
                CallNativeEnableObjectReference(ref);
            }
            ExecuteConsoleCommand(std::format("{:08X}.moveto player", a_refID));
            });
    }

    void QueueActorUnstick(RE::FormID a_refID) {
        SKSE::GetTaskInterface()->AddTask([a_refID]() {
            if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_refID)) {
                ExecuteConsoleCommand(std::format("{:08X}.setpos z {:.2f}", a_refID, ref->GetPositionZ() + 60.0f));
            }
            });
    }

    void QueueActorResurrect(RE::FormID a_refID, bool a_resetInventory) {
        SKSE::GetTaskInterface()->AddTask([a_refID, a_resetInventory]() {
            if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_refID)) {
                if (ref->IsDisabled()) {
                    ExecuteConsoleCommand(std::format("{:08X}.enable", a_refID));
                }
            }
            ExecuteConsoleCommand(std::format("{:08X}.resurrect {}", a_refID, a_resetInventory ? 1 : 0));
            });
    }

    void ClearActorProtection(RE::Actor* a_actor) {
        if (!a_actor) {
            return;
        }
        a_actor->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kEssential, RE::Actor::BOOL_FLAGS::kProtected);
        if (auto* base = a_actor->GetActorBase()) {
            base->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kEssential, RE::ACTOR_BASE_DATA::Flag::kProtected);
        }
    }

    void QueueActorKill(RE::FormID a_refID) {
        SKSE::GetTaskInterface()->AddTask([a_refID]() {
            ExecuteConsoleCommand(std::format("{:08X}.setghost 0", a_refID));
            ClearActorProtection(RE::TESForm::LookupByID<RE::Actor>(a_refID));
            ExecuteConsoleCommand(std::format("{:08X}.kill", a_refID));
            });
    }

    void QueueActorResetAI(RE::FormID a_refID) {
        SKSE::GetTaskInterface()->AddTask([a_refID]() {
            ExecuteConsoleCommand(std::format("{:08X}.stopcombat", a_refID));
            ExecuteConsoleCommand(std::format("{:08X}.stopcombatalarm", a_refID));
            ExecuteConsoleCommand(std::format("{:08X}.resetai", a_refID));
            ExecuteConsoleCommand(std::format("{:08X}.evp", a_refID));
            });
    }

    void QueueActorRespawnCycle(RE::FormID a_refID) {
        SKSE::GetTaskInterface()->AddTask([a_refID]() {
            ExecuteConsoleCommand(std::format("{:08X}.disable", a_refID));
            });
        SKSE::GetTaskInterface()->AddTask([a_refID]() {
            ExecuteConsoleCommand(std::format("{:08X}.enable", a_refID));
            });
    }

    std::string DescribeActorRef(RE::TESObjectREFR* a_ref) {
        if (!a_ref) {
            return "Unnamed Reference";
        }
        std::string name = a_ref->GetDisplayFullName() ? a_ref->GetDisplayFullName() : "";
        if (name.empty() && a_ref->GetBaseObject() && a_ref->GetBaseObject()->GetName()) {
            name = a_ref->GetBaseObject()->GetName();
        }
        if (name.empty() && a_ref->GetFormEditorID()) {
            name = a_ref->GetFormEditorID();
        }
        return name.empty() ? "Unnamed Reference" : name;
    }
}

void QuestManager::ScanActorRefs() {
    m_actorRefs.clear();
    m_selectedActorRefIdx = -1;

    for (const auto& entry : m_allQuests) {
        auto* quest = entry.quest;
        if (!quest) {
            continue;
        }
        if (m_actorOnlyJournalQuests && !entry.isActiveInJournal) {
            continue;
        }

        std::vector<std::pair<std::uint32_t, RE::ObjectRefHandle>> handles;
        {
            RE::BSReadLockGuard lock(quest->aliasAccessLock);
            for (const auto& [aliasID, handle] : quest->refAliasMap) {
                handles.emplace_back(aliasID, handle);
            }
        }

        for (auto* al : quest->aliases) {
            if (!al) {
                continue;
            }
            const bool alreadyFilled = std::any_of(handles.begin(), handles.end(),
                [&](const auto& h) { return h.first == al->aliasID && h.second.get(); });
            if (alreadyFilled) {
                continue;
            }
            auto* staticRef = ResolveStaticAliasReference(quest, al);
            if (staticRef) {
                handles.emplace_back(al->aliasID, staticRef->CreateRefHandle());
            }
        }

        for (const auto& [aliasID, handle] : handles) {
            auto refPtr = handle.get();
            if (!refPtr) {
                continue;
            }
            auto* actor = skyrim_cast<RE::Actor*>(refPtr.get());
            if (!actor || actor->IsPlayerRef()) {
                continue;
            }

            ActorRefEntry e{};
            e.questFormID = entry.formID;
            e.questName = entry.name;
            e.questEditorID = entry.editorID;
            e.aliasID = aliasID;
            e.aliasName = std::format("Alias #{}", aliasID);
            for (auto* al : quest->aliases) {
                if (al && al->aliasID == aliasID && !al->aliasName.empty()) {
                    e.aliasName = al->aliasName.c_str();
                    break;
                }
            }
            e.refFormID = actor->GetFormID();
            e.refName = DescribeActorRef(actor);
            m_actorRefs.push_back(std::move(e));
        }
    }

    m_actorRefsScanned = true;
    m_statusMessage = std::format("Found {} actor reference(s) bound to quest aliases.", m_actorRefs.size());
}

void QuestManager::QueueActorVerify(RE::FormID a_refID, std::string a_label, std::function<bool(RE::Actor*)> a_check) {
    std::thread([this, a_refID, a_label = std::move(a_label), a_check = std::move(a_check)]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        SKSE::GetTaskInterface()->AddTask([this, a_refID, a_label, a_check]() {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_refID);
            if (!actor) {
                m_statusMessage = std::format("{}: actor no longer available.", a_label);
                return;
            }
            m_statusMessage = a_check(actor)
                ? std::format("{}: done.", a_label)
                : std::format("{}: no effect after 0.7s (reverted by a quest script / alias flag?).", a_label);
            });
        }).detach();
}

void QuestManager::OpenInActorsTab(const QuestEntry& a_entry, std::uint32_t a_aliasID) {
    if (!a_entry.isActiveInJournal) {
        m_actorOnlyJournalQuests = false;
    }
    m_actorOnlyProblems = false;
    m_searchActorRefs[0] = '\0';
    ScanActorRefs();

    for (std::size_t i = 0; i < m_actorRefs.size(); ++i) {
        if (m_actorRefs[i].questFormID == a_entry.formID && m_actorRefs[i].aliasID == a_aliasID) {
            m_selectedActorRefIdx = static_cast<int>(i);
            break;
        }
    }
    if (m_selectedActorRefIdx < 0 && a_entry.quest) {
        for (auto* al : a_entry.quest->aliases) {
            if (!al || al->aliasID != a_aliasID) {
                continue;
            }
            auto* staticRef = ResolveStaticAliasReference(a_entry.quest, al);
            auto* staticActor = staticRef ? skyrim_cast<RE::Actor*>(staticRef) : nullptr;
            if (staticActor) {
                ActorRefEntry e{};
                e.questFormID = a_entry.formID;
                e.questName = a_entry.name;
                e.questEditorID = a_entry.editorID;
                e.aliasID = a_aliasID;
                e.aliasName = al->aliasName.empty() ? std::format("Alias #{}", a_aliasID) : std::string(al->aliasName.c_str());
                e.refFormID = staticActor->GetFormID();
                e.refName = DescribeActorRef(staticActor);
                m_actorRefs.push_back(std::move(e));
                m_selectedActorRefIdx = static_cast<int>(m_actorRefs.size()) - 1;
            }
            break;
        }
    }
    if (m_selectedActorRefIdx > 0 && m_selectedActorRefIdx < static_cast<int>(m_actorRefs.size())) {
        std::rotate(m_actorRefs.begin(), m_actorRefs.begin() + m_selectedActorRefIdx, m_actorRefs.begin() + m_selectedActorRefIdx + 1);
        m_selectedActorRefIdx = 0;
    }
    m_scrollActorToSelected = m_selectedActorRefIdx >= 0;
    m_switchToActorsTab = true;
}

void QuestManager::RenderActorRefsTab() {
    if (!m_actorRefsScanned) {
        ScanActorRefs();
    }

    struct Diag {
        RE::Actor* actor = nullptr;
        bool missing = false;
        bool dead = false;
        bool disabled = false;
        bool unloaded = false;
        bool stuck = false;
        double idleSeconds = 0.0;
        float distance = 0.0f;
    };

    const double now = ImGui::GetTime();
    auto* player = RE::PlayerCharacter::GetSingleton();

    auto diagnose = [&](RE::FormID a_id) {
        Diag d{};
        d.actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
        if (!d.actor || d.actor->IsDeleted()) {
            d.missing = true;
            return d;
        }
        d.dead = d.actor->IsDead();
        d.disabled = d.actor->IsDisabled();
        d.unloaded = !d.actor->Is3DLoaded();
        if (player) {
            d.distance = player->GetPosition().GetDistance(d.actor->GetPosition());
        }

        auto& track = m_actorTracks[a_id];
        const auto pos = d.actor->GetPosition();
        if (track.sinceTime == 0.0 || track.lastPos.GetDistance(pos) > 5.0f) {
            track.lastPos = pos;
            track.sinceTime = now;
        }
        d.idleSeconds = now - track.sinceTime;
        d.stuck = !d.dead && !d.disabled && !d.unloaded && d.idleSeconds >= kStuckSeconds;
        return d;
        };

    auto isProblem = [](const Diag& d) {
        return d.missing || d.dead || d.disabled || d.stuck;
        };

    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##ActorSearch", "Search actor / quest / alias...", m_searchActorRefs, sizeof(m_searchActorRefs));
    ImGui::SameLine();
    if (ImGui::Button("Rescan Actors")) {
        ScanActorRefs();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Only journal quests", &m_actorOnlyJournalQuests)) {
        ScanActorRefs();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Only quests that are currently in your journal.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Only problem actors", &m_actorOnlyProblems);

    ImGui::Separator();

    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
        };
    const std::string needle = lower(m_searchActorRefs);

    ImGui::BeginChild("ActorListChild", ImVec2(560.0f, 0.0f), true);
    for (std::size_t i = 0; i < m_actorRefs.size(); ++i) {
        const auto& e = m_actorRefs[i];
        if (!needle.empty()) {
            const std::string hay = lower(e.refName + " " + e.questName + " " + e.questEditorID + " " + e.aliasName);
            if (hay.find(needle) == std::string::npos) {
                continue;
            }
        }
        const Diag d = diagnose(e.refFormID);
        if (m_actorOnlyProblems && !isProblem(d)) {
            continue;
        }

        ImGui::PushID(static_cast<int>(i));
        const char* badge = d.missing ? "MISSING" : d.dead ? "DEAD" : d.disabled ? "DISABLED" : d.unloaded ? "UNLOADED" : d.stuck ? "STUCK?" : "OK";
        ImVec4 col = (d.missing || d.dead) ? ImVec4(0.95f, 0.30f, 0.30f, 1.0f)
            : (d.disabled || d.stuck) ? ImVec4(0.95f, 0.65f, 0.25f, 1.0f)
            : d.unloaded ? ImVec4(0.60f, 0.70f, 0.90f, 1.0f)
            : ImVec4(0.35f, 0.95f, 0.45f, 1.0f);
        ImGui::TextColored(col, "[%s]", badge);
        ImGui::SameLine();
        const std::string label = std::format("{} - {} / {}", e.refName, e.questName.empty() ? e.questEditorID : e.questName, e.aliasName);
        if (m_scrollActorToSelected && m_selectedActorRefIdx == static_cast<int>(i)) {
            ImGui::SetScrollHereY(0.0f);
            m_scrollActorToSelected = false;
        }
        if (ImGui::Selectable(label.c_str(), m_selectedActorRefIdx == static_cast<int>(i))) {
            m_selectedActorRefIdx = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("ActorDetailChild", ImVec2(0.0f, 0.0f), true);

    if (m_selectedActorRefIdx < 0 || m_selectedActorRefIdx >= static_cast<int>(m_actorRefs.size())) {
        ImGui::TextDisabled("Select an actor from the list.");
        ImGui::EndChild();
        return;
    }

    const ActorRefEntry sel = m_actorRefs[m_selectedActorRefIdx];
    const Diag d = diagnose(sel.refFormID);

    ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "%s", sel.refName.c_str());
    ImGui::Text("Ref: 0x%08X", sel.refFormID);
    ImGui::Text("Quest: %s (%s)", sel.questName.c_str(), sel.questEditorID.c_str());
    ImGui::Text("Alias: [ID %u] %s", sel.aliasID, sel.aliasName.c_str());
    ImGui::Separator();

    if (d.missing || !d.actor) {
        ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.30f, 1.0f), "Reference no longer exists (deleted / not resolvable).");
        ImGui::EndChild();
        return;
    }

    RE::Actor* actor = d.actor;
    const auto pos = actor->GetPosition();
    const char* cellName = (actor->GetParentCell() && actor->GetParentCell()->GetName()) ? actor->GetParentCell()->GetName() : "<none>";
    const float health = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);

    ImGui::Text("State: %s | %s | %s", d.dead ? "DEAD" : "Alive", d.disabled ? "Disabled" : "Enabled", d.unloaded ? "3D unloaded" : "3D loaded");
    ImGui::Text("Health: %.1f", health);
    ImGui::Text("Cell: %s", cellName);
    ImGui::Text("Position: %.1f, %.1f, %.1f", pos.x, pos.y, pos.z);
    ImGui::Text("Distance to player: %.0f", d.distance);
    ImGui::Text("In combat: %s", actor->IsInCombat() ? "Yes" : "No");
    ImGui::Text("Idle (not moved): %.0fs%s", d.idleSeconds, d.stuck ? "  -> possibly stuck" : "");

    bool essential = false;
    bool prot = false;
    if (auto* base = actor->GetActorBase()) {
        essential = base->actorData.actorBaseFlags.any(RE::ACTOR_BASE_DATA::Flag::kEssential);
        prot = base->actorData.actorBaseFlags.any(RE::ACTOR_BASE_DATA::Flag::kProtected);
    }
    essential = essential || actor->GetActorRuntimeData().boolFlags.any(RE::Actor::BOOL_FLAGS::kEssential);
    prot = prot || actor->GetActorRuntimeData().boolFlags.any(RE::Actor::BOOL_FLAGS::kProtected);

    ImGui::Separator();

    const RE::FormID id = sel.refFormID;

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.62f, 0.32f, 1.0f));
    if (ImGui::Button("Quick Fix", ImVec2(180.0f, 34.0f))) {
        if (d.dead) {
            QueueActorResurrect(id, m_actorResetInventory);
            QueueActorMoveToPlayer(id);
            m_statusMessage = std::format("Quick Fix: resurrecting and moving '{}' to player.", sel.refName);
        }
        else if (d.disabled) {
            QueueRefCommand(id, "enable");
            m_statusMessage = std::format("Quick Fix: enabling '{}'.", sel.refName);
        }
        else if (d.unloaded) {
            QueueActorMoveToPlayer(id);
            m_statusMessage = std::format("Quick Fix: moving unloaded '{}' to player.", sel.refName);
        }
        else {
            QueueActorResetAI(id);
            QueueActorUnstick(id);
            m_statusMessage = std::format("Quick Fix: reset AI and unstuck '{}'.", sel.refName);
        }
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Dead: Resurrect + Move to Player\nDisabled: Enable\nUnloaded: Move to Player\nOtherwise: Reset AI + Unstick");
    }

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.35f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.46f, 0.16f, 1.0f));
    if (ImGui::Button("Full Reset", ImVec2(180.0f, 34.0f))) {
        QueueActorRespawnCycle(id);
        QueueActorResurrect(id, m_actorResetInventory);
        QueueActorResetAI(id);
        ForceFillQuestAliases(RE::TESForm::LookupByID<RE::TESQuest>(sel.questFormID));
        m_statusMessage = std::format("Full Reset: disable/enable, resurrect and AI reset for '{}'.", sel.refName);
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Disable -> Enable -> Resurrect -> Reset AI -> Re-fill aliases");
    }

    ImGui::Spacing();
    ImGui::Checkbox("Reset inventory when resurrecting", &m_actorResetInventory);

	auto actionBtn = [](const char* a_label, bool a_enabled, const char* a_tooltip) {
		const ImVec2 btn(170.0f, 28.0f);
		if (!a_enabled) {
			ImGui::BeginDisabled();
		}
		const bool clicked = ImGui::Button(a_label, btn);
		if (!a_enabled) {
			ImGui::EndDisabled();
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("%s%s", a_tooltip, a_enabled ? "" : "\n(Not available: no effect in the current state.)");
		}
		return clicked && a_enabled;
		};

	if (ImGui::CollapsingHeader("Single Actions", ImGuiTreeNodeFlags_DefaultOpen)) {
		const bool inCombat = actor->IsInCombat();

		if (actionBtn("Resurrect", d.dead, "Revives a dead actor (and enables him if disabled).")) {
			QueueActorResurrect(id, m_actorResetInventory);
			QueueActorVerify(id, "Resurrect", [](RE::Actor* a) { return !a->IsDead(); });
			m_statusMessage = std::format("Resurrecting '{}'...", sel.refName);
		}
		ImGui::SameLine();
		if (actionBtn("Kill", !d.dead, "Kills the actor.")) {
			QueueActorKill(id);
            QueueActorVerify(id, "Kill", [](RE::Actor* a) { return a->IsDead(); });
            m_statusMessage = std::format("Killing '{}'...", sel.refName);
		}

		if (actionBtn("Disable", !d.disabled, "Removes the actor from the world (close the menu to see it).")) {
			QueueRefCommand(id, "disable");
			QueueActorVerify(id, "Disable", [](RE::Actor* a) { return a->IsDisabled(); });
			m_statusMessage = std::format("Disabling '{}'...", sel.refName);
		}
		ImGui::SameLine();
		if (actionBtn("Enable", d.disabled, "Brings a disabled actor back into the world.")) {
			QueueRefCommand(id, "enable");
			QueueActorVerify(id, "Enable", [](RE::Actor* a) { return !a->IsDisabled(); });
			m_statusMessage = std::format("Enabling '{}'...", sel.refName);
		}
		ImGui::SameLine();
		if (actionBtn("Disable -> Enable", !d.disabled, "Re-spawns the actor: disables and re-enables him.\nFixes broken 3D / stuck states.")) {
			QueueActorRespawnCycle(id);
			QueueActorVerify(id, "Disable -> Enable", [](RE::Actor* a) { return !a->IsDisabled(); });
			m_statusMessage = std::format("Re-spawning '{}'...", sel.refName);
		}

		if (actionBtn("Move to Player", true, "Teleports the actor to your position.")) {
			QueueActorMoveToPlayer(id);
			m_statusMessage = std::format("Moving '{}' to player.", sel.refName);
		}
		ImGui::SameLine();
		if (actionBtn("Teleport to Actor", true, "Teleports you to the actor.")) {
			TeleportToSpecificAlias(RE::TESForm::LookupByID<RE::TESQuest>(sel.questFormID), sel.aliasID, sel.aliasName);
		}
		ImGui::SameLine();
		if (actionBtn("Unstick", true, "Lifts the actor 60 units up to free him from geometry.")) {
			QueueActorUnstick(id);
			m_statusMessage = std::format("Lifting '{}' out of geometry.", sel.refName);
		}

		if (actionBtn("Reset AI", true, "Re-evaluates packages and resets the AI.\nOnly visible on actors stuck in an idle, scene or package.")) {
			QueueActorResetAI(id);
			m_statusMessage = std::format("Reset AI of '{}'.", sel.refName);
		}
		ImGui::SameLine();
		if (actionBtn("Stop Combat", inCombat, "Ends the actor's combat.\nHostile actors who still see you will re-engage; use Pacify for that.")) {
			QueueRefCommand(id, "stopcombat");
			QueueRefCommand(id, "stopcombatalarm");
			QueueActorVerify(id, "Stop Combat", [](RE::Actor* a) { return !a->IsInCombat(); });
			m_statusMessage = std::format("Stopping combat for '{}'...", sel.refName);
		}
		ImGui::SameLine();
		if (actionBtn("Pacify", inCombat, "Stop Combat + Aggression 0 + Ghost.\nKeeps a hostile actor calm and unable to die.")) {
			QueueRefCommand(id, "stopcombat");
			QueueRefCommand(id, "stopcombatalarm");
			QueueRefCommand(id, "setav aggression 0");
			QueueRefCommand(id, "setghost 1");
			QueueActorVerify(id, "Pacify", [](RE::Actor* a) { return !a->IsInCombat(); });
			m_statusMessage = std::format("Pacifying '{}'...", sel.refName);
		}

		if (actionBtn("Re-bind Alias", true, "Forces this actor back into its quest alias.")) {
			if (auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(sel.questFormID)) {
				RE::ObjectRefHandle newHandle(actor);
				RE::BSWriteLockGuard writeLock(quest->aliasAccessLock);
				auto it = quest->refAliasMap.find(sel.aliasID);
				if (it != quest->refAliasMap.end()) {
					it->second = newHandle;
				}
				else {
					quest->refAliasMap.emplace(sel.aliasID, newHandle);
				}
				m_statusMessage = std::format("Re-bound alias '{}' to '{}'.", sel.aliasName, sel.refName);
			}
		}
		ImGui::SameLine();
		if (actionBtn("Ghost On", true, "Actor takes no damage and cannot die.\nNo visible change; only noticeable when attacking him.")) {
			QueueRefCommand(id, "setghost 1");
			m_statusMessage = std::format("Ghost mode enabled for '{}'.", sel.refName);
		}
		ImGui::SameLine();
		if (actionBtn("Ghost Off", true, "Removes ghost mode (actor can take damage again).")) {
			QueueRefCommand(id, "setghost 0");
			m_statusMessage = std::format("Ghost mode disabled for '{}'.", sel.refName);
		}
	}

    if (ImGui::CollapsingHeader("Protection", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto* base = actor->GetActorBase();
        if (base) {
            if (ImGui::Checkbox("Essential", &essential)) {
                base->actorData.actorBaseFlags.set(essential, RE::ACTOR_BASE_DATA::Flag::kEssential);
                actor->GetActorRuntimeData().boolFlags.set(essential, RE::Actor::BOOL_FLAGS::kEssential);
                m_statusMessage = std::format("Essential {} for '{}'.", essential ? "enabled" : "disabled", sel.refName);
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("Protected", &prot)) {
                base->actorData.actorBaseFlags.set(prot, RE::ACTOR_BASE_DATA::Flag::kProtected);
                actor->GetActorRuntimeData().boolFlags.set(prot, RE::Actor::BOOL_FLAGS::kProtected);
                m_statusMessage = std::format("Protected {} for '{}'.", prot ? "enabled" : "disabled", sel.refName);
            }
            ImGui::TextDisabled("Affects the base NPC form (all refs sharing this base).");
        }
    }

    ImGui::EndChild();
}

void QuestManager::RenderUI() {
    if (m_needsRefresh.load()) {
        RefreshQuests();
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    ImGui::SetNextWindowSize(ImVec2(1560.0f, 810.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(1280.0f, 720.0f), ImVec2(FLT_MAX, FLT_MAX));
    bool open = D3DHook::IsMenuOpen();

    std::string winTitle = std::format("Skyrim Quest Manager (Press {} to Hide)###QuestManagerMainWin", m_toggleKeyName);
    if (ImGui::Begin(winTitle.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("QuestManagerTabs")) {
            if (ImGui::BeginTabItem("Active Quests (In Journal)")) {
                RenderActiveQuestsTab();
                ImGui::EndTabItem();
            }

            ImGuiTabItemFlags allQuestsFlags = m_switchToAllQuestsTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("All Quests (Game, DLCs & Mods)", nullptr, allQuestsFlags)) {
                m_switchToAllQuestsTab = false;
                RenderAllQuestsTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Rewards & Prerequisites Explorer")) {
                RenderItemsAndRewardsTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Script & Condition Monitor")) {
                RenderScriptMonitorTab();
                ImGui::EndTabItem();
            }

            ImGuiTabItemFlags actorsFlags = m_switchToActorsTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem("Actors / Refs", nullptr, actorsFlags)) {
                m_switchToActorsTab = false;
                RenderActorRefsTab();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f), "Status: %s",
            m_statusMessage.empty() ? "Ready." : m_statusMessage.c_str());
    }
    ImGui::End();

    if (!open) {
        D3DHook::SetMenuOpen(false);
    }
}
