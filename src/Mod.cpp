#include <ForgeApi.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Avatars.h"
#include "Background.h"
#include "Bodies.h"
#include "Focus.h"
#include "Net.h"
#include "Room.h"
#include "Spent.h"
#include "Protocol.h"

using namespace Amod;

namespace
{
    struct Settings
    {
        char peer[128];     // mp_peer: address used for hello packets
        int port;           // mp_port: first local port
        int slots;          // mp_slots: number of ports in the range
        float rate;         // mp_rate: hellos per second
        float tick;         // mp_tick: body updates per second
        int host;           // mp_host: 1 host, 0 client, -1 first port decides
        float stale;        // mp_stale: seconds a body stays drawable
        float delay;        // mp_delay: interpolation delay
        float anchor;       // mp_anchor: client position update rate
        float snap;         // mp_snap: maximum position drift before correction
        float pull;         // mp_pull: host correction strength
        float hard;         // mp_hard: distance where correction becomes an instant move
        int lag;            // mp_lag: intentional packet delay in milliseconds
        int world;          // mp_world: room objects are controlled by the host
        float worldRate;    // mp_wrate: room state update rate
        int hold;           // mp_hold: client removes room objects from its game
        int touch;          // mp_touch: stepped bodies interact with room objects
        int background;     // mp_background: keep running when unfocused
        int focus;          // mp_focus: capture keyboard only while focused
        int loss;           // mp_loss: percentage of outgoing packets dropped
        bool chatter;       // mp_log: log one line per packet
    };

    Settings g_settings =
    {
        "127.0.0.1",
        27015,
        4,
        1.0f,
        30.0f,
        -1,
        5.0f,
        0.10f,
        4.0f,
        1.2f,
        0.0f,
        0.60f,
        0,
        1,
        5.0f,
        0,
        1,
        1,
        1,
        0,
        true,
    };

    const int MAX_PEERS = 8;

    struct Peer
    {
        Mp::Address address;
        char name[Mp::MAX_NAME + 1];
        double heardAt;     // local clock time of the last packet
        int packets;
        bool greeted;       // peer has answered at least once
        uint8_t id;         // host-assigned player id, 0 until assigned
    };

    Mp::Socket g_socket;
    Mp::Address g_targets[MAX_PEERS];    // hello destinations
    int g_targetCount = 0;

    Peer g_peers[MAX_PEERS];             // known peers
    int g_peerCount = 0;

    char g_name[Mp::MAX_NAME + 1] = "player";

    bool g_running = false;
    uint16_t g_sequence = 0;

    // The host is player 0 and assigns the remaining ids.
    bool g_isHost = true;

    uint8_t g_myId = 0;
    Mp::Address g_host = {};

    // Bodies are indexed by player id; host uses MOVE and clients use STATE.
    Mp::Body g_bodies[MAX_PEERS + 1] = {};
    double g_bodyAt[MAX_PEERS + 1] = {};

    Mp::Body g_before[MAX_PEERS + 1] = {};
    double g_beforeAt[MAX_PEERS + 1] = {};

    // Marks bodies simulated locally instead of from network snapshots.
    bool g_ours[MAX_PEERS + 1] = {};

    // Host-side remote bodies and their latest inputs.
    void* g_avatar[MAX_PEERS + 1] = {};
    Mp::Input g_pressed[MAX_PEERS + 1] = {};
    double g_pressedAt[MAX_PEERS + 1] = {};

    // Track how long host and client positions have disagreed.
    double g_apartAt[MAX_PEERS + 1] = {};
    const double APART_PATIENCE = 1.5;
    float g_avatarFacing[MAX_PEERS + 1] = {};

    Mp::Body g_anchored[MAX_PEERS + 1] = {};
    double g_anchoredAt[MAX_PEERS + 1] = {};

    // A room is owned by the player who entered it first and is still inside.
    const int MAX_ROOMS = 64;
    const uint8_t NOBODY = 0xFF;

    uint8_t g_owner[MAX_ROOMS] = {};
    uint16_t g_wasIn[MAX_PEERS + 1] = {};
    double g_enteredAt[MAX_PEERS + 1] = {};

    void ResetOwners()
    {
        for (int room = 0; room < MAX_ROOMS; ++room)
        {
            g_owner[room] = NOBODY;
        }

        for (int id = 0; id <= MAX_PEERS; ++id)
        {
            g_enteredAt[id] = 0.0;
            g_wasIn[id] = 0;
        }
    }

    const double ANCHOR_FRESH = 1.5;

    bool IRun(uint16_t room)
    {
        return room >= MAX_ROOMS || g_owner[room] == NOBODY || g_owner[room] == g_myId;
    }

    // Only the host's settled ownership decision lets this copy publish room state.
    bool IOwn(uint16_t room)
    {
        return room < MAX_ROOMS && g_owner[room] == g_myId && (g_isHost || g_myId != 0);
    }

    float g_sinceAnchor = 0.0f;
    float g_sinceWorld = 0.0f;
    float g_sinceSpent = 0.0f;
    float g_sinceLook = 0.0f;

    // Host's latest position for the local player.
    Mp::Body g_saidOfMe = {};
    double g_saidOfMeAt = 0.0;
    uint16_t g_lastState = 0;
    bool g_hadState = false;

    uint64_t g_pulled = 0;
    uint64_t g_moved = 0;

    // Track position divergence and the last non-reproducible action.
    double g_apartSince = 0.0;
    double g_notWalkingAt = 0.0;

    // Position divergence must persist this long before correction.
    const double PATIENCE = 0.5;

    // Grace period after climbing before position correction.
    const double GRACE = 1.0;

    // Delayed packets used to simulate network latency.
    struct Held
    {
        Mp::Address from;
        double due;
        int size;
        uint8_t data[Mp::MAX_PACKET];
    };

    const int MAX_HELD = 64;

    Held g_held[MAX_HELD] = {};
    int g_heldCount = 0;

    // Clients silent this long are considered inactive.
    const double INPUT_FRESH = 0.5;

    // Input captured from this player's controls.
    Mp::Input g_mine = {};
    uint32_t g_myTick = 0;

    int g_jumpKeys[8] = {};
    int g_jumpKeyCount = -1;

    const double PEER_TIMEOUT = 5.0;

    float g_sinceTick = 0.0f;
    Mp::Avatars::Drawn g_drawn = {};
    uint16_t g_myRoom = 0;


    uint64_t g_sent = 0;
    uint64_t g_received = 0;
    uint64_t g_ignored = 0;      // received but invalid or irrelevant
    uint64_t g_dropped = 0;      // intentionally dropped outgoing packets
    uint64_t g_renderFrames = 0; // frames that pumped the network
    uint64_t g_worldFrames = 0;  // frames that advanced the level
    int g_lastRtt = -1;          // RTT in milliseconds, -1 until WELCOME

    float g_sinceHello = 0.0f;

    AmodToken g_frameToken = AMOD_INVALID_TOKEN;

    // Local high-resolution clock.

    LARGE_INTEGER g_frequency = {};
    LARGE_INTEGER g_start = {};

    void ClockStart()
    {
        QueryPerformanceFrequency(&g_frequency);
        QueryPerformanceCounter(&g_start);
    }

    double Now()
    {
        if (g_frequency.QuadPart == 0)
        {
            return 0.0;
        }

        LARGE_INTEGER at = {};
        QueryPerformanceCounter(&at);

        return static_cast<double>(at.QuadPart - g_start.QuadPart) /
            static_cast<double>(g_frequency.QuadPart);
    }

    uint32_t NowMs()
    {
        return static_cast<uint32_t>(Now() * 1000.0);
    }

    // Peer management.

    uint8_t FreeId()
    {
        for (uint8_t id = 1; id <= MAX_PEERS; ++id)
        {
            bool taken = false;

            for (int at = 0; at < g_peerCount && !taken; ++at)
            {
                taken = g_peers[at].id == id;
            }

            if (!taken)
            {
                return id;
            }
        }

        return 0;
    }

    Peer* FindPeer(const Mp::Address& address)
    {
        for (int at = 0; at < g_peerCount; ++at)
        {
            if (g_peers[at].address == address)
            {
                return &g_peers[at];
            }
        }

        return nullptr;
    }

    Peer* RememberPeer(const Mp::Address& address, const char* name)
    {
        Peer* peer = FindPeer(address);

        if (peer == nullptr)
        {
            if (g_peerCount >= MAX_PEERS)
            {
                return nullptr;
            }

            peer = &g_peers[g_peerCount++];
            *peer = Peer{};
            peer->address = address;

            Log(AMOD_LOG_INFO, "a peer at %s", Mp::ToText(address));
            Console::Printf("mp: peer %s\n", Mp::ToText(address));
        }

        if (name != nullptr && name[0] != 0)
        {
            strcpy_s(peer->name, sizeof(peer->name), name);
        }

        peer->heardAt = Now();
        ++peer->packets;
        return peer;
    }

