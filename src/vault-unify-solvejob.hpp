#if !defined( _VAULT_UNIFY_SOLVEJOB_HPP )
#define _VAULT_UNIFY_SOLVEJOB_HPP

#include <map>
#include <string>

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
     * Return a textual representation of the result bindings as key/value pairs.
     */
    SolutionListPtr getSolutionList() const;

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


private:
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


    Engine* m_pEngine;

    int m_sliceCount;

    DebugLocation m_debugLocation;

    /// Number of unification errors (UnifyError) recorded so far. See getErrorCount().
    int m_errorCount;

    /// Description of the most recent unification error. See getLastError().
    std::string m_lastError;

};

} // namespace unify
} // namespace vault

#endif
