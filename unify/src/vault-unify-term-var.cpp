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

namespace vault {
namespace unify {


const std::string VarTerm::toContextString( 
    const UnifyContext* pUCStackTop,
    const UnifyContext* pUCTerm ) const
{
    UnifyContextId uidOrg;
    if( pUCTerm ) {
        uidOrg = pUCTerm->getUnifyContextId();
    } else {
        uidOrg = 0;
    }
    AssignmentId aid( uidOrg, m_uidTerm );
    
    VAULT_UNIFY_DI( UNIFY, "VT%lld: toContextString() called for stacktop %lld, term %lld.\n" 
        , (long long) m_uidTerm
        , (long long) pUCStackTop->getUnifyContextId()
        , (long long) pUCTerm?pUCTerm->getUnifyContextId():0ll
        );
    
    boost::shared_ptr<SingleVarInstance> spInstance;
    InstanceId iid = 0;
    (void) pUCStackTop->findVarBinding( aid, iid );
    if( iid ) {
        (void) pUCStackTop->findVarInstance( iid, spInstance );
    }
    if( spInstance ) {
        VAULT_UNIFY_DI( UNIFY, "VT%lld: Found binding to instance.\n", 
            (long long) m_uidTerm );
        const AbstractTerm* pInstanceTerm = spInstance->getTerm();
        UnifyContext* pUCInstanceTerm = spInstance->getUnifyContext();
        if( pInstanceTerm ) {
            // Correct?
            return pInstanceTerm->toContextString( 
                pUCStackTop, pUCInstanceTerm );
        } else {
            // Not instantiated yet.
            return toString();
        }
    } else {
        return toString();
    }
}


const std::string VarTerm::toJSON( 
    bool useContext,
    const UnifyContext* pUCStackTop,
    const UnifyContext* pUCTerm ) const
{
    if( useContext ) {
        UnifyContextId uidOrg;
        if( pUCTerm ) {
            uidOrg = pUCTerm->getUnifyContextId();
        } else {
            uidOrg = 0;
        }
        AssignmentId aid( uidOrg, m_uidTerm );
        
        VAULT_UNIFY_DI( UNIFY, "VT%lld: toContextString() called for stacktop %lld, term %lld.\n" 
            , (long long) m_uidTerm
            , (long long) pUCStackTop->getUnifyContextId()
            , (long long) pUCTerm?pUCTerm->getUnifyContextId():0ll
            );
        
        boost::shared_ptr<SingleVarInstance> spInstance;
        InstanceId iid = 0;
        (void) pUCStackTop->findVarBinding( aid, iid );
        if( iid ) {
            (void) pUCStackTop->findVarInstance( iid, spInstance );
        }
        if( spInstance ) {
            VAULT_UNIFY_DI( UNIFY, "VT%lld: Found binding to instance.\n", 
                (long long) m_uidTerm );
            const AbstractTerm* pInstanceTerm = spInstance->getTerm();
            UnifyContext* pUCInstanceTerm = spInstance->getUnifyContext();
            if( pInstanceTerm ) {
                // Correct?
                return pInstanceTerm->toJSON( 
                    true, pUCStackTop, pUCInstanceTerm );
            } else {
                // Not instantiated yet.
                return toJSON( false, NULL, NULL );
            }
        } else {
            return toJSON( false, NULL, NULL );
        }
    } else {
        char s[50];
        if( m_originalVarName.length() ) {
            const char* pName = NULL;
            pName = m_originalVarName.c_str();
            snprintf( s, 50, "\"%s\"", pName );
        } else {
            snprintf( s, 50, "\"VT%lld\"", (long long) m_uidTerm );
        }
        return std::string( s );
    }
}


std::atomic<VarTermId> VarTerm::m_counterUidTerm( 1 );

// 2nd order call: this is clause, other is goal.
UnifyResult VarTerm::unifyConsTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOther,
        UnifyContext* pUCMine,
        const ConsTerm* pOther ) const
{
    // Sort parameters and forward to generic version.
    return pUCStackTop->genericUnifyVarWithKnown(
        pEngine,
        pUCStackTop,
        pUCMine,
        this,
        pUCOther,       // Unify to scope the variable
        pOther          // non-var term
        );
}


UnifyResult VarTerm::unifyMapTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOther,
        UnifyContext* pUCMine,
        const MapTerm* pOther ) const
{
    // Sort parameters and forward to generic version.
    return pUCStackTop->genericUnifyVarWithKnown(
        pEngine,
        pUCStackTop,
        pUCMine,
        this,
        pUCOther,       // Unify to scope the variable
        pOther          // non-var term, map here.
        );
}


