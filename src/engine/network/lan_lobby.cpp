#include "lan_lobby.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

#include "serialize.h"

namespace
{
    constexpr uint32_t lobbyProtocolVersion = 1;

    constexpr uint16_t lobbyDiscoveryPort = 26367; // arbitrary; LAN-only
    constexpr uint16_t lobbyDefaultTcpPort = 26368;

    enum class MsgType : uint8_t
    {
        // UDP
        Advertise = 1,

        // TCP
        Hello = 10,
        HelloAck = 11,
        Chat = 20,
        Kick = 30
    };

    std::vector<uint8_t> makePacket( MsgType type, const std::function<void( RWStreamBuf & )> & writeBody )
    {
        RWStreamBuf buf;
        buf << static_cast<uint32_t>( 0x4C4F4242 ); // 'LOBB'
        buf << lobbyProtocolVersion;
        buf << static_cast<uint8_t>( type );
        writeBody( buf );

        const auto view = buf.getRawView( buf.size() );
        return std::vector<uint8_t>( view.first, view.first + view.second );
    }

    bool parseHeader( ROStreamBuf & buf, MsgType & outType )
    {
        uint32_t magic = 0;
        uint32_t version = 0;
        uint8_t type = 0;

        buf >> magic >> version >> type;
        if ( buf.fail() ) {
            return false;
        }

        if ( magic != 0x4C4F4242 ) {
            return false;
        }

        if ( version != lobbyProtocolVersion ) {
            return false;
        }

        outType = static_cast<MsgType>( type );
        return true;
    }

    uint64_t nowMs()
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<uint64_t>( std::chrono::duration_cast<std::chrono::milliseconds>( now ).count() );
    }
}

namespace Network
{
    uint64_t LanLobbyHost::_nowMs()
    {
        return ::nowMs();
    }

    uint64_t LanLobbyHost::_randomU64()
    {
        std::random_device rd;
        std::mt19937_64 gen( rd() );
        std::uniform_int_distribution<uint64_t> dist;
        return dist( gen );
    }

    LanLobbyHost::LanLobbyHost() = default;

    bool LanLobbyHost::start( const std::string & lobbyName, const std::string & hostPlayerName, LobbyPrivacy privacy, const std::string & inviteCode )
    {
        stop();

        if ( !_subsystem.isReady() ) {
            return false;
        }

        _privacy = privacy;
        _inviteCode = inviteCode;
        _lobbyName = lobbyName;
        _hostPlayerName = hostPlayerName;
        _lobbyId = _randomU64();

        _udp = Socket( Socket::Type::UDP );
        if ( !_udp.isValid() ) {
            return false;
        }
        _udp.setReuseAddr( true );
        _udp.setBroadcast( true );
        _udp.setNonBlocking( true );
        _udp.bind( 0 );

        _tcpListen = Socket( Socket::Type::TCP );
        if ( !_tcpListen.isValid() ) {
            return false;
        }
        _tcpListen.setReuseAddr( true );
        _tcpListen.setNonBlocking( true );

        // Bind TCP port; try default and then OS-assigned if taken.
        if ( !_tcpListen.bind( lobbyDefaultTcpPort ) ) {
            if ( !_tcpListen.bind( 0 ) ) {
                return false;
            }
            _tcpPort = _tcpListen.getLocalPort();
        }
        else {
            _tcpPort = lobbyDefaultTcpPort;
        }

        if ( !_tcpListen.listen() ) {
            return false;
        }

        _lastAdvertiseMs = 0;
        _running = true;

        // Seed chat with system line.
        _chat.push_back( LobbyChatMessage{ _nowMs(), "system", "Lobby started" } );

        return true;
    }

    void LanLobbyHost::stop()
    {
        _running = false;
        _clients.clear();
        _chat.clear();
        _udp.close();
        _tcpListen.close();
        _tcpPort = 0;
        _lobbyId = 0;
        _lastAdvertiseMs = 0;
    }

    bool LanLobbyHost::isRunning() const
    {
        return _running;
    }

    uint16_t LanLobbyHost::tcpPort() const
    {
        return _tcpPort;
    }

    uint64_t LanLobbyHost::lobbyId() const
    {
        return _lobbyId;
    }

