#ifndef AZANGARA_MULTIPLAYER_PROTOCOL_H
#define AZANGARA_MULTIPLAYER_PROTOCOL_H

#include <stdint.h>
#include <string.h>

namespace Mp
{
    const uint8_t                                                   PROTOCOL_VERSION = 14;

    enum PacketType : uint8_t
    {
        PACKET_NONE = 0,
        PACKET_HELLO = 1,       // client -> host, until answered
        PACKET_WELCOME = 2,     // host -> client, in answer
        PACKET_INPUT = 3,       // client -> host, what he is pressing
        PACKET_STATE = 4,       // host -> clients, 20-30 Hz, and who runs which room
        PACKET_EVENT = 5,       // host -> clients, reliable
        PACKET_BYE = 6,         // either way, once
        PACKET_MOVE = 7,        // client -> host, where he is. INPUT took over at stage 2.
        PACKET_WORLD = 8,       // whoever runs the room -> everybody else in it
        PACKET_ASK = 9,         // client -> host, open this one for me too
        PACKET_SPENT = 10,      // host -> clients, what the level has already given up
    };

    // A dead body is still drawn; the game plays its death out.
    const uint8_t                                                   BODY_DEAD = 0x01;

    // One player as the wire carries him.
    struct                                                          Body
    {
        uint8_t                                                     id;

        uint32_t                                                    level;

        uint16_t                                                    room;
        float                                                       position[3];
        float                                                       facing;

        uint16_t                                                    frameA;
        uint16_t                                                    frameB;
        uint8_t                                                     blend;      // 0..255, A to B
        uint8_t                                                     state;
        uint8_t                                                     subState;
        uint8_t                                                     flags;
    };

    const int                                                       BODY_SIZE = 31;

    const uint8_t                                                   INPUT_JUMP = 0x01;

    struct                                                          Input
    {
        uint32_t                                                    tick;
        int8_t                                                      dirX;
        int8_t                                                      dirY;
        uint8_t                                                     buttons;

        float                                                       facing;

        // His own animation, passed on rather than worked out again.
        uint16_t                                                    frameA;
        uint16_t                                                    frameB;
        uint8_t                                                     blend;
    };

    const int                                                       INPUT_SIZE = 16;

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

    inline void                                                     WriteBody(Writer& writer, const Body& body)
    {
        writer.U8(body.id);
        writer.U32(body.level);
        writer.U16(body.room);
        writer.Float(body.position[0]);
        writer.Float(body.position[1]);
        writer.Float(body.position[2]);
        writer.Float(body.facing);
        writer.U16(body.frameA);
        writer.U16(body.frameB);
        writer.U8(body.blend);
        writer.U8(body.state);
        writer.U8(body.subState);
        writer.U8(body.flags);
    }

    inline void                                                     WriteInput(Writer& writer, const Input& input)
    {
        writer.U32(input.tick);
        writer.U8(static_cast<uint8_t>(input.dirX));
        writer.U8(static_cast<uint8_t>(input.dirY));
        writer.U8(input.buttons);
        writer.Float(input.facing);
        writer.U16(input.frameA);
        writer.U16(input.frameB);
        writer.U8(input.blend);
    }

    inline void                                                     ReadInput(Reader& reader, Input& input)
    {
        input.tick = reader.U32();
        input.dirX = static_cast<int8_t>(reader.U8());
        input.dirY = static_cast<int8_t>(reader.U8());
        input.buttons = reader.U8();
        input.facing = reader.Float();
        input.frameA = reader.U16();
        input.frameB = reader.U16();
        input.blend = reader.U8();
    }

    inline void                                                     ReadBody(Reader& reader, Body& body)
    {
        body.id = reader.U8();
        body.level = reader.U32();
        body.room = reader.U16();
        body.position[0] = reader.Float();
        body.position[1] = reader.Float();
        body.position[2] = reader.Float();
        body.facing = reader.Float();
        body.frameA = reader.U16();
        body.frameB = reader.U16();
        body.blend = reader.U8();
        body.state = reader.U8();
        body.subState = reader.U8();
        body.flags = reader.U8();
    }
}

#endif
