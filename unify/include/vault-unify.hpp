#if !defined( _VAULT_UNIFY_HPP )
#define _VAULT_UNIFY_HPP

/**
 * @file vault-unification.cpp
 *
 * @author Timo Weggen
 *
 * Implementation of unification engine.
 */

#include <stdint.h>
#include <stdio.h>
#include <errno.h>
 
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/function.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>
#include <boost/lexical_cast.hpp>

#include <string>
#include <map>
#include <vector>
#include <list>
#include <set>

/*
 * Debug trace categories. Guarded so a build can override single
 * categories from the compiler command line (e.g. -DVAULT_UNIFY_ITERATE=1)
 * without editing this header.
 */
#if !defined( VAULT_UNIFY_ALWAYS )
#define VAULT_UNIFY_ALWAYS 1
#endif
#if !defined( VAULT_UNIFY_SOLUTION )
#define VAULT_UNIFY_SOLUTION 0
#endif
#if !defined( VAULT_UNIFY_ITERATE )
#define VAULT_UNIFY_ITERATE 0
#endif
#if !defined( VAULT_UNIFY_UNIFY )
#define VAULT_UNIFY_UNIFY 1
#endif
#if !defined( VAULT_UNIFY_SCHEDULE )
#define VAULT_UNIFY_SCHEDULE 0
#endif
#if !defined( VAULT_UNIFY_SERVER )
#define VAULT_UNIFY_SERVER 1
#endif
#if !defined( VAULT_UNIFY_PARSER )
#define VAULT_UNIFY_PARSER 0
#endif
/*
 * Runtime master switch for every trace category above, ANDed with the
 * compile-time flag so it can only ever silence output, never add any.
 * Enabled by default, i.e. a build that does not touch it behaves exactly
 * as it always did.
 *
 * It exists for the interactive REPL (tools/unify-repl.cpp): a prompt
 * whose every keystroke is answered by a page of engine trace on stderr is
 * not usable, and the alternative -- telling the user to run it with
 * `2>/dev/null`, as README.md quite reasonably does for a batch run --
 * would throw away parse-error diagnostics along with the trace, since
 * both go to stderr. Nothing else in the engine calls the setter.
 *
 * Declared here, ahead of `namespace vault`, because VAULT_UNIFY_DI is
 * expanded at call sites both inside and outside that namespace; the
 * definition lives in vault-unify-misc.cpp.
 */
namespace vault { namespace unify {
bool isDebugTraceEnabled();
void setDebugTraceEnabled( bool enabled );
} }

#define VAULT_UNIFY_DI( X, ... ) do { if( VAULT_UNIFY_ ## X && ::vault::unify::isDebugTraceEnabled() ) { fprintf( stderr, __FILE__ ":%s():%d:", __func__, __LINE__ ); fprintf( stderr, __VA_ARGS__ ); } } while(0)


// This is an experimental feature.
#define VAULT_UNIFY_USE_VIRTUAL_DEREF 0

#include <vault-unify-debug-interface.hpp>


namespace vault {
namespace unify {

enum UnifyResult {
    UnifyError = -1,
    UnifyNot = 0,
    UnifyLast = 1,
    UnifyNotLast = 2
};

static inline bool Unifies( UnifyResult unifyResult ) { return unifyResult==UnifyLast || unifyResult==UnifyNotLast; }


// Convenience
typedef boost::unique_lock<boost::mutex> Guard;

class TermDebugInfo;
// ROADMAP Phase 2 ("File imports / include", SPEC.md section 15): forward
// declared here (ahead of its previous sole forward declaration near
// RuntimeContext further down, kept there too -- repeating a forward
// declaration of an incomplete type is harmless) so World::
// adoptFileDebugInfo()/m_lsOwnedFileDebugInfos below can name
// `FileDebugInfo*` without needing its full definition (vault-unify-debug.hpp
// is a src/-private header; only .cpp files that actually construct/inspect
// one include it).
class FileDebugInfo;

class Atom
{
public:
    Atom( const std::string& value ) : m_value( value ) {}
    
    const std::string& value() const { return m_value; }
    bool operator == ( const Atom& other ) const {
        return m_value == other.m_value; }
    bool operator != ( const Atom& other ) const {
        return m_value != other.m_value; }
    bool operator < ( const Atom& other ) const {
        return m_value < other.m_value; }

private:
    std::string m_value;
};


class Goal;
class ConsTerm;
class Clause;
class UnifyContext;


/**
 * Identifies one loaded module -- one .ufy file, the transcript, or the
 * anonymous module a bare World starts with.
 *
 * Introduced with clause provenance (plans/todo/lens/ACCEPTANCE.md, engine
 * item E1). Ids are assigned by World::moduleIdForFile() in first-seen
 * order and are stable for the life of a World; 0 is "no module", which is
 * what a builtin and a runtime-asserted clause carry.
 */
typedef uint64_t ModuleId;

/// See ModuleId: never a valid module.
const ModuleId NO_MODULE = 0;


/**
 * Where a clause came from -- engine item E1.
 *
 * Before this existed, "is this user code?" was inferred by the REPL from a
 * `dynamic_cast<const SimpleBuiltinClause*>` plus a `__` head-name prefix
 * test (unify-run's `:list`). Both were guesses that happened to be right:
 * the cast because every builtin is currently that one subclass, the prefix
 * because the parser happens to name its desugared clauses that way. Neither
 * survives contact with a user predicate legitimately named `__cache`, and
 * neither can answer the questions the catalogue, the source view and the
 * image writer actually ask -- which file, which line, which module.
 *
 * So the answer is recorded when it is known -- at the one append -- rather
 * than reconstructed later from the shape of the name.
 */
struct ClauseOrigin {
    enum Kind {
        /// Parsed from a module's source text.
        MODULE = 0,
        /// Created at run time by assert()/asserta()/assertz().
        ASSERTED,
        /// Provided by the engine itself (World::init()).
        BUILTIN,
        /// Produced by parse-time desugaring: __if__N, __fe__N, __feb__N,
        /// __for__N. Real clauses, but never the user's own text -- an
        /// image that printed these back would emit machine output instead
        /// of the program.
        SYNTHESIZED,
        /// Typed at an interactive prompt.
        TRANSCRIPT
    };

    ClauseOrigin()
        : kind( MODULE )
        , module( NO_MODULE )
        , line( 0 )
        , endLine( 0 )
    {
    }

    Kind        kind;
    ModuleId    module;

    /// The source file, when there is one. Empty for BUILTIN, ASSERTED and
    /// TRANSCRIPT. Duplicated from the clause's DebugLocation rather than
    /// referenced, because a DebugLocation is optional and provenance is not.
    std::string uriFile;

    /// 1-based; 0 when unknown.
    uint64_t    line;

    /// 1-based end of the clause's source span; 0 when unknown. Set for
    /// MODULE and TRANSCRIPT clauses so the source view can show a whole
    /// definition rather than re-deriving its extent.
    uint64_t    endLine;

    /// True for clauses the user did not write and would not expect to see
    /// listed: builtins and desugaring artefacts. The one place this
    /// judgement is made, so it cannot drift between the REPL, the
    /// catalogue and the image writer.
    bool isInternal() const { return kind == BUILTIN || kind == SYNTHESIZED; }
};


/**
 * A predicate's identity -- engine item E2.
 *
 * The unit the catalogue, the browser, the source view and `undefine` all
 * address. Module is part of the key because the same name/arity in two
 * modules is two predicates.
 */
struct PredicateKey {
    PredicateKey()
        : arity( 0 )
        , module( NO_MODULE )
    {
    }

    PredicateKey( const std::string& strName, int nArity, ModuleId idModule )
        : name( strName )
        , arity( nArity )
        , module( idModule )
    {
    }

    std::string name;
    int         arity;
    ModuleId    module;

    /// Ordered so the catalogue can be a std::map and a listing is stable
    /// without a sort: module, then name, then arity.
    bool operator < ( const PredicateKey& other ) const {
        if( module != other.module ) { return module < other.module; }
        if( name != other.name ) { return name < other.name; }
        return arity < other.arity;
    }
    bool operator == ( const PredicateKey& other ) const {
        return module == other.module
            && arity == other.arity
            && name == other.name;
    }
};


/**
 * One row of the definition catalogue -- engine item E2.
 *
 * The catalogue exists because the only way to answer "what predicates are
 * defined?" used to be to walk the whole clause list and group by head name
 * -- which is O(total clauses) per question, is what the REPL's `:list`
 * does today, and is unusable for a browser that asks after every edit.
 * It is also, not incidentally, the index ROADMAP Phase 3's first-argument
 * indexing needs: built once, used twice.
 */
struct CatalogueEntry {
    CatalogueEntry()
        : kind( ClauseOrigin::MODULE )
        , firstLine( 0 )
        , clauseCount( 0 )
        , retiredCount( 0 )
        , generation( 0 )
    {
    }

    PredicateKey       key;

    /// The provenance of this predicate's clauses (engine item E1). A
    /// predicate whose clauses disagree reports the first clause's kind;
    /// mixing builtin and user clauses under one key is not possible today.
    ClauseOrigin::Kind kind;

    /// Where the first clause was defined, for "jump to definition".
    std::string        uriFile;
    uint64_t           firstLine;

    /// Live clauses -- tombstones excluded.
    uint32_t           clauseCount;

    /**
     * Tombstoned clauses still occupying the clause list.
     *
     * Exposed rather than hidden because clauses are never removed (see
     * Clause::isRetired()), so repeated redefinition grows the list
     * monotonically and every future goal walks past the corpses. This
     * counter is what lets that degradation be MEASURED instead of
     * discovered as a mysterious slowdown -- it is the number gate G3.7
     * of plans/todo/lens/ACCEPTANCE.md is written against.
     */
    uint32_t           retiredCount;

    /// World mutation generation of the last change to this predicate.
    uint64_t           generation;
};


class AbstractTerm;
class TermTraversable;


class AbstractTermIterator {
public:
    virtual ~AbstractTermIterator();
    virtual bool isValid() const = 0;
    virtual const TermTraversable* getTermTraversable() const = 0;
    virtual void next()  = 0;
};


class TermTraversable {
public:
    virtual ~TermTraversable();
    virtual AbstractTermIterator* abstractTermIterator() const = 0;

