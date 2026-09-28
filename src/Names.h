#ifndef AZANGARA_MULTIPLAYER_NAMES_H
#define AZANGARA_MULTIPLAYER_NAMES_H

#include <stdint.h>

namespace Mp
{
    namespace Names
    {
        bool                                                        Open();
        void                                                        Close();

        const char*                                                 Mine();

        void                                                        Set(uint8_t id, const char* name);
        const char*                                                 Of(uint8_t id);
        void                                                        Forget();

        void                                                        Build(const char* menuId);

        bool                                                        Click(const char* itemId);
    }
}

#endif
