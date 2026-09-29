#include "Menus.h"
#include "Lobby.h"
#include "Names.h"
#include "TextInput.h"

#include <ForgeApi.h>
#include <stdio.h>
#include <string.h>

using namespace Amod;

namespace Mp
{
    namespace Menus
    {
        namespace
        {
            const char* const SAVE_ITEM = "ID_SAVE_GAME";
            const char* const LOAD_ITEM = "ID_LOAD_GAME";

            const char* const MENUS[] = { "IDM_ESC_MENU", "IDM_MAIN_MENU" };
            const int MENU_COUNT = sizeof(MENUS) / sizeof(MENUS[0]);

            const char* const MAIN_MENU = "IDM_MAIN_MENU";
            const char* const LOBBY_MENU = "IDM_MP_LOBBY";
            const char* const LOBBY_ITEM = "ID_MP_MENU";

            struct Script
            {
                const char* menuId;
                const char* file;
            };

            const Script SCRIPTS[] =
            {
                { LOBBY_MENU, "scripts/menu_lobby.txt" },
                { "IDM_MP_NICK", "scripts/menu_nick.txt" },
                { "IDM_MP_ADDRESS", "scripts/menu_address.txt" },
                { "IDM_MP_NOTICE", "scripts/menu_notice.txt" },
            };

            const char* const NOTICE_MENU = "IDM_MP_NOTICE";
            const char* const NOTICE_TEXT = "ID_MP_NOTICE_TEXT";

            char g_notice[256] = {};

            bool g_noticeArmed = false;
            int g_noticeWait = 0;
            int g_noticeFrames = 0;
            int g_noticeTries = 0;

            const int NOTICE_LEAVE = 30;
            const int NOTICE_SETTLE = 3;
            const int NOTICE_RETRY = 30;
            const int NOTICE_TRIES = 10;

            const int SCRIPT_COUNT = sizeof(SCRIPTS) / sizeof(SCRIPTS[0]);

            const char* const LOBBY_BACKGROUND = R"(
                $images {
                    $image {
                        $texture "textures/lobby.jpg"
                        $pos 0 480
                        $size 640 480
                        $unreg_only 0
                    }
                }
            )";

            const char* const LOBBY_ITEMS = R"(
                $menu_items {
                    $item {
                        $id "ID_MP_CREATE"
                        $text "CREATE ROOM"
                        $pos 320 330
                        $rect_size 300 45
                        $plank_width 6
                        $h_align "center"
                        $v_align "center"
                        $help "Open a room and wait for the others"
                    }
                    $item {
                        $id "ID_MP_JOIN"
                        $text "JOIN ROOM"
                        $pos 320 280
                        $rect_size 300 45
                        $plank_width 6
                        $h_align "center"
                        $v_align "center"
                        $help "Walk into somebody else's room"
                    }
                    $item {
                        $id "ID_BACK"
                        $text "BACK"
                        $pos 320 90
                        $rect_size 200 45
                        $plank_width 6
                        $h_align "center"
                        $v_align "center"
                        $help "Back to the main menu"
                    }
                }
            )";

            const char* const LOBBY_BUTTON =
                "$menu_items\n"
                "{\n"
                "    $item\n"
                "    {\n"
                "        $v_align \"center\"\n"
                "        $h_align \"center\"\n"
                "\n"
                "        $id \"ID_MP_MENU\"\n"
                "        $text \"MULTIPLAYER\"\n"
                "        $pos 320 340\n"
                "        $rect_size 300 45\n"
                "        $plank_width 9\n"
                "        $sub_menu \"IDM_MP_LOBBY\"\n"
                "\n"
                "        $cursor3d\n"
                "        {\n"
                "            $orient 0.36 0 1.57\n"
                "            $pos -3.6 2.15 -9\n"
                "        }\n"
                "\n"
                "        $help \"Play online together\"\n"
                "        $warning \"\"\n"
                "        $unreg_enabled 1\n"
                "    }\n"
                "}\n";

            bool g_ready = false;
            bool g_registered = false;

            const int MAX_KEPT = 8;

            const uintptr_t WARNS_AT = 0x5D1;

            struct Kept
            {
                char menu[64];
                const char* item;
                char warning[256];
                uint8_t warns;
            };

            Kept g_kept[MAX_KEPT] = {};
            int g_keptCount = 0;