    void FindJumpKeys()
    {
        g_jumpKeyCount = 0;

        const int count = Input::KeyCount();

        for (int key = 0; key < count && g_jumpKeyCount < 8; ++key)
        {
            AmodKeyInfo info = {};

            if (Input::GetKey(key, &info) && info.target != nullptr &&
                _stricmp(info.target, "jump") == 0)
            {
                g_jumpKeys[g_jumpKeyCount++] = key;
            }
        }
    }

    bool JumpHeld()
    {
        if (g_jumpKeyCount < 0)
        {
            FindJumpKeys();
        }

        for (int at = 0; at < g_jumpKeyCount; ++at)
        {
            if (Input::IsDown(g_jumpKeys[at]))
            {
                return true;
            }
        }

        return false;
    }

    void __cdecl OnMove(AmodPlayerMove* move, void*)
    {
        if (move == nullptr)
        {
            return;
        }

        g_mine.dirX = static_cast<int8_t>(move->dirX);
        g_mine.dirY = static_cast<int8_t>(move->dirY);
    }

    // Packet sending.

    bool ShouldLose()
    {
        return g_settings.loss > 0 && (rand() % 100) < g_settings.loss;
    }

    void SendTo(const Mp::Address& to, const uint8_t* data, int size)
    {
        if (ShouldLose())
        {
            ++g_dropped;
            return;
        }

        if (g_socket.Send(to, data, size) < 0)
        {
            Log(AMOD_LOG_WARNING, "sending to %s failed with %d", Mp::ToText(to), g_socket.LastError());
            return;
        }

        ++g_sent;
    }

    void SendHello(const Mp::Address& to)
    {
        uint8_t buffer[Mp::MAX_PACKET];
        Mp::Writer writer(buffer, sizeof(buffer));

        writer.Header(Mp::PACKET_HELLO, g_sequence++);
        writer.Text(g_name, Mp::MAX_NAME);
        writer.U32(NowMs());        // echoed by WELCOME for RTT measurement

        if (writer.Overflowed())
        {
            return;
        }

        SendTo(to, buffer, writer.Size());
    }

    void SendWelcome(const Mp::Address& to, uint32_t stamp, uint8_t id)
    {
        uint8_t buffer[Mp::MAX_PACKET];
        Mp::Writer writer(buffer, sizeof(buffer));

        writer.Header(Mp::PACKET_WELCOME, g_sequence++);
        writer.Text(g_name, Mp::MAX_NAME);
        writer.U32(stamp);          // client's timestamp, used to measure its RTT
        writer.U8(id);

        if (writer.Overflowed())
        {
            return;
        }

        SendTo(to, buffer, writer.Size());
    }

    void SendBye()
    {
        uint8_t buffer[Mp::MAX_PACKET];
        Mp::Writer writer(buffer, sizeof(buffer));

        writer.Header(Mp::PACKET_BYE, g_sequence++);
        writer.U8(0);               // reserved reason code

        for (int at = 0; at < g_peerCount; ++at)
        {
            g_socket.Send(g_peers[at].address, buffer, writer.Size());
        }
    }

    void RememberBody(const Mp::Body& body, uint8_t id, bool ours = false)
    {
        if (id > MAX_PEERS)
        {
            return;
        }

        g_ours[id] = ours;

        g_before[id] = g_bodies[id];
        g_beforeAt[id] = g_bodyAt[id];

        g_bodies[id] = body;
        g_bodies[id].id = id;
        g_bodyAt[id] = Now();
    }

    Mp::Body Smoothed(uint8_t id, double now)
    {
        const Mp::Body& latest = g_bodies[id];
        const Mp::Body& was = g_before[id];

        const double span = g_bodyAt[id] - g_beforeAt[id];
        const double when = now - g_settings.delay;

        // Locally simulated bodies do not need snapshot interpolation.
        if (g_ours[id])
        {
            return latest;
        }

        if (g_beforeAt[id] <= 0.0 || span <= 0.0 || when >= g_bodyAt[id])
        {
            return latest;
        }

        if (when <= g_beforeAt[id])
        {
            return was;
        }

        const float part = static_cast<float>((when - g_beforeAt[id]) / span);

        Mp::Body body = latest;

        for (int axis = 0; axis < 3; ++axis)
        {
            body.position[axis] = was.position[axis] +
                (latest.position[axis] - was.position[axis]) * part;
        }

        // Interpolate facing using the shortest rotation path.
        float turn = latest.facing - was.facing;
        while (turn > 3.14159265f)
        {
            turn -= 6.28318531f;
        }
        while (turn < -3.14159265f)
        {
            turn += 6.28318531f;
        }
        body.facing = was.facing + turn * part;

        return body;
    }

    bool BodyIsLive(uint8_t id, double now)
    {
        return g_bodyAt[id] > 0.0 && (now - g_bodyAt[id]) < g_settings.stale;
    }

    void SettleRooms(const Mp::Body& mine, bool here)
    {
        const double now = Now();
        uint16_t where[MAX_PEERS + 1] = {};
        bool playing[MAX_PEERS + 1] = {};

        for (int id = 0; id <= MAX_PEERS; ++id)
        {
            const uint8_t who = static_cast<uint8_t>(id);

            // Use the client's anchor to determine where remote players currently stand.
            playing[id] = (who == g_myId)
                ? here
                : (g_anchoredAt[id] > 0.0 && (now - g_anchoredAt[id]) < ANCHOR_FRESH);
            where[id] = (who == g_myId) ? mine.room : g_anchored[id].room;

            if (!playing[id])
            {
                g_enteredAt[id] = 0.0;
                continue;
            }

            if (g_enteredAt[id] == 0.0 || g_wasIn[id] != where[id])
            {
                g_wasIn[id] = where[id];
                g_enteredAt[id] = now;
            }
        }

        for (int room = 0; room < MAX_ROOMS; ++room)
        {
            const uint8_t owner = g_owner[room];

            if (owner <= MAX_PEERS && playing[owner] && where[owner] == room)
            {
                continue;
            }

            uint8_t first = NOBODY;

            for (int id = 0; id <= MAX_PEERS; ++id)
            {
                if (playing[id] && where[id] == room &&
                    (first == NOBODY || g_enteredAt[id] < g_enteredAt[first]))
                {
                    first = static_cast<uint8_t>(id);
                }
            }

            g_owner[room] = first;
        }
    }

    void SendAnchor(const Mp::Body& body)
    {
        if (!g_host.IsSet())
        {
            return;
        }

        uint8_t buffer[Mp::MAX_PACKET];
        Mp::Writer writer(buffer, sizeof(buffer));

        writer.Header(Mp::PACKET_MOVE, g_sequence++);
        Mp::WriteBody(writer, body);

        if (!writer.Overflowed())
        {
            SendTo(g_host, buffer, writer.Size());
        }
    }

    void SendInput()
    {
        if (!g_host.IsSet())
        {
            return;
        }

        uint8_t buffer[Mp::MAX_PACKET];
        Mp::Writer writer(buffer, sizeof(buffer));

        writer.Header(Mp::PACKET_INPUT, g_sequence++);
        Mp::WriteInput(writer, g_mine);

        if (!writer.Overflowed())
        {
            SendTo(g_host, buffer, writer.Size());
        }
    }

    void SendAsk(const uint32_t* ids, int count)
    {
        if (!g_host.IsSet() || ids == nullptr || count <= 0)
        {
            return;
        }

        uint8_t buffer[Mp::MAX_PACKET];
        Mp::Writer writer(buffer, sizeof(buffer));

        writer.Header(Mp::PACKET_ASK, g_sequence++);
        writer.U8(static_cast<uint8_t>(count));

        for (int at = 0; at < count; ++at)
        {
            writer.U32(ids[at]);
        }

        if (!writer.Overflowed())
        {
            SendTo(g_host, buffer, writer.Size());
        }
    }

