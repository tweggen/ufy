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
#include <vault-unify-debug.hpp>

#include <set>

namespace vault {
namespace unify {


/**
 * ROADMAP Phase 1 (Ownership model), pass 2: program-lifetime cleanup.
 *
 * Order matters here only in the sense that everything below must run
 * BEFORE m_rootState (an ExecutionState held by value) and everything it
 * owns are destroyed automatically once this destructor's body returns
 * (normal member-destruction order): ExecutionState::~ExecutionState() /
 * Clause::~Clause() / StandardClause::~StandardClause() no longer touch
 * term memory themselves (see their comments), relying on this pass having
 * already freed it.
 *
 * 1) TermDebugInfo/FileDebugInfo. Every entry actually created today is
 *    exclusively owned by World: ExecutionState::appendClause() and
 *    AnyTermFactory::operator()(ConsTermInput) (vault-unify-parser.cpp)
 *    both always construct a *fresh* TermDebugInfo/FileDebugInfo, and the
 *    one path that could alias a FileDebugInfo across many entries --
 *    parseExecuteSegment()'s pFileDebugInfo parameter, forwarded via
 *    PrologParser::Context::setFileDebugInfo() -- is always NULL at both
 *    call sites today (unify-run.cpp, vault-unify-rest-server.cpp). We
 *    still de-duplicate FileDebugInfo pointers defensively before deleting
 *    them, in case that ever changes. This is done first, while every
 *    AbstractTerm* map key (whatever it points at, or used to) is still
 *    exactly the value it was inserted with; nothing here dereferences a
 *    key, only the mapped TermDebugInfo / FileDebugInfo pointer values.
 *
 * 2) Clause database term trees. Every clause's head term, and (for
 *    StandardClause) every term in its body Goal's list, across the WHOLE
 *    ExecutionState tree (root + any child states), collected into ONE
 *    de-duplicated set and deleted exactly once -- see the ownership note
 *    on collectTermTree()/deleteTermTree() in vault-unify.hpp for why a
 *    per-clause pass would risk a double free (a clause's own head and
 *    body can still share a repeated variable's VarTerm*). `if` statement
 *    desugaring (AnyTermFactory::operator()(IfStatementInput),
 *    vault-unify-parser.cpp) used to also alias a VarTerm between sibling
 *    clauses -- and into whichever query the `if` appeared in -- but no
 *    longer does: it clones cond/body into fresh variables private to
 *    each synthesized clause (cloneTermTree(), declared next to
 *    collectTermTree()/deleteTermTree()), so that is no longer a reason
 *    this pass has to span the whole ExecutionState tree in one set --
 *    it still does, but only for the intra-clause reason above (and
 *    because a single wide pass is simpler than one pass per clause).
 *
 * Deliberately NOT freed here: query Goal term trees. These are owned by
 * whichever SolveJob solved that query and are actually gone by the time
 * World is destroyed now -- ~SolveJob() (vault-unify-solvejob.cpp) frees
 * them itself, which is only safe because the clause database no longer
 * aliases into them (see the previous paragraph); see the comment on
 * SolveJob's m_arenaGoals cleanup there for the detail.
 */
World::~World()
{
    /*
     * De-duplicate the TermDebugInfo values as well as the FileDebugInfos:
     * the '->' desugaring in vault-unify-parser.cpp registers one
     * TermDebugInfo under SEVERAL term keys (pre-goal and replacement
     * terms share the same info object), so deleting per map entry would
     * double-free (found as a heap-use-after-free by the ASan leak run).
     */
    std::set<const TermDebugInfo*> visitedTermDebugInfos;
    std::set<const FileDebugInfo*> visitedFileDebugInfos;
    std::list<TermDebugInfo*> lsAllDebugInfos( m_lsRetiredDebugInfos );
    TermDebugMap::const_iterator itTDI, itTDIEnd = m_mapDebugInfos.end();
    for( itTDI = m_mapDebugInfos.begin(); itTDI != itTDIEnd; ++itTDI ) {
        lsAllDebugInfos.push_back( itTDI->second );
    }
    std::list<TermDebugInfo*>::const_iterator itAll, itAllEnd = lsAllDebugInfos.end();
    for( itAll = lsAllDebugInfos.begin(); itAll != itAllEnd; ++itAll ) {
        TermDebugInfo* pTermDebugInfo = *itAll;
        if( !pTermDebugInfo ) {
            continue;
        }
        if( !visitedTermDebugInfos.insert( pTermDebugInfo ).second ) {
            // Already freed via another key or the retired list.
            continue;
        }
        const FileDebugInfo* pFileDebugInfo = pTermDebugInfo->getFileDebugInfo();
        if( pFileDebugInfo && visitedFileDebugInfos.insert( pFileDebugInfo ).second ) {
            delete pFileDebugInfo;
        }
        delete pTermDebugInfo;
    }
    m_mapDebugInfos.clear();
    m_lsRetiredDebugInfos.clear();

    std::set<const AbstractTerm*> visitedTerms;
    m_rootState.collectAllTermTrees( visitedTerms );
    std::set<const AbstractTerm*>::const_iterator itTerm, itTermEnd = visitedTerms.end();
    for( itTerm = visitedTerms.begin(); itTerm != itTermEnd; ++itTerm ) {
        delete *itTerm;
    }

    // m_rootState and everything it recursively owns (child ExecutionStates,
    // Clause objects) are destroyed automatically right after this body
    // returns -- see ExecutionState::~ExecutionState().
}


void World::setTermDebugInfo( const AbstractTerm* pTerm, TermDebugInfo* pTermDebugInfo )
{
    // TXWTODO: Lock begin
    /*
     * Do NOT delete a replaced value here: TermDebugInfo objects can be
     * registered under several term keys (see the '->' desugaring in
     * vault-unify-parser.cpp), so the old value may still be referenced by
     * another entry. Park it on the retired list instead; the
     * de-duplicating sweep in ~World() reclaims it exactly once.
     */
    TermDebugMap::iterator it = m_mapDebugInfos.find( pTerm );
    if( it != m_mapDebugInfos.end() && it->second && it->second != pTermDebugInfo ) {
        m_lsRetiredDebugInfos.push_back( it->second );
    }
    m_mapDebugInfos[pTerm] = pTermDebugInfo;
    // TXWTODO: Lock end.
}


TermDebugInfo* World::getTermDebugInfo( const AbstractTerm* pTerm )
{
    // TermDebugInfo* pTermDebugInfo = NULL;
    // TXWTODO: Guard from here.
    TermDebugMap::iterator it = m_mapDebugInfos.find( pTerm );
    if( it != m_mapDebugInfos.end() ) {
        return it->second;
    } else {
        return NULL;
    }
}


#if 0
int World::appendRootClause( const Clause* pClause, ClauseId& out_uid )
{
    static ClauseId nextClauseId;
    m_rootClauses.push_back( pClause );
    out_uid = ++nextClauseId;
    return 0;
}
#endif


void World::init()
{
    WorldPtr spWorld = shared_from_this();
    m_rootState.appendClause( spWorld, new UnifyBuiltinClause() );
    m_rootState.appendClause( spWorld, new PrintBuiltinClause() );
    m_rootState.appendClause( spWorld, new EmitBuiltinClause() );
    m_rootState.appendClause( spWorld, new MemberBuiltinClause() );
    // ROADMAP Phase 2: Arithmetic and comparison builtins (SPEC.md).
    m_rootState.appendClause( spWorld, new ArithEvalBuiltinClause() );
    m_rootState.appendClause( spWorld, new CompareBuiltinClause() );
    // ROADMAP ("for"/"foreach" loops + ranges, language owner request
    // 2026-08-21, SPEC.md): what `foreach`'s synthesized clause and a
    // variable-bounds range desugar to, respectively.
    m_rootState.appendClause( spWorld, new ArrayAtBuiltinClause() );
    m_rootState.appendClause( spWorld, new RangeBuiltinClause() );
}


World::World()
        : m_rootState( NULL )
{
}


/** 
 * Trigger shutdown of this world. 
 *
 * @param spWorld
 *    A shared poin the to this world.
 *    If the shared pointer leaves scope, the world can be considered to
 *    be deleted.
 */
int World::asyncShutdown( WorldPtr spWorld )
{
    if( this != spWorld.get() ) {
        return -EINVAL;
    }
    /// TODO: Trigger shutdown of world object, interrupting all async
    /// Execution states recursively.
    return 0;
}


};

};


