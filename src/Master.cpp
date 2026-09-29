#include "Master.h"

#include "Protocol.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <ForgeApi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")

using namespace Amod;

namespace Mp
{
    namespace Master
    {
        namespace
        {
            const DWORD TIMEOUT = 4000;
            const DWORD STOP_PATIENCE = 5000;

            const wchar_t* const IPV4_ECHO = L"https://api4.ipify.org";

            const unsigned JOB_ANNOUNCE = 1;
            const unsigned JOB_WITHDRAW = 2;
            const unsigned JOB_LIST = 4;

            const int MAX_BODY = 1024;
            const int MAX_LIST = 16384;

            CRITICAL_SECTION g_lock;
            bool g_open = false;
            HANDLE g_thread = nullptr;
            HANDLE g_wake = nullptr;
            volatile LONG g_stop = 0;

            wchar_t g_rooms[512] = {};

            // Shared with the thread, under g_lock.
            unsigned g_jobs = 0;
            char g_announce[MAX_BODY] = {};
            char g_withdraw[MAX_BODY] = {};
            char g_list[MAX_LIST] = {};
            bool g_listFresh = false;
            char g_failure[128] = {};

            // The thread's own.
            char g_ip[64] = {};

            // The main thread's own: the list as last parsed.
            Entry g_entries[MAX_ENTRIES] = {};
            int g_entryCount = 0;

            void Append(char* out, size_t size, const char* text)
            {
                strcat_s(out, size, text);
            }

            void AppendEncoded(char* out, size_t size, const char* text)
            {
                char piece[4] = {};

                for (const unsigned char* at = reinterpret_cast<const unsigned char*>(text != nullptr ? text : ""); *at != 0; ++at)
                {
                    const bool plain = (*at >= 'a' && *at <= 'z') || (*at >= 'A' && *at <= 'Z') || (*at >= '0' && *at <= '9') ||
                        *at == '-' || *at == '_' || *at == '.' || *at == '~';

                    if (plain)
                    {
                        piece[0] = static_cast<char>(*at);
                        piece[1] = 0;
                    }
                    else
                    {
                        sprintf_s(piece, "%%%02X", *at);
                    }

                    Append(out, size, piece);
                }
            }

            void AppendField(char* out, size_t size, const char* key, const char* value)
            {
                if (out[0] != 0)
                {
                    Append(out, size, "&");
                }

                Append(out, size, key);
                Append(out, size, "=");
                AppendEncoded(out, size, value);
            }

