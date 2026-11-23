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
    // Engine item E14: this is the lock the two TXWTODO comments asked for.
    //
    // LOCK ORDER: ExecutionState::appendClause() calls this while already
    // holding clauseDbMutex(), so m_mutexDebugInfos is always taken SECOND
    // and must never be held while taking the clause-db lock.
    Guard g( m_mutexDebugInfos );

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
}


TermDebugInfo* World::getTermDebugInfo( const AbstractTerm* pTerm )
{
    // Engine item E14. The reader needs the lock as much as the writer:
    // a std::map lookup concurrent with a std::map insert is undefined
    // behaviour outright, not a benign word-sized race, and this lookup is
    // reachable from the debugger's thread while the parser is inserting.
    Guard g( m_mutexDebugInfos );

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
    // Engine item E1: builtins belong to no module and have no source
    // file. In particular NOT their own C++ file -- every builtin calls
    // setDebugLocation( "file://" __FILE__, ... ) in its constructor, so
    // taking the origin from there would register a "module" per builtin
    // implementation file and attribute engine internals to it.
    ClauseOrigin originBuiltin;
    originBuiltin.kind = ClauseOrigin::BUILTIN;

    WorldPtr spWorld = shared_from_this();
    m_rootState.appendClause( spWorld, new UnifyBuiltinClause(), originBuiltin );
    m_rootState.appendClause( spWorld, new PrintBuiltinClause(), originBuiltin );
    m_rootState.appendClause( spWorld, new EmitBuiltinClause(), originBuiltin );
    m_rootState.appendClause( spWorld, new MemberBuiltinClause(), originBuiltin );
    // ROADMAP Phase 2: Arithmetic and comparison builtins (SPEC.md).
    m_rootState.appendClause( spWorld, new ArithEvalBuiltinClause(), originBuiltin );
    m_rootState.appendClause( spWorld, new CompareBuiltinClause(), originBuiltin );
    // ROADMAP ("for"/"foreach" loops + ranges, language owner request
    // 2026-08-21, SPEC.md): what `foreach`'s synthesized clause and a
    // variable-bounds range desugar to, respectively.
    m_rootState.appendClause( spWorld, new ArrayAtBuiltinClause(), originBuiltin );
    m_rootState.appendClause( spWorld, new RangeBuiltinClause(), originBuiltin );
    // ROADMAP Phase 2: String operations (concat, compare, match), SPEC.md.
    // Compare already exists via the comparison operators (CompareBuiltinClause
    // above); this adds concat/strlen (parser-recognized, like findall) and
    // the contains/startswith/endswith plain goal builtins (soft-reserved by
    // clause order -- see vault-unify-clause-builtin.hpp's class comments).
    m_rootState.appendClause( spWorld, new ConcatBuiltinClause(), originBuiltin );
    m_rootState.appendClause( spWorld, new StrlenBuiltinClause(), originBuiltin );
    m_rootState.appendClause( spWorld, new ContainsBuiltinClause(), originBuiltin );
    m_rootState.appendClause( spWorld, new StartswithBuiltinClause(), originBuiltin );
    m_rootState.appendClause( spWorld, new EndswithBuiltinClause(), originBuiltin );
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


/*
 * Engine item E1 (clause provenance): the module registry.
 *
 * A vector for the id space and a map for the lookup, rather than one
 * std::map<std::string,ModuleId> that also has to be reverse-searched:
 * moduleFile() is on the catalogue's path, and scanning a map to turn an id
 * back into a file would make listing a large world quadratic for no reason.
 *
 * No locking here -- appendClause() holds World::clauseDbMutex() across the
 * call. See moduleIdForFile()'s declaration for why that is deliberate.
 */
ModuleId World::moduleIdForFile( const std::string& uriFile )
{
    if( uriFile.empty() ) {
        return NO_MODULE;
    }

    std::map<std::string, ModuleId>::const_iterator it
        = m_mapModuleIds.find( uriFile );
    if( it != m_mapModuleIds.end() ) {
        return it->second;
    }

    m_lsModuleFiles.push_back( uriFile );
    const ModuleId id = (ModuleId) m_lsModuleFiles.size();
    m_mapModuleIds[uriFile] = id;
    return id;
}


const std::string& World::moduleFile( ModuleId id ) const
{
    static const std::string strEmpty;
    if( id == NO_MODULE || id > m_lsModuleFiles.size() ) {
        return strEmpty;
    }
    return m_lsModuleFiles[(size_t)(id - 1)];
}



/*
 * Engine item E2 (definition catalogue).
 *
 * Maintained incrementally at the two points where the clause database
 * changes -- appendClause() and retract's retire() -- rather than rebuilt
 * on demand. Rebuilding means walking every clause in the World to answer
 * "what is defined?", which is what the REPL's `:list` does and what makes
 * it O(database) per keystroke in a browser that asks after every edit.
 *
 * No locking in catalogueAppend()/catalogueRetire(): both are called from
 * inside a critical section that already holds clauseDbMutex(). The reader
 * (copyCatalogue) takes it, which is the reader-safety half of engine item
 * E14 -- see that function's declaration.
 */
static bool catalogueKeyForClause(
        const vault::unify::Clause* pClause,
        vault::unify::PredicateKey& out_key )
{
    if( !pClause ) {
        return false;
    }
    const vault::unify::ConsTerm* pHead = pClause->leftHandTerm();
    if( !pHead ) {
        return false;
    }
    out_key.name = pHead->getName().value();
    out_key.arity = pHead->getArity();
    out_key.module = pClause->getOrigin().module;
    return true;
}


void World::catalogueAppend( const Clause* pClause )
{
    PredicateKey key;
    if( !catalogueKeyForClause( pClause, key ) ) {
        return;
    }

    std::map<PredicateKey, CatalogueEntry>::iterator it
        = m_mapCatalogue.find( key );
    if( it == m_mapCatalogue.end() ) {
        CatalogueEntry entry;
        entry.key = key;
        entry.kind = pClause->getOrigin().kind;
        entry.uriFile = pClause->getOrigin().uriFile;
        entry.firstLine = pClause->getOrigin().line;
        entry.clauseCount = 1;
        entry.retiredCount = 0;
        entry.generation = pClause->getAppendGeneration();
        m_mapCatalogue[key] = entry;
        return;
    }

    ++it->second.clauseCount;
    it->second.generation = pClause->getAppendGeneration();
}


void World::catalogueRetire( const Clause* pClause )
{
    PredicateKey key;
    if( !catalogueKeyForClause( pClause, key ) ) {
        return;
    }

    std::map<PredicateKey, CatalogueEntry>::iterator it
        = m_mapCatalogue.find( key );
    if( it == m_mapCatalogue.end() ) {
        return;
    }

    if( it->second.clauseCount > 0 ) {
        --it->second.clauseCount;
    }
    ++it->second.retiredCount;
    it->second.generation = pClause->getRetireGeneration();

    // The entry is deliberately NOT erased when clauseCount reaches zero.
    // A predicate every clause of which has been retracted still exists as
    // far as the database is concerned -- its tombstones are still walked
    // by every goal -- and a browser that made such a predicate vanish
    // would hide exactly the state a user is most likely to be debugging.
    // retiredCount is what says what happened.
}


void World::copyCatalogue( std::vector<CatalogueEntry>& out_lsEntries )
{
    Guard g( clauseDbMutex() );

    out_lsEntries.clear();
    out_lsEntries.reserve( m_mapCatalogue.size() );
    std::map<PredicateKey, CatalogueEntry>::const_iterator it;
    for( it = m_mapCatalogue.begin(); it != m_mapCatalogue.end(); ++it ) {
        out_lsEntries.push_back( it->second );
    }
}


bool World::findCatalogueEntry( const PredicateKey& key,
                                CatalogueEntry& out_entry )
{
    Guard g( clauseDbMutex() );

    std::map<PredicateKey, CatalogueEntry>::const_iterator it
        = m_mapCatalogue.find( key );
    if( it == m_mapCatalogue.end() ) {
        return false;
    }
    out_entry = it->second;
    return true;
}



};

};