    template<typename VisitorType> void applyVisitor( VisitorType visitor ) const {
        visitor( this );
        AbstractTermIterator* ti = abstractTermIterator();
        // Do we have children?
        if( ti ) {
            while( ti->isValid() ) {
                const TermTraversable* term = ti->getTermTraversable();
                if( term ) {
                    term->applyVisitor( visitor );
                }
                ti->next();
            }
        }
    }
};


struct Goal
        : public TermTraversable
{
    Goal() {}

    Goal( const AbstractTerm* ct1 ) {
        m_listAbstractTerms.push_back( ct1 );
    }
    
    Goal( const AbstractTerm* ct1, const AbstractTerm* ct2 ) {
        m_listAbstractTerms.push_back( ct1 );
        m_listAbstractTerms.push_back( ct2 );
    }
    
    Goal( const AbstractTerm* ct1, const AbstractTerm* ct2, const AbstractTerm* ct3 ) {
        m_listAbstractTerms.push_back( ct1 );
        m_listAbstractTerms.push_back( ct2 );
        m_listAbstractTerms.push_back( ct3 );
    }
    
    template <typename Iterator> Goal( Iterator begin, Iterator end ) {
        for( ; begin != end; ++begin ) {
            m_listAbstractTerms.push_back( *begin );
        }
    }
    
    bool isEmpty() const { return m_listAbstractTerms.empty(); }

    class GoalIterator 
            : public AbstractTermIterator 
    {
    public:
        GoalIterator() : m_goal( NULL ), m_idx( 0 ) {}
        GoalIterator( const Goal* goal ) : m_goal( goal ), m_idx( 0 ) {
            m_it = m_goal->m_listAbstractTerms.begin();
            m_itEnd = m_goal->m_listAbstractTerms.end();
        }
        GoalIterator( const GoalIterator& other )
                : m_goal( other.m_goal )
                , m_idx( other.m_idx )
                , m_it( other.m_it )
                , m_itEnd( other.m_itEnd )
                {}
        GoalIterator& operator=( const GoalIterator& other ) = default;

        virtual bool isValid() const {
            return m_goal && m_it != m_itEnd; 
        }
        
        const AbstractTerm* getAbstractTerm() const {
            return *m_it;
        }
        
        const AbstractTerm* getTerm() const {
            return (const AbstractTerm*) *m_it;
        }
        
        virtual const TermTraversable* getTermTraversable() const;
        
        virtual void next() {
            ++m_it;
        }
        
        int getIndex() const { return m_idx; }

    private:
        const Goal* m_goal;
        int m_idx;
        std::list<const AbstractTerm*>::const_iterator m_it, m_itEnd;
    };

    std::string toString() const;
    std::string toJSON( bool useContent, const UnifyContext* pUCStackTop, const UnifyContext* pUCTerm ) const;
    
    AbstractTermIterator* abstractTermIterator() const {
        return new GoalIterator( this );
    }
    GoalIterator termIterator() const {
        return GoalIterator( this );
    }

    std::list<const AbstractTerm*> m_listAbstractTerms;
};


class Engine;
class World;
typedef boost::shared_ptr<World> WorldPtr;


/** 
 * An execution state contains all information about a current unification
 * process. ExecutionStates form a tree.
 *
 * Reading a clause from a source means to create an execution context for 
 * that source and to add the clause to the list of clauses for his particular 
 * execution context.
 *
 * Unifying a goal means fork a child contexts recursively.
 * For each clause the program found an execution state is created.
 * The list of goals to prove is filled with the terms found in the goal.
 * The main task of the execution stack is to prove these clauses.
 * 
 * A new execution state therefore can be created from either of:
 * - A parent context and a goal, which in turn should be unified.
 * - A parent context and a list of clauses, which should be added to
 *   the current database.
 * - A parent context plus a list of var mappings.
 *
 * Goals are defined as asynchrononus, if at least on of their terms
 * is an asynchronous condition.
 *
 * Goals can be attributed sequential: In that case, their terms
 * have to be evaluated sequentially (default).
 *
 * Goals can be attributed parallel: In that case, their terms can
 * be evaluated in any order.
 */
struct ExecutionState {
public:
    ExecutionState( ExecutionState* parent, World* pWorld );
    ~ExecutionState();

    class ClauseIterator {
    private:
        void enterState( ExecutionState* es )
        {
            m_currentState = es;
            m_itClause = es->m_listClauses.begin();
            m_itClauseEnd = es->m_listClauses.end();
        }

    public:
        ClauseIterator() : m_currentState( NULL ), m_invalidated( false ), m_snapshotGen( 0 ) {}

        /**
         * ROADMAP Phase 2 (runtime assert/retract, SPEC.md -- "logical
         * update view"): captures `es`'s World's CURRENT mutation
         * generation as this iterator's snapshotGen -- this is the whole
         * mechanism that makes the iteration see the clause database
         * exactly as it stood at THIS moment, regardless of what asserts/
         * retracts happen later while it is still in progress (see
         * isValid() below). Body lives out-of-line
         * (vault-unify-execution-state.cpp), like isValid(), because it
         * needs World's complete definition (only forward-declared at this
         * point in the header) to call currentGeneration(). This read is
         * deliberately UNLOCKED -- see World::clauseDbMutex()'s comment for
         * why that is a currently-benign, ROADMAP-5.1-scoped race.
         *
         * "THIS moment" means when THIS constructor call runs, nothing
         * more -- for a job's ROOT SolveContext (only), that can be far
         * earlier than "when this search actually starts" (a top-level
         * query's root SolveContext is built by SolveJob::startJob() on
         * the PARSER thread, at parse time); SolveJob::performSlice()
         * (vault-unify-solvejob.cpp) knows about exactly that one case and
         * re-invokes this same constructor (via ExecutionState::
         * clauseIterator()) once, on the root context's first slice, to
         * replace that stale snapshot with one taken at actual execution
         * time -- see its comment and SPEC.md's logical-update-view
         * section for why. Every non-root ClauseIterator is already
         * constructed live, while its job is actually running, so no such
         * correction is ever needed for one of those.
         */
        ClauseIterator( ExecutionState* es );
        ~ClauseIterator() {}

        /**
         * ROADMAP Phase 2 (Cut): returns false immediately if invalidate()
         * (below) was ever called, checked before the natural exhaustion/
         * parent-fallback walk so a cut takes effect regardless of how many
         * candidates (in this or any parent ExecutionState) remain.
         *
         * ROADMAP Phase 2 (runtime assert/retract, SPEC.md -- "logical
         * update view"): otherwise, skips forward over any clause not
         * VISIBLE to this iterator's snapshotGen (captured at construction,
         * see above): a clause is visible iff its
         * `getAppendGeneration() <= snapshotGen` (an assert()/parse-time
         * append that happened AFTER this iteration started is invisible --
         * it never existed yet, from this iteration's point of view) AND
         * (`!isRetired()` OR `getRetireGeneration() > snapshotGen`) (a
         * retract() that happened BEFORE this iteration started makes the
         * clause invisible the whole time; one that happens DURING/AFTER
         * this iteration started leaves the clause visible to THIS
         * iteration regardless of when in its own scan it reaches that
         * clause's position -- this iteration's view of the database is
         * frozen at snapshotGen). This is checked freshly on every call
         * (never cached), but the comparison itself, not the freshness, is
         * what implements the "one fixed view for the iteration's whole
         * lifetime" contract -- this method's BODY lives out-of-line in
         * vault-unify-execution-state.cpp rather than inline here like the
         * rest of this class because Clause is still only forward-declared
         * at this point in the header, and the generation accessors need it
         * complete.
         */
        bool isValid();

        const Clause* getClause() const {
            return *m_itClause;
        }

        void next() {
            ++m_itClause;
        }

        /**
         * ROADMAP Phase 2 (Cut): force this iterator to report no further
         * candidates from now on, regardless of how many clauses remain
         * unvisited (in this ExecutionState or any parent one isValid()
         * would otherwise fall back into). Used by SolveJob::performSlice()
         * to prune choice points when a `cut;` goal executes -- see
         * SPEC.md's cut section and the design notes in
         * vault-unify-solvejob.cpp. Idempotent; does not touch
         * getClause()/next(), exactly like natural exhaustion, callers
         * must stop calling those once isValid() reports false.
         */
        void invalidate() {
            m_invalidated = true;
        }

    private:
        ExecutionState* m_currentState;
        std::list<Clause*>::const_iterator m_itClause;
        std::list<Clause*>::const_iterator m_itClauseEnd;

        /// See invalidate()/isValid() above.
        bool m_invalidated;

        /// See the constructor's and isValid()'s comments above -- the
        /// mutation generation this iterator's whole traversal is pinned
        /// to. 0 for a default-constructed (no ExecutionState) iterator,
        /// which is never walked for real candidates (see the callers that
        /// use the default constructor purely to satisfy a UnifyContext
        /// parameter for a throwaway trial unification).
        uint64_t m_snapshotGen;
    };
    const ClauseIterator clauseIterator() {
        return ClauseIterator( this );
    }

    /**
     * Create a child execution state from this execution state.
     */
    ExecutionState* fork();

    /**
     * Append a clause to this execution state.
     * This is an atomic operation with respect to the execution state.
     * A clause cannot be removed from an execution state.
     *
     * @param pClause
     *     The execution context takes ownership of the clause.
     * @param origin
     *     Where this clause came from -- engine item E1. Required rather
     *     than defaulted: this is the single choke point through which
     *     every clause in the database passes, so it is the one place where
     *     forgetting to say is caught by the compiler instead of producing
     *     a database that quietly claims everything is user code.
     *
     *     The caller fills in `kind`, `uriFile` and `line`; `module` is
     *     resolved here from `uriFile` via World::moduleIdForFile().
     *
     *     Note that the origin is NOT derived from the clause's
     *     DebugLocation, which would have been the obvious shortcut and is
     *     wrong twice over: the parser never sets a DebugLocation on a
     *     clause at all (only on terms, and only via the World's debug-info
     *     map), so every parsed clause would have come out with no file;
     *     and every builtin DOES set one -- to its own C++ source file and
     *     line -- so every builtin would have been assigned a module named
     *     after vault-unify-clause-builtin-print.cpp. The first version of
     *     E1 did exactly this and the engine test caught both.
     */
    int appendClause( WorldPtr spWorld, Clause*, const ClauseOrigin& origin );

    /**
     * Collect (without deleting) every term reachable from every clause in
     * this state, and recursively from every child state, into out_visited.
     * A clause's head/body can alias within itself (see the ownership note
     * above AbstractTerm's collectTermTree()/deleteTermTree() declarations),
     * so the whole ExecutionState tree is collected into ONE de-duplicated
     * set before anything is deleted -- see World::~World().
     */
    void collectAllTermTrees( std::set<const AbstractTerm*>& out_visited );

    Engine* m_pEngine;

    ExecutionState* m_pParent;

    /// ROADMAP Phase 2 (runtime assert/retract, SPEC.md): the World this
    /// state belongs to -- needed by ClauseIterator's constructor to read
    /// the current mutation generation (World::currentGeneration()) as its
    /// snapshotGen. Only the root ExecutionState (World::getRootState()) is
    /// actually used anywhere today (fork()/m_listChildStates are unused,
    /// dead code), but every ExecutionState -- root or forked child --
    /// shares the one World it was created in or under.
    World* m_pWorld;

    /// Clauses as added by this execution state.
    std::list<Clause*> m_listClauses;

    /// Child execution states spawned by this execution state.
    std::list<ExecutionState*> m_listChildStates;
};


class AbstractTerm;

/**
 * Every single var term that we declare has an id.
 */
typedef uint32_t VarTermId;

/**
 * Because a varterm can have a different binding in every clause it appers in,
 * it is convenient to have an id for the unify context of the clause as well.
 */ 
typedef uint32_t UnifyContextId;


/**
 * Every variable is valid in a given instantiation scope only.
 * This AssignmentId is the key to look up a variable.
 */
class AssignmentId {
public:
    AssignmentId(
            UnifyContextId uidUnifyContext,
            VarTermId uidVarTerm ) 
        : m_uidUnifyContext( uidUnifyContext )
        , m_uidVarTerm( uidVarTerm ) {}

    UnifyContextId getUnifyContextId() const {
        return m_uidUnifyContext;
    }
    
    VarTermId getVarTermId() const {
        return m_uidVarTerm;
    }
    
