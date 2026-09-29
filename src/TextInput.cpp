#include "TextInput.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ForgeApi.h>
#include <stdio.h>
#include <string.h>

using namespace Amod;

namespace Mp
{
    namespace TextInput
    {
        namespace
        {
            const int MAX_TEXT = 64;
            const int MAX_FIELDS = 8;

            // Caret blink, half a period each way.
            const DWORD BLINK = 500;

            struct Field
            {
                char menuId[32];
                char id[32];
                int length;
                char text[MAX_TEXT + 1];
                char written[MAX_TEXT + 2];
            };

            Field g_fields[MAX_FIELDS] = {};
            int g_fieldCount = 0;

            const int MAX_SUBMITS = 4;
            Submit g_submits[MAX_SUBMITS] = {};
            int g_submitCount = 0;

            HWND g_window = nullptr;
            WNDPROC g_windowProc = nullptr;

            AmodToken g_frame = AMOD_INVALID_TOKEN;

            bool Same(const char* one, const char* other)
            {
                return one != nullptr && other != nullptr && strcmp(one, other) == 0;
            }

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

            void Copy(char* out, size_t size, const char* text)
            {
                strncpy_s(out, size, text != nullptr ? text : "", _TRUNCATE);
            }

            // The field's $static_text, found by its $id among all the menu's pieces; statics have no lookup of their own.
            bool Write(const Field& field, const char* text)
            {
                const int count = Menu::Count(field.menuId);

                for (int at = 0; at < count; ++at)
                {
                    const Menu::Element piece = Menu::At(field.menuId, at);

                    if (piece.IsValid() && Same(piece["kind"].Text(""), "static") && Same(piece["id"].Text(""), field.id))
                    {
                        return piece["text"].Set(text);
                    }
                }

                return false;
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

                if (letter == 27)
                {
                    if (Same(field->menuId, Box::BoxMenu()))
                    {
                        Box::Back();
                    }
                    return;
                }

                if (letter == '\r')
                {
                    for (int at = 0; at < g_submitCount; ++at)
                    {
                        g_submits[at](field->menuId, field->id);
                    }
                    return;
                }

                if (letter < 32 || letter > 126 || length >= field->length || length >= MAX_TEXT)
                {
                    return;
                }

                field->text[length] = static_cast<char>(letter);
                field->text[length + 1] = 0;
            }

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

                g_windowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc)));
                g_window = g_windowProc != nullptr ? window : nullptr;
            }

            // The letters of the fields on screen, and the caret blinking after them while there is room.
            void __cdecl OnFrame(const AmodFrame*, void*)
            {
                const char* const shown = Shown();
                const bool caret = (GetTickCount() / BLINK) % 2 == 0;

                for (int at = 0; at < g_fieldCount; ++at)
                {
                    Field& field = g_fields[at];

                    if (!Same(field.menuId, shown))
                    {
                        continue;
                    }

                    char text[MAX_TEXT + 2] = {};
                    Copy(text, sizeof(text), field.text);

                    if (caret && static_cast<int>(strlen(field.text)) < field.length)
                    {
                        strcat_s(text, sizeof(text), "_");
                    }

                    if (!Same(text, field.written) && Write(field, text))
                    {
                        Copy(field.written, sizeof(field.written), text);
                    }
                }
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
                field.length = block["length"].Int(16);
                field.length = field.length < 1 ? 1 : (field.length > MAX_TEXT ? MAX_TEXT : field.length);

                // Nothing to write the letters into: the script lacks the $static_text of this $id.
                if (!Write(field, ""))
                {
                    Log(AMOD_LOG_WARNING, "%s has no $static_text with $id \"%s\" for its $input", menuId, field.id);
                }
            }

            root.Free();
            Hook();

            if (g_frame == AMOD_INVALID_TOKEN)
            {
                g_frame = Render::OnFrameBegin(&OnFrame);
            }

            return true;
        }

        void Answer(Submit submit)
        {
            if (submit != nullptr && g_submitCount < MAX_SUBMITS)
            {
                g_submits[g_submitCount++] = submit;
            }
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

        void Close()
        {
            if (g_frame != AMOD_INVALID_TOKEN)
            {
                Render::CancelFrame(g_frame);
                g_frame = AMOD_INVALID_TOKEN;
            }

            if (g_window != nullptr && g_windowProc != nullptr)
            {
                SetWindowLongPtrA(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_windowProc));
            }

            g_window = nullptr;
            g_windowProc = nullptr;
            g_fieldCount = 0;
            g_submitCount = 0;
        }
    }
}
