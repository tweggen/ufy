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


/**
 * Unify ConsTerm with ConsTerm.
 * 
 * Second double dispatch function. Unifying two ConsTerms works
 * by comparing signature and recursively unifying sub-terms.
 *
 * @param pUC
 *     The unify context that which should store the result of unification.
 * @param uidOtherUnifyContext
 *     The uid of the other unify context.
 * 
 * @return
 *     Returns positive, if terms unify. Returns zero, if terms do not unify.
 *     Returns negative on execution error.
 */
UnifyResult ConsTerm::unifyConsTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOther,    
        UnifyContext* pUCMine,
        const ConsTerm* pOther ) const
{
    int res;

    VAULT_UNIFY_DI( UNIFY, "ConsTerm: Asked to unify %lld:%s with %lld:%s into %lld.\n", 
        pUCMine?pUCMine->getUnifyContextId():0ll,
        this->toString().c_str(),
        pUCOther?pUCOther->getUnifyContextId():0ll,
        pOther->toString().c_str(),
        pUCStackTop?pUCStackTop->getUnifyContextId():0ll
        );

    // ConsTerm 
    // 1. If terms are identical, unifies to rhs of clause.
    if( this == pOther ) {
        // Generate copy in goal.
        // Unify.
        return UnifyLast;
    }

    if( pOther->m_name != m_name ) {
        VAULT_UNIFY_DI( UNIFY, "ConsTerm: Name differs.\n" );
        // Does not unify.
        return UnifyNot;
    }

    size_t l = m_vecTerms.size();
    // 2. If terms have different aryty, they do not unify.
    if( pOther->m_vecTerms.size() != l ) {
        VAULT_UNIFY_DI( UNIFY, "ConsTerm: Aryness differs.\n" );
        // Does not unify.
        return UnifyNot;
    }
    
    for( size_t i=0; i<l; ++i ) {
        const AbstractTerm* pTermMe = m_vecTerms[i];
        const AbstractTerm* pTermOther = pOther->m_vecTerms[i];

        // Try to unify the terms. Possible variable binding will be emitted
        // to the unification context.
        // Old API:         res = pTermMe->unifyTerm( pEngine, pUCStackTop, pUCOther, pUCMine, pTermOther );
        res = pUCStackTop->unifyTerms( pEngine, pTermMe, pUCOther, pUCMine, pTermOther );
        // If unifies and dynamic, continue unification.
        // If does not unify and dynamic, continue unification as far as possible.
        // If unifies and static, continue unification.
        // If does not unify and static, abort unification.
        if( !res ) {
            VAULT_UNIFY_DI( UNIFY, "ConsTerm: Subterm doesnt unify.\n" );
            return UnifyNot;
        }
    }

    // Unifies.
    return UnifyLast;
}


// 2nd order call: this is clause, other is goal.
UnifyResult ConsTerm::unifyVarTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOther,
        UnifyContext* pUCMine,
        const VarTerm* pOther ) const
{
    // Sort parameters and forward to generic version.
    return pUCStackTop->genericUnifyVarWithKnown(
        pEngine,
        pUCStackTop,
        pUCOther,       // Unify to scope the variable
        pOther,         // Variable in scope
        pUCMine,        // Unify context of myself
        this            // non-var term
        );
}

UnifyResult ConsTerm::unifyMapTerm(
        Engine* /*pEngine*/,
        UnifyContext* /*pUCStackTop*/,
        UnifyContext* /*pUCOther*/,
        UnifyContext* /*pUCMine*/,
        const MapTerm* /*pOther*/ ) const
{
    /*
     * These are terms of different types.
     * Thees cannot unify at all.
     */
    return UnifyNot;
}

UnifyResult ConsTerm::unifyTerm(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOther,
        UnifyContext* pUCMine,
        const AbstractTerm* pOther ) const
{
    // I am asked to unify with myself, no further binding.
    if( this == pOther ) return UnifyLast;

    // Dispatch second half.
    return pOther->unifyConsTerm(
        pEngine,
        pUCStackTop,
        pUCMine,
        pUCOther,
        this );
}


#if VAULT_UNIFY_USE_VIRTUAL_DEREF
const AbstractTerm* ConsTerm::deref() const
{
#if 1
    return this;
#else
    const char* name = m_name.value().c_str();

    // Likely case first: This is not a tree traversal modifier.
    if( *name != '=' ) {
        return this;
    }

    // TXWTODO: Optimize
    if( !strncmp( name+1, "deref", 5 ) ) {
        if( m_vecTerms.size() != 2 ) {
            // Wrong type. Doesn't unify.
            VAULT_UNIFY_DI( ALWAYS, "size != 2\n" );
            return NULL;
        }
        MapTerm* pMap = dynamic_cast<MapTerm*>( m_vecTerms[0] );
        if( !pMap ) {
            // Invalid operation. 
            // Return NULL so that unification will fail.
            VAULT_UNIFY_DI( ALWAYS, "no Map\n" );
            return NULL;
            // TXWTODO: Couldn't we just not evaluate this?
        }
        ConsTerm* pKey = dynamic_cast<ConsTerm*>( m_vecTerms[1] );
        if( !pKey ) {
            // Invalid operation.
            // Return NULL so that unification will fail.
            VAULT_UNIFY_DI( ALWAYS, "no ConsTerm as key\n" );
            return NULL;
        }
        VAULT_UNIFY_DI( ALWAYS, "Derefed map key \"%s\".", pKey->getName().value().c_str() );
        const AbstractTerm* pResultTerm = pMap->getValue( pKey->getName().value() );

        return pResultTerm;
    } else {
        VAULT_UNIFY_DI( ALWAYS, "Unsupported tree modifier %s.\n", name );
        // Unsupported tree modifier.
        return NULL;
    }
#endif
}
#endif


const std::string ConsTerm::toContextString( 
        const UnifyContext* pUCStackTop,
        const UnifyContext* pUCTerm ) const
{
    std::string strRes = m_name.value();
    if( !m_vecTerms.empty() ) {
        strRes += "(";
        int l = m_vecTerms.size();
        for( int i=0; i<l; i++ ) {
            if( i ) strRes += ",";
            strRes += m_vecTerms[i]->toContextString( pUCStackTop, pUCTerm );
        }
        strRes += ")";
    }
    return strRes;
}


};
};