    bool operator < ( const AssignmentId& other ) const {
        return (m_uidUnifyContext < other.m_uidUnifyContext)
                || (m_uidUnifyContext == other.m_uidUnifyContext
                  && m_uidVarTerm < other.m_uidVarTerm);
    }

protected:
private:
    UnifyContextId m_uidUnifyContext;
    VarTermId m_uidVarTerm;
};

class UnifyContext;

/**
 * Original clauses also have ids assigned.
 */
typedef uint64_t ClauseId;

class VarTerm;
class ConsTerm;
class MapTerm;
class ArrayTerm;


/**
 * This is the base class of all terms.
 */
class AbstractTerm
    : public TermTraversable
{
public:
    virtual ~AbstractTerm() {};


#if VAULT_UNIFY_USE_VIRTUAL_DEREF
    /**
     * Perform pre-unification operations on term tree.
     * This includes the type of '->' deref operations.
     */
    virtual const AbstractTerm* deref() const;
#endif

    int getBoundTerm(
        const UnifyContext* pUCStackTop,
        const UnifyContext* pUCInput,
        // const AbstractTerm* pInputTerm,
        const AbstractTerm*& out_pTerm,
        UnifyContext*& out_pUCOutput ) const ;


    /**
     * Unify this term with an arbitrary abstract term. This is the 
     * first part of a double dispatch function call. The second part
     * unifyConsTerm.
     *
     * First order call: this points to the goal term, other points to the
     * clause term.
     *
     * @param pUCOriginal
     *     The unify context this ConsTerm was "instantiated" in.
     * @param pUCCand
     *     The unify context we should emit declarations to.
     * @param pAbstractTerm
     *     The term that we should try to unify with. This term origins in
     *     some clause.
     *
     * @return 
     *     1 if unifies, 0 if not.
     */
    virtual UnifyResult unifyTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOther,
        UnifyContext* pUCMine,
        const AbstractTerm* pOther ) const = 0;

    /**
     * Unify this term with a consterm. This is the second part of a double
     * dispatch function call.
     *
     * Second order call: This points to the clause term, other points to the 
     * goal term.
     *
     * @param pUCOriginal
     *     The unify context this ConsTerm was "instantiated" in.
     * @param pUCCand
     *     The unify context we should emit declarations to.
     * @param pAbstractTerm
     *     The term that we should try to unify with. This term origins in
     *     some clause.
     *
     * @return 
     *     1 if unifies, 0 if not.
     */
    virtual UnifyResult unifyConsTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOther,
        UnifyContext* pUCMine,
        const ConsTerm* pOther ) const = 0;

    /**
     * Unify this term with a varterm. This is the second part of a double
     * dispatch function call.
     *
     * @param pUCOriginal
     *     The unify context this ConsTerm was "instantiated" in.
     * @param pUCCand
     *     The unify context we should emit declarations to.
     * @param pOther
     *     The term that we should try to unify with. This term origins in
     *     some clause.
     *
     * Second order call: This points to the clause term, other points to the 
     * goal term.
     *
     * @return 
     *     1 if unifies, 0 if not.
     */
    virtual UnifyResult unifyVarTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const VarTerm* pOther ) const = 0;

    /**
     * Unify this term with a mapterm. This is the second part of a double
     * dispatch function call.
     *
     * @param pUCOriginal
     *     The unify context this ConsTerm was "instantiated" in.
     * @param pUCCand
     *     The unify context we should emit declarations to.
     * @param pOther
     *     The term that we should try to unify with. This term origins in
     *     some clause.
     *
     * Second order call: This points to the clause term, other points to the 
     * goal term.
     *
     * @return 
     *     1 if unifies, 0 if not.VarTermId
     */
    virtual UnifyResult unifyMapTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const MapTerm* pOther ) const = 0;

    /**
     * Unify this term with an array term. This is the second part of a
     * double dispatch function call (ROADMAP Phase 2, "consistent
     * list/array semantics" -- SPEC.md section 11).
     *
     * @param pUCOriginal
     *     The unify context this ConsTerm was "instantiated" in.
     * @param pUCCand
     *     The unify context we should emit declarations to.
     * @param pOther
     *     The term that we should try to unify with. This term origins in
     *     some clause.
     *
     * Second order call: This points to the clause term, other points to the
     * goal term.
     *
     * @return
     *     1 if unifies, 0 if not.
     */
    virtual UnifyResult unifyArrayTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const ArrayTerm* pOther ) const = 0;

    /**
     * Expensive function for testing purposes.
     */
    virtual const std::string toString() const = 0;
    
    virtual const std::string toContextString(
        const UnifyContext* pUCStackTop,
        const UnifyContext* pUCTerm ) const = 0;

    virtual const std::string toJSON( bool useContent, const UnifyContext* pUCStackTop, const UnifyContext* pUCTerm ) const = 0;

#if 0
    template<typename VisitorType> void applyVisitor( VisitorType visitor ) {
        visitor( term );
        AbstractTermIterator* ti = abstractTermIterator();
        // Do we have children?
        if( ti ) {
            while( ti->isValid() ) {
                AbstractTerm* term = ti->getTerm();
                if( term ) {
                    term->applyVisitor( visitor );
                }
                ti->next();
            }
        }
    }
#endif
private:
};


/**
 * ROADMAP Phase 1 (Ownership model), pass 2: program-lifetime term memory.
 *
 * Term trees allocated at parse time (clause heads/bodies, builtin clause
 * heads) are NOT simply "one tree per owner": AnyTermFactory
 * (vault-unify-parser.cpp) resolves repeated variable names within one
 * ClauseContext to the SAME VarTerm*, and that ClauseContext is shared
 * across an entire top-level clause (its head AND its body) or an entire
 * top-level query -- so e.g. a clause's own head and body legitimately
 * share a repeated variable's VarTerm*.
 *
 * `if( cond ) { ... }` statements used to make this worse by sharing a
 * VarTerm across an auxiliary clause synthesized straight into the World's
 * clause database and the call-site term left behind in the enclosing
 * clause/query -- see AnyTermFactory::operator()(IfStatementInput)
 * (vault-unify-parser.cpp), which no longer does this: it clones cond/body
 * with cloneTermTree() (declared below) into fresh variables private to
 * each synthesized clause, so the only remaining aliasing is the ordinary
 * intra-clause kind (head/body sharing a repeated variable) described
 * above.
 *
 * That still means a term node can be reachable from more than one place
 * within the SAME clause (head and body). Deleting such a tree one owner
 * at a time risks a double free/use-after-free. The safe pattern is:
 * collectTermTree() every root that might alias another into ONE std::set
 * (which de-duplicates by pointer identity and stops recursing into
 * anything already visited), then delete every pointer in that set exactly
 * once -- see World::~World()/ExecutionState::collectAllTermTrees() for the
 * concrete use (the whole clause database is walked into one such set, a
 * superset of what any single clause needs, but still safe and simple).
 *
 * deleteTermTree() is a convenience for the simple case: a single call site
 * that owns pTerm's entire tree exclusively, with no aliasing into anything
 * outside that one call -- e.g. a query's own Goal term trees (see
 * ~SolveJob(), vault-unify-solvejob.cpp), now that they can no longer have
 * been aliased into the World's clause database by an `if` statement. Do
 * not use it on a clause's head/body -- those can still alias each other
 * intra-clause -- use collectTermTree() into a shared set instead.
 */
void collectTermTree( const AbstractTerm* pTerm, std::set<const AbstractTerm*>& out_visited );
void deleteTermTree( const AbstractTerm* pTerm );

/**
 * Deep-clone pTerm's whole subtree for AnyTermFactory::operator()(IfStatementInput)'s
 * `if` desugaring (vault-unify-parser.cpp), so a synthesized auxiliary
 * clause can get its own variables instead of aliasing the enclosing
 * clause/query's term tree.
 *
 * Every VarTerm encountered is looked up in varSubstitution and replaced
 * by its mapped fresh VarTerm* -- the SAME source VarTerm* always maps to
 * the SAME fresh VarTerm*, so shared structure (e.g. one variable used
 * twice in cond+body) is preserved in the clone. Everything else (ConsTerm,
 * MapTerm) is freshly allocated, with children cloned recursively; a
 * MapTerm's Atom* keys are copied too, since ~MapTerm() deletes them (see
 * MapTerm::getEntries()). A VarTerm missing from varSubstitution is an
 * internal-error case -- the caller is expected to have built the map from
 * every VarTerm actually occurring in the subtree first -- logged via
 * VAULT_UNIFY_DI(ALWAYS, ...) and cloned as a fresh, unmapped VarTerm
 * rather than crashing.
 */
AbstractTerm* cloneTermTree( const AbstractTerm* pTerm, const std::map<const VarTerm*, VarTerm*>& varSubstitution );

/**
 * ROADMAP Phase 2 ("findall"): resolve pTerm (typically a findall
 * template, or a sub-term of one) against a SOLVED UnifyContext
 * pUCStackTop, producing a fully independent, GROUND clone: every VarTerm
 * encountered is resolved via findVarBinding()/findVarInstance() (mirroring
 * SolveJob::getSolutionList()'s own resolution) and, if bound, the bound
 * instance term is itself resolved recursively (in ITS OWN binding's
 * UnifyContext) rather than referenced -- every node returned is a fresh
 * allocation, safe to outlive pUCStackTop's own arena. A still-unbound
 * VarTerm is cloned as a fresh, unbound VarTerm (findall never fails on
 * this).
 *
 * pUCTerm is the scope UnifyContext for variables occurring directly in
 * pTerm -- NULL for the initial call from findall's solver special form
 * (SolveJob::performSlice(), vault-unify-solvejob.cpp), since the template
 * sits directly in the nested job's own top-level goal, exactly like the
 * top-level query variables SolveJob::getSolutionList() resolves via
 * AssignmentId(0, ...); see that function and SPEC.md's findall section
 * for the analysis of why this is correct given findall's (documented,
 * v1) fresh-scope subgoal semantics.
 */
AbstractTerm* resolveTermGrounded(
        const AbstractTerm* pTerm,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCTerm );

class ConsTerm;

class MapTerm : public AbstractTerm
{
public:
    MapTerm(
        const Atom**,
        AbstractTerm**,
        int nTuples );
    virtual ~MapTerm();

    virtual UnifyResult unifyTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const AbstractTerm* pOther ) const;
    virtual UnifyResult unifyConsTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const ConsTerm* pOther ) const;
    virtual UnifyResult unifyVarTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const VarTerm* pOther ) const;
    virtual UnifyResult unifyMapTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const MapTerm* pOther ) const;
    virtual UnifyResult unifyArrayTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const ArrayTerm* pOther ) const;

    virtual const std::string toString() const;

    virtual const std::string toJSON( bool useContent, const UnifyContext* pUCStackTop, const UnifyContext* pUCTerm ) const;

    virtual const std::string toContextString(
        const UnifyContext* pUCStackTop,
        const UnifyContext* pUCTerm ) const;

    const AbstractTerm* getValue( const std::string& key ) const;

    /**
     * Enumerate this map's key/value pairs, for cloneTermTree()
     * (vault-unify-terms.cpp), which cannot otherwise reach m_mapContents
     * to reconstruct an equivalent MapTerm. Keys are returned as owned by
     * THIS MapTerm still (see ~MapTerm()); a caller building a new MapTerm
     * from them must copy each one (`new Atom(...)`) first, since a
     * MapTerm's constructor takes ownership of whatever Atom* it is given.
     */
    void getEntries( std::vector<std::pair<const Atom*, AbstractTerm*> >& out_entries ) const {
        MapTermMap::const_iterator it, itEnd = m_mapContents.end();
        for( it = m_mapContents.begin(); it != itEnd; ++it ) {
            out_entries.push_back( std::make_pair( it->second.first, it->second.second ) );
        }
    }

