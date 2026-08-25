
#ifndef _VAULT_OZW_GENERIC_NODE_VALUE_CLAUSE_
#define _VAULT_OZW_GENERIC_NODE_VALUE_CLAUSE_

#include <vault-unify.hpp>

#include "vault-unify-iterator-clause.hpp"

namespace vault {
namespace ozw {

/**
 * Saves discontinuous iteration state when iterating over several nodes.
 */
template <class Object1, class Object2IteratorFactory>
class GenericIterator2CCC    
    : public vault::unify::ClauseContinuationContext
{
public:
    typedef typename Object2IteratorFactory::iterator_type Object2Iterator;

    GenericIterator2CCC(
        const vault::unify::Clause* pClause,
        Object1 object1,
        const Object2IteratorFactory& iterator2Factory )
            : vault::unify::ClauseContinuationContext( pClause )
            , m_pChildCCC( NULL )
            , m_matchMode( UndefinedMatches )
            , m_object1( object1 )
            , m_object2Iterator( iterator2Factory.begin( object1 ) )
            , m_object2IteratorEnd( iterator2Factory.end( object1 ) )
    {
    }
    virtual ~GenericIterator2CCC() {}

    enum MatchMode {
        UndefinedMatches,
        SeveralMatches,
        SingleMatch
    };

    /**
     * A continuation context that may or may not be created by the
     * specific node clause sub class.
     */ 
    vault::unify::ClauseContinuationContext* m_pChildCCC;

    MatchMode m_matchMode;

    Object1 m_object1;
    Object2Iterator m_object2Iterator;
    Object2Iterator m_object2IteratorEnd;
};



template <class Object1IteratorFactory, class Object2IteratorFactory>
class GenericIterator2Clause
        : public GenericIteratorClause<Object1IteratorFactory>
{
public:
    typedef typename GenericIteratorClause<Object1IteratorFactory>::Object Object1;
    typedef typename GenericIteratorClause<Object1IteratorFactory>::ObjectIterator Object1Iterator;
    typedef typename Object2IteratorFactory::iterator_type Object2Iterator;
    typedef typename Object2Iterator::value_type Object2;

    GenericIterator2Clause( 
            const char* name, int ary,
            const Object1IteratorFactory& iterator1Factory,
            const Object2IteratorFactory& iterator2Factory)
        : GenericIteratorClause<Object1IteratorFactory>( name, ary, iterator1Factory )
        , m_iterator2Factory( iterator2Factory ) {}

    virtual ~GenericIterator2Clause() {}

protected:

    /**
     * Using a given node id, operate the unification process.
     * Do not touch the GenericIteratorCCC part, currently, I do 
     * not support sub-continuation.
     */
    virtual vault::unify::Clause::UnificationState startUnificationForNode(
            Object1 spNode,
            vault::unify::Engine* pEngine,
            vault::unify::UnifyContext* pUCStackTop,
            vault::unify::UnifyContext* pUCOriginal,
            vault::unify::UnifyContext* pUCCand,
            const vault::unify::Goal*& out_pGoal,
            vault::unify::ClauseContinuationContext*& pCCC ) const
    {
        using namespace vault::unify;

        out_pGoal = NULL;
        GenericIterator2CCC<Object1, Object2IteratorFactory>* pGenericIterator2CCC;
        vault::unify::Clause::UnificationState subUnificationState = vault::unify::Clause::UnificationOK;

        // VAULT_OZW_DI( ALWAYS, "Called.\n" );

        /*
         * look, wether we have a consterm with exactly three arguments
         * to unify with our lhs.
         */
        const ConsTerm* pGoalTerm = dynamic_cast<const ConsTerm*>( pUCStackTop->m_csTermToUnify.getAbstractTerm() );

        // The argument can either be an unbound variable or some term.
        // If it is an unbound variable, we start iterating over all possible nodes,
        // using a continuation object.
        // Otherwise, we get the bound term. If the bound term is a consterm
        // then we test wether it matches a node id that exists.
        const vault::unify::AbstractTerm* pTermSecond = pGoalTerm->getTermAt( 1 );
        const vault::unify::VarTerm* pVarSecond = dynamic_cast<const vault::unify::VarTerm*>( pTermSecond );
        const vault::unify::AbstractTerm* pTermSecondBound = NULL;
        // Note, that we do not need the map's unify context.
        vault::unify::UnifyContext* pUCSecond = NULL;

        if( pVarSecond ) {
            // Yes, this is a variable, and it is bound.
            (void) pVarSecond->getBoundTerm(
                pUCStackTop,
                pUCOriginal,
                pTermSecondBound,
                pUCSecond );
        }

        // Always create a continuation context, even if won't really use it.
        if( !pCCC ) {
            pGenericIterator2CCC = new GenericIterator2CCC<Object1, Object2IteratorFactory>( 
                this, spNode, m_iterator2Factory );
            pCCC = pGenericIterator2CCC;

            // Determine mode of operation.
            if( pVarSecond && !pTermSecondBound ) {
                pGenericIterator2CCC->m_matchMode = GenericIterator2CCC<Object1, Object2IteratorFactory>::SeveralMatches;
            } else {
                pGenericIterator2CCC->m_matchMode = GenericIterator2CCC<Object1, Object2IteratorFactory>::SingleMatch;         
            }

        } else {
            pGenericIterator2CCC = dynamic_cast<GenericIterator2CCC<Object1, Object2IteratorFactory>*>( pCCC );
            if( GenericIterator2CCC<Object1, Object2IteratorFactory>::UndefinedMatches==pGenericIterator2CCC->m_matchMode ) {
                // VAULT_OZW_DI( ALWAYS, "Undefined match mode?\n" );
                return vault::unify::Clause::UnificationError;
            }
        }


        // Operate depending on the results.
        if( GenericIterator2CCC<Object1, Object2IteratorFactory>::SeveralMatches == pGenericIterator2CCC->m_matchMode ) {

            // We iterate over the variables.
#if 1
            if( pGenericIterator2CCC->m_object2Iterator==pGenericIterator2CCC->m_object2IteratorEnd ) {
                delete pCCC;
                pCCC = NULL;
                pUCStackTop->unificationDone( UnifyNot, NULL );
                return vault::unify::Clause::UnificationOK;             
            }
            Object2 spValue = *(pGenericIterator2CCC->m_object2Iterator);
#else           
            // Look up the current node if we did not yet.
            vault::ozw::ValueInfoPtr spValue = pGenericIterator2CCC->m_spValue;
            if( !spValue ) {
                spValue = spNode->getNextValueInfo( pGenericIterator2CCC->m_lastValueId );
                // No further match? Delete continuation.
                if( !spValue ) {
                    delete pCCC;
                    pCCC = NULL;
                    pUCStackTop->unificationDone( UnifyNot, NULL );
                    return UnificationOK;
                } 
                pGenericIterator2CCC->m_spValue = spValue;
            }   
#endif
            // Unify id with variable.
#if 1
            vault::unify::ConsTerm *consTerm = 
                new vault::unify::ConsTerm( 
                    getCombineId( spValue ).c_str() );
#else
            char s[24];
            snprintf( s, 23, "ozwv0x%08x%08x",
                (unsigned) (spValue->m_valueId.GetId()>>32),
                (unsigned) (spValue->m_valueId.GetId() & 0xffffffff ) );
            vault::unify::ConsTerm *consTerm = new vault::unify::ConsTerm( s );
#endif

            UnifyResult unifyResult = consTerm->unifyTerm( pEngine, pUCStackTop, pUCOriginal, pUCOriginal, 
                pTermSecond );

            // Only call unify for node if the nodeid could be unified.
            if( Unifies( unifyResult ) ) {
                subUnificationState = startUnificationForValue(
                    spNode, spValue,
                    pEngine, pUCStackTop, pUCOriginal, pUCCand, out_pGoal,
                    pGenericIterator2CCC->m_pChildCCC );
            } else {
                pUCStackTop->unificationDone( UnifyNot, NULL );
            }

            // Only advance to next node in next iteration, if the child did not create
            // a sub-continuation context.
            if( !pGenericIterator2CCC->m_pChildCCC ) {
#if 1
                ++pGenericIterator2CCC->m_object2Iterator;
#else
                // Remember last id for continuation.
                pGenericIterator2CCC->m_lastValueId = spValue->m_valueId;
                pGenericIterator2CCC->m_spValue.reset();
#endif
            } else {
                // Otherwise, operate on this node until the child continuation context is gone.
            }

            return subUnificationState;

        } else { // This is single match operation.

            // Do we already know the node?
            Object2 spValue = *(pGenericIterator2CCC->m_object2Iterator);

            // Parse the node.
            // lhs either is some term or a var bound to a term.
            const vault::unify::AbstractTerm* pAnySecond;
            if( pTermSecondBound ) {
                pAnySecond = pTermSecondBound;
            } else {
                pAnySecond = pTermSecond;
            }
            // Must be consTerm
            const vault::unify::ConsTerm* pConsTerm = dynamic_cast<const vault::unify::ConsTerm*>( pAnySecond );
            if( pConsTerm ) {
                pGenericIterator2CCC->m_object2Iterator = m_iterator2Factory.find( spNode, pConsTerm );
            }
            if( pGenericIterator2CCC->m_object2Iterator==pGenericIterator2CCC->m_object2IteratorEnd ) {
                // Does not unify: Node not found.
                delete pCCC;
                pCCC = NULL;
                pUCStackTop->unificationDone( UnifyNot, NULL );
                return vault::unify::Clause::UnificationOK;           
            }
            spValue = *(pGenericIterator2CCC->m_object2Iterator);

            subUnificationState = startUnificationForValue(
                    spNode, spValue,
                    pEngine, pUCStackTop, pUCOriginal, pUCCand, out_pGoal,
                    pGenericIterator2CCC->m_pChildCCC );

            // Do we need to reiterate?
            if( !pGenericIterator2CCC->m_pChildCCC ) {
                delete pCCC;
                pCCC = NULL;
            } else {
                // Keep the pCCC.
            }

            return subUnificationState;

        }

        // Never reached.
        return vault::unify::Clause::UnificationOK;
    }


    /**
     * Using a given node id, operate the unification process.
     * Do not touch the GenericIteratorCCC part, currently, I do 
     * not support sub-continuation.
     */
    virtual vault::unify::Clause::UnificationState startUnificationForValue(
        Object1 spNode,
        Object2 spValue,
        vault::unify::Engine* pEngine,
        vault::unify::UnifyContext* pUCStackTop,
        vault::unify::UnifyContext* pUCOriginal,
        vault::unify::UnifyContext* pUCCand,
        const vault::unify::Goal*& out_pGoal,
        vault::unify::ClauseContinuationContext*& pCCC ) const = 0;

private:
    Object2IteratorFactory m_iterator2Factory;
};


};
};


#endif
