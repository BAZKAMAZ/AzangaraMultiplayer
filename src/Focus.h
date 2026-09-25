#ifndef AZANGARA_MULTIPLAYER_FOCUS_H
#define AZANGARA_MULTIPLAYER_FOCUS_H

namespace Mp
{
    namespace Focus
    {
        bool                                                        Take();
        void                                                        Release();
        bool                                                        IsOn();

        bool                                                        HasKeyboard();
    }
}

#endif
