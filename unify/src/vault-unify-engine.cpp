/**
 * @file vault-unify-engine.cpp
 *
 * @author Timo Weggen
 *
 * Implementation of unification engine.
 */

#include <string>
#include <iostream>

#include <boost/shared_ptr.hpp>
#include <boost/thread.hpp>
#include <boost/format.hpp>

#include <list>

#include <vault-unification.hpp>

#include <vault-unify-clause-builtin.hpp>

namespace vault {
namespace unify {


UserEventListener::~UserEventListener()
{}


vault::unify::DebugLocation Engine::getDebugLocation()
{
    Guard g( m_mutex );

    boost::shared_ptr<Job> spJob;
    if( m_isDebugHalted ) {
        if( !m_lsReadyJobs.empty() ) {
            spJob = m_lsReadyJobs.front();
        }
    }
    if( !spJob ) return DebugLocation();
    return spJob->getDebugLocation();
}


int Engine::getDebugProperties( std::list<DebugProperty>& out_lsProperties, uint64_t frameId )
{
    Guard g( m_mutex );

    out_lsProperties.clear();
    boost::shared_ptr<Job> spJob;
    if( m_isDebugHalted ) {
        if( !m_lsReadyJobs.empty() ) {
            spJob = m_lsReadyJobs.front();
        }
    }
    if( !spJob ) return 0;
    return spJob->getDebugProperties( out_lsProperties, frameId );    
}


int Engine::getDebugStack( std::list<StackFrame>& out_lsFrames )
{
    Guard g( m_mutex );

    out_lsFrames.clear();
    boost::shared_ptr<Job> spJob;
    if( m_isDebugHalted ) {
        if( !m_lsReadyJobs.empty() ) {
            spJob = m_lsReadyJobs.front();
        }
    }
    if( !spJob ) return 0;
    return spJob->getDebugStack( out_lsFrames );
}


#if 0
int Engine::getDebugStack( std::list<StackFrame>& out_lsFrames )
{
    Guard g( m_mutex );

    boost::shared_ptr<Job> spJob;
    if( m_isDebugHalted ) {
        if( !m_lsReadyJobs.empty() ) {
            spJob = m_lsReadyJobs.front();
        }
    }
    if( !spJob ) return std::list<DebugLocation>();
    return spJob->getDebugStack( out_lsFrames );
}
#endif


/**
 * Execute jobs as they come in.
 */
void Engine::executionLoop()
{
    Guard g( m_mutex );

    while(1) {
        /*
         * If there is no ready job available, or if the engine
         * is debug halted, then wait for a signal.
         */
        if( m_lsReadyJobs.empty() || m_isDebugHalted ) {
            VAULT_UNIFY_DI( SCHEDULE, "No jobs to be done. Suspending.\n" );
            ++m_nThreadsWaiting;
            m_cond.wait( g );
            VAULT_UNIFY_DI( SCHEDULE, "Was triggered.\n" );
            --m_nThreadsWaiting;
            continue;
        } // else:
        boost::shared_ptr<Job> spJob = m_lsReadyJobs.front();
        m_lsReadyJobs.pop_front();
        DebugListener::ChangeReason debugTargetState = m_debugTargetState;
        g.unlock();
        spJob->setDebugTargetState( debugTargetState );
        spJob->performSlice();
        Job::State jobState = spJob->state();

        switch( jobState ) {

        case Job::CREATED:
            // Shouldn't be here.
            break;

        case Job::READY: 
            {
                VAULT_UNIFY_DI( ALWAYS, "Job %lld ready.\n", 
                    (long long) spJob->getId() );
                Guard g( m_mutex );
                m_lsReadyJobs.push_back( spJob );
            }
            break;

        case Job::BLOCKED:
            {
                VAULT_UNIFY_DI( ALWAYS, "Job %lld blocked.\n", 
                    (long long) spJob->getId() );
                Guard g( m_mutex );
                m_lsBlockedJobs.push_back( spJob );
            }
            break;

        case Job::FINISHED:
            {
                VAULT_UNIFY_DI( ALWAYS, "Job %lld finished.\n",
                    (long long) spJob->getId() );
                if( spJob->m_onFinished ) {
                    spJob->m_onFinished( spJob );
                }
                /*
                 * ROADMAP Phase 1 ("Ownership model"): let the job actually
                 * die instead of parking it in m_lsZombieJobs forever (which
                 * meant ~SolveJob, and therefore its per-job arena, never
                 * ran). m_onFinished is the last consumer of this job - a
                 * callback that wants the results has already called
                 * getSolutionList() and consumed them by the time the call
                 * above returns - so it is safe to just let
                 * spJob go out of scope here: it was already popped off
                 * m_lsReadyJobs above, so this is the only remaining
                 * reference, and the shared_ptr's refcount drops to zero.
                 */
                spJob->triggerRelease();
            }
            break;

        case Job::DONE:            // don't add it, everything is fine.
            break;

        case Job::DEBUGHALTED: {
            // Job has halted. Most probably, we want to inform the debugger.
            VAULT_UNIFY_DI( ALWAYS, "Job DEBUGHALTED.\n" );
            {
                Guard g( m_mutex );
                m_isDebugHalted = true;
                m_lastDebugTargetState = debugTargetState;
                m_debugTargetState = DebugListener::REGULAR;
                m_lsReadyJobs.push_front( spJob );
            }

            // Call debug listener.
            if( m_pDebugListener ) {
                m_pDebugListener->onDebugStateChanged(
                    DebugListener::INTERRUPTED,
                    debugTargetState );
            }

            break;
        }
        }

        g.lock();
    }
}


void Engine::addUserEventListener( UserEventListener* listener )
{
    {
        Guard g( m_mutex );
        m_vecUserEventListener.push_back( listener );
    }
}


void Engine::removeUserEventListener( UserEventListener* /*listener*/ )
{
    // TXWTODO: Write me
}


void Engine::emitEvent( const std::string& str )
{
    VAULT_UNIFY_DI( SERVER, "Called with \"%s\".\n", str.c_str() );
    std::vector<UserEventListener*> vec;
    {
        Guard g( m_mutex );
        vec = m_vecUserEventListener;
    }
    std::vector<UserEventListener*>::iterator it, itEnd = vec.end();
    for( it=vec.begin(); it != itEnd; ++it ) {
        VAULT_UNIFY_DI( SERVER, "Calling listener.\n" );
        (*it)->userEvent( this, str );
    }
}


/** 
 * Create a new worker thread to process jobs.
 */
void Engine::addWorkerThread()
{
    VAULT_UNIFY_DI( SCHEDULE, "Adding new worker thread.\n" );
    /*
     * Modern boost::thread (like std::thread) calls std::terminate() if a
     * still-joinable thread object is destroyed. A local here would go out
     * of scope (and terminate the process) as soon as this function
     * returns, so keep it alive on the heap, owned by the engine.
     * TXWTODO: join on engine shutdown (ROADMAP Phase 5.1)
     */
    boost::thread* pbtWorker = new boost::thread( &Engine::executionLoop, this );
    Guard g( m_mutex );
    m_lsWorkerThreads.push_back( pbtWorker );
}


void Engine::onJobReleased( boost::shared_ptr<Job> spJob )
{
    {
        Guard g( m_mutex );
        // TXWTODO: Efficiency.
        // TXWTODO: Robustness: Check wether job existed at all.
        m_lsZombieJobs.remove( spJob );
    }
    // Still referenced by argument var.
    // Unreferencecd on caller side.
}


JobId Engine::addJob( boost::shared_ptr<Job> spJob )
{
    DebugListener::ChangeReason debugTargetState;
    {
        Guard g( m_mutex );
        debugTargetState = m_debugTargetState;
    }
    spJob->setDebugTargetState( debugTargetState );

    VAULT_UNIFY_DI( ALWAYS, "Starting job %lld.\n", 
        (long long) spJob->getId() );
    {
        // TXWTODO: Set debug target state upon adding.
        Guard g( m_mutex );
        m_lsReadyJobs.push_back( spJob );
        if( m_nThreadsWaiting ) {
            VAULT_UNIFY_DI( SCHEDULE, "Triggering engine.\n" );
            m_cond.notify_one();
        } else {
            VAULT_UNIFY_DI( SCHEDULE, "Engine running. Not triggering.\n" );
        }
    }
    return 0;
}


int Engine::shutdownWorld( WorldPtr spWorld )
{
    spWorld->asyncShutdown( spWorld );
    return 0;
}


WorldPtr Engine::createWorld()
{
    WorldPtr spWorld( new World() );
    return spWorld;
}


/**
 * Output a world change for debugging.
 */
void Engine::emitChange( const std::string& change )
{
    writeOutput( "stderr", "Change: " + change + "\n" );
}


/*
 * Engine item E4 (output redirection).
 *
 * The default sink exists so that "no sink installed" is not a special case
 * anywhere else: writeOutput() always has somewhere to write, and the
 * previous behaviour is expressed as one small object rather than as a
 * branch repeated in every builtin.
 *
 * The bytes here are load-bearing. `print` used to write
 *     std::cout << "print: " << s << std::endl
 * and std::endl is a newline AND a flush, so both are reproduced: the
 * caller supplies the newline inside strText, and this flushes. The whole
 * golden corpus under test/golden pins this output, so a tidier
 * formulation that dropped the flush would reorder output against anything
 * else writing to the same descriptor and break tests that have nothing to
 * do with E4.
 */
namespace {

class DefaultOutputSink
    : public OutputSink
{
public:
    virtual void onOutput( const std::string& strStream,
                           const std::string& strText )
    {
        if( strStream == "stderr" ) {
            std::cerr << strText << std::flush;
        } else {
            std::cout << strText << std::flush;
        }
    }
};

DefaultOutputSink g_defaultOutputSink;


/*
 * Engine item E10 (structured diagnostics).
 *
 * The bytes below are exactly what reportParseError() and
 * reportImportError() used to fprintf() directly:
 *
 *     <file>:<line>:<column>: parse error
 *     <the offending source line>
 *     <spaces>^
 *
 * and, for a diagnostic with no source line to show,
 *
 *     <file>:<line>: <message>
 *
 * That format is what unify-run prints today and what its CI logs are read
 * against, so it is reproduced rather than improved: E10 moves where the
 * decision is made, not what the default decision is.
 */
class DefaultDiagnosticSink
    : public DiagnosticSink
{
public:
    virtual void onDiagnostic( const Diagnostic& diagnostic )
    {
        const char* strSeverity = "";
        if( diagnostic.severity == Diagnostic::WARNING ) {
            strSeverity = "warning: ";
        } else if( diagnostic.severity == Diagnostic::NOTE ) {
            strSeverity = "note: ";
        }

        if( diagnostic.column > 0 ) {
            fprintf( stderr, "%s:%llu:%llu: %s%s\n",
                diagnostic.uriFile.c_str(),
                (unsigned long long) diagnostic.line,
                (unsigned long long) diagnostic.column,
                strSeverity,
                diagnostic.message.c_str() );
        } else {
            fprintf( stderr, "%s:%llu: %s%s\n",
                diagnostic.uriFile.c_str(),
                (unsigned long long) diagnostic.line,
                strSeverity,
                diagnostic.message.c_str() );
        }

        if( !diagnostic.sourceLine.empty() ) {
            fprintf( stderr, "%s\n", diagnostic.sourceLine.c_str() );
            std::string strCaret(
                diagnostic.column > 1
                    ? (std::size_t)( diagnostic.column - 1 )
                    : (std::size_t) 0,
                ' ' );
            fprintf( stderr, "%s^\n", strCaret.c_str() );
        }
    }
};

DefaultDiagnosticSink g_defaultDiagnosticSink;

} // anonymous namespace


OutputSink::~OutputSink()
{
}


void Engine::setOutputSink( OutputSink* pOutputSink )
{
    m_pOutputSink = pOutputSink ? pOutputSink : &g_defaultOutputSink;
}


DiagnosticSink::~DiagnosticSink()
{
}


void Engine::setDiagnosticSink( DiagnosticSink* pDiagnosticSink )
{
    m_pDiagnosticSink = pDiagnosticSink;
}


void Engine::writeDiagnostic( const Diagnostic& diagnostic,
                              DiagnosticDefault whenNoSink )
{
    if( m_pDiagnosticSink ) {
        // An installed sink gets everything, including the runtime errors
        // that used to go nowhere. That is E10's actual deliverable.
        m_pDiagnosticSink->onDiagnostic( diagnostic );
        return;
    }

    if( whenNoSink == DIAGNOSTIC_PRINT ) {
        g_defaultDiagnosticSink.onDiagnostic( diagnostic );
    }
}


void Engine::writeOutput( const std::string& strStream,
                          const std::string& strText )
{
    // m_pOutputSink is never NULL (the constructor installs the default),
    // but a garbage Engine is a real failure mode in this codebase --
    // RuntimeContext has no constructor and leaves its members
    // uninitialized until setupDone() -- so this stays defensive rather
    // than assuming.
    if( m_pOutputSink ) {
        m_pOutputSink->onOutput( strStream, strText );
    }
}


/**
 * The engine has been asked by the debugger to enter a given state.
 */
int Engine::requestDebugState( DebugListener::ChangeReason debugTargetState )
{
    VAULT_UNIFY_DI( ALWAYS, "State %d requested.\n", 
        (int) debugTargetState );

    Guard g( m_mutex );

    /*
     * First look, what we currently are doing, then request a new state.
     * Finally, if there is job in the blocked by debugger queue, possibly
     * wake it up.
     */

    /*
     * Scheduler works like that: 
     * - without debugging, the first job in the ready queue is executed
     *   for one slice. 
     *
     * With debugging:
     * - If no job is in the ready queue, the debugger will just remember the 
     *   desired debug target state. Then, if a job is started, the target state
     *   is passed to the job.
     * - If there is a job debug halted, set the desired target state and remove
     *   it from the halted slot.
     * - If there is a job in the ready queue, it is set as the current debug halted
     *   job.
     */

    m_debugTargetState = debugTargetState;

    if( !m_lsReadyJobs.empty() ) {
        boost::shared_ptr<Job> spJob = m_lsReadyJobs.front();
        spJob->setDebugTargetState( debugTargetState );
        m_isDebugHalted = false;        
        if( m_nThreadsWaiting ) {
            VAULT_UNIFY_DI( SCHEDULE, "Triggering engine after requesting new debug state.\n" );
            m_cond.notify_one();
        }
    }

    return 0;
}


int Engine::run()
{
    return requestDebugState( DebugListener::START );
}


int Engine::stepInto()
{
    return requestDebugState( DebugListener::STEP_INTO );
}


int Engine::stepOver()
{
    return requestDebugState( DebugListener::STEP_OVER );
}


int Engine::stepOut()
{
    return requestDebugState( DebugListener::STEP_OUT );
}


int Engine::stop()
{
    return requestDebugState( DebugListener::STOP );
}


int Engine::detach()
{
    return requestDebugState( DebugListener::DETACH );
}


void Engine::setDebugListener( vault::unify::DebugListener* dl )
{
    Guard g( m_mutex );
    m_pDebugListener = dl;
}


bool Engine::isDebuggerAttached() const
{
    Guard g( m_mutex );
    return m_pDebugListener != NULL;
}


Engine::~Engine()
{
}


Engine::Engine()
{
    m_nThreadsWaiting = 0;
    m_debugTargetState = DebugListener::REGULAR;
    m_lastDebugTargetState = DebugListener::REGULAR;
    // These two were never initialized: executionLoop() gates on
    // m_isDebugHalted and the DEBUGHALTED path calls m_pDebugListener,
    // so garbage here stalls the scheduler or crashes.
    m_isDebugHalted = false;
    m_pDebugListener = NULL;
    // Engine items E4/E10: never NULL, so the writers have no unset case.
    m_pOutputSink = &g_defaultOutputSink;
    m_pDiagnosticSink = NULL;
}


};

};


