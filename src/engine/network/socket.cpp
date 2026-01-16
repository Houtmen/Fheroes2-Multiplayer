#include "socket.h"

#include <cstring>

#include "logging.h"

#ifdef _WIN32
    #define NOMINMAX
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")

    namespace
    {
        using NativeSocket = SOCKET;
        constexpr NativeSocket invalidSocket = INVALID_SOCKET;

        NativeSocket toNative( intptr_t h )
        {
            return static_cast<NativeSocket>( h );
        }

        intptr_t fromNative( NativeSocket s )
        {
            return static_cast<intptr_t>( s );
        }

        int lastSocketError()
        {
            return WSAGetLastError();
        }

        bool wouldBlock( int err )
        {
            return err == WSAEWOULDBLOCK;
        }
    }
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>

    namespace
    {
        using NativeSocket = int;
        constexpr NativeSocket invalidSocket = -1;

        NativeSocket toNative( intptr_t h )
        {
            return static_cast<NativeSocket>( h );
        }

        intptr_t fromNative( NativeSocket s )
        {
            return static_cast<intptr_t>( s );
        }

        int lastSocketError()
        {
            return errno;
        }

        bool wouldBlock( int err )
        {
            return err == EWOULDBLOCK || err == EAGAIN;
        }

        void closesocket_posix( NativeSocket s )
        {
            ::close( s );
        }
    }
#endif

namespace
{
    sockaddr_in makeSockaddr( const Network::IpEndpoint & endpoint )
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons( endpoint.port );

#ifdef _WIN32
        InetPtonA( AF_INET, endpoint.address.c_str(), &addr.sin_addr );
#else
        inet_pton( AF_INET, endpoint.address.c_str(), &addr.sin_addr );
#endif

        return addr;
    }

    Network::IpEndpoint toEndpoint( const sockaddr_in & addr )
    {
        char buf[64]{};
#ifdef _WIN32
        InetNtopA( AF_INET, const_cast<IN_ADDR *>( &addr.sin_addr ), buf, static_cast<DWORD>( sizeof( buf ) ) );
#else
        inet_ntop( AF_INET, &addr.sin_addr, buf, sizeof( buf ) );
#endif
        return Network::IpEndpoint{ buf, ntohs( addr.sin_port ) };
    }

    int socketTypeToNative( Network::Socket::Type type )
    {
        return type == Network::Socket::Type::UDP ? SOCK_DGRAM : SOCK_STREAM;
    }

#ifdef _WIN32
    using AddrLenT = int;
#else
    using AddrLenT = socklen_t;
#endif
}

namespace Network
{
    SocketSubsystem::SocketSubsystem()
    {
#ifdef _WIN32
        WSADATA wsaData{};
        const int rc = WSAStartup( MAKEWORD( 2, 2 ), &wsaData );
        _ready = ( rc == 0 );
        if ( !_ready ) {
            ERROR_LOG( "WSAStartup failed: " << rc )
        }
#else
        _ready = true;
#endif
    }

    SocketSubsystem::~SocketSubsystem()
    {
#ifdef _WIN32
        if ( _ready ) {
            WSACleanup();
        }
#endif
    }

    Socket::Socket( Type type )
        : _type( type )
    {
        const NativeSocket s = ::socket( AF_INET, socketTypeToNative( type ), 0 );
        if ( s == invalidSocket ) {
            _handle = 0;
            DEBUG_LOG( DBG_NETWORK, DBG_WARN, "socket() failed: " << lastSocketError() )
            return;
        }

        _handle = fromNative( s );
    }

    Socket::Socket( Socket && other ) noexcept
        : _handle( other._handle )
        , _type( other._type )
    {
        other._handle = 0;
    }

    Socket & Socket::operator=( Socket && other ) noexcept
    {
        if ( this == &other ) {
            return *this;
        }

        close();
        _handle = other._handle;
        _type = other._type;
        other._handle = 0;

        return *this;
    }

    Socket::~Socket()
    {
        close();
    }

    bool Socket::isValid() const
    {
        return _handle != 0;
    }

    void Socket::close()
    {
        if ( !_handle ) {
            return;
        }

        const NativeSocket s = toNative( _handle );
#ifdef _WIN32
        ::closesocket( s );
#else
        closesocket_posix( s );
#endif
        _handle = 0;
    }

    bool Socket::setNonBlocking( bool enable )
    {
        if ( !_handle ) {
            return false;
        }

#ifdef _WIN32
        u_long mode = enable ? 1 : 0;
        const int rc = ioctlsocket( toNative( _handle ), FIONBIO, &mode );
        return rc == 0;
#else
        const int s = toNative( _handle );
        const int flags = fcntl( s, F_GETFL, 0 );
        if ( flags < 0 ) {
            return false;
        }
        const int rc = fcntl( s, F_SETFL, enable ? ( flags | O_NONBLOCK ) : ( flags & ~O_NONBLOCK ) );
        return rc == 0;
#endif
    }

    bool Socket::setReuseAddr( bool enable )
    {
        if ( !_handle ) {
            return false;
        }

        int opt = enable ? 1 : 0;
        const int rc = setsockopt( toNative( _handle ), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>( &opt ), sizeof( opt ) );
        return rc == 0;
    }

