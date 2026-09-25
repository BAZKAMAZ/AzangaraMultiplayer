#ifndef AZANGARA_MULTIPLAYER_NET_H
#define AZANGARA_MULTIPLAYER_NET_H

#include <stdint.h>

namespace Mp
{
    struct                                                          Address
    {
        uint32_t                                                    ip;
        uint16_t                                                    port;

        bool                                                        operator==(const Address& other) const
                                                                    {
                                                                        return ip == other.ip && port == other.port;
                                                                    }

        bool                                                        IsSet() const { return port != 0; }
    };

    bool                                                            NetStartup();
    void                                                            NetShutdown();

    // "1.2.3.4", "1.2.3.4:27015" or "host.name:27015". Blocking! Only ever called from a console
    bool                                                            Resolve(const char* text, uint16_t defaultPort, Address& out);

    // Into a rotating set of small buffers, so several can be printed in one line.
    const char*                                                     ToText(const Address& address);

    bool                                                            IsLoopback(const Address& address);

    Address                                                         WithPort(const Address& address, uint16_t hostPort);
    uint16_t                                                        PortOf(const Address& address);

    class Socket
    {
    public:
                                                                    Socket();
                                                                    ~Socket();

        bool                                                        Open(uint16_t port);

        // First free port in [first, first + count). Which one it got is Port().
        bool                                                        OpenAny(uint16_t first, int count);
                                                                    
        void                                                        Close();
                                                                    
        bool                                                        IsOpen() const;
        uint16_t                                                    Port() const { return m_port; }

        int                                                         Send(const Address& to, const void* data, int size);

        int                                                         Receive(void* buffer, int size, Address& from);

        // What the last failing call said, for the log.
        int                                                         LastError() const { return m_error; }

    private:
                                                                    Socket(const Socket&);
                                                                    Socket& operator=(const Socket&);

        uintptr_t                                                   m_handle;
        uint16_t                                                    m_port;
        int                                                         m_error;
    };
}

#endif
