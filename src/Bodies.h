#ifndef AZANGARA_MULTIPLAYER_BODIES_H
#define AZANGARA_MULTIPLAYER_BODIES_H

#include <stdint.h>

namespace Mp
{
    namespace Bodies
    {
        bool                                                        Open();
        void                                                        Close();
        bool                                                        IsOpen();

        void*                                                       Create(const float position[3]);
        void                                                        Destroy(void* body);

        // 0 when it went where it was sent, 1..4 for the side that stopped it.
        int                                                         Step(void* body, int dirX, int dirY, int jump, float delta);

        void                                                        Read(void* body, float position[3], int& state, int& subState);
        void                                                        Place(void* body, const float position[3]);

        // Why Open refused, for the console.
        const char*                                                 Trouble();
    }
}

#endif