    // Host and clients exchange the persistent spent state of bonuses, monsters and doors.
    void SendSpent()
    {
        uint8_t what[Mp::MAX_PACKET];
        const int size = Mp::Spent::Read(what, sizeof(what) - Mp::HEADER_SIZE - 2);

        if (size <= 0)
        {
            return;
        }

        uint8_t buffer[Mp::MAX_PACKET];
        Mp::Writer writer(buffer, sizeof(buffer));

        writer.Header(Mp::PACKET_SPENT, g_sequence++);
        writer.U16(static_cast<uint16_t>(size));

        for (int at = 0; at < size; ++at)
        {
            writer.U8(what[at]);
        }

        if (writer.Overflowed())
        {
            return;
        }

        if (g_isHost)
        {
            for (int at = 0; at < g_peerCount; ++at)
            {
                if (g_peers[at].id != 0)
                {
                    SendTo(g_peers[at].address, buffer, writer.Size());
                }
            }
        }
        else if (g_host.IsSet())
        {
            SendTo(g_host, buffer, writer.Size());
        }
    }

    // Send the current room's machines and monsters from whoever owns that room.
    void SendWorld(uint16_t room)
    {
        Mp::Room::Thing things[Mp::Room::MAX_THINGS];
        const int count = Mp::Room::Read(things, Mp::Room::MAX_THINGS);

        Mp::Room::Mover movers[Mp::Room::MAX_MOVERS];
        const int walking = Mp::Room::ReadMovers(movers, Mp::Room::MAX_MOVERS);

        if ((count == 0 && walking == 0) || g_settings.world == 0)
        {
            return;
        }

        uint8_t buffer[Mp::MAX_PACKET];
        Mp::Writer writer(buffer, sizeof(buffer));

        writer.Header(Mp::PACKET_WORLD, g_sequence++);
        writer.U16(room);
        writer.U8(static_cast<uint8_t>(count));

        for (int at = 0; at < count; ++at)
        {
            writer.U32(things[at].id);
            writer.Float(things[at].position[0]);
            writer.Float(things[at].position[1]);
            writer.Float(things[at].position[2]);
            writer.U8(things[at].flags);
            writer.U32(static_cast<uint32_t>(things[at].state));
            writer.Float(things[at].timer);
        }

        // Append room-local monster movers.
        writer.U8(static_cast<uint8_t>(walking));

        for (int at = 0; at < walking; ++at)
        {
            writer.U8(movers[at].placed);
            writer.Float(movers[at].position[0]);
            writer.Float(movers[at].position[1]);
            writer.Float(movers[at].position[2]);
            writer.U8(static_cast<uint8_t>(movers[at].facing));
            writer.U8(movers[at].has);
            writer.Float(movers[at].phase);
        }

        if (writer.Overflowed())
        {
            return;
        }

        if (!g_isHost)
        {
            if (g_host.IsSet())
            {
                SendTo(g_host, buffer, writer.Size());
            }

            return;
        }

        for (int at = 0; at < g_peerCount; ++at)
        {
            if (g_peers[at].id != 0)
            {
                SendTo(g_peers[at].address, buffer, writer.Size());
            }
        }
    }

    // Host to clients: broadcast all live player bodies.
    void SendState()
    {
        uint8_t buffer[Mp::MAX_PACKET];
        Mp::Writer writer(buffer, sizeof(buffer));

        const double now = Now();
        int count = 0;

        writer.Header(Mp::PACKET_STATE, g_sequence++);
        writer.U32(NowMs());

        // Count live bodies before writing them.
        for (int id = 0; id <= MAX_PEERS; ++id)
        {
            if (BodyIsLive(static_cast<uint8_t>(id), now))
            {
                ++count;
            }
        }

        writer.U8(static_cast<uint8_t>(count));

        for (int id = 0; id <= MAX_PEERS; ++id)
        {
            if (BodyIsLive(static_cast<uint8_t>(id), now))
            {
                Mp::WriteBody(writer, g_bodies[id]);
            }
        }

        // Send ownership for every room currently occupied.
        uint16_t said[MAX_PEERS + 1] = {};
        int rooms = 0;

        for (int id = 0; id <= MAX_PEERS; ++id)
        {
            const uint16_t room = g_wasIn[id];
            bool already = false;

            if (g_enteredAt[id] == 0.0 || room >= MAX_ROOMS || g_owner[room] == NOBODY)
            {
                continue;
            }

            for (int at = 0; at < rooms; ++at)
            {
                already = already || said[at] == room;
            }

            if (!already)
            {
                said[rooms++] = room;
            }
        }

        writer.U8(static_cast<uint8_t>(rooms));

        for (int at = 0; at < rooms; ++at)
        {
            writer.U16(said[at]);
            writer.U8(g_owner[said[at]]);
        }

        if (writer.Overflowed())
        {
            return;
        }

        for (int at = 0; at < g_peerCount; ++at)
        {
            if (g_peers[at].id != 0)
            {
                SendTo(g_peers[at].address, buffer, writer.Size());
            }
        }
    }

    // Packet receiving.

    void ForgetPeer(const Mp::Address& address);

