#include "Names.h"
#include "Avatars.h"
#include "TextInput.h"

#include <ForgeApi.h>
#include <math.h>
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
            const char* const NICK_MENU = "IDM_MP_NICK";

            const char* const NICK_ITEM = "ID_MP_NICK";
            const char* const NICK_FIELD = "ID_MP_NICK_FIELD";
            const char* const NICK_OK = "ID_MP_NICK_OK";

            const char* const NICK_KEY = "mp_name";
            const char* const LIFT_KEY = "mp_name_lift";

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

            const float BADGE_RIGHT = ICON_X - ICON_SIZE * 0.5f - 6.0f;
            const float BADGE_LETTER = 16.0f;
            const float BADGE_PAD = 20.0f;
            const float BADGE_TALL = 28.0f;
            const float BADGE_FONT = 0.8f;
            const float BADGE_TEXT_LIFT = -12.0f;

            const char* const BADGE_FORMAT = R"(
                $images
                {
                    $image
                    {
                        $texture "textures/frame/a40.png"
                        $pos %.0f %.0f
                        $size %.0f %.0f
                        $unreg_only 0
                    }
                    $image
                    {
                        $texture "textures/frame/a40.png"
                        $pos %.0f %.0f
                        $size %.0f %.0f
                        $unreg_only 0
                    }
                }
                $static_text
                {
                    $static
                    {
                        $id "ID_MP_NICK_BADGE"
                        $text "%s"
                        $pos %.0f %.0f
                        $font_scale %.2f %.2f
                        $font_color 1 1 1 1
                        $line_height 0
                        $align "center"
                        $unreg_only 0
                    }
                }
            )";

            const char* const BADGE_TEXT = "ID_MP_NICK_BADGE";

            float g_badgeWide = 0.0f;

            const int ALIGN_CENTRE = 0x24;

            const float LETTER = 11.0f;

            const int MAX_IDS = 9;

            char g_mine[MAX_NAME + 1] = "player";
            char g_names[MAX_IDS][MAX_NAME + 1] = {};

            AmodToken g_frame = AMOD_INVALID_TOKEN;

            bool g_building = false;

            bool Same(const char* one, const char* other)
            {
                return one != nullptr && other != nullptr && strcmp(one, other) == 0;
            }

            void UpdateBadge();

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
                UpdateBadge();
            }

            void __cdecl OnSubmit(const char* menuId, const char* fieldId)
            {
                if (Same(menuId, NICK_MENU) && Same(fieldId, NICK_FIELD))
                {
                    Accept();
                    Box::Back();
                }
            }

            void Strip(const char* text, float x, float y, float scale)
            {
                const float wide = static_cast<float>(strlen(text)) * LETTER * scale + 12.0f;
                const float tall = 26.0f * scale;

                Render::Rect(x - wide * 0.5f, y - tall * 0.5f, wide, tall, 0x99000000);
                Render::Text(Render::GetFont("default"), x, y, text, scale, 0xFFFFFFFF, ALIGN_CENTRE);
            }

            // A quote would end the script's string early.
            void Shown(char* shown, size_t size)
            {
                strncpy_s(shown, size, g_mine, _TRUNCATE);

                for (char* at = shown; *at != 0; ++at)
                {
                    *at = *at == '"' ? '\'' : *at;
                }
            }

            // Whole pixels, so the size read back from the menu compares equal to it.
            float BadgeWide(const char* shown)
            {
                return static_cast<float>(strlen(shown)) * BADGE_LETTER + BADGE_PAD;
            }

            void UpdateBadge()
            {
                if (g_badgeWide <= 0.0f)
                {
                    return;
                }

                char shown[MAX_NAME + 1] = {};
                Shown(shown, sizeof(shown));

                const float wide = BadgeWide(shown);
                const int count = Menu::Count(MAIN_MENU);

                for (int at = 0; at < count; ++at)
                {
                    const Menu::Element piece = Menu::At(MAIN_MENU, at);

                    if (!piece.IsValid())
                    {
                        continue;
                    }

                    const char* const kind = piece["kind"].Text("");

                    if (strcmp(kind, "static") == 0 && Same(piece["id"].Text(""), BADGE_TEXT))
                    {
                        piece["text"].Set(shown);
                        piece["pos"].Set(piece["pos"].Number(0) + (g_badgeWide - wide) * 0.5f, piece["pos"].Number(1));
                        continue;
                    }

                    if (strcmp(kind, "image") != 0 || fabsf(piece["size"].Number(0) - g_badgeWide) > 0.5f 
                        || fabsf(piece["size"].Number(1) - BADGE_TALL) > 0.5f)
                    {
                        continue;
                    }

                    piece["pos"].Set(piece["pos"].Number(0) + g_badgeWide - wide, piece["pos"].Number(1));
                    piece["size"].Set(wide, BADGE_TALL);
                }

                g_badgeWide = wide;
            }

            void AddBadge()
            {
                char shown[MAX_NAME + 1] = {};
                Shown(shown, sizeof(shown));

                const float wide = BadgeWide(shown);
                const float left = BADGE_RIGHT - wide;
                const float top = ICON_Y + BADGE_TALL * 0.5f;

                char fragment[1536];

                sprintf_s(fragment, BADGE_FORMAT,
                    left, top, wide, BADGE_TALL,
                    left, top, wide, BADGE_TALL,
                    shown, left + wide * 0.5f, ICON_Y + BADGE_TEXT_LIFT, BADGE_FONT, BADGE_FONT);

                Menu::Add(MAIN_MENU, fragment);
                g_badgeWide = wide;
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
                        Strip(name, x, y, 0.6f);
                    }
                }
            }

            void __cdecl OnFrame(const AmodFrame*, void*)
            {
                const char* const current = Box::Current();

                if (World::IsLevelLoaded() && (current == nullptr || current[0] == 0))
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
                AddBadge();
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
