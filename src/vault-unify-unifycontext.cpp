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
    
InstanceId UnifyContext::m_iidLast = 1;


void SingleVarInstance::setValue(
    UnifyContext* pUCTerm,
    const AbstractTerm* pTerm )
{
    m_pUCTerm = pUCTerm;
    m_pTerm = pTerm;
}

    
SingleVarInstance::SingleVarInstance( 
    UnifyContext* pUCTerm,
    const AbstractTerm* pTerm )
        : m_pUCTerm( pUCTerm )
        , m_pTerm( pTerm )
{
    // nothing.
}


int UnifyContext::bindVarInstance(
    InstanceId iid,
    boost::shared_ptr<SingleVarInstance> spInstance )
{
    m_mapVarInstances[iid] = spInstance;
    return 0;
}


int UnifyContext::findVarInstance(
    InstanceId iid,
    boost::shared_ptr<SingleVarInstance>& out_spInstance ) const
{
    boost::shared_ptr<SingleVarInstance> spInstance;
    
    // First find the binding.
    const UnifyContext* uc = this;
    while( uc ) {
        // Binding declared locally?
        // VAULT_UNIFY_DI( UNIFY, "UC %lld: Asked to find a binding for InstanceId %lld.\n",
        //     (long long) m_uidUnifyContext,
        //     (long long) iid );

        std::map<InstanceId,
                boost::shared_ptr<SingleVarInstance> >::const_iterator it =
            uc->m_mapVarInstances.find( iid );
        if( it==uc->m_mapVarInstances.end() ) {
            // VAULT_UNIFY_DI( UNIFY, "UC %lld: Not found, considering parent 0x%08x.\n",
            //     (long long) uc->m_uidUnifyContext, (unsigned) uc->m_pParentUnifyContext );
            // Not declared locally. Look for variable in parent.
            uc = uc->m_pParentUnifyContext;
            continue;
        }
        // We have a binding.
        spInstance = it->second;
        break;
    }
    if( spInstance ) {
        // Already instanciated?
        out_spInstance = spInstance;
        return 1;
    } else {
        out_spInstance.reset();
        return 0;
    }
    // Never reached
}


int UnifyContext::bindVarBinding(
    AssignmentId aid,
    InstanceId& out_iid,
    InstanceId iidUser )
{
    if( 0==iidUser ) {
        iidUser = ++m_iidLast;
    }
    out_iid = iidUser;
    m_mapVarMappings[aid] = iidUser;    
    return 0;
}


int UnifyContext::findVarBinding(
    AssignmentId aid,
    InstanceId& out_iid ) const
{
    InstanceId iidRes = 0;
    
    // First find the binding.
    const UnifyContext* uc = this;
    if( uc ) {
        VAULT_UNIFY_DI( UNIFY, "UC %lld: Asked to find a binding for varterm %lld::VT%lld \"%s\"\n",
            (long long) m_uidUnifyContext,
            (long long) aid.getUnifyContextId(),
            (long long) aid.getVarTermId(),
            uc->toString().c_str() );
    }
    while( uc ) {
        // Binding declared locally?
        std::map<AssignmentId,InstanceId>::const_iterator it =
            uc->m_mapVarMappings.find( aid );
        if( it==uc->m_mapVarMappings.end() ) {
            // VAULT_UNIFY_DI( UNIFY, "UC %lld: Not found, considering parent 0x%08x.\n",
            //    (long long) uc->m_uidUnifyContext, (unsigned) uc->m_pParentUnifyContext );
            // Not declared locally. Look for variable in parent.
            uc = uc->m_pParentUnifyContext;
            continue;
        }
        // We have a binding.
        iidRes = it->second;
        break;
    }
    if( iidRes ) {
        // Already instanciated?
        VAULT_UNIFY_DI( UNIFY, "UC %lld: Found.\n", (long long) m_uidUnifyContext );
        out_iid = iidRes;
        return 1;
    } else {
        VAULT_UNIFY_DI( UNIFY, "UC %lld: Not found.\n", (long long) m_uidUnifyContext );
        out_iid= 0;
        return 0;
    }
    // Never reached
}


boost::shared_ptr<SingleVarInstance> UnifyContext::createVarInstance(
        UnifyContext* pUCTerm,
        const AbstractTerm* pTerm )
{
    boost::shared_ptr<SingleVarInstance> spInstance(
        new SingleVarInstance( pUCTerm, pTerm ) );
    return spInstance;
}


