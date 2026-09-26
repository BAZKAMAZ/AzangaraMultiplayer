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

                // Use the loader's state property, falling back to shown for disappearing platforms.
                const char* phase;
                bool hasTimer;

                // Track when phases diverge; timer-driven fields are corrected only if needed.
                DWORD sinceApart;
                bool theirs;
                bool ours;
                bool started;
                DWORD askedAt;
            };

            Known g_known[MAX_THINGS] = {};
            int g_knownCount = 0;

            bool g_trace = false;

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

                Thing& thing = out[written++];

                thing.id = ctrl.id;
                thing.position[0] = ctrl.position[0];
                thing.position[1] = ctrl.position[1];
                thing.position[2] = ctrl.position[2];
                thing.flags = static_cast<uint8_t>(ctrl.tagOn != 0 ? THING_TAG_ON : 0);
                thing.state = 0;
                thing.timer = 0.0f;

                // Read available state and timer properties once because property checks scan the entity list.
                Known* known = Find(ctrl.id);

                if (known == nullptr && g_knownCount < MAX_THINGS)
                {
                    known = &g_known[g_knownCount++];
                    *known = Known{};
                    known->id = ctrl.id;
                    known->index = at;
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
                }

                AmodCtrl here = {};

                // Reuse the cached ctrl index until it stops matching the expected entity.
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

                // Sync the machine's state instead of its position so both copies keep running their own script.
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

                // Keep the timer in sync because it controls the machine's next state change.
                if (known->hasTimer && (thing.flags & THING_HAS_TIMER) != 0 &&  Ctrls::GetProperty(index, "timer", value) && value.kind == AMOD_VALUE_FLOAT &&
                    value.count > 0)
                {
                    const float gap = value.numbers[0] - thing.timer;

                    if (gap > 0.1f || gap < -0.1f)
                    {
                        Ctrls::SetProperty(index, "timer", Value(thing.timer));
                    }
                }

                if (known->phase != nullptr && (thing.flags & THING_HAS_STATE) != 0 && Ctrls::GetProperty(index, known->phase, value) && 
                    value.kind == AMOD_VALUE_INT &&
                    value.integer != thing.state)
                {
                    // Timer-driven machines are allowed to reach the target phase naturally; doors are set directly.
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

                // Send tag changes only when the host value changes; otherwise let the object run itself.
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
