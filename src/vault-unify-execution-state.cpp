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
#include <vault-unify-clause-standard.hpp>
#include <vault-unify-debug.hpp>

#include <set>

namespace vault {
namespace unify {

    
/**
 * See the declaration (include/vault-unify.hpp) for the full contract; this
 * body lives here, out-of-line, rather than inline in the class definition
 * (unlike the rest of ClauseIterator) specifically because it calls
 * Clause::isRetired() -- Clause is still only forward-declared at
 * ClauseIterator's point in the header, so this file (which, via
 * vault-unification.hpp, has already seen Clause's complete definition by
 * the time this function body is compiled) is where it has to live.
 */
bool ExecutionState::ClauseIterator::isValid()
{
    if( m_invalidated ) {
        return false;
    }
    while(1) {
        while( m_itClause != m_itClauseEnd && (*m_itClause)->isRetired() ) {
            ++m_itClause;
        }
        if( m_itClause != m_itClauseEnd ) {
            return true;
        }
        if( NULL==m_currentState->m_pParent ) {
            return false;
        }
        enterState( m_currentState->m_pParent );
    }
}


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


/**
 * ROADMAP Phase 1 (Ownership model), pass 2. See the ownership note above
 * collectTermTree()/deleteTermTree() in vault-unify.hpp: clause term trees
 * can alias within one clause (a repeated variable shared between its own
 * head and body) -- `if` statement desugaring used to also alias a VarTerm
 * across sibling clauses (and into whichever query the `if` appeared in),
 * but AnyTermFactory::operator()(IfStatementInput) (vault-unify-parser.cpp)
 * now clones cond/body into fresh, private variables per synthesized
 * clause instead, so that cross-clause case no longer occurs. Either way,
 * the WHOLE tree of ExecutionStates is walked into one de-duplicated set
 * before any of it is deleted (a wider net than the intra-clause case
 * strictly requires, but simpler than one pass per clause). World::~World()
 * does that before the root ExecutionState (and this destructor,
 * recursively) run, so by the time we get here, deleting the
 * Clause/ExecutionState *objects* is safe: Clause::~Clause() and
 * StandardClause::~StandardClause() no longer touch term memory.
 */
void ExecutionState::collectAllTermTrees( std::set<const AbstractTerm*>& out_visited )
{
    std::list<Clause*>::const_iterator itClause, itClauseEnd = m_listClauses.end();
    for( itClause = m_listClauses.begin(); itClause != itClauseEnd; ++itClause ) {
        const Clause* pClause = *itClause;
        collectTermTree( pClause->leftHandTerm(), out_visited );

        const StandardClause* pStandardClause = dynamic_cast<const StandardClause*>( pClause );
        if( pStandardClause && pStandardClause->rightHandGoal() ) {
            const Goal* pGoal = pStandardClause->rightHandGoal();
            std::list<const AbstractTerm*>::const_iterator itTerm, itTermEnd = pGoal->m_listAbstractTerms.end();
            for( itTerm = pGoal->m_listAbstractTerms.begin(); itTerm != itTermEnd; ++itTerm ) {
                collectTermTree( *itTerm, out_visited );
            }
        }
    }

    std::list<ExecutionState*>::const_iterator itChild, itChildEnd = m_listChildStates.end();
    for( itChild = m_listChildStates.begin(); itChild != itChildEnd; ++itChild ) {
        (*itChild)->collectAllTermTrees( out_visited );
    }
}


ExecutionState::~ExecutionState()
{
    std::list<ExecutionState*>::const_iterator itChild, itChildEnd = m_listChildStates.end();
    for( itChild = m_listChildStates.begin(); itChild != itChildEnd; ++itChild ) {
        delete *itChild;
    }
    m_listChildStates.clear();

    std::list<Clause*>::const_iterator itClause, itClauseEnd = m_listClauses.end();
    for( itClause = m_listClauses.begin(); itClause != itClauseEnd; ++itClause ) {
        delete *itClause;
    }
    m_listClauses.clear();
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


