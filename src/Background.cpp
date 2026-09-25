#include "Background.h"

#include <ForgeApi.h>

using namespace Amod;

namespace Mp
{
    namespace Background
    {
        namespace
        {
            //
            //   xor dl, dl
            //   cmp [ecx+5], dl      is the window active
            //   jnz awake
            //   cmp [ecx+6], dl      or is a repaint pending
            //   jnz awake
            //   xor al, al           <- the two bytes, at +0x0C
            //   mov [ecx+6], dl
            //   ret
            // awake:
            //   mov al, 1
            //   mov [ecx+6], dl
            //   ret
            const char* const SIGNATURE = "32 D2 38 51 05 75 0B 38 51 06 75 06 32 C0 88 51 06 C3 B0 01 88 51 06 C3";

            const uint32_t ASLEEP_AT = 0x0C;

            // mov al, 1 in place of xor al, al: the answer becomes yes, and clearing the repaint
            // flag below it still happens.
            const uint8_t AWAKE[2] = { 0xB0, 0x01 };

            //
            //   cmp [esp+4], al      al is 0 here
            //   jnz out              still in front, nothing to do
            //   mov [esp+4], 1
            //   jmp Game_SetPaused
            //
            const char* const PAUSE_SIGNATURE =
                "8B 0D ?? ?? ?? ?? 85 C9 74 09 E8 ?? ?? ?? ?? 84 C0 75 2A 80 3D ?? ?? ?? ?? 00 "
                "74 21 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 75 13 38 44 24 04 75 0D "
                "C7 44 24 04 01 00 00 00 E9 ?? ?? ?? ?? C3";

            const uint8_t RETURN[1] = { 0xC3 };

            uintptr_t g_at = 0;
            uint8_t g_was[2] = {};

            uintptr_t g_pauseAt = 0;
            uint8_t g_pauseWas[1] = {};

            bool TakePause()
            {
                if (g_pauseAt != 0)
                {
                    return true;
                }

                AmodFindResult why = AMOD_FIND_NOT_FOUND;
                const uintptr_t found = Interop::Find(PAUSE_SIGNATURE, &why);

                if (found == 0)
                {
                    Log(AMOD_LOG_WARNING, "the activate callback was not found (%s); losing focus will still pause",
                        why == AMOD_FIND_AMBIGUOUS ? "more than one match" : "no match");
                    return false;
                }

                if (!Interop::Read(found, g_pauseWas, sizeof(g_pauseWas)) ||
                    !Interop::Write(found, RETURN, sizeof(RETURN)))
                {
                    Log(AMOD_LOG_WARNING, "the activate callback at 0x%08X would not take the write", found);
                    return false;
                }

                g_pauseAt = found;
                Log(AMOD_LOG_INFO, "losing focus no longer pauses the world (0x%08X: %02X -> C3)",
                    found, g_pauseWas[0]);

                return true;
            }
        }

        bool Take()
        {
            if (g_at != 0)
            {
                return true;
            }

            if (!Interop::IsAvailable())
            {
                Log(AMOD_LOG_WARNING, "no interop; the game will sleep when it is not in front");
                return false;
            }

            AmodFindResult why = AMOD_FIND_NOT_FOUND;
            const uintptr_t found = Interop::Find(SIGNATURE, &why);

            if (found == 0)
            {
                Log(AMOD_LOG_WARNING, "App_IsAwake was not found (%s); the game will sleep when it is not in front",
                    why == AMOD_FIND_AMBIGUOUS ? "more than one match" : "no match");
                return false;
            }

            const uintptr_t at = found + ASLEEP_AT;

            if (!Interop::Read(at, g_was, sizeof(g_was)))
            {
                Log(AMOD_LOG_WARNING, "App_IsAwake at 0x%08X would not read", at);
                return false;
            }

            if (!Interop::Write(at, AWAKE, sizeof(AWAKE)))
            {
                Log(AMOD_LOG_WARNING, "App_IsAwake at 0x%08X would not take the write", at);
                return false;
            }

            g_at = at;
            Log(AMOD_LOG_INFO, "the game keeps running behind (0x%08X: %02X %02X -> %02X %02X)",
                at, g_was[0], g_was[1], AWAKE[0], AWAKE[1]);

            // Running is not enough on its own: the world still stops, banner and all.
            TakePause();

            return true;
        }

        void Release()
        {
            if (g_pauseAt != 0)
            {
                Interop::Write(g_pauseAt, g_pauseWas, sizeof(g_pauseWas));
                g_pauseAt = 0;
            }

            if (g_at == 0)
            {
                return;
            }

            Interop::Write(g_at, g_was, sizeof(g_was));
            g_at = 0;
        }

        bool IsOn()
        {
            return g_at != 0;
        }

        bool KeepsPlaying()
        {
            return g_pauseAt != 0;
        }
    }
}