void UnifyContext::unificationDone(
    UnifyResult unificationResult,
    const Goal* pGoal )
{
    static const char* strUnifyResult[] = { "UnifyError", "UnifyNot", "UnifyLast", "UnifyNotLast" };

    if( unificationResult ) {
        VAULT_UNIFY_DI( UNIFY, "UC %lld: Unification done for %s, result %s, new goal %s.\n",
            (long long) getUnifyContextId(),
            strUnifyResult[((int)unificationResult)+1],
            m_csTermToUnify.getAbstractTerm()->toString().c_str(),
            pGoal?pGoal->toString().c_str():"(nil)" );
    } else {
        // VAULT_UNIFY_DI( UNIFY, "Unification NOT done for %s..\n",
        //    m_csTermToUnify.getAbstractTerm()->toString().c_str() );
    }
    m_isUnificationDone = true;
    m_unificationResult = unificationResult;
    m_pGoal = pGoal; 
}


/**
 * Unify a scoped variable with another (non-scoped) term.
 * pUCVarScope may or may not be this.
 */
UnifyResult UnifyContext::genericUnifyVarWithKnown(
    Engine* pEngine,
    UnifyContext* pUCStackTop,
    UnifyContext* pUCVarScope,
    const VarTerm* pVarTerm,
    UnifyContext* pUCOther,
    const AbstractTerm* pOtherTerm )
{
    VAULT_UNIFY_DI( UNIFY, "Called with ucStackTop==%lld, ucVarScope==%lld, "
        "pVarTerm=%s, ucOther==%lld, pOtherTerm=%s \n", 
        pUCStackTop?pUCStackTop->getUnifyContextId():0ll,
        pUCVarScope?pUCVarScope->getUnifyContextId():0ll,
        pVarTerm?pVarTerm->toString().c_str():"(no term)",
        pUCOther?pUCOther->getUnifyContextId():0ll,
        pOtherTerm?pOtherTerm->toString().c_str():"(no term)"
        );
    
    VAULT_UNIFY_DI( UNIFY, "VarTerm %lld::%lld into %s: Unify with with %s.\n", 
        (long long) pUCVarScope?pUCVarScope->getUnifyContextId():0ll,
        (long long) pVarTerm->getBinding(),
        pUCStackTop->toString().c_str(), 
        pOtherTerm->toString().c_str() );

    /*
     * Is there already a binding for pUCVarScope::pVarTerm in this?
     */
    
    /* 
     * Create the id object for the variable instance in question.
     * The variable instance is associated with the unify context
     * that instanciated the goal part it was created in.
     */
    UnifyContextId uidScope;
    if( pUCVarScope ) {
        uidScope = pUCVarScope->getUnifyContextId();
    } else {
        uidScope = 0;
    }
    AssignmentId aid( uidScope, pVarTerm->getBinding() );
    InstanceId iid = 0;
    boost::shared_ptr<SingleVarInstance> spInstance;
    (void) pUCStackTop->findVarBinding( aid, iid );
    if( iid ) {
        (void) pUCStackTop->findVarInstance( iid, spInstance );
    }

    if( !iid ) {
        /*
         * If do not have any binding yet, create one binding.
         */
        spInstance = createVarInstance( pUCOther, pOtherTerm );
        pUCStackTop->bindVarBinding( aid, iid );
        pUCStackTop->bindVarInstance( iid, spInstance );
        // This unifies.
        return UnifyLast;
    } else /* !iid */ {
        /* 
         * I have an instance id. Look, if I already have some content.
         */
        if( spInstance ) {
            // I unify with the other term, if my content unifies.
            return spInstance->getTerm()->unifyTerm(
                pEngine,
                pUCStackTop,
                pUCOther, spInstance->getUnifyContext(), pOtherTerm );            
        } else {
            /*
             * I do not carry content yet. Bind my content to the variable.
             */
            spInstance = createVarInstance( pUCOther, pOtherTerm );
            pUCStackTop->bindVarInstance( iid, spInstance );
            // This also unifies.
            return UnifyLast;
        }
    }
    // Never reached.
}


