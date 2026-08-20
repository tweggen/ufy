#include <string>
#include <list>
#include <ctype.h>
#include <string.h>

#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>

#include <vault-unify.hpp>
#include "vault-unify-xdebug.hpp"

// IDE:       command [SPACE] [args] -- data [NULL]
// DEBUGGER:  [NUMBER] [NULL] XML(data) [NULL]

namespace vault {
namespace unify {


XDebugContext& XDebugContext::setCommandConsumer( XDebugCommandConsumer* pCommandConsumer )
{
    m_pCommandConsumer = pCommandConsumer;
    return *this;
}


XDebugContext& XDebugContext::setXMLConsumer( XDebugXMLConsumer* pXMLConsumer )
{
    m_pXMLConsumer = pXMLConsumer;
    return *this;
}


int XDebugContext::sendXML( const char* inputStart, const char* inputEnd )
{
    if( !m_pXMLConsumer ) {
        VAULT_UNIFY_DI( SERVER, "No xml consumer defined.\n" );
        return -ENOSYS;
    }

    static std::string prelude = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
    // static std::string prelude = "<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>";

    int32_t length = inputEnd - inputStart + prelude.length();
    char sLength[14];
    snprintf( sLength, 14, "%d", (int) length );
    // followed by NUL
    // followed by payload
    // followed by NUL
    struct XDebugXMLConsumer::IOV iov[4];
    iov[0].data = sLength;
    iov[0].length = strlen( sLength )+1;
    iov[1].data = prelude.c_str();
    iov[1].length = prelude.length();
    iov[2].data = inputStart;
    iov[2].length = inputEnd-inputStart;
    iov[3].data = sLength+iov[0].length-1;
    iov[3].length = 1;
    m_pXMLConsumer->consumeXMLPacket( 4, iov );
    return 0;
}


int XDebugContext::flushCurrentInputArg()
{
    m_currInputArgs.push_back( m_currentInputArg );
    m_currentInputArg.clear();
    return 0;
}


/**
 * Flush the current input argument to the list of unparsed inputs.
 */
int XDebugContext::tryFlushCurrentInputArg()
{
    if( !m_currentInputArg.empty() ) {
        flushCurrentInputArg();
    }
    return 0;
}


int XDebugContext::executeLine( const std::list<std::string>& lsCommand )
{
    if( m_pCommandConsumer ) {
        m_pCommandConsumer->consumeXDebugCommand( lsCommand );
    }
    return 0;
}

int XDebugContext::flushCurrentLine()
{
    tryFlushCurrentInputArg();
    if( !m_currInputArgs.empty() ) {
        // We do not copy m_currInputArgs, as we are the only thread
        // Modifying it.
        executeLine( m_currInputArgs );
        m_currInputArgs.clear();
    }
    m_currentInputArg.clear();
    return 0;
}


/**
 * Parse an xdebug input string, updating the state machine.
 */
int XDebugContext::consumeInput( const char* inputStart, const char* inputEnd )
{
    VAULT_UNIFY_DI( SERVER, "Received \"%s\".\n", std::string( inputStart, inputEnd-inputStart ).c_str() );

    Guard g( m_mutex );

    while( inputStart != inputEnd ) {
        char ch = *inputStart++;
        // A NUL byte terminates the input according to definition.
        if( 0x0d==ch  || 0x0a==ch || !ch ) {
            m_parseState = EXPECT_NOTHING;

            m_mutex.unlock();
            flushCurrentLine();
            m_mutex.lock();

            m_parseState = EXPECT_SPACE_CHAR_QUOTE;
            // We could remember everything that's left by
            // m_currUNparsedInput = std::string( inputStart, inputEnd );
            // but we just want to continue,
            break;
        }
        switch( m_parseState ) {
        case EXPECT_NOTHING:
            // Nothing.
            break;
        case EXPECT_SPACE_CHAR_QUOTE:
            if( isspace( ch ) ) {

                m_mutex.unlock();
                tryFlushCurrentInputArg();
                m_mutex.lock();

                continue;   
            }
            if( '"'==ch ) {
                m_parseState = EXPECT_QUOTE_QUOTED_CHAR;
                continue;
            }
            // Everything else is a valid character.
            m_currentInputArg += ch;
            break;
        case EXPECT_SPACE_CHAR:
            if( isspace( ch ) ) {

                m_mutex.unlock();
                tryFlushCurrentInputArg();
                m_mutex.lock();

                m_parseState = EXPECT_SPACE_CHAR_QUOTE;
                continue;
            }
            // Everyhing else is a continuation of the current string.
            m_currentInputArg += ch;
            break;
        case EXPECT_QUOTE_QUOTED_CHAR:
            if( '"'==ch ) {
                m_parseState = EXPECT_SPACE_CHAR_QUOTE;

                m_mutex.unlock();
                tryFlushCurrentInputArg();
                m_mutex.lock();

                continue;
            }
            if( '\\'==ch ) {
                m_parseState = EXPECT_QUOTED_CHARACTER;
                continue;
            }
            // Everythign else is valid content of the string.
            m_currentInputArg += ch;
            break;
        case EXPECT_QUOTED_CHARACTER:
            // Whatever it is, just append it.
            m_currentInputArg += ch;
            m_parseState = EXPECT_QUOTE_QUOTED_CHAR;
            break;
        }
    }
    return 0;
}


int XDebugContext::onConnected()
{
    VAULT_UNIFY_DI( SERVER, "Connected to debugger.\n" );

    std::string strIdeKey( "COMBINE1");
    {
        char* s = getenv( "DBGP_IDEKEY" );
        if( s && *s ) {
            strIdeKey = s;
        }
    }
    std::string strCookie;
    {
        char* s = getenv( "DBGP_COOKIE" );
        if( s && *s ) {
            strCookie = s;
        }
    }

    // TXWTODO: Try to extract a meaningful thread id.
    std::string strThreadId = "32000";

    // TXWTODO: Setup with the main file that we debug.
    std::string strFilePath;
    char cwd[PATH_MAX];
    if( !getcwd( cwd, sizeof( cwd ) ) ) {
        cwd[0] = '\0';
    }
    if( '/' != *m_initialFileName.c_str() ) {
//      strFilePath = "file://localhost";
        strFilePath = "file://";
        strFilePath += cwd;
        strFilePath += "/";
        strFilePath += m_initialFileName;
    } else {
        strFilePath = "file://";
        strFilePath += m_initialFileName;
    }
    std::string strInitPacket =
        "<init xmlns=\"urn:debugger_protocol_v1\" xmlns:xdebug=\"http://xdebug.org/dbgp/xdebug\" appid=\"combineXDebug\""
            " idekey=\"" + strIdeKey + "\""
            " session=\"" + strCookie + "\""
            " thread=\"" + strThreadId + "\""
            " parent=\"combineAppId\""
            " language=\"bobtalk\""
            " protocol_version=\"1.0\""
            " fileuri=\"" + strFilePath + "\"><engine version=\"0.5.2\"><![CDATA[Xdebug]]></engine></init>";
    sendXML( 
        strInitPacket.c_str(), 
        strInitPacket.c_str() 
            + strInitPacket.length() );

    return 0;
}



void XDebugContext::sendRunResponse( std::string /*id*/, DebugListener::ExecutionStatus )
{

}


void XDebugContext::sendStandardResponse(
        std::string id,
        std::string command,
        DebugListener::ExecutionStatus executionStatus,
        const DebugLocation& location )
{
    std::string strFile;
    std::string strType;

    if( location.uriFile == "dbgp://stdin" ) {
        strType = "eval";
        strFile = "file:///tmp/ufy";
    } else {
        strType = "file";
        strFile = location.uriFile;
    }

    std::string response = str( boost::format(
        "<response"
            " xmlns=\"urn:debugger_protocol_v1\" xmlns:xdebug=\"http://xdebug.org/dbgp/xdebug\""
            " command=\"%1%\""
            " status=\"%2%\""
            " reason=\"ok\""
            " transaction_id=\"%3%\">"
            " <xdebug:message filename=\"%4%\" lineno=\"%5%\"></xdebug:message></response>"
        )
        % command
        % executionStatusString( executionStatus )
        % id
        % strFile
        % location.line )
        ;

    sendXML( response );
}


void XDebugContext::sendSuccessResponse(
        std::string id,
        std::string command,
        std::string success )
{
    std::string response = str( boost::format(
        "<response"
            " xmlns=\"urn:debugger_protocol_v1\" xmlns:xdebug=\"http://xdebug.org/dbgp/xdebug\""
            " command=\"%1%\""
            " success=\"%2%\""
            " transaction_id=\"%3%\">"
            "</response>"
        )
        % command
        % success
        % id
         )
        ;

    sendXML( response );
}


void XDebugContext::sendStack(
        std::string id,
        DebugListener::ExecutionStatus executionStatus,
        DebugListener::ChangeReason changeReason,
        const std::list<StackFrame>& lsStack )
{
    std::string response = str( boost::format(
        "<response"
            " xmlns=\"urn:debugger_protocol_v1\" xmlns:xdebug=\"http://xdebug.org/dbgp/xdebug\""
            " command=\"status\""
            " status=\"%1%\""
            " reason=\"%2%\""
            " transaction_id=\"%3%\">"
        )
        % executionStatusString( executionStatus )
        % changeReasonString( changeReason )
        % id )
        ;

#define STACK_DIRECTION 0
#if STACK_DIRECTION==1
    std::list<StackFrame>::const_iterator itEnd = lsStack.end(), it = lsStack.begin();
    int idxLevel = lsStack.size()-1;
#else
    /*
     * atom.io seems to expect the stack starting with level 0.
     */
    //
    std::list<StackFrame>::const_reverse_iterator itEnd = lsStack.rend(), it = lsStack.rbegin();
    int idxLevel = 0;
#endif

    for( ; it!=itEnd; ++it ) {
        std::string strFile = it->m_location.uriFile;
        std::string strType;
        std::string strMethod;
        if( it->m_location.uriFile == "dbgp://stdin" ) {
            strType = "eval";
            strFile = "file:///tmp/ufy";
        } else {
            strType = "file";
            strFile = it->m_location.uriFile;
        }
        strMethod=it->m_location.strMethod;
        if( 0==strMethod.length() ) {
            char s[70];
            snprintf( s, 70, "UnifyContext%lld()", (long long) it->m_id );
            strMethod = s;
        }
        response += str( boost::format(
            "<stack"
                " level=\"%1%\""
                " type=\"%2%\"" // file/eval
                " filename=\"%3%\""
                " lineno=\"%4%\""
                " where=\"%5%\"" // command name
            //cmdbegin="line_number:offset"
            //cmdend="line_number:offset"
                "/>"
            )
            % idxLevel
            % strType
            % strFile
            % it->m_location.line
            % strMethod
            );
#if STACK_DIRECTION==1
        idxLevel--;
#else
        idxLevel++;
#endif
    }

    response += "</response>";

    sendXML( response );
}


void XDebugContext::sendContext(
        std::string id,
        std::string strContextId,
        const std::list<DebugProperty>& lsProperties )
{
    std::string response = str( boost::format(
        "<response"
            " xmlns=\"urn:debugger_protocol_v1\" xmlns:xdebug=\"http://xdebug.org/dbgp/xdebug\""
            " command=\"context_get\""
            " context=\"%1%\""
            " transaction_id=\"%2%\">"
        )
        % strContextId
        % id )
        ;
    std::list<DebugProperty>::const_iterator itEnd = lsProperties.end(), it;

    for( it=lsProperties.begin(); it!=itEnd; ++it ) {
        std::string strType;

        // char s[70];
        // snprintf( s, 70, "%lld (unknown)", (long long) it->m_id );
        response += str( boost::format(
            "<property"
                " type=\"string\""
                " facet=\"\""
                " size=\"%1%\""
                " name=\"%2%\""
                " fullname=\"$%2%\""
                " children=\"false\""
                " numchildren=\"0\""
                " encoding=\"base64\""
            ">"
            )
            % (sizeof( std::string ) + it->m_value.length() )
            % it->m_name
            )
            ;

#if 0
        response += 
            "<name encoding=\"base64\">"
            + base64_encode( (uint8_t*) it->m_name.c_str(), it->m_name.length() )
            + "</name>";
        response += 
            "<fullname encoding=\"base64\">"
            + base64_encode( (uint8_t*) it->m_name.c_str(), it->m_name.length() )
            + "</fullname>";
#endif
        /*
         * In contrast to the spec, atom.io expects base64 as a child.
         */
        response += 
            //"<value encoding=\"base64\">"
            base64_encode( (uint8_t*) it->m_value.c_str(), it->m_value.length() )
            //+ "</value>";
            ;

        response += "</property>";

    }

    response += "</response>";

    sendXML( response );
}


void XDebugContext::sendStatus( 
        std::string id,
        DebugListener::ExecutionStatus executionStatus,
        DebugListener::ChangeReason changeReason,
        DebugLocation location )
{
    std::string response = str( boost::format(
        "<response"
            " xmlns=\"urn:debugger_protocol_v1\" xmlns:xdebug=\"http://xdebug.org/dbgp/xdebug\""
            " command=\"status\""
            " status=\"%1%\""
            " reason=\"%2%\""
            " transaction_id=\"%3%\">"
            " <xdebug:message filename=\"%4%\" lineno=\"%5%\"></xdebug:message></response>"
        )
        % executionStatusString( executionStatus )
        % changeReasonString( changeReason )
        % id
        % location.uriFile
        % location.line )
        ;

    sendXML( response );
}


void XDebugContext::sendFeatureGetResponse(
        std::string id,
        std::string supported,
        std::string content )
{
    std::string response = str( boost::format(
        "<response"
            " xmlns=\"urn:debugger_protocol_v1\" xmlns:xdebug=\"http://xdebug.org/dbgp/xdebug\""
            " command=\"feature_get\""
            " supported=\"%1%\""
            " transaction_id=\"%2%\">"
            "%3%</response>"
        )
        % supported
        % id
        % content )
        ;

    sendXML( response );
}


void XDebugContext::sendContextNamesResponse(
        std::string id,
        std::string command,
        std::string content )
{
    std::string response = str( boost::format(
        "<response"
            " xmlns=\"urn:debugger_protocol_v1\" xmlns:xdebug=\"http://xdebug.org/dbgp/xdebug\""
            " command=\"%1%\""
            " transaction_id=\"%2%\">"
            "%3%</response>"
        )
        % command
        % id
        % content )
        ;

    sendXML( response );
}


void XDebugContext::sendStepIntoResponse(
        std::string id,
        DebugListener::ExecutionStatus executionStatus,
        DebugLocation location )

{
    sendStandardResponse( id, "step_into", executionStatus, location );
}


void XDebugContext::sendStepOverResponse( 
        std::string id,
        DebugListener::ExecutionStatus executionStatus,
        DebugLocation location )
{
    sendStandardResponse( id, "step_over", executionStatus, location );
}


void XDebugContext::sendStepOutResponse(
        std::string id,
        DebugListener::ExecutionStatus executionStatus,
        DebugLocation location )
{
    sendStandardResponse( id, "step_out", executionStatus, location );
}


void XDebugContext::sendStopResponse(       
        std::string id,
        DebugListener::ExecutionStatus executionStatus,
        DebugLocation location )
{
    sendStandardResponse( id, "stop", executionStatus, location );
}


void XDebugContext::sendDetachResponse( std::string id )
{
    std::string response = str( boost::format(
        "<response"
            " xmlns=\"urn:debugger_protocol_v1\" xmlns:xdebug=\"http://xdebug.org/dbgp/xdebug\" appid=\"combineXDebug\""
            " command=\"%1%\""
            " reason=\"ok\""
            " transaction_id=\"%2%\">"
            " </response>"
        )
        % "detach"
        % id
        );

    sendXML( response );
}


XDebugContext::~XDebugContext()
{
}


XDebugContext::XDebugContext()
    : m_parseState( EXPECT_SPACE_CHAR_QUOTE )
{
}


};
};
