#if !defined( _VAULT_UNIFY_XDEBUG_TCP_HPP )
#define _VAULT_UNIFY_XDEBUG_TCP_HPP 1

#include "vault-unify-xdebug.hpp"
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <boost/scoped_array.hpp>

namespace vault {
namespace unify {


class XDebugTCPClient
    : public XDebugXMLConsumer
{
public:
    XDebugTCPClient();
    ~XDebugTCPClient();

    int setXDebugContext( XDebugContext* xdebugContext );
    int setUrlDebugger( const std::string& urlDebugger );

    virtual void consumeXMLPacket( int nIOV, const IOV* pIOV );

    int connect();
    int disconnect();

protected:
    void triggerRead();
    void onConnected( const boost::system::error_code& ec );
    void onData( const boost::system::error_code& ec, std::size_t bytes_transferred );

private:
    // boost::asio::io_service m_ioService;
    // boost::asio::io_service::work m_ioServiceWork;
    std::array<char, 4096> m_readBuffer;

    std::string m_urlDebugger;

    boost::shared_ptr<boost::asio::ip::tcp::socket> m_spTcpSocket;

    XDebugContext* m_xdebugContext;
};


};
};

#endif