            // One HTTP(S) exchange; what came back is in out, null terminated. False on any failure.
            bool Request(const wchar_t* url, const wchar_t* verb, const char* body, char* out, int capacity)
            {
                out[0] = 0;

                wchar_t host[256] = {};
                wchar_t path[1024] = {};
                wchar_t extra[1024] = {};

                URL_COMPONENTS parts = {};
                parts.dwStructSize = sizeof(parts);
                parts.lpszHostName = host;
                parts.dwHostNameLength = ARRAYSIZE(host);
                parts.lpszUrlPath = path;
                parts.dwUrlPathLength = ARRAYSIZE(path);
                parts.lpszExtraInfo = extra;
                parts.dwExtraInfoLength = ARRAYSIZE(extra);

                if (!WinHttpCrackUrl(url, 0, 0, &parts))
                {
                    return false;
                }

                wcscat_s(path, extra);

                HINTERNET const session = WinHttpOpen(L"AzangaraMultiplayer", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

                if (session == nullptr)
                {
                    return false;
                }

                WinHttpSetTimeouts(session, TIMEOUT, TIMEOUT, TIMEOUT, TIMEOUT);

                HINTERNET const connection = WinHttpConnect(session, host, parts.nPort, 0);
                HINTERNET const request = connection != nullptr
                    ? WinHttpOpenRequest(connection, verb, path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;

                const DWORD length = body != nullptr ? static_cast<DWORD>(strlen(body)) : 0;
                bool done = false;

                if (request != nullptr && WinHttpSendRequest(request, L"Content-Type: application/x-www-form-urlencoded\r\n", static_cast<DWORD>(-1L),
                    const_cast<char*>(body != nullptr ? body : ""), length, length, 0) && WinHttpReceiveResponse(request, nullptr))
                {
                    DWORD status = 0;
                    DWORD size = sizeof(status);

                    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);

                    int kept = 0;
                    DWORD read = 0;

                    while (kept + 1 < capacity && WinHttpReadData(request, out + kept, static_cast<DWORD>(capacity - 1 - kept), &read) && read > 0)
                    {
                        kept += static_cast<int>(read);
                    }

                    out[kept] = 0;
                    done = status == 200;

                    // Said by the main thread in Take; the log is not the thread's to write.
                    if (!done)
                    {
                        EnterCriticalSection(&g_lock);
                        sprintf_s(g_failure, "%lu %.100s", status, out);
                        LeaveCriticalSection(&g_lock);
                    }
                }

                if (request != nullptr)
                {
                    WinHttpCloseHandle(request);
                }

                if (connection != nullptr)
                {
                    WinHttpCloseHandle(connection);
                }

                WinHttpCloseHandle(session);
                return done;
            }

            // The public IPv4, looked up once; "" until it answers.
            const char* PublicIp()
            {
                if (g_ip[0] == 0 && Request(IPV4_ECHO, L"GET", nullptr, g_ip, sizeof(g_ip)))
                {
                    for (char* at = g_ip; *at != 0; ++at)
                    {
                        *at = (*at == '\r' || *at == '\n' || *at == ' ') ? 0 : *at;
                    }
                }

                return g_ip;
            }

            void Send(const wchar_t* verb, const char* fields)
            {
                char body[MAX_BODY + 96] = {};
                char answer[256] = {};

                strcpy_s(body, fields);
                AppendField(body, sizeof(body), "ip", PublicIp());

                Request(g_rooms, verb, body, answer, sizeof(answer));
            }

            DWORD WINAPI Run(LPVOID)
            {
                for (;;)
                {
                    WaitForSingleObject(g_wake, INFINITE);

                    char announce[MAX_BODY] = {};
                    char withdraw[MAX_BODY] = {};

                    EnterCriticalSection(&g_lock);
                    const unsigned jobs = g_jobs;
                    g_jobs = 0;
                    strcpy_s(announce, g_announce);
                    strcpy_s(withdraw, g_withdraw);
                    LeaveCriticalSection(&g_lock);

                    if ((jobs & JOB_WITHDRAW) != 0)
                    {
                        Send(L"DELETE", withdraw);
                    }

                    if (g_stop != 0)
                    {
                        return 0;
                    }

                    if ((jobs & JOB_ANNOUNCE) != 0)
                    {
                        Send(L"POST", announce);
                    }

                    if ((jobs & JOB_LIST) != 0)
                    {
                        static char list[MAX_LIST];
                        wchar_t url[600] = {};

                        swprintf_s(url, L"%ls?version=%u", g_rooms, static_cast<unsigned>(PROTOCOL_VERSION));

                        if (Request(url, L"GET", nullptr, list, sizeof(list)))
                        {
                            EnterCriticalSection(&g_lock);
                            strcpy_s(g_list, list);
                            g_listFresh = true;
                            LeaveCriticalSection(&g_lock);
                        }
                    }
                }
            }

            void Queue(unsigned job)
            {
                g_jobs |= job;
                SetEvent(g_wake);
            }

            void CopyField(char* out, size_t size, const char* from, const char* to)
            {
                const size_t length = static_cast<size_t>(to - from);
                strncpy_s(out, size, from, length < size ? length : size - 1);
            }

            // address \t players \t palace \t level \t name, a line a room.
            void Parse(const char* list)
            {
                g_entryCount = 0;

                for (const char* line = list; *line != 0 && g_entryCount < MAX_ENTRIES;)
                {
                    const char* end = strchr(line, '\n');
                    end = end != nullptr ? end : line + strlen(line);

                    const char* fields[5] = {};
                    const char* ends[5] = {};
                    int count = 0;

                    for (const char* at = line; count < 5;)
                    {
                        const char* tab = static_cast<const char*>(memchr(at, '\t', static_cast<size_t>(end - at)));

                        fields[count] = at;
                        ends[count] = tab != nullptr ? tab : end;
                        ++count;

                        if (tab == nullptr)
                        {
                            break;
                        }

                        at = tab + 1;
                    }

                    if (count == 5)
                    {
                        Entry& entry = g_entries[g_entryCount++];
                        char players[16] = {};

                        entry = Entry{};
                        CopyField(entry.address, sizeof(entry.address), fields[0], ends[0]);
                        CopyField(players, sizeof(players), fields[1], ends[1]);
                        CopyField(entry.palace, sizeof(entry.palace), fields[2], ends[2]);
                        CopyField(entry.level, sizeof(entry.level), fields[3], ends[3]);
                        CopyField(entry.name, sizeof(entry.name), fields[4], ends[4]);
                        entry.players = atoi(players);
                    }

                    line = *end != 0 ? end + 1 : end;
                }
            }
        }