protected:
private:
    typedef std::string MapTermKey;
    typedef std::pair<const Atom*,AbstractTerm*> MapTermValue;
    typedef std::map<MapTermKey, MapTermValue> MapTermMap;

    class TermIterator 
            : public AbstractTermIterator 
    {
    public:
        TermIterator() : m_mapTerm( NULL ) {}
        TermIterator( const MapTerm* mapTerm ) : m_mapTerm( mapTerm ) {
            m_it = m_mapTerm->m_mapContents.begin();
            m_itEnd = m_mapTerm->m_mapContents.end();
        }
        TermIterator( const TermIterator& other ) 
                : m_mapTerm( other.m_mapTerm )
                , m_it( other.m_it )
                , m_itEnd( other.m_itEnd )
                {}
        
        virtual bool isValid() const {
            return m_mapTerm && m_it != m_itEnd; 
        }
        
        virtual const TermTraversable* getTermTraversable() const {
            return dynamic_cast<TermTraversable*>( m_it->second.second );
        }
        
        virtual void next() {
            ++m_it;
        }

    private:
        const MapTerm* m_mapTerm;
        MapTermMap::const_iterator m_it, m_itEnd;
    };
        
    MapTermMap m_mapContents;

public:

    AbstractTermIterator* abstractTermIterator() const {
        return new TermIterator( this );
    }

};

class VarTerm : public AbstractTerm
{
public:
    VarTerm();
    virtual ~VarTerm() {}

    VarTerm& setOriginalVarName( const std::string& originalVarName ) {
        m_originalVarName = originalVarName;
        return *this;
    }

    std::string getOriginalVarName() const {
        return m_originalVarName;
    }

    virtual UnifyResult unifyTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const AbstractTerm* pOther ) const;
    virtual UnifyResult unifyConsTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const ConsTerm* pOther ) const;
    virtual UnifyResult unifyVarTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const VarTerm* pOther ) const;
    virtual UnifyResult unifyMapTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const MapTerm* pOther ) const;
    virtual UnifyResult unifyArrayTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const ArrayTerm* pOther ) const;

    // void setBinding( VarTermId uidTerm ) { m_uidTerm = uidTerm; }
    VarTermId getBinding() const { return m_uidTerm; }

    virtual const std::string toString() const {
        char s[50];
        if( m_originalVarName.length() ) {
            const char* pName = NULL;
            pName = m_originalVarName.c_str();
            snprintf( s, 50, "%s (VT%lld)",
                pName, (long long) m_uidTerm );
        } else {
            snprintf( s, 50, "VT%lld", (long long) m_uidTerm );
        }
        return std::string( s );
    }
    
    virtual const std::string toJSON( bool useContent, const UnifyContext* pUCStackTop, const UnifyContext* pUCTerm ) const;
    
    virtual const std::string toContextString(
        const UnifyContext* pUCStackTop,
        const UnifyContext* pUCTerm ) const;

    AbstractTermIterator* abstractTermIterator() const {
        return NULL;
    }

private:
    static VarTermId m_counterUidTerm;
    VarTermId m_uidTerm;

    std::string m_originalVarName;
};


class ConsTerm : public AbstractTerm
{
public:
    ConsTerm( const Atom& sym, int nTerms, AbstractTerm** ppTerms )
            : m_name( sym )
            , m_vecTerms( ppTerms, ppTerms+nTerms )
            , m_isNegated( false ) {}

    virtual ~ConsTerm() {}

    ConsTerm& setNegated( bool f ) { m_isNegated = f; return *this; }
    bool isNegated() const { return m_isNegated; }
    
#if VAULT_UNIFY_USE_VIRTUAL_DEREF
    virtual const AbstractTerm* deref() const;
#endif

    ConsTerm( const char* fun, const char* a1, const char* a2, const char* a3 )
        : m_name( fun ) 
        , m_isNegated( false )
    {
        m_vecTerms.push_back( new ConsTerm( a1 ) );
        m_vecTerms.push_back( new ConsTerm( a2 ) );
        m_vecTerms.push_back( new ConsTerm( a3 ) );
    }

    ConsTerm( const char* fun, const char* a1, const char* a2 )
        : m_name( fun ) 
        , m_isNegated( false )
    {
        m_vecTerms.push_back( new ConsTerm( a1 ) );
        m_vecTerms.push_back( new ConsTerm( a2 ) );
    }

    ConsTerm( const char* fun, const char* a1 )
        : m_name( fun ) 
        , m_isNegated( false )
    {
        m_vecTerms.push_back( new ConsTerm( a1 ) );
    }

    ConsTerm( const char* fun )
        : m_name( fun ) 
        , m_isNegated( false )
    {
    }

    ConsTerm( const char* fun, AbstractTerm* t1, AbstractTerm* t2, AbstractTerm* t3 )
        : m_name( fun ) 
        , m_isNegated( false )
    {
        m_vecTerms.push_back( t1 );
        m_vecTerms.push_back( t2 );
        m_vecTerms.push_back( t3 );
    }
    
    ConsTerm( const char* fun, AbstractTerm* t1, AbstractTerm* t2 )
        : m_name( fun ) 
        , m_isNegated( false )
    {
        m_vecTerms.push_back( t1 );
        m_vecTerms.push_back( t2 );
    }
    
    ConsTerm( const char* fun, AbstractTerm* t1 )
        : m_name( fun ) 
        , m_isNegated( false )
    {
        m_vecTerms.push_back( t1 );
    }
    
    virtual UnifyResult unifyTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const AbstractTerm* pOther ) const;
    virtual UnifyResult unifyConsTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const ConsTerm* pOther ) const;
    virtual UnifyResult unifyVarTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const VarTerm* pOther ) const;
    virtual UnifyResult unifyMapTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const MapTerm* pOther ) const;
    virtual UnifyResult unifyArrayTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const ArrayTerm* pOther ) const;

    virtual const std::string toJSON( bool useContent, const UnifyContext* pUCStackTop, const UnifyContext* pUCTerm ) const {
        int l = m_vecTerms.size();
        std::string strCT;

        if( l ) strCT += "[ ";

        strCT += "\"";
        strCT += m_name.value();
        strCT += "\"";
        
        for( int i=0; i<l; ++i ) {
            strCT += ", ";
            AbstractTerm* term = m_vecTerms[i];
            if( term ) {
                strCT += term->toJSON( useContent, pUCStackTop, pUCTerm );
            } else {
                strCT += "null";
            }
        }
        if( l ) strCT += " ]";
        return strCT;
    }

    virtual const std::string toString() const {
        int l = m_vecTerms.size();
        std::string strCT = m_name.value();
        
        if( l ) {
            strCT += "( ";
            for( int i=0; i<l; ++i ) {
                if( i ) {
                    strCT += ", ";
                }
                AbstractTerm* term = m_vecTerms[i];
                if( term ) {
                    strCT += term->toString();
                } else {
                    strCT += "NULL";
                }
            }
            strCT += " )";
        } else {
            // No brackets.
        }
        return strCT;
    }
    
    const Atom& getName() const { return m_name; }
    int getArity() const { return m_vecTerms.size(); }
    const AbstractTerm* getTermAt( int i ) const { return m_vecTerms[i]; }

    virtual const std::string toContextString(
        const UnifyContext* pUCStackTop,
        const UnifyContext* pUCTerm ) const;


private:
    class TermIterator 
            : public AbstractTermIterator 
    {
    public:
        TermIterator() : m_consTerm( NULL ) {}
        TermIterator( const ConsTerm* consTerm ) : m_consTerm( consTerm ) {
            m_it = m_consTerm->m_vecTerms.begin();
            m_itEnd = m_consTerm->m_vecTerms.end();
        }
        TermIterator( const TermIterator& other ) 
                : m_consTerm( other.m_consTerm )
                , m_it( other.m_it )
                , m_itEnd( other.m_itEnd )
                {}
        
        virtual bool isValid() const {
            return m_consTerm && m_it != m_itEnd; 
        }
        
        virtual const TermTraversable* getTermTraversable() const {
            return dynamic_cast<TermTraversable*>( *m_it );
        }
        
        virtual void next() {
            ++m_it;
        }

    private:
        const ConsTerm* m_consTerm;
        std::vector<AbstractTerm*>::const_iterator m_it, m_itEnd;
    };
        
    Atom m_name;
    std::vector<AbstractTerm*> m_vecTerms;
    bool m_isNegated;

public:
    AbstractTermIterator* abstractTermIterator() const {
        return new TermIterator( this );
    }

};


/**
 * ROADMAP Phase 2 ("consistent list/array semantics", SPEC.md section 11):
 * a first-class, ordered term kind for array literals (`[a, b]`). Prior to
 * this, `AnyTermFactory::operator()(const ArrayTermInput&)`
 * (vault-unify-parser.cpp) desugared an array literal straight into a
 * `MapTerm` keyed by stringified numeric indices -- a documented SPEC.md
 * quirk, now removed. `ArrayTerm` mirrors `ConsTerm`'s style (a plain
 * `std::vector<AbstractTerm*>`, ordinary "same length + pairwise unify"
 * unification, see unifyArrayTerm() implementations) rather than
 * `MapTerm`'s (no keys, so no per-entry `Atom*` to own/free -- see
 * `~ArrayTerm()`).
 */
class ArrayTerm : public AbstractTerm
{
public:
    /**
     * Construct from a transient array of nTerms elements. Mirrors
     * ConsTerm's constructor: the elements' pointer VALUES are copied out
     * into this ArrayTerm's own std::vector; ppTerms itself is never
     * retained (the caller must free it -- see e.g.
     * AnyTermFactory::operator()(const ArrayTermInput&),
     * vault-unify-parser.cpp).
     */
    ArrayTerm( AbstractTerm** ppTerms, int nTerms )
            : m_vecElements( ppTerms, ppTerms + nTerms ) {}

    /**
     * Empty array, elements appended one at a time via append() -- used by
     * findall's solver special form (SolveJob::performSlice(),
     * vault-unify-solvejob.cpp) to build its result array solution by
     * solution.
     */
    ArrayTerm() {}

    virtual ~ArrayTerm() {}

    ArrayTerm& append( AbstractTerm* pTerm ) {
        m_vecElements.push_back( pTerm );
        return *this;
    }

    virtual UnifyResult unifyTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const AbstractTerm* pOther ) const;
    virtual UnifyResult unifyConsTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const ConsTerm* pOther ) const;
    virtual UnifyResult unifyVarTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const VarTerm* pOther ) const;
    virtual UnifyResult unifyMapTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const MapTerm* pOther ) const;
    virtual UnifyResult unifyArrayTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const ArrayTerm* pOther ) const;

    virtual const std::string toString() const;

    virtual const std::string toJSON( bool useContent, const UnifyContext* pUCStackTop, const UnifyContext* pUCTerm ) const;

    virtual const std::string toContextString(
        const UnifyContext* pUCStackTop,
        const UnifyContext* pUCTerm ) const;

    int size() const { return (int) m_vecElements.size(); }
    const AbstractTerm* getElementAt( int i ) const { return m_vecElements[i]; }

    /**
     * Enumerate this array's elements, for cloneTermTree()/
     * resolveTermGrounded() (vault-unify-terms.cpp), which cannot
     * otherwise reach m_vecElements.
     */
    void getElements( std::vector<AbstractTerm*>& out_elements ) const {
        out_elements = m_vecElements;
    }

