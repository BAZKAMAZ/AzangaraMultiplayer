#include "Lobby.h"
#include "TextInput.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ForgeApi.h>
#include <stdio.h>
#include <string.h>

using namespace Amod;

namespace Mp
{
    namespace Lobby
    {
        namespace
        {
            const char* const MENU = "IDM_MP_LOBBY";
            const char* const MAIN_MENU = "IDM_MAIN_MENU";
            const char* const TABLE = "ID_MP_ROOMS";
            const char* const PALACE = "ID_MP_PALACE";
            const char* const PREV = "ID_MP_PREV";
            const char* const NEXT = "ID_MP_NEXT";
            const char* const CREATE = "ID_MP_CREATE";
            const char* const JOIN = "ID_MP_JOIN";
            const char* const ENTER_IP = "ID_MP_ENTER_IP";

            // IP address input menu.
            const char* const ADDRESS_MENU = "IDM_MP_ADDRESS";
            const char* const ADDRESS = "ID_MP_ADDRESS";
            const char* const ADDRESS_OK = "ID_MP_ADDRESS_OK";

            Room g_rooms[MAX_ROOMS] = {};
            int g_roomCount = 0;
            int g_palace = 0;
            int g_picked = -1;
            bool g_lined = false;

            int g_clickedRow = -1;
            DWORD g_clickedAt = 0;

            // Room list row positions.
            const float LINE_FIRST = 350.0f;
            const float LINE_STEP = 29.0f;
            const float LINE_LEFT = 40.0f;
            const float LINE_RIGHT = 600.0f;

            const float PICKED[4] = { 1.0f, 0.4f, 1.0f, 1.0f };

            Opener g_open = nullptr;
            Joiner g_join = nullptr;
            Closer g_close = nullptr;

            bool Same(const char* one, const char* other)
            {
                return one != nullptr && other != nullptr && strcmp(one, other) == 0;
            }

            int Palaces()
            {
                return Sav::IsAvailable() ? Sav::Count() : 0;
            }

            const char* const PICTURES[] = { "img_enabled", "img_disabled", "img_blocked" };
            const int PICTURE_COUNT = sizeof(PICTURES) / sizeof(PICTURES[0]);

            struct Picture
            {
                char name[128];
                bool mipmapped;
                bool hasAlpha;
            };

            struct Castle
            {
                char id[64];
                char level[129];
                Picture pictures[PICTURE_COUNT];
            };

            // Default lobby button textures.
            int g_ownPictures[PICTURE_COUNT] = {};
            bool g_ownKept = false;

            AmodTextureId g_loaded[PICTURE_COUNT] = {};

            void Remember(Picture& picture, int handle)
            {
                picture = Picture{};

                AmodTexture texture = {};
                const AmodTextureId id = handle != 0 ? Textures::FromGame(static_cast<uint32_t>(handle)) : AMOD_INVALID_TEXTURE;

                if (id == AMOD_INVALID_TEXTURE || !Textures::Get(id, texture) || texture.name == nullptr)
                {
                    return;
                }

                strncpy_s(picture.name, sizeof(picture.name), texture.name, _TRUNCATE);
                picture.mipmapped = texture.mipmapped != 0;
                picture.hasAlpha = texture.hasAlpha != 0;
            }

            const int MAX_CASTLES = 32;

            Castle g_castles[MAX_CASTLES] = {};
            int g_castleCount = 0;

            void Learn(const char* menuId)
            {
                if (Same(menuId, MENU))
                {
                    return;
                }

                const int count = Menu::Count(menuId);

                for (int at = 0; at < count; ++at)
                {
                    const Menu::Element button = Menu::At(menuId, at);
                    const Menu::Property start = button["start_level"];

                    if (!button.IsValid() || !start.Exists())
                    {
                        continue;
                    }

                    const char* const id = button.Id();
                    const char* const level = start.Text("");

                    if (id == nullptr || id[0] == 0 || level[0] == 0)
                    {
                        continue;
                    }

                    int slot = 0;
                    while (slot < g_castleCount && !Same(g_castles[slot].id, id))
                    {
                        ++slot;
                    }

                    if (slot == g_castleCount)
                    {
                        if (g_castleCount >= MAX_CASTLES)
                        {
                            continue;
                        }

                        ++g_castleCount;
                    }

                    strcpy_s(g_castles[slot].id, sizeof(g_castles[slot].id), id);
                    strcpy_s(g_castles[slot].level, sizeof(g_castles[slot].level), level);

                    for (int picture = 0; picture < PICTURE_COUNT; ++picture)
                    {
                        Remember(g_castles[slot].pictures[picture], button[PICTURES[picture]].Integer(0));
                    }
                }
            }

            const Castle* CastleOf(const char* id)
            {
                for (int at = 0; at < g_castleCount; ++at)
                {
                    if (Same(g_castles[at].id, id))
                    {
                        return &g_castles[at];
                    }
                }

                return nullptr;
            }

