#if !defined( _VAULT_UNIFY_SOLVEJOB_HPP )
#define _VAULT_UNIFY_SOLVEJOB_HPP

#include <map>
#include <string>
#include <vector>

namespace vault {
namespace unify {

/**
 * Each solve context contains the current state of iteration for one
 * logical part of execution.
 */
struct SolveContext {
public:

    /**
     * Create a solve context based on a given parent context.
     * The child job ought to try a unification step based on the
     * term referenced by the iterator.
     *
     * @param pStartState
     *    The execution state we are working inside.
     * @param scParent
     *    The parent solve context.
     * @param ucParent
     *    The unify context we are working in.
     * @param pNewGoalPart
     *    The goal part we shall solve. Might refer to another
     *    goal chain defined outside.
     *
     * Neither of these objects belong to us. They must exist as long
     * the solve context exists.
     */
    SolveContext(
        ExecutionState* pStartState, 
        SolveContext* scParent,
        UnifyContext* ucParent,
        GoalPart* pNewGoalPart,
        const GoalPartCursor& csCurrent
        );

    ~SolveContext();

    /// Our parent solve context.
    SolveContext* m_parentSolveContext;

    // Iteration state: Iterating over multiple results from a single clause.
    ClauseContinuationContext* m_pCCC;

    // Iteration state: Iterating over the list of clauses.

    /// The clause that we should try to unify next in a child state.
    ExecutionState::ClauseIterator m_itNextChildClause;
        
    // Tree node content

    //  Input for unification:
    
    /// Our current unify context we are working in.
    UnifyContext* m_pUnifyContext;

    /**
     * The goal part that belongs to my context. This is required to release
     * memory in the end. While the goal belongs to the unify context, the 
     * goal part belongs to this data structure.
     */
    GoalPart* m_pMyGoalPart;

    GoalPartCursor m_csCurrent;

