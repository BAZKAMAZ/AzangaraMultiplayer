#include "Spent.h"

#include <ForgeApi.h>

#include <string.h>

using namespace Amod;

namespace Mp
{
    namespace Spent
    {
        namespace
        {
            const int MASK_BYTES = MAX_PLACES / 8;

            // Room data plus four placement masks; the fourth stores each door's swing direction.
            const int ROOM_BYTES = 2 + 4 * (1 + MASK_BYTES);

            int g_rooms = 0;
            int g_gone = 0;

            bool Has(const uint8_t* mask, int at)
            {
                return (mask[at / 8] & (1 << (at % 8))) != 0;
            }

            void Set(uint8_t* mask, int at)
            {
                mask[at / 8] = static_cast<uint8_t>(mask[at / 8] | (1 << (at % 8)));
            }

            // Reads a placed monster's live state directly from its placement record.
            bool MonsterAlive(int room, int index)
            {
                if (Monsters::g_monsters == nullptr ||
                    Monsters::g_monsters->GetPlacedProperty == nullptr)
                {
                    return true;
                }

                AmodValue value = {};

                if (Monsters::g_monsters->GetPlacedProperty(Monsters::g_monsterSelf, room, index,
                    "live", &value) == 0)
                {
                    return true;
                }

                return value.kind != AMOD_VALUE_INT || value.integer != 0;
            }

            // Ctrl_Spawn's door kind; only doors use state 1/2 as open states.
            const int DOOR_KIND = 2;

            bool IsDoor(int room, int at)
            {
                AmodCtrlPlacement placement = {};

                return Ctrls::GetPlacement(at, placement, room) && placement.kind == DOOR_KIND;
            }

            // Door state 1 or 2 means open; other states are temporary swing animations.
            bool DoorOpen(int room, int at)
            {
                AmodValue value = {};

                return Ctrls::GetPlacementProperty(at, "state", value, room) && value.kind == AMOD_VALUE_INT 
                    && (value.integer == 1 || value.integer == 2);
            }

            // Returns whether the door's open_speed points in the reverse direction.
            bool DoorBackwards(int room, int at)
            {
                AmodValue value = {};

                return Ctrls::GetPlacementProperty(at, "open_speed", value, room) && value.kind == AMOD_VALUE_FLOAT 
                    && value.count > 0 && value.numbers[0] < 0.0f;
            }

            void SwingDoor(int room, int at, bool backwards)
            {
                AmodValue value = {};

                if (!Ctrls::GetPlacementProperty(at, "open_speed", value, room) || value.kind != AMOD_VALUE_FLOAT || value.count < 1)
                {
                    return;
                }

                const float size = value.numbers[0] < 0.0f ? -value.numbers[0] : value.numbers[0];

                Ctrls::SetPlacementProperty(at, "open_speed", Value(backwards ? -size : size), room);
            }

            // Opening a door also clears its tag and touchable flags.
            void OpenLive(int room, int at)
            {
                AmodCtrlPlacement placement = {};

                const int index = Ctrls::GetPlacement(at, placement, room)
                    ? Ctrls::IndexOf(placement.id) : -1;

                if (index >= 0)
                {
                    Ctrls::SetTagOn(index, false);
                    Ctrls::SetTouchable(index, false);
                }
            }

            // Collects changed bonuses, monsters, doors and their swing directions for one room.
            bool MasksOf(int room, uint8_t* taken, uint8_t* dead, uint8_t* open, uint8_t* back, int& bonuses, int& monsters, int& things, int& gone)
            {
                bonuses = Bonuses::PlacementCountIn(room);
                monsters = Monsters::PlacedCount(room);
                things = Ctrls::PlacementCount(room);

                bool any = false;

                for (int which = 0; which < bonuses && which < MAX_PLACES; ++which)
                {
                    AmodBonusSpawnInfo info = {};

                    if (Bonuses::GetPlacement(which, info, room) && info.isLive == 0)
                    {
                        Set(taken, which);
                        any = true;
                        ++gone;
                    }
                }

                for (int which = 0; which < monsters && which < MAX_PLACES; ++which)
                {
                    if (!MonsterAlive(room, which))
                    {
                        Set(dead, which);
                        any = true;
                        ++gone;
                    }
                }

                for (int which = 0; which < things && which < MAX_PLACES; ++which)
                {
                    if (IsDoor(room, which) && DoorOpen(room, which))
                    {
                        Set(open, which);
                        any = true;
                        ++gone;

                        if (DoorBackwards(room, which))
                        {
                            Set(back, which);
                        }
                    }
                }

                return any;
            }

