#ifndef AZANGARA_MULTIPLAYER_ROOM_H
#define AZANGARA_MULTIPLAYER_ROOM_H

#include <stdint.h>

namespace Mp
{
    namespace Room
    {
        const uint8_t THING_TAG_ON = 0x01;   // a door open, a field on
        const uint8_t THING_HIDDEN = 0x02;

        // Only host-changed objects need position sync; other objects keep their own game updates.
        const uint8_t THING_MOVES = 0x04;

        // Sync the object's live state and timer instead of forcing its position every frame.
        const uint8_t THING_HAS_STATE = 0x08;

        // Sync a machine's timer when present; doors without one use direct state changes.
        const uint8_t THING_HAS_TIMER = 0x10;

        struct                                                      Thing
        {
            uint32_t                                                id;   // stable, never reused
            float                                                   position[3];
            uint8_t                                                 flags;
            int32_t                                                 state;
            float                                                   timer;
        };

        const int                                                   MAX_THINGS = 24;
        const int                                                   THING_SIZE = 25;

        int                                                         Read(Thing* out, int max);
        int                                                         Apply(const Thing* things, int count, uint32_t* wants, int maxWants);

        void                                                        Hold(bool freeze);
        void                                                        Trace(bool on);
        void                                                        Trace(bool on);
        bool                                                        Holding();

        void                                                        LetGo();

        int                                                         Held();
        void                                                        Report(void (*line)(const char* text));
    }
}

#endif
