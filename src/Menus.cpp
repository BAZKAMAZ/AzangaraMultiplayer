#include "Menus.h"

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

            // Kept once, and only the first time: what the script wrote is what goes back.
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

            void __cdecl OnMenu(AmodMenuEvent event, void* data, void*)
            {
                if (event == AMOD_MENU_LOADED)
                {
                    const AmodMenuLoaded* const loaded = static_cast<const AmodMenuLoaded*>(data);

                    if (loaded != nullptr)
                    {
                        Apply(loaded->menuId);
                    }

                    return;
                }

                AmodMenuClicked* const clicked = static_cast<AmodMenuClicked*>(data);

                if (g_block && clicked != nullptr && Ours(clicked->itemId))
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
            Block(false);

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
    }
}
