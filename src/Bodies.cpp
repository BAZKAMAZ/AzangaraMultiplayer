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

            // Player_Spawn puts Player_Die here, and everything that kills reads it back out.
            const uint32_t DIE_AT = 0x5C4;

            const uint32_t MARKS_AT = 0x590; // State markers
            const uint32_t TAKEN_AT = 0x594; // death screen
            const uint32_t HURT_AT = 0x25C; // fire count

            const uint32_t DEAD_MARK = 0x01;
            const uint32_t CRASHED_MARK = 0x02;
            const uint32_t BURNING_MARK = 0x04;
            const uint32_t SHOCKED_MARK = 0x08;

            const uint32_t DELTA_AT = 0x088;
            const uint32_t TOUCHED_AT = 0x1BC;
            const uint32_t PUSHED_AT = 0x1C0;
            const uint32_t HIT_AT = 0x1C4;
            const uint32_t COLLIDES_AT = 0x595;

            // body[0x16] at 0x58: 0 on foot, 1 ladder, 2 chain, 3 pipe.
            const int ON_FOOT = 0;
            const int ON_CHAIN = 2;

            // lookup at 0x0043D7C0 reads rows one to five of it looking for 'I'.
            const uint32_t CHAIN_SEARCH_AT = BODY_AT + 0xB8 + 8 + 4;
            const int CHAIN_SEARCH_SIZE = 20;

            void __cdecl NoDeath(void*, int)
            {
            }

            bool ChainInWindow(const uint8_t* body)
            {
                const uint8_t* const cells = body + CHAIN_SEARCH_AT;

                for (int at = 0; at < CHAIN_SEARCH_SIZE; ++at)
                {
                    if (cells[at] == 'I')
                    {
                        return true;
                    }
                }

                return false;
            }

            // Body_Step(body, dx, dy, jump, delta, flags)
            const char* const STEP_SIGNATURE =
                "56 8B 74 24 08 8D 86 B8 00 00 00 50 8D 4E 1C 51 8D 56 04 52 "
                "FF 15 ?? ?? ?? ?? 83 C4 0C 80 7E 60";

            // A short routine whose first instruction is a compare against the player entity
            const char* const PLAYER_SIGNATURE =
                "83 3D ?? ?? ?? ?? 00 74 24 80 3D ?? ?? ?? ?? 00 75 1B 80 3D ?? ?? ?? ?? 00 "
                "74 12 8B 0D ?? ?? ?? ?? 6A 00 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? C3";

            const uint32_t PLAYER_OPERAND_AT = 2;

            const char* const TOUCH_SIGNATURE =
                "51 57 8B 7C 24 0C 80 BF B4 05 00 00 00 C6 44 24 07 00 0F 84 ?? ?? ?? ?? "
                "83 BF 3C 01 00 00 00 0F 85 ?? ?? ?? ?? 53 55 56";

            // Entity_CollideRoomBoxes(body). Entities_Update calls it after every entity's own update
            const char* const COLLIDE_SIGNATURE =
                "A1 ?? ?? ?? ?? 81 EC 44 01 00 00 55 85 C0 0F 84 ?? ?? ?? ?? "
                "8B AC 24 4C 01 00 00 83 7D 1C 00 0F 85 ?? ?? ?? ?? 53 56";

            typedef int(__cdecl* StepRoutine)(void* body, int dirX, int dirY, int jump, float delta, int flags);
            typedef char(__stdcall* CollideRoutine)(void* body);
            typedef char(__cdecl* TouchRoutine)(void* entity);

            StepRoutine g_step = nullptr;
            CollideRoutine g_collide = nullptr;
            TouchRoutine g_touch = nullptr;
            bool g_touching = true;
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

            const uintptr_t touch = Interop::Find(TOUCH_SIGNATURE);
            if (touch == 0)
            {
                g_trouble = "Entities_ResolveTouches was not found";
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
            g_touch = reinterpret_cast<TouchRoutine>(touch);
            g_playerAt = global;
            g_trouble = "";

            Log(AMOD_LOG_INFO, "bodies are open (step 0x%08X, boxes 0x%08X, touches 0x%08X, player at 0x%08X)",
                step, collide, touch, global);
            return true;
        }

        void Touching(bool on)
        {
            g_touching = on;
        }

        bool IsTouching()
        {
            return g_touching && g_touch != nullptr;
        }

        void Close()
        {
            g_step = nullptr;
            g_collide = nullptr;
            g_touch = nullptr;
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

            // Keep ladder, chain and pipe handlers, but replace the death handler so client deaths never kill the host player.
            const void* const nothing = reinterpret_cast<const void*>(&NoDeath);
            memcpy(body + DIE_AT, &nothing, sizeof(nothing));

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

            uint8_t* const entity = static_cast<uint8_t*>(body);

            int state = 0;
            memcpy(&state, entity + STATE_AT, sizeof(state));

            if (state == ON_CHAIN && !ChainInWindow(entity))
            {
                const int onFoot = ON_FOOT;

                memcpy(entity + STATE_AT, &onFoot, sizeof(onFoot));
                memcpy(entity + SUBSTATE_AT, &onFoot, sizeof(onFoot));
            }

            // Before the step, exactly as Entities_Update does it for every entity it walks.
            memcpy(entity + PREVIOUS_AT, entity + POSITION_AT, sizeof(float) * 3);
            memcpy(entity + DELTA_AT, &delta, sizeof(delta));

            void* const at = entity + BODY_AT;
            const int stopped = g_step(at, x, y, jump != 0 ? 1 : 0, delta, 0);

            const uint32_t nothing = 0;

            memcpy(entity + HIT_AT, &nothing, sizeof(nothing));
            memcpy(entity + TOUCHED_AT, &nothing, sizeof(nothing));
            memcpy(entity + PUSHED_AT, &nothing, sizeof(nothing));

            uint32_t hit = 0;
            uint32_t was = 0;

            if (g_collide != nullptr && entity[COLLIDES_AT] != 0)
            {
                memcpy(&was, entity + HIT_AT, sizeof(was));
                memcpy(entity + HIT_AT, &nothing, sizeof(nothing));

                g_collide(at);

                memcpy(&hit, entity + HIT_AT, sizeof(hit));

                uint32_t pushed = 0;
                memcpy(&pushed, entity + PUSHED_AT, sizeof(pushed));
                pushed |= hit;
                memcpy(entity + PUSHED_AT, &pushed, sizeof(pushed));

                hit |= was;
                memcpy(entity + HIT_AT, &hit, sizeof(hit));
            }

            if (g_touching && g_touch != nullptr)
            {
                memcpy(&was, entity + HIT_AT, sizeof(was));
                memcpy(entity + HIT_AT, &nothing, sizeof(nothing));

                g_touch(body);

                memcpy(&hit, entity + HIT_AT, sizeof(hit));

                uint32_t touched = 0;
                memcpy(&touched, entity + TOUCHED_AT, sizeof(touched));
                touched |= hit;
                memcpy(entity + TOUCHED_AT, &touched, sizeof(touched));

                hit |= was;
                memcpy(entity + HIT_AT, &hit, sizeof(hit));
            }

            // Nothing that hits this copy is allowed to stick; deaths are handled by the local player.
            uint32_t marks = 0;
            memcpy(&marks, entity + MARKS_AT, sizeof(marks));

            if ((marks & (DEAD_MARK | CRASHED_MARK | BURNING_MARK | SHOCKED_MARK)) != 0)
            {
                marks &= ~(DEAD_MARK | CRASHED_MARK | BURNING_MARK | SHOCKED_MARK);
                memcpy(entity + MARKS_AT, &marks, sizeof(marks));
            }

            const uint8_t alive = 0;
            const float unhurt = 0.0f;

            memcpy(entity + TAKEN_AT, &alive, sizeof(alive));
            memcpy(entity + HURT_AT, &unhurt, sizeof(unhurt));

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