    bool Socket::setBroadcast( bool enable )
    {
        if ( !_handle ) {
            return false;
        }

        int opt = enable ? 1 : 0;
        const int rc = setsockopt( toNative( _handle ), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>( &opt ), sizeof( opt ) );
        return rc == 0;
    }

    bool Socket::bind( uint16_t port, std::string_view address )
    {
        if ( !_handle ) {
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons( port );
#ifdef _WIN32
        InetPtonA( AF_INET, std::string( address ).c_str(), &addr.sin_addr );
#else
        inet_pton( AF_INET, std::string( address ).c_str(), &addr.sin_addr );
#endif

        const int rc = ::bind( toNative( _handle ), reinterpret_cast<sockaddr *>( &addr ), sizeof( addr ) );
        return rc == 0;
    }

    bool Socket::listen( int backlog )
    {
        if ( !_handle || _type != Type::TCP ) {
            return false;
        }

        return ::listen( toNative( _handle ), backlog ) == 0;
    }

    std::optional<Socket> Socket::accept( IpEndpoint * outPeer )
    {
        if ( !_handle || _type != Type::TCP ) {
            return std::nullopt;
        }

        sockaddr_in addr{};
        AddrLenT len = sizeof( addr );
        const NativeSocket c = ::accept( toNative( _handle ), reinterpret_cast<sockaddr *>( &addr ), &len );
        if ( c == invalidSocket ) {
            const int err = lastSocketError();
            if ( wouldBlock( err ) ) {
                return std::nullopt;
            }
            DEBUG_LOG( DBG_NETWORK, DBG_WARN, "accept() failed: " << err )
            return std::nullopt;
        }

        if ( outPeer ) {
            *outPeer = toEndpoint( addr );
        }

        Socket s;
        s._type = Type::TCP;
        s._handle = fromNative( c );

        return s;
    }

    bool Socket::connect( const IpEndpoint & endpoint )
    {
        if ( !_handle || _type != Type::TCP ) {
            return false;
        }

        const sockaddr_in addr = makeSockaddr( endpoint );
        const int rc = ::connect( toNative( _handle ), reinterpret_cast<const sockaddr *>( &addr ), sizeof( addr ) );
        if ( rc == 0 ) {
            return true;
        }

        const int err = lastSocketError();
#ifdef _WIN32
        if ( err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY ) {
            return true;
        }
#else
        if ( err == EINPROGRESS || err == EALREADY ) {
            return true;
        }
#endif

        DEBUG_LOG( DBG_NETWORK, DBG_WARN, "connect() failed: " << err )
        return false;
    }

    int Socket::send( const void * data, size_t size )
    {
        if ( !_handle ) {
            return -1;
        }

        const int rc = ::send( toNative( _handle ), static_cast<const char *>( data ), static_cast<int>( size ), 0 );
        if ( rc < 0 ) {
            const int err = lastSocketError();
            if ( wouldBlock( err ) ) {
                return 0;
            }
            return -1;
        }

        return rc;
    }

    int Socket::recv( void * data, size_t size )
    {
        if ( !_handle ) {
            return -1;
        }

        const int rc = ::recv( toNative( _handle ), static_cast<char *>( data ), static_cast<int>( size ), 0 );
        if ( rc < 0 ) {
            const int err = lastSocketError();
            if ( wouldBlock( err ) ) {
                return 0;
            }
            return -1;
        }

        return rc;
    }

    int Socket::sendTo( const void * data, size_t size, const IpEndpoint & endpoint )
    {
        if ( !_handle || _type != Type::UDP ) {
            return -1;
        }

        const sockaddr_in addr = makeSockaddr( endpoint );
        const int rc = ::sendto( toNative( _handle ), static_cast<const char *>( data ), static_cast<int>( size ), 0, reinterpret_cast<const sockaddr *>( &addr ), sizeof( addr ) );
        if ( rc < 0 ) {
            const int err = lastSocketError();
            if ( wouldBlock( err ) ) {
                return 0;
            }
            return -1;
        }

        return rc;
    }

    int Socket::recvFrom( void * data, size_t size, IpEndpoint * outPeer )
    {
        if ( !_handle || _type != Type::UDP ) {
            return -1;
        }

        sockaddr_in addr{};
        AddrLenT len = sizeof( addr );
        const int rc = ::recvfrom( toNative( _handle ), static_cast<char *>( data ), static_cast<int>( size ), 0, reinterpret_cast<sockaddr *>( &addr ), &len );
        if ( rc < 0 ) {
            const int err = lastSocketError();
            if ( wouldBlock( err ) ) {
                return 0;
            }
            return -1;
        }

        if ( outPeer ) {
            *outPeer = toEndpoint( addr );
        }

        return rc;
    }

    intptr_t Socket::nativeHandle() const
    {
        if ( !_handle ) {
            return -1;
        }

        return _handle;
    }

    uint16_t Socket::getLocalPort() const
    {
        if ( !_handle ) {
            return 0;
        }

        sockaddr_in addr{};
        AddrLenT len = sizeof( addr );
        if ( ::getsockname( toNative( _handle ), reinterpret_cast<sockaddr *>( &addr ), &len ) != 0 ) {
            return 0;
        }

        if ( addr.sin_family != AF_INET ) {
            return 0;
        }

        return ntohs( addr.sin_port );
    }
}
