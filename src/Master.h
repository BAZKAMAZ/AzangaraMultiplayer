#ifndef AZANGARA_MULTIPLAYER_MASTER_H
#define AZANGARA_MULTIPLAYER_MASTER_H

#include <stdint.h>

namespace Mp
{
    namespace Master
    {
        struct                                                      Entry
        {
            char                                                    address[24];
            int                                                     players;
            char                                                    palace[21];
            char                                                    level[129];
            char                                                    name[25];
        };

        const int                                                   MAX_ENTRIES = 16;

        bool                                                        Open(const char* url);
        void                                                        Close();

        void                                                        Announce(uint16_t port, const char* name, const char* palace, const char* level, int players);
        void                                                        Withdraw(uint16_t port);

        void                                                        AskList();
        int                                                         Take(Entry* out, int capacity);
    }
}

#endif