    void OnPacket(const Mp::Address& from, const uint8_t* data, int size)
    {
        Mp::Reader reader(data, size);

        uint8_t type = 0;
        uint16_t sequence = 0;

        if (!reader.Header(type, sequence))
        {
            ++g_ignored;
            return;
        }

        ++g_received;

        switch (type)
        {
        case Mp::PACKET_HELLO:
        {
            char name[Mp::MAX_NAME + 1] = {};
            reader.Text(name, sizeof(name));
            const uint32_t stamp = reader.U32();

            if (reader.Bad())
            {
                ++g_ignored;
                return;
            }

            Peer* const peer = RememberPeer(from, name);

            if (g_settings.chatter)
            {
                //Console::Printf("mp: HELLO #%u from %s (%s)\n", sequence, Mp::ToText(from), name);
            }

            uint8_t id = 0;

            if (peer != nullptr && g_isHost)
            {
                if (peer->id == 0)
                {
                    peer->id = FreeId();
                    Console::Printf("mp: %s is player %u\n", Mp::ToText(from), peer->id);
                    Log(AMOD_LOG_INFO, "%s is player %u", Mp::ToText(from), peer->id);
                }

                id = peer->id;
            }

            SendWelcome(from, stamp, id);
            break;
        }

        case Mp::PACKET_WELCOME:
        {
            char name[Mp::MAX_NAME + 1] = {};
            reader.Text(name, sizeof(name));
            const uint32_t stamp = reader.U32();
            const uint8_t id = reader.U8();

            if (reader.Bad())
            {
                ++g_ignored;
                return;
            }

            Peer* const peer = RememberPeer(from, name);
            g_lastRtt = static_cast<int>(NowMs() - stamp);

            // Only the host assigns ids; this is how a client learns its own id.
            if (id != 0 && !g_isHost)
            {
                if (g_myId != id || !(g_host == from))
                {
                    Console::Printf("mp: host %s calls me player %u\n", Mp::ToText(from), id);
                    Log(AMOD_LOG_INFO, "host %s calls me player %u", Mp::ToText(from), id);
                }

                g_myId = id;
                g_host = from;
            }

            if (peer != nullptr && !peer->greeted)
            {
                peer->greeted = true;
                Log(AMOD_LOG_INFO, "%s answered as '%s', id %u, %d ms round trip",
                    Mp::ToText(from), name, static_cast<unsigned>(id), g_lastRtt);
            }

            if (g_settings.chatter)
            {
                //Console::Printf("mp: WELCOME #%u from %s (%s), id %u, %d ms\n", sequence, Mp::ToText(from), name, static_cast<unsigned>(id), g_lastRtt);
            }
            break;
        }

        case Mp::PACKET_INPUT:
        {
            Mp::Input input = {};
            Mp::ReadInput(reader, input);

            if (reader.Bad() || !g_isHost)
            {
                ++g_ignored;
                return;
            }

            const Peer* const peer = FindPeer(from);
            if (peer == nullptr || peer->id == 0)
            {
                ++g_ignored;
                return;
            }

            // Ignore older input packets that arrive after newer ones.
            if (g_pressed[peer->id].tick == 0 || input.tick >= g_pressed[peer->id].tick)
            {
                g_pressed[peer->id] = input;
                g_pressedAt[peer->id] = Now();
            }
            break;
        }

        case Mp::PACKET_MOVE:
        {
            Mp::Body body = {};
            Mp::ReadBody(reader, body);

            if (reader.Bad() || !g_isHost)
            {
                ++g_ignored;
                return;
            }

            const Peer* const peer = FindPeer(from);
            if (peer == nullptr || peer->id == 0)
            {
                ++g_ignored;
                return;
            }

            g_anchored[peer->id] = body;
            g_anchoredAt[peer->id] = Now();
            break;
        }

        case Mp::PACKET_STATE:
        {
            const uint32_t sent = reader.U32();
            const int count = reader.U8();

            // The host owns STATE generation, so it ignores incoming STATE packets.
            if (reader.Bad() || g_isHost)
            {
                ++g_ignored;
                return;
            }

            (void)sent;

            // Reject snapshots older than the latest accepted sequence.
            if (g_hadState && static_cast<int16_t>(sequence - g_lastState) <= 0)
            {
                ++g_ignored;
                return;
            }

            g_lastState = sequence;
            g_hadState = true;

            for (int at = 0; at < count; ++at)
            {
                Mp::Body body = {};
                Mp::ReadBody(reader, body);

                if (reader.Bad())
                {
                    ++g_ignored;
                    return;
                }

                if (body.id == g_myId)
                {
                    // Keep the host copy of the local player for reconciliation only.
                    g_saidOfMe = body;
                    g_saidOfMeAt = Now();
                }
                else
                {
                    RememberBody(body, body.id);
                }
            }

            // Replace local room ownership with the host's latest ownership table.
            {
                const int rooms = reader.U8();

                if (reader.Bad() || rooms > MAX_PEERS + 1)
                {
                    ++g_ignored;
                    return;
                }

                uint16_t which[MAX_PEERS + 1] = {};
                uint8_t owner[MAX_PEERS + 1] = {};

                for (int at = 0; at < rooms; ++at)
                {
                    which[at] = reader.U16();
                    owner[at] = reader.U8();
                }

                if (reader.Bad())
                {
                    ++g_ignored;
                    return;
                }

                for (int room = 0; room < MAX_ROOMS; ++room)
                {
                    g_owner[room] = NOBODY;
                }

                for (int at = 0; at < rooms; ++at)
                {
                    if (which[at] < MAX_ROOMS)
                    {
                        g_owner[which[at]] = owner[at];
                    }
                }
            }
            break;
        }

        case Mp::PACKET_WORLD:
        {
            const uint16_t room = reader.U16();
            const int count = reader.U8();

            if (reader.Bad() || count > Mp::Room::MAX_THINGS)
            {
                ++g_ignored;
                return;
            }

            // The host validates the sender's room ownership before relaying the packet.
            if (g_isHost)
            {
                const Peer* const said = FindPeer(from);

                if (said == nullptr || said->id == 0 || room >= MAX_ROOMS ||
                    g_owner[room] != said->id)
                {
                    ++g_ignored;
                    return;
                }

                for (int at = 0; at < g_peerCount; ++at)
                {
                    if (g_peers[at].id != 0 && !(g_peers[at].address == from))
                    {
                        SendTo(g_peers[at].address, data, size);
                    }
                }
            }

            Mp::Room::Thing things[Mp::Room::MAX_THINGS];

            for (int at = 0; at < count; ++at)
            {
                things[at].id = reader.U32();
                things[at].position[0] = reader.Float();
                things[at].position[1] = reader.Float();
                things[at].position[2] = reader.Float();
                things[at].flags = reader.U8();
                things[at].state = static_cast<int32_t>(reader.U32());
                things[at].timer = reader.Float();
            }

            const int walking = reader.U8();

            if (reader.Bad() || walking > Mp::Room::MAX_MOVERS)
            {
                ++g_ignored;
                return;
            }

            Mp::Room::Mover movers[Mp::Room::MAX_MOVERS];

            for (int at = 0; at < walking; ++at)
            {
                movers[at].placed = reader.U8();
                movers[at].position[0] = reader.Float();
                movers[at].position[1] = reader.Float();
                movers[at].position[2] = reader.Float();
                movers[at].facing = static_cast<int8_t>(reader.U8());
                movers[at].has = reader.U8();
                movers[at].phase = reader.Float();
            }

            if (reader.Bad())
            {
                ++g_ignored;
                return;
            }

            // Apply room state only for the current room and only when another player owns it.
            if (room != g_myRoom || g_settings.world == 0 || IRun(room))
            {
                break;
            }

            Mp::Room::ApplyMovers(movers, walking);

            uint32_t wants[Mp::Room::MAX_THINGS];
            const int wanted = Mp::Room::Apply(things, count, wants, Mp::Room::MAX_THINGS);

            if (wanted > 0)
            {
                SendAsk(wants, wanted);
            }
            break;
        }

        case Mp::PACKET_SPENT:
        {
            const int held = reader.U16();

            // Spent objects only disappear, so packets can be applied in any order.
            if (reader.Bad() || held <= 0 || held > reader.Left())
            {
                ++g_ignored;
                return;
            }

            uint8_t what[Mp::MAX_PACKET];

            for (int at = 0; at < held; ++at)
            {
                what[at] = reader.U8();
            }

            if (reader.Bad())
            {
                ++g_ignored;
                return;
            }

            Mp::Spent::Apply(what, held);
            break;
        }

        case Mp::PACKET_ASK:
        {
            const int count = reader.U8();

            if (reader.Bad() || !g_isHost || count > Mp::Room::MAX_THINGS)
            {
                ++g_ignored;
                return;
            }

            for (int at = 0; at < count; ++at)
            {
                const uint32_t id = reader.U32();
                const int index = Ctrls::IndexOf(id);

                if (!reader.Bad() && index >= 0)
                {
                    // Host applies the requested tag change; the next WORLD packet propagates it.
                    Ctrls::SetTagOn(index, true);
                }
            }
            break;
        }

        case Mp::PACKET_BYE:
        {
            Console::Printf("mp: BYE from %s\n", Mp::ToText(from));
            ForgetPeer(from);
            break;
        }

        default:
        {
            // INPUT, STATE and EVENT are handled elsewhere and only counted here.
            if (g_settings.chatter)
            {
                Console::Printf("mp: %s #%u from %s, %d bytes\n", Mp::PacketName(type), sequence, Mp::ToText(from), size);
            }
            break;
        }
        }
    }

    // Release delayed packets in due-time order.
    void ReleaseHeld()
    {
        const double now = Now();

        int kept = 0;

        for (int at = 0; at < g_heldCount; ++at)
        {
            if (g_held[at].due <= now)
            {
                OnPacket(g_held[at].from, g_held[at].data, g_held[at].size);
            }
            else
            {
                if (kept != at)
                {
                    g_held[kept] = g_held[at];
                }
                ++kept;
            }
        }

        g_heldCount = kept;
    }

    void Hold(const Mp::Address& from, const uint8_t* data, int size)
    {
        if (g_heldCount >= MAX_HELD)
        {
            // Drop packets when the intentional delay queue is full.
            ++g_dropped;
            return;
        }

        Held& one = g_held[g_heldCount++];

        one.from = from;
        one.due = Now() + g_settings.lag / 1000.0;
        one.size = size;
        memcpy(one.data, data, static_cast<size_t>(size));
    }

    void Pump(float seconds)
    {
        if (!g_running || !g_socket.IsOpen())
        {
            return;
        }

        uint8_t buffer[Mp::MAX_PACKET];

        for (;;)
        {
            Mp::Address from = {};
            const int size = g_socket.Receive(buffer, sizeof(buffer), from);

            if (size > 0)
            {
                if (g_settings.lag > 0)
                {
                    Hold(from, buffer, size);
                }
                else
                {
                    OnPacket(from, buffer, size);
                }
                continue;
            }

            if (size < 0)
            {
                Log(AMOD_LOG_WARNING, "the socket failed with %d, the network is off",
                    g_socket.LastError());
                g_running = false;
            }

            break;
        }

        ReleaseHeld();

        if (g_settings.rate <= 0.0f)
        {
            return;
        }

        g_sinceHello += seconds;

        const float period = 1.0f / g_settings.rate;
        if (g_sinceHello < period)
        {
            return;
        }

        g_sinceHello = 0.0f;

        for (int at = 0; at < g_targetCount; ++at)
        {
            SendHello(g_targets[at]);
        }
    }

    void DropAvatar(uint8_t id)
    {
        if (id <= MAX_PEERS && g_avatar[id] != nullptr)
        {
            Mp::Bodies::Destroy(g_avatar[id]);
            g_avatar[id] = nullptr;
        }
    }

    void DropAvatars()
    {
        for (int id = 0; id <= MAX_PEERS; ++id)
        {
            DropAvatar(static_cast<uint8_t>(id));
        }
    }