            bool g_block = false;
            AmodToken g_onLoaded = AMOD_INVALID_TOKEN;
            AmodToken g_onClicked = AMOD_INVALID_TOKEN;

            bool Same(const char* one, const char* other)
            {
                return one != nullptr && other != nullptr && strcmp(one, other) == 0;
            }

            bool Ours(const char* item)
            {
                return Same(item, SAVE_ITEM) || Same(item, LOAD_ITEM);
            }

            Kept* Find(const char* menu, const char* item)
            {
                for (int at = 0; at < g_keptCount; ++at)
                {
                    if (Same(g_kept[at].menu, menu) && g_kept[at].item == item)
                    {
                        return &g_kept[at];
                    }
                }

                return nullptr;
            }

            void Keep(const char* menu, const char* item, const char* warning, uint8_t warns)
            {
                if (Find(menu, item) != nullptr || g_keptCount >= MAX_KEPT || menu == nullptr)
                {
                    return;
                }

                Kept& kept = g_kept[g_keptCount++];

                strncpy_s(kept.menu, menu, _TRUNCATE);
                strncpy_s(kept.warning, warning != nullptr ? warning : "", _TRUNCATE);
                kept.item = item;
                kept.warns = warns;
            }

            void Warns(const Menu::Element& item, uint8_t on)
            {
                uint8_t* const raw = static_cast<uint8_t*>(item.Raw());

                if (raw != nullptr)
                {
                    memcpy(raw + WARNS_AT, &on, sizeof(on));
                }
            }

            uint8_t Warning(const Menu::Element& item)
            {
                const uint8_t* const raw = static_cast<const uint8_t*>(item.Raw());
                uint8_t on = 0;

                if (raw != nullptr)
                {
                    memcpy(&on, raw + WARNS_AT, sizeof(on));
                }

                return on;
            }

            void Apply(const char* menu)
            {
                if (!Menu::IsAvailable() || menu == nullptr || menu[0] == 0)
                {
                    return;
                }

                const char* const items[2] = { SAVE_ITEM, LOAD_ITEM };

                for (int at = 0; at < 2; ++at)
                {
                    const Menu::Element item = Menu::Find(menu, items[at]);

                    if (!item.IsValid())
                    {
                        continue;
                    }

                    if (g_block)
                    {
                        char text[256];

                        Keep(menu, items[at], item["warning"].Text(), Warning(item));

                        sprintf_s(text, "You cannot %s the game in multiplayer",
                            items[at] == SAVE_ITEM ? "save" : "load");

                        item["warning"].Set(text);
                        Warns(item, 1);
                    }
                    else
                    {
                        const Kept* const kept = Find(menu, items[at]);

                        if (kept != nullptr)
                        {
                            item["warning"].Set(kept->warning);
                            Warns(item, kept->warns);
                        }
                    }
                }
            }

            void RegisterAll()
            {
                if (g_registered || !Box::IsAvailable())
                {
                    return;
                }

                g_registered = true;

                for (int at = 0; at < SCRIPT_COUNT; ++at)
                {
                    const Script& script = SCRIPTS[at];
                    bool made = Box::Exists(script.menuId) || Box::RegisterFromScript(script.menuId, script.file);

                    if (!made && Same(script.menuId, LOBBY_MENU) && Box::Register(LOBBY_MENU))
                    {
                        made = true;
                        Menu::Add(LOBBY_MENU, LOBBY_BACKGROUND);
                        Menu::Add(LOBBY_MENU, LOBBY_ITEMS);
                        Mp::Lobby::Build(LOBBY_MENU);
                    }

                    if (!made)
                    {
                        Log(AMOD_LOG_WARNING, "%s would not register from %s", script.menuId, script.file);
                        continue;
                    }

                    Mp::TextInput::Load(script.menuId, script.file);

                    g_ready = g_ready || Same(script.menuId, LOBBY_MENU);
                }
            }

