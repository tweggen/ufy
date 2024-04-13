#if !defined( _VAULT_UNIFY_XDEBUG_HPP )
#define _VAULT_UNIFY_XDEBUG_HPP 1

#include <vault-unify.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>

namespace vault {
namespace unify {
    
std::string base64_encode(uint8_t const* buf, unsigned int bufLen);

class RuntimeContext;


/**
 * Process the debugging commands received. 
 */
class XDebugCommandConsumer {
public:
    virtual ~XDebugCommandConsumer() {}

    virtual void consumeXDebugCommand( const std::list<std::string>& lsCommand ) = 0;
};


class XDebugXMLConsumer {
public:
    virtual ~XDebugXMLConsumer() {}

    struct IOV {
        const char* data;
        int length;
    };

    /**
     * Consumer the given XML data using the XDebug XML  
     *
     * @param nIOV
     *    The number of scatter gather parts that should be processed
     * @param pIOV
     *    A pointer to an array of scatter parts.
     */
    virtual void consumeXMLPacket( int nIOV, const IOV* pIOV ) = 0;
};


/**
 * This class defines one debugging context to access the
 * unify engine.
 */
class XDebugContext 
{
public:
    XDebugContext();
    ~XDebugContext();

    /**
     * Set up the consumer that processes the commands we have parsed.
     */
    XDebugContext&  setCommandConsumer( XDebugCommandConsumer* consumer );

    /**
     * Set up a consumer for the xml data as sent by the xdebug binding.
     */
    XDebugContext& setXMLConsumer( XDebugXMLConsumer* xmlConsumer );

    /**
     * Called if a debug session has connected.
     */
    int onConnected();

    /**
     * Consume input from the ide.
     */
    int consumeInput( const char* inputStart, const char* inputEnd );


    /**
     * File name of initial file.
     */
    void setFileName( std::string file ) { m_initialFileName = file; }

    /**
     * Send data to the IDE.
     */
    int sendXML( const char* xmlStart, const char* xmlEnd );
    int sendXML( const char* xmlStart ) {
        return sendXML( xmlStart, xmlStart+strlen( xmlStart ) );
    }
    int sendXML( const std::string& str ) {
        return sendXML( str.c_str(), str.c_str()+str.length() );
    }

    void sendStandardResponse(
            std::string id,
            std::string command,
            DebugListener::ExecutionStatus executionStatus,
            const DebugLocation& location );

    void sendStack( std::string id,
        DebugListener::ExecutionStatus,
        DebugListener::ChangeReason changeReason,
        const std::list<StackFrame>& lsStack );
    void sendContext(
        std::string id,
        std::string strContextId,
        const std::list<DebugProperty>& lsProperties );
    void sendStatus( std::string id,
        DebugListener::ExecutionStatus,
        DebugListener::ChangeReason changeReason,
        DebugLocation debugLocation );
    void sendSuccessResponse( std::string id, std::string command, std::string success );
    void sendContextNamesResponse( std::string id, std::string command, std::string content );
    void sendFeatureGetResponse( std::string id, std::string supported, std::string content );
    void sendRunResponse( std::string id, DebugListener::ExecutionStatus );
    void sendStepIntoResponse( std::string id, DebugListener::ExecutionStatus, DebugLocation );
    void sendStepOverResponse( std::string id, DebugListener::ExecutionStatus, DebugLocation );
    void sendStepOutResponse( std::string id, DebugListener::ExecutionStatus, DebugLocation );
    void sendStopResponse( std::string id, DebugListener::ExecutionStatus, DebugLocation );
    void sendDetachResponse( std::string id );

    static std::string executionStatusString( DebugListener::ExecutionStatus s ) {
        switch( s ) {
            case DebugListener::STARTING: return "starting"; break;
            case DebugListener::RUNNING: return "running"; break;
            case DebugListener::DETACHED: return "stopped"; break;
            case DebugListener::INTERRUPTED: return "break"; break;
            case DebugListener::DONE: return "stopping"; break;
            case DebugListener::CRASHED: return "break"; break;
            default: return "break"; break;
        }
    }