    LobbyPrivacy LanLobbyHost::privacy() const
    {
        return _privacy;
    }

    const std::string & LanLobbyHost::inviteCode() const
    {
        return _inviteCode;
    }

    void LanLobbyHost::pump()
    {
        if ( !_running ) {
            return;
        }

        _advertise();
        _acceptClients();

        for ( auto & c : _clients ) {
            _pumpClient( c );
        }

        // Remove disconnected clients.
        _clients.erase( std::remove_if( _clients.begin(), _clients.end(), []( const Client & c ) { return !c.socket.isValid(); } ), _clients.end() );
    }

    std::optional<LobbyChatMessage> LanLobbyHost::popChat()
    {
        if ( _chat.empty() ) {
            return std::nullopt;
        }

        LobbyChatMessage msg = std::move( _chat.front() );
        _chat.pop_front();
        return msg;
    }

    void LanLobbyHost::sendChatFromHost( const std::string & text )
    {
        if ( !_running ) {
            return;
        }

        LobbyChatMessage msg{ _nowMs(), _hostPlayerName.empty() ? "host" : _hostPlayerName, text };
        _chat.push_back( msg );

        auto packet = makePacket( MsgType::Chat, [&]( RWStreamBuf & buf ) {
            buf << msg.timestampMs;
            buf << std::string_view( msg.from );
            buf << std::string_view( msg.text );
        } );

        _broadcastPacket( packet );
    }

    void LanLobbyHost::_advertise()
    {
        const uint64_t now = _nowMs();
        if ( now - _lastAdvertiseMs < 1000 ) {
            return;
        }
        _lastAdvertiseMs = now;

        auto packet = makePacket( MsgType::Advertise, [&]( RWStreamBuf & buf ) {
            buf << _lobbyId;
            buf << _tcpPort;
            buf << static_cast<uint8_t>( _privacy );
            buf << std::string_view( _lobbyName );
            buf << std::string_view( _hostPlayerName );
        } );

        IpEndpoint dest{ "255.255.255.255", lobbyDiscoveryPort };
        _udp.sendTo( packet.data(), packet.size(), dest );
    }

    void LanLobbyHost::_acceptClients()
    {
        while ( true ) {
            IpEndpoint peer;
            auto accepted = _tcpListen.accept( &peer );
            if ( !accepted.has_value() ) {
                return;
            }

            Client c;
            c.socket = std::move( *accepted );
            c.socket.setNonBlocking( true );
            c.endpoint = peer;
            _clients.push_back( std::move( c ) );
        }
    }

    void LanLobbyHost::_pumpClient( Client & client )
    {
        uint8_t buf[4096];
        const int rc = client.socket.recv( buf, sizeof( buf ) );
        if ( rc < 0 ) {
            client.socket.close();
            return;
        }
        if ( rc == 0 ) {
            return;
        }

        client.rx.insert( client.rx.end(), buf, buf + rc );

        // Packets are length-prefixed (uint32) for TCP.
        while ( true ) {
            if ( client.rx.size() < sizeof( uint32_t ) ) {
                return;
            }

            uint32_t packetLen = 0;
            std::memcpy( &packetLen, client.rx.data(), sizeof( uint32_t ) );
            packetLen = le32toh( packetLen );

            if ( packetLen == 0 || packetLen > 1024 * 64 ) {
                client.socket.close();
                return;
            }

            if ( client.rx.size() < sizeof( uint32_t ) + packetLen ) {
                return;
            }

            std::vector<uint8_t> packet( client.rx.begin() + sizeof( uint32_t ), client.rx.begin() + sizeof( uint32_t ) + packetLen );
            client.rx.erase( client.rx.begin(), client.rx.begin() + sizeof( uint32_t ) + packetLen );

            ROStreamBuf s( std::move( packet ) );
            MsgType type{};
            if ( !parseHeader( s, type ) ) {
                continue;
            }

            if ( type == MsgType::Hello ) {
                uint64_t lobbyId = 0;
                std::string playerName;
                std::string invite;
                s >> lobbyId >> playerName >> invite;
                if ( s.fail() || lobbyId != _lobbyId ) {
                    client.socket.close();
                    return;
                }

                if ( _privacy == LobbyPrivacy::InviteOnly && invite != _inviteCode ) {
                    auto kick = makePacket( MsgType::Kick, [&]( RWStreamBuf & w ) { w << std::string_view( "Invalid invite code" ); } );
                    _sendPacket( client.socket, kick );
                    client.socket.close();
                    return;
                }

                client.joined = true;
                client.name = playerName;

                auto ack = makePacket( MsgType::HelloAck, [&]( RWStreamBuf & w ) {
                    w << _lobbyId;
                    w << std::string_view( _lobbyName );
                    w << std::string_view( _hostPlayerName );
                    w << static_cast<uint8_t>( _privacy );
                } );
                _sendPacket( client.socket, ack );

                _chat.push_back( LobbyChatMessage{ _nowMs(), "system", client.name + " joined" } );
                continue;
            }

            if ( type == MsgType::Chat ) {
                uint64_t ts = 0;
                std::string from;
                std::string text;
                s >> ts >> from >> text;
                if ( s.fail() || !client.joined ) {
                    continue;
                }

                _chat.push_back( LobbyChatMessage{ ts, from, text } );

                // Relay to others.
                auto relay = makePacket( MsgType::Chat, [&]( RWStreamBuf & w ) {
                    w << ts;
                    w << std::string_view( from );
                    w << std::string_view( text );
                } );
                _broadcastPacket( relay );
            }
        }
    }

