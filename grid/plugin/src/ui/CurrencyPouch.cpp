#include "ui/CurrencyPouch.h"

#include "api/HostApi.h"
#include "ui/Fallback.h"
#include "ui/Grid.h"
#include "ui/LootBarter.h"
#include "ui/Sfx.h"
#include "ui/Theme.h"
#include "ui/UIRoot.h"
#include "ui/WinManager.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace FUI::CurrencyPouch
{
    namespace
    {
        constexpr const char* kUiIni = "Data/SKSE/Plugins/GridInventory_ui.ini";

        // ---- settings (read once) ------------------------------------------
        struct Settings
        {
            bool  enabled = true;      // CurrencyPouch = 1
            int   hotkey = 0;          // CurrencyPouchHotkey = P (a letter), 0 = none
            float x = -1.0f;           // CurrencyPouchX / Y: first-open position, -1 = beside the inventory
            float y = -1.0f;
            bool  debugBoard = false;  // CurrencyPouchDebugLog = 1: a line per board refresh
            bool  debugUi = false;     // UiStateDebugLog = 1: a line per UI-state reset
        };

        const Settings& Cfg()
        {
            static const Settings s = [] {
                Settings out;
                std::ifstream in{ kUiIni };
                std::string   line;
                auto trim = [](std::string a_s) {
                    a_s.erase(0, a_s.find_first_not_of(" \t"));
                    a_s.erase(a_s.find_last_not_of(" \t\r") + 1);
                    return a_s;
                };
                while (std::getline(in, line)) {
                    if (const auto c = line.find_first_of(";#"); c != std::string::npos) line.erase(c);
                    const auto eq = line.find('=');
                    if (eq == std::string::npos) continue;
                    const std::string key = trim(line.substr(0, eq));
                    const std::string val = trim(line.substr(eq + 1));
                    if (key == "CurrencyPouch") {
                        out.enabled = !(val == "0" || val == "false" || val == "off");
                    } else if (key == "CurrencyPouchHotkey") {
                        if (val.size() == 1 && std::isalpha(static_cast<unsigned char>(val[0]))) {
                            out.hotkey = ImGuiKey_A + (std::toupper(static_cast<unsigned char>(val[0])) - 'A');
                        }
                    } else if (key == "CurrencyPouchX") {
                        try { out.x = std::stof(val); } catch (...) {}
                    } else if (key == "CurrencyPouchY") {
                        try { out.y = std::stof(val); } catch (...) {}
                    } else if (key == "CurrencyPouchDebugLog") {
                        out.debugBoard = val == "1" || val == "true" || val == "on";
                    } else if (key == "UiStateDebugLog") {
                        out.debugUi = val == "1" || val == "true" || val == "on";
                    }
                }
                SKSE::log::info("[CURRENCY] pouch {}, hotkey {}", out.enabled ? "on" : "OFF",
                    out.hotkey ? std::string(1, static_cast<char>('A' + (out.hotkey - ImGuiKey_A))) : std::string("none"));
                return out;
            }();
            return s;
        }

        // ---- the currency forms -------------------------------------------
        std::mutex                 g_lock;
        std::vector<std::uint32_t> g_forms;          // FormID order: fixed slot order
        std::unordered_map<std::uint32_t, int> g_counts;
        bool                       g_formsKnown = false;
        std::vector<std::pair<std::uint32_t, int>> g_dropQ;   // ground drops, game thread
        std::set<std::uint32_t>    g_warned;
        std::atomic<bool>          g_reported{ false };
        bool                       g_open = false;
        std::uint32_t              g_drag = 0;       // slot being dragged out

        void EnsureForms()
        {
            if (g_formsKnown) return;
            const auto* ap = HostApi::ApplierTable();
            auto*       dh = RE::TESDataHandler::GetSingleton();
            if (!ap || !dh) return;
            std::vector<std::uint32_t> forms;
            for (auto* misc : dh->GetFormArray<RE::TESObjectMISC>()) {
                if (misc && ap->IsCurrency(ap->self, misc->GetFormID())) {
                    forms.push_back(misc->GetFormID());
                }
            }
            std::sort(forms.begin(), forms.end());
            std::scoped_lock lock{ g_lock };
            g_forms = std::move(forms);
            g_formsKnown = true;
            SKSE::log::info("[CURRENCY] {} currency form(s) claimed by the applier", g_forms.size());
        }

        // RENDER THREAD (first draw): every icon loaded now, the report sent.
        void PreloadAndReport()
        {
            if (g_reported.load()) return;
            EnsureForms();
            std::vector<std::uint32_t> forms;
            {
                std::scoped_lock lock{ g_lock };
                forms = g_forms;
            }
            if (forms.empty()) return;

            GridInvAPI::CurrencyReport report{};
            report.structSize = sizeof(report);
            report.abiVersion = GridInvAPI::kABIVersion;
            report.pouchActive = Cfg().enabled ? 1u : 0u;
            for (const auto id : forms) {
                if (report.count >= GridInvAPI::kMaxCurrencyRows) break;
                auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(id);
                if (!obj) continue;
                const auto def = Grid::ResolveDef(obj);
                auto&      row = report.rows[report.count++];
                row.base = id;
                row.w = static_cast<std::uint8_t>(def.w);
                row.h = static_cast<std::uint8_t>(def.h);
                row.iconSource = IconOf(obj) ? GridInvAPI::kCurrencyIconPng : GridInvAPI::kCurrencyIconNone;
            }
            SKSE::GetMessagingInterface()->Dispatch(GridInvAPI::kMsgCurrencyReport, &report,
                sizeof(report), nullptr);
            g_reported.store(true);
            SKSE::log::info("[CURRENCY] report sent: {} currency row(s), pouch {}", report.count,
                report.pouchActive ? "on" : "off");
        }
    }

    bool IsCurrencyId(std::uint32_t a_base)
    {
        const auto* ap = HostApi::ApplierTable();
        return a_base != 0 && ap && ap->IsCurrency(ap->self, a_base);
    }

    bool IsCurrency(const RE::TESBoundObject* a_obj)
    {
        return a_obj && IsCurrencyId(a_obj->GetFormID());
    }

    const IconCache::Icon* IconOf(RE::TESBoundObject* a_obj)
    {
        if (!a_obj) return nullptr;
        const auto* icon = Fallback::GetItemOnly(a_obj);
        if (!icon) {
            std::scoped_lock lock{ g_lock };
            if (g_warned.insert(a_obj->GetFormID()).second) {
                SKSE::log::warn("[CURRENCY] icon lookup MISSED for currency {:08X} ('{}'): expected "
                                "GridInventory_fallback/item/{} -- drawing nothing rather than a "
                                "capture or a category icon",
                    a_obj->GetFormID(), a_obj->GetName() ? a_obj->GetName() : "?",
                    Fallback::ItemFileName(a_obj));
            }
        }
        return icon;
    }

    bool Enabled()
    {
        return Cfg().enabled && HostApi::ApplierTable() != nullptr;
    }

    bool HidingNow()
    {
        return Enabled() && LootBarter::CurrentMode() == LootBarter::Mode::kNormal;
    }

    bool HidesFromBoard(const RE::TESBoundObject* a_obj)
    {
        // MISC only: every currency is one, and nothing else can be hidden by
        // an applier that answers too broadly.
        return a_obj && a_obj->Is(RE::FormType::Misc) && HidingNow() && IsCurrency(a_obj);
    }

    bool DebugBoard() { return Cfg().debugBoard; }
    bool DebugUiState() { return Cfg().debugUi; }

    void ResetTransient(const char* a_why)
    {
        std::size_t dropped = 0;
        {
            std::scoped_lock lock{ g_lock };
            dropped = g_dropQ.size();
            g_dropQ.clear();
        }
        const bool dragging = g_drag != 0;
        g_drag = 0;
        if (Cfg().debugUi) {
            SKSE::log::info("[UISTATE] pouch reset ({}): drag {}, {} queued drop(s) dropped",
                a_why ? a_why : "?", dragging ? "cancelled" : "none", dropped);
        }
    }

    void Tick()
    {
        if (!Enabled()) return;
        // ★FAIL SAFE (2.1.1): nothing here runs while the world is changing --
        // the queued drop below puts a reference into the player's cell.
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded()) return;
        if (const auto* cell = player->GetParentCell(); !cell || !cell->IsAttached()) return;
        if (auto* ui = RE::UI::GetSingleton(); !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return;
        EnsureForms();

        std::vector<std::pair<std::uint32_t, int>> drops;
        std::vector<std::uint32_t>                 forms;
        {
            std::scoped_lock lock{ g_lock };
            drops.swap(g_dropQ);
            forms = g_forms;
        }
        for (const auto& [id, n] : drops) {
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(id);
            if (obj && n > 0) {
                // ★THE ENGINE'S OWN DROP: one reference carrying the count,
                // weight and every other mod see an ordinary drop.
                player->RemoveItem(obj, n, RE::ITEM_REMOVE_REASON::kDropping, nullptr, nullptr);
                SKSE::log::info("[CURRENCY] dropped {}x {:08X} from the pouch", n, id);
            }
        }

        // Counts, read off a COPY of the inventory (GetInventory), each tick.
        std::unordered_map<std::uint32_t, int> counts;
        const std::set<std::uint32_t>          wanted(forms.begin(), forms.end());
        for (auto& [obj, data] : player->GetInventory([&](RE::TESBoundObject& a_o) {
                 return wanted.contains(a_o.GetFormID());
             })) {
            if (obj) counts[obj->GetFormID()] += data.first;
        }
        std::scoped_lock lock{ g_lock };
        g_counts = std::move(counts);
    }

    bool Close()
    {
        if (!g_open) return false;
        g_open = false;
        return true;
    }

    void Draw()
    {
        if (!Enabled()) return;
        // ★FAIL SAFE (2.1.1): only over a live board, with a player.
        if (!UIRoot::IsBoardLive() || !RE::PlayerCharacter::GetSingleton()) {
            g_drag = 0;
            return;
        }
        PreloadAndReport();
        // ★ONLY ON THE PLAYER'S OWN INVENTORY: never over a container or a shop.
        if (LootBarter::CurrentMode() != LootBarter::Mode::kNormal) return;

        const float S = Theme::Scale();
        const auto& sk = Theme::S();
        auto*       wm = WinManager::GetSingleton();
        const bool  typing = ImGui::GetIO().WantTextInput;
        if (Cfg().hotkey && !typing && ImGui::IsKeyPressed(static_cast<ImGuiKey>(Cfg().hotkey), false)) {
            g_open = !g_open;
            g_open ? Sfx::SelectOn() : Sfx::SelectOff();
        }

        // ---- the toggle button, on the inventory window's right edge -------
        ImVec2 mainPos(100.0f, 100.0f);
        ImVec2 mainSize(400.0f, 400.0f);
        if (auto* w = wm->Find("main")) {
            mainPos = w->pos;
            mainSize = w->size;
        }
        const ImVec2 btn(30.0f * S, 30.0f * S);
        // ★2.1.1: a word under the button, so it reads as the pouch at a glance.
        constexpr const char* kToggleLabel = "Orbs";
        const ImVec2 labelSize = ImGui::CalcTextSize(kToggleLabel);
        const float  toggleW = (std::max)(btn.x, labelSize.x) + 8.0f * S;
        ImGui::SetNextWindowPos(ImVec2(mainPos.x + mainSize.x + 4.0f * S, mainPos.y + 40.0f * S));
        ImGui::SetNextWindowSize(ImVec2(toggleW, btn.y + labelSize.y + 12.0f * S));
        ImGui::Begin("##currency_toggle", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoBackground);
        UIRoot::NoteOverlayRect();
        {
            std::vector<std::uint32_t> forms;
            {
                std::scoped_lock lock{ g_lock };
                forms = g_forms;
            }
            const IconCache::Icon* face = nullptr;
            if (!forms.empty()) {
                face = IconOf(RE::TESForm::LookupByID<RE::TESBoundObject>(forms.front()));
            }
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##pouchbtn", btn)) {
                g_open = !g_open;
                g_open ? Sfx::SelectOn() : Sfx::SelectOff();
            }
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + btn.x, p.y + btn.y), IM_COL32(20, 18, 24, 220), 4.0f);
            dl->AddRect(p, ImVec2(p.x + btn.x, p.y + btn.y),
                ImGui::IsItemHovered() || g_open ? Theme::Acc(0.9f) : Theme::Acc(0.45f), 4.0f);
            if (face && face->srv) {
                dl->AddImage(reinterpret_cast<ImTextureID>(face->srv), ImVec2(p.x + 3.0f * S, p.y + 3.0f * S),
                    ImVec2(p.x + btn.x - 3.0f * S, p.y + btn.y - 3.0f * S));
            }
            const bool btnHovered = ImGui::IsItemHovered();
            dl->AddText(ImVec2(p.x + (btn.x - labelSize.x) * 0.5f, p.y + btn.y + 2.0f * S),
                btnHovered || g_open ? IM_COL32(240, 235, 220, 255) : IM_COL32(200, 195, 180, 220), kToggleLabel);
            if (btnHovered) {
                Theme::PushTipStyle();
                ImGui::SetTooltip("%s the currency pouch%s", g_open ? "Close" : "Open",
                    Cfg().hotkey ? std::string(" (")
                    .append(1, static_cast<char>('A' + (Cfg().hotkey - ImGuiKey_A))).append(")").c_str() : "");
                Theme::PopTipStyle();
            }
        }
        ImGui::End();

        if (!g_open) return;

        // ---- the pouch window ---------------------------------------------
        std::vector<std::uint32_t>             forms;
        std::unordered_map<std::uint32_t, int> counts;
        {
            std::scoped_lock lock{ g_lock };
            forms = g_forms;
            counts = g_counts;
        }
        constexpr int kCols = 3;
        const float   cell = 52.0f * S;
        const float   gap = 6.0f * S;
        const int     rows = (static_cast<int>(forms.size()) + kCols - 1) / kCols;
        const float   topPad = Theme::TitleTopPad();
        // ★2.1.1: a hint line and the "Deposit all" button under the slots.
        const float   footerH = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() + 6.0f * S;
        const ImVec2  size(kCols * cell + (kCols - 1) * gap + 2.0f * Theme::FrameInsetX() + 16.0f * S,
            WinManager::TitleBarH() + (std::max)(1, rows) * (cell + gap) + footerH + 2.0f * Theme::FrameInsetY() + 16.0f * S);
        const ImVec2 defPos = Cfg().x >= 0.0f && Cfg().y >= 0.0f
                                  ? ImVec2(Cfg().x, Cfg().y)
                                  : ImVec2(mainPos.x + mainSize.x + 44.0f * S, mainPos.y);
        wm->ApplyNext("currencypouch", defPos, size, WinManager::Anchor::kTopLeft, topPad);
        ImGui::Begin("##grid_currencypouch", nullptr, kManagedWinFlags);
        UIRoot::NoteOverlayRect();
        wm->TitleBar("currencypouch", "Currency", 0.0f, true);

        auto*        dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        for (int i = 0; i < static_cast<int>(forms.size()); ++i) {
            const std::uint32_t id = forms[static_cast<std::size_t>(i)];
            auto*               obj = RE::TESForm::LookupByID<RE::TESBoundObject>(id);
            const int           count = counts.contains(id) ? counts[id] : 0;
            const ImVec2        p(origin.x + 8.0f * S + (i % kCols) * (cell + gap),
                origin.y + 4.0f * S + (i / kCols) * (cell + gap));
            ImGui::SetCursorScreenPos(p);
            ImGui::PushID(static_cast<int>(id));
            ImGui::InvisibleButton("##slot", ImVec2(cell, cell),
                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            const bool hovered = ImGui::IsItemHovered();
            dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), IM_COL32(16, 14, 20, 200), 3.0f);
            dl->AddRect(p, ImVec2(p.x + cell, p.y + cell), hovered ? Theme::Acc(0.9f) : Theme::Acc(0.3f), 3.0f);
            if (const auto* icon = IconOf(obj); icon && icon->srv) {
                // ★Dimmed at 0: every slot is always shown, in a fixed order.
                const ImU32 tint = count > 0 ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 70);
                dl->AddImage(reinterpret_cast<ImTextureID>(icon->srv), ImVec2(p.x + 4.0f * S, p.y + 4.0f * S),
                    ImVec2(p.x + cell - 4.0f * S, p.y + cell - 4.0f * S), ImVec2(0, 0), ImVec2(1, 1), tint);
            }
            char num[16];
            std::snprintf(num, sizeof(num), "%d", count);
            const ImVec2 ts = ImGui::CalcTextSize(num);
            dl->AddText(ImVec2(p.x + cell - ts.x - 4.0f * S, p.y + cell - ts.y - 2.0f * S),
                count > 0 ? IM_COL32(240, 235, 220, 255) : IM_COL32(140, 140, 140, 160), num);

            // ★RIGHT-CLICK: one orb onto the cursor as the applicator -- the
            // grid's own currency gesture (Shift keeps it, right-click/Esc
            // cancels, a click on an item uses it).
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && count > 0) {
                Grid::ArmCurrency(id);
            }
            // ★LEFT-DRAG OUT: the stack to the ground, released outside every
            // window (the engine's own drop, on the next tick).
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && count > 0) {
                g_drag = id;
            }
            if (hovered && g_drag != id) {
                std::string tip = obj && obj->GetName() ? obj->GetName() : "?";
                GridInvAPI::TooltipLine lines[4]{};
                const auto n = HostApi::AnnotatorLines(id, nullptr, lines, 4);
                for (std::uint32_t k = 0; k < n; ++k) {
                    tip += "\n";
                    tip += lines[k].text;
                }
                tip += "\nYou have " + std::to_string(count);
                Theme::PushTipStyle();
                ImGui::SetTooltip("%s", tip.c_str());
                Theme::PopTipStyle();
            }
            ImGui::PopID();
        }

        // ---- 2.1.1: the hint and "Deposit all" ------------------------------
        {
            int total = 0;
            for (const auto id : forms) {
                if (const auto it = counts.find(id); it != counts.end()) total += it->second;
            }
            const float footY = origin.y + 4.0f * S + (std::max)(1, rows) * (cell + gap) + 2.0f * S;
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0f * S, footY));
            if (total == 0) {
                ImGui::TextDisabled("Orbs you pick up appear here");
            } else {
                ImGui::TextDisabled("%d orb%s", total, total == 1 ? "" : "s");
            }
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0f * S, ImGui::GetCursorScreenPos().y));
            if (ImGui::Button("Deposit all")) {
                // Every orb form out of the board's cells and into this view.
                // The engine's inventory is not touched: nothing to lose or dupe.
                Grid::DepositCurrencies(forms);
                Sfx::SelectOn();
            }
            if (ImGui::IsItemHovered()) {
                Theme::PushTipStyle();
                ImGui::SetTooltip("Take every orb off the inventory board and keep it here");
                Theme::PopTipStyle();
            }
        }
        (void)sk;
        ImGui::End();

        if (g_drag != 0) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            auto*        obj = RE::TESForm::LookupByID<RE::TESBoundObject>(g_drag);
            if (const auto* icon = IconOf(obj); icon && icon->srv) {
                ImGui::GetForegroundDrawList()->AddImage(reinterpret_cast<ImTextureID>(icon->srv),
                    ImVec2(m.x - 18.0f * S, m.y - 18.0f * S), ImVec2(m.x + 18.0f * S, m.y + 18.0f * S));
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                const bool outside = !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) &&
                                     !UIRoot::MouseInOverlay();
                if (outside) {
                    int count = 0;
                    {
                        std::scoped_lock lock{ g_lock };
                        count = g_counts.contains(g_drag) ? g_counts[g_drag] : 0;
                        if (count > 0) g_dropQ.emplace_back(g_drag, count);
                    }
                    if (count > 0) Sfx::SelectOff();
                }
                g_drag = 0;
            }
        }
    }
}
