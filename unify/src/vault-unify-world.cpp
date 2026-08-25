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
 * 1) TermDebugInfo/FileDebugInfo. Every TermDebugInfo is exclusively owned
 *    by World (ExecutionState::appendClause() and AnyTermFactory::
 *    operator()(ConsTermInput), vault-unify-parser.cpp, both always
 *    construct a fresh one). FileDebugInfo is different: since ROADMAP
 *    Phase 2 ("File imports / include") gave parseExecuteSegment() a real,
 *    non-NULL pFileDebugInfo (unify-run.cpp for the main program, and
 *    RuntimeContext itself for each imported file -- forwarded via
 *    PrologParser::Context::setFileDebugInfo()), the SAME FileDebugInfo
 *    pointer is now genuinely aliased across every TermDebugInfo built while
 *    parsing that one file (AnyTermFactory::operator()(ConsTermInput) reads
 *    it back via Context::getFileDebugInfo() for each), so de-duplicating
 *    before deleting is load-bearing here, not merely defensive. It is not
 *    sufficient on its own, though: a file that never builds a single term
 *    (e.g. one containing only further `import` statements or comments)
 *    would never be reached by this discovery at all -- see
 *    adoptFileDebugInfo()'s comment (include/vault-unify.hpp) for the
 *    explicit registry that closes that gap, swept together with this one
 *    below via the same de-duplicating set. This whole pass runs first,
 *    while every AbstractTerm* map key (whatever it points at, or used to)
 *    is still exactly the value it was inserted with; nothing here
 *    dereferences a key, only the mapped TermDebugInfo / FileDebugInfo
 *    pointer values.
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

    /*
     * ROADMAP Phase 2 ("File imports / include", SPEC.md section 15):
     * adoptFileDebugInfo()-registered FileDebugInfos (see that method's
     * comment, include/vault-unify.hpp) -- one per file parseExecuteSegment
     * is ever handed (the main program, each distinct imported file). Fed
     * into the SAME visitedFileDebugInfos set as the TermDebugInfo-reachable
     * ones above, so a FileDebugInfo discovered both ways (the common case:
     * a file that built at least one term) is still deleted exactly once,
     * while one discovered ONLY here (a file that built zero terms, e.g. an
     * import-only or comment-only file) is no longer left leaked.
     */
    std::list<FileDebugInfo*>::const_iterator itOwnedFDI, itOwnedFDIEnd = m_lsOwnedFileDebugInfos.end();
    for( itOwnedFDI = m_lsOwnedFileDebugInfos.begin(); itOwnedFDI != itOwnedFDIEnd; ++itOwnedFDI ) {
        const FileDebugInfo* pFileDebugInfo = *itOwnedFDI;
        if( pFileDebugInfo && visitedFileDebugInfos.insert( pFileDebugInfo ).second ) {
            delete pFileDebugInfo;
        }
    }
    m_lsOwnedFileDebugInfos.clear();

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
    // ROADMAP Phase 2: String operations (concat, compare, match), SPEC.md.
    // Compare already exists via the comparison operators (CompareBuiltinClause
    // above); this adds concat/strlen (parser-recognized, like findall) and
    // the contains/startswith/endswith plain goal builtins (soft-reserved by
    // clause order -- see vault-unify-clause-builtin.hpp's class comments).
    m_rootState.appendClause( spWorld, new ConcatBuiltinClause() );
    m_rootState.appendClause( spWorld, new StrlenBuiltinClause() );
    m_rootState.appendClause( spWorld, new ContainsBuiltinClause() );
    m_rootState.appendClause( spWorld, new StartswithBuiltinClause() );
    m_rootState.appendClause( spWorld, new EndswithBuiltinClause() );
}


World::World()
        : m_rootState( NULL, this )
        , m_mutationGeneration( 0 )
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


