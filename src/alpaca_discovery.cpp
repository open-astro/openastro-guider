/*
 *  alpaca_discovery.cpp
 *  PHD Guiding
 *
 *  Copyright (c) 2026 PHD2 Developers
 *  All rights reserved.
 *
 *  This source code is distributed under the following "BSD" license
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *    Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *    Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *    Neither the name of Craig Stark, Stark Labs nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "alpaca_discovery.h"
#include "json_parser.h"
#include <set>
#include <map>
#include <vector>
#include <cstring>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// Alpaca discovery protocol constants
static const unsigned int ALPACA_DISCOVERY_PORT = 32227;
static const char *ALPACA_DISCOVERY_MESSAGE = "alpacadiscovery1";

static wxString AddrToString(const in_addr& addr)
{
    char buf[INET_ADDRSTRLEN] = { 0 };
    inet_ntop(AF_INET, &addr, buf, INET_ADDRSTRLEN);
    return wxString(buf, wxConvUTF8);
}

static std::vector<sockaddr_in> BuildBroadcastTargets()
{
    std::vector<sockaddr_in> targets;

    auto addTarget = [&](uint32_t addr)
    {
        sockaddr_in target {};
        target.sin_family = AF_INET;
        target.sin_port = htons(ALPACA_DISCOVERY_PORT);
        target.sin_addr.s_addr = addr;
        for (const auto& existing : targets)
        {
            if (existing.sin_addr.s_addr == addr)
                return;
        }
        targets.push_back(target);
        Debug.Write(wxString::Format("AlpacaDiscovery: Added discovery target %s\n", AddrToString(target.sin_addr)));
    };

    // Unicast to loopback so we discover Alpaca servers bound only to 127.0.0.1
    // (e.g. ASCOM Remote Server's default "Loopback" IP setting). Subnet
    // broadcasts do not reach loopback-only listeners.
    addTarget(htonl(INADDR_LOOPBACK));

    addTarget(INADDR_BROADCAST);

    struct ifaddrs *ifap = nullptr;
    if (getifaddrs(&ifap) == 0)
    {
        for (auto ifa = ifap; ifa; ifa = ifa->ifa_next)
        {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if (!(ifa->ifa_flags & IFF_UP))
                continue;
            if (ifa->ifa_flags & IFF_LOOPBACK)
                continue;
            if (!ifa->ifa_netmask)
                continue;

            uint32_t hostAddr = ntohl(reinterpret_cast<sockaddr_in *>(ifa->ifa_addr)->sin_addr.s_addr);
            uint32_t mask = ntohl(reinterpret_cast<sockaddr_in *>(ifa->ifa_netmask)->sin_addr.s_addr);
            uint32_t broadcast = (hostAddr & mask) | (~mask);
            addTarget(htonl(broadcast));
        }
        freeifaddrs(ifap);
    }

    return targets;
}

wxArrayString AlpacaDiscovery::DiscoverServers(int numQueries, int timeoutSeconds)
{
    Debug.Write(wxString::Format("AlpacaDiscovery: DiscoverServers entry queries=%d timeout=%d\n", numQueries, timeoutSeconds));
    wxArrayString serverList;
    DiscoverServers(serverList, numQueries, timeoutSeconds);
    Debug.Write(
        wxString::Format("AlpacaDiscovery: DiscoverServers exit count=%u\n", static_cast<unsigned int>(serverList.GetCount())));
    return serverList;
}

void AlpacaDiscovery::DiscoverServers(wxArrayString& serverList, int numQueries, int timeoutSeconds)
{
    serverList.Clear();

    // Use a single socket for both sending and receiving
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        Debug.Write("AlpacaDiscovery: Failed to create socket\n");
        Debug.Write(wxString::Format("AlpacaDiscovery: socket() failed: %s\n", strerror(errno)));
        return;
    }
    Debug.Write(wxString::Format("AlpacaDiscovery: socket created (%d)\n", sock));

    // Set socket receive timeout
    // Unix uses struct timeval (seconds + microseconds)
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000; // 100ms timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *) &timeout, sizeof(timeout));

    // Enable SO_REUSEADDR to allow port reuse
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *) &reuse, sizeof(reuse));
    Debug.Write("AlpacaDiscovery: set SO_REUSEADDR\n");

    // Enable broadcast
    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char *) &broadcast, sizeof(broadcast));
    Debug.Write("AlpacaDiscovery: set SO_BROADCAST\n");

    // Bind to any available port
    struct sockaddr_in localAddr;
    memset(&localAddr, 0, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = 0; // Let system choose port

    if (bind(sock, (struct sockaddr *) &localAddr, sizeof(localAddr)) < 0)
    {
        Debug.Write(wxString::Format("AlpacaDiscovery: Failed to bind socket: %s\n", strerror(errno)));
        close(sock);
        return;
    }

    // Get the port we bound to (for debugging)
    socklen_t addrLen = sizeof(localAddr);
    getsockname(sock, (struct sockaddr *) &localAddr, &addrLen);
    Debug.Write(wxString::Format("AlpacaDiscovery: Socket bound to port %d\n", ntohs(localAddr.sin_port)));

    auto broadcastTargets = BuildBroadcastTargets();

    Debug.Write(wxString::Format("AlpacaDiscovery: Starting discovery - %zu broadcast target(s)\n", broadcastTargets.size()));

    // Set to store unique servers (host:port)
    std::set<wxString> uniqueServers;

    // Send discovery queries
    for (int query = 0; query < numQueries; query++)
    {
        // Send discovery message
        const char *msg = ALPACA_DISCOVERY_MESSAGE;
        size_t msgLen = strlen(msg);

        for (const auto& targetAddr : broadcastTargets)
        {
            Debug.Write(wxString::Format("AlpacaDiscovery: Sending query %d: '%s' (%d bytes) to %s:%d\n", query + 1, msg,
                                         (int) msgLen, AddrToString(targetAddr.sin_addr), ntohs(targetAddr.sin_port)));

            int sent = sendto(sock, msg, (int) msgLen, 0, (const struct sockaddr *) &targetAddr, sizeof(targetAddr));
            if (sent < 0)
            {
                Debug.Write(
                    wxString::Format("AlpacaDiscovery: Error sending discovery query %d: %s\n", query + 1, strerror(errno)));
            }
            else
            {
                Debug.Write(
                    wxString::Format("AlpacaDiscovery: Successfully sent discovery query %d (%d bytes)\n", query + 1, sent));
            }
        }

        // Wait for responses
        wxLongLong startTime = wxGetLocalTimeMillis();
        wxLongLong timeoutMs = timeoutSeconds * 1000;

        Debug.Write(wxString::Format("AlpacaDiscovery: Waiting %d seconds for responses...\n", timeoutSeconds));

        // Continue receiving until timeout, processing all queued responses
        bool receivedAny = false;
        while (wxGetLocalTimeMillis() - startTime < timeoutMs)
        {
            char buffer[1024];
            struct sockaddr_in fromAddr;
            socklen_t fromLen = sizeof(fromAddr);

            // Try to receive data (will timeout after 100ms due to SO_RCVTIMEO)
            int received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *) &fromAddr, &fromLen);

            if (received > 0)
            {
                receivedAny = true;
                buffer[received] = '\0';

                // Get IP address from the sender (UDP from address)
                char ipStr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(fromAddr.sin_addr), ipStr, INET_ADDRSTRLEN);
                wxString ipAddress(ipStr, wxConvUTF8);
                wxString response(buffer, wxConvUTF8);

                Debug.Write(wxString::Format("AlpacaDiscovery: Received %d bytes from %s:%d\n", received, ipAddress,
                                             ntohs(fromAddr.sin_port)));
                Debug.Write(wxString::Format("AlpacaDiscovery: Response data: %s\n", response));

                // Parse JSON response - format is {"AlpacaPort": <port>}
                JsonParser parser;
                long port = 0;

                if (parser.Parse(std::string(buffer)))
                {
                    const json_value *root = parser.Root();
                    if (root && root->type == JSON_OBJECT)
                    {
                        // Look for AlpacaPort field
                        json_for_each(n, root)
                        {
                            if (n->name && strcmp(n->name, "AlpacaPort") == 0)
                            {
                                if (n->type == JSON_INT)
                                {
                                    port = n->int_value;
                                }
                                else if (n->type == JSON_FLOAT)
                                {
                                    port = static_cast<long>(n->float_value);
                                }
                                break;
                            }
                        }
                    }
                }

                // If we got a valid port (a legal TCP port, 1..65535), add the server
                if (port > 0 && port <= 65535 && !ipAddress.IsEmpty())
                {
                    wxString serverStr = wxString::Format("%s:%ld", ipAddress, port);

                    // Add to set to avoid duplicates
                    if (uniqueServers.find(serverStr) == uniqueServers.end())
                    {
                        uniqueServers.insert(serverStr);
                        serverList.Add(serverStr);
                        Debug.Write(wxString::Format("AlpacaDiscovery: Found server: %s\n", serverStr));
                    }
                    else
                    {
                        Debug.Write(wxString::Format("AlpacaDiscovery: Duplicate server ignored: %s\n", serverStr));
                    }
                }
                else
                {
                    Debug.Write(
                        wxString::Format("AlpacaDiscovery: Invalid response format or missing port from %s\n", ipAddress));
                }

                // Continue immediately to process any additional queued responses
                // Don't sleep here to avoid missing rapid responses
                continue;
            }
            else if (received < 0)
            {
                // EAGAIN/EWOULDBLOCK/EINTR are expected in non-blocking mode or with timeout
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ETIMEDOUT)
                {
                    Debug.Write(wxString::Format("AlpacaDiscovery: recvfrom error: %s\n", strerror(errno)));
                }
                // If we've received responses before, continue a bit longer in case more arrive
                // Otherwise, small delay to avoid busy waiting
                if (!receivedAny)
                {
                    wxMilliSleep(10);
                }
            }
            else
            {
                // No data received (timeout or error), small delay to avoid busy waiting
                wxMilliSleep(10);
            }
        }

        // Small delay between queries
        if (query < numQueries - 1)
        {
            wxMilliSleep(100);
        }
    }

    close(sock);

    if (serverList.GetCount() > 0)
    {
        Debug.Write(wxString::Format("AlpacaDiscovery: Discovery complete - Found %u server(s):\n",
                                     static_cast<unsigned int>(serverList.GetCount())));
        for (size_t i = 0; i < serverList.GetCount(); i++)
        {
            Debug.Write(wxString::Format("AlpacaDiscovery:   [%u] %s\n", static_cast<unsigned int>(i + 1), serverList[i]));
        }
    }
    else
    {
        Debug.Write("AlpacaDiscovery: Discovery complete - No servers found\n");
    }
}

bool AlpacaDiscovery::ParseServerString(const wxString& serverStr, wxString& host, long& port)
{
    host.Clear();
    port = 0;

    int colonPos = serverStr.Find(':');
    if (colonPos == wxNOT_FOUND)
    {
        return false;
    }

    host = serverStr.Left(colonPos);
    wxString portStr = serverStr.Mid(colonPos + 1);

    return portStr.ToLong(&port) && port > 0;
}
