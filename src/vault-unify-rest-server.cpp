

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <boost/config/warning_disable.hpp>

#include <vault-unification.hpp>

#include <vault-unify-solvejob.hpp>

#include <vault-unify-rest-server.hpp>

namespace vault {
namespace unify {


/**
 * The combine engine sent out an event. We need to push it to the client.
 */
void RestCombineProvider::userEvent( Engine* pEngine, const std::string& strUserEvent )
{
    VAULT_UNIFY_DI( SERVER, "Called.\n" );
    {
        Guard g( m_mutex );
        m_lsMessages.push_back( strUserEvent );
        m_cond.notify_one();
    }
}


int RestCombineProvider::handleRestPost(
    web::http::http_request& message, 
    const std::string url,
    web::json::value jsonValue,
    std::string& strResult )
{
    // Extract possible JSON parameters.
    // VAULT_UNIFY_DI( SERVER, "Received json value '%s'.\n", jsonValue.serialize().c_str() );
    std::string strGoal = jsonValue.at( U( "goal" ) ).as_string();

    VAULT_UNIFY_DI( SERVER, "Asked to solve goal '%s'.\n", strGoal.c_str() );
    std::string::const_iterator itLine = strGoal.begin();
    std::string::const_iterator itLineEnd = strGoal.end();


    int res = m_rt->parseExecuteSegment(
            itLine, itLineEnd,
            [ this, message ](
                boost::shared_ptr<vault::unify::Job> spJob ) mutable
            -> void {

            // We run asynchronnously, so we need another string.
            std::string strResult;

            int httpResult = 200;

            boost::shared_ptr<vault::unify::SolveJob> spSolveJob = boost::dynamic_pointer_cast<vault::unify::SolveJob>( spJob );
            if( !spSolveJob ) {
                httpResult = 500;
                (void) httpResult;
                // TXWTODO: Reply with error.
                return;
            }

            strResult = "{";

            vault::unify::SolveJob::SolutionListPtr spSolutionList = spSolveJob->getSolutionList();
            vault::unify::SolveJob::SolutionList::const_iterator itSol, itSolEnd = spSolutionList->end();
            bool isFirstSol = true;
            for( itSol=spSolutionList->begin(); itSol != itSolEnd; ++itSol ) {
                if( !isFirstSol ) {
                    strResult += ", {";
                } else {
                    strResult += " {";
                    isFirstSol = false;
                }
                vault::unify::SolveJob::SolutionMapPtr spSolutionMap = *itSol;
                vault::unify::SolveJob::SolutionMap::const_iterator it, itEnd = spSolutionMap->end();
                bool isFirstVar = true;
                for( it=spSolutionMap->begin(); it != itEnd; ++it ) {
                    if( !isFirstVar ) {
                        strResult += ", \"";
                    } else {
                        strResult += " \"";
                        isFirstVar = false;
                    }
                    strResult += it->first;
                    strResult += "\": \"";
                    strResult += it->second;
                    strResult += "\"";
                }
                strResult += " }";
            }
            strResult += " }";

            web::http::http_response response( web::http::status_codes::OK );
            std::string strOrigin;
            if( message.headers().has( "Origin" ) ) {
                strOrigin = message.headers()["Origin"];
            }
            response.set_body( strResult );
            response.headers().add( "Access-Control-Allow-Origin", strOrigin );
            message.reply( response );

            return;
        });
    
    int syncHttpResult;
    if( res<0 ) {
        // TXWTODO: Find a better code for parsing/execution problems.
        syncHttpResult = 500;
    } else {
        // If everything was ok, we signal that we will reply for ourselves in the
        // future.
        syncHttpResult = 0;
    }
    return syncHttpResult;
}


int RestCombineProvider::handleRestGet( 
    web::http::http_request& message, 
    const std::string url,
    std::string& strResult )
{
    if( url == "/combine/events" ) {
        // Collect events.
        {
            Guard g( m_mutex );

            if( m_lsMessages.empty() ) {
                // Let get event requests last 15 seconds.
                // TXWTODO: Config.
                m_cond.timed_wait( g, boost::posix_time::milliseconds(15000) );
            }
        }

        strResult = "{ \"userEvents\": [ ";
        int nResults = 0;
        {
            Guard g( m_mutex );
            std::list<std::string>::iterator it, itEnd = m_lsMessages.end();
            bool first = true;
            for( it=m_lsMessages.begin(); it != itEnd; ++it ) {
                ++nResults;
                if( !first ) {
                    strResult += ", ";
                } else {
                    //strResult += "";
                    first = false;
                }
                strResult += *it;
                //strResult += "\"";
            }
            strResult += " ], \"nUserEvents\": \"";
            strResult += boost::lexical_cast<std::string>( nResults );
            strResult += "\" }";
            m_lsMessages.clear();
            VAULT_UNIFY_DI( SERVER, "Replying with content: \n%s\n", strResult.c_str() );
        }
        return 200;
    } else {
        return 404;
    }
}


RestCombineProvider& RestCombineProvider::setRuntimeContext( RuntimeContext* rt )
{
    if( !m_rt ) {
        m_rt = rt;
        rt->getEngine()->addUserEventListener( this );
    }
    return *this;
}


RestCombineProvider::~RestCombineProvider()
{
    // TXWTODO: This is a race condition. A callback might in the porcess
    // of running or already be scheduled while this function is called.
    m_rt->getEngine()->removeUserEventListener( this );
}


RestCombineProvider::RestCombineProvider()
    : RestContentEmptyProvider( "/combine/events" )
    , m_rt( NULL )
{
}


};
};