    void StepAvatars(float seconds, const Mp::Body& mine)
    {
        if (!Mp::Bodies::IsOpen())
        {
            return;
        }

        for (int at = 0; at < g_peerCount; ++at)
        {
            const uint8_t id = g_peers[at].id;

            if (id == 0 || id > MAX_PEERS)
            {
                continue;
            }

            const double now = Now();

            const bool playing = (now - g_pressedAt[id]) < INPUT_FRESH && (now - g_anchoredAt[id]) < INPUT_FRESH;

            // Do not simulate a remote body against the wrong room.
            if (!playing || g_anchored[id].room != mine.room)
            {
                if (g_avatar[id] != nullptr)
                {
                    DropAvatar(id);
                    g_bodyAt[id] = 0.0;
                    g_beforeAt[id] = 0.0;
                }
                continue;
            }

            if (g_avatar[id] == nullptr)
            {
                // Spawn the remote body at its latest anchor position.
                g_avatar[id] = Mp::Bodies::Create(g_anchored[id].position);
                g_avatarFacing[id] = g_anchored[id].facing;

                if (g_avatar[id] == nullptr)
                {
                    continue;
                }
            }

            const Mp::Input& pressed = g_pressed[id];
            const int jump = (pressed.buttons & Mp::INPUT_JUMP) != 0 ? 1 : 0;

            Mp::Bodies::Step(g_avatar[id], pressed.dirX, pressed.dirY, jump, seconds);

            Mp::Body body = {};
            int state = 0;
            int subState = 0;

            Mp::Bodies::Read(g_avatar[id], body.position, state, subState);

            // Anchors correct persistent drift, not ordinary jumps or climbs.
            const float* const said = g_anchored[id].position;
            float apart = 0.0f;

            for (int axis = 0; axis < 3; ++axis)
            {
                const float gap = body.position[axis] - said[axis];
                apart += gap * gap;
            }

            if (apart <= g_settings.snap * g_settings.snap)
            {
                g_apartAt[id] = 0.0;
            }
            else
            {
                if (g_apartAt[id] <= 0.0)
                {
                    g_apartAt[id] = Now();
                }

                if ((Now() - g_apartAt[id]) > APART_PATIENCE)
                {
                    Mp::Bodies::Place(g_avatar[id], said);
                    Mp::Bodies::Read(g_avatar[id], body.position, state, subState);
                    g_apartAt[id] = 0.0;
                }
            }

            // Use the facing reported by the remote player.
            g_avatarFacing[id] = pressed.facing;

            body.id = id;
            body.room = g_anchored[id].room;
            body.facing = g_avatarFacing[id];
            body.frameA = pressed.frameA;
            body.frameB = pressed.frameB;
            body.blend = pressed.blend;
            body.state = static_cast<uint8_t>(state);
            body.subState = static_cast<uint8_t>(subState);

            RememberBody(body, id, true);
        }
    }

    // Place remote bodies through the game's own collision and touch passes.
    void PlaceAvatars(const Mp::Body* others, int count, float seconds, const Mp::Body& mine)
    {
        if (others == nullptr || !Mp::Bodies::IsOpen())
        {
            return;
        }

        bool wanted[MAX_PEERS + 1] = {};

        for (int at = 0; at < count; ++at)
        {
            const uint8_t id = others[at].id;

            if (id > MAX_PEERS || others[at].room != mine.room)
            {
                continue;
            }

            if (g_avatar[id] == nullptr)
            {
                g_avatar[id] = Mp::Bodies::Create(others[at].position);
                g_avatarFacing[id] = others[at].facing;
            }

            if (g_avatar[id] == nullptr)
            {
                continue;
            }

            wanted[id] = true;

            // Step the body to run collision and touch logic, then restore its network position.
            Mp::Bodies::Place(g_avatar[id], others[at].position);
            Mp::Bodies::Step(g_avatar[id], 0, 0, 0, seconds);
            Mp::Bodies::Place(g_avatar[id], others[at].position);
        }

        for (int id = 0; id <= MAX_PEERS; ++id)
        {
            if (!wanted[id] && g_avatar[id] != nullptr)
            {
                DropAvatar(static_cast<uint8_t>(id));
            }
        }
    }

    // Remove all state associated with a disconnected peer immediately.
    void ForgetPeer(const Mp::Address& address)
    {
        const bool wasHost = g_host.IsSet() && g_host == address;

        for (int at = 0; at < g_peerCount; ++at)
        {
            if (!(g_peers[at].address == address))
            {
                continue;
            }

            const uint8_t id = g_peers[at].id;

            if (id != 0 && id <= MAX_PEERS)
            {
                g_bodyAt[id] = 0.0;
                g_beforeAt[id] = 0.0;
                DropAvatar(id);
            }

            g_peers[at] = g_peers[--g_peerCount];
            break;
        }

        // Player 0 is the host and is not stored as a peer.
        if (wasHost)
        {
            Mp::Room::LetGo();
            g_bodyAt[0] = 0.0;
            g_beforeAt[0] = 0.0;
            g_host = Mp::Address{};
            g_myId = 0;
        }
    }

    void DropSilentPeers()
    {
        const double now = Now();

        for (int at = 0; at < g_peerCount; )
        {
            if ((now - g_peers[at].heardAt) < PEER_TIMEOUT)
            {
                ++at;
                continue;
            }

            Console::Printf("mp: %s went quiet\n", Mp::ToText(g_peers[at].address));

            const uint8_t id = g_peers[at].id;
            if (id != 0 && id <= MAX_PEERS)
            {
                g_bodyAt[id] = 0.0;
                DropAvatar(id);
            }

            g_peers[at] = g_peers[--g_peerCount];
        }
    }

    // Locally predict movement and gradually reconcile it toward the host.
    void Reconcile(const Mp::Body& mine, float seconds)
    {
        if (g_isHost || g_myId == 0 || g_saidOfMeAt <= 0.0 || g_settings.pull <= 0.0f)
        {
            return;
        }

        const double now = Now();

        // Skip stale corrections and cross-room mismatches.
        if ((now - g_saidOfMeAt) > 1.0 || g_saidOfMe.room != mine.room)
        {
            return;
        }

        // Do not correct while climbing; wait until the player is on foot.
        if (mine.state != AMOD_PLAYER_STATE_ONFOOT)
        {
            g_notWalkingAt = now;
            g_apartSince = 0.0;
            return;
        }

        if ((now - g_notWalkingAt) < GRACE)
        {
            return;
        }

        float gap[3] = {};
        float apart = 0.0f;

        for (int axis = 0; axis < 3; ++axis)
        {
            gap[axis] = g_saidOfMe.position[axis] - mine.position[axis];
            apart += gap[axis] * gap[axis];
        }

        apart = sqrtf(apart);

        // Small differences are ignored.
        if (apart < 0.05f)
        {
            g_apartSince = 0.0;
            return;
        }

        // Require the difference to persist before changing the position.
        if (g_apartSince <= 0.0)
        {
            g_apartSince = now;
        }

        if ((now - g_apartSince) < PATIENCE)
        {
            return;
        }

        if (apart >= g_settings.hard)
        {
            Player::SetPosition(g_saidOfMe.position[0], g_saidOfMe.position[1],
                g_saidOfMe.position[2]);
            g_apartSince = 0.0;
            ++g_moved;
            return;
        }

        // Use SetPositionOnly so the correction does not erase movement delta.
        const float part = g_settings.pull * seconds > 1.0f ? 1.0f : g_settings.pull * seconds;

        Player::SetPositionOnly(mine.position[0] + gap[0] * part, mine.position[1] + gap[1] * part, mine.position[2] + gap[2] * part);
        ++g_pulled;
    }