            const char* LevelOf(const char* id)
            {
                const Castle* const castle = CastleOf(id);
                return castle != nullptr ? castle->level : "";
            }

            const char* LevelAt(const char* address)
            {
                for (int at = 0; at < g_roomCount; ++at)
                {
                    if (Same(g_rooms[at].address, address))
                    {
                        return g_rooms[at].level;
                    }
                }

                return "";
            }

            void JoinRow(int row)
            {
                if (row < 0 || row >= g_roomCount)
                {
                    Console::Print("mp: pick a room or enter an address first\n");
                    return;
                }

                if (g_join != nullptr)
                {
                    g_join(g_rooms[row].address, g_rooms[row].level);
                }
            }

            bool JoinTyped()
            {
                const char* const address = TextInput::Text(ADDRESS);

                if (address[0] == 0)
                {
                    Console::Print("mp: type an address first\n");
                    return false;
                }

                if (g_join != nullptr)
                {
                    g_join(address, LevelAt(address));
                }

                return true;
            }

            void __cdecl OnSubmit(const char* menuId, const char* fieldId)
            {
                if (Same(menuId, ADDRESS_MENU) && Same(fieldId, ADDRESS) && JoinTyped())
                {
                    Box::Back();
                }
            }

            void ShortName(const char* path, char* out, int size)
            {
                const char* name = path != nullptr ? path : "";

                for (const char* at = name; *at != 0; ++at)
                {
                    if (*at == '/' || *at == '\\')
                    {
                        name = at + 1;
                    }
                }

                int kept = 0;
                while (name[kept] != 0 && name[kept] != '.' && kept + 1 < size)
                {
                    out[kept] = name[kept];
                    ++kept;
                }

                out[kept] = 0;
            }

            void ShowPalace()
            {
                const Menu::Element icon = Menu::Find(MENU, PALACE);

                if (!icon.IsValid())
                {
                    return;
                }

                if (!g_ownKept)
                {
                    g_ownKept = true;

                    for (int picture = 0; picture < PICTURE_COUNT; ++picture)
                    {
                        g_ownPictures[picture] = icon[PICTURES[picture]].Integer(0);
                    }
                }

                char name[32] = "none";
                const Castle* castle = nullptr;

                if (Palaces() > 0)
                {
                    g_palace = (g_palace % Palaces() + Palaces()) % Palaces();
                    ShortName(Sav::NameAt(g_palace), name, sizeof(name));
                    castle = CastleOf(Sav::NameAt(g_palace));

                    icon["start_level"].Set(Palace());
                }

                for (int picture = 0; picture < PICTURE_COUNT; ++picture)
                {
                    const Picture* const wanted = castle != nullptr ? &castle->pictures[picture] : nullptr;
                    AmodTextureId loaded = AMOD_INVALID_TEXTURE;

                    if (wanted != nullptr && wanted->name[0] != 0 && Files::Read(wanted->name) > 0)
                    {
                        loaded = Textures::Load(wanted->name, wanted->mipmapped, wanted->hasAlpha);
                    }

                    const uint32_t handle = loaded != AMOD_INVALID_TEXTURE ? Textures::ToGame(loaded) : 0;

                    icon[PICTURES[picture]].Set(handle != 0 ? static_cast<int>(handle) : g_ownPictures[picture]);

                    // Release the previous texture.
                    if (g_loaded[picture] != AMOD_INVALID_TEXTURE)
                    {
                        Textures::Release(g_loaded[picture]);
                    }

                    g_loaded[picture] = handle != 0 ? loaded : AMOD_INVALID_TEXTURE;

                    if (handle == 0 && loaded != AMOD_INVALID_TEXTURE)
                    {
                        Textures::Release(loaded);
                    }
                }

                icon["caption.text"].Set(name);
                icon["help"].Set(name);
            }

            void Fill()
            {
                const Menu::Element table = Menu::Find(MENU, TABLE);
                const int lines = table.IsValid() ? table.LineCount() : 0;

                for (int line = 0; line < lines; ++line)
                {
                    char text[32];

                    char id[24];

                    sprintf_s(id, "ID_MP_ROW_%d", line);

                    const Menu::Element row = Menu::Find(MENU, id);

                    if (row.IsValid())
                    {
                        row["visible"].Set(line < g_roomCount);
                    }

                    if (line >= g_roomCount)
                    {
                        for (int column = 0; column < 5; ++column)
                        {
                            table.SetCell(column, line, nullptr);
                        }

                        table.SetLineDots(line, false);
                        table.SetLineStyle(line, nullptr);
                        continue;
                    }

                    const Room& room = g_rooms[line];

                    sprintf_s(text, "%d", line + 1);
                    table.SetCell(0, line, text);

                    if (room.ping >= 0)
                    {
                        sprintf_s(text, "%d", room.ping);
                        table.SetCell(1, line, text);
                    }
                    else
                    {
                        table.SetCell(1, line, "?");
                    }

                    table.SetCell(2, line, room.address);
                    table.SetCell(3, line, room.palace);

                    if (room.players >= 0)
                    {
                        sprintf_s(text, "%d", room.players);
                        table.SetCell(4, line, text);
                    }
                    else
                    {
                        table.SetCell(4, line, "-");
                    }

                    table.SetLineDots(line, true);
                    table.SetLineStyle(line, line == g_picked ? PICKED : nullptr);
                }
            }
        }

