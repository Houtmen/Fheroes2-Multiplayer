#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "socket.h"

namespace Network
{
    enum class LobbyPrivacy : uint8_t
    {
        Open = 0,
        InviteOnly = 1
    };

    struct LobbyHostInfo
    {
        std::string lobbyName;
        std::string hostPlayerName;
        LobbyPrivacy privacy{ LobbyPrivacy::Open };
        uint16_t tcpPort{ 0 };

        IpEndpoint endpoint; // populated from discovery packet (sender IP + tcpPort)
        uint64_t lobbyId{ 0 }; // random id
        uint32_t protocolVersion{ 1 };
    };

    struct LobbyChatMessage
    {
        uint64_t timestampMs{ 0 };
        std::string from;
        std::string text;
    };

    // Host-side lobby (LAN only): advertises via UDP broadcast and accepts TCP clients.
    class LanLobbyHost
    {
    public:
        LanLobbyHost();

        bool start( const std::string & lobbyName, const std::string & hostPlayerName, LobbyPrivacy privacy, const std::string & inviteCode );
        void stop();
        bool isRunning() const;

        uint16_t tcpPort() const;
        uint64_t lobbyId() const;
        LobbyPrivacy privacy() const;
        const std::string & inviteCode() const;

        void pump(); // call periodically from main loop

        // Messages to show in host UI (includes host and clients).
        std::optional<LobbyChatMessage> popChat();

        // Add a message from host; broadcast to all connected clients.
        void sendChatFromHost( const std::string & text );

    private:
        struct Client
        {
            Socket socket{ Socket::Type::TCP };
            IpEndpoint endpoint;
            std::string name;
            bool joined{ false };
            std::vector<uint8_t> rx;
        };

        bool _running{ false };
        SocketSubsystem _subsystem;

        Socket _udp{ Socket::Type::UDP };
        Socket _tcpListen{ Socket::Type::TCP };

        uint16_t _tcpPort{ 0 };
        uint64_t _lobbyId{ 0 };
        LobbyPrivacy _privacy{ LobbyPrivacy::Open };
        std::string _inviteCode;
        std::string _lobbyName;
        std::string _hostPlayerName;

        uint64_t _lastAdvertiseMs{ 0 };
        std::deque<LobbyChatMessage> _chat;
        std::vector<Client> _clients;

        void _advertise();
        void _acceptClients();
        void _pumpClient( Client & client );

        void _broadcastPacket( const std::vector<uint8_t> & packet );
        void _sendPacket( Socket & socket, const std::vector<uint8_t> & packet );

        static uint64_t _nowMs();
        static uint64_t _randomU64();
    };

    // Client-side discovery + lobby connection.
    class LanLobbyClient
    {
    public:
        LanLobbyClient();

        void startDiscovery();
        void stopDiscovery();
        void pumpDiscovery();

        std::vector<LobbyHostInfo> drainDiscovered();

        bool connectToHost( const LobbyHostInfo & host, const std::string & playerName, const std::string & inviteCode );
        void disconnect();
        bool isConnected() const;

        void pumpConnection();

        void sendChat( const std::string & text );
        std::optional<LobbyChatMessage> popChat();

    private:
        SocketSubsystem _subsystem;

        Socket _udp{ Socket::Type::UDP };
        bool _discovering{ false };
        std::deque<LobbyHostInfo> _discovered;

        Socket _tcp{ Socket::Type::TCP };
        bool _connected{ false };
        std::vector<uint8_t> _rx;
        std::deque<LobbyChatMessage> _chat;

        std::string _playerName;
        std::string _inviteCode;

        void _pumpUdp();
        void _pumpTcp();

        void _sendPacket( Socket & socket, const std::vector<uint8_t> & packet );

        static uint64_t _nowMs();
    };
}