    void LanLobbyHost::_broadcastPacket( const std::vector<uint8_t> & packet )
    {
        for ( auto & c : _clients ) {
            if ( c.socket.isValid() && c.joined ) {
                _sendPacket( c.socket, packet );
            }
        }
    }

    void LanLobbyHost::_sendPacket( Socket & socket, const std::vector<uint8_t> & packet )
    {
        // Length-prefix.
        RWStreamBuf framed;
        framed.putLE32( static_cast<uint32_t>( packet.size() ) );
        framed.putRaw( packet.data(), packet.size() );
        const auto view = framed.getRawView( framed.size() );
        socket.send( view.first, view.second );
    }

    LanLobbyClient::LanLobbyClient() = default;

    void LanLobbyClient::startDiscovery()
    {
        stopDiscovery();

        if ( !_subsystem.isReady() ) {
            return;
        }

        _udp = Socket( Socket::Type::UDP );
        if ( !_udp.isValid() ) {
            return;
        }

        _udp.setReuseAddr( true );
        _udp.setNonBlocking( true );
        _udp.bind( lobbyDiscoveryPort );

        _discovering = true;
    }

    void LanLobbyClient::stopDiscovery()
    {
        _discovering = false;
        _udp.close();
        _discovered.clear();
    }

    void LanLobbyClient::pumpDiscovery()
    {
        if ( !_discovering ) {
            return;
        }

        _pumpUdp();
    }

    std::vector<LobbyHostInfo> LanLobbyClient::drainDiscovered()
    {
        std::vector<LobbyHostInfo> out;
        while ( !_discovered.empty() ) {
            out.push_back( std::move( _discovered.front() ) );
            _discovered.pop_front();
        }
        return out;
    }

    bool LanLobbyClient::connectToHost( const LobbyHostInfo & host, const std::string & playerName, const std::string & inviteCode )
    {
        disconnect();

        if ( !_subsystem.isReady() ) {
            return false;
        }

        _playerName = playerName;
        _inviteCode = inviteCode;

        _tcp = Socket( Socket::Type::TCP );
        if ( !_tcp.isValid() ) {
            return false;
        }

        _tcp.setNonBlocking( true );

        IpEndpoint ep = host.endpoint;
        ep.port = host.tcpPort;

        if ( !_tcp.connect( ep ) ) {
            disconnect();
            return false;
        }

        _connected = true;

        auto hello = makePacket( MsgType::Hello, [&]( RWStreamBuf & buf ) {
            buf << host.lobbyId;
            buf << std::string_view( _playerName );
            buf << std::string_view( _inviteCode );
        } );
        _sendPacket( _tcp, hello );

        return true;
    }

    void LanLobbyClient::disconnect()
    {
        _connected = false;
        _tcp.close();
        _rx.clear();
        _chat.clear();
    }

    bool LanLobbyClient::isConnected() const
    {
        return _connected && _tcp.isValid();
    }

