#ifndef AZANGARA_MULTIPLAYER_SPENT_H
#define AZANGARA_MULTIPLAYER_SPENT_H

#include <stdint.h>

namespace Mp
{
    namespace Spent
    {
        const int                                                   MAX_ROOMS = 64;
        const int                                                   MAX_PLACES = 64;

        void                                                        Watch();
        void                                                        Forget();

        int                                                         Read(uint8_t* out, int max);

        void                                                        Apply(const uint8_t* data, int size);

        bool                                                        Changed();

        void                                                        Doors(int room, void (*line)(const char* text));

        int                                                         Rooms();
        int                                                         Gone();
    }
}

#endif