private:
    class TermIterator
            : public AbstractTermIterator
    {
    public:
        TermIterator() : m_arrayTerm( NULL ) {}
        TermIterator( const ArrayTerm* arrayTerm ) : m_arrayTerm( arrayTerm ) {
            m_it = m_arrayTerm->m_vecElements.begin();
            m_itEnd = m_arrayTerm->m_vecElements.end();
        }
        TermIterator( const TermIterator& other )
                : m_arrayTerm( other.m_arrayTerm )
                , m_it( other.m_it )
                , m_itEnd( other.m_itEnd )
                {}

        virtual bool isValid() const {
            return m_arrayTerm && m_it != m_itEnd;
        }

        virtual const TermTraversable* getTermTraversable() const {
            return dynamic_cast<TermTraversable*>( *m_it );
        }

        virtual void next() {
            ++m_it;
        }

    private:
        const ArrayTerm* m_arrayTerm;
        std::vector<AbstractTerm*>::const_iterator m_it, m_itEnd;
    };

    std::vector<AbstractTerm*> m_vecElements;

public:
    AbstractTermIterator* abstractTermIterator() const {
        return new TermIterator( this );
    }

};



#if 0
/**
 * A dynamic term is a term that always has a value. However, the system
 * knows it possibly might change. As such, a dependency on that term is
 * generated.
 * Any unification basing also on that term will monitor that dynamic term.
 *
 * What is the relationship between a dynamic term and an asynchronous clause?
 */
#endif


#if 0 
/**
 * An asynchronous clause is a clause which is not atomically unifiable.
 * A native asynchronous clause is an asynchronous clause which is implemented
 * by means of native code.
 */
 
/**
 * A standr clause is the way to imply the value of several goals from a ggiven set of terms.
 */
#endif


/**
 * Whenever a clause is unable to atomically finish unification, it returns a 
 * clause continuation context object. It will be asked to continue
 * unification at a later time.
 */
class ClauseContinuationContext {
public:
    ClauseContinuationContext( const Clause* pClause ) : m_pClause( pClause ) {}
    virtual ~ClauseContinuationContext();

protected:
    const Clause* m_pClause;
};

class Clause 
{
public:
    enum UnificationState {
        UnificationError = -1,
        UnificationOK = 0
    };

    Clause( ConsTerm* pLeftHandTerm );
    virtual ~Clause();

    void setDebugLocation( const std::string& uriFile, uint64_t line ) {
        setDebugLocation( DebugLocation( uriFile, line ) );
    }
    void setDebugLocation( const DebugLocation& debugLocation ) {
        m_debugLocation = debugLocation;
    }
    DebugLocation getDebugLocation() const
        { return m_debugLocation; }

    ClauseId getUID() const { return m_uid; }

    /**
     * Provenance -- engine item E1. Stamped by
     * ExecutionState::appendClause(); meaningful only for a clause that has
     * actually been appended (an unappended clause reports the default,
     * ClauseOrigin::MODULE with no file, which is why the append is the
     * thing that sets it rather than the constructor).
     */
    const ClauseOrigin& getOrigin() const { return m_origin; }
    void setOrigin( const ClauseOrigin& origin ) { m_origin = origin; }

    const ConsTerm* leftHandTerm() const { return m_pLeftHandTerm; }

    virtual UnificationState startUnification(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* pUCCand,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& inout_pCCC ) const = 0;
    
    virtual std::string toString() const = 0;

    virtual bool isTerminal() const = 0;

    /**
     * ROADMAP Phase 2 (runtime assert/retract, SPEC.md): `retract(...)`
     * (SolveJob::performSlice()'s `__builtin_retract` special form,
     * vault-unify-solvejob.cpp) never erases a clause out of its
     * ExecutionState's m_listClauses -- doing so could invalidate another
     * ClauseIterator (or a UnifyContext::m_itClause / SolveContext::
     * m_itNextChildClause copy of one) currently positioned on exactly that
     * std::list NODE elsewhere in the same search (or a concurrently
     * running nested job), since std::list::erase() only guarantees OTHER
     * iterators stay valid, not ones pointing at the erased element itself.
     * Instead the clause is tombstoned in place -- retire() flips this flag
     * (and stamps a retirement generation, see below), and
     * ClauseIterator::isValid() skips any clause not VISIBLE to that
     * iterator's own generation snapshot while walking m_listClauses -- so
     * the node (and every list iterator referencing it) remains perfectly
     * valid. "Permanently excluded from future candidate consideration" is
     * true for any iterator constructed AFTER this retire() -- but, per
     * SPEC.md's logical update view, NOT for one already in flight when it
     * happens (see the generation comparison on isRetired()/retire()
     * below): that already-in-progress iteration keeps seeing the clause
     * for the rest of its own lifetime, exactly as if the retract had not
     * happened yet from its point of view.
     *
     * Because the tombstoned Clause stays a normal member of its
     * ExecutionState's m_listClauses, it needs no separate "retired list"
     * for its OWN memory: ExecutionState::collectAllTermTrees() and
     * ~ExecutionState() already walk every clause in m_listClauses
     * (retired or not) exactly once, so its head/body term trees and the
     * Clause object itself are freed by the ordinary ~World() sweep,
     * automatically, with no extra bookkeeping (contrast
     * World::m_lsRetiredDebugInfos, vault-unify-world.cpp, which needs its
     * own retired list precisely because a REPLACED TermDebugInfo* is
     * removed from the map that would otherwise reach it again).
     */
    bool isRetired() const { return m_isRetired; }

    /**
     * ROADMAP Phase 2 (runtime assert/retract, SPEC.md -- "logical update
     * view"): tombstone this clause AND stamp the World mutation generation
     * (see World::bumpGeneration()) it was retired in, so a ClauseIterator
     * whose snapshotGen predates `gen` still offers this clause (it hasn't
     * "seen" the retract yet) while one whose snapshotGen is >= `gen`
     * (constructed at or after this retire()) does not. The caller
     * (SolveJob::performSlice()'s `__builtin_retract` special form) must
     * hold World::clauseDbMutex() while calling this, exactly like
     * ExecutionState::appendClause() -- both mutate state guarded by that
     * one lock.
     */
    void retire( uint64_t gen ) { m_isRetired = true; m_retireGeneration = gen; }

    /// See ExecutionState::appendClause() -- stamped with the World
    /// mutation generation (World::bumpGeneration()) this clause was
    /// appended in. A ClauseIterator whose snapshotGen is < this value
    /// never offers this clause -- it did not exist yet, from that
    /// iteration's point of view (see ClauseIterator::isValid()).
    uint64_t getAppendGeneration() const { return m_appendGeneration; }
    void setAppendGeneration( uint64_t gen ) { m_appendGeneration = gen; }

    /// See retire() above. 0 (never stamped) for a clause that has never
    /// been retired; isRetired() is the authoritative "is this clause
    /// retired at all" check -- this value only matters once isRetired()
    /// is true.
    uint64_t getRetireGeneration() const { return m_retireGeneration; }

private:
    ConsTerm* m_pLeftHandTerm;

    static ClauseId m_nextUid;

    /// Uid of the clause.
    ClauseId m_uid;

    DebugLocation m_debugLocation;

    /// See isRetired()/retire() above.
    bool m_isRetired;

    /// See getAppendGeneration()/setAppendGeneration() above.
    uint64_t m_appendGeneration;

    /// See getRetireGeneration()/retire() above.
    uint64_t m_retireGeneration;

    /// See getOrigin()/setOrigin() above (engine item E1).
    ClauseOrigin m_origin;
};


class SimpleBuiltinClause
        : public Clause
{
public:
    SimpleBuiltinClause( ConsTerm* pLeftHandTerm );
    
    virtual std::string toString() const;

    virtual bool isTerminal() const;

    /**
     * Return the nth argument, if it is a consterm or a varterm bound to a consterm.
     */
    static const vault::unify::ConsTerm* getConsTermArg( 
        UnifyContext* pUCStackTop, UnifyContext* pUCOriginal,
        const ConsTerm* pGoalTerm,
        int idx
        );
};


typedef uint64_t InstanceId;


/** 
 * A var instance carries an instantiated, bound variable.
 */ 
struct SingleVarInstance
{
public:
    SingleVarInstance( UnifyContext*, const AbstractTerm* );
    void setValue( UnifyContext*, const AbstractTerm* );

    const AbstractTerm* getTerm() const {
        return m_pTerm;
    }
    UnifyContext* getUnifyContext() const {
        return m_pUCTerm;
    }

private:
    UnifyContext* m_pUCTerm;    
    const AbstractTerm* m_pTerm;
};

#if 0
// Actor (state) objects
class Actor {
public:
    Actor() {}
    virtual ~Actor() {}
    
    virtual int performSlice() = 0;
private:
};
#endif


struct GoalPart {
public:
    const std::string toString() const;
    
    GoalPart(
            const Goal* pGoal, 
            GoalPart* pParentGoalPart,
            UnifyContext* pOriginUnifyContext,
            const vault::unify::Goal::GoalIterator& nextGoalTerm );

    UnifyContext* getOriginUnifyContext() const { return m_pOriginUnifyContext; }

    /**
     * This goal is "new" for this goal part.
     */
    const Goal* m_pNewGoal;

    /**
     * The parent goal part. The iterator refers to the parent part.
     */
    GoalPart* m_pParentGoalPart;

    /**
     * The term "after" newGoal.
     */
    vault::unify::Goal::GoalIterator m_itNextTermInParent;

    /**
     * The unify context that created this goal part.
     */
    UnifyContext* m_pOriginUnifyContext;
};

class GoalPartCursor {
private:
    void normalize() {
        // On creation, we have to normalize.
        while( !m_itCurrentTerm.isValid() ) {
            // pop out.
            m_itCurrentTerm =  m_pCurrentGoalPart->m_itNextTermInParent;
            m_pCurrentGoalPart = m_pCurrentGoalPart->m_pParentGoalPart;
            if( !m_pCurrentGoalPart ) {
                break;
            }
        }
    }

public:
    GoalPartCursor( GoalPart* goalPart ) 
            : m_pCurrentGoalPart( goalPart )
            , m_itCurrentTerm( goalPart?goalPart->m_pNewGoal->termIterator():NULL )
    {
        normalize();
    }

    GoalPartCursor()
            : m_pCurrentGoalPart( NULL )
    {
    }
    
    void next() {
        m_itCurrentTerm.next();
        normalize();
    }

    bool isValid() {
        if( m_pCurrentGoalPart ) return true;
        else return false;
    }
    
    const AbstractTerm* getAbstractTerm() const {
        return m_itCurrentTerm.getAbstractTerm();
    }
    
    UnifyContext* getOriginUnifyContext() const {
        return m_pCurrentGoalPart->getOriginUnifyContext();
    }
    
