#include "TextInput.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ForgeApi.h>

#include <math.h>
#include <string.h>

using namespace Amod;

namespace Mp
{
    namespace TextInput
    {
        namespace
        {
            const char* const LEFT = "textures/menu/text_field_left.png";
            const char* const TILE = "textures/menu/text_field_tile.png";
            const char* const RIGHT = "textures/menu/text_field_right.png";

            // A piece is this square at $scale 1; a letter is about this wide at $font_scale 1.
            const float PART = 55.0f;
            const float LETTER = 13.0f;

            // Left and middle vertically, as DrawTextLine counts its align bits.
            const int ALIGN_LEFT_MIDDLE = 0x20;

            const int MAX_TEXT = 64;
            const int MAX_FIELDS = 8;

            // $id, $pos (left edge, middle, menu script space), $scale, $length; $font and $font_scale are optional.
            struct Field
            {
                char menuId[32];
                char id[32];
                float x;
                float y;
                float scale[2];
                float fontScale;
                char font[32];
                int length;
                char text[MAX_TEXT + 1];
            };

            Field g_fields[MAX_FIELDS] = {};
            int g_fieldCount = 0;

            AmodTextureId g_left = AMOD_INVALID_TEXTURE;
            AmodTextureId g_tile = AMOD_INVALID_TEXTURE;
            AmodTextureId g_right = AMOD_INVALID_TEXTURE;
            bool g_loaded = false;

            Submit g_submit = nullptr;

            HWND g_window = nullptr;
            WNDPROC g_windowProc = nullptr;

            bool Same(const char* one, const char* other)
            {
                return one != nullptr && other != nullptr && strcmp(one, other) == 0;
            }

            // An overlay takes the keys before the screen under it.
            const char* Shown()
            {
                const char* const box = Box::BoxMenu();
                return box != nullptr && box[0] != 0 ? box : Box::Current();
            }

            Field* Named(const char* id)
            {
                for (int at = 0; at < g_fieldCount; ++at)
                {
                    if (Same(g_fields[at].id, id))
                    {
                        return &g_fields[at];
                    }
                }

                return nullptr;
            }

            Field* Focused()
            {
                const char* const shown = Shown();

                for (int at = 0; at < g_fieldCount; ++at)
                {
                    if (Same(g_fields[at].menuId, shown))
                    {
                        return &g_fields[at];
                    }
                }

                return nullptr;
            }

            void Type(unsigned letter)
            {
                Field* const field = Focused();

                if (field == nullptr)
                {
                    return;
                }

                const int length = static_cast<int>(strlen(field->text));

                if (letter == '\b')
                {
                    if (length > 0)
                    {
                        field->text[length - 1] = 0;
                    }
                    return;
                }

                if (letter == '\r')
                {
                    if (g_submit != nullptr)
                    {
                        g_submit(field->menuId, field->id);
                    }
                    return;
                }

                // The menu fonts carry printable ASCII and nothing else.
                if (letter < 32 || letter > 126 || length >= field->length || length >= MAX_TEXT)
                {
                    return;
                }

                field->text[length] = static_cast<char>(letter);
                field->text[length + 1] = 0;
            }

            // The game has no text entry of its own for a mod, so its window's letters are read here.
            LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
            {
                if (message == WM_CHAR && Focused() != nullptr)
                {
                    Type(static_cast<unsigned>(wparam));
                    return 0;
                }

                return CallWindowProcA(g_windowProc, window, message, wparam, lparam);
            }

