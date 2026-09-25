#ifndef AZANGARA_MULTIPLAYER_BACKGROUND_H
#define AZANGARA_MULTIPLAYER_BACKGROUND_H

namespace Mp
{
    namespace Background
    {
        bool                                                        Take();

        void                                                        Release();

        bool                                                        IsOn();

        bool                                                        KeepsPlaying();
    }
}

#endif
