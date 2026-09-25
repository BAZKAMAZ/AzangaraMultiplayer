#include "Bodies.h"

#include <ForgeApi.h>

#include <string.h>

using namespace Amod;

namespace Mp
{
    namespace Bodies
    {
        namespace
        {
            const uint32_t ENTITY_SIZE = 0x5D4;
            const uint32_t BODY_AT = 0x120;
            const uint32_t POSITION_AT = 0x124;
            const uint32_t PREVIOUS_AT = 0x130;
            const uint32_t STATE_AT = 0x178;
            const uint32_t SUBSTATE_AT = 0x17C;

            // Body_Step's branches, gated by the flags at body+0. Player_Spawn sets 0x5CF.
            const uint32_t LADDER_FLAG = 0x40;
            const uint32_t CHAIN_FLAG = 0x80;
            const uint32_t PIPE_FLAG = 0x100;

            // Body_Step(body, dx, dy, jump, delta, flags)
            const char* const STEP_SIGNATURE =
                "56 8B 74 24 08 8D 86 B8 00 00 00 50 8D 4E 1C 51 8D 56 04 52 "
                "FF 15 ?? ?? ?? ?? 83 C4 0C 80 7E 60";

            // A short routine whose first instruction is a compare against the player entity
            const char* const PLAYER_SIGNATURE =
                "83 3D ?? ?? ?? ?? 00 74 24 80 3D ?? ?? ?? ?? 00 75 1B 80 3D ?? ?? ?? ?? 00 "
                "74 12 8B 0D ?? ?? ?? ?? 6A 00 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? C3";

            const uint32_t PLAYER_OPERAND_AT = 2;

            // Entity_CollideRoomBoxes(body). Entities_Update calls it after every entity's own update
            const char* const COLLIDE_SIGNATURE =
                "A1 ?? ?? ?? ?? 81 EC 44 01 00 00 55 85 C0 0F 84 ?? ?? ?? ?? "
                "8B AC 24 4C 01 00 00 83 7D 1C 00 0F 85 ?? ?? ?? ?? 53 56";

            typedef int(__cdecl* StepRoutine)(void* body, int dirX, int dirY, int jump, float delta, int flags);
            typedef char(__stdcall* CollideRoutine)(void* body);

            StepRoutine g_step = nullptr;
            CollideRoutine g_collide = nullptr;
            uintptr_t g_playerAt = 0;
            const char* g_trouble = "not opened";

            uintptr_t PlayerEntity()
            {
                uintptr_t entity = 0;

                if (g_playerAt == 0 || !Interop::ReadValue(g_playerAt, entity))
                {
                    return 0;
                }

                return entity;
            }
        }

        bool Open()
        {
            if (g_step != nullptr)
            {
                return true;
            }

            if (!Interop::IsAvailable())
            {
                g_trouble = "no interop";
                return false;
            }

            const uintptr_t step = Interop::Find(STEP_SIGNATURE);
            if (step == 0)
            {
                g_trouble = "Body_Step was not found";
                return false;
            }

            const uintptr_t collide = Interop::Find(COLLIDE_SIGNATURE);
            if (collide == 0)
            {
                g_trouble = "Entity_CollideRoomBoxes was not found";
                return false;
            }

            const uintptr_t anchor = Interop::Find(PLAYER_SIGNATURE);
            if (anchor == 0)
            {
                g_trouble = "the player entity was not found";
                return false;
            }

            uint32_t global = 0;
            if (!Interop::ReadValue(anchor + PLAYER_OPERAND_AT, global) || global == 0)
            {
                g_trouble = "the player entity address would not read";
                return false;
            }

            g_step = reinterpret_cast<StepRoutine>(step);
            g_collide = reinterpret_cast<CollideRoutine>(collide);
            g_playerAt = global;
            g_trouble = "";

            Log(AMOD_LOG_INFO, "bodies are open (step 0x%08X, boxes 0x%08X, player at 0x%08X)",
                step, collide, global);
            return true;
        }

        void Close()
        {
            g_step = nullptr;
            g_collide = nullptr;
            g_playerAt = 0;
            g_trouble = "not opened";
        }

        bool IsOpen()
        {
            return g_step != nullptr;
        }

        void* Create(const float position[3])
        {
            const uintptr_t player = PlayerEntity();
            if (player == 0 || position == nullptr)
            {
                return nullptr;
            }

            uint8_t* const body = new uint8_t[ENTITY_SIZE];

            if (!Interop::Read(player, body, ENTITY_SIZE))
            {
                delete[] body;
                return nullptr;
            }

            uint32_t flags = 0;
            memcpy(&flags, body + BODY_AT, sizeof(flags));
            flags &= ~(LADDER_FLAG | CHAIN_FLAG | PIPE_FLAG);
            memcpy(body + BODY_AT, &flags, sizeof(flags));

            // And it starts on foot, whatever the player was doing when he was copied.
            const int onFoot = 0;
            memcpy(body + STATE_AT, &onFoot, sizeof(onFoot));
            memcpy(body + SUBSTATE_AT, &onFoot, sizeof(onFoot));

            Place(body, position);
            return body;
        }

        void Destroy(void* body)
        {
            delete[] static_cast<uint8_t*>(body);
        }

        int Step(void* body, int dirX, int dirY, int jump, float delta)
        {
            if (g_step == nullptr || body == nullptr)
            {
                return -1;
            }

            const int x = dirX < 0 ? -1 : (dirX > 0 ? 1 : 0);
            const int y = dirY < 0 ? -1 : (dirY > 0 ? 1 : 0);

            void* const at = static_cast<uint8_t*>(body) + BODY_AT;
            const int stopped = g_step(at, x, y, jump != 0 ? 1 : 0, delta, 0);

            if (g_collide != nullptr)
            {
                g_collide(at);
            }

            return stopped;
        }

        void Read(void* body, float position[3], int& state, int& subState)
        {
            state = 0;
            subState = 0;

            if (body == nullptr)
            {
                return;
            }

            const uint8_t* const at = static_cast<const uint8_t*>(body);

            if (position != nullptr)
            {
                memcpy(position, at + POSITION_AT, sizeof(float) * 3);
            }

            memcpy(&state, at + STATE_AT, sizeof(int));
            memcpy(&subState, at + SUBSTATE_AT, sizeof(int));
        }

        void Place(void* body, const float position[3])
        {
            if (body == nullptr || position == nullptr)
            {
                return;
            }

            uint8_t* const at = static_cast<uint8_t*>(body);

            memcpy(at + POSITION_AT, position, sizeof(float) * 3);
            memcpy(at + PREVIOUS_AT, position, sizeof(float) * 3);
        }

        const char* Trouble()
        {
            return g_trouble;
        }
    }
}
