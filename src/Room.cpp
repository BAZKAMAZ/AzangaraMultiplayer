#include "Room.h"

#include <ForgeApi.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

using namespace Amod;

namespace Mp
{
    namespace Room
    {
        namespace
        {
            struct Known
            {
                AmodCtrlId id;

                // Cache ctrl indices because each lookup scans the live entity list.
                int index;
                bool asked;

                // Doors are synchronized through the spent list instead of this packet.
                bool isDoor;

                // Moving objects sync their position after they are detected as movers.
                bool moves;
                float was[3];

                // Use state, or shown for disappearing platforms, plus an optional timer.
                const char* phase;
                bool hasTimer;

                // Track phase divergence and apply delayed corrections when needed.
                DWORD sinceApart;
                bool theirs;
                bool ours;
                bool started;
                DWORD askedAt;
            };

            Known g_known[MAX_THINGS] = {};
            int g_knownCount = 0;

            bool g_trace = false;

            // Positions within this distance are considered equal.
            const float SAME = 0.02f;

            // Larger gaps are corrected immediately.
            const float TOO_FAR = 1.0f;

            bool Apart(float mine, float theirs)
            {
                const float gap = mine - theirs;

                return gap > SAME || gap < -SAME;
            }

            Known* Find(AmodCtrlId id)
            {
                for (int at = 0; at < g_knownCount; ++at)
                {
                    if (g_known[at].id == id)
                    {
                        return &g_known[at];
                    }
                }

                return nullptr;
            }

            const int DOOR_KIND = 2;

            bool IsDoor(AmodCtrlId id)
            {
                const int count = Ctrls::PlacementCount();

                for (int at = 0; at < count; ++at)
                {
                    AmodCtrlPlacement placement = {};

                    if (Ctrls::GetPlacement(at, placement) && placement.id == id)
                    {
                        return placement.kind == DOOR_KIND;
                    }
                }

                return false;
            }
        }

        int Read(Thing* out, int max)
        {
            if (out == nullptr || !Ctrls::IsAvailable())
            {
                return 0;
            }

            const int count = Ctrls::Count();
            int written = 0;

            for (int at = 0; at < count && written < max; ++at)
            {
                AmodCtrl ctrl = {};

                if (!Ctrls::Get(at, ctrl) || ctrl.id == AMOD_CTRL_NONE)
                {
                    continue;
                }

                // Read available state and timer properties once because property checks scan the entity list.
                Known* known = Find(ctrl.id);

                if (known == nullptr && g_knownCount < MAX_THINGS)
                {
                    known = &g_known[g_knownCount++];
                    *known = Known{};
                    known->id = ctrl.id;
                    known->index = at;
                    known->isDoor = IsDoor(ctrl.id);
                    known->was[0] = ctrl.position[0];
                    known->was[1] = ctrl.position[1];
                    known->was[2] = ctrl.position[2];
                }

                if (known != nullptr ? known->isDoor : IsDoor(ctrl.id))
                {
                    continue;
                }

                if (known != nullptr)
                {
                    // Once movement is detected, keep syncing this object's position.
                    known->moves = known->moves ||
                        Apart(known->was[0], ctrl.position[0]) ||
                        Apart(known->was[1], ctrl.position[1]) ||
                        Apart(known->was[2], ctrl.position[2]);

                    known->was[0] = ctrl.position[0];
                    known->was[1] = ctrl.position[1];
                    known->was[2] = ctrl.position[2];
                }

                Thing& thing = out[written++];

                thing.id = ctrl.id;
                thing.position[0] = ctrl.position[0];
                thing.position[1] = ctrl.position[1];
                thing.position[2] = ctrl.position[2];
                thing.flags = static_cast<uint8_t>(ctrl.tagOn != 0 ? THING_TAG_ON : 0);
                thing.state = 0;
                thing.timer = 0.0f;

                if (known != nullptr && known->moves)
                {
                    thing.flags |= THING_MOVES;
                }

                if (known != nullptr && !known->asked)
                {
                    AmodValue first = {};

                    known->asked = true;
                    known->phase = nullptr;

                    if (Ctrls::GetProperty(at, "state", first) && first.kind == AMOD_VALUE_INT)
                    {
                        known->phase = "state";
                    }
                    else if (Ctrls::GetProperty(at, "shown", first) && first.kind == AMOD_VALUE_INT)
                    {
                        known->phase = "shown";
                    }

                    known->hasTimer = Ctrls::GetProperty(at, "timer", first) && first.kind == AMOD_VALUE_FLOAT;
                }

                if (known != nullptr)
                {
                    AmodValue value = {};

                    if (known->phase != nullptr &&
                        Ctrls::GetProperty(at, known->phase, value) && value.kind == AMOD_VALUE_INT)
                    {
                        thing.state = value.integer;
                        thing.flags |= THING_HAS_STATE;
                    }

                    if (known->hasTimer && Ctrls::GetProperty(at, "timer", value) && value.kind == AMOD_VALUE_FLOAT && value.count > 0)
                    {
                        thing.timer = value.numbers[0];
                        thing.flags |= THING_HAS_TIMER;
                    }
                }
            }

            return written;
        }

