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
#define VAULT_UNIFY_DI( X, ... ) do { if( VAULT_UNIFY_ ## X ) { fprintf( stderr, __FILE__ ":%s():%d:", __func__, __LINE__ ); fprintf( stderr, __VA_ARGS__ ); } } while(0)


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
    ExecutionState( ExecutionState* parent );
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
        ClauseIterator() : m_currentState( NULL ) {}

        ClauseIterator( ExecutionState* es )
                // : m_currentState( NULL ) is in enterState
        {
            enterState( es );
        }
        ~ClauseIterator() {}
        
        bool isValid() {
            while(1) {
                if( m_itClause != m_itClauseEnd ) {
                    return true;
                }
                if( NULL==m_currentState->m_pParent ) {
                    return false;
                }
                enterState( m_currentState->m_pParent );
            }
        }
        
        const Clause* getClause() const {
            return *m_itClause;
        }
        
        void next() {
            ++m_itClause;
        }

    private:
        ExecutionState* m_currentState;
        std::list<Clause*>::const_iterator m_itClause;
        std::list<Clause*>::const_iterator m_itClauseEnd;
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
     */
    int appendClause( WorldPtr spWorld, Clause* );

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

private:    
    ConsTerm* m_pLeftHandTerm;

    static ClauseId m_nextUid;

    /// Uid of the clause.
    ClauseId m_uid;

    DebugLocation m_debugLocation;
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
class Engine
    : public ExecutionController
{
public:
    Engine();
    ~Engine();
    
    void setWorldChangeSink( WorldChangeSink* pWorldChangeSink ) {
        m_pWorldChangeSink = pWorldChangeSink;
    }

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
        const FileDebugInfo* pFileDebugInfo = NULL );

    Engine* getEngine() const { return m_pEngine; }
    WorldPtr getWorld() const { return m_spWorld; }

private:
    vault::unify::WorldPtr m_spWorld;

    vault::unify::Engine* m_pEngine;

    vault::unify::WorldChangeSink* m_pWorldChangeSink;

    vault::unify::ExecutionState* m_esRoot;
};


};
};

#endif

