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
                // We can now at this job to the zombie list.
                {
                    Guard g( m_mutex );
                    m_lsZombieJobs.push_back( spJob );
                }
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


void Engine::removeUserEventListener( UserEventListener* listener )
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
    std::cerr << "Change: " << change << std::endl;
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
}


};

};


