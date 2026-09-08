#include <string>
#include <list>
#include <ctype.h>
#include <string.h>

#include <iostream>
#include <fstream>
#include <streambuf>

#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/assign/list_of.hpp>
#include <sstream>

#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>

#include <vault-unify.hpp>
#include "vault-unify-xdebug.hpp"

// IDE:       command [SPACE] [args] -- data [NULL]
// DEBUGGER:  [NUMBER] [NULL] XML(data) [NULL]

namespace vault {
namespace unify {

static const std::string base64_chars = 
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrstuvwxyz"
             "0123456789+/";


static inline bool is_base64(uint8_t c) {
  return (isalnum(c) || (c == '+') || (c == '/'));
}

std::string base64_encode(uint8_t const* buf, unsigned int bufLen) {
  std::string ret;
  int i = 0;
  int j = 0;
  uint8_t char_array_3[3];
  uint8_t char_array_4[4];

  while (bufLen--) {
    char_array_3[i++] = *(buf++);
    if (i == 3) {
      char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
      char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
      char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
      char_array_4[3] = char_array_3[2] & 0x3f;

      for(i = 0; (i <4) ; i++)
        ret += base64_chars[char_array_4[i]];
      i = 0;
    }
  }

  if (i)
  {
    for(j = i; j < 3; j++)
      char_array_3[j] = '\0';

    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
    char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
    char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
    char_array_4[3] = char_array_3[2] & 0x3f;

    for (j = 0; (j < i + 1); j++)
      ret += base64_chars[char_array_4[j]];

    while((i++ < 3))
      ret += '=';
  }

  return ret;
}

#if 0
std::vector<uint8_t> base64_decode(std::string const& encoded_string) {
  int in_len = encoded_string.size();
  int i = 0;
  int j = 0;
  int in_ = 0;
  uint8_t char_array_4[4], char_array_3[3];
  std::vector<uint8_t> ret;

  while (in_len-- && ( encoded_string[in_] != '=') && is_base64(encoded_string[in_])) {
    char_array_4[i++] = encoded_string[in_]; in_++;
    if (i ==4) {
      for (i = 0; i <4; i++)
        char_array_4[i] = base64_chars.find(char_array_4[i]);

      char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
      char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
      char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

      for (i = 0; (i < 3); i++)
          ret.push_back(char_array_3[i]);
      i = 0;
    }
  }

  if (i) {
    for (j = i; j <4; j++)
      char_array_4[j] = 0;

    for (j = 0; j <4; j++)
      char_array_4[j] = base64_chars.find(char_array_4[j]);

    char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
    char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
    char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

    for (j = 0; (j < i - 1); j++) ret.push_back(char_array_3[j]);
  }

  return ret;
}
#endif

std::string base64_decode( const std::string& s )
{
    namespace bai = boost::archive::iterators;

    std::stringstream os;

    typedef bai::transform_width<bai::binary_from_base64<const char *>, 8, 6> base64_dec;

    unsigned int size = s.size();

    // Remove the padding characters, cf. https://svn.boost.org/trac/boost/ticket/5629
    if (size && s[size - 1] == '=') {
        --size;
        if (size && s[size - 1] == '=') {
            --size;
        }
    }
    if (size == 0) return std::string();

    std::copy( base64_dec( s.data() ), base64_dec( s.data() + size ),
        std::ostream_iterator<char>( os ) );

    return os.str();
}


int parseCommand(
        const std::list<std::string>& lsCommand, 
        const std::map<std::string, std::string> argSpec,
        std::map<std::string, std::string>& out_args )
{
    std::list<std::string>::const_iterator it = lsCommand.begin();
    std::list<std::string>::const_iterator itEnd = lsCommand.end();

    // Skip the command, it already has been parsed.
    ++it;

    enum OptState {
        EXPECT_OPTION,
        EXPECT_OPTARG,
        EXPECT_DATA,
        EXPECT_EOF,
        EXPECT_ERROR
    };

    OptState optState = EXPECT_OPTION;
    std::string option;

    for( ; it != itEnd && optState < EXPECT_EOF; ++it ) {

        switch( optState ) {

        case EXPECT_OPTION:
            option = *it;
            // VAULT_UNIFY_DI( SERVER, "Found option \"%s\".\n", option.c_str() );

            if( "--" == option ) {
                // expect data in next argument.
                optState = EXPECT_DATA;
            } else {
                // Do we know this option?
                const std::map<std::string, std::string>::const_iterator it = argSpec.find( option );
                if( it==argSpec.end() ) {
                    // optState = EXPECT_ERROR;
                    // Unknown option? Read anyway.
                    optState = EXPECT_OPTARG;
                } else {
                    // We know this option.
                    optState = EXPECT_OPTARG;
                }
            }
            break;

        case EXPECT_OPTARG:
            out_args[option] = *it;
            optState = EXPECT_OPTION;
            VAULT_UNIFY_DI( SERVER, "Found option \"%s\", value \"%s\".\n",
                option.c_str(),
                it->c_str() );
            break;

        case EXPECT_DATA:
            out_args["--"] = base64_decode( *it );
            optState = EXPECT_EOF;
            break;

        case EXPECT_EOF:
        case EXPECT_ERROR:
            break;

        }

    }

    switch( optState ) {

    case EXPECT_ERROR:
        VAULT_UNIFY_DI( SERVER, "Error while parsing options.\n" );
        return -EFAULT;

    case EXPECT_OPTARG:
        out_args[option] = "";
        break;

    default:
        break;

    }

    // Second iteration: Look, wether all required arguments are received.
    std::map<std::string, std::string>::const_iterator itArg, itArgEnd = argSpec.end();
    for( itArg=argSpec.begin(); itArg != itArgEnd; ++itArg ) {
        if( "1"==itArg->second ) {
            // required=? Then check.
            if( out_args.find( itArg->first )==out_args.end() ) {
                VAULT_UNIFY_DI( SERVER, "Required option \"%s\" not found.\n", itArg->first.c_str() );
                return -ENOENT;
            } else {
                // VAULT_UNIFY_DI( SERVER, "Required option \"%s\" found.\n", itArg->first.c_str() );
            }
        }
    }

    return 0;
}


XDebugCombine& XDebugCombine::setRuntimeContext( RuntimeContext* pRuntimeContext )
{
    Guard g( m_mutex );
    m_pRuntimeContext = pRuntimeContext;
    return *this;
}


XDebugCombine& XDebugCombine::setXDebugContext( XDebugContext* pXDebugContext )
{
    Guard g( m_mutex );
    m_pXDebugContext = pXDebugContext;
    return *this;
}


XDebugCombine& XDebugCombine::setExecutionController( ExecutionController* pExecutionController )
{
    Guard g( m_mutex );
    m_pExecutionController = pExecutionController;
    return *this;
}


void XDebugCombine::consumeXDebugCommand( const std::list<std::string>& lsCommand )
{
    int resGetOpt = 0;

    if( lsCommand.empty() ) return;

    if( !m_pExecutionController ) {
        VAULT_UNIFY_DI( ALWAYS, "XDebugCombine: No ExecutionController attached.\n" );
        return;
    }

    std::list<std::string>::const_iterator it;

    {
        std::list<std::string>::const_iterator itEnd = lsCommand.end();
        for( it=lsCommand.begin(); it != itEnd; ++it ) {
            VAULT_UNIFY_DI( SERVER, "Received \"%s\"\n", it->c_str() );
        }
    }

    it = lsCommand.begin();
    std::map<std::string, std::string> optargs;

    if( "breakpoint_set"==*it ) {

        static std::map<std::string, std::string> argSpec =
            boost::assign::map_list_of( "-i", "1" );
        resGetOpt = parseCommand( lsCommand, argSpec, optargs );
        if( resGetOpt<0 ) return;

        // Engine item E14: shared by every debugger connection.
static std::atomic<int> fakeBreakpointId( 10000 );
        ++fakeBreakpointId;

        m_pXDebugContext->sendXML( "<response"
            " xmlns=\"urn:debugger_protocol_v1\" xmlns:xdebug=\"http://xdebug.org/dbgp/xdebug\""
            " command=\"breakpoint_set\""
            " id=\""+boost::lexical_cast<std::string>( fakeBreakpointId )+"\""
            " state=\"enabled\""
            " transaction_id=\""+optargs["-i"]+"\"></response>" );

    } else if( "context_get"==*it ) {

        static std::map<std::string, std::string> argSpec =
            boost::assign::map_list_of( "-i", "1" );
        resGetOpt = parseCommand( lsCommand, argSpec, optargs );
        if( resGetOpt<0 ) return;
        std::string strContextId;

        if( optargs.find( "-c" ) != optargs.end() ) {
            strContextId = optargs["-c"];
        }
        std::list<DebugProperty> lsProperties;
        (void) m_pExecutionController->getDebugProperties( lsProperties, ~0ull );
        m_pXDebugContext->sendContext( optargs["-i"], strContextId, lsProperties );

    } else if( "context_names"==*it ) {

        static std::map<std::string, std::string> argSpec =
            boost::assign::map_list_of( "-i", "1" );
        resGetOpt = parseCommand( lsCommand, argSpec, optargs );
        if( resGetOpt<0 ) return;

        m_pXDebugContext->sendContextNamesResponse(
            optargs["-i"], "context_names",
            "<context name=\"RootContext\" id=\"0\"/>" );

    } else if( "feature_set"==*it ) {

        static std::map<std::string, std::string> argSpec =
            boost::assign::map_list_of( "-n", "1" )( "-v", "1" )( "-i", "1" );
        resGetOpt = parseCommand( lsCommand, argSpec, optargs );
        if( resGetOpt<0 ) return;

        m_pXDebugContext->sendXML( "<response"
            " xmlns=\"urn:debugger_protocol_v1\" xmlns:xdebug=\"http://xdebug.org/dbgp/xdebug\""
            " command=\"feature_set\""
            " feature=\""+optargs["-n"]+"\""
            " success=\"1\""
            " transaction_id=\""+optargs["-i"]+"\"></response>" );

    } else if( "feature_get"==*it ) {

        static std::map<std::string, std::string> argSpec =
            boost::assign::map_list_of( "-n", "1" )( "-i", "1" );
        resGetOpt = parseCommand( lsCommand, argSpec, optargs );
        if( resGetOpt<0 ) return;

        std::string strFeature = optargs["-n"];
        m_strLastId = optargs["-i"];

        /* 
         * Individual replies for different versions.
         */
        if( strFeature=="breakpoint_types" ) {
            m_pXDebugContext->sendFeatureGetResponse( m_strLastId, "1", "line call return" );
        } else {
            m_pXDebugContext->sendFeatureGetResponse( m_strLastId, "0", "" );
        }
        m_strLastId = "";

    } else if( "run"==*it ) {

        static std::map<std::string, std::string> argSpec =
            boost::assign::map_list_of( "-i", "1" );
        resGetOpt = parseCommand( lsCommand, argSpec, optargs );
        if( resGetOpt<0 ) return;

        // TXWTODO: Integrate run.
#if 0
        m_pXDebugContext->sendXML( "<response"
            " xmlns=\"urn:debugger_protocol_v1\" xmlns:xdebug=\"http://xdebug.org/dbgp/xdebug\" appid=\"combineXDebug\""
            " command=\"run\""
            " status=\"running\""
            " reason=\"ok\""
            " transaction_id=\""+optargs["-i"]+"\"/>" );
#endif

        {
            Guard g( m_mutex );
            m_strLastId = optargs["-i"];
            m_controllerState = ControllerWaitForResponse;
            m_expectedChangeReason = DebugListener::START;
        }

        m_pExecutionController->run();

    } else if( "source"==*it ) {

        static std::map<std::string, std::string> argSpec =
            boost::assign::map_list_of( "-f", "1" )( "-i", "1" );
        resGetOpt = parseCommand( lsCommand, argSpec, optargs );
        if( resGetOpt<0 ) return;

        m_strLastId = optargs["-i"];
        std::string strFilename = optargs["-f"];
        int lineBegin = 0;
        int lineEnd = INT_MAX;

        if( optargs.find( "-b" ) != optargs.end() ) {
            lineBegin = atoi( optargs["-b"].c_str() );
        }
        if( optargs.find( "-e" ) != optargs.end() ) {
            lineEnd = atoi( optargs["-e"].c_str() );
        }

        // We try to respond on this one immediately.
        // First, the stdin case.
        std::string strReply = "<response"
            " xmlns=\"urn:debugger_protocol_v1\" xmlns:xdebug=\"http://xdebug.org/dbgp/xdebug\""
            " command=\"source\""
            " success=\"1\""
            " transaction_id=\"" + optargs["-i"] + "\" encoding=\"base64\">";
        std::string strData;
        bool haveSource = false;
        if( "dbgp://stdin"==strFilename ) {

            //if( lineBegin < 1 ) lineBegin = 1;
            //if( lineEnd > 1 ) lineEnd = 1;
        } else {
            // TXWTODO: Check for partial read.
            if( !strFilename.compare( 0, 7, "file://" ) ) {
                strFilename = strFilename.substr( 7 );
                /*
                 * Just open it. This used to call ::access( ..., R_OK )
                 * first, which is POSIX-only -- the one thing keeping this
                 * backend off Windows -- and was also a check-then-open
                 * race: the answer could change between the two calls.
                 * Whether the stream opened is the same question, asked
                 * once, portably.
                 */
                std::ifstream t( strFilename.c_str() );
                if( t ) {
                    std::string str(
                        (std::istreambuf_iterator<char>(t)),
                        std::istreambuf_iterator<char>());
                    strData = str;
                    haveSource = true;
                }
            }
        }

        if( !haveSource ) {

            if( 0==lineBegin ) lineBegin = 1;
            if( INT_MAX==lineEnd ) lineEnd = lineBegin;
            for( int line=lineBegin; line<=lineEnd; ++line ) {
                strData += "No data.\n";
            }
        }

        strReply += base64_encode( (uint8_t*) strData.c_str(), strData.length() );

        strReply += "</response>";

        m_pXDebugContext->sendXML( strReply );

    } else if( "stack_get"==*it ) {

        static std::map<std::string, std::string> argSpec =
            boost::assign::map_list_of( "-i", "1" );
        resGetOpt = parseCommand( lsCommand, argSpec, optargs );
        if( resGetOpt<0 ) return;

        std::list<StackFrame> lsStack;
        (void) m_pExecutionController->getDebugStack( lsStack );

        m_strLastStatusId = optargs["-i"];
        // TXWTODO: Mutex protect reading the different members.
        m_pXDebugContext->sendStack( 
            m_strLastStatusId,
            m_executionStatus,
            m_lastChangeReason,
            lsStack );

    } else if( "status"==*it ) {

        static std::map<std::string, std::string> argSpec =
            boost::assign::map_list_of( "-i", "1" );
        resGetOpt = parseCommand( lsCommand, argSpec, optargs );
        if( resGetOpt<0 ) return;

        m_locCurrent = m_pExecutionController->getDebugLocation();

        m_strLastStatusId = optargs["-i"];
        // TXWTODO: Mutex protect reading the different members.
        m_pXDebugContext->sendStatus( 
            m_strLastStatusId,
            m_executionStatus,
            m_lastChangeReason,
            m_locCurrent );

    } else if( "stderr"==*it ) {

        static std::map<std::string, std::string> argSpec =
            boost::assign::map_list_of( "-i", "1" );
        resGetOpt = parseCommand( lsCommand, argSpec, optargs );
        if( resGetOpt<0 ) return;

        // TXWTODO: Do actually copy stdout.
        m_pXDebugContext->sendSuccessResponse( optargs["-i"], "stderr", "1" );

    } else if( "stdout"==*it ) {

        static std::map<std::string, std::string> argSpec =
            boost::assign::map_list_of( "-i", "1" );
        resGetOpt = parseCommand( lsCommand, argSpec, optargs );
        if( resGetOpt<0 ) return;

        // TXWTODO: Do actually copy stdout.
        m_pXDebugContext->sendSuccessResponse( optargs["-i"], "stdout", "1" );

    } else if( "step_into"==*it ) {

        static std::map<std::string, std::string> argSpec =
            boost::assign::map_list_of( "-i", "1" );
        resGetOpt = parseCommand( lsCommand, argSpec, optargs );
        if( resGetOpt<0 ) return;

        {
            Guard g( m_mutex );

            // Verify state.
            if( m_controllerState != ControllerIdle ) {
                // Whatever we like to do.
                VAULT_UNIFY_DI( SERVER, "Received command in wrong state %d.\n", (int) m_controllerState );
                return;
            }
            m_strLastId = optargs["-i"];
            m_controllerState = ControllerWaitForResponse;
            m_expectedChangeReason = DebugListener::STEP_INTO;
        }

        m_pExecutionController->stepInto();

    } else if( "step_into"==*it ) {

        static std::map<std::string, std::string> argSpec =
            boost::assign::map_list_of( "-i", "1" );
        resGetOpt = parseCommand( lsCommand, argSpec, optargs );
        if( resGetOpt<0 ) return;

        {
            Guard g( m_mutex );

            // Verify state.
            if( m_controllerState != ControllerIdle ) {
                // Whatever we like to do.
                VAULT_UNIFY_DI( SERVER, "Received command in wrong state %d.\n", (int) m_controllerState );
                return;
            }
            m_strLastId = optargs["-i"];
            m_controllerState = ControllerWaitForResponse;
            m_expectedChangeReason = DebugListener::STEP_OVER;
        }

        m_pExecutionController->stepOver();

    }
}


/**
 * Called by the combine engine as soon the execution status changed.
 *
 * Usually, we to inform the debugger engine about the state change.
 */
void XDebugCombine::onDebugStateChanged(
        vault::unify::DebugListener::ExecutionStatus status,
        vault::unify::DebugListener::ChangeReason reason )
{
    VAULT_UNIFY_DI( SERVER, "Called status %d reason %d.\n",
        (int) status, (int) reason );

    enum Action {
        ActionNothing,
        ActionSendReply
    } action = ActionNothing;

    ControllerState currControllerState;
    DebugListener::ChangeReason currExpectedChangeReason;
    std::string strLastId;
    DebugLocation locCurrent;

    // Fetch current position, if it makes sense.
    switch( status ) {
    case DebugListener::RUNNING:
    case DebugListener::DONE:
        break;
    default:
        locCurrent = m_pExecutionController->getDebugLocation();
        break;
    }

    /*
     * Check the new execution status, then check the current
     * one. Figure out, which action we should perform.
     */
    {
        Guard g( m_mutex );

        // Mirror for further processing
        currControllerState = m_controllerState;
        currExpectedChangeReason = m_expectedChangeReason;
        strLastId = m_strLastId;
        // And store the location we jsut read.
        m_locCurrent = locCurrent;

        // This is not entirely correct, as we do not check for
        // unsolicited status 
        m_controllerState = ControllerIdle;


        switch( status ) {
        case DebugListener::STARTING:
            /*
             * Why should the engine report that it now is starting?
             * Ignore and accept.
             */
            if( m_executionStatus != DebugListener::STARTING ) {
                VAULT_UNIFY_DI( SERVER, "Engine reported starting although we are in %d.\n",
                    (int) m_executionStatus );
                break;
            }
            // Already is.
            // m_executionStatus = status;
            break;

        case DebugListener::RUNNING:
            /*
             * The engine reports it now is running?
             */ 
            break;

        case DebugListener::DETACHED:
            /*
             * The engine reports it is detached. This means the
             * job has finished and we are no more called back.
             */
            break;

        case DebugListener::INTERRUPTED:
            /*
             * Execution is interrupted. This may be the consequence
             * of finishing a step_xxx operation or hitting a breakpoint.
             */
            action = ActionSendReply;

            break;

        case DebugListener::DONE:
            /*
             * Job has finished operation. We now are expected to detach.
             * Or the job might detach us.
             */
             break;

        case DebugListener::CRASHED:
            /*
             * An exception has happened executing the job. We can consider
             * it as crashed. However, we still might be able to perform
             * some analysis.
             */
            break;

        default:
            break;
        }
    }

    switch( action ) {
    case ActionSendReply:
        /*
         * Depending on the controller state, send a reply / status update.
         * The command field of the reply is given by the expected change reason.
         * The state of the reply is determined by the execution status as reported.
         */

        switch( currControllerState ) {
        case ControllerIdle: {
            /*
             * The Debugger IDE did not request any action.
             * So this is an unsolicited update. As long we do not support 
             * async operation, we ignore this kind of updates.
             */
            std::string strCommand = 
                XDebugContext::changeReasonString( DebugListener::START );
            m_pXDebugContext->sendStandardResponse(
                m_strLastId,
                strCommand,
                status,
                locCurrent );

            break;
        }

        case ControllerWaitForResponse: {
            /*
             * Send a reply using the new execution statem and the reason.
             */
            std::string strCommand = 
                XDebugContext::changeReasonString( currExpectedChangeReason );

            /*
             * Analyse the actual change reason.
             * While the expected change reason reflects the command we have issued
             * to the engine, the real change reason also contains breakpoints etc.
             */
            switch( currExpectedChangeReason ) {
            case DebugListener::START:
                // break;

            case DebugListener::STEP_INTO:
            case DebugListener::STEP_OVER:
            case DebugListener::STEP_OUT:
            case DebugListener::STOP:
            case DebugListener::DETACH:
                /*
                 * These are valid commands that have been issued by the IDE
                 * before.
                 */

                // TXWTODO: Derive the report state from this type.

                break;

            case DebugListener::BREAKPOINT:
            case DebugListener::REGULAR:

                // TXWTODO: Derive the report state from this state.

                break;

            }

            m_pXDebugContext->sendStandardResponse(
                m_strLastId,
                strCommand,
                status,
                locCurrent );

            break;
        }
        break;
    }
    
    default:
    case ActionNothing:
        break;
    }
}


XDebugCombine::~XDebugCombine()
{
}


XDebugCombine::XDebugCombine()
    : m_pRuntimeContext( NULL )
    , m_pXDebugContext( NULL )
    , m_pExecutionController( NULL )
    , m_executionStatus( DebugListener::STARTING )
    , m_lastChangeReason( DebugListener::REGULAR )
    , m_controllerState( ControllerIdle )
{
}

};
};
