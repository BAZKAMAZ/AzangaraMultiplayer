#include <ForgeApi.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Avatars.h"
#include "Background.h"
#include "Focus.h"
#include "Net.h"
#include "Protocol.h"

using namespace Amod;

namespace
{
    // settings, all out of Azangara.cfg
    struct Settings
    {
        char peer[128];     // mp_peer:  where to say hello; a bare address means mp_port
        int port;           // mp_port:  first port of the local range
        int slots;          // mp_slots: how many ports the range holds
        float rate;         // mp_rate:  hellos a second
        float tick;         // mp_tick:  bodies a second
        int host;           // mp_host:  1 host, 0 client, -1 whoever took the first port
        float stale;        // mp_stale: seconds a body is still worth drawing
        float delay;        // mp_delay: how far behind the newest snapshot to draw
        int background;     // mp_background: keep running when the window is not in front
        int focus;          // mp_focus: take the keyboard only while in front
        int loss;           // mp_loss:  per cent of outgoing packets thrown away on purpose
        bool chatter;       // mp_log:   one line per packet
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
        double heardAt;     // our own clock, seconds
        int packets;
        bool greeted;       // it has answered at least once
        uint8_t id;         // what the host calls him; 0 until it says
    };

    Mp::Socket g_socket;
    Mp::Address g_targets[MAX_PEERS];    // where hellos go
    int g_targetCount = 0;

    Peer g_peers[MAX_PEERS];             // who has been heard from
    int g_peerCount = 0;

    char g_name[Mp::MAX_NAME + 1] = "player";

    bool g_running = false;
    uint16_t g_sequence = 0;

    // The host is player 0 and hands out the rest.
    bool g_isHost = true;

    uint8_t g_myId = 0;
    Mp::Address g_host = {};

    // Everyone, by id. The host fills these from MOVE, a client from STATE.
    Mp::Body g_bodies[MAX_PEERS + 1] = {};
    double g_bodyAt[MAX_PEERS + 1] = {};

    Mp::Body g_before[MAX_PEERS + 1] = {};
    double g_beforeAt[MAX_PEERS + 1] = {};

    const double PEER_TIMEOUT = 5.0;

    float g_sinceTick = 0.0f;
    Mp::Avatars::Drawn g_drawn = {};
    uint16_t g_myRoom = 0;


    uint64_t g_sent = 0;
    uint64_t g_received = 0;
    uint64_t g_ignored = 0;      // arrived, but not ours: wrong version or too short
    uint64_t g_dropped = 0;      // thrown away by mp_loss on the way out
    uint64_t g_renderFrames = 0; // frames the network was pumped on
    uint64_t g_worldFrames = 0;  // frames the game ran a level on
    int g_lastRtt = -1;          // milliseconds, -1 until a WELCOME comes back

    float g_sinceHello = 0.0f;

    AmodToken g_frameToken = AMOD_INVALID_TOKEN;

    // our own clock

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

    // peers

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

    // sending

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
        writer.U32(NowMs());        // comes back in the WELCOME, which is the round trip

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
        writer.U32(stamp);          // theirs, not ours - it is their clock being measured
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
        writer.U8(0);               // a reason, once there is more than one

        for (int at = 0; at < g_peerCount; ++at)
        {
            g_socket.Send(g_peers[at].address, buffer, writer.Size());
        }
    }

    void RememberBody(const Mp::Body& body, uint8_t id)
    {
        if (id > MAX_PEERS)
        {
            return;
        }

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

        const float step = latest.frame - was.frame;
        body.frame = (step > -8.0f && step < 8.0f) ? was.frame + step * part : latest.frame;

        // The short way round, or turning about-face would swing him the long way.
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

    // Client -> host: where I am.
    void SendMove(const Mp::Body& body)
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

    // Host -> clients: everyone, including the host.
    void SendState()
    {
        uint8_t buffer[Mp::MAX_PACKET];
        Mp::Writer writer(buffer, sizeof(buffer));

        const double now = Now();
        int count = 0;

        writer.Header(Mp::PACKET_STATE, g_sequence++);
        writer.U32(NowMs());

        // Counted first, so the count byte is right before the bodies go in.
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

    // receiving

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

            // Only a host hands out an id, so this is also how a client learns which peer it is.
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
                //Console::Printf("mp: WELCOME #%u from %s (%s), id %u, %d ms\n",
                //                sequence, Mp::ToText(from), name, static_cast<unsigned>(id), g_lastRtt);
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

            // The id is the host's to decide, not the sender's.
            const Peer* const peer = FindPeer(from);
            if (peer == nullptr || peer->id == 0)
            {
                ++g_ignored;
                return;
            }

            RememberBody(body, peer->id);
            break;
        }

        case Mp::PACKET_STATE:
        {
            const uint32_t sent = reader.U32();
            const int count = reader.U8();

            // The world is the host's; a host that took one would be reading its own back.
            if (reader.Bad() || g_isHost)
            {
                ++g_ignored;
                return;
            }

            (void)sent;   // stage 3 interpolates between snapshots by this

            for (int at = 0; at < count; ++at)
            {
                Mp::Body body = {};
                Mp::ReadBody(reader, body);

                if (reader.Bad())
                {
                    ++g_ignored;
                    return;
                }

                // Mine comes back too; reconciling it is stage 3.
                if (body.id != g_myId)
                {
                    RememberBody(body, body.id);
                }
            }
            break;
        }

        case Mp::PACKET_BYE:
        {
            Console::Printf("mp: BYE from %s\n", Mp::ToText(from));
            break;
        }

        default:
        {
            // INPUT, STATE and EVENT are stages 1 and 2. Counted, not acted on.
            if (g_settings.chatter)
            {
                Console::Printf("mp: %s #%u from %s, %d bytes\n",
                                Mp::PacketName(type), sequence, Mp::ToText(from), size);
            }
            break;
        }
        }
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
                OnPacket(from, buffer, size);
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
            }

            g_peers[at] = g_peers[--g_peerCount];
        }
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

        // A client with no id yet would be writing itself into the host's slot.
        if (here && (g_isHost || g_myId != 0))
        {
            RememberBody(mine, g_myId);
        }

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
                SendMove(mine);
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

        g_myRoom = mine.room;
        Mp::Avatars::Show(others, count, mine.room);
        g_drawn = Mp::Avatars::Tally();
    }

    void __cdecl OnFrameBegin(const AmodFrame* frame, void*)
    {
        ++g_renderFrames;
        Pump(frame != nullptr ? frame->delta : 0.0f);
    }

    // opening the socket

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

        Console::Printf("mp: drawn %.0f ms behind, between snapshots\n", g_settings.delay * 1000.0f);

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

            Console::Printf("mp: player %u room %u at %.1f %.1f %.1f, facing %.2f, frame %.1f, %.1fs ago%s\n",
                            body.id, body.room,
                            body.position[0], body.position[1], body.position[2],
                            body.facing, body.frame, age, why);
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
            Console::Print("mp: peer <addr[:port]> | host [1|0] | who | avatar | frame <n> | delay <s> | stale <s> | background [1|0] | focus [1|0] | hello | on | off | log | loss <0-100>\n");
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

                // The ids were handed out under the old role and mean nothing now.
                g_myId = 0;
                g_host = Mp::Address{};

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
    Mp::Focus::Release();
    Mp::Background::Release();
    Mp::Avatars::Forget();
    StopNetwork();
    Mp::NetShutdown();
}
