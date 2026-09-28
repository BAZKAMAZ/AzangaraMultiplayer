#ifndef AZANGARA_MULTIPLAYER_TEXT_INPUT_H
#define AZANGARA_MULTIPLAYER_TEXT_INPUT_H

namespace Mp
{
    namespace TextInput
    {
        bool                                                        Load(const char* menuId, const char* scriptFile);

        typedef void (*Submit)(const char* menuId, const char* fieldId);

        void                                                        Answer(Submit submit);

        const char*                                                 Text(const char* fieldId);
        void                                                        SetText(const char* fieldId, const char* text);

        void                                                        Draw();

        void                                                        Close();
    }
}

#endif
