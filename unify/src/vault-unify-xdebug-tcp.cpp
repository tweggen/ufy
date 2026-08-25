

#include <string>
#include <list>
#include <ctype.h>
#include <string.h>

#include <vault/vault.hpp>
#include "vault-unify-xdebug-tcp.hpp"
#include <vault-unify.hpp>

namespace vault {
namespace unify {


void XDebugTCPClient::triggerRead()
{
    // Continue reading remaining data until EOF.
    boost::asio::async_read( *m_spTcpSocket.get(), boost::asio::buffer( m_readBuffer ),
        boost::asio::transfer_at_least( 1 ),
        boost::bind( &XDebugTCPClient::onData, this,
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred ) );
}


void XDebugTCPClient::onData( const boost::system::error_code& ec, std::size_t nBytes )
{
    if( !ec ) {
        const char* data = static_cast<const char*>( m_readBuffer.data() );
        const char* end = data + nBytes;
        m_xdebugContext->consumeInput( data, end );
        triggerRead();
    } else {
        VAULT_UNIFY_DI( SERVER, "Connecting to server failed. Error Code = %s.\n",
                "error" /* Tools::getErrorMessage( ec ).c_str() */ );
        // TXWTODO: Close connection
    }
}


void XDebugTCPClient::onConnected( const boost::system::error_code& ec )
{
    if ( !ec ) {
        m_xdebugContext->onConnected();
        triggerRead();
    } else {
        // An error occurred connecting to the server.
        // We will retry to connect after some time. */
        VAULT_UNIFY_DI( SERVER, "Connecting to server failed. Error Code = %s.\n",
                "error" /* Tools::getErrorMessage( ec ).c_str() */ );

        //closeNoLock();
        
        //retry();
    }
}

int XDebugTCPClient::connect()
{
    m_spTcpSocket.reset(
        new boost::asio::ip::tcp::socket(
            vault::BoostAsioIoService::get() ) );

    if( !m_urlDebugger.length() ) {
        VAULT_UNIFY_DI( SERVER, "No server url specified.\n" );
        return -ENOENT;
    }

    if( !m_spTcpSocket ) {
        VAULT_UNIFY_DI( SERVER, "Internal error: No tcp socket defined.\n" );
        return -ENOENT;
    }

    /*
     * ip::address::from_string() was deprecated in Boost 1.66 in favour of
     * ip::make_address() and removed in 1.87; the legacy Boost.Jam build
     * still compiles against a pre-1.66 Boost, so pick by version.
     */
    boost::asio::ip::tcp::endpoint *endpoint =
         new boost::asio::ip::tcp::endpoint(
#if BOOST_VERSION >= 106600
             boost::asio::ip::make_address( m_urlDebugger ),
#else
             boost::asio::ip::address::from_string( m_urlDebugger ),
#endif
             9000 );
    try {
        m_spTcpSocket->async_connect( *endpoint,
            boost::bind( &XDebugTCPClient::onConnected, this,
            boost::asio::placeholders::error ) );
    } catch( boost::exception& e ) {
        VAULT_UNIFY_DI( SERVER,
            "Caught exception while connecting socket: %s\n",
            boost::diagnostic_information( e ).c_str() );
    }
    return -ENOSYS;
}

void XDebugTCPClient::consumeXMLPacket( int nIOV, const IOV* pIOV )
{
    boost::system::error_code ec;

    std::vector<boost::asio::const_buffer> buffers;

    for( int i=0; i<nIOV; ++i ) {
        VAULT_UNIFY_DI( SERVER, "Writing \"%s\".\n", 
            std::string( (const char*) pIOV[i].data, (size_t) pIOV[i].length ).c_str() );
        buffers.push_back(
            boost::asio::const_buffer(
                pIOV[i].data, pIOV[i].length ) );
    }

    (void) /* bytesWritten = */ boost::asio::write(
        *m_spTcpSocket, buffers, ec );

    (void) ec;
}

int XDebugTCPClient::disconnect()
{
    return -ENOSYS;
}

int XDebugTCPClient::setXDebugContext( XDebugContext* xdebugContext )
{
    m_xdebugContext = xdebugContext;
    return 0;
}

int XDebugTCPClient::setUrlDebugger( const std::string& urlDebugger )
{
    m_urlDebugger = urlDebugger;
    return 0;
}


XDebugTCPClient::~XDebugTCPClient()
{
}

XDebugTCPClient::XDebugTCPClient()
    : m_xdebugContext( NULL )
{
}

};
};
