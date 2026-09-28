#ifndef AZANGARA_MULTIPLAYER_AVATARS_H
#define AZANGARA_MULTIPLAYER_AVATARS_H

#include "Protocol.h"

namespace Mp
{
    namespace Avatars
    {
        struct                                                      Drawn
        {
            int                                                     queued;
            int                                                     elsewhere;   // another room
            int                                                     refused;     // the scene list said no
            bool                                                    noModel;
            bool                                                    noView;      // no camera to place them by
        };

        bool                                                        ReadLocal(Body& out);

        void                                                        Show(const Body* bodies, int count, uint16_t room);
        Drawn                                                       Tally();

        // Below zero the body's own frame is used; anything else freezes every avatar there.
        void                                                        HoldFrame(float frame);
        float                                                       HeldFrame();

        // The bodies of the last Show, for whatever draws beside them.
        int                                                         ShownCount();
        const Body*                                                 ShownAt(int index);
        uint16_t                                                    ShownRoom();

        bool                                                        Project(const float world[3], float& x, float& y);

        void                                                        Forget();
        void                                                        Report(void (*line)(const char* text));
    }
}

#endif
