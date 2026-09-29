#ifndef AZANGARA_MULTIPLAYER_MENUS_H
#define AZANGARA_MULTIPLAYER_MENUS_H

namespace Mp
{
    namespace Menus
    {
        bool                                                        Open();
        void                                                        Close();
        void                                                        Block(bool block);
        bool                                                        Blocked();

        bool                                                        Show();
        bool                                                        Ready();

        bool                                                        SetStatic(const char* menuId, const char* staticId, const char* text);

        void                                                        Notice(const char* text, bool afterLevel);

        void                                                        Tick();
    }
}

#endif