    //  Output from unification:
    int m_sliceCount;
};


/**
 * A solve job exists to solve and monitor a goal.
 * 
 * A solve job holds two subtrees: One subtree of current actions (the tree
 * of solve contexts), and a subtree of (succeeded) partial unification
 * contexts. 
 */
class SolveJob 
        : public Job
{
public:
    SolveJob();
    virtual ~SolveJob();

    typedef std::map<std::string,std::string> SolutionMap;
    typedef boost::shared_ptr<SolutionMap> SolutionMapPtr;
    typedef std::list<SolutionMapPtr> SolutionList;
    typedef boost::shared_ptr<SolutionList> SolutionListPtr;

#if 0
    /**
     * Set the root execution state for this job. The execution state
     * belongs to the caller. It must not be removed as long the job
     * object exists.
     */
    virtual void setExecutionState( ExecutionState* es ) {
        m_pStartState = es;
    }
#endif


    virtual int performSlice();
    
    /**
     * Start the solve job in the context of the given engine.
     * The engine belongs to the caller. It must be removed as long
     * the job object exists.
     */
    virtual int startJob( Engine* );
    
    /**
     * Associate the world we operate in.
     */
    SolveJob& setWorld( WorldPtr spWorld );

    virtual int triggerRelease();

    /**
     * Set the goal to solve for this solve job. The goal object and
     * all of its sub-terms belong to caller. They must exist as long
     * the solve job exists. They will not be modified in any way by
     * solve job.
     */
    int setGoal( const Goal* );

    /**
     * Adopt a Goal object (typically the one just passed to setGoal())
     * so this job takes ownership of it: it will be deleted when this
     * job is destroyed, instead of leaking or requiring the caller to
     * track its lifetime.
     *
     * ROADMAP Phase 1 (Ownership model): this adopts only the Goal object
     * itself, not the TERM trees it references (its m_listAbstractTerms) --
     * those are handled separately, in ~SolveJob(), which now deletes them
     * too (see the comment there for why that is safe).
     */
    void adoptGoal( const Goal* );

    /**
     * Return a textual representation of the result bindings as key/value pairs.
     *
     * Engine item E7.4 -- a DELIBERATE divergence from
     * getGroundedSolutions() below, and the one place a reader will look
     * for it: this method still OMITS an unbound variable entirely, while
     * the term form now emits it as an unbound VarTerm. The two therefore
     * no longer always have the same key set, and only the term form
     * satisfies SESSION-API.md's "Var carries the variable's display name
     * for unbound bindings".
     *
     * The reason is that there is no honest string for an unbound
     * variable. Every candidate ("$x", "_", "$x = $x") is a rendering
     * choice, and the only surviving caller of this method is
     * unify-repl.cpp, whose printed output is a user-facing decision
     * nobody has taken. E7's own plan lists that REPL as something to
     * leave alone (plans/todo/lens/E7-STRUCTURED-VALUES.md, "Things to
     * leave alone"). Whoever takes that decision should delete this
     * paragraph and make the two agree again.
     */
    SolutionListPtr getSolutionList() const;

    /**
     * Engine item E7.2 (plans/todo/lens/E7-STRUCTURED-VALUES.md): the
     * solutions as TERMS instead of as text, owned by the returned object.
     *
     * getSolutionList() above flattens every binding to a string, which is
     * lossy in the one direction a front end cares about: `point( 1, 2 )`
     * arrives as nine characters somebody has to re-parse. This hands out
     * the structure instead.
     *
     * The reason it is a class rather than a plain container of pointers is
     * lifetime. m_listUnifySolutions holds non-owning pointers into this
     * job's arena, and ~SolveJob() frees that arena the moment the last
     * reference to the job goes away -- which Engine::executionLoop() does
     * as soon as the onFinished callback returns. So the job does not hand
     * out arena pointers at all: it GROUNDS each binding with
     * resolveTermGrounded(), whose contract is "every node returned is a
     * fresh allocation, safe to outlive pUCStackTop's own arena"
     * (include/vault-unify.hpp), and the result owns those clones. Terms
     * read out of a GroundedSolutions stay valid after the job is gone;
     * they die with the GroundedSolutions and not before.
     *
     * Move-only on purpose: a copy would give two objects the same term
     * pointers and the second destructor would double-free them.
     */
    class GroundedSolutions
    {
    public:
        /// One solution: variable display name -> its own grounded term tree.
        typedef std::map<std::string,const AbstractTerm*> BindingMap;
        typedef std::vector<BindingMap>::const_iterator const_iterator;

        GroundedSolutions() {}

        /**
         * Frees every term tree handed over by getGroundedSolutions().
         */
        ~GroundedSolutions();

        GroundedSolutions( GroundedSolutions&& other ) noexcept;
        GroundedSolutions& operator=( GroundedSolutions&& other );

        GroundedSolutions( const GroundedSolutions& ) = delete;
        GroundedSolutions& operator=( const GroundedSolutions& ) = delete;

        /// Number of solutions, in the order the engine emitted them.
        size_t size() const { return m_lsSolutions.size(); }

        bool empty() const { return m_lsSolutions.empty(); }

        /// The bindings of solution `idx`. Throws if idx >= size().
        const BindingMap& at( size_t idx ) const { return m_lsSolutions.at( idx ); }

        const_iterator begin() const { return m_lsSolutions.begin(); }
        const_iterator end() const { return m_lsSolutions.end(); }

        /**
         * The term bound to strVar in solution idx, or NULL if there is no
         * such solution or the goal has no variable of that name.
         *
         * Engine item E7.4 changed what NULL means. It used to also cover
         * "the goal has that variable and the solve left it unbound";
         * every variable the goal mentions now has a row, so that case
         * comes back as a non-NULL, unbound VarTerm carrying its display
         * name instead. NULL is now only ever "no such variable" -- which
         * is the answer a caller can actually act on, and the reason the
         * distinction is worth making: a front end that shows `$x = $x`
         * has told the user something true, and one that shows no row at
         * all has told them their variable does not exist.
         *
         * Test for it with dynamic_cast<const VarTerm*>, or let
         * toSessionValue() map it to Value::Kind::Var.
         */
        const AbstractTerm* find( size_t idx, const std::string& strVar ) const;

    private:
        friend class SolveJob;

        /**
         * Take over one solution's bindings (and the term trees they point
         * at). Empties mapBindings, so the caller cannot keep a second
         * handle on terms this object now owns.
         */
        void adoptSolution( BindingMap& mapBindings );

        /**
         * Free every term tree reachable from one solution's bindings.
         *
         * ONE std::set for the whole solution rather than a
         * deleteTermTree() per binding: term nodes can be reachable from
         * more than one root (see the ownership note above
         * collectTermTree() in include/vault-unify.hpp), and a per-term
         * delete would then free the same node twice. The set de-duplicates
         * by pointer identity -- the same pattern ~SolveJob() uses for a
         * Goal's term trees.
         *
         * A MapTerm's Atom* KEYS are freed by ~MapTerm() itself and are not
         * AbstractTerms, so they are neither collected nor deleted here;
         * its value terms are not freed by ~MapTerm() and are (they show up
         * as ordinary children of the walk).
         */
        static void releaseSolution( BindingMap& mapBindings );

        std::vector<BindingMap> m_lsSolutions;
    };

    /**
     * See GroundedSolutions above. Like getSolutionList(), this may only be
     * called while the job is still alive, i.e. from inside its onFinished
     * callback -- but unlike getSolutionList(), what it returns outlives it.
     *
     * Engine item E7.4: EVERY variable the goal mentions gets a binding in
     * every solution. An unbound one arrives as an unbound VarTerm rather
     * than as a missing key, so a solution's binding count is now the
     * goal's variable count and not "however many happened to be bound".
     * A front end that treated an empty binding map as "the goal has no
     * variables" needs to look at the map's contents instead.
     */
    GroundedSolutions getGroundedSolutions() const;

    virtual DebugLocation getDebugLocation();
    virtual int getDebugStack( std::list<StackFrame>& );
    virtual int getDebugProperties( std::list<DebugProperty>&, uint64_t );

    /**
     * Number of unification errors (UnifyError) recorded while solving this
     * job so far. Phase 1 (ROADMAP): unification errors are reported and
     * recorded here instead of being silently treated as "did not unify".
     * Zero means no error has occurred.
     */
    int getErrorCount() const { return m_errorCount; }

    /**
     * A human-readable description of the most recently recorded
     * unification error, or an empty string if none occurred yet.
     */
    const std::string& getLastError() const { return m_lastError; }

    /**
     * Every error recorded while solving this job, in order -- engine item
     * E10.
     *
     * getLastError() keeps only the most recent one, which is enough for
     * unify-run's one-line summary and useless for a diagnostics panel: a
     * goal that fails ten different ways has ten things to show the user,
     * and nine of them used to be overwritten. Kept alongside the count and
     * the last message rather than replacing them, because both are on
     * unify-run's existing path and E10 is not licence to change it.
     *
     * LIFETIME: valid only while the job is; ~SolveJob frees the job's
     * whole arena. A consumer that needs these after completion must copy
     * them inside the onFinished callback, exactly like getSolutionList().
     */
    const std::vector<Diagnostic>& getDiagnostics() const
        { return m_lsDiagnostics; }


private:
    /**
     * Collect every variable occurring in this job's goal, as
     * VarTermId -> the variable's original (display) name.
     *
     * ONE copy of the collection rule, shared by getSolutionList() and
     * getGroundedSolutions(). They must agree on which variables a
     * solution has and what each one is called -- two copies of this walk
     * is exactly how the string form and the term form would quietly grow
     * different key sets.
     */
    void collectGoalVarNames( std::map<VarTermId,std::string>& out_mapVarTerms ) const;

    /**
     * Start a unification process in the current state.
     */
    vault::unify::Clause::UnificationState startUnification(
        UnifyContext* uc,
        ClauseContinuationContext*& pCCC );

    /**
     * Emit a unify context that contains a solution.
     */
    int emitSolution( UnifyContext* uc );

    // int enterGoal( SolveContext* sc );

    void discardTop( SolveContext*& sc );

    /**
     * Record a unification error: increments getErrorCount() and stores
     * the given message so it can be retrieved via getLastError().
     */
    void recordError( const std::string& strError );

    /**
     * Adopt a UnifyContext instantiated while solving this job into the
     * job's arena (see m_arenaUnifyContexts): it will be deleted when this
     * job is destroyed. Returns its argument unchanged so it can be used
     * inline at the `new` call site, e.g.
     * `UnifyContext* uc = adoptUnifyContext( new UnifyContext( ... ) );`.
     */
    UnifyContext* adoptUnifyContext( UnifyContext* pUnifyContext ) {
        m_arenaUnifyContexts.push_back( pUnifyContext );
        return pUnifyContext;
    }

    /**
     * Adopt a GoalPart instantiated while solving this job into the job's
     * arena (see m_arenaGoalParts). See adoptUnifyContext().
     */
    GoalPart* adoptGoalPart( GoalPart* pGoalPart ) {
        m_arenaGoalParts.push_back( pGoalPart );
        return pGoalPart;
    }

    /**
     * The goal to solve.
     */
    const Goal* m_pGoal;
    
    /**
     * The state we shall base our unification on.
     */
    ExecutionState* m_pStartState;

    /**
     * The world we operate in.
     */
    WorldPtr m_spWorld;

    /**
     * The current stack of solve contexts.
     * 
     * TXWTODO: This context stack actually is a special
     * case of the tree of contexts to solve.
     */
    std::list<SolveContext*> m_stackContext;

    /// The list of leaf nodes containing solutions.
    std::list<UnifyContext*> m_listUnifySolutions;

    /**
     * ROADMAP Phase 1 ("Ownership model"): the per-job arena.
     *
     * Every UnifyContext and GoalPart instantiated while solving this job
     * is registered here (via adoptUnifyContext()/adoptGoalPart()) and
     * owned by this job: they are freed in ~SolveJob(), not one at a time
     * while solving. m_listUnifySolutions above and every SolveContext's
     * m_pUnifyContext/m_pMyGoalPart are non-owning references into these
     * arenas - they must never be deleted directly.
     */
    std::vector<UnifyContext*> m_arenaUnifyContexts;
    std::vector<GoalPart*> m_arenaGoalParts;

    /// Goal objects adopted via adoptGoal(); freed in ~SolveJob().
    std::vector<const Goal*> m_arenaGoals;

    Engine* m_pEngine;

    int m_sliceCount;

    DebugLocation m_debugLocation;

    /// Number of unification errors (UnifyError) recorded so far. See getErrorCount().
    int m_errorCount;

    /// Description of the most recent unification error. See getLastError().
    std::string m_lastError;

    /// See getDiagnostics() above (engine item E10).
    std::vector<Diagnostic> m_lsDiagnostics;

};

} // namespace unify
} // namespace vault

#endif