        bool Open(const char* url)
        {
            if (g_open || url == nullptr || url[0] == 0)
            {
                return false;
            }

            wchar_t base[480] = {};

            if (MultiByteToWideChar(CP_UTF8, 0, url, -1, base, ARRAYSIZE(base)) == 0)
            {
                return false;
            }

            // Whatever the setting ends in, the list lives at /rooms.
            size_t length = wcslen(base);
            while (length > 0 && base[length - 1] == L'/')
            {
                base[--length] = 0;
            }

            swprintf_s(g_rooms, L"%ls/rooms", base);

            InitializeCriticalSection(&g_lock);
            g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            g_stop = 0;
            g_thread = g_wake != nullptr ? CreateThread(nullptr, 0, &Run, nullptr, 0, nullptr) : nullptr;

            if (g_thread == nullptr)
            {
                if (g_wake != nullptr)
                {
                    CloseHandle(g_wake);
                    g_wake = nullptr;
                }

                DeleteCriticalSection(&g_lock);
                return false;
            }

            g_open = true;
            return true;
        }

        void Close()
        {
            if (!g_open)
            {
                return;
            }

            InterlockedExchange(&g_stop, 1);
            SetEvent(g_wake);

            if (WaitForSingleObject(g_thread, STOP_PATIENCE) != WAIT_OBJECT_0)
            {
                TerminateThread(g_thread, 0);
            }

            CloseHandle(g_thread);
            CloseHandle(g_wake);
            DeleteCriticalSection(&g_lock);

            g_thread = nullptr;
            g_wake = nullptr;
            g_open = false;
            g_entryCount = 0;
        }

        void Announce(uint16_t port, const char* name, const char* palace, const char* level, int players)
        {
            if (!g_open)
            {
                return;
            }

            char fields[MAX_BODY] = {};
            char number[16] = {};

            sprintf_s(number, "%u", static_cast<unsigned>(PROTOCOL_VERSION));
            AppendField(fields, sizeof(fields), "version", number);
            sprintf_s(number, "%u", static_cast<unsigned>(port));
            AppendField(fields, sizeof(fields), "port", number);
            sprintf_s(number, "%d", players);
            AppendField(fields, sizeof(fields), "players", number);
            AppendField(fields, sizeof(fields), "palace", palace);
            AppendField(fields, sizeof(fields), "level", level);
            AppendField(fields, sizeof(fields), "name", name);

            EnterCriticalSection(&g_lock);
            strcpy_s(g_announce, fields);
            g_jobs &= ~JOB_WITHDRAW;
            Queue(JOB_ANNOUNCE);
            LeaveCriticalSection(&g_lock);
        }

        void Withdraw(uint16_t port)
        {
            if (!g_open)
            {
                return;
            }

            char fields[MAX_BODY] = {};
            char number[16] = {};

            sprintf_s(number, "%u", static_cast<unsigned>(port));
            AppendField(fields, sizeof(fields), "port", number);

            EnterCriticalSection(&g_lock);
            strcpy_s(g_withdraw, fields);
            g_jobs &= ~JOB_ANNOUNCE;
            Queue(JOB_WITHDRAW);
            LeaveCriticalSection(&g_lock);
        }

        void AskList()
        {
            if (!g_open)
            {
                return;
            }

            EnterCriticalSection(&g_lock);
            Queue(JOB_LIST);
            LeaveCriticalSection(&g_lock);
        }

        int Take(Entry* out, int capacity)
        {
            if (g_open)
            {
                static char list[MAX_LIST];
                bool fresh = false;

                EnterCriticalSection(&g_lock);
                fresh = g_listFresh;
                g_listFresh = false;

                if (fresh)
                {
                    strcpy_s(list, g_list);
                }

                char failure[sizeof(g_failure)] = {};
                strcpy_s(failure, g_failure);
                g_failure[0] = 0;

                LeaveCriticalSection(&g_lock);

                if (failure[0] != 0)
                {
                    Log(AMOD_LOG_WARNING, "the room list answered %s", failure);
                }

                if (fresh)
                {
                    Parse(list);
                }
            }

            int count = 0;

            for (; out != nullptr && count < capacity && count < g_entryCount; ++count)
            {
                out[count] = g_entries[count];
            }

            return count;
        }
    }
}