            // Stores the last state sent for each room.
            uint32_t g_said[MAX_ROOMS] = {};

            // Room where the last packet ran out of space.
            int g_from = 0;

            uint32_t Fold(const uint8_t* taken, const uint8_t* dead, const uint8_t* open,
                const uint8_t* back)
            {
                uint32_t sum = 2166136261u;

                for (int at = 0; at < MASK_BYTES; ++at)
                {
                    sum = (sum ^ taken[at]) * 16777619u;
                    sum = (sum ^ dead[at]) * 16777619u;
                    sum = (sum ^ open[at]) * 16777619u;
                    sum = (sum ^ back[at]) * 16777619u;
                }

                return sum;
            }
        }

        void Watch()
        {
        }

        void Forget()
        {
            g_rooms = 0;
            g_gone = 0;

            memset(g_said, 0, sizeof(g_said));
        }

        // Checks whether the current room has changed since it was last sent.
        bool Changed()
        {
            const int room = World::CurrentRoom();

            if (room < 0 || room >= MAX_ROOMS)
            {
                return false;
            }

            uint8_t taken[MASK_BYTES] = {};
            uint8_t dead[MASK_BYTES] = {};
            uint8_t open[MASK_BYTES] = {};
            uint8_t back[MASK_BYTES] = {};
            int bonuses = 0;
            int monsters = 0;
            int things = 0;
            int gone = 0;

            MasksOf(room, taken, dead, open, back, bonuses, monsters, things, gone);

            return Fold(taken, dead, open, back) != g_said[room];
        }

        int Read(uint8_t* out, int max)
        {
            if (out == nullptr || max < 1)
            {
                return 0;
            }

            const int rooms = World::RoomCount() < MAX_ROOMS ? World::RoomCount() : MAX_ROOMS;
            int at = 1;
            int written = 0;
            int gone = 0;
            int left = -1;

            // Resume from the last room that did not fit so later rooms are not skipped.
            for (int step = 0; step < rooms; ++step)
            {
                const int room = (g_from + step) % rooms;

                uint8_t taken[MASK_BYTES] = {};
                uint8_t dead[MASK_BYTES] = {};
                uint8_t open[MASK_BYTES] = {};
                uint8_t back[MASK_BYTES] = {};
                int bonuses = 0;
                int monsters = 0;
                int things = 0;

                const bool any = MasksOf(room, taken, dead, open, back, bonuses, monsters, things, gone);

                if (!any || at + ROOM_BYTES > max)
                {
                    // Remember unsent changed rooms and mark empty rooms as already reported.
                    if (any)
                    {
                        left = left < 0 ? room : left;
                    }
                    else
                    {
                        g_said[room] = Fold(taken, dead, open, back);
                    }

                    continue;
                }

                out[at++] = static_cast<uint8_t>(room & 0xFF);
                out[at++] = static_cast<uint8_t>((room >> 8) & 0xFF);

                out[at++] = static_cast<uint8_t>(bonuses < MAX_PLACES ? bonuses : MAX_PLACES);
                memcpy(out + at, taken, MASK_BYTES);
                at += MASK_BYTES;

                out[at++] = static_cast<uint8_t>(monsters < MAX_PLACES ? monsters : MAX_PLACES);
                memcpy(out + at, dead, MASK_BYTES);
                at += MASK_BYTES;

                out[at++] = static_cast<uint8_t>(things < MAX_PLACES ? things : MAX_PLACES);
                memcpy(out + at, open, MASK_BYTES);
                at += MASK_BYTES;

                out[at++] = static_cast<uint8_t>(things < MAX_PLACES ? things : MAX_PLACES);
                memcpy(out + at, back, MASK_BYTES);
                at += MASK_BYTES;

                g_said[room] = Fold(taken, dead, open, back);
                ++written;
            }

            g_from = left < 0 ? 0 : left;
            g_rooms = written;
            g_gone = gone;

            if (written == 0)
            {
                return 0;
            }

            out[0] = static_cast<uint8_t>(written);
            return at;
        }

