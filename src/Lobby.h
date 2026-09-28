#ifndef AZANGARA_MULTIPLAYER_LOBBY_H
#define AZANGARA_MULTIPLAYER_LOBBY_H

namespace Mp
{
    namespace Lobby
    {
        // One line of the rooms table. A room nobody has answered for yet keeps its -1.
        struct                                                      Room
        {
            int                                                     ping;
            int                                                     players;
            char                                                    address[24];
            char                                                    palace[20];
            char                                                    level[129];
        };

        const int                                                   MAX_ROOMS = 10;

        typedef void (*Opener)(const char* palace);
        typedef void (*Joiner)(const char* address, const char* level);

        typedef void (*Closer)();

        void                                                        Answer(Opener open, Joiner join, Closer close);

        // Any menu is up: its castle buttons are learnt; the lobby's own gets its palace and rooms.
        void                                                        Build(const char* menuId);

        bool                                                        Click(const char* itemId);

        void                                                        Rooms(const Room* rooms, int count);

        // First level of the picked palace, from its castle button; "" when none is known.
        const char*                                                 Palace();
    }
}

#endif
