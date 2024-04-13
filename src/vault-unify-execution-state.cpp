/**
 * @file vault-unify-execution-state.cpp
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
#include <vault-unify-debug.hpp>

namespace vault {
namespace unify {

    
int ExecutionState::appendClause( WorldPtr spWorld, Clause* clause )
{
    // LOCK( this )
    m_listClauses.push_back( clause );
    DebugLocation debugLocation = clause->getDebugLocation();
    if( debugLocation.uriFile.length() ) {
        const AbstractTerm* pTerm = clause->leftHandTerm();
        spWorld->setTermDebugInfo(
            pTerm, new TermDebugInfo( 
                pTerm, 
                new FileDebugInfo( debugLocation.uriFile ),
                debugLocation.line ) );
    }
    // UNLOCK( this )
    return 0;
}


ExecutionState* ExecutionState::fork()
{
    // LOCK( this )
    ExecutionState* child = new ExecutionState( this );
    m_listChildStates.push_back( child );
    // UNLOCK( this )
    return child;
}


ExecutionState::~ExecutionState()
{
}


/**
 * Create a new client state from a parent state.
 * The parent state must be locked at the time of calling. 
 */
ExecutionState::ExecutionState( ExecutionState* parent )
        : m_pParent( parent )
{
    // ASSERT_LOCK( parent )
#if 0 
    if( parent ) {
        m_pThread = parent->m_pThread;
    } else {
        m_pThread = NULL;
    }
#endif
}

};

};