    const vault::unify::Goal::GoalIterator getGoalIterator() const {
        return m_itCurrentTerm;
    }
    
    GoalPart* getGoalPart() const {
        return m_pCurrentGoalPart;
    }
    
private:
    GoalPart* m_pCurrentGoalPart;
    vault::unify::Goal::GoalIterator m_itCurrentTerm;
};


/**
 * Every unification context is executed in a context of its own.
 */
class UnifyContext {
public:
    /**
     * Create a new unify context to unify the term given.
     */
    UnifyContext(
            UnifyContext* parentUnifyContext,
            const GoalPartCursor& itTerm,
            const ExecutionState::ClauseIterator& itClause );

    /**
     * ROADMAP Phase 2 (Arithmetic and comparison builtins; extended for
     * "findall"): frees terms adopted via adoptTerm() below. See
     * adoptTerm()'s comment for why this is a NEW ownership need distinct
     * from every other term this module allocates: everywhere else, a term
     * built at parse time is reachable from a Goal/clause head that an
     * existing sweep already frees (World::~World() / ~SolveJob(), both
     * via collectTermTree()), and a term built at solve time by e.g.
     * UnifyBuiltinClause/MemberBuiltinClause is never a NEW allocation --
     * it is always one of the existing goal/clause terms it was handed.
     * ArithEvalBuiltinClause (vault-unify-clause-builtin-arith.cpp) was the
     * first builtin to allocate a genuinely new term (the evaluated
     * numeric result) at solve time: nothing else points at it
     * structurally (it is reachable only via whatever VarTerm binding it
     * gets unified into, and VarTerm::abstractTermIterator() returns NULL
     * -- collectTermTree() never follows a variable's runtime binding, by
     * design, only the static term tree), so it would otherwise leak.
     * SolveJob::performSlice()'s __builtin_findall handling
     * (vault-unify-solvejob.cpp) now adopts a whole freshly-cloned
     * ArrayTerm subtree the same way, so this destructor collects every
     * adopted term's entire tree (via collectTermTree(), into one
     * de-duplicated set -- exactly World::~World()'s/~SolveJob()'s own
     * pattern) rather than assuming a single leaf node; see
     * vault-unify-unifycontext.cpp.
     */
    ~UnifyContext();

    /**
     * Adopt pTerm (built fresh at solve time -- e.g. an evaluated
     * arithmetic result, or a findall result ArrayTerm and its whole
     * resolveTermGrounded()-cloned element subtree) into THIS
     * UnifyContext: its entire tree is deleted in ~UnifyContext() (via
     * collectTermTree(), see that destructor's comment). This UnifyContext
     * is itself always one of the per-job arena entries in
     * SolveJob::m_arenaUnifyContexts (every UnifyContext instantiated
     * while solving a job is adopted there via
     * SolveJob::adoptUnifyContext() -- see that class), so pTerm's
     * lifetime becomes tied to the job's own arena cleanup in
     * ~SolveJob(), exactly like everything else that job owns.
     *
     * Only ever used for a term (or term tree) that is freshly allocated
     * for this adoption alone -- never for a term shared with, or
     * reachable from, any clause/query term tree, so there is no risk of
     * this colliding with collectTermTree()'s de-duplicated sweeps
     * (World::~World(), ~SolveJob()'s Goal-arena cleanup): those never
     * reach an adopted term in the first place (see this class's
     * destructor comment), and this method is never called twice for the
     * same pTerm.
     *
     * Returns its argument unchanged so it can be used inline at the `new`
     * call site, matching the existing adoptUnifyContext()/adoptGoalPart()
     * idiom (SolveJob, vault-unify-solvejob.hpp).
     */
    AbstractTerm* adoptTerm( AbstractTerm* pTerm ) {
        m_lsAdoptedTerms.push_back( pTerm );
        return pTerm;
    }

    std::string toString() const;
    
    UnifyContextId getUnifyContextId() const { return m_uidUnifyContext; }

    UnifyContext* getParentUnifyContext() const  { return m_pParentUnifyContext; }

    UnifyResult unifyTerms(
        Engine* pEngine,
        const AbstractTerm* pMyTerm,
        UnifyContext* pUCOther,
        UnifyContext* pUCMine,
        const AbstractTerm* pOther 
        );

    bool isNegated() const {
        return m_isNegated;
    }

    UnifyContext& setNegated( bool f ) { m_isNegated = f; return *this; }
    UnifyContext& setFoundClause( bool f ) { m_foundClause = f; return *this; }

    /**
     * Can be called by an ongoing unification if it is done.
     * During async unification processes, this triggers execution of
     * the unify context.
     */
    void unificationDone(
            UnifyResult unificationResult,
            const Goal* pGoal );
    
    bool isUnificationDone() const {
        return m_isUnificationDone;
    }

    UnifyResult getUnificationResult() const;
    
    int findVar(
            AssignmentId aid,
            boost::shared_ptr<SingleVarInstance>& out_spInstance ) const;

    static boost::shared_ptr<SingleVarInstance> createVarInstance(
            UnifyContext* pUCTerm,
            const AbstractTerm* pTerm );
    
    int createVar(
            AssignmentId aid,
            boost::shared_ptr<SingleVarInstance> spInstance,
            UnifyContext* pTermUnifyContext,
            const AbstractTerm* pTerm );
    
    int findVarBinding(
            AssignmentId aid,
            InstanceId& out_iid ) const;
        
    int bindVarBinding(
            AssignmentId aid,
            InstanceId& out_iid,
            InstanceId iid=0 );
    
    int bindVarBindingUsing(
            AssignmentId aid,
            InstanceId iid ) {
        InstanceId iidDummy = 0;
        return bindVarBinding( aid, iidDummy, iid );
    }
    
    int findVarInstance(
            InstanceId iid,
            boost::shared_ptr<SingleVarInstance>& out_spInstance ) const;
    
    int bindVarInstance(
            InstanceId iid,
            boost::shared_ptr<SingleVarInstance> spInstance );
    
    static UnifyResult genericUnifyVarWithKnown(
            Engine* pEngine,
            UnifyContext* pUCStackTop,
            UnifyContext* pUCVarScope,
            const VarTerm* pVarTerm,
            UnifyContext* pUCOther,
            const AbstractTerm* pOtherTerm );

    // int findCreateVarBinding(
    //     VarTermId uid, const AbstractTerm* pTerm, const AbstractTerm*& out_pTerm );

    static InstanceId createIid() {
        return ++m_iidLast;
    }
    static InstanceId m_iidLast;

    /**
     * The parent unification context.
     */
    UnifyContext* m_pParentUnifyContext;
        
    /**
     * The actual term to unify.
     */
    GoalPartCursor m_csTermToUnify;

    /**
     * The variable bindings emit during unifying the clause.
     */
    std::map<AssignmentId,InstanceId> m_mapVarMappings;

    /**
     * The map of variable instances.
     */ 
    std::map<InstanceId,boost::shared_ptr<SingleVarInstance> > m_mapVarInstances;

    /** 
     * The goal that we added.
     * (As stored in itCurrentClause.?)
     */
    const Goal* m_pGoal;
    
    /**
     * Reference to the winning clause.
     */
    ExecutionState::ClauseIterator m_itClause;

    bool m_isUnificationDone;
    UnifyResult m_unificationResult;
    
    /** 
     * A unique id for this unify context.
     */
    UnifyContextId m_uidUnifyContext;

    /**
     * Wether the result for this unification context shall be inverted or not.
     */
    bool m_isNegated;

    /**
     * Wether we found a clause to match by signature.
     */
    bool m_foundClause;

    /**
     * Terms adopted via adoptTerm() (see its comment): freed one at a
     * time (plain `delete`, not collectTermTree()) in ~UnifyContext().
     */
    std::vector<AbstractTerm*> m_lsAdoptedTerms;
};


typedef uint64_t JobId; 
class Job;


/**
 * A unify world contains the current state of the system.
 * This contains both constant parts and current unification states.
 *
 * Operations on the world can be readonly or modifying.
 * If an operation is readonly, if references all static contexts, thereby
 * marking them copy-on-write.
 * All reading operations, that share a given context, may after all reference
 * the same copy-on-write context, that turns into a standard (copied) context
 * later on.
 * 
 * Any modifying operation requests a writable context on modification for
 * its transaction context.
 * If it encounters a "unused" record, it can put a modification record into
 * its transaction (note the use of nil clauses to erase any).
 * If it encounters a copy-on-write context, it creates a new context
 * containing the differences to the later readonly context.
 * 
 * Please note that modifying contexts usually happens only if blocks of
 * clauses are removed / added (possibly because a device is added/removed)
 * or if base predicates change ( e.g. network_available( false ) replaces
 * network_available( true ) ). 
 */
