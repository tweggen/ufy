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
 * World::currentGeneration() -- World is still only forward-declared at
 * ClauseIterator's point in the header, so this file (which, via
 * vault-unification.hpp, has already seen World's complete definition by
 * the time this function body is compiled) is where it has to live.
 */
ExecutionState::ClauseIterator::ClauseIterator( ExecutionState* es )
        : m_invalidated( false )
        , m_snapshotGen( es->m_pWorld ? es->m_pWorld->currentGeneration() : 0 )
{
    enterState( es );
}


/**
 * See the declaration (include/vault-unify.hpp) for the full contract; this
 * body lives here, out-of-line, rather than inline in the class definition
 * (unlike the rest of ClauseIterator) specifically because it calls
 * Clause::isRetired()/getAppendGeneration()/getRetireGeneration() -- Clause
 * is still only forward-declared at ClauseIterator's point in the header,
 * so this file (which, via vault-unification.hpp, has already seen Clause's
 * complete definition by the time this function body is compiled) is where
 * it has to live.
 */
bool ExecutionState::ClauseIterator::isValid()
{
    if( m_invalidated ) {
        return false;
    }
    while(1) {
        while( m_idxClause < m_idxClauseEnd ) {
            const Clause* pClause = m_currentState->m_clauseStore.at( m_idxClause );
            // ROADMAP Phase 2 (runtime assert/retract, SPEC.md -- "logical
            // update view"): a clause appended AFTER this iterator's
            // snapshotGen was captured did not exist yet, from this
            // iteration's point of view -- skip it, regardless of how far
            // this scan has advanced. A clause retired BEFORE snapshotGen
            // is likewise permanently invisible to this iteration; one
            // retired AT OR AFTER snapshotGen remains visible to THIS
            // iteration for its whole lifetime (that retract() had not
            // "happened yet", from this snapshot's point of view, even
            // though the tombstone is already in place by the time we walk
            // past it -- see Clause::retire()'s comment on why tombstoning
            // rather than erasing keeps that check simply a flag/generation
            // comparison instead of a structural one).
            if( pClause->getAppendGeneration() > m_snapshotGen ) {
                ++m_idxClause;
                continue;
            }
            if( pClause->isRetired() && pClause->getRetireGeneration() <= m_snapshotGen ) {
                ++m_idxClause;
                continue;
            }
            break;
        }
        if( m_idxClause < m_idxClauseEnd ) {
            return true;
        }
        if( NULL==m_currentState->m_pParent ) {
            return false;
        }
        enterState( m_currentState->m_pParent );
    }
}


/*
 * Engine item E16: the append-only clause store. See ClauseStore's
 * declaration for why it exists and why the read path takes no lock.
 */
ClauseStore::ClauseStore()
    : m_lsSegments( new std::atomic<Clause**>[ kMaxSegments ] )
    , m_count( 0 )
{
    for( size_t i = 0; i < kMaxSegments; ++i ) {
        m_lsSegments[i].store( NULL, std::memory_order_relaxed );
    }
}


ClauseStore::~ClauseStore()
{
    clear();
    delete[] m_lsSegments;
}


int ClauseStore::append( Clause* pClause )
{
    const size_t idx = m_count.load( std::memory_order_relaxed );
    const size_t seg = idx >> kSegmentShift;

    if( seg >= kMaxSegments ) {
        return -ENOMEM;
    }

    if( NULL == m_lsSegments[seg].load( std::memory_order_relaxed ) ) {
        Clause** pSegment = new Clause*[ kSegmentSize ];
        for( size_t i = 0; i < kSegmentSize; ++i ) {
            pSegment[i] = NULL;
        }
        // Released before the count below, so a reader that sees the new
        // count is guaranteed to see this pointer too.
        m_lsSegments[seg].store( pSegment, std::memory_order_release );
    }

    m_lsSegments[seg].load( std::memory_order_relaxed )[ idx & kSegmentMask ]
        = pClause;

    // THE publication. Everything written above -- the clause's fields, the
    // slot, the segment pointer -- becomes visible to any thread that
    // observes this count, and no reader ever looks at an index at or above
    // the count it observed. That pairing is the whole synchronisation.
    m_count.store( idx + 1, std::memory_order_release );
    return 0;
}


