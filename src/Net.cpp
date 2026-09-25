#include "Net.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

namespace Mp
{
    namespace
    {
        int g_started = 0;

        const uintptr_t NO_SOCKET = static_cast<uintptr_t>(INVALID_SOCKET);
    }

    bool NetStartup()
    {
        if (g_started > 0)
        {
            ++g_started;
            return true;
        }

        WSADATA data = {};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            return false;
        }

        g_started = 1;
        return true;
    }

    void NetShutdown()
    {
        if (g_started <= 0)
        {
            return;
        }

        if (--g_started == 0)
        {
            WSACleanup();
        }
    }

    bool Resolve(const char* text, uint16_t defaultPort, Address& out)
    {
        out.ip = 0;
        out.port = 0;

        if (text == nullptr || text[0] == 0)
        {
            return false;
        }

        char host[128] = {};
        uint16_t port = defaultPort;

        const char* colon = strrchr(text, ':');
        if (colon != nullptr && colon[1] != 0)
        {
            const size_t length = static_cast<size_t>(colon - text);
            if (length == 0 || length >= sizeof(host))
            {
                return false;
            }

            memcpy(host, text, length);
            host[length] = 0;

            const int asked = atoi(colon + 1);
            if (asked <= 0 || asked > 65535)
            {
                return false;
            }
            port = static_cast<uint16_t>(asked);
        }
        else
        {
            if (strlen(text) >= sizeof(host))
            {
                return false;
            }
            strcpy_s(host, sizeof(host), text);
        }

        // A dotted quad first, so the common case never touches a resolver.
        in_addr numeric = {};
        if (inet_pton(AF_INET, host, &numeric) == 1)
        {
            out.ip = numeric.s_addr;
            out.port = htons(port);
            return true;
        }

        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        addrinfo* found = nullptr;
        if (getaddrinfo(host, nullptr, &hints, &found) != 0 || found == nullptr)
        {
            return false;
        }

        const sockaddr_in* address = reinterpret_cast<const sockaddr_in*>(found->ai_addr);
        out.ip = address->sin_addr.s_addr;
        out.port = htons(port);

        freeaddrinfo(found);
        return true;
    }

    const char* ToText(const Address& address)
    {
        static char texts[4][32];
        static int next = 0;

        char* const text = texts[next];
        next = (next + 1) % 4;

        in_addr numeric = {};
        numeric.s_addr = address.ip;

        char host[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &numeric, host, sizeof(host)) == nullptr)
        {
            strcpy_s(host, sizeof(host), "?");
        }

        sprintf_s(text, 32, "%s:%u", host, static_cast<unsigned>(ntohs(address.port)));
        return text;
    }

    bool IsLoopback(const Address& address)
    {
        return (ntohl(address.ip) >> 24) == 127u;
    }

    Address WithPort(const Address& address, uint16_t hostPort)
    {
        Address out = address;
        out.port = htons(hostPort);
        return out;
    }

    uint16_t PortOf(const Address& address)
    {
        return ntohs(address.port);
    }

    Socket::Socket()
        : m_handle(NO_SOCKET), m_port(0), m_error(0)
    {
    }

    Socket::~Socket()
    {
        Close();
    }

    bool Socket::Open(uint16_t port)
    {
        Close();

        const SOCKET handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (handle == INVALID_SOCKET)
        {
            m_error = WSAGetLastError();
            return false;
        }

        u_long nonBlocking = 1;
        if (ioctlsocket(handle, FIONBIO, &nonBlocking) == SOCKET_ERROR)
        {
            m_error = WSAGetLastError();
            closesocket(handle);
            return false;
        }

        sockaddr_in local = {};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = htons(port);

        if (bind(handle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
        {
            m_error = WSAGetLastError();
            closesocket(handle);
            return false;
        }

        // Asking for port 0 means the system picked one, so it is read back rather than assumed.
        int length = sizeof(local);
        if (getsockname(handle, reinterpret_cast<sockaddr*>(&local), &length) == 0)
        {
            m_port = ntohs(local.sin_port);
        }
        else
        {
            m_port = port;
        }

        m_handle = static_cast<uintptr_t>(handle);
        m_error = 0;
        return true;
    }

    bool Socket::OpenAny(uint16_t first, int count)
    {
        for (int at = 0; at < count; ++at)
        {
            const int port = static_cast<int>(first) + at;
            if (port > 65535)
            {
                break;
            }

            if (Open(static_cast<uint16_t>(port)))
            {
                return true;
            }
        }

        return false;
    }

    void Socket::Close()
    {
        if (m_handle != NO_SOCKET)
        {
            closesocket(static_cast<SOCKET>(m_handle));
            m_handle = NO_SOCKET;
        }

        m_port = 0;
    }

    bool Socket::IsOpen() const
    {
        return m_handle != NO_SOCKET;
    }

    int Socket::Send(const Address& to, const void* data, int size)
    {
        if (!IsOpen() || data == nullptr || size <= 0)
        {
            return -1;
        }

        sockaddr_in remote = {};
        remote.sin_family = AF_INET;
        remote.sin_addr.s_addr = to.ip;
        remote.sin_port = to.port;

        const int sent = sendto(static_cast<SOCKET>(m_handle), static_cast<const char*>(data), size, 0,
                                reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));

        if (sent == SOCKET_ERROR)
        {
            m_error = WSAGetLastError();

            if (m_error == WSAEWOULDBLOCK)
            {
                return size;
            }

            return -1;
        }

        return sent;
    }

    int Socket::Receive(void* buffer, int size, Address& from)
    {
        from.ip = 0;
        from.port = 0;

        if (!IsOpen() || buffer == nullptr || size <= 0)
        {
            return -1;
        }

        sockaddr_in remote = {};
        int length = sizeof(remote);

        const int read = recvfrom(static_cast<SOCKET>(m_handle),
                                  static_cast<char*>(buffer), size, 0,
                                  reinterpret_cast<sockaddr*>(&remote), &length);

        if (read == SOCKET_ERROR)
        {
            m_error = WSAGetLastError();

            // Nothing waiting is the normal answer, not an error.
            if (m_error == WSAEWOULDBLOCK)
            {
                return 0;
            }

            if (m_error == WSAECONNRESET || m_error == WSAEMSGSIZE)
            {
                return 0;
            }

            return -1;
        }

        from.ip = remote.sin_addr.s_addr;
        from.port = remote.sin_port;
        return read;
    }
}
