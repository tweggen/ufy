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

TermTraversable::~TermTraversable() {}


#if VAULT_UNIFY_USE_VIRTUAL_DEREF
const AbstractTerm* AbstractTerm::deref() const
{
    return this;
}
#endif

/**
 * Given an arbitrary term, return a bound term or NULL.
 * If the term is non-var, return it. If it is a varterm, find the
 * instance and return. If no instance is bound, return NULL.
 *
 * TXWTODO: Implement a polymorphic helper function in XxxtTerm?
 */
int AbstractTerm::getBoundTerm(
    const UnifyContext* pUCStackTop,
    const UnifyContext* pUCInput,
    // const AbstractTerm* pInputTerm,
    const AbstractTerm*& out_pTerm,
    UnifyContext*& out_pUCOutput ) const
{
    const AbstractTerm* pInputTerm = this;
    const VarTerm* pVarTerm = dynamic_cast<const VarTerm*>( pInputTerm );
    if( !pVarTerm ) {
        out_pUCOutput = const_cast<UnifyContext*>( pUCInput );
        out_pTerm = pInputTerm;
        return 0;
    }
    // Dereference VarTerm.
    UnifyContextId uidOrg;
    if( pUCInput ) {
        uidOrg = pUCInput->getUnifyContextId();
    } else {
        uidOrg = 0;
    }
    AssignmentId aid( uidOrg, pVarTerm->getBinding() );

    boost::shared_ptr<SingleVarInstance> spInstance;
    InstanceId iid = 0;
    (void) pUCStackTop->findVarBinding( aid, iid );
    if( iid ) {
        (void) pUCStackTop->findVarInstance( iid, spInstance );
    }
    if( spInstance ) {
        VAULT_UNIFY_DI( UNIFY, "VT%lld: Found binding to instance.\n", 
            (long long) pVarTerm->getBinding() );
        const AbstractTerm* pInstanceTerm = spInstance->getTerm();
        UnifyContext* pUCInstanceTerm = spInstance->getUnifyContext();
        out_pTerm = pInstanceTerm;
        out_pUCOutput = pUCInstanceTerm;
   } else {
        out_pTerm = NULL;
        out_pUCOutput = NULL;
   }
   return 0;
}



AbstractTermIterator::~AbstractTermIterator() {}

const TermTraversable* Goal::GoalIterator::getTermTraversable() const {
    return dynamic_cast<const TermTraversable*>( *m_it );
}


/**
 * ROADMAP Phase 1 (Ownership model), pass 2. See the ownership note above
 * the declarations of collectTermTree()/deleteTermTree() in vault-unify.hpp.
 *
 * Walks pTerm's children generically via the existing TermTraversable /
 * AbstractTermIterator machinery (ConsTerm's sub-terms, MapTerm's values;
 * VarTerm is a leaf and returns a NULL iterator), rather than reaching into
 * ConsTerm/MapTerm internals directly.
 */
void collectTermTree( const AbstractTerm* pTerm, std::set<const AbstractTerm*>& out_visited )
{
    if( !pTerm ) {
        return;
    }
    if( !out_visited.insert( pTerm ).second ) {
        // Already visited (an aliased/shared sub-term) -- do not recurse
        // into it again.
        return;
    }

    AbstractTermIterator* pIt = pTerm->abstractTermIterator();
    if( pIt ) {
        while( pIt->isValid() ) {
            const TermTraversable* pChildTraversable = pIt->getTermTraversable();
            if( pChildTraversable ) {
                const AbstractTerm* pChildTerm = dynamic_cast<const AbstractTerm*>( pChildTraversable );
                if( pChildTerm ) {
                    collectTermTree( pChildTerm, out_visited );
                }
            }
            pIt->next();
        }
        delete pIt;
    }
}


void deleteTermTree( const AbstractTerm* pTerm )
{
    if( !pTerm ) {
        return;
    }
    std::set<const AbstractTerm*> visited;
    collectTermTree( pTerm, visited );
    std::set<const AbstractTerm*>::const_iterator it, itEnd = visited.end();
    for( it = visited.begin(); it != itEnd; ++it ) {
        delete *it;
    }
}


};
};