        void Apply(const uint8_t* data, int size)
        {
            if (data == nullptr || size < 1)
            {
                return;
            }

            const int rooms = data[0];
            int at = 1;
            int gone = 0;

            for (int which = 0; which < rooms; ++which)
            {
                if (at + ROOM_BYTES > size)
                {
                    break;
                }

                const int room = data[at] | (data[at + 1] << 8);
                at += 2;

                const int bonuses = data[at++];
                const uint8_t* const taken = data + at;
                at += MASK_BYTES;

                const int monsters = data[at++];
                const uint8_t* const dead = data + at;
                at += MASK_BYTES;

                const int things = data[at++];
                const uint8_t* const open = data + at;
                at += MASK_BYTES;

                const int swung = data[at++];
                const uint8_t* const back = data + at;
                at += MASK_BYTES;

                for (int one = 0; one < bonuses && one < MAX_PLACES; ++one)
                {
                    // Remove only if the bonus is still present.
                    if (Has(taken, one) && Bonuses::RemovePlacement(one, room))
                    {
                        ++gone;
                    }
                }

                for (int one = 0; one < monsters && one < MAX_PLACES; ++one)
                {
                    // Remove the placed monster so both the record and active instance are gone.
                    if (Has(dead, one) && Monsters::RemovePlaced(one, room))
                    {
                        ++gone;
                    }
                }

                for (int one = 0; one < things && one < MAX_PLACES; ++one)
                {
                    if (Has(open, one) && IsDoor(room, one) && !DoorOpen(room, one))
                    {
                        // Set the swing direction before starting the door's open state.
                        SwingDoor(room, one, one < swung && Has(back, one));

                        Ctrls::SetPlacementProperty(one, "state", Value(1), room);
                        OpenLive(room, one);
                        ++gone;
                    }
                }
            }

            g_rooms = rooms;
            g_gone = gone;
        }

        void Doors(int room, void (*line)(const char* text))
        {
            if (line == nullptr)
            {
                return;
            }

            char text[256];
            const int things = Ctrls::PlacementCount(room);

            sprintf_s(text, "mp: room %d holds %d placement(s)\n", room, things);
            line(text);

            for (int at = 0; at < things && at < MAX_PLACES; ++at)
            {
                AmodCtrlPlacement placement = {};

                if (!Ctrls::GetPlacement(at, placement, room))
                {
                    continue;
                }

                AmodValue value = {};
                const bool has = Ctrls::GetPlacementProperty(at, "state", value, room) &&
                    value.kind == AMOD_VALUE_INT;
                const int index = Ctrls::IndexOf(placement.id);
                AmodCtrl live = {};
                const bool here = index >= 0 && Ctrls::Get(index, live);

                sprintf_s(text, "mp:  %d %s kind=%d%s state=%d live=%d tag=%d%s\n", at,
                    placement.kindName != nullptr ? placement.kindName : "?",
                    placement.kind,
                    placement.kind == DOOR_KIND ? " DOOR" : "",
                    has ? value.integer : -1,
                    here ? index : -1,
                    here ? live.tagOn : -1,
                    placement.kind == DOOR_KIND && has && (value.integer == 1 || value.integer == 2)
                    ? " OPEN" : "");
                line(text);
            }
        }

        int Rooms()
        {
            return g_rooms;
        }

        int Gone()
        {
            return g_gone;
        }
    }
}