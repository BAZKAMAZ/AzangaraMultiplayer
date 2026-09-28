#include "Names.h"

#include "Avatars.h"
#include "TextInput.h"

#include <ForgeApi.h>

#include <stdio.h>
#include <string.h>

using namespace Amod;

namespace Mp
{
    namespace Names
    {
        namespace
        {
            const char* const MAIN_MENU = "IDM_MAIN_MENU";
            // Registered with the lobby in Menus.cpp, from resmod/scripts/menu_nick.txt.
            const char* const NICK_MENU = "IDM_MP_NICK";

            const char* const NICK_ITEM = "ID_MP_NICK";
            const char* const NICK_FIELD = "ID_MP_NICK_FIELD";
            const char* const NICK_OK = "ID_MP_NICK_OK";

            const char* const NICK_KEY = "mp_name";
            const char* const LIFT_KEY = "mp_name_lift";

            // Bottom right of the main menu, in script space (y up); the nickname sits to the left of it.
            const float ICON_X = 612.0f;
            const float ICON_Y = 30.0f;
            const float ICON_SIZE = 36.0f;

            const char* const NICK_BUTTON = R"(
                $img_buttons
                {
                    $button
                    {
                        $id "ID_MP_NICK"
                        $pos 612 30
                        $rect_size 36 36
                        $v_align "center"
                        $h_align "center"
                        $img_enabled "textures/nick_icon.png"
                        $img_disabled "textures/nick_icon.png"
                        $box_menu "IDM_MP_NICK"
                        $help "Change your nickname"
                        $warning ""
                        $item_anim
                        {
                            $color_passive 0.8 0.8 0.8 1
                            $color_active 1 1 1 1
                            $color_disabled 0.7 0.7 0.7 0.5
                            $color_blocked 0.5 0 0 1
                            $scale_passive 0.9 0.9
                            $scale_active 1 1
                        }
                        $unreg_enabled 1
                        $keyctrl_button 0
                    }
                }
            )";

            const int ALIGN_RIGHT_MIDDLE = 0x22;
            const int ALIGN_CENTRE = 0x24;

            const float LETTER = 11.0f;

            const int MAX_IDS = 9;

            char g_mine[MAX_NAME + 1] = "player";
            char g_names[MAX_IDS][MAX_NAME + 1] = {};

            AmodToken g_frame = AMOD_INVALID_TOKEN;

            // Adding the button builds the main menu again, which comes back here; once is enough.
            bool g_building = false;

            bool Same(const char* one, const char* other)
            {
                return one != nullptr && other != nullptr && strcmp(one, other) == 0;
            }

            // Spaces off both ends; nothing left keeps the old one.
            void Accept()
            {
                const char* typed = TextInput::Text(NICK_FIELD);

                while (*typed == ' ')
                {
                    ++typed;
                }

                char name[MAX_NAME + 1] = {};
                strncpy_s(name, sizeof(name), typed, _TRUNCATE);

                for (int end = static_cast<int>(strlen(name)) - 1; end >= 0 && name[end] == ' '; --end)
                {
                    name[end] = 0;
                }

                if (name[0] == 0)
                {
                    return;
                }

                strcpy_s(g_mine, sizeof(g_mine), name);
                Config::Set(NICK_KEY, g_mine, true);
                Console::Printf("mp: you are %s now\n", g_mine);
            }

            void __cdecl OnSubmit(const char* menuId, const char* fieldId)
            {
                if (Same(menuId, NICK_MENU) && Same(fieldId, NICK_FIELD))
                {
                    Accept();
                    Box::Back();
                }
            }

            void Strip(const char* text, float x, float y, float scale, int align)
            {
                const float wide = static_cast<float>(strlen(text)) * LETTER * scale + 12.0f;
                const float tall = 26.0f * scale;
                const float left = align == ALIGN_CENTRE ? x - wide * 0.5f : x - wide + 6.0f;

                Render::Rect(left, y - tall * 0.5f, wide, tall, 0x99000000);
                Render::Text(Render::GetFont("bigitem"), x, y, text, scale, 0xFFFFFFFF, align);
            }

            void DrawBadge()
            {
                const float x = ICON_X - ICON_SIZE * 0.5f - 8.0f;
                const float y = AMOD_MENU_TALL - ICON_Y;

                Strip(g_mine, x, y, 0.8f, ALIGN_RIGHT_MIDDLE);
            }

            // The others over their heads, the host's word for who is who.
            void DrawLabels()
            {
                const float lift = Config::Number(LIFT_KEY, 0.35f);

                for (int at = 0; at < Avatars::ShownCount(); ++at)
                {
                    const Body* const body = Avatars::ShownAt(at);

                    if (body == nullptr || body->room != Avatars::ShownRoom())
                    {
                        continue;
                    }

                    const char* const name = Of(body->id);
                    const float head[3] = { body->position[0], body->position[1] + lift, body->position[2] };
                    float x = 0.0f;
                    float y = 0.0f;

                    if (name[0] != 0 && Avatars::Project(head, x, y))
                    {
                        Strip(name, x, y, 0.6f, ALIGN_CENTRE);
                    }
                }
            }

            void __cdecl OnFrame(const AmodFrame*, void*)
            {
                TextInput::Draw();

                const char* const current = Box::Current();
                const char* const box = Box::BoxMenu();

                if (Same(current, MAIN_MENU) && (box == nullptr || box[0] == 0))
                {
                    DrawBadge();
                }
                else if (World::IsLevelLoaded() && (current == nullptr || current[0] == 0))
                {
                    DrawLabels();
                }
            }
        }

        bool Open()
        {
            Config::Add(NICK_KEY, "player");
            Config::Add(LIFT_KEY, "0.35");

            strncpy_s(g_mine, sizeof(g_mine), Config::Text(NICK_KEY, "player"), _TRUNCATE);

            TextInput::Answer(&OnSubmit);
            g_frame = Render::OnFrame(&OnFrame);

            return g_frame != AMOD_INVALID_TOKEN;
        }

        void Close()
        {
            if (g_frame != AMOD_INVALID_TOKEN)
            {
                Render::CancelFrame(g_frame);
                g_frame = AMOD_INVALID_TOKEN;
            }

            TextInput::Close();
        }

        const char* Mine()
        {
            return g_mine;
        }

        void Set(uint8_t id, const char* name)
        {
            if (id < MAX_IDS)
            {
                strncpy_s(g_names[id], sizeof(g_names[id]), name != nullptr ? name : "", _TRUNCATE);
            }
        }

        const char* Of(uint8_t id)
        {
            return id < MAX_IDS ? g_names[id] : "";
        }

        void Forget()
        {
            memset(g_names, 0, sizeof(g_names));
        }

        void Build(const char* menuId)
        {
            if (!Same(menuId, MAIN_MENU) || g_building)
            {
                return;
            }

            g_building = true;

            if (!Menu::Find(MAIN_MENU, NICK_ITEM).IsValid())
            {
                Menu::Add(MAIN_MENU, NICK_BUTTON);
            }

            g_building = false;
        }

        bool Click(const char* itemId)
        {
            if (Same(itemId, NICK_ITEM))
            {
                TextInput::SetText(NICK_FIELD, g_mine);
            }
            else if (Same(itemId, NICK_OK))
            {
                Accept();
            }

            return false;
        }
    }
}
