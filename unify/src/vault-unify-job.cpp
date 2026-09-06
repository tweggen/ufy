/**
 * @file vault-unify-world.cpp
 *
 * @author Timo Weggen
 *
 * Implementation of unification engine.
 */

#include <string>
 
#include <boost/shared_ptr.hpp>
#include <boost/thread.hpp>

#include <list>

#include <vault-unification.hpp>

#include <vault-unify-clause-builtin.hpp>

namespace vault {
namespace unify {


std::atomic<JobId> Job::m_idNextJob( (JobId) 0 );


Job& Job::onFinished( boost::function<void (boost::shared_ptr<Job>)> onFinished )
{
    m_onFinished = onFinished;
    return *this;
}

Job& Job::state( Job::State state )
{
    // TXWTODO: Mutex
    m_state = state;
    // Scheduler will call onFinish.
    return *this;
}

Job::State Job::state() const
{
    // TXWTODO: Mutex.
    return m_state;
}


DebugLocation Job::getDebugLocation()
{
    return DebugLocation( "(undefined)", 1 );
}


int Job::getDebugProperties( std::list<DebugProperty>& out_lsProperties, uint64_t )
{
    out_lsProperties.clear();
    return 0;
}

int Job::getDebugStack( std::list<StackFrame>& out_lsStack )
{
    out_lsStack.clear();
    return 0;
}


int Job::setDebugTargetState( DebugListener::ChangeReason debugTargetState )
{
    // TXWTODO: Mutex
    m_debugTargetState = debugTargetState;
    return 0;
}


Job::Job()
{
    // Engine item E14: the counter is std::atomic now, so this is the
    // increment the TXWTODO asked for -- without a mutex, which for a
    // single counter would be both slower and no more correct.
    m_id = ++m_idNextJob;
    m_state = CREATED;
    m_debugTargetState = DebugListener::REGULAR;
}

};

};