class World 
    : public boost::enable_shared_from_this<World>
{
public:
    World();
    /**
     * ROADMAP Phase 1 (Ownership model), pass 2: frees the clause database
     * (every clause's head/body term trees, as one de-duplicated pass --
     * see collectTermTree()/ExecutionState::collectAllTermTrees()), then
     * the TermDebugInfo/FileDebugInfo entries, before the root
     * ExecutionState (and everything it owns) is destroyed automatically
     * right after this destructor's body returns. See vault-unify-world.cpp.
     */
    ~World();

    void init();

    ExecutionState* getRootState() {
        return &m_rootState;
    }
    
    int asyncShutdown( WorldPtr spWorld ); 
    
#if 0
    int appendRootClause( const Clause* , uint64_t& uid );
#endif

    void setTermDebugInfo( const AbstractTerm*, TermDebugInfo* );
    TermDebugInfo* getTermDebugInfo( const AbstractTerm* );

    /**
     * ROADMAP Phase 2 ("File imports / include", SPEC.md section 15):
     * registers exclusive ownership of a `FileDebugInfo` created for a
     * "current file" that `RuntimeContext::parseExecuteSegment()` is about
     * to parse (the main program, via unify-run.cpp, or an imported file,
     * via RuntimeContext itself) -- see vault-unify-world.cpp's `~World()`
     * for why this registry exists alongside the pre-existing TermDebugInfo-
     * reachable discovery: a segment that never successfully builds a
     * single term (e.g. a file that only contains further `import`
     * statements, or comments) would otherwise never be reached by that
     * discovery sweep at all, and its FileDebugInfo would leak. Every
     * `FileDebugInfo` a caller creates for this purpose MUST be registered
     * here exactly once, right after construction; `~World()` deletes every
     * registered pointer exactly once, de-duplicated against the same
     * pointer if ALSO discovered via a TermDebugInfo (they are the same
     * object either way -- registration does not change who a TermDebugInfo
     * points at, only who is responsible for the final `delete`). Callers
     * (RuntimeContext, unify-run.cpp) must NOT delete these themselves --
     * see the ownership note on RuntimeContext::parseExecuteSegment()'s
     * import handling and on unify-run.cpp's main() for why (destruction-
     * order hazard: this World generally outlives the caller that created
     * the FileDebugInfo, but even were that not so, a caller-side delete
     * would race/duplicate this sweep).
     */
    void adoptFileDebugInfo( FileDebugInfo* pFileDebugInfo ) {
        m_lsOwnedFileDebugInfos.push_back( pFileDebugInfo );
    }

    /**
     * Record a clause in the definition catalogue -- engine item E2.
     *
     * LOCKING: the caller must hold clauseDbMutex(). Public only because
     * ExecutionState::appendClause() is the single call site and is not a
     * member of World; it is not part of the World's usable surface.
     */
    void catalogueAppend( const Clause* pClause );

    /**
     * Account for a clause being tombstoned -- engine item E2. Same locking
     * rule as catalogueAppend(); called from `retract`'s special form,
     * which already holds the lock to call Clause::retire().
     */
    void catalogueRetire( const Clause* pClause );

    /**
     * Snapshot the catalogue -- engine items E2 and E14.
     *
     * Takes clauseDbMutex() and copies, rather than handing out a reference
     * into the live map. That is the whole point: World::clauseDbMutex()'s
     * comment justifies an unlocked reader side by "this engine has exactly
     * one reader today, the single worker thread", and a session that
     * answers `listing` while a query runs makes that false. Copying a few
     * hundred rows is cheap next to the alternative, which is a std::map
     * being read during a rehash.
     */
    void copyCatalogue( std::vector<CatalogueEntry>& out_lsEntries );

    /**
     * Snapshot one predicate's catalogue entry. Returns false if there is
     * no such predicate. Same locking as copyCatalogue().
     */
    bool findCatalogueEntry( const PredicateKey& key,
                             CatalogueEntry& out_entry );

    /**
     * The module id for a source file, assigning one on first sight
     * (engine item E1).
     *
     * Ids are 1-based and dense, in first-seen order, and stable for the
     * life of this World; NO_MODULE (0) is returned for an empty path,
     * which is what a builtin or a runtime-asserted clause has.
     *
     * LOCKING: this is called from ExecutionState::appendClause(), which
     * holds clauseDbMutex(). It is not safe to call from anywhere that does
     * not hold that lock, and there is deliberately no internal locking --
     * a second, finer lock here would have to be ordered against the clause
     * -db one and would buy nothing, since every writer is already inside
     * that critical section. See clauseDbMutex()'s comment.
     */
    ModuleId moduleIdForFile( const std::string& uriFile );

    /**
     * The file a module id names, or an empty string for NO_MODULE or an
     * id this World never issued.
     */
    const std::string& moduleFile( ModuleId id ) const;

    /// How many modules have been seen. Ids run 1..moduleCount().
    size_t moduleCount() const { return m_lsModuleFiles.size(); }

    /**
     * ROADMAP Phase 2 (runtime assert/retract, SPEC.md -- "logical update
     * view"): the current clause-database mutation generation, for a
     * ClauseIterator to snapshot at construction (see
     * ExecutionState::ClauseIterator's constructor). Deliberately UNLOCKED
     * -- see clauseDbMutex()'s comment below for why that is fine today.
     */
    uint64_t currentGeneration() const { return m_mutationGeneration; }

    /**
     * Advance and return the new clause-database mutation generation.
     * Called exactly once per mutation -- once per ExecutionState::
     * appendClause() (a parse-time clause or a runtime assert()) and once
     * per retract()'s Clause::retire() -- each such call site stamps the
     * mutating/retiring Clause with the returned value
     * (Clause::setAppendGeneration()/retire()). The caller MUST hold
     * clauseDbMutex() while calling this (see below); it is not
     * synchronized on its own.
     */
    uint64_t bumpGeneration() { return ++m_mutationGeneration; }

    /**
     * ROADMAP Phase 5.1 (pulled forward): guards m_mutationGeneration
     * together with whichever ExecutionState::m_listClauses is being
     * mutated. appendClause() (called from BOTH the parser/main thread --
     * for parse-time clauses, see RuntimeContext::parseExecuteSegment() --
     * AND the worker thread -- for a runtime assert(), SolveJob::
     * performSlice()) and retract()'s Clause::retire() call (worker thread
     * only) both lock this for their list-mutation-plus-generation-bump
     * critical section, replacing the commented-out `// LOCK( this )` /
     * `// UNLOCK( this )` placeholders that used to bracket
     * ExecutionState::appendClause()'s body -- what was once merely a
     * theoretical race (nothing else ever mutated the clause list
     * concurrently) is a real one now that assert() makes the WORKER
     * thread a clause-list writer too, alongside the parser thread.
     *
     * Reader side (a ClauseIterator snapshotting currentGeneration() at
     * construction, or walking m_listClauses/checking isRetired() while
     * traversing) is deliberately left UNLOCKED here -- this engine has
     * exactly one reader today, the single worker thread (SPEC.md's
     * FIFO-single-worker-thread guarantee), so two READS never race each
     * other, and a read racing the parser thread's WRITE is a pre-existing,
     * benign (ASan/UBSan-silent -- ordinary word-sized reads/writes, no
     * heap corruption) condition, not a new one this change introduces.
     * Making the reader side race-free too (e.g. so TSan would also be
     * silent) is ROADMAP 5.1 scope, not this task's.
     */
    boost::mutex& clauseDbMutex() { return m_mutexClauseDb; }

private:
    /// The single root execution state.
    ExecutionState m_rootState;

    /// Constant root clauses, built-ins etc.
    /// These are non-modifyable builtin clauses.
    std::list<const Clause*> m_rootClauses;

    typedef std::map<const AbstractTerm*, TermDebugInfo*> TermDebugMap;
    TermDebugMap m_mapDebugInfos;

    /// Values replaced in m_mapDebugInfos. They may still be shared with
    /// other map entries, so they cannot be deleted at replace time;
    /// ~World()'s de-duplicating sweep reclaims them exactly once.
    std::list<TermDebugInfo*> m_lsRetiredDebugInfos;

    /// See adoptFileDebugInfo() above: FileDebugInfo objects explicitly
    /// registered for ownership (currently: one per file parseExecuteSegment
    /// is ever given -- the main program, and each distinct imported file),
    /// freed exactly once by ~World()'s sweep alongside whatever it also
    /// discovers via TermDebugInfo entries.
    std::list<FileDebugInfo*> m_lsOwnedFileDebugInfos;

    /// See catalogueAppend()/copyCatalogue() above (engine item E2).
    std::map<PredicateKey, CatalogueEntry> m_mapCatalogue;

    /// See moduleIdForFile()/moduleFile() above (engine item E1). The
    /// vector is the id space -- index i holds the file for id i+1 -- and
    /// the map is only an index into it, so an id can never be invalidated
    /// by a later insertion.
    std::vector<std::string> m_lsModuleFiles;
    std::map<std::string, ModuleId> m_mapModuleIds;

    /// See currentGeneration()/bumpGeneration() above.
    /// See currentGeneration()/bumpGeneration() above.
    uint64_t m_mutationGeneration;

    /// See clauseDbMutex() above.
    boost::mutex m_mutexClauseDb;
};


class WorldChangeSink {
public:
    WorldChangeSink( Engine* pEngine ) {
        m_pEngine = pEngine;
    }
    Engine* getEngine() const { return m_pEngine; }
protected:
private:
    Engine* m_pEngine;
};


/**
 * User event listeners receive events as sent by the user's emit
 * function.
 */
class UserEventListener {
public:
    virtual ~UserEventListener();

    virtual void userEvent( Engine*, const std::string& ) = 0;
};


/**
 * The Engine operates on the data as available in the world data structure.
 * It provides the real world resources which not directly are persistable.
 *
 */
/**
 * One user error, as data -- engine item E10.
 *
 * Before this existed, a parse error was three fprintf()s to stderr
 * (reportParseError, vault-unify-runtime-context.cpp) and the caller got
 * back an error COUNT; a runtime UnifyError incremented a counter and
 * overwrote a single "last error" string (SolveJob::recordError). Neither
 * is something a diagnostics panel can navigate to, neither survives a
 * process boundary, and neither can be attributed to the query that caused
 * it. Formatting is a presentation decision, so it moves to the sink and
 * the engine reports facts.
 *
 * The default sink prints exactly the bytes the fprintf()s did -- see
 * DefaultDiagnosticSink in vault-unify-engine.cpp. That format is what
 * unify-run's users and its CI logs read, so E10 is a change of plumbing,
 * not of output.
 */
struct Diagnostic {
    enum Severity {
        NOTE = 0,
        WARNING,
        ERROR
    };

    Diagnostic()
        : severity( ERROR )
        , line( 0 )
        , column( 0 )
    {
    }

    Severity    severity;

    /// The file, or a pseudo-URI such as "<repl>" or "<input>".
    std::string uriFile;

    /// 1-based; 0 when unknown (a runtime error has no column).
    uint64_t    line;
    uint64_t    column;

    /// What went wrong, with no file/line prefix and no trailing newline.
    std::string message;

    /**
     * The offending source line, without its newline. Empty when there is
     * none to show -- a runtime error, or an import that parsed fine and
     * merely could not be opened.
     *
     * Carried here rather than re-read from the file by the consumer,
     * because a transcript line has no file to re-read and a remote core's
     * files are on the wrong machine.
     */
    std::string sourceLine;
};


/**
 * Where diagnostics go -- engine item E10. See OutputSink below for the
 * ownership and threading rules, which are the same.
 */
class DiagnosticSink {
public:
    virtual ~DiagnosticSink();
    virtual void onDiagnostic( const Diagnostic& diagnostic ) = 0;
};


/**
 * Where a program's output goes -- engine item E4.
 *
 * Before this existed, `print` wrote to std::cout, `emit` wrote to
 * std::cout, and world-change logging wrote to std::cerr, each directly
 * from whichever thread happened to be executing. That is fine for a
 * command-line tool and unusable for anything else: a remote engine's
 * `print` sent to the engine process's stdout is simply lost on another
 * machine, and two sessions sharing one process interleave into the same
 * two file descriptors with no way to tell whose output is whose.
 *
 * So output becomes something a caller can be handed instead of something
 * the engine performs. The default sink (Engine's, installed unless
 * replaced) reproduces the previous bytes exactly -- see DefaultOutputSink
 * in vault-unify-engine.cpp -- because the golden corpus pins them and a
 * "harmless" reformatting here would break every one of them.
 *
 * THREADING: called on the engine's worker thread, from inside a builtin.
 * An implementation must not block and must not call back into the engine.
 */
class OutputSink {
public:
    virtual ~OutputSink();

    /**
     * @param strStream
     *     "stdout" or "stderr" -- which of the two the default sink would
     *     have used. A session sink turns this into the `stream` field of
     *     an Output event rather than choosing a file descriptor.
     * @param strText
     *     The complete text to write, newline included. Deliberately not
     *     line-oriented: the caller assembles the whole line so that a sink
     *     never has to reassemble one, and so the default sink is a single
     *     write with no formatting decisions of its own.
     */
    virtual void onOutput( const std::string& strStream,
                           const std::string& strText ) = 0;
};