    void Tick(float seconds)
    {
        if (!g_running)
        {
            return;
        }

        Mp::Body mine = {};
        const bool here = Mp::Avatars::ReadLocal(mine);

        mine.id = g_myId;

        if (!here)
        {
            DropAvatars();
            Mp::Room::LetGo();

            // A new level starts with fresh spent and room state.
            Mp::Spent::Forget();
            ResetOwners();
            g_jumpKeyCount = -1;
        }

        if (here && (g_isHost || g_myId != 0))
        {
            RememberBody(mine, g_myId, true);
        }

        if (here && g_isHost)
        {
            StepAvatars(seconds, mine);
        }

        if (here)
        {
            Reconcile(mine, seconds);
        }

        g_mine.tick = ++g_myTick;
        g_mine.buttons = JumpHeld() ? Mp::INPUT_JUMP : 0;
        g_mine.facing = mine.facing;
        g_mine.frameA = mine.frameA;
        g_mine.frameB = mine.frameB;
        g_mine.blend = mine.blend;

        g_sinceTick += seconds;

        const float period = g_settings.tick > 0.0f ? 1.0f / g_settings.tick : 0.0f;

        if (period > 0.0f && g_sinceTick >= period)
        {
            g_sinceTick = 0.0f;
            DropSilentPeers();

            if (g_isHost)
            {
                SendState();
            }
            else if (here)
            {
                SendInput();
            }
        }

        if (g_isHost)
        {
            SettleRooms(mine, here);
        }

        if (here && g_settings.world != 0 && g_settings.worldRate > 0.0f && IOwn(mine.room))
        {
            g_sinceWorld += seconds;

            if (g_sinceWorld >= 1.0f / g_settings.worldRate)
            {
                g_sinceWorld = 0.0f;
                SendWorld(mine.room);
            }
        }

        // Periodically exchange persistent spent state.
        if (here && g_settings.world != 0)
        {
            g_sinceSpent += seconds;
            g_sinceLook += seconds;

            // Check for immediate room changes several times per second.
            if (g_sinceLook >= 0.25f)
            {
                g_sinceLook = 0.0f;

                if (Mp::Spent::Changed())
                {
                    g_sinceSpent = 0.0f;
                    SendSpent();
                }
            }

            if (g_sinceSpent >= 2.0f)
            {
                g_sinceSpent = 0.0f;
                SendSpent();
            }
        }

        // Clients send position anchors at the configured rate.
        if (!g_isHost && here && g_settings.anchor > 0.0f)
        {
            g_sinceAnchor += seconds;

            if (g_sinceAnchor >= 1.0f / g_settings.anchor)
            {
                g_sinceAnchor = 0.0f;
                SendAnchor(mine);
            }
        }

        if (!here)
        {
            return;
        }

        Mp::Body others[MAX_PEERS + 1];
        int count = 0;
        const double now = Now();

        for (int id = 0; id <= MAX_PEERS; ++id)
        {
            if (id != g_myId && BodyIsLive(static_cast<uint8_t>(id), now))
            {
                others[count++] = Smoothed(static_cast<uint8_t>(id), now);
            }
        }

        // Clear cached room objects when entering a different room.
        if (mine.room != g_myRoom)
        {
            Mp::Room::LetGo();
        }

        g_myRoom = mine.room;

        // Clients simulate remote bodies locally for collision and touch behavior.
        if (!g_isHost)
        {
            PlaceAvatars(others, count, seconds, mine);
        }

        Mp::Avatars::Show(others, count, mine.room);
        g_drawn = Mp::Avatars::Tally();
    }

    void __cdecl OnFrameBegin(const AmodFrame* frame, void*)
    {
        ++g_renderFrames;
        Pump(frame != nullptr ? frame->delta : 0.0f);
    }

    // Open the network socket.

    void BuildLoopbackTargets(const Mp::Address& peer)
    {
        for (int at = 0; at < g_settings.slots && g_targetCount < MAX_PEERS; ++at)
        {
            const int port = g_settings.port + at;

            if (port == static_cast<int>(g_socket.Port()) || port > 65535)
            {
                continue;
            }

            g_targets[g_targetCount++] = Mp::WithPort(peer, static_cast<uint16_t>(port));
        }
    }

    bool StartNetwork()
    {
        g_targetCount = 0;
        g_peerCount = 0;

        if (!g_socket.OpenAny(static_cast<uint16_t>(g_settings.port), g_settings.slots))
        {
            Log(AMOD_LOG_ERROR, "no free port in %d..%d (last error %d)", g_settings.port, g_settings.port + g_settings.slots - 1, g_socket.LastError());
            return false;
        }

        Mp::Address peer = {};
        if (!Mp::Resolve(g_settings.peer, static_cast<uint16_t>(g_settings.port), peer))
        {
            Log(AMOD_LOG_WARNING, "mp_peer '%s' is not an address; nothing will be sent", g_settings.peer);
        }
        else if (Mp::IsLoopback(peer))
        {
            BuildLoopbackTargets(peer);
        }
        else
        {
            g_targets[g_targetCount++] = peer;
        }

        g_isHost = g_settings.host < 0
            ? g_socket.Port() == static_cast<uint16_t>(g_settings.port)
            : g_settings.host != 0;

        sprintf_s(g_name, sizeof(g_name), "az%u", static_cast<unsigned>(g_socket.Port()));

        g_running = true;

        Log(AMOD_LOG_INFO, "listening on port %u as '%s', %d target(s)", static_cast<unsigned>(g_socket.Port()), g_name, g_targetCount);

        for (int at = 0; at < g_targetCount; ++at)
        {
            Log(AMOD_LOG_INFO, "  target %s", Mp::ToText(g_targets[at]));
        }

        return true;
    }

    void StopNetwork()
    {
        if (g_socket.IsOpen())
        {
            SendBye();
            g_socket.Close();
        }

        g_running = false;
        g_targetCount = 0;
        g_peerCount = 0;
    }

    void ReadSettings()
    {
        Config::Add("mp_peer", g_settings.peer);
        Config::Add("mp_port", "27015");
        Config::Add("mp_slots", "4");
        Config::Add("mp_rate", "1");
        Config::Add("mp_tick", "30");
        Config::Add("mp_host", "-1");
        Config::Add("mp_stale", "5");
        Config::Add("mp_delay", "0.1");
        Config::Add("mp_anchor", "4");
        Config::Add("mp_snap", "1.2");
        Config::Add("mp_pull", "0");
        Config::Add("mp_hard", "0.6");
        Config::Add("mp_lag", "0");
        Config::Add("mp_world", "1");
        Config::Add("mp_wrate", "5");
        Config::Add("mp_hold", "0");
        Config::Add("mp_touch", "1");
        Config::Add("mp_background", "1");
        Config::Add("mp_focus", "1");
        Config::Add("mp_loss", "0");
        Config::Add("mp_log", "1");

        strcpy_s(g_settings.peer, sizeof(g_settings.peer), Config::Text("mp_peer", "127.0.0.1"));

        g_settings.port = static_cast<int>(Config::Number("mp_port", 27015.0f));
        g_settings.slots = static_cast<int>(Config::Number("mp_slots", 4.0f));
        g_settings.rate = Config::Number("mp_rate", 1.0f);
        g_settings.tick = Config::Number("mp_tick", 30.0f);
        g_settings.host = static_cast<int>(Config::Number("mp_host", -1.0f));
        g_settings.stale = Config::Number("mp_stale", 5.0f);
        g_settings.delay = Config::Number("mp_delay", 0.10f);
        g_settings.anchor = Config::Number("mp_anchor", 4.0f);
        g_settings.snap = Config::Number("mp_snap", 1.2f);
        g_settings.pull = Config::Number("mp_pull", 0.0f);
        g_settings.hard = Config::Number("mp_hard", 0.60f);
        g_settings.lag = static_cast<int>(Config::Number("mp_lag", 0.0f));
        g_settings.world = Config::Number("mp_world", 1.0f) != 0.0f ? 1 : 0;
        g_settings.worldRate = Config::Number("mp_wrate", 5.0f);

        if (g_settings.worldRate < 0.5f || g_settings.worldRate > 30.0f)
        {
            g_settings.worldRate = 5.0f;
        }
        g_settings.hold = Config::Number("mp_hold", 0.0f) != 0.0f ? 1 : 0;

        g_settings.touch = Config::Number("mp_touch", 1.0f) != 0.0f ? 1 : 0;

        Mp::Room::Hold(g_settings.hold != 0);
        Mp::Bodies::Touching(g_settings.touch != 0);
        g_settings.background = Config::Number("mp_background", 1.0f) != 0.0f ? 1 : 0;
        g_settings.focus = Config::Number("mp_focus", 1.0f) != 0.0f ? 1 : 0;

        if (g_settings.host < -1 || g_settings.host > 1)
        {
            g_settings.host = -1;
        }
        g_settings.loss = static_cast<int>(Config::Number("mp_loss", 0.0f));
        g_settings.chatter = Config::Number("mp_log", 1.0f) != 0.0f;

        if (g_settings.port < 1 || g_settings.port > 65535)
        {
            g_settings.port = 27015;
        }

        if (g_settings.slots < 1 || g_settings.slots > MAX_PEERS)
        {
            g_settings.slots = 4;
        }

        if (g_settings.pull < 0.0f || g_settings.pull > 60.0f)
        {
            g_settings.pull = 0.0f;
        }

        if (g_settings.hard <= 0.0f || g_settings.hard > 10.0f)
        {
            g_settings.hard = 0.60f;
        }

        if (g_settings.lag < 0 || g_settings.lag > 1000)
        {
            g_settings.lag = 0;
        }

        if (g_settings.anchor < 0.5f || g_settings.anchor > 30.0f)
        {
            g_settings.anchor = 4.0f;
        }

        if (g_settings.snap <= 0.0f || g_settings.snap > 10.0f)
        {
            g_settings.snap = 1.2f;
        }

        if (g_settings.delay < 0.0f || g_settings.delay > 1.0f)
        {
            g_settings.delay = 0.10f;
        }

        if (g_settings.stale <= 0.0f || g_settings.stale > 60.0f)
        {
            g_settings.stale = 5.0f;
        }

        if (g_settings.tick < 1.0f || g_settings.tick > 120.0f)
        {
            g_settings.tick = 30.0f;
        }

        if (g_settings.loss < 0 || g_settings.loss > 100)
        {
            g_settings.loss = 0;
        }
    }