UnifyResult VarTerm::unifyArrayTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOther,
        UnifyContext* pUCMine,
        const ArrayTerm* pOther ) const
{
    // Sort parameters and forward to generic version.
    return pUCStackTop->genericUnifyVarWithKnown(
        pEngine,
        pUCStackTop,
        pUCMine,
        this,
        pUCOther,       // Unify to scope the variable
        pOther          // non-var term, array here.
        );
}


// this is from the goal, other is from the clause.
UnifyResult VarTerm::unifyTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOther,
        UnifyContext* pUCMine,
        const AbstractTerm* pOther ) const
{
    return pOther->unifyVarTerm( 
        pEngine,
        pUCStackTop, 
        pUCMine,
        pUCOther,
        this );
}


// This is from the clause, other is from the goal.
UnifyResult VarTerm::unifyVarTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOther,
        UnifyContext* pUCMine,
        const VarTerm* pOther ) const
{
    /*
     * I shall unify with another variable from another context.
     * If noone has an instance, bind both in my context.
     * If the exactly one of us has an instance, create a binding in my 
     * context for the other one.
     * If neither has a instance, create an empty instance for both.
     */
    VAULT_UNIFY_DI( UNIFY, "VarTerm: Asked to unify %lld:%s with %lld:%s into %lld.\n",
        pUCMine?pUCMine->getUnifyContextId():0ll,
        this->toString().c_str(),
        pUCOther?pUCOther->getUnifyContextId():0ll,
        pOther->toString().c_str(),
        pUCStackTop?pUCStackTop->getUnifyContextId():0ll );

    // Resolve the vars.

    UnifyContextId uidMine;
    if( pUCMine ) {
        uidMine = pUCMine->getUnifyContextId();
    } else {
        uidMine = 0;
    }
    UnifyContextId uidOther;
    if( pUCOther ) {
        uidOther = pUCOther->getUnifyContextId();
    } else {
        uidOther = 0;
    }

    /*
     * I am asked to unify with myself (the very same VarTerm* C++ object on
     * both sides) -- but that is only a genuine no-op, requiring no binding
     * at all, if the two SCOPES also coincide. A clause's head and body
     * share one ClauseContext, so a repeated variable name (e.g. an
     * argument threaded unchanged through the clause's own recursive
     * self-call, SPEC.md section 10) resolves to one shared VarTerm* --
     * but the head is unified into a BRAND NEW activation's scope
     * (pUCMine/pUCOther here) while the very same pointer, on the other
     * side, still belongs to the CALLING activation's scope. Taking the
     * old scope-blind fast path in that case would report success while
     * recording no AssignmentId binding whatsoever, silently losing the
     * value: the new activation's copy of the variable would read back as
     * unbound. So: same object + same scope -> trivially unifies (and the
     * general logic below would reach exactly the same conclusion, via
     * aidMine==aidOther, if allowed to run -- this is purely a shortcut).
     * Same object + DIFFERENT scope falls through into the ordinary
     * var-var alias logic below, exactly as if two textually distinct
     * variables were being unified, so a proper cross-scope alias gets
     * recorded.
     */
    if( this == pOther && uidMine == uidOther ) return UnifyLast;

    boost::shared_ptr<SingleVarInstance> spMyInstance;
    AssignmentId aidMine( uidMine, m_uidTerm );
    InstanceId iidMine = 0;
    (void) pUCStackTop->findVarBinding( aidMine, iidMine );
    if( iidMine ) {
        (void) pUCStackTop->findVarInstance( iidMine, spMyInstance );
    }
    /*
     * Now, spMyInstance may contain a reference to my binding, if I have any.
     */

    boost::shared_ptr<SingleVarInstance> spOtherInstance;
    AssignmentId aidOther( uidOther, pOther->getBinding() );
    InstanceId iidOther = 0;
    (void) pUCStackTop->findVarBinding( aidOther, iidOther );
    if( iidOther ) {
        (void) pUCStackTop->findVarInstance( iidOther, spOtherInstance );
    }
    /*
     * Now, spOtherInstance may contain a reference to the other binding, if I have any.
     */
    VAULT_UNIFY_DI( UNIFY, "iidMine==%lld, iidOther==%lld.\n" 
            , (long long) iidMine
            , (long long) iidOther );

    /* 
     * First, test the instances.
     */
    if( !iidMine ) {
        if( !iidOther ) {
            /*
             * No iids. Create one and assign to both.
             * This unifies.
             */
            InstanceId iid = pUCStackTop->createIid();
            pUCStackTop->bindVarBindingUsing( aidMine, iid );
            pUCStackTop->bindVarBindingUsing( aidOther, iid );
            return UnifyLast;
        } else /* !iidOther */ {
            /*
             * Other has iid, i don't. Bind me to other.
             */
            pUCStackTop->bindVarBindingUsing( aidMine, iidOther );
            return UnifyLast;
        }
    } else /* !iidMine */ {
        if( !iidOther ) {
            /*
             * I have iid, other doesn't.
             */
            pUCStackTop->bindVarBindingUsing( aidOther, iidMine );
            return UnifyLast;
        } else /* !iidOther */ {
            /* 
             * Both have iids. So compare the content.
             */
            if( iidMine==iidOther ) {
                /*
                 * Trivial: same iids. We unify, regardless of the content.
                 */
                return UnifyLast;
            } else {
                /*
                 * Iids do not match. Care about content.
                 * This is the only case we do not return as unified.
                 * Implementation continued after the if() hierarchy.
                 */
            }
        }
    }

    /*
     * Both var terms have an instance id. Compare the contents.
     */
    if( !spMyInstance ) {
        if( !spOtherInstance ) {
            /*
             * Neither term has an instance created.
             * Both have iids. Override the other variable binding
             * with my iid (also could be other way round).
             */
            pUCStackTop->bindVarBindingUsing( aidOther, iidMine );
            // unifies.
            return UnifyLast;
        } else /* !spOtherInstance */ {
            /*
             * I don't have a binding, the other one has.
             * Attach ourselves to the other binding.
             */
            pUCStackTop->bindVarInstance( iidMine, spOtherInstance );
            // unifies.
            return UnifyLast;
        }
    } else /* !spMyInstance */ {
        if( !spOtherInstance ) {
            /*
             * I do have a binding, the other one does not have any.
             */
            pUCStackTop->bindVarInstance( iidOther, spMyInstance );
            // unifies.
            return UnifyLast;
        } else /* !spOtherInstance */ {
            if( spOtherInstance==spMyInstance ) {
                /*
                 * Same content? This unifies.
                 */
                return UnifyLast;
            }
            /*
             * Both do have a binding.
             */

            /*return spMyInstance->getTerm()->unifyTerm(
                pEngine,
                pUCStackTop,
                spOtherInstance->getUnifyContext(),
                spMyInstance->getUnifyContext(),
                spOtherInstance->getTerm() );*/
            return pUCStackTop->unifyTerms(
                pEngine,
                spMyInstance->getTerm(),
                spOtherInstance->getUnifyContext(),
                spMyInstance->getUnifyContext(),
                spOtherInstance->getTerm() );
        }
    }
    // never reached.

    return UnifyNot;
}


VarTerm::VarTerm()
{
    m_uidTerm = ++m_counterUidTerm;
}

};
};


