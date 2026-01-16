#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Network
{
    struct IpEndpoint
    {
        std::string address; // IPv4 string
        uint16_t port{ 0 };
    };

    // Minimal cross-platform socket wrapper (IPv4 only for now).
    class Socket
    {
    public:
        enum class Type
        {
            UDP,
            TCP
        };

        Socket() = default;
        explicit Socket( Type type );

        Socket( const Socket & ) = delete;
        Socket & operator=( const Socket & ) = delete;

        Socket( Socket && other ) noexcept;
        Socket & operator=( Socket && other ) noexcept;

        ~Socket();

        bool isValid() const;
        void close();

        bool setNonBlocking( bool enable );
        bool setReuseAddr( bool enable );
        bool setBroadcast( bool enable );

        bool bind( uint16_t port, std::string_view address = "0.0.0.0" );
        bool listen( int backlog = 8 );
        std::optional<Socket> accept( IpEndpoint * outPeer = nullptr );
        bool connect( const IpEndpoint & endpoint );

        // Returns number of bytes sent/received; 0 means would-block/closed depending on context.
        int send( const void * data, size_t size );
        int recv( void * data, size_t size );

        int sendTo( const void * data, size_t size, const IpEndpoint & endpoint );
        int recvFrom( void * data, size_t size, IpEndpoint * outPeer );

        intptr_t nativeHandle() const;

        // Returns bound local port for IPv4 sockets, or 0 on failure.
        uint16_t getLocalPort() const;

    private:
        intptr_t _handle{ 0 }; // SOCKET on Win, int on POSIX.
        Type _type{ Type::UDP };
    };

    // Windows requires explicit init/cleanup of Winsock.
    class SocketSubsystem
    {
    public:
        SocketSubsystem();
        SocketSubsystem( const SocketSubsystem & ) = delete;
        SocketSubsystem & operator=( const SocketSubsystem & ) = delete;
        ~SocketSubsystem();

        bool isReady() const
        {
            return _ready;
        }

    private:
        bool _ready{ false };
    };
}