            void __cdecl OnMenu(AmodMenuEvent event, void* data, void*)
            {
                if (event == AMOD_MENU_LOADED)
                {
                    const AmodMenuLoaded* const loaded = static_cast<const AmodMenuLoaded*>(data);

                    if (loaded == nullptr)
                    {
                        return;
                    }

                    Apply(loaded->menuId);
                    Mp::Lobby::Build(loaded->menuId);
                    Mp::Names::Build(loaded->menuId);

                    if (!Same(loaded->menuId, MAIN_MENU))
                    {
                        return;
                    }

                    RegisterAll();

                    if (!Menu::Find(MAIN_MENU, LOBBY_ITEM).IsValid())
                    {
                        Menu::Add(MAIN_MENU, LOBBY_BUTTON);
                    }

                    return;
                }

                AmodMenuClicked* const clicked = static_cast<AmodMenuClicked*>(data);

                if (clicked == nullptr)
                {
                    return;
                }

                if (Mp::Lobby::Click(clicked->itemId) || Mp::Names::Click(clicked->itemId))
                {
                    clicked->handled = 1;
                    return;
                }

                if (g_block && Ours(clicked->itemId))
                {
                    clicked->handled = 1;
                }
            }
        }

        bool Open()
        {
            if (!Menu::IsAvailable())
            {
                return false;
            }

            g_onLoaded = Menu::Subscribe(AMOD_MENU_LOADED, &OnMenu);
            g_onClicked = Menu::Subscribe(AMOD_MENU_CLICKED, &OnMenu);

            return g_onLoaded != AMOD_INVALID_TOKEN && g_onClicked != AMOD_INVALID_TOKEN;
        }

        void Close()
        {
            g_block = false;
            g_keptCount = 0;

            if (g_onLoaded != AMOD_INVALID_TOKEN)
            {
                Menu::Unsubscribe(g_onLoaded);
            }

            if (g_onClicked != AMOD_INVALID_TOKEN)
            {
                Menu::Unsubscribe(g_onClicked);
            }

            g_onLoaded = AMOD_INVALID_TOKEN;
            g_onClicked = AMOD_INVALID_TOKEN;
        }

        void Block(bool block)
        {
            if (block == g_block)
            {
                return;
            }

            g_block = block;

            for (int at = 0; at < MENU_COUNT; ++at)
            {
                Apply(MENUS[at]);
            }
        }

        bool Blocked()
        {
            return g_block;
        }

        bool Show()
        {
            return g_ready && Box::Open(LOBBY_MENU);
        }

        bool Ready()
        {
            return g_ready;
        }

        bool SetStatic(const char* menuId, const char* staticId, const char* text)
        {
            const int count = Menu::Count(menuId);
            Menu::Element last(nullptr);

            for (int at = 0; at < count; ++at)
            {
                const Menu::Element piece = Menu::At(menuId, at);

                if (!piece.IsValid() || strcmp(piece["kind"].Text(""), "static") != 0)
                {
                    continue;
                }

                if (strcmp(piece["id"].Text(""), staticId) == 0)
                {
                    return piece["text"].Set(text);
                }

                last = piece;
            }

            return last.IsValid() && last["text"].Set(text);
        }

        void Notice(const char* text, bool afterLevel)
        {
            strncpy_s(g_notice, sizeof(g_notice), text != nullptr ? text : "", _TRUNCATE);

            g_noticeArmed = true;
            g_noticeWait = afterLevel ? NOTICE_LEAVE : 0;
            g_noticeFrames = 0;
            g_noticeTries = 0;
        }

        void Tick()
        {
            if (g_notice[0] == 0 || !g_registered || !g_noticeArmed)
            {
                return;
            }

            if (g_noticeWait > 0)
            {
                --g_noticeWait;
                return;
            }

            const char* const current = Box::Current();
            const char* const box = Box::BoxMenu();

            if (Same(box, NOTICE_MENU))
            {
                g_notice[0] = 0;
                g_noticeArmed = false;
                return;
            }

            if (!(Same(current, MAIN_MENU) || Same(current, LOBBY_MENU)) || (box != nullptr && box[0] != 0))
            {
                g_noticeFrames = 0;
                return;
            }

            const int frame = g_noticeFrames++;

            if (frame < NOTICE_SETTLE || (frame - NOTICE_SETTLE) % NOTICE_RETRY != 0)
            {
                return;
            }

            if (++g_noticeTries > NOTICE_TRIES)
            {
                Log(AMOD_LOG_WARNING, "%s would not open; the notice was: %s", NOTICE_MENU, g_notice);
                g_notice[0] = 0;
                g_noticeArmed = false;
                return;
            }

            if (!SetStatic(NOTICE_MENU, NOTICE_TEXT, g_notice))
            {
                Log(AMOD_LOG_WARNING, "%s has no static text %s", NOTICE_MENU, NOTICE_TEXT);
            }

            Box::ShowBox(NOTICE_MENU);
        }
    }
}