    void Status()
    {
        if (!g_socket.IsOpen())
        {
            Console::Print("mp: off\n");
        }
        else
        {
            Console::Printf("mp: port %u as '%s', %s\n", static_cast<unsigned>(g_socket.Port()), g_name,
                g_running ? "running" : "paused");
        }

        if (g_isHost)
        {
            Console::Printf("mp: host%s, player 0, %.0f bodies a second\n",
                g_settings.host < 0 ? " (took the first port)" : "", g_settings.tick);
        }
        else if (g_myId != 0)
        {
            Console::Printf("mp: client, player %u, host %s\n", g_myId, Mp::ToText(g_host));
        }
        else
        {
            Console::Print("mp: client, waiting for a host to answer\n");
        }

        Console::Printf("mp: sent %llu, got %llu, ignored %llu, lost on purpose %llu\n", g_sent, g_received, g_ignored, g_dropped);

        if (g_lastRtt >= 0)
        {
            Console::Printf("mp: round trip %d ms\n", g_lastRtt);
        }

        Console::Printf("mp: pumped on %llu frame(s), the game ran a level on %llu\n", g_renderFrames, g_worldFrames);

        Console::Printf("mp: room %u, %d avatar(s) drawn, %d elsewhere, %d refused%s\n",
            g_myRoom, g_drawn.queued, g_drawn.elsewhere, g_drawn.refused,
            g_drawn.noModel ? ", no model" : (g_drawn.noView ? ", no camera" : ""));

        if (g_myRoom < MAX_ROOMS && g_owner[g_myRoom] != NOBODY && !IRun(g_myRoom))
        {
            Console::Printf("mp: this room is run by player %u; I follow him\n", g_owner[g_myRoom]);
        }
        else
        {
            Console::Printf("mp: this room is %s\n",
                IOwn(g_myRoom) ? "mine to run" : "nobody's yet");
        }

        if (Mp::Bodies::IsOpen())
        {
            int standing = 0;

            for (int id = 0; id <= MAX_PEERS; ++id)
            {
                standing += g_avatar[id] != nullptr ? 1 : 0;
            }

            Console::Printf("mp: %d body(ies) stepped by the game\n", standing);
        }
        else
        {
            Console::Printf("mp: no bodies of our own (%s)\n", Mp::Bodies::Trouble());
        }

        if (!g_isHost && Mp::Room::Held() > 0)
        {
            Console::Printf("mp: %d of the room's things are watched; machines run themselves\n",
                Mp::Room::Held());
        }

        Console::Printf("mp: drawn %.0f ms behind, between snapshots\n", g_settings.delay * 1000.0f);

        if (g_settings.lag > 0)
        {
            Console::Printf("mp: every packet held %d ms on purpose, %d waiting\n",
                g_settings.lag, g_heldCount);
        }

        if (!g_isHost)
        {
            if (g_settings.pull <= 0.0f)
            {
                Console::Print("mp: my own body is nobody's to correct ('mp pull 3' hands it to the host)\n");
            }
            else
            {
                Console::Printf("mp: pulled over %llu time(s), moved outright %llu\n", g_pulled, g_moved);
            }
        }

        Console::Printf("mp: behind the front window the game %s, %s and %s\n",
            Mp::Background::IsOn() ? "keeps running" : "sleeps",
            Mp::Background::KeepsPlaying() ? "does not pause" : "PAUSES",
            Mp::Focus::IsOn() ? "lets go of the keys" : "still takes the keys");

        for (int at = 0; at < g_targetCount; ++at)
        {
            Console::Printf("mp: -> %s\n", Mp::ToText(g_targets[at]));
        }

        const double now = Now();
        for (int at = 0; at < g_peerCount; ++at)
        {
            const Peer& peer = g_peers[at];

            Console::Printf("mp: <- %s '%s', %d packet(s), %.1fs ago%s\n", Mp::ToText(peer.address), peer.name, peer.packets,
                now - peer.heardAt, peer.greeted ? ", answered" : "");
        }

        if (g_peerCount == 0)
        {
            Console::Print("mp: nobody has been heard from yet\n");
        }
    }

    void PrintLine(const char* text)
    {
        Console::Print(text);
    }

    void Who()
    {
        const double now = Now();
        int shown = 0;

        for (int id = 0; id <= MAX_PEERS; ++id)
        {
            if (g_bodyAt[id] <= 0.0)
            {
                continue;
            }

            const Mp::Body& body = g_bodies[id];

            const double age = now - g_bodyAt[id];

            const char* why = "";
            if (id == g_myId)
            {
                why = " (me, never drawn)";
            }
            else if (age >= g_settings.stale)
            {
                why = " STALE";
            }
            else if (body.room != g_myRoom)
            {
                why = " ANOTHER ROOM";
            }

            Console::Printf("mp: player %u room %u at %.1f %.1f %.1f, facing %.2f, frames %u/%u, %.1fs ago%s\n",
                body.id, body.room,
                body.position[0], body.position[1], body.position[2],
                body.facing, body.frameA, body.frameB, age, why);
            ++shown;
        }

        if (shown == 0)
        {
            Console::Print("mp: no bodies yet; start a level\n");
        }
    }

