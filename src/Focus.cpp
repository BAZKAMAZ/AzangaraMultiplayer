#include "Focus.h"

#include <ForgeApi.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

using namespace Amod;

namespace Mp
{
    namespace Focus
    {
        namespace
        {
            typedef SHORT(WINAPI* AsyncKeyRoutine)(int key);

            AsyncKeyRoutine g_real = nullptr;
            uintptr_t g_slot = 0;
            bool g_hasKeyboard = true;

            bool Foreground()
            {
                static DWORD asked = 0;

                const DWORD now = GetTickCount();
                if (now != asked)
                {
                    asked = now;

                    DWORD process = 0;
                    GetWindowThreadProcessId(GetForegroundWindow(), &process);
                    g_hasKeyboard = process == GetCurrentProcessId();
                }

                return g_hasKeyboard;
            }

            SHORT WINAPI AsyncKeyState(int key)
            {
                if (!Foreground())
                {
                    return 0;
                }

                return g_real != nullptr ? g_real(key) : 0;
            }

            uintptr_t SlotFor(const char* dll, const char* name, const void* routine)
            {
                const uintptr_t base = GameModuleBase();
                if (base == 0)
                {
                    return 0;
                }

                const IMAGE_DOS_HEADER* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                {
                    return 0;
                }

                const IMAGE_NT_HEADERS32* const nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);

                if (nt->Signature != IMAGE_NT_SIGNATURE)
                {
                    return 0;
                }

                const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

                if (imports.VirtualAddress == 0)
                {
                    return 0;
                }

                const IMAGE_IMPORT_DESCRIPTOR* descriptor =
                    reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);

                for (; descriptor->Name != 0; ++descriptor)
                {
                    if (descriptor->FirstThunk == 0)
                    {
                        continue;
                    }

                    uintptr_t* const slots = reinterpret_cast<uintptr_t*>(base + descriptor->FirstThunk);

                    if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), dll) == 0)
                    {
                        // A bound import has no name table of its own and points at the slots.
                        const uint32_t* const named = reinterpret_cast<const uint32_t*>(
                            base + (descriptor->OriginalFirstThunk != 0
                                        ? descriptor->OriginalFirstThunk
                                        : descriptor->FirstThunk));

                        for (int at = 0; named[at] != 0; ++at)
                        {
                            if ((named[at] & IMAGE_ORDINAL_FLAG32) != 0)
                            {
                                continue;
                            }

                            const IMAGE_IMPORT_BY_NAME* const entry = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + named[at]);

                            if (strcmp(reinterpret_cast<const char*>(entry->Name), name) == 0)
                            {
                                return reinterpret_cast<uintptr_t>(&slots[at]);
                            }
                        }
                    }

                    if (routine == nullptr)
                    {
                        continue;
                    }

                    for (uintptr_t* slot = slots; *slot != 0; ++slot)
                    {
                        if (*slot == reinterpret_cast<uintptr_t>(routine))
                        {
                            return reinterpret_cast<uintptr_t>(slot);
                        }
                    }
                }

                return 0;
            }
        }

        bool Take()
        {
            if (g_slot != 0)
            {
                return true;
            }

            if (!Interop::IsAvailable())
            {
                Log(AMOD_LOG_WARNING, "no interop; every copy on this machine takes the same keys");
                return false;
            }

            const HMODULE user32 = GetModuleHandleA("user32.dll");

            void* const real = user32 != nullptr
                ? reinterpret_cast<void*>(GetProcAddress(user32, "GetAsyncKeyState"))
                : nullptr;

            if (real == nullptr)
            {
                Log(AMOD_LOG_WARNING, "GetAsyncKeyState was not found in user32");
                return false;
            }

            const uintptr_t slot = SlotFor("user32.dll", "GetAsyncKeyState", real);
            if (slot == 0)
            {
                Log(AMOD_LOG_WARNING, "the game does not import GetAsyncKeyState by the table");
                return false;
            }

            g_real = reinterpret_cast<AsyncKeyRoutine>(real);

            void* const ours = reinterpret_cast<void*>(&AsyncKeyState);
            if (!Interop::Write(slot, &ours, sizeof(ours)))
            {
                Log(AMOD_LOG_WARNING, "the import table at 0x%08X would not take the write", slot);
                g_real = nullptr;
                return false;
            }

            g_slot = slot;
            Log(AMOD_LOG_INFO, "keys are taken only in front (import at 0x%08X)", slot);

            return true;
        }

        void Release()
        {
            if (g_slot == 0)
            {
                return;
            }

            void* const real = reinterpret_cast<void*>(g_real);
            Interop::Write(g_slot, &real, sizeof(real));

            g_slot = 0;
            g_real = nullptr;
            g_hasKeyboard = true;
        }

        bool IsOn()
        {
            return g_slot != 0;
        }

        bool HasKeyboard()
        {
            return g_slot == 0 || g_hasKeyboard;
        }
    }
}