std::string UnifyContext::toString() const
{
    char s[50];
    snprintf( s, 50, "{ id=%lld ", (long long) m_uidUnifyContext );
    std::string strMappings( s );
    bool isFirst = false;
    
    {
        // First find the binding.
            // Binding declared locally?
        std::map<AssignmentId,InstanceId>::const_iterator it =
            m_mapVarMappings.begin();
        std::map<AssignmentId,InstanceId>::const_iterator itEnd =
            m_mapVarMappings.end();
    
        for( ; it != itEnd; ++it ) {
            if( !isFirst ) {
                strMappings += ", ";
            } else {
                isFirst = false;
            }
            AssignmentId aid = it->first;
            InstanceId iid = it->second;
            snprintf( s, 50, "%lld::VT%lld" /* ",%lld" */ "= %lld "
                , (long long) aid.getUnifyContextId()
                , (long long) aid.getVarTermId()
                , (long long) iid
                );
            strMappings += s;
            boost::shared_ptr<SingleVarInstance> spInstance;
            (void) findVarInstance( iid, spInstance );
            if( spInstance ) {
                const UnifyContext* pUC = spInstance->getUnifyContext();
                if( pUC ) {
                    snprintf( s, 50, "%lld::", (long long) pUC->getUnifyContextId() );
                    strMappings += s;
                }
                const AbstractTerm* pTerm = spInstance->getTerm();
                if( pTerm ) {
                    strMappings += pTerm->toString().c_str();
                }
            } else {
                // Not instantiated yet.
            }
        }
    }
    {
        // First find the binding.
            // Binding declared locally?
        std::map<InstanceId,boost::shared_ptr<SingleVarInstance> >::const_iterator it =
            m_mapVarInstances.begin();
        std::map<InstanceId,boost::shared_ptr<SingleVarInstance> >::const_iterator itEnd =
            m_mapVarInstances.end();
    
        for( ; it != itEnd; ++it ) {
            if( !isFirst ) {
                strMappings += ", ";
            } else {
                isFirst = false;
            }
            InstanceId iid = it->first;
            boost::shared_ptr<SingleVarInstance> spInstance = it->second;
            const UnifyContext* pUC = spInstance->getUnifyContext();
            snprintf( s, 50, "%lld: " /* ",%lld" */ "= %lld::\"%s\""
                , (long long) iid
                , (long long) (pUC?pUC->getUnifyContextId():0ll)
                , spInstance->getTerm()->toString().c_str()
                );
            strMappings += s;            
        }
    }
    if( m_pParentUnifyContext ) {
        if( !isFirst ) {
            strMappings += ", ";
        } else {
            isFirst = false;
        }
        strMappings += m_pParentUnifyContext->toString(); 
    }
    strMappings += " }";
    return strMappings;
}


UnifyResult UnifyContext::unifyTerms(
        Engine* pEngine,
        const AbstractTerm* pMyTerm,
        UnifyContext* pUCOther,
        UnifyContext* pUCMine,
        const AbstractTerm* pOther 
        )
{
    UnifyResult res;

#if VAULT_UNIFY_USE_VIRTUAL_DEREF
    const AbstractTerm* pDerefMyTerm = pMyTerm->deref();
    const AbstractTerm* pDerefOther = pOther->deref();
#else
    const AbstractTerm* pDerefMyTerm = pMyTerm;
    const AbstractTerm* pDerefOther = pOther;
#endif

    // TXWTODO: Simplify both sides.
    res = pDerefMyTerm->unifyTerm( pEngine, this, pUCOther, pUCMine, pDerefOther );
    return res;
}


/**
 * Return the result of the actual unification.
 *
 * A unification is successful, if it unified and is not negated.
 * If it is negated, at least one clause must have matched the sequence
 * in order to return a positive result.
 */
UnifyResult UnifyContext::getUnificationResult() const
{
    if( !m_isNegated || UnifyError==m_unificationResult ) {
        return m_unificationResult;
    } else {
        if( Unifies( m_unificationResult ) ) {
            return UnifyNot;
        } else {
            if( m_foundClause ) {
                return UnifyLast;
            } else {
                return UnifyNot;
            }
        }
    }
}


UnifyContext::UnifyContext(
        UnifyContext* parentUnifyContext,
        const GoalPartCursor& csTerm,
        const ExecutionState::ClauseIterator& itClause ) 
    : m_pParentUnifyContext( parentUnifyContext )
    , m_csTermToUnify( csTerm )
    , m_pGoal( NULL )
    , m_itClause( itClause )
    , m_isUnificationDone( false )
    , m_unificationResult( UnifyError )
    , m_isNegated( false )
    , m_foundClause( false )
{
    static UnifyContextId uidNextUnifyContext = 1;
    m_uidUnifyContext = ++uidNextUnifyContext;
}


};
};


