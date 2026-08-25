#if !defined( _VAULT_UNIFY_REST_SERVER_HPP )
#define _VAULT_UNIFY_REST_SERVER_HPP

/**
 * @file vault-unify-rest-server.cpp
 *
 * @author Timo Weggen
 *
 * Implementation of unification engine.
 */

#include <cpprest/http_listener.h>
#include <vault-rest-server.hpp>

namespace vault {
namespace unify {


class RestCombineProvider
    : public RestContentEmptyProvider
    , public UserEventListener
{
public:
    RestCombineProvider();
    virtual ~RestCombineProvider();

    RestCombineProvider& setRuntimeContext( RuntimeContext* rt );

    // for RestContentEmptyProvider

    virtual int handleRestGet( web::http::http_request& message, const std::string url, std::string& out_result );
    virtual int handleRestPost( web::http::http_request& message, const std::string url, web::json::value data, std::string& out_result );

    virtual void userEvent( Engine*, const std::string& );

private:
    RuntimeContext* m_rt;
    std::list<std::string> m_lsMessages;

    boost::mutex m_mutex;
    boost::condition_variable m_cond;

};


};
};

#endif