        void BuildLines()
        {
            if (g_lined || !Menu::IsAvailable())
            {
                return;
            }

            g_lined = true;

            for (int line = 0; line < MAX_ROOMS; ++line)
            {
                char fragment[768];

                sprintf_s(fragment, R"(
                    $img_buttons
                    {
                        $button
                        {
                            $id "ID_MP_ROW_%d"
                            $pos %.0f %.0f
                            $rect_size %.0f %.0f
                            $h_align "center"
                            $v_align "center"
                            $img_enabled "textures/frame/a40.png"
                            $img_disabled "textures/frame/a40.png"
                            $help ""
                            $warning ""
                            $unreg_enabled 1
                            $keyctrl_button 0
                        }
                    }
                )",
                    line, (LINE_LEFT + LINE_RIGHT) * 0.5f, LINE_FIRST - LINE_STEP * line,
                    LINE_RIGHT - LINE_LEFT, LINE_STEP);

                const Menu::Element row = Menu::Add(MENU, fragment);

                if (row.IsValid())
                {
                    row["anim.color_passive"].Set(1.0f, 1.0f, 1.0f, 0.0f);
                    row["anim.color_active"].Set(1.0f, 1.0f, 1.0f, 0.45f);
                    row["anim.color_disabled"].Set(1.0f, 1.0f, 1.0f, 0.0f);
                    row["anim.color_blocked"].Set(1.0f, 1.0f, 1.0f, 0.0f);
                }
            }
        }

        int LineOf(const char* itemId)
        {
            const char* const head = "ID_MP_ROW_";
            const size_t length = strlen(head);

            return (itemId != nullptr && strncmp(itemId, head, length) == 0)
                ? atoi(itemId + length) : -1;
        }

        void Answer(Opener open, Joiner join, Closer close)
        {
            g_open = open;
            g_join = join;
            g_close = close;

            TextInput::Answer(&OnSubmit);
        }

        void Build(const char* menuId)
        {
            Learn(menuId);

            if (Same(menuId, MAIN_MENU) && g_close != nullptr)
            {
                g_close();
            }

            if (!Same(menuId, MENU))
            {
                return;
            }

            BuildLines();
            ShowPalace();
            Fill();
        }

        bool Click(const char* itemId)
        {
            if (Same(itemId, PREV) || Same(itemId, NEXT))
            {
                if (Palaces() < 1)
                {
                    Console::Print("mp: the game has opened no palaces to pick from\n");
                    return true;
                }

                g_palace += Same(itemId, NEXT) ? 1 : -1;
                ShowPalace();

                Console::Printf("mp: palace %s -> %s\n", Sav::NameAt(g_palace),
                    Palace()[0] != 0 ? Palace() : "no castle button names it yet");

                return true;
            }

            const int row = LineOf(itemId);

            if (row >= 0)
            {
                const DWORD now = GetTickCount();
                const bool twice = row == g_clickedRow && now - g_clickedAt <= GetDoubleClickTime();

                g_picked = row < g_roomCount ? row : -1;
                g_clickedRow = twice ? -1 : row;
                g_clickedAt = now;
                Fill();

                if (twice)
                {
                    JoinRow(g_picked);
                }

                return true;
            }

            if (Same(itemId, ENTER_IP))
            {
                TextInput::SetText(ADDRESS, g_picked >= 0 && g_picked < g_roomCount
                    ? g_rooms[g_picked].address : Config::Text("mp_peer", ""));

                return false;
            }

            if (Same(itemId, ADDRESS_OK))
            {
                return !JoinTyped();
            }

            if (Same(itemId, CREATE))
            {
                if (g_open != nullptr)
                {
                    g_open(Palace());
                }

                return true;
            }

            if (Same(itemId, JOIN))
            {
                JoinRow(g_picked);
                return true;
            }

            return false;
        }

        void Rooms(const Room* rooms, int count)
        {
            g_roomCount = 0;

            for (int at = 0; rooms != nullptr && at < count && at < MAX_ROOMS; ++at)
            {
                g_rooms[g_roomCount++] = rooms[at];
            }

            Fill();
        }

        const char* Palace()
        {
            return Palaces() > 0 ? LevelOf(Sav::NameAt(g_palace)) : "";
        }
    }
}
