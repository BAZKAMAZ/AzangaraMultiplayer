#ifndef AZANGARA_MULTIPLAYER_ROOM_H
#define AZANGARA_MULTIPLAYER_ROOM_H

#include <stdint.h>

namespace Mp
{
    namespace Room
    {
        const uint8_t THING_TAG_ON = 0x01;   // a door open, a field on
        const uint8_t THING_HIDDEN = 0x02;

        // Only host-changed objects need position sync; others keep their own updates.
        const uint8_t THING_MOVES = 0x04;

        // Sync live state and timer instead of forcing the object's position every frame.
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

        // Monster movers are keyed by placement, which stays the same across machines.
        const uint8_t                                               MOVER_PHASE = 0x01;

        struct                                                      Mover
        {
            uint8_t                                                 placed;
            float                                                   position[3];
            int8_t                                                  facing;

            uint8_t                                                 has;
            float                                                   phase;
        };

        const int                                                   MAX_MOVERS = 32;
        const int                                                   MOVER_SIZE = 19;

        int                                                         Read(Thing* out, int max);
        int                                                         Apply(const Thing* things, int count, uint32_t* wants, int maxWants);

        // Monster movers only exist in the room a player is currently in.
        int                                                         ReadMovers(Mover* out, int max);
        void                                                        ApplyMovers(const Mover* movers, int count);

        // A remote body must not touch the exit when its owner leaves the level.
        void                                                        ShutExit(bool shut);

        void                                                        Hold(bool freeze);
        void                                                        Trace(bool on);
        bool                                                        Holding();

        void                                                        LetGo();

        int                                                         Held();
        void                                                        Report(void (*line)(const char* text));
    }
}

#endif