    void LanLobbyClient::pumpConnection()
    {
        if ( !isConnected() ) {
            return;
        }

        _pumpTcp();
    }

    void LanLobbyClient::sendChat( const std::string & text )
    {
        if ( !isConnected() ) {
            return;
        }

        LobbyChatMessage msg{ _nowMs(), _playerName.empty() ? "player" : _playerName, text };

        auto packet = makePacket( MsgType::Chat, [&]( RWStreamBuf & buf ) {
            buf << msg.timestampMs;
            buf << std::string_view( msg.from );
            buf << std::string_view( msg.text );
        } );
        _sendPacket( _tcp, packet );

        _chat.push_back( msg );
    }

    std::optional<LobbyChatMessage> LanLobbyClient::popChat()
    {
        if ( _chat.empty() ) {
            return std::nullopt;
        }

        LobbyChatMessage msg = std::move( _chat.front() );
        _chat.pop_front();
        return msg;
    }

    void LanLobbyClient::_pumpUdp()
    {
        uint8_t buf[4096];
        IpEndpoint peer;

        while ( true ) {
            const int rc = _udp.recvFrom( buf, sizeof( buf ), &peer );
            if ( rc <= 0 ) {
                return;
            }

            std::vector<uint8_t> packet( buf, buf + rc );
            ROStreamBuf s( std::move( packet ) );
            MsgType type{};
            if ( !parseHeader( s, type ) ) {
                continue;
            }

            if ( type != MsgType::Advertise ) {
                continue;
            }

            LobbyHostInfo info;
            uint16_t port = 0;
            uint8_t privacy = 0;
            s >> info.lobbyId >> port >> privacy >> info.lobbyName >> info.hostPlayerName;
            if ( s.fail() ) {
                continue;
            }

            info.tcpPort = port;
            info.privacy = static_cast<LobbyPrivacy>( privacy );
            info.protocolVersion = lobbyProtocolVersion;
            info.endpoint = peer;
            info.endpoint.port = port;

            _discovered.push_back( std::move( info ) );
        }
    }

    void LanLobbyClient::_pumpTcp()
    {
        uint8_t buf[4096];
        const int rc = _tcp.recv( buf, sizeof( buf ) );
        if ( rc < 0 ) {
            disconnect();
            return;
        }
        if ( rc == 0 ) {
            return;
        }

        _rx.insert( _rx.end(), buf, buf + rc );

        while ( true ) {
            if ( _rx.size() < sizeof( uint32_t ) ) {
                return;
            }

            uint32_t packetLen = 0;
            std::memcpy( &packetLen, _rx.data(), sizeof( uint32_t ) );
            packetLen = le32toh( packetLen );

            if ( packetLen == 0 || packetLen > 1024 * 64 ) {
                disconnect();
                return;
            }

            if ( _rx.size() < sizeof( uint32_t ) + packetLen ) {
                return;
            }

            std::vector<uint8_t> packet( _rx.begin() + sizeof( uint32_t ), _rx.begin() + sizeof( uint32_t ) + packetLen );
            _rx.erase( _rx.begin(), _rx.begin() + sizeof( uint32_t ) + packetLen );

            ROStreamBuf s( std::move( packet ) );
            MsgType type{};
            if ( !parseHeader( s, type ) ) {
                continue;
            }

            if ( type == MsgType::Chat ) {
                LobbyChatMessage msg;
                s >> msg.timestampMs >> msg.from >> msg.text;
                if ( !s.fail() ) {
                    _chat.push_back( std::move( msg ) );
                }
            }
            else if ( type == MsgType::Kick ) {
                std::string reason;
                s >> reason;
                _chat.push_back( LobbyChatMessage{ _nowMs(), "system", "Kicked: " + reason } );
                disconnect();
                return;
            }
        }
    }

    void LanLobbyClient::_sendPacket( Socket & socket, const std::vector<uint8_t> & packet )
    {
        RWStreamBuf framed;
        framed.putLE32( static_cast<uint32_t>( packet.size() ) );
        framed.putRaw( packet.data(), packet.size() );
        const auto view = framed.getRawView( framed.size() );
        socket.send( view.first, view.second );
    }

    uint64_t LanLobbyClient::_nowMs()
    {
        return ::nowMs();
    }
}
