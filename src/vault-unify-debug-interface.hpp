#if !defined( _VAULT_UNIFY_DEBUG_INTERFACE_HPP )
#define _VAULT_UNIFY_DEBUG_INTERFACE_HPP

namespace vault {
namespace unify {    

struct DebugLocation {
    DebugLocation( const std::string& argFile, int64_t argLine )
            : uriFile( argFile )
            , line( argLine ) {}
    DebugLocation( const std::string& argFile, int64_t argLine, const std::string& argMethod )
            : uriFile( argFile )
            , line( argLine )
            , strMethod( argMethod ) {}
    DebugLocation() 
            : uriFile()
            , line( 0 )
            , strMethod()
    {}

    std::string uriFile;
    int64_t line;
    std::string strMethod;
};


struct StackFrame {
    StackFrame( const DebugLocation& location, int64_t id )
            : m_location( location )
            , m_id( id ) 
    {}
    StackFrame()
            : m_id( 0 )
    {}

    DebugLocation m_location;
    uint64_t m_id;
};


struct DebugProperty {
    DebugProperty(
        const std::string& name, const std::string& value,
        const DebugLocation& location ) 
            : m_name( name )
            , m_value( value )
            , m_location( location )
    {}

    std::string m_name;
    std::string m_value;
    DebugLocation m_location;
    uint64_t m_frameId;
};


/**
 * A debug listener is usually a debugger or a similar tool.
 * By the DebugListener interface, this tool can be notified
 * as soon the state of the executing application changes.
 */
class DebugListener {
public:

    virtual ~DebugListener() {}

    enum ExecutionStatus {
        STARTING,
        RUNNING,
        DETACHED,
        INTERRUPTED,
        DONE,
        CRASHED
    };
    enum ChangeReason {
        START,
        STEP_INTO,
        STEP_OVER,
        STEP_OUT,
        STOP,
        BREAKPOINT,
        REGULAR,
        DETACH
    };

    virtual void onDebugStateChanged(
        ExecutionStatus status,
        ChangeReason reason
        ) = 0;

};


/**
 * An execution controller is something that can be controlled
 * by a debugger or a similar development tool
 */
class ExecutionController
{
public:
    virtual ~ExecutionController() {}

    virtual void setDebugListener( DebugListener* ) = 0;

    virtual int run() = 0;
    virtual int stepInto() = 0;
    virtual int stepOver() = 0;
    virtual int stepOut() = 0;
    virtual int stop() = 0;
    virtual int detach() = 0;

    virtual DebugLocation getDebugLocation() = 0;
    virtual int getDebugStack( std::list<StackFrame>& out_lsFrames ) = 0;

    /** 
     * Return the properties (locals) of the given frame. When ~0 is passed,
     * the properties from all frames are returned.
     */
    virtual int getDebugProperties( std::list<DebugProperty>& out_lsProperties, uint64_t frameId ) = 0;
};


/**
 * A debug contrller is somebody who implements debugging, usually
 * a debugger.
 * 
 * (Yes, this might be the same class that also implements the Debug
 * Listener interface).
 */
class DebugController
{
public:
};

};
};

#endif