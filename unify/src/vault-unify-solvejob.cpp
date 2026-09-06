/**
 * @file vault-unification.cpp
 *
 * @author Timo Weggen
 *
 * Implementation of unification engine.
 */

#include <string>
 
#include <boost/shared_ptr.hpp>

#include <list>

#include <vault-unification.hpp>
#include <vault-unify-solvejob.hpp>
#include <vault-unify-debug.hpp>
#include <vault-unify-clause-standard.hpp>

namespace vault {
namespace unify {


/**
 * ROADMAP Phase 2 (runtime assert/retract, SPEC.md): does pTerm's tree
 * (typically a resolveTermGrounded() clone) still contain an unbound
 * VarTerm anywhere? Used by the `__builtin_assert` special form
 * (SolveJob::performSlice() below) to enforce v1's "facts must be ground"
 * rule. Walks generically via the existing TermTraversable/
 * AbstractTermIterator machinery, exactly like collectTermTree()
 * (vault-unify-terms.cpp) -- but this is a read-only predicate, not a
 * collect-and-delete pass, so it stays local to this file rather than
 * joining that module's exported ownership helpers.
 */
static bool termTreeHasUnboundVar( const AbstractTerm* pTerm )
{
    if( !pTerm ) {
        return false;
    }
    if( dynamic_cast<const VarTerm*>( pTerm ) ) {
        return true;
    }
    bool found = false;
    AbstractTermIterator* pIt = pTerm->abstractTermIterator();
    if( pIt ) {
        while( !found && pIt->isValid() ) {
            const TermTraversable* pChildTraversable = pIt->getTermTraversable();
            const AbstractTerm* pChildTerm = dynamic_cast<const AbstractTerm*>( pChildTraversable );
            if( pChildTerm && termTreeHasUnboundVar( pChildTerm ) ) {
                found = true;
            }
            pIt->next();
        }
        delete pIt;
    }
    return found;
}


SolveContext::~SolveContext()
{
}


SolveContext::SolveContext(
    ExecutionState* m_pStartState, 
    SolveContext* scParent,
    UnifyContext* ucParent,
    GoalPart* pNewGoalPart,
    const GoalPartCursor& csCurrent
    )
    : m_parentSolveContext( scParent )
    , m_pCCC( NULL )
    , m_pUnifyContext( ucParent )
    , m_pMyGoalPart( pNewGoalPart )
    , m_csCurrent( csCurrent )
    , m_sliceCount( 0 )
{
    VAULT_UNIFY_DI( UNIFY, "SolveContext(): Creating for %s.\n",
        pNewGoalPart?pNewGoalPart->m_pNewGoal->toString().c_str():"(oldone)" );
    m_itNextChildClause = m_pStartState->clauseIterator();
}


SolveJob::SolutionListPtr SolveJob::getSolutionList() const
{
    int res;

    /*
     * Local visitor class to collect all var terms in goal.
     */
    class CollectVarTermsVisitor {
    public:
        CollectVarTermsVisitor( std::map<VarTermId,std::string>& varTermMap ) 
            : m_varTermMap( varTermMap ) {}

        void operator() ( const TermTraversable* term ) {
            const VarTerm* varTerm = dynamic_cast<const VarTerm*>( term );
            if( varTerm ) {
                m_varTermMap[varTerm->getBinding()] = varTerm->getOriginalVarName();
            }
        }
    private:
        std::map<VarTermId,std::string>& m_varTermMap;
    };

    VAULT_UNIFY_DI( SOLUTION, "Called.\n" );

    /* 
     * First, collect the var terms.
     */
    std::map<VarTermId,std::string> mapVarTerms;
    CollectVarTermsVisitor varTermCollector( mapVarTerms );
    m_pGoal->applyVisitor( varTermCollector );

    SolutionList* l = new SolutionList;
    SolutionListPtr spList( l );

    /* 
        case DebugListener::CRASHED:
     * Now, iterate through the possible solutions.
     * For each of the solutions, find a set of assignments for the
     * list of variables collected above.
     */
    std::list<UnifyContext*>::const_iterator itSol, itSolEnd = m_listUnifySolutions.cend();
    for( itSol=m_listUnifySolutions.begin(); itSol != itSolEnd; ++itSol ) {
        /*
         * In this data structure, we will collect the results.
         */
        SolutionMap* m = new SolutionMap;
        SolutionMapPtr spMap( m );

        UnifyContext* uc = *itSol;
        /*
         * Iterate and resolve all variables that have been found in the goal.
         */
        std::map<VarTermId,std::string>::const_iterator itVar, itVarEnd = mapVarTerms.end();
        for( itVar=mapVarTerms.begin(); itVar != itVarEnd; ++itVar ) {
            AssignmentId aid( 0, itVar->first );
            InstanceId iid;
            res = uc->findVarBinding( aid, iid );
            if( res>=0 ) {
                /*
                 * This variable seems to have a value. Find out the content.
                 */
                boost::shared_ptr<SingleVarInstance> spInstance;
                // We have to find the instance recursively starting at the uc leaf.
                (void) uc->findVarInstance( iid, spInstance );
                if( spInstance && spInstance->getTerm() ) {
                    const AbstractTerm* pTerm = spInstance->getTerm();
                    std::string value = pTerm->toString();
                    VAULT_UNIFY_DI( SOLUTION, "Var %s is \"%s\".\n", itVar->second.c_str(), value.c_str() );
                    (*m)[itVar->second] = value.c_str();
                } else {
                    VAULT_UNIFY_DI( SOLUTION, "%lld::VT%lld" /* ",%lld" */ "= %lld does not seem to be instantiated yet.\n"
                        , (long long) aid.getUnifyContextId()
                        , (long long) aid.getVarTermId()
                        , (long long) iid
                        );
                }                    

            }
        }
        // TXWTODO: Do not break, but extend the data structure we return.
        l->push_back( spMap );
    }

    return spList;
}


void SolveJob::discardTop( SolveContext*& sc )
{
    VAULT_UNIFY_DI( UNIFY, "Popping Solve context.\n" );
    SolveContext* scTop = m_stackContext.back();
    if( sc != scTop ) {
        // TXWTODO: This invariant (sc must be the current stack top) should never be
        // violated. Rather than aborting the whole process, log and back out, treating
        // this as "nothing to discard" so the caller can keep running.
        VAULT_UNIFY_DI( ALWAYS, "Internal error in discardTop(): sc (%p) does not match m_stackContext.back() (%p). Not discarding.\n",
            (void*) sc, (void*) scTop );
        return;
    }
    m_stackContext.pop_back();
    delete sc;
    sc = NULL;
}


int SolveJob::emitSolution( UnifyContext* uc )
{
    m_listUnifySolutions.push_back( uc );
    return 0;
}


int SolveJob::startJob( Engine* pEngine )
{
    m_pEngine = pEngine;

    GoalPart* pGoalPart = adoptGoalPart( new GoalPart(
        m_pGoal,                // Externally provided goal
        NULL,                   // no parent Goal Part
        0,                      // No associated unifycontextid (no vars contained)
        vault::unify::Goal::GoalIterator() ) );
    // pEngine->logChange() << *pGoalPart;

    UnifyContext* pRootUnifyContext = adoptUnifyContext( new UnifyContext(
        NULL,                   // no parent unify context
        GoalPartCursor(),
        ExecutionState::ClauseIterator() ) );
    // pEngine->logChange() << *pRootUnifyContext;
        
    // Create a root UnifyContext.
    // Create new Clause Context, empty vars.
    SolveContext* sc = new SolveContext( m_pStartState, 
        NULL, // no parent solve context.
        pRootUnifyContext,
        pGoalPart,
        GoalPartCursor( pGoalPart ) );
    m_stackContext.push_back( sc );

    return 0;
}


/**
 * Try to unify the term as described in the unify context with a given
 * clause.
 *
 * Unify context contains all parameters for unification:
 * - the clause that we should try to unify
 * - the term that we are trying to unify.
 * - a parent unification context carrying variable bindings.
 *
 * @return
 *    Returns zero, if unification process is not done yet.
 *    Returns positive, if unifications process has terminated.
 */
vault::unify::Clause::UnificationState SolveJob::startUnification(
        UnifyContext* pUCStackTop,
        ClauseContinuationContext*& pCCC )
{
    //bool isMatch = false;
    bool isMatchName = false;

    /*
     * Read the next library clause we want to try unifying with.
     */
    const Clause* pClause = pUCStackTop->m_itClause.getClause();

    /*
     * This is the unify context that created the next term in the current goal.
     */
    UnifyContext* pGoalUnifyContext =
            pUCStackTop->m_csTermToUnify.getOriginUnifyContext();

    /*
     * Here we store the new goal part (i.e. the clause right hand side),
     * if the unification succeeds. 
     */
    const Goal* pResultingGoal = NULL;
    // We know that this is gonna be a consterm.
    const ConsTerm* pConsTerm = dynamic_cast<const ConsTerm*>( pUCStackTop->m_csTermToUnify.getAbstractTerm() );
    // A couple of operations rely on this being a consterm.
    if( pConsTerm ) {
        if( pConsTerm->isNegated() ) {
            // Remember that the consterm is negated.
            pUCStackTop->setNegated( true );
        }
        // For some operatinos, like the ! operator, we need to know wether
        // we had at least on structural match.
        const ConsTerm* pClauseTerm = pClause->leftHandTerm();

        if( pClauseTerm->getName() == pConsTerm->getName()  ) {
            isMatchName = true;
            if( pClauseTerm->getArity() == pConsTerm->getArity() ) {
                pUCStackTop->setFoundClause( true );
                // isMatch = true;
            }
        }

    } else {
        VAULT_UNIFY_DI( ALWAYS, "Non consterm found.\n" );
    }

    if( !isMatchName ) {
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return vault::unify::Clause::UnificationOK;
    }

    vault::unify::Clause::UnificationState unificationState = pClause->startUnification(
        m_pEngine,
        pUCStackTop,  // StackTop
        pGoalUnifyContext, // Other 
        pUCStackTop, // Mine 
        pResultingGoal, // Output 
        pCCC );

    (void) pResultingGoal; // We don't need it.

    return unificationState;
}


/**
 * Return the position of the current execution.
 * Result is valid only in halted or finished state.
 */
DebugLocation SolveJob::getDebugLocation()
{
    // Mutex?
    // if( m_debugLocation.line == 0 ) {
        
        // Look up current debug location from map.
        if( !m_stackContext.empty() ) {

            /*
             * Get top stack element of our solve context tree.
             */
            SolveContext* sc = m_stackContext.back();

            if( sc ) {
                /*
                 * If we reached the end of the goal we ought to solve, then we can consider
                 * the current state of variables to be a solution n-tuple.
                 */
                if( sc->m_csCurrent.isValid() ) {
                    const AbstractTerm* pCurrentTerm = sc->m_csCurrent.getAbstractTerm();
                    const TermDebugInfo* pTermDebugInfo = m_spWorld->getTermDebugInfo( pCurrentTerm );
                    if( pTermDebugInfo ) {
                        const FileDebugInfo* pFileDebugInfo = pTermDebugInfo->getFileDebugInfo();
                        if( pFileDebugInfo ) {
                            m_debugLocation = DebugLocation(
                                pFileDebugInfo->getFileUri(),
                                pTermDebugInfo->getLine() );
                        }
                    }
                }
            }
        }
    //}

    return m_debugLocation;
}


int SolveJob::getDebugStack( std::list<StackFrame>& out_lsStack )
{
    // Look up current debug location from map.
    if( !m_stackContext.empty() ) {

        std::list<SolveContext*>::iterator itEnd = m_stackContext.end(), it;
        // SolveContext* sc = m_stackContext.back();

        for( it=m_stackContext.begin(); it != itEnd; ++it ) {
            SolveContext* sc = *it;
            StackFrame stackFrame;
            uint64_t id = 0;

            if( sc->m_csCurrent.isValid() ) {
                const UnifyContext* pUC = sc->m_pUnifyContext;
                if( pUC ) {
                    id = (uint64_t) pUC->getUnifyContextId();
                }
                DebugLocation debugLocation;
                const AbstractTerm* pCurrentTerm = sc->m_csCurrent.getAbstractTerm();
                const TermDebugInfo* pTermDebugInfo = m_spWorld->getTermDebugInfo( pCurrentTerm );
                std::string strFile;
                int64_t line = 0;
                std::string strMethod;
                if( pTermDebugInfo ) {
                    const FileDebugInfo* pFileDebugInfo = pTermDebugInfo->getFileDebugInfo();
                    if( pFileDebugInfo ) {
                        strFile = pFileDebugInfo->getFileUri();
                        line = pTermDebugInfo->getLine();
                    }
                } else {
                    VAULT_UNIFY_DI( ALWAYS, "No Debug Location for \"%s\".\n",
                        pCurrentTerm->toString().c_str() );
                }
                if( pCurrentTerm ) {
                    strMethod = pCurrentTerm->toString();
                }
                stackFrame = StackFrame( 
                    DebugLocation( strFile, line, strMethod ), id );
            }
            out_lsStack.push_back( stackFrame );
        }
    }

    return 0;
}


int SolveJob::getDebugProperties( std::list<DebugProperty>& out_lsProperties, uint64_t /*frameId*/ )
{
    // Look up current debug location from map.
    if( !m_stackContext.empty() ) {

        std::list<SolveContext*>::iterator itEnd = m_stackContext.end(), it;
        // SolveContext* sc = m_stackContext.back();

        for( it=m_stackContext.begin(); it != itEnd; ++it ) {
            SolveContext* sc = *it;
            // uint64_t id = 0;

            if( sc->m_csCurrent.isValid() ) {
                const UnifyContext* pUC = sc->m_pUnifyContext;
                if( pUC ) {

                    // id = (uint64_t) pUC->getUnifyContextId();

                    // We return bindings for this frame, but instances from parent frames.
                    std::map<AssignmentId,InstanceId>::const_iterator it,
                        itEnd = pUC->m_mapVarMappings.end();
                    for( it=pUC->m_mapVarMappings.begin(); it!=itEnd; ++it ) {
                        boost::shared_ptr<SingleVarInstance> spInstance;
                        AssignmentId aid = it->first;
                        InstanceId iid = it->second;
                        pUC->findVarInstance( iid, spInstance );
                        DebugLocation debugLocation;

                        std::string strValue;
                        char internalName[50];
                        snprintf( internalName, 50, "%lldVT%lld" /* ",%lld" */ "IID%lld"
                            , (long long) aid.getUnifyContextId()
                            , (long long) aid.getVarTermId()
                            , (long long) iid
                            );
                        if( spInstance ) {
                            const UnifyContext* pUCInstance = spInstance->getUnifyContext();
                            if( pUCInstance ) {
                                char prefix[50];
                                snprintf( prefix, 50, "%lld::", (long long) pUCInstance->getUnifyContextId() );
                                //strValue += prefix;
                            }
                            const AbstractTerm* pTerm = spInstance->getTerm();
                            if( pTerm ) {
                                strValue += pTerm->toString().c_str();
                                const TermDebugInfo* pTermDebugInfo = m_spWorld->getTermDebugInfo( pTerm );
                                if( pTermDebugInfo ) {
                                    const FileDebugInfo* pFileDebugInfo = pTermDebugInfo->getFileDebugInfo();
                                    if( pFileDebugInfo ) {
                                        debugLocation = DebugLocation(
                                            pFileDebugInfo->getFileUri(),
                                            pTermDebugInfo->getLine() );
                                    }
                                }
                            }
                        } else {
                            strValue = "(uninitialized)";
                        }

                        out_lsProperties.push_back(
                            DebugProperty( 
                                internalName, strValue,
                                debugLocation
                                ) );
                    }

                }

            }
        }
    }

    return 0;
}


/**
 * Operate as much as possible on the current goal. Returns if no further atomic
 * unification can be performed.
 *
 * Notes on debugging conditions:
 * The debugger interface allows to STEP_INTO, STEP_OVER, STEP_OUT .
 * In addition, breakpoints may be set.
 * The current mode of operation is stored in the solve job and can be
 * accessed with getDebugTargetState(). If a debug target state of STEP_INTO
 * is detected, the operation is halted in every iteration.
 */
int SolveJob::performSlice()
{
    ++m_sliceCount;

    /*
     * Continue searching for new solutions in current state.
     */
    while( !m_stackContext.empty() ) {
 
        /*
         * Get top stack element of our solve context tree.
         */
        SolveContext* sc = m_stackContext.back();

        ++sc->m_sliceCount;

        /*
         * ROADMAP Phase 2 (runtime assert/retract, SPEC.md -- "logical
         * update view"): the ROOT SolveContext (m_parentSolveContext==NULL;
         * exactly one per job, pushed by startJob()) is special: for a
         * top-level query, startJob() runs on the PARSER thread at PARSE
         * time (RuntimeContext::parseExecuteSegment() calls it right after
         * parsing the query, long before this job is drained off the
         * engine's queue and actually run here, on the worker thread) --
         * so the m_itNextChildClause SolveContext's constructor built back
         * then captured its generation snapshot at PARSE time, not at
         * "when this search actually starts" like every other
         * SolveContext's iterator correctly does (every non-root
         * SolveContext is pushed HERE, in performSlice(), already running
         * on the worker thread). A parse-time snapshot is too early: it
         * can predate mutations a PRIOR job (FIFO-earlier on this same
         * worker thread) already made to the database by the time this
         * job's first slice actually runs, wrongly hiding them.
         *
         * Fix: on the root context's very first visit (sc->m_sliceCount
         * having just become 1, above) -- and ONLY then, never on a later
         * revisit, which would be a backtrack into an already-in-progress
         * scan whose view of the database must NOT jump forward mid-scan
         * -- reconstruct m_itNextChildClause from scratch, capturing the
         * generation as it stands right now, at actual execution time.
         * m_pStartState is the same ExecutionState the stale iterator was
         * originally built from (SolveContext's constructor), so this is
         * exactly "redo that construction, now instead of at parse time".
         *
         * This also fires for the nested findall SolveJob's own root
         * context (constructed by ITS startJob(), see the
         * __builtin_findall block below) on its own first slice -- but
         * harmlessly: that startJob()/performSlice() pair are adjacent
         * statements on the SAME (worker) thread with no intervening
         * mutation, so the reconstructed snapshot is identical to the one
         * already captured.
         */
        if( NULL==sc->m_parentSolveContext && 1==sc->m_sliceCount ) {
            sc->m_itNextChildClause = m_pStartState->clauseIterator();
        }

        /*
         * If we reached the end of the goal we ought to solve, then we can consider
         * the current state of variables to be a solution n-tuple.
         */
        if( !sc->m_csCurrent.isValid() ) {
            VAULT_UNIFY_DI( ITERATE, "No more parts found in entire chain.\n" );
            VAULT_UNIFY_DI( SOLUTION, "SOLUTION: %s.\n", 
                sc->m_pUnifyContext?sc->m_pUnifyContext->toString().c_str():"(nil?)" );

            if( sc->m_pUnifyContext ) {
                emitSolution( sc->m_pUnifyContext );
            }

            /*
             * There is no next element in the entire goal part chain.
             * So we can pop out and discard this solution context.
             */
            discardTop( sc );
            continue;
        }

        VAULT_UNIFY_DI( ITERATE, "sc->m_itCurrentTerm now points to %s, ucid==%lld.\n"
            , sc->m_csCurrent.getAbstractTerm()->toString().c_str()
            , sc->m_csCurrent.getOriginUnifyContext()
                ?(uint64_t) sc->m_csCurrent.getOriginUnifyContext()->getUnifyContextId()
                :0ll );

        // Look for debug flags
        DebugListener::ChangeReason debugTargetState = getDebugTargetState();

        /*
         * If debugging, a lot of continuation commands want to pause before the
         * first execution of anything. So look, wether we ought to halt at this point.
         */
        if( 1==m_sliceCount ) {

            switch( debugTargetState ) {

            case DebugListener::STEP_INTO:
                // Handled by solve context local slice count.
                break;

            case DebugListener::START:
                // Leave.
                state( Job::DEBUGHALTED );
                return 0;
                break;

            case DebugListener::STEP_OVER:
            case DebugListener::STEP_OUT:
                // Don't know, basically is a regular.
                // Fall through
            case DebugListener::REGULAR:
                // Just continue to operate.
                break;

            case DebugListener::STOP:
            case DebugListener::DETACH:
                state( Job::FINISHED );
                return 0;

            default:
            case DebugListener::BREAKPOINT:
                // Not applicable, internal error.
                VAULT_UNIFY_DI( ALWAYS, "Internal error: Wrong debug target state %d in job.\n",
                    (int) debugTargetState );
                break;

            }

        }

        if( 1==sc->m_sliceCount ) {

            switch( debugTargetState ) {

            case DebugListener::STEP_INTO:
                // Leave.
                state( Job::DEBUGHALTED );
                return 0;
                break;

            case DebugListener::START:
            case DebugListener::STEP_OVER:
            case DebugListener::STEP_OUT:
            case DebugListener::REGULAR:
            case DebugListener::STOP:
            case DebugListener::DETACH:
            default:
            case DebugListener::BREAKPOINT:
                // Doesn't matter.
                break;

            }

        }


        /*
         * IMPORTANT (ROADMAP Phase 2, Cut): this exhaustion check MUST run
         * BEFORE the cut-recognition block below, not after. A context
         * whose m_itNextChildClause was invalidated by a cut (see below)
         * needs to fall straight into discardTop() the next time it
         * becomes the stack top (i.e. once its own continuation has been
         * fully backtracked out of) -- never re-examine sc->m_csCurrent
         * and re-recognize the very same cut term a second time. Ordering
         * the exhaustion check first gives cut exactly-once semantics for
         * free: isValid() is true the first time a cut term is reached
         * (the iterator was just freshly constructed for this
         * SolveContext), so the cut block below still runs then; the cut
         * block invalidates sc's OWN iterator as part of its prune (see
         * below), so isValid() is false on every subsequent visit, and
         * this check pops the context instead of re-running the cut.
         */
        if( !sc->m_itNextChildClause.isValid() ) {
            VAULT_UNIFY_DI( ITERATE, "No next child clause to test.\n" );

            /*
             * When there is no next child clause to test, we finished
             * one depth-first branch. So pop out again.
             */
            discardTop( sc );
            continue;
        }

        /*
         * ROADMAP Phase 2 (Cut): `cut;` desugars (parser, see
         * AnyTermFactory::operator()(const ConsTermInput&) in
         * vault-unify-parser.cpp) to the reserved, zero-arity ConsTerm
         * "__builtin_cut". It is handled here directly -- NOT as a
         * registered Clause/builtin -- because cut needs access to the
         * SolveContext stack itself (to prune choice points), which no
         * Clause::startUnification() implementation ever gets.
         *
         * Semantics (standard Prolog cut, scoped to the clause containing
         * it -- see SPEC.md): cut succeeds exactly once and commits:
         * (a) no further clause alternatives are tried for the goal G that
         *     selected the clause C cut appears in, and
         * (b) goals in C's body to the LEFT of the cut do not backtrack
         *     into new solutions.
         * Goals AFTER the cut are unaffected -- none of their SolveContexts
         * exist yet at this point (they are only pushed once the search
         * actually reaches them), so there is nothing above the current
         * stack top to invalidate for them.
         *
         * ENTRY CONTEXT: given X = the GoalPart that owns the term
         * currently at sc->m_csCurrent (the clause body -- or, for a
         * query-level cut, the query's own root GoalPart -- containing the
         * cut), walk sc's m_parentSolveContext chain up to the unique
         * ancestor p with p->m_pMyGoalPart==X: that p is the SolveContext
         * created (in the "haveNewGoal" branch below) for X's very FIRST
         * body term -- every other SolveContext along X's own term chain
         * is pushed via the "!haveNewGoal" terminal-continuation branch,
         * which always passes pNewGoalPart=NULL, so p is the unique such
         * ancestor. p->m_parentSolveContext is then the context whose
         * m_itNextChildClause enumerates G's alternative clauses (it PUSHED
         * p when it matched clause C) -- that is the entry context. At
         * query top level p has no parent (it IS the root SolveContext
         * pushed directly in startJob()): there is no separate "caller" to
         * cut off, so p itself is the entry context in that case.
         *
         * PRUNE: m_stackContext is a genuine LIFO stack -- SolveContexts
         * are only ever removed from its back (discardTop()), so between
         * "now" and when the entry context was pushed, every still-live
         * context created since sits, in order, between the entry context
         * and the stack's current back. Invalidating each one's
         * m_itNextChildClause (from the top down to and including the
         * entry context) therefore prunes exactly: every choice point for
         * goals to the left of the cut (including any left there by a
         * fully-resolved sibling subtree, e.g. a preceding goal that itself
         * called a further user-defined rule -- correct per (b), those are
         * still "to the left"), the choice point for the first body term
         * itself, and G's own remaining clause alternatives (a) -- and
         * nothing belonging to goals after the cut, since those contexts
         * do not exist yet. Invalidating stops trying further candidates
         * without popping anything, so already-found continuations above
         * the cut keep running; a pruned context simply falls into the
         * ordinary "no next child clause" discardTop() path above once
         * control naturally backtracks into it.
         *
         * This also invalidates sc's OWN clause iterator (sc is the loop's
         * first entry, at m_stackContext.back()) -- this is what makes cut
         * exactly-once (see the comment above the exhaustion check just
         * above): without it, sc's iterator would still enumerate every
         * remaining clause by name on a later visit, and (a) it would
         * re-recognize and re-run this very block on the very next visit
         * (infinite/duplicate re-execution -- a real bug caught by hand-
         * tracing, not merely a defensive nicety), and (b) even if that
         * were somehow avoided, a user clause literally named bare `cut`
         * (parsed to this very same reserved name, see the parser) would
         * otherwise wrongly unify with this term once backtracking
         * returned to sc.
         */
        {
            const ConsTerm* pCutCandidate = dynamic_cast<const ConsTerm*>( sc->m_csCurrent.getAbstractTerm() );
            if( pCutCandidate && 0==pCutCandidate->getArity()
                    && pCutCandidate->getName().value() == "__builtin_cut" ) {

                GoalPart* pCutGoalPart = sc->m_csCurrent.getGoalPart();
                SolveContext* p = sc;
                while( p->m_pMyGoalPart != pCutGoalPart ) {
                    p = p->m_parentSolveContext;
                }
                SolveContext* scEntry = p->m_parentSolveContext ? p->m_parentSolveContext : p;

                std::list<SolveContext*>::reverse_iterator itPrune = m_stackContext.rbegin(),
                    itPruneEnd = m_stackContext.rend();
                for( ; itPrune != itPruneEnd; ++itPrune ) {
                    (*itPrune)->m_itNextChildClause.invalidate();
                    if( *itPrune == scEntry ) {
                        break;
                    }
                }
                if( itPrune == itPruneEnd ) {
                    // Should never happen (scEntry is, by construction, an
                    // ancestor of sc and therefore still on the stack --
                    // see the derivation above); log rather than loop
                    // forever or dereference past the end.
                    VAULT_UNIFY_DI( ALWAYS, "Internal error in cut: entry context %p not found on stack.\n",
                        (void*) scEntry );
                }

                VAULT_UNIFY_DI( ITERATE, "cut: pruned choice points down to and including entry context %p.\n",
                    (void*) scEntry );

                /*
                 * Advance exactly like the terminal-clause continuation
                 * below: cut succeeds once, immediately, carrying forward
                 * the CURRENT bindings -- it binds nothing new, so no
                 * fresh UnifyContext is created.
                 */
                GoalPartCursor csNext( sc->m_csCurrent );
                csNext.next();

                SolveContext* scChild = new SolveContext(
                    m_pStartState,          // ExecutionState
                    sc,                     // SolveContext (parent)
                    sc->m_pUnifyContext,    // Parent unify context: unchanged.
                    NULL,                   // No new GoalPart -- same chain continues.
                    csNext                  // where to iterate next.
                    );
                m_stackContext.push_back( scChild );

                continue;
            }
        }

        /*
         * ROADMAP Phase 2 ("findall"): `$out = findall( $tmpl, $goal );`
         * desugars (AnyTermFactory::operator()(const InfixTermsInput&),
         * vault-unify-parser.cpp) to the reserved goal term
         * `__builtin_findall(tmpl, subgoal, out)`, recognized here
         * structurally -- exactly like cut above -- rather than as a
         * registered Clause: findall needs to run a whole nested search to
         * exhaustion on the spot, which no Clause::startUnification() ever
         * gets the means to do (it only ever sees two UnifyContexts and an
         * Engine, never the solver's own job machinery).
         *
         * Mechanism (see SPEC.md's findall section for the full analysis):
         * a fresh, synchronous, same-thread SolveJob solves `subgoal` as if
         * it were a brand-new top-level query (its own root UnifyContext
         * has no parent) against the SAME World -- so it is never enqueued
         * on m_pEngine, and OUTER bindings are not consulted (a documented
         * v1 scope limitation: only bindings the subgoal itself creates,
         * during its own nested solve, are visible when resolving the
         * template). This is exactly the fresh-top-level-query shape
         * SolveJob::getSolutionList() already assumes when it resolves
         * every query variable via AssignmentId(0, ...) -- see
         * resolveTermGrounded() (vault-unify-terms.cpp), which mirrors that
         * resolution for the (grounded, cloned) template instead of a
         * string.
         *
         * Each solution's template instance is grounded (every bound
         * VarTerm resolved to a fresh CLONE, since the nested job's entire
         * arena is destroyed the moment this block returns -- nothing may
         * keep pointing into it) into a fresh ArrayTerm, adopted into a
         * fresh UnifyContext and unified against `out` exactly like
         * __builtin_eval's result (ArithEvalBuiltinClause, vault-unify-
         * clause-builtin-arith.cpp). findall itself never fails (zero
         * solutions -> an empty array) and is deterministic (exactly one
         * solution, no choice point) -- but the final unify against `out`
         * can still fail/error like any ordinary unification, e.g. if
         * `out` is already bound to something incompatible.
         */
        {
            const ConsTerm* pFindallCandidate = dynamic_cast<const ConsTerm*>( sc->m_csCurrent.getAbstractTerm() );
            if( pFindallCandidate && 3==pFindallCandidate->getArity()
                    && pFindallCandidate->getName().value() == "__builtin_findall" ) {

                const AbstractTerm* pTmplTerm = pFindallCandidate->getTermAt( 0 );
                const AbstractTerm* pSubgoalTerm = pFindallCandidate->getTermAt( 1 );
                const AbstractTerm* pOutTerm = pFindallCandidate->getTermAt( 2 );

                UnifyContext* pUCOriginScope = sc->m_csCurrent.getOriginUnifyContext();

                // a. Nested, synchronous, same-thread solve of `subgoal`,
                // never enqueued -- driven directly, on this very stack
                // frame. nestedGoal merely wraps pSubgoalTerm (owned by the
                // enclosing clause/query's own term tree); it is never
                // adopted, so its destructor here frees only the thin Goal
                // wrapper, never pSubgoalTerm itself.
                Goal nestedGoal( pSubgoalTerm );
                SolveJob nestedJob;
                nestedJob.setWorld( m_spWorld );
                nestedJob.setGoal( &nestedGoal );
                nestedJob.startJob( m_pEngine );
                // REGULAR is the default debug target state (Job::Job(),
                // vault-unify-job.cpp) for a job never handed to a
                // debugger -- performSlice() runs to full exhaustion in a
                // single call in that state (it only returns early for
                // debug-halt/stop/detach target states, none of which a
                // freshly constructed job is ever in).
                nestedJob.performSlice();

                // b. One grounded, cloned template instance per solution,
                // in solution order.
                ArrayTerm* pResultArray = new ArrayTerm();
                std::list<UnifyContext*>::const_iterator
                    itSol = nestedJob.m_listUnifySolutions.begin(),
                    itSolEnd = nestedJob.m_listUnifySolutions.end();
                for( ; itSol != itSolEnd; ++itSol ) {
                    AbstractTerm* pGrounded = resolveTermGrounded( pTmplTerm, *itSol, NULL );
                    pResultArray->append( pGrounded );
                }

                // c. nestedJob (and nestedGoal) go out of scope at the end
                // of this block; ~SolveJob() frees the nested job's entire
                // arena (UnifyContexts, GoalParts, SolveContexts). Nothing
                // above depends on it any more: pResultArray and every
                // element in it were built by resolveTermGrounded() as
                // entirely independent clones.

                // Unify the result array against `out`, exactly like
                // __builtin_eval's evaluated result: a fresh UnifyContext
                // (parented on the current chain) both receives new
                // bindings and scopes the array's own fresh (unbound-
                // template-var) VarTerms; `out`'s own scope is wherever
                // __builtin_findall(...) itself was written, i.e.
                // pUCOriginScope.
                UnifyContext* pUCCand = adoptUnifyContext( new UnifyContext(
                    sc->m_pUnifyContext,
                    sc->m_csCurrent,
                    ExecutionState::ClauseIterator() ) );
                AbstractTerm* pAdoptedArray = pUCCand->adoptTerm( pResultArray );

                UnifyResult unifyResult = pAdoptedArray->unifyTerm(
                    m_pEngine,
                    pUCCand,
                    pUCOriginScope,
                    pUCCand,
                    pOutTerm );

                if( UnifyError==unifyResult ) {
                    std::string strError = "Unification error binding findall() result against its output argument.";
                    VAULT_UNIFY_DI( ALWAYS, "Job %lld: %s\n", (long long) getId(), strError.c_str() );
                    recordError( strError );
                    unifyResult = UnifyNot;
                }

                // d. Exactly one solution, no choice point: never
                // re-examine this same term again, whichever way the
                // unify against `out` went -- mirrors cut's own iterator
                // invalidation above (same reasoning: without this, a
                // later revisit of sc would re-recognize and re-run this
                // very block).
                sc->m_itNextChildClause.invalidate();

                if( Unifies( unifyResult ) ) {
                    GoalPartCursor csNext( sc->m_csCurrent );
                    csNext.next();

                    SolveContext* scChild = new SolveContext(
                        m_pStartState,
                        sc,
                        pUCCand,
                        NULL,
                        csNext
                        );
                    m_stackContext.push_back( scChild );
                }
                // else: findall's own search always "succeeds" (possibly
                // with an empty array), but binding that array against
                // `out` failed here -- an ordinary goal failure. Nothing is
                // pushed; sc falls into the "no next child clause"
                // discardTop() path above the next time it is visited.

                continue;
            }
        }

        /*
         * ROADMAP Phase 2 (runtime assert/retract, SPEC.md): `assert(
         * fact(a, b) );` desugars (AnyTermFactory::operator()(const
         * ConsTermInput&), vault-unify-parser.cpp) to the reserved,
         * 1-arity goal term `__builtin_assert(fact(a, b))`, recognized here
         * directly -- exactly like cut/findall above -- rather than as a
         * registered Clause: assert needs the World (to append a new
         * clause to the root ExecutionState's clause list), which no
         * Clause::startUnification() implementation ever gets (its
         * signature only ever hands it two UnifyContexts and an Engine,
         * include/vault-unify.hpp).
         *
         * v1 scope (SPEC.md): FACTS only, and the argument must be fully
         * GROUND (no unbound variables) once resolved against the current
         * bindings -- asserting a rule, or a fact whose argument still
         * contains an unbound variable after resolution, is a v1
         * limitation reported as a (recorded, job-error-count-raising)
         * failure, not silently accepted.
         *
         * Mechanism: resolveTermGrounded() (vault-unify-terms.cpp) -- the
         * same helper findall uses for its own per-solution template clone
         * above -- resolves the argument against the CURRENT live chain:
         * sc->m_pUnifyContext carries every binding made so far along this
         * depth-first path (exactly the "search root" every ordinary
         * candidate unification is handed too -- e.g. the pUCCand
         * StandardClause::startUnification() receives is always
         * constructed with sc->m_pUnifyContext as ITS OWN parent, see the
         * ordinary clause-candidate code below), and pUCOriginScope scopes
         * the argument term's OWN variables (mirroring findall's pOutTerm
         * scoping just above). Every node resolveTermGrounded() returns is
         * a fresh allocation (bound VarTerms resolved recursively, unbound
         * ones cloned fresh) -- entirely independent of, and never aliased
         * into, the existing clause database or any live query/clause term
         * tree, so it is always safe either to free it (deleteTermTree(),
         * on the v1-limitation paths below) or to hand it over to a brand
         * new StandardClause (the success path).
         *
         * On success the grounded clone (required to be a ConsTerm --
         * Clause's head is always one, include/vault-unify.hpp) becomes a
         * new StandardClause's head with NO body (a fact -- mirrors
         * PrologParser::Context::createClause()'s own `new
         * StandardClause(head, NULL)` for an ordinary parsed fact,
         * vault-unify-parser.cpp), appended via
         * ExecutionState::appendClause() to the END of the World's root
         * ExecutionState's m_listClauses -- assertz/definition-order
         * semantics (SPEC.md), and the EXACT list both ClauseIterator and
         * `__builtin_retract` below walk. From this point on, the new
         * clause's term tree is owned exactly like any parser-built fact:
         * ExecutionState::collectAllTermTrees()/~ExecutionState() (driven
         * by World::~World()'s de-duplicated sweep) frees it once, with no
         * extra bookkeeping -- appendClause() does not distinguish a
         * solve-time clause from a parse-time one.
         *
         * Like findall, this is deterministic (exactly one outcome, no
         * choice point) and never produces a continuation goal of its own.
         */
        {
            const ConsTerm* pAssertCandidate = dynamic_cast<const ConsTerm*>( sc->m_csCurrent.getAbstractTerm() );
            if( pAssertCandidate && 1==pAssertCandidate->getArity()
                    && pAssertCandidate->getName().value() == "__builtin_assert" ) {

                const AbstractTerm* pArgTerm = pAssertCandidate->getTermAt( 0 );
                UnifyContext* pUCOriginScope = sc->m_csCurrent.getOriginUnifyContext();

                AbstractTerm* pGrounded = resolveTermGrounded(
                    pArgTerm, sc->m_pUnifyContext, pUCOriginScope );

                UnifyResult unifyResult;

                if( termTreeHasUnboundVar( pGrounded ) ) {
                    std::string strError = "assert(): argument '";
                    strError += pArgTerm->toString();
                    strError += "' is not ground (contains unbound variable(s)); "
                        "v1 only supports asserting ground facts.";
                    VAULT_UNIFY_DI( ALWAYS, "Job %lld: %s\n", (long long) getId(), strError.c_str() );
                    recordError( strError );
                    deleteTermTree( pGrounded );
                    unifyResult = UnifyNot;
                } else if( ConsTerm* pFactHead = dynamic_cast<ConsTerm*>( pGrounded ) ) {
                    StandardClause* pNewClause = new StandardClause( pFactHead, NULL );
                    // Engine item E1: created at run time, so no source
                    // file and no module -- an image writes these back as
                    // canonical facts (E6), not as text it never had.
                    ClauseOrigin originAsserted;
                    originAsserted.kind = ClauseOrigin::ASSERTED;
                    m_spWorld->getRootState()->appendClause(
                        m_spWorld, pNewClause, originAsserted );
                    unifyResult = UnifyLast;
                } else {
                    std::string strError = "assert(): argument '";
                    strError += pArgTerm->toString();
                    strError += "' does not resolve to a named term and cannot become a fact.";
                    VAULT_UNIFY_DI( ALWAYS, "Job %lld: %s\n", (long long) getId(), strError.c_str() );
                    recordError( strError );
                    deleteTermTree( pGrounded );
                    unifyResult = UnifyNot;
                }

                // Exactly one outcome, no choice point -- mirrors cut's/
                // findall's own iterator invalidation above: without this,
                // a later revisit of sc would re-recognize and re-run this
                // very block.
                sc->m_itNextChildClause.invalidate();

                if( Unifies( unifyResult ) ) {
                    GoalPartCursor csNext( sc->m_csCurrent );
                    csNext.next();

                    SolveContext* scChild = new SolveContext(
                        m_pStartState,
                        sc,
                        sc->m_pUnifyContext,    // assert binds nothing new.
                        NULL,
                        csNext
                        );
                    m_stackContext.push_back( scChild );
                }
                // else: the ground/head-shape check failed above (error
                // already recorded); nothing pushed -- sc falls into the
                // "no next child clause" discardTop() path the next time
                // it is visited, exactly like an ordinary failed goal.

                continue;
            }
        }

        /*
         * ROADMAP Phase 2 (runtime assert/retract, SPEC.md): `retract(
         * fact(a, $x) );` desugars to the reserved, 1-arity goal term
         * `__builtin_retract(fact(a, $x))`, recognized here directly for
         * the same reason as assert above: retract needs to scan AND
         * mutate the World's clause database directly, which no Clause
         * ever gets access to.
         *
         * v1 scope (SPEC.md): only STANDARD-CLAUSE FACTS are retractable --
         * builtins (SimpleBuiltinClause) and rules with a non-empty body
         * are skipped while scanning, never matched against. Finds the
         * FIRST such clause (definition/assertz order -- the same
         * m_listClauses order ExecutionState::ClauseIterator walks) whose
         * head unifies with the argument; on a match, the clause is
         * tombstoned (Clause::retire(), include/vault-unify.hpp -- see its
         * comment for why this, rather than erasing it from
         * m_listClauses, is the safe choice given how
         * ExecutionState::ClauseIterator/UnifyContext::m_itClause/
         * SolveContext::m_itNextChildClause hold plain std::list iterator
         * copies that could otherwise be positioned on the very node being
         * removed) and retract succeeds once (no choice point -- the FIRST
         * match only, per SPEC.md). No match at all is an ordinary,
         * silent UnifyNot -- exactly like a goal with no matching clauses.
         *
         * The trial unification is a throwaway, "check-and-remove" step,
         * NOT a binding goal (SPEC.md): the argument's own vars are
         * resolved through pUCOriginScope/sc->m_pUnifyContext (the real,
         * live chain -- so an already-bound variable used inside the
         * retract argument, e.g. `retract(fact(a, $y))` after $y was bound
         * earlier in the very same clause body, is correctly read), but
         * every NEW binding this trial unification would create is written
         * only into a fresh, throwaway UnifyContext scoped to this trial
         * alone (bindVarBinding()/bindVarInstance(), vault-unify-
         * unifycontext.cpp, always mutate the UnifyContext they are
         * called ON -- never a parent's own maps), local to this block and
         * destroyed the moment it ends -- so nothing the trial binds ever
         * escapes into the enclosing search, matching UnifyBuiltinClause's
         * own minimal direct-unifyTerm() pattern (vault-unify-clause-
         * builtin-unify.cpp) rather than going through the normal
         * candidate-unification machinery (SolveJob::startUnification()),
         * which always adopts pUCCand into the job's own long-lived arena.
         */
        {
            const ConsTerm* pRetractCandidate = dynamic_cast<const ConsTerm*>( sc->m_csCurrent.getAbstractTerm() );
            if( pRetractCandidate && 1==pRetractCandidate->getArity()
                    && pRetractCandidate->getName().value() == "__builtin_retract" ) {

                const AbstractTerm* pArgTerm = pRetractCandidate->getTermAt( 0 );
                UnifyContext* pUCOriginScope = sc->m_csCurrent.getOriginUnifyContext();

                bool foundMatch = false;
                {
                    ExecutionState* pRootState = m_spWorld->getRootState();
                    std::list<Clause*>::const_iterator
                        itCand = pRootState->m_listClauses.begin(),
                        itCandEnd = pRootState->m_listClauses.end();
                    for( ; !foundMatch && itCand != itCandEnd; ++itCand ) {
                        Clause* pCand = *itCand;
                        if( pCand->isRetired() ) {
                            continue;
                        }
                        // Only standard-clause FACTS (no body) are
                        // retractable in v1 -- skips builtins
                        // (SimpleBuiltinClause) and rules alike.
                        StandardClause* pStdCand = dynamic_cast<StandardClause*>( pCand );
                        if( !pStdCand || !pStdCand->isTerminal() ) {
                            continue;
                        }

                        const ConsTerm* pCandHead = pStdCand->leftHandTerm();

                        // Throwaway trial context: parented on the real,
                        // live chain (sc->m_pUnifyContext) purely so reads
                        // (findVarBinding()'s parent walk) see every
                        // binding made so far -- every WRITE this trial
                        // performs lands in ucScratch itself, discarded
                        // when this block ends (see the comment above).
                        UnifyContext ucScratch(
                            sc->m_pUnifyContext,
                            GoalPartCursor(),
                            ExecutionState::ClauseIterator() );

                        UnifyResult trialResult = pArgTerm->unifyTerm(
                            m_pEngine,
                            &ucScratch,      // pUCStackTop: the trial's own binding sink.
                            &ucScratch,      // pUCOther: scope for pCandHead's own vars.
                            pUCOriginScope,  // pUCMine: scope for pArgTerm's own vars.
                            pCandHead );

                        if( Unifies( trialResult ) ) {
                            // ROADMAP Phase 2 (runtime assert/retract,
                            // SPEC.md -- "logical update view"): stamp the
                            // generation this retire() itself creates,
                            // under the same lock appendClause() uses (see
                            // World::clauseDbMutex()'s comment) -- this is
                            // the other of the two call sites that mutate
                            // the shared generation counter/clause
                            // liveness state.
                            Guard g( m_spWorld->clauseDbMutex() );
                            pCand->retire( m_spWorld->bumpGeneration() );
                            // Engine item E2: keep the catalogue in step
                            // with the tombstone, inside the same critical
                            // section, so no reader can observe a clause
                            // count that disagrees with the clause list.
                            m_spWorld->catalogueRetire( pCand );
                            foundMatch = true;
                        } else if( UnifyError==trialResult ) {
                            // Defensive: no plain ConsTerm/VarTerm/MapTerm/
                            // ArrayTerm unification actually returns this
                            // today (see SPEC.md), but treat it like any
                            // other unification error rather than silently
                            // matching or crashing.
                            std::string strError = "retract(): unification error trying candidate '";
                            strError += pCandHead->toString();
                            strError += "' against argument '";
                            strError += pArgTerm->toString();
                            strError += "'.";
                            VAULT_UNIFY_DI( ALWAYS, "Job %lld: %s\n", (long long) getId(), strError.c_str() );
                            recordError( strError );
                        }
                        // ucScratch destructs here -- its m_lsAdoptedTerms
                        // is always empty (a plain unifyTerm() call on
                        // ConsTerm/VarTerm/MapTerm/ArrayTerm never adopts a
                        // freshly allocated term the way ArithEvalBuiltinClause/
                        // findall do), so there is nothing to free beyond
                        // the (stack-allocated) UnifyContext object itself.
                    }
                }

                // Exactly one outcome, no choice point -- see assert's own
                // comment above for why.
                sc->m_itNextChildClause.invalidate();

                if( foundMatch ) {
                    GoalPartCursor csNext( sc->m_csCurrent );
                    csNext.next();

                    SolveContext* scChild = new SolveContext(
                        m_pStartState,
                        sc,
                        sc->m_pUnifyContext,    // retract binds nothing new.
                        NULL,
                        csNext
                        );
                    m_stackContext.push_back( scChild );
                }
                // else: no matching clause found -- an ordinary, silent
                // UnifyNot; nothing pushed, sc falls into the "no next
                // child clause" discardTop() path the next time it is
                // visited, exactly like a goal with no matching clauses.

                continue;
            }
        }

        const Clause* cl = sc->m_itNextChildClause.getClause();

        VAULT_UNIFY_DI( ITERATE, "Testing child clause '%s' within unify context %lld.\n"
            , cl->toString().c_str()
            , (long long) sc->m_pUnifyContext->getUnifyContextId() );

        /*
         * We will now begin to unify the next record. 
         * So create a unify context that holds the possible results
         * of the unification.
         * If it contains new goals to solve, another solve context might
         * be created to iterate through it.
         */

        UnifyContext* pUCCand = adoptUnifyContext( new UnifyContext(
            sc->m_pUnifyContext,
            sc->m_csCurrent,
            sc->m_itNextChildClause ) );

        /*
         * Begin unification of the current goal term with the current
         * unification candidate.
         * 
         * Once, the unification has been triggered, it may have:
         * - atomically finished (with or without success)
         * - result still pending.
         * - yielded a current result, albeit no terminal result, still pending.
         * 
         * Also, it may have created some bindings along the way.
         *
         * In summary, startUnification generates a {pending; terminated} x {success; failure }
         * tuple.
         */
        vault::unify::Clause::UnificationState unificationState = startUnification( pUCCand, sc->m_pCCC );
        
        if( !pUCCand->isUnificationDone() ) {
            // VAULT_UNIFY_DI( ALWAYS, "Warning: Unification not done.\n" );
            // TXWTODO: Support discontinuous clauses.
        }

        UnifyResult unifyResult = pUCCand->getUnificationResult();

        // Did we have an internal error during the unification process?
        if( unificationState<0 || UnifyError==unifyResult ) {
            /*
             * Phase 1 (ROADMAP): make unification errors visible instead of
             * silently mapping them to "did not unify". Report the error
             * (job id, the clause we were trying, and the goal term we
             * were trying to unify it with) and record it on the job so
             * callers can query it afterwards.
             *
             * We deliberately do NOT abort the whole search here: aborting
             * on one bad candidate would change search semantics. Instead,
             * after recording+reporting, we treat this one candidate as
             * not-unified and continue trying the remaining candidates,
             * same as before.
             */
            std::string strError = "Unification error trying clause '";
            strError += cl->toString();
            strError += "' against goal '";
            strError += sc->m_csCurrent.getAbstractTerm()->toString();
            strError += "'.";

            VAULT_UNIFY_DI( ALWAYS, "Job %lld: %s\n",
                (long long) getId(), strError.c_str() );

            recordError( strError );

            unifyResult = UnifyNot;
        }

        /*
         * Now we have three regular options: not unified, this was the last solution, 
         * or this was a solution and another one is likely to follow up.
         */
        if( UnifyNot==unifyResult ) {

            /*
             * We have not been able to unify the current candidate.
             * we need to try with the next one on the current term
             * in the goalpart chain.
             */
             
            VAULT_UNIFY_DI( ITERATE, "Does not unify.\n" );

            // pUCCand is arena-owned (adoptUnifyContext() above); it is
            // freed with the rest of the job's arena in ~SolveJob(), not
            // deleted here.

            // ... and try the next child clause.

        } else if( UnifyLast==unifyResult || UnifyNotLast==unifyResult ) {

            /*
             * At this point, we have a valid unification, and it is the last one.
             */
            
            VAULT_UNIFY_DI( ITERATE, "Unifies, UnifyContext is '%s'.\n",
                pUCCand->toString().c_str() );
            
            /*
             * We only have a new goal part, if the clause had a right hand side.
             */
            SolveContext* scChild = NULL;
            
            bool haveNewGoal;
            if( cl->isTerminal() ) {
                haveNewGoal = false;
            } else {
                if( pUCCand->m_pGoal ) {
                    haveNewGoal = true;
                } else {
                    haveNewGoal = false;
                }
            }
        
            if( !haveNewGoal ) {

                /*
                 * We successfully finished a unification with an empty right hand side.
                 * That means that we can iterate to the next element of the current
                 * Goal Part chain.
                 */

                /*
                 * Advance in goal part is done at the beginning of the next
                 * iteration.
                 * If there is no next element in the current goal part chain, we can emit a solution.
                 */

                GoalPartCursor csNext( sc->m_csCurrent );
                csNext.next();

                VAULT_UNIFY_DI( ITERATE, "sc->m_csCurrent now points to %s.\n",
                    sc->m_csCurrent.getAbstractTerm()->toString().c_str() );
    
                /*
                 * Push the context to continue depth search with the next clause 
                 * to investigate.
                 * We even might push for an empty clause.
                 */
                scChild = new SolveContext(
                    m_pStartState,       // ExecutionState
                    sc,                  // SolveContext
                    pUCCand,             // Parent unify context (req'd?)
                    NULL,                // new GoalPart to use.
                    csNext               // where to iterate.
                    );

                VAULT_UNIFY_DI( ITERATE, "Pushing new solve context for next term.\n" );
                
            } else {

                /*
                 * We have done a unification. Unification created a new goal that
                 * substitutes the current term and thus creates a new GoalPart chain.
                 */
                VAULT_UNIFY_DI( ITERATE, "Clause is not terminal. Trying to unify.\n" );

                GoalPart* newGoalPart = NULL;
                Goal::GoalIterator itNext( sc->m_csCurrent.getGoalIterator() );
                itNext.next();
                newGoalPart = adoptGoalPart( new GoalPart(
                    pUCCand->m_pGoal,
                    sc->m_csCurrent.getGoalPart(),
                    pUCCand, // The unify context we origin in.
                    itNext // might already be invalid.
                    ) );
                scChild = new SolveContext(
                    m_pStartState,      // ExecutionState
                    sc,                 // SolveContext
                    pUCCand,            // Parent unify context (req'd?)
                    newGoalPart,        // new GoalPart to use.
                    GoalPartCursor( newGoalPart ) // point of iteration. 
                    );

                VAULT_UNIFY_DI( ITERATE, "Pushing new solve context for term %s.\n",
                    pUCCand->m_pGoal->toString().c_str() );

            } /* else !haveNewGoal */

            m_stackContext.push_back( scChild );

        } /* if UnifyLast || UnifyNotLast */

        /*
         * In the parent context, continue with the next clause or with the next result
         * of the same clause.
         */
        if( NULL==sc->m_pCCC ) {
            sc->m_itNextChildClause.next();
        } else {
            // Same clause, next continuation context call.
            VAULT_UNIFY_DI( ITERATE, "Warning: Clause continuation non-NULL.\n" );
        }

    }

    if( m_stackContext.empty() ) {
        // Nothing more to do. If we are not finished, set to finished.
        state( Job::FINISHED );
    }
    return 0;
}


int SolveJob::setGoal( const Goal* pGoal )
{
    m_pGoal = pGoal;
    return 0;
}


void SolveJob::adoptGoal( const Goal* pGoal )
{
    m_arenaGoals.push_back( pGoal );
}


SolveJob& SolveJob::setWorld( WorldPtr spWorld )
{
    m_spWorld = spWorld;
    m_pStartState = spWorld->getRootState();
    return *this;
}


/**
 * Release all data structures associated with this job object. 
 */
int SolveJob::triggerRelease()
{
    return 0;
}


SolveJob::~SolveJob()
{
    VAULT_UNIFY_DI( ALWAYS, "Destroying job %lld.\n", (long long) getId() );

    /*
     * ROADMAP Phase 1 ("Ownership model"): release everything this job
     * owns, in an order that is safe regardless of whether the job ran to
     * completion or was aborted mid-search.
     *
     * 1) SolveContexts still left on m_stackContext (job aborted
     *    mid-search, e.g. debugger STOP/DETACH in performSlice() sets
     *    Job::FINISHED without draining the stack via discardTop()).
     *    SolveContexts are NOT arena-owned - they only reference arena
     *    objects (m_pUnifyContext, m_pMyGoalPart) - and ~SolveContext() is
     *    empty (does not touch either), so it does not matter whether we
     *    free these before or after the arenas below.
     *
     * 2) The GoalPart arena, then the UnifyContext arena. Both GoalPart
     *    and UnifyContext use compiler-generated destructors that only
     *    tear down their own members (maps, GoalPartCursor,
     *    ExecutionState::ClauseIterator, ...); neither one deletes the
     *    peers it merely points to (parent goal part, parent/origin unify
     *    context, ...), so freeing them in either order is safe. We free
     *    GoalParts first since a GoalPart's m_pOriginUnifyContext points
     *    at the UnifyContext that created it.
     *
     *    m_listUnifySolutions is a list of pointers into
     *    m_arenaUnifyContexts (populated by emitSolution()); it does not
     *    own them separately and must never be walked here, or those
     *    UnifyContexts would be double-freed.
     *
     * 3) The Goal(s) adopted via adoptGoal(), AND NOW their TERM trees too
     *    (every term in each Goal's m_listAbstractTerms, deleted via
     *    collectTermTree() into one de-duplicated set per Goal before the
     *    Goal object itself is deleted -- a single query Goal can still
     *    have its own internal aliasing, e.g. a repeated variable used
     *    twice, so a per-term deleteTermTree() would risk a double free;
     *    see the ownership note on collectTermTree()/deleteTermTree() in
     *    include/vault-unify.hpp).
     *
     *    This used to be unsafe: if the query contained an
     *    `if( cond ) { ... }` statement, the old desugaring in
     *    AnyTermFactory::operator()(IfStatementInput) (vault-unify-parser.cpp)
     *    aliased a VarTerm between the synthesized clause it appended
     *    straight into World's (permanent) root ExecutionState and the
     *    call-site term left behind in this query's own Goal -- freeing it
     *    here would have left World's later, de-duplicated clause-database
     *    cleanup (World::~World()) to delete an already-freed pointer. That
     *    desugaring now clones cond/body into fresh variables private to
     *    each synthesized clause (see cloneTermTree(), declared next to
     *    collectTermTree()/deleteTermTree()), so a query's Goal term trees
     *    no longer share anything with World's clause database and can
     *    safely be freed here, independently of World's own lifetime. See
     *    test/conformance/if-statement.ufy.
     *
     *    getSolutionList() (called from onFinished(), before a job is ever
     *    destroyed) has already turned every solution into plain strings
     *    (VarTermId -> std::string) by this point, so nothing outside this
     *    job still needs these term trees once we get here. Solution
     *    UnifyContexts (freed in step 2 above) hold SingleVarInstance
     *    pointers into these same terms, but SingleVarInstance has no
     *    custom destructor and nothing else in this class dereferences a
     *    term, so freeing terms after (or before) the UnifyContext arena
     *    is equally safe.
     */
    while( !m_stackContext.empty() ) {
        SolveContext* sc = m_stackContext.back();
        m_stackContext.pop_back();
        delete sc;
    }

    std::vector<GoalPart*>::iterator itGP, itGPEnd = m_arenaGoalParts.end();
    for( itGP = m_arenaGoalParts.begin(); itGP != itGPEnd; ++itGP ) {
        delete *itGP;
    }
    m_arenaGoalParts.clear();

    std::vector<UnifyContext*>::iterator itUC, itUCEnd = m_arenaUnifyContexts.end();
    for( itUC = m_arenaUnifyContexts.begin(); itUC != itUCEnd; ++itUC ) {
        delete *itUC;
    }
    m_arenaUnifyContexts.clear();

    std::vector<const Goal*>::iterator itG, itGEnd = m_arenaGoals.end();
    for( itG = m_arenaGoals.begin(); itG != itGEnd; ++itG ) {
        const Goal* pGoal = *itG;
        std::set<const AbstractTerm*> visitedTerms;
        std::list<const AbstractTerm*>::const_iterator
            itTerm = pGoal->m_listAbstractTerms.begin(),
            itTermEnd = pGoal->m_listAbstractTerms.end();
        for( ; itTerm != itTermEnd; ++itTerm ) {
            collectTermTree( *itTerm, visitedTerms );
        }
        std::set<const AbstractTerm*>::const_iterator itV, itVEnd = visitedTerms.end();
        for( itV = visitedTerms.begin(); itV != itVEnd; ++itV ) {
            delete *itV;
        }
        delete pGoal;
    }
    m_arenaGoals.clear();
}


SolveJob::SolveJob()
        : m_pGoal( NULL )
        , m_pStartState( NULL )
        , m_sliceCount( 0 )
        , m_errorCount( 0 )
{
}


/**
 * Record a unification error (Phase 1: make UnifyError visible instead of
 * silently treating it as "did not unify"). Increments the error count and
 * remembers the message so it can be retrieved via getLastError().
 */
void SolveJob::recordError( const std::string& strError )
{
    ++m_errorCount;
    m_lastError = strError;

    // Engine item E10: also record it as data, and hand it to the engine's
    // diagnostic sink so a front end sees runtime errors on the same path
    // as parse errors rather than having to poll getLastError() and hope it
    // has not been overwritten.
    //
    // A runtime error has no column and usually no source line -- it is a
    // goal that failed, not text that would not parse -- so the default
    // sink prints its one-line form. The file/line come from the job's
    // debug location when there is one.
    Diagnostic diagnostic;
    diagnostic.severity = Diagnostic::ERROR;
    diagnostic.message = strError;
    diagnostic.column = 0;

    DebugLocation debugLocation = getDebugLocation();
    diagnostic.uriFile = debugLocation.uriFile.empty()
        ? std::string( "<goal>" )
        : debugLocation.uriFile;
    diagnostic.line = debugLocation.line;

    m_lsDiagnostics.push_back( diagnostic );

    if( m_pEngine ) {
        // DIAGNOSTIC_SILENT: before E10 this path printed nothing at all,
        // and unify-run's output must not move. An installed sink still
        // receives it -- see Engine::DiagnosticDefault.
        m_pEngine->writeDiagnostic( diagnostic, Engine::DIAGNOSTIC_SILENT );
    }
}


};
};