    void __cdecl OnMpCommand(int argc, const char** argv, void*)
    {
        if (argc < 2)
        {
            Status();
            Console::Print("mp: peer <addr[:port]> | host [1|0] | who | avatar | frame <n> | delay <s> | lag <ms> | pull <n> | snap <n> | ctrls | trace [1|0] | world [1|0] | hold [1|0] | touch [1|0] | stale <s> | background [1|0] | focus [1|0] | hello | on | off | log | loss <0-100>\n");
            return;
        }

        const char* const what = argv[1];

        if (_stricmp(what, "peer") == 0)
        {
            if (argc < 3)
            {
                Console::Printf("mp: peer is %s\n", g_settings.peer);
                return;
            }
            strcpy_s(g_settings.peer, sizeof(g_settings.peer), argv[2]);
            Config::Set("mp_peer", g_settings.peer);

            g_targetCount = 0;

            Mp::Address peer = {};
            if (!Mp::Resolve(g_settings.peer, static_cast<uint16_t>(g_settings.port), peer))
            {
                Console::Printf("mp: '%s' is not an address\n", g_settings.peer);
                return;
            }

            if (Mp::IsLoopback(peer))
            {
                BuildLoopbackTargets(peer);
            }
            else
            {
                g_targets[g_targetCount++] = peer;
            }

            Console::Printf("mp: %d target(s)\n", g_targetCount);
            return;
        }

        if (_stricmp(what, "host") == 0)
        {
            if (argc >= 3)
            {
                g_settings.host = atoi(argv[2]) != 0 ? 1 : 0;
                g_isHost = g_settings.host != 0;
                Config::Set("mp_host", g_settings.host);

                // Reset ids and room ownership after changing roles.
                g_myId = 0;
                g_host = Mp::Address{};

                ResetOwners();

                for (int at = 0; at < g_peerCount; ++at)
                {
                    g_peers[at].id = 0;
                }

                for (int id = 0; id <= MAX_PEERS; ++id)
                {
                    g_bodyAt[id] = 0.0;
                }
            }

            Console::Printf("mp: %s\n", g_isHost ? "host" : "client");
            return;
        }

        if (_stricmp(what, "background") == 0)
        {
            if (argc >= 3)
            {
                g_settings.background = atoi(argv[2]) != 0 ? 1 : 0;
                Config::Set("mp_background", g_settings.background);

                if (g_settings.background != 0)
                {
                    Mp::Background::Take();
                }
                else
                {
                    Mp::Background::Release();
                }
            }

            Console::Printf("mp: behind the front window the game %s\n",
                Mp::Background::IsOn() ? "keeps running" : "sleeps");
            return;
        }

        if (_stricmp(what, "focus") == 0)
        {
            if (argc >= 3)
            {
                g_settings.focus = atoi(argv[2]) != 0 ? 1 : 0;
                Config::Set("mp_focus", g_settings.focus);

                if (g_settings.focus != 0)
                {
                    Mp::Focus::Take();
                }
                else
                {
                    Mp::Focus::Release();
                }
            }

            Console::Printf("mp: a window behind %s\n",
                Mp::Focus::IsOn() ? "lets go of the keys" : "still takes the keys");
            return;
        }

        if (_stricmp(what, "frame") == 0)
        {
            if (argc >= 3)
            {
                Mp::Avatars::HoldFrame(static_cast<float>(atof(argv[2])));
            }

            const float held = Mp::Avatars::HeldFrame();

            if (held < 0.0f)
            {
                Console::Print("mp: avatars animate as they are sent; 'mp frame <n>' freezes them\n");
            }
            else
            {
                Console::Printf("mp: every avatar is held at frame %.1f; 'mp frame -1' lets go\n", held);
            }
            return;
        }

        if (_stricmp(what, "trace") == 0)
        {
            const bool on = argc >= 3 && atoi(argv[2]) != 0;

            Mp::Room::Trace(on);
            Console::Printf("mp: every tag written is %s in forge.log\n", on ? "put" : "not put");
            return;
        }

        if (_stricmp(what, "doors") == 0)
        {
            Mp::Spent::Doors(argc >= 3 ? atoi(argv[2]) : g_myRoom, &PrintLine);
            return;
        }

        if (_stricmp(what, "ctrls") == 0)
        {
            Mp::Room::Report(&PrintLine);
            return;
        }

        if (_stricmp(what, "world") == 0)
        {
            if (argc >= 3)
            {
                g_settings.world = atoi(argv[2]) != 0 ? 1 : 0;
                Config::Set("mp_world", g_settings.world);

                if (g_settings.world == 0)
                {
                    Mp::Room::LetGo();
                }
            }

            Console::Printf("mp: the room's moving parts are %s\n",
                g_settings.world != 0 ? "the host's" : "each copy's own");
            return;
        }

        if (_stricmp(what, "touch") == 0)
        {
            if (argc >= 3)
            {
                g_settings.touch = atoi(argv[2]) != 0 ? 1 : 0;
                Config::Set("mp_touch", g_settings.touch);
                Mp::Bodies::Touching(g_settings.touch != 0);
            }

            Console::Printf("mp: a stepped body %s what it stands in\n",
                Mp::Bodies::IsTouching() ? "meets" : "walks through");
            return;
        }

        if (_stricmp(what, "hold") == 0)
        {
            if (argc >= 3)
            {
                g_settings.hold = atoi(argv[2]) != 0 ? 1 : 0;
                Config::Set("mp_hold", g_settings.hold);
                Mp::Room::Hold(g_settings.hold != 0);
            }

            Console::Printf("mp: a client %s them off its own game\n",
                Mp::Room::Holding() ? "takes" : "does not take");
            return;
        }

        if (_stricmp(what, "lag") == 0)
        {
            if (argc >= 3)
            {
                const int asked = atoi(argv[2]);
                g_settings.lag = (asked >= 0 && asked <= 1000) ? asked : 0;
                Config::Set("mp_lag", g_settings.lag);

                if (g_settings.lag == 0)
                {
                    g_heldCount = 0;
                }
            }

            Console::Printf("mp: every packet is held %d ms before it is read\n", g_settings.lag);
            return;
        }

        if (_stricmp(what, "snap") == 0)
        {
            if (argc >= 3)
            {
                const float asked = static_cast<float>(atof(argv[2]));

                g_settings.snap = (asked > 0.0f && asked <= 10.0f) ? asked : 1.2f;
                Config::Set("mp_snap", g_settings.snap);
            }

            Console::Printf("mp: a body is put back where he says he is past %.2f, and only after %.1fs\n",
                g_settings.snap, APART_PATIENCE);
            return;
        }

        if (_stricmp(what, "pull") == 0)
        {
            if (argc >= 3)
            {
                const float asked = static_cast<float>(atof(argv[2]));
                g_settings.pull = (asked >= 0.0f && asked <= 60.0f) ? asked : 0.0f;
                Config::Set("mp_pull", g_settings.pull);
            }

            Console::Printf("mp: my body is walked %.1f of the way over a second, moved outright past %.2f\n",
                g_settings.pull, g_settings.hard);
            return;
        }

        if (_stricmp(what, "delay") == 0)
        {
            if (argc >= 3)
            {
                const float asked = static_cast<float>(atof(argv[2]));
                g_settings.delay = (asked >= 0.0f && asked <= 1.0f) ? asked : 0.10f;
                Config::Set("mp_delay", g_settings.delay);
            }

            Console::Printf("mp: avatars are drawn %.0f ms behind; 0 draws the newest as it stands\n",
                g_settings.delay * 1000.0f);
            return;
        }

        if (_stricmp(what, "stale") == 0)
        {
            if (argc >= 3)
            {
                const float asked = static_cast<float>(atof(argv[2]));
                g_settings.stale = (asked > 0.0f && asked <= 60.0f) ? asked : 5.0f;
                Config::Set("mp_stale", g_settings.stale);
            }

            Console::Printf("mp: a body is drawn for %.1fs after it was last heard\n",
                g_settings.stale);
            return;
        }

        if (_stricmp(what, "who") == 0)
        {
            Who();
            return;
        }

        if (_stricmp(what, "avatar") == 0)
        {
            Mp::Avatars::Report(&PrintLine);
            return;
        }

        if (_stricmp(what, "hello") == 0)
        {
            for (int at = 0; at < g_targetCount; ++at)
            {
                SendHello(g_targets[at]);
            }

            Console::Printf("mp: hello to %d target(s)\n", g_targetCount);
            return;
        }

        if (_stricmp(what, "on") == 0)
        {
            if (!g_socket.IsOpen())
            {
                StartNetwork();
            }
            else
            {
                g_running = true;
            }

            Status();
            return;
        }

        if (_stricmp(what, "off") == 0)
        {
            g_running = false;
            Console::Print("mp: paused; 'mp on' again\n");
            return;
        }

        if (_stricmp(what, "log") == 0)
        {
            g_settings.chatter = !g_settings.chatter;
            Config::Set("mp_log", g_settings.chatter ? 1 : 0);
            Console::Printf("mp: a line per packet is %s\n", g_settings.chatter ? "on" : "off");
            return;
        }

        if (_stricmp(what, "loss") == 0)
        {
            if (argc >= 3)
            {
                const int asked = atoi(argv[2]);
                g_settings.loss = (asked < 0) ? 0 : (asked > 100 ? 100 : asked);
                Config::Set("mp_loss", g_settings.loss);
            }

            Console::Printf("mp: %d%% of what is sent is thrown away\n", g_settings.loss);
            return;
        }

        Console::Printf("mp: no such thing as '%s'\n", what);
    }
}

AMOD_EXPORT int __cdecl AmodInit(const AmodApi* api)
{
    if (!Attach(api))
    {
        return AMOD_ERROR_API_VERSION;
    }

    ClockStart();
    srand(GetTickCount());

    ReadSettings();

    if (!Mp::NetStartup())
    {
        Log(AMOD_LOG_ERROR, "Winsock would not start");
        return AMOD_ERROR;
    }

    if (!StartNetwork())
    {
        Mp::NetShutdown();
        return AMOD_ERROR;
    }

    g_frameToken = Render::OnFrameBegin(&OnFrameBegin);

    if (g_frameToken == AMOD_INVALID_TOKEN)
    {
        Log(AMOD_LOG_WARNING, "no per-frame hook; the network only runs inside a level");
    }

    if (g_settings.background != 0)
    {
        Mp::Background::Take();
    }

    if (g_settings.focus != 0)
    {
        Mp::Focus::Take();
    }

    Mp::Bodies::Open();
    Mp::Spent::Watch();

    ResetOwners();

    Player::SetMoveHandler(&OnMove);

    Console::AddCommand("mp", &OnMpCommand);

    Log(AMOD_LOG_INFO, "%s is up as %s", ModId(), g_isHost ? "host" : "client");
    return AMOD_OK;
}

AMOD_EXPORT void __cdecl AmodUpdate(const AmodFrame* frame)
{
    ++g_worldFrames;

    const float delta = frame != nullptr ? frame->delta : 0.0f;

    if (g_frameToken == AMOD_INVALID_TOKEN)
    {
        Pump(delta);
    }

    Tick(delta);
}

AMOD_EXPORT void __cdecl AmodShutdown(void)
{
    Mp::Room::LetGo();
    DropAvatars();
    Mp::Bodies::Close();
    Mp::Focus::Release();
    Mp::Background::Release();
    Mp::Avatars::Forget();
    StopNetwork();
    Mp::NetShutdown();
}