        namespace
        {
            // Monster live fields are accessed through the underlying property interface.
            void Put(int index, const char* key, const AmodValue& value)
            {
                if (Monsters::g_monsters != nullptr && Monsters::g_monsters->SetProperty != nullptr)
                {
                    Monsters::g_monsters->SetProperty(Monsters::g_monsterSelf, index, key, &value);
                }
            }

            bool Peek(int index, const char* key, AmodValue& value)
            {
                return Monsters::g_monsters != nullptr &&
                    Monsters::g_monsters->GetProperty != nullptr &&
                    Monsters::g_monsters->GetProperty(Monsters::g_monsterSelf, index, key, &value) != 0;
            }

            // The bat uses a phase-driven circular movement.
            bool Circles(const char* moves)
            {
                return moves != nullptr && strcmp(moves, "bat") == 0;
            }

        }

        int ReadMovers(Mover* out, int max)
        {
            if (out == nullptr || !Monsters::IsAvailable())
            {
                return 0;
            }

            const int count = Monsters::Count();
            int written = 0;

            for (int at = 0; at < count && written < max; ++at)
            {
                AmodMonster monster = {};

                if (!Monsters::Get(at, monster) || monster.placed < 0 ||
                    monster.placed >= MAX_MOVERS * 8)
                {
                    continue;
                }

                Mover& mover = out[written++];

                mover.placed = static_cast<uint8_t>(monster.placed);
                mover.position[0] = monster.position[0];
                mover.position[1] = monster.position[1];
                mover.position[2] = monster.position[2];
                mover.facing = static_cast<int8_t>(monster.facing);
                mover.has = 0;
                mover.phase = 0.0f;

                AmodValue value = {};

                if (Circles(monster.moves) && Peek(at, "phase", value) && value.kind == AMOD_VALUE_FLOAT && value.count > 0)
                {
                    mover.has |= MOVER_PHASE;
                    mover.phase = value.numbers[0];
                }
            }

            return written;
        }

        void ApplyMovers(const Mover* movers, int count)
        {
            if (movers == nullptr || !Monsters::IsAvailable())
            {
                return;
            }

            // Cache the room's monsters because each loader lookup scans the live entity list.
            const int live = Monsters::Count();
            AmodMonster here[MAX_MOVERS] = {};
            int found = 0;

            for (int at = 0; at < live && found < MAX_MOVERS; ++at)
            {
                if (Monsters::Get(at, here[found]))
                {
                    ++found;
                }
            }

            for (int at = 0; at < count; ++at)
            {
                const Mover& mover = movers[at];

                for (int which = 0; which < found; ++which)
                {
                    const AmodMonster& monster = here[which];

                    if (monster.placed < 0 || static_cast<uint8_t>(monster.placed) != mover.placed)
                    {
                        continue;
                    }

                    // Sync circular movers by phase; other monsters are corrected by position.
                    if ((mover.has & MOVER_PHASE) != 0)
                    {
                        Put(monster.index, "phase", Value(mover.phase));

                        if (monster.facing != mover.facing)
                        {
                            Put(monster.index, "facing", Value(static_cast<int>(mover.facing)));
                        }

                        break;
                    }

                    // Pull normal movers toward the host position instead of forcing them there.
                    static const char* const AXIS[3] = { "x", "y", "z" };
                    float gap[3] = {};
                    float worst = 0.0f;

                    for (int axis = 0; axis < 3; ++axis)
                    {
                        gap[axis] = mover.position[axis] - monster.position[axis];
                        worst = (gap[axis] > worst) ? gap[axis] : (-gap[axis] > worst ? -gap[axis] : worst);
                    }

                    if (worst > SAME)
                    {
                        const float part = worst > TOO_FAR ? 1.0f : 0.35f;

                        // Write position only when the difference is large enough to matter.
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            if (Apart(monster.position[axis], mover.position[axis]))
                            {
                                Put(monster.index, AXIS[axis], 
                                    Value(monster.position[axis] + gap[axis] * part));
                            }
                        }
                    }

                    if (monster.facing != mover.facing)
                    {
                        Put(monster.index, "facing", Value(static_cast<int>(mover.facing)));
                    }

                    break;
                }
            }
        }

