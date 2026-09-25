#ifndef AZANGARA_MULTIPLAYER_PROTOCOL_H
#define AZANGARA_MULTIPLAYER_PROTOCOL_H

#include <stdint.h>
#include <string.h>

namespace Mp
{
    // Checked on the first byte of everything that arrives. Bumped when any packet changes meaning.
    const uint8_t                                                   PROTOCOL_VERSION = 1;

    enum PacketType : uint8_t
    {
        PACKET_NONE = 0,
        PACKET_HELLO = 1,       // client -> host, until answered
        PACKET_WELCOME = 2,     // host -> client, in answer
        PACKET_INPUT = 3,       // client -> host, every frame
        PACKET_STATE = 4,       // host -> clients, 20-30 Hz
        PACKET_EVENT = 5,       // host -> clients, reliable
        PACKET_BYE = 6,         // either way, once
    };

    // version, type, sequence
    const int                                                       HEADER_SIZE = 4;

    // Well under any path MTU, so nothing is ever fragmented.
    const int                                                       MAX_PACKET = 1200;

    const int                                                       MAX_NAME = 24;

    inline const char*                                              PacketName(uint8_t type)
    {
        switch (type)
        {
        case PACKET_HELLO:   return "HELLO";
        case PACKET_WELCOME: return "WELCOME";
        case PACKET_INPUT:   return "INPUT";
        case PACKET_STATE:   return "STATE";
        case PACKET_EVENT:   return "EVENT";
        case PACKET_BYE:     return "BYE";
        default:             return "?";
        }
    }

    class Writer
    {
    public:
                                                                    Writer(uint8_t* buffer, int size)
            : m_buffer(buffer), m_size(size), m_at(0), m_overflow(false)
        {
        }

        void                                                        Header(uint8_t type, uint16_t sequence)
        {
            U8(PROTOCOL_VERSION);
            U8(type);
            U16(sequence);
        }

        void                                                        U8(uint8_t value)
        {
            if (!Room(1))
            {
                return;
            }

            m_buffer[m_at++] = value;
        }

        void                                                        U16(uint16_t value)
        {
            if (!Room(2))
            {
                return;
            }

            m_buffer[m_at++] = static_cast<uint8_t>(value & 0xFF);
            m_buffer[m_at++] = static_cast<uint8_t>((value >> 8) & 0xFF);
        }

        void                                                        U32(uint32_t value)
        {
            if (!Room(4))
            {
                return;
            }

            for (int shift = 0; shift < 32; shift += 8)
            {
                m_buffer[m_at++] = static_cast<uint8_t>((value >> shift) & 0xFF);
            }
        }

        void                                                        U64(uint64_t value)
        {
            U32(static_cast<uint32_t>(value & 0xFFFFFFFFu));
            U32(static_cast<uint32_t>(value >> 32));
        }

        void                                                        Float(float value)
        {
            uint32_t bits = 0;
            memcpy(&bits, &value, 4);
            U32(bits);
        }

        // Length byte then the characters. Truncated rather than refused.
        void                                                        Text(const char* text, int cap)
        {
            int length = 0;
            while (text != nullptr && text[length] != 0 && length < cap)
            {
                ++length;
            }

            U8(static_cast<uint8_t>(length));

            for (int at = 0; at < length; ++at)
            {
                U8(static_cast<uint8_t>(text[at]));
            }
        }

        int                                                         Size() const { return m_at; }

        // Checked once, after the last field, rather than per call.
        bool                                                        Overflowed() const { return m_overflow; }

    private:
        bool                                                        Room(int bytes)
        {
            if (m_at + bytes > m_size)
            {
                m_overflow = true;
                return false;
            }

            return true;
        }

        uint8_t*                                                    m_buffer;
        int                                                         m_size;
        int                                                         m_at;
        bool                                                        m_overflow;
    };

    // Reads what a Writer wrote. A read past the end yields zero and sets Bad().
    class Reader
    {
    public:
                                                                    Reader(const uint8_t* buffer, int size)
            : m_buffer(buffer), m_size(size), m_at(0), m_bad(false)
        {
        }

        // False when this is not one of ours at all: wrong version or too short.
        bool                                                        Header(uint8_t& type, uint16_t& sequence)
        {
            if (m_size < HEADER_SIZE)
            {
                m_bad = true;
                return false;
            }

            const uint8_t version = U8();
            type = U8();
            sequence = U16();

            return version == PROTOCOL_VERSION;
        }

        uint8_t                                                     U8()
        {
            if (!Room(1))
            {
                return 0;
            }

            return m_buffer[m_at++];
        }

        uint16_t                                                    U16()
        {
            if (!Room(2))
            {
                return 0;
            }

            const uint16_t low = m_buffer[m_at++];
            const uint16_t high = m_buffer[m_at++];

            return static_cast<uint16_t>(low | (high << 8));
        }

        uint32_t                                                    U32()
        {
            if (!Room(4))
            {
                return 0;
            }

            uint32_t value = 0;
            for (int shift = 0; shift < 32; shift += 8)
            {
                value |= static_cast<uint32_t>(m_buffer[m_at++]) << shift;
            }

            return value;
        }

        uint64_t                                                    U64()
        {
            const uint64_t low = U32();
            const uint64_t high = U32();

            return low | (high << 32);
        }

        float                                                       Float()
        {
            const uint32_t bits = U32();
            float value = 0.0f;
            memcpy(&value, &bits, 4);

            return value;
        }

        // Always null terminates, whatever the wire said.
        void                                                        Text(char* out, int cap)
        {
            const int length = U8();

            int kept = 0;
            for (int at = 0; at < length; ++at)
            {
                const char one = static_cast<char>(U8());
                if (kept + 1 < cap)
                {
                    out[kept++] = one;
                }
            }

            if (cap > 0)
            {
                out[kept] = 0;
            }
        }

        bool                                                        Bad() const { return m_bad; }
        int                                                         Left() const { return m_size - m_at; }

    private:
        bool                                                        Room(int bytes)
        {
            if (m_at + bytes > m_size)
            {
                m_bad = true;
                return false;
            }

            return true;
        }

        const uint8_t*                                              m_buffer;
        int                                                         m_size;
        int                                                         m_at;
        bool                                                        m_bad;
    };
}

#endif