            void Hook()
            {
                if (g_windowProc != nullptr)
                {
                    return;
                }

                HWND const window = static_cast<HWND>(Render::Window());

                if (window == nullptr)
                {
                    return;
                }

                g_windowProc = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc)));
                g_window = g_windowProc != nullptr ? window : nullptr;
            }

            void LoadPieces()
            {
                if (g_loaded)
                {
                    return;
                }

                g_loaded = true;
                g_left = Textures::Load(LEFT, false, true);
                g_tile = Textures::Load(TILE, false, true);
                g_right = Textures::Load(RIGHT, false, true);

                if (g_left == AMOD_INVALID_TEXTURE || g_tile == AMOD_INVALID_TEXTURE || g_right == AMOD_INVALID_TEXTURE)
                {
                    Log(AMOD_LOG_WARNING, "a text field piece is missing: %s, %s, %s", LEFT, TILE, RIGHT);
                }
            }

            void Copy(char* out, size_t size, const char* text)
            {
                strncpy_s(out, size, text != nullptr ? text : "", _TRUNCATE);
            }

            void DrawField(const Field& field)
            {
                const float wide = PART * field.scale[0];
                const float tall = PART * field.scale[1];
                const float left = field.x;
                const float top = AMOD_MENU_TALL - field.y - tall * 0.5f;

                // As many middle pieces as $length letters need.
                int tiles = static_cast<int>(ceilf(field.length * LETTER * field.fontScale / wide));
                tiles = tiles < 1 ? 1 : tiles;

                Render::Sprite(g_left, left, top, wide, tall);

                for (int at = 0; at < tiles; ++at)
                {
                    Render::Sprite(g_tile, left + wide * (1 + at), top, wide, tall);
                }

                Render::Sprite(g_right, left + wide * (1 + tiles), top, wide, tall);

                char shown[MAX_TEXT + 2] = {};
                Copy(shown, sizeof(shown), field.text);

                // The caret blinks at the end while there is room for another letter.
                if ((GetTickCount() / 500) % 2 == 0 && static_cast<int>(strlen(field.text)) < field.length)
                {
                    strcat_s(shown, sizeof(shown), "_");
                }

                const AmodFont font = Render::GetFont(field.font[0] != 0 ? field.font : "bigitem");

                Render::Text(font, left + wide * 0.6f, top + tall * 0.5f, shown, field.fontScale, 0xFFFFFFFF,
                    ALIGN_LEFT_MIDDLE);
            }

            bool IsInput(const char* name)
            {
                if (name != nullptr && name[0] == '$')
                {
                    ++name;
                }

                return Same(name, "input");
            }

            void Forget(const char* menuId)
            {
                int kept = 0;

                for (int at = 0; at < g_fieldCount; ++at)
                {
                    if (!Same(g_fields[at].menuId, menuId))
                    {
                        g_fields[kept++] = g_fields[at];
                    }
                }

                g_fieldCount = kept;
            }
        }

        bool Load(const char* menuId, const char* scriptFile)
        {
            const Data::Block root = Data::ParseFile(scriptFile);

            if (!root.IsValid())
            {
                Log(AMOD_LOG_WARNING, "%s could not be read for its $input blocks", scriptFile);
                return false;
            }

            Forget(menuId);

            const int count = root.ChildCount();

            for (int at = 0; at < count && g_fieldCount < MAX_FIELDS; ++at)
            {
                const Data::Block block = root.ChildAt(at);

                if (!IsInput(block.Name()))
                {
                    continue;
                }

                Field& field = g_fields[g_fieldCount++];

                field = Field{};
                Copy(field.menuId, sizeof(field.menuId), menuId);
                Copy(field.id, sizeof(field.id), block["id"].Text(""));
                Copy(field.font, sizeof(field.font), block["font"].Text(""));
                field.x = block["pos"].Number(0, 320.0f);
                field.y = block["pos"].Number(1, 240.0f);
                field.scale[0] = block["scale"].Number(0, 1.0f);
                field.scale[1] = block["scale"].Number(1, field.scale[0]);
                field.fontScale = block["font_scale"].Number(0, 1.0f);
                field.length = block["length"].Int(16);
                field.length = field.length < 1 ? 1 : (field.length > MAX_TEXT ? MAX_TEXT : field.length);
            }

            root.Free();
            Hook();
            return true;
        }

        void Answer(Submit submit)
        {
            g_submit = submit;
        }

        const char* Text(const char* fieldId)
        {
            const Field* const field = Named(fieldId);
            return field != nullptr ? field->text : "";
        }

        void SetText(const char* fieldId, const char* text)
        {
            Field* const field = Named(fieldId);

            if (field != nullptr)
            {
                Copy(field->text, sizeof(field->text), text);
                field->text[field->length] = 0;
            }
        }

        void Draw()
        {
            const char* const shown = Shown();

            for (int at = 0; at < g_fieldCount; ++at)
            {
                if (Same(g_fields[at].menuId, shown))
                {
                    LoadPieces();
                    DrawField(g_fields[at]);
                }
            }
        }

        void Close()
        {
            if (g_window != nullptr && g_windowProc != nullptr)
            {
                SetWindowLongPtrA(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_windowProc));
            }

            g_window = nullptr;
            g_windowProc = nullptr;
            g_fieldCount = 0;
        }
    }
}