void ClauseStore::clear()
{
    const size_t n = m_count.load( std::memory_order_relaxed );
    const size_t used = ( n + kSegmentSize - 1 ) >> kSegmentShift;
    for( size_t i = 0; i < used && i < kMaxSegments; ++i ) {
        delete[] m_lsSegments[i].load( std::memory_order_relaxed );
        m_lsSegments[i].store( NULL, std::memory_order_relaxed );
    }
    m_count.store( 0, std::memory_order_relaxed );
}


int ExecutionState::appendClause( WorldPtr spWorld, Clause* clause,
                                  const ClauseOrigin& origin )
{
    Guard g( spWorld->clauseDbMutex() );
    // ROADMAP Phase 2 (runtime assert/retract, SPEC.md -- "logical update
    // view"): stamp the clause with the generation this append itself
    // creates, BEFORE it becomes visible to any reader (push_back below) --
    // every append (parse-time or a runtime assert()) goes through this one
    // function, so this is the one place that needs to do it. See
    // World::clauseDbMutex()'s comment for why this lock is now load
    // -bearing (assert() makes the worker thread a clause-list writer too,
    // alongside the parser thread).
    clause->setAppendGeneration( spWorld->bumpGeneration() );

    // Engine item E1 (clause provenance). Stamped here, under the same lock
    // and before the push_back that makes the clause visible, for the same
    // reason the generation is: this is the one function every clause in
    // the database passes through, so it is the only place where "where did
    // this come from" can be recorded as a fact rather than guessed later
    // from the shape of the clause's name.
    //
    // Only the module id is derived; the rest is what the caller said. See
    // the declaration for why deriving the file from the clause's
    // DebugLocation looks right and is not.
    {
        ClauseOrigin stamped = origin;
        // World::moduleIdForFile() is called under this lock deliberately
        // -- see its comment.
        stamped.module = spWorld->moduleIdForFile( stamped.uriFile );
        clause->setOrigin( stamped );
    }

    // Engine item E2: the catalogue is maintained here, under the same
    // lock and from the same single choke point as the provenance stamp
    // above -- so it cannot drift from the clause list it describes.
    spWorld->catalogueAppend( clause );

    const int rcAppend = m_clauseStore.append( clause );
    if( 0 != rcAppend ) {
        // Engine item E16: the segment directory is full (8.4M clauses in
        // one World). Reported rather than swallowed -- a clause that
        // silently failed to be added would look exactly like a program
        // that never defined it.
        fprintf( stderr,
            "unify: clause database full (%zu clauses); '%s' was not added.\n",
            m_clauseStore.size(),
            clause->toString().c_str() );
        return rcAppend;
    }
    DebugLocation debugLocation = clause->getDebugLocation();
    if( debugLocation.uriFile.length() ) {
        const AbstractTerm* pTerm = clause->leftHandTerm();
        spWorld->setTermDebugInfo(
            pTerm, new TermDebugInfo(
                pTerm,
                new FileDebugInfo( debugLocation.uriFile ),
                debugLocation.line ) );
    }
    return 0;
}


ExecutionState* ExecutionState::fork()
{
    // LOCK( this )
    ExecutionState* child = new ExecutionState( this, m_pWorld );
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
    const size_t nClauses = m_clauseStore.size();
    for( size_t idx = 0; idx < nClauses; ++idx ) {
        const Clause* pClause = m_clauseStore.at( idx );
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

    const size_t nClauses = m_clauseStore.size();
    for( size_t idx = 0; idx < nClauses; ++idx ) {
        delete m_clauseStore.at( idx );
    }
    m_clauseStore.clear();
}


/**
 * Create a new client state from a parent state.
 * The parent state must be locked at the time of calling.
 */
ExecutionState::ExecutionState( ExecutionState* parent, World* pWorld )
        : m_pParent( parent )
        , m_pWorld( pWorld )
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


