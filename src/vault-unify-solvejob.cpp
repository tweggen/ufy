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

namespace vault {
namespace unify {


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

    GoalPart* pGoalPart = new GoalPart( 
        m_pGoal,                // Externally provided goal
        NULL,                   // no parent Goal Part
        0,                      // No associated unifycontextid (no vars contained)
        vault::unify::Goal::GoalIterator() );
    // pEngine->logChange() << *pGoalPart;

    UnifyContext* pRootUnifyContext = new UnifyContext(
        NULL,                   // no parent unify context
        GoalPartCursor(),
        ExecutionState::ClauseIterator() );
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


        if( !sc->m_itNextChildClause.isValid() ) {            
            VAULT_UNIFY_DI( ITERATE, "No next child clause to test.\n" );
            
            /*
             * When there is no next child clause to test, we finished
             * one depth-first branch. So pop out again.
             */
            discardTop( sc );
            continue;
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

        UnifyContext* pUCCand = new UnifyContext( 
            sc->m_pUnifyContext,
            sc->m_csCurrent,
            sc->m_itNextChildClause );

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
        if( unificationState<0 ) {
            // What to do? Act, as if it is not unified.
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

            // delete candidate again.
            delete pUCCand;

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
                newGoalPart = new GoalPart(
                    pUCCand->m_pGoal,
                    sc->m_csCurrent.getGoalPart(),
                    pUCCand, // The unify context we origin in.
                    itNext // might already be invalid.
                    );
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
}


SolveJob::SolveJob()
        : m_pGoal( NULL )
        , m_pStartState( NULL )
        , m_sliceCount( 0 )
{
}


};
};