    static std::string changeReasonString( DebugListener::ChangeReason c ) {
            switch( c ) {
            case DebugListener::START: return "run"; break;
            case DebugListener::STEP_INTO: return "step_into"; break;
            case DebugListener::STEP_OVER: return "step_over"; break;
            case DebugListener::STEP_OUT: return "step_out"; break;
            case DebugListener::STOP: return "stop"; break;
            default: return "unsupported"; break;
        }
    }

protected:
    int executeLine( const std::list<std::string>& lsCommand );

private:
    int flushCurrentLine();
    int flushCurrentInputArg();
    int tryFlushCurrentInputArg();

    enum ParseState {
        EXPECT_SPACE_CHAR_QUOTE,
        EXPECT_SPACE_CHAR,
        EXPECT_QUOTE_QUOTED_CHAR,
        EXPECT_QUOTED_CHARACTER,
        EXPECT_NOTHING
    };

    boost::mutex m_mutex;

    /**
     * This is the object receiving the commands that have been passed to us.
     */
    XDebugCommandConsumer* m_pCommandConsumer;

    /**
     * This is the consumer of the xml data as returned from the engine.
     */
    XDebugXMLConsumer* m_pXMLConsumer;

    // Now some members to describe the current input we consume

    /** 
     * Holds command line arguments we have been supplied that
     * have not been scanned yet.
     */ 
    std::list<std::string> m_currInputArgs;

    /**
     * Holds input data that we have been provided, which we did not 
     * scan yet.
     */
    std::string m_currUnparsedInput;

    /**
     * The current parse state.
     */
    ParseState m_parseState;

    /**
     * The current input argument we scan.
     */ 
    std::string m_currentInputArg;

    /**
     * Initial file name.
     */
    std::string m_initialFileName;

};


/**
 * Interfaces the generic xdebug interface to the combine engine.
 */
class XDebugCombine 
    : public XDebugCommandConsumer
    , public DebugListener
{
public:
    XDebugCombine();
    virtual ~XDebugCombine();

    XDebugCombine& setRuntimeContext( RuntimeContext* rctx );
    XDebugCombine& setXDebugContext( XDebugContext* debugContext );
    XDebugCombine& setExecutionController( ExecutionController* pExecutionController );

    virtual void consumeXDebugCommand( const std::list<std::string>& lsCommand );

protected:

    // For the DebugListener interface

    virtual void onDebugStateChanged(
            ExecutionStatus status,
            ChangeReason reason
            );

private:
    enum ControllerState {
        ControllerIdle,
        ControllerWaitForResponse
    };

    boost::mutex m_mutex;

    /**
     * The vault runtime context we ought to debug.
     */
    RuntimeContext* m_pRuntimeContext;

    XDebugContext* m_pXDebugContext;

    /**
     * The execution controller this debug session is associated with.
     */
    ExecutionController* m_pExecutionController;

    /**
     * The current point of execution.
     */
    DebugLocation m_locCurrent;

    /**
     * The last known execution status of the job.
     */
    DebugListener::ExecutionStatus m_executionStatus;

    /**
     * The reason as reported why the last known execution status has been reached.
     */
    DebugListener::ChangeReason m_lastChangeReason;

    /**
     * What communication are expecting as a debug listener.
     */
    ControllerState m_controllerState;

    /**
     * If we are waiting for a response, what kind of response are 
     * we waiting for?
     */
    DebugListener::ChangeReason m_expectedChangeReason;

    /**
     * If the IDE issued an command that we will reply to, this is the 
     * id of the command.
     */
    std::string m_strLastId;

    /**
     * Command id of the last status inqziery command.
     */
    std::string m_strLastStatusId;
};


};
};

#endif