        int Apply(const Thing* things, int count, uint32_t* wants, int maxWants)
        {
            int wanted = 0;

            if (things == nullptr || !Ctrls::IsAvailable())
            {
                return 0;
            }

            const DWORD now = GetTickCount();

            for (int at = 0; at < count; ++at)
            {
                const Thing& thing = things[at];

                Known* known = Find(thing.id);

                if (known == nullptr)
                {
                    if (g_knownCount >= MAX_THINGS)
                    {
                        continue;
                    }

                    known = &g_known[g_knownCount++];
                    *known = Known{};
                    known->id = thing.id;
                    known->index = -1;
                    known->isDoor = IsDoor(thing.id);
                }

                if (known->isDoor)
                {
                    continue;
                }

                AmodCtrl here = {};

                // Reuse the cached ctrl index until the entity no longer matches.
                if (known->index < 0 || !Ctrls::Get(known->index, here) || here.id != thing.id)
                {
                    known->index = Ctrls::IndexOf(thing.id);

                    if (known->index < 0 || !Ctrls::Get(known->index, here))
                    {
                        known->index = -1;
                        continue;
                    }
                }

                const int index = known->index;
                const bool theirs = (thing.flags & THING_TAG_ON) != 0;
                const bool ours = here.tagOn != 0;

                // Moving objects are corrected toward the host position instead of being snapped every frame.
                if ((thing.flags & THING_MOVES) != 0)
                {
                    float gap[3] = {};
                    float worst = 0.0f;

                    for (int axis = 0; axis < 3; ++axis)
                    {
                        gap[axis] = thing.position[axis] - here.position[axis];
                        worst = (gap[axis] > worst) ? gap[axis] : (-gap[axis] > worst ? -gap[axis] : worst);
                    }

                    if (worst > SAME)
                    {
                        const float part = worst > TOO_FAR ? 1.0f : 0.35f;
                        Ctrls::SetPosition(index,
                            here.position[0] + gap[0] * part,
                            here.position[1] + gap[1] * part,
                            here.position[2] + gap[2] * part);
                    }
                }

                // Sync machine state instead of position so each copy keeps running its own script.
                if (!known->asked)
                {
                    AmodValue first = {};

                    known->asked = true;
                    known->phase = nullptr;

                    if (Ctrls::GetProperty(index, "state", first) && first.kind == AMOD_VALUE_INT)
                    {
                        known->phase = "state";
                    }
                    else if (Ctrls::GetProperty(index, "shown", first) && first.kind == AMOD_VALUE_INT)
                    {
                        known->phase = "shown";
                    }

                    known->hasTimer = (thing.flags & THING_HAS_TIMER) != 0;
                }

                AmodValue value = {};

                // Keep the timer synchronized because it controls the next machine state change.
                if (known->hasTimer && (thing.flags & THING_HAS_TIMER) != 0 && Ctrls::GetProperty(index, "timer", value) && value.kind == AMOD_VALUE_FLOAT &&
                    value.count > 0)
                {
                    const float gap = value.numbers[0] - thing.timer;

                    if (gap > 0.1f || gap < -0.1f)
                    {
                        Ctrls::SetProperty(index, "timer", Value(thing.timer));
                    }
                }

                if (known->phase != nullptr && (thing.flags & THING_HAS_STATE) != 0 && Ctrls::GetProperty(index, known->phase, value) 
                    && value.kind == AMOD_VALUE_INT && value.integer != thing.state)
                {
                    // Let timer-driven machines reach their phase naturally; set other states directly.
                    const bool waitForIt = known->hasTimer;

                    if (known->sinceApart == 0)
                    {
                        known->sinceApart = now;
                    }

                    if (!waitForIt || (now - known->sinceApart) > 1000)
                    {
                        if (g_trace)
                        {
                            Log(AMOD_LOG_INFO, "ctrl %u (%s) %s %d -> %d", thing.id, here.name != nullptr ? here.name : "?", known->phase,
                                value.integer, thing.state);
                        }

                        Ctrls::SetProperty(index, known->phase, Value(thing.state));

                        if (known->hasTimer)
                        {
                            Ctrls::SetProperty(index, "timer", Value(thing.timer));
                        }

                        known->sinceApart = 0;
                    }
                }
                else
                {
                    known->sinceApart = 0;
                }

                if (!known->started)
                {
                    known->started = true;
                    known->theirs = theirs;
                    known->ours = ours;

                    if (ours != theirs)
                    {
                        if (g_trace)
                        {
                            Log(AMOD_LOG_INFO, "ctrl %u (%s) starts %s", thing.id, here.name != nullptr ? here.name : "?", theirs ? "on" : "off");
                        }

                        Ctrls::SetTagOn(index, theirs);
                        known->ours = theirs;
                    }

                    continue;
                }

                // Sync tag changes only when the host value changes.
                if (theirs != known->theirs)
                {
                    if (g_trace)
                    {
                        Log(AMOD_LOG_INFO, "ctrl %u (%s) -> %s", thing.id, here.name != nullptr ? here.name : "?", theirs ? "on" : "off");
                    }

                    Ctrls::SetTagOn(index, theirs);
                    known->ours = theirs;
                    known->askedAt = 0;
                }
                else if (ours && !known->ours && !theirs)
                {
                    known->askedAt = now;

                    if (wants != nullptr && wanted < maxWants)
                    {
                        wants[wanted++] = thing.id;
                    }

                    if (g_trace)
                    {
                        Log(AMOD_LOG_INFO, "ctrl %u (%s) was switched on here", thing.id, here.name != nullptr ? here.name : "?");
                    }
                }

                known->theirs = theirs;
                known->ours = ours;
            }

            return wanted;
        }