class Engine
    : public ExecutionController
{
public:
    Engine();
    ~Engine();
    
    void setWorldChangeSink( WorldChangeSink* pWorldChangeSink ) {
        m_pWorldChangeSink = pWorldChangeSink;
    }

    /**
     * Engine item E4: redirect this engine's program output.
     *
     * Passing NULL restores the default sink (stdout/stderr, byte-for-byte
     * as before E4). The caller retains ownership and must outlive the
     * engine, or must clear the sink before dying -- there is no shutdown
     * path to do it for them (ROADMAP 5.1).
     */
    void setOutputSink( OutputSink* pOutputSink );

    /**
     * Engine item E10: redirect this engine's diagnostics. NULL restores
     * the default (gcc-style text on stderr, byte-for-byte as before E10).
     */
    void setDiagnosticSink( DiagnosticSink* pDiagnosticSink );

    /**
     * What the DEFAULT sink should do with a diagnostic when no sink has
     * been installed.
     *
     * This exists because E10 must not change what unify-run prints. The
     * two error paths it unifies did not previously behave the same way: a
     * parse error was three lines on stderr, and a runtime UnifyError
     * (SolveJob::recordError) printed NOTHING AT ALL -- it bumped a counter
     * and overwrote a "last error" string that only unify-run's one-line
     * summary ever read.
     *
     * Routing both to a printing default would have made the engine start
     * reporting runtime errors it had always swallowed. That is arguably an
     * improvement, and it is not this item's to make: "unify-run keeps
     * working, unchanged" is a rule of the plan, and a diagnostics change
     * that quietly adds output to everyone's CI logs is exactly the kind of
     * drive-by the rule is there to prevent.
     *
     * So the call site states what silence meant before, and an INSTALLED
     * sink receives everything either way -- which is the whole point:
     * a front end gets the runtime errors, and unify-run's output does not
     * move.
     */
    enum DiagnosticDefault {
        /// No sink installed: print it, as this path always did.
        DIAGNOSTIC_PRINT = 0,
        /// No sink installed: drop it, as this path always did.
        DIAGNOSTIC_SILENT
    };

    /** Report one diagnostic. The single funnel, for the same reason. */
    void writeDiagnostic( const Diagnostic& diagnostic,
                          DiagnosticDefault whenNoSink = DIAGNOSTIC_PRINT );

    /**
     * Write one complete piece of program output. Used by the `print` and
     * `emit` builtins and by world-change logging; the single funnel is the
     * point, since anything that bypasses it is invisible to a remote
     * front end.
     */
    void writeOutput( const std::string& strStream, const std::string& strText );

    WorldPtr createWorld();

    int shutdownWorld( WorldPtr spWorld );

    JobId addJob( boost::shared_ptr<Job> spJob );

    /**
     * The job and all of its resources have been released.
     * The job object itselv now can be unreferenced, i.e.
     * deleted synchronously.
     *
     * ROADMAP Phase 1: nothing calls this any more (executionLoop() used
     * to park every FINISHED job in m_lsZombieJobs forever instead of
     * letting it go out of scope; it now just lets the shared_ptr die so
     * the job's arena is freed). Left in place, unused, in case a future
     * consumer needs to retrieve a finished job asynchronously.
     */
    void onJobReleased( boost::shared_ptr<Job> spJob );
    // terminateJob();
    // suspendJob();

    void addWorkerThread();

    WorldChangeSink& logChange() {
        return *m_pWorldChangeSink;
    }

    // Internal.
    void emitChange( const std::string& );

    void addUserEventListener( UserEventListener* );
    void removeUserEventListener( UserEventListener* );

    void emitEvent( const std::string& );

    // For interface ExecutionController

    virtual void setDebugListener( DebugListener* );

    virtual int run();
    virtual int stepInto();
    virtual int stepOver();
    virtual int stepOut();
    virtual int stop();
    virtual int detach();

    virtual DebugLocation getDebugLocation();
    virtual int getDebugStack( std::list<StackFrame>& out_lsFrames );
    virtual int getDebugProperties( std::list<DebugProperty>& out_lsProperties, uint64_t frameId );

    // Convenience
    DebugListener* getDebugListener() const { return m_pDebugListener; }

    bool isDebuggerAttached() const;

    bool isDebugHalted() const;

private:
    void executionLoop();

    int requestDebugState( DebugListener::ChangeReason debugTargetState );

    /**
     * We remember the state this engine should change to as requested from the debugger.
     */ 
    DebugListener::ChangeReason m_debugTargetState;

    mutable boost::mutex m_mutex;
    boost::condition_variable m_cond;
    
    /**
     * Holds the number of threads currently waiting for work.
     */
    int m_nThreadsWaiting;

    /// See setOutputSink()/writeOutput() above (engine item E4). Never

    /// NULL: cleared back to the process-output default rather than unset.

    OutputSink* m_pOutputSink;

    /// See setDiagnosticSink()/writeDiagnostic() above (engine item E10).
    /// NULL when no sink has been installed -- unlike m_pOutputSink, the
    /// "nobody is listening" case is not uniform here (see
    /// DiagnosticDefault), so it has to be distinguishable.
    DiagnosticSink* m_pDiagnosticSink;


    WorldChangeSink* m_pWorldChangeSink;

    /**
     * A list of jobs ready for execution.
     */
    std::list<boost::shared_ptr<Job> > m_lsReadyJobs;

    /**
     * A list of jobs waiting for some external action.
     */
    std::list<boost::shared_ptr<Job> > m_lsBlockedJobs;

    /**
     * A list of termianted jobs, bothe regular and by exception.
     * They are waiting for somebody to read the status.
     *
     * ROADMAP Phase 1: no longer populated. executionLoop() used to push
     * every FINISHED job here and never remove it (nothing calls
     * onJobReleased()), which meant a job - and its per-job arena - was
     * never destructed. Left declared, unused, alongside onJobReleased()
     * in case a future asynchronous consumer needs it.
     */
    std::list<boost::shared_ptr<Job> > m_lsZombieJobs;

    // boost::shared_ptr<Job> m_spDebugHaltedJob;

    std::vector<UserEventListener*> m_vecUserEventListener;

    DebugListener* m_pDebugListener;


    /** 
     * This flag is set, if the debugger currently is halted due to
     * 
     */
    bool m_isDebugHalted;

    DebugListener::ChangeReason m_lastDebugTargetState;

    /**
     * Worker threads started by addWorkerThread(). Kept here so the
     * boost::thread objects stay alive for the life of the engine instead
     * of being destroyed (and calling std::terminate() on a still-joinable
     * thread) as soon as addWorkerThread() returns.
     * TXWTODO: join on engine shutdown (ROADMAP Phase 5.1)
     */
    std::list<boost::thread*> m_lsWorkerThreads;
};


class Job 
{
public:
    enum State {
        CREATED,
        READY,
        BLOCKED,
        FINISHED,
        DEBUGHALTED,
        DONE
    };

    Job();

    virtual ~Job() {}
    virtual int performSlice() = 0;
    virtual int startJob( Engine* ) = 0;
    State state() const;

    int setDebugTargetState( DebugListener::ChangeReason debugTargetState );
    DebugListener::ChangeReason getDebugTargetState() const {
        return m_debugTargetState;
    }
    virtual DebugLocation getDebugLocation();

    /**
     * Return a stack of debug locations to enable debugger's backtrace.
     */
    virtual int getDebugStack(
            std::list<StackFrame>& out_lsStack );

    virtual int getDebugProperties( 
            std::list<DebugProperty>& out_lsProperties, 
            uint64_t frameId );

    /**
     * The job is in FINISHED state.
     * Trigger release of all resources associated with this job.
     * If the the release finished, call engine::onJobReleased( )
     */
    virtual int triggerRelease() = 0;

    JobId getId() const { return m_idNextJob; }

    Job& onFinished( boost::function<void (boost::shared_ptr<Job>)> onFinished );

protected:

    /**
     * Allow sub-classes to announce the transition to a particular state.
     */
    Job& state( State state );

private:
    friend class Engine;

    JobId m_id;
    State m_state;
    static JobId m_idNextJob;
    boost::function<void (boost::shared_ptr<Job>)> m_onFinished;
    DebugListener::ChangeReason m_debugTargetState;
};


template <typename T> WorldChangeSink& operator << ( WorldChangeSink& sink, const T& object) {
    Engine* pEngine = sink.getEngine();
    pEngine->emitChange( object.toString() );
    return sink;
}


class FileDebugInfo;

class RuntimeContext {
public:

    /**
     * ROADMAP Phase 1 (Ownership model), pass 2: frees the WorldChangeSink
     * allocated by setupDone(). Does NOT delete m_pEngine: Engine's worker
     * thread has no shutdown path yet (ROADMAP Phase 5.1), so the Engine is
     * intentionally left alive/leaked, exactly as before this pass.
     * Releasing m_spWorld (a shared_ptr, decremented automatically via
     * normal member destruction after this body runs) triggers ~World()
     * once this is the last reference.
     */
    ~RuntimeContext();

    int setupDone();

    int parseExecuteSegment(
        std::string::const_iterator itLine, 
        std::string::const_iterator itLineEnd,
        boost::function<void (boost::shared_ptr<vault::unify::Job>)> onFinished,
        const FileDebugInfo* pFileDebugInfo = NULL,
        /**
         * Engine item E1: the provenance to stamp on every clause this
         * segment defines.
         *
         * Defaulted to MODULE because that is what a file is, and every
         * caller that parses a file wants it. The REPL passes TRANSCRIPT
         * for text typed at the prompt -- a distinction this function
         * cannot make for itself, since the REPL hands it a FileDebugInfo
         * just as a file does (with the pseudo-URI "<repl>").
         */
        ClauseOrigin::Kind kind = ClauseOrigin::MODULE );

    Engine* getEngine() const { return m_pEngine; }
    WorldPtr getWorld() const { return m_spWorld; }

private:
    /**
     * ROADMAP Phase 2 ("File imports / include", SPEC.md section 15):
     * resolves, canonicalizes and (once-per-RuntimeContext) parses one
     * `import "path";` statement encountered by parseExecuteSegment() --
     * see that method's body (vault-unify-runtime-context.cpp) for how the
     * import event is recognized, and its own comment for the resolution/
     * once-semantics/error-reporting rules. `strRawPath` is exactly the
     * quoted string's unescaped contents; `pCurrentFileDebugInfo` is the
     * FileDebugInfo of the file the import statement itself appears in (NULL
     * if unknown, e.g. a REST-fed segment), used both to resolve the import
     * path (relative to ITS directory) and to attribute the "cannot open"
     * diagnostic to the right file/line. `line` is the best-effort source
     * line of the import statement (same precision as the existing "line %d:
     * Added clause" trace uses). Returns the number of NEW parse errors
     * this import (and anything it recursively imports) produced, exactly
     * like parseExecuteSegment() itself; an already-imported file
     * contributes 0 and is otherwise a silent no-op.
     */
    int processImport(
        const std::string& strRawPath,
        int line,
        const FileDebugInfo* pCurrentFileDebugInfo,
        boost::function<void (boost::shared_ptr<vault::unify::Job>)> onFinished );

    vault::unify::WorldPtr m_spWorld;

    vault::unify::Engine* m_pEngine;

    vault::unify::WorldChangeSink* m_pWorldChangeSink;

    vault::unify::ExecutionState* m_esRoot;

    /**
     * ROADMAP Phase 2 ("File imports / include"): once-semantics registry
     * ("like #pragma once") -- canonicalized (boost::filesystem::canonical())
     * absolute-path keys of every file successfully imported so far via THIS
     * RuntimeContext, so a diamond/cycle of imports naturally terminates (a
     * file already in this set is skipped silently, see processImport()).
     * The main program itself is deliberately NOT added here: re-importing
     * it is merely redundant (its clauses/queries would simply run again),
     * not unsafe, and unify-run.cpp already passes the main program's
     * content directly to parseExecuteSegment(), never through
     * processImport().
     */
    std::set<std::string> m_importedFiles;
};


};
};

#endif