        void Hold(bool)
        {
        }

        bool Holding()
        {
            return false;
        }

        void Trace(bool on)
        {
            g_trace = on;
        }

        void LetGo()
        {
            g_knownCount = 0;
        }

        int Held()
        {
            return g_knownCount;
        }

        // Report room contents and each object's known state to the console.
        void Report(void (*line)(const char* text))
        {
            if (line == nullptr || !Ctrls::IsAvailable())
            {
                return;
            }

            char text[512];
            const int count = Ctrls::Count();

            sprintf_s(text, "mp: %d thing(s) in this room\n", count);
            line(text);

            for (int at = 0; at < count; ++at)
            {
                AmodCtrl ctrl = {};

                if (!Ctrls::Get(at, ctrl))
                {
                    continue;
                }

                const Known* const known = Find(ctrl.id);

                sprintf_s(text, "mp:  %u %s tag=%d hidden=%d%s\n", ctrl.id, ctrl.name != nullptr ? ctrl.name : "?", ctrl.tagOn,
                    ctrl.isHidden, known != nullptr && known->started
                    ? (known->theirs ? " host says on" : " host says off")
                    : "");

                line(text);

                const int properties = Ctrls::PropertyCount(at);
                int written = 0;

                text[0] = 0;

                for (int which = 0; which < properties; ++which)
                {
                    const char* const name = Ctrls::PropertyNameAt(at, which);

                    if (name == nullptr || name[0] == 0)
                    {
                        continue;
                    }

                    if (written == 0)
                    {
                        strcpy_s(text, "mp:    ");
                    }

                    if (strlen(text) + strlen(name) + 3 >= sizeof(text))
                    {
                        break;
                    }

                    strcat_s(text, name);
                    strcat_s(text, " ");
                    ++written;
                }

                if (written > 0)
                {
                    strcat_s(text, "\n");
                    line(text);
                }
            }
        }
    }
}