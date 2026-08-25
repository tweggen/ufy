#ifndef _VAULT_OZW_GENERIC_NODE_CLAUSE_
#define _VAULT_OZW_GENERIC_NODE_CLAUSE_

#include <vault-unify.hpp>

namespace vault {
namespace ozw {

template <class S> std::string getCombineId( const S obj ) { return obj->getCombineId(); }

template <> inline std::string getCombineId( const std::string s ) { return s; }

class Manager;

// typedef Manager::NodeIterator ObjectIterator;
// typedef NodeIteratorFactory ObjectIteratorFactory;

/**
 * Saves discontinuous iteration state when iterating over several nodes.
 */
template <class ObjectIteratorFactory>
class GenericIteratorCCC     
    : public vault::unify::ClauseContinuationContext
{
public:
    typedef typename ObjectIteratorFactory::iterator_type ObjectIterator;

    GenericIteratorCCC( 
        const vault::unify::Clause* pClause,
        const ObjectIteratorFactory& iteratorFactory )
            : vault::unify::ClauseContinuationContext( pClause )
            , m_pChildCCC( NULL )
            , m_matchMode( UndefinedMatches )
            , m_objectIterator( iteratorFactory.begin() )
            , m_objectIteratorEnd( iteratorFactory.end() )
    {
    }

    virtual ~GenericIteratorCCC() {}

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

    ObjectIterator m_objectIterator;
    ObjectIterator m_objectIteratorEnd;
};

/**
 * Unify clause to list all zWave or to test wether a string describes a zwave home/node id.
 */
template <class ObjectIteratorFactory> class GenericIteratorClause
        : public vault::unify::SimpleBuiltinClause
{
public:
    typedef typename ObjectIteratorFactory::iterator_type ObjectIterator;
    typedef typename ObjectIterator::value_type Object;

    GenericIteratorClause(
        const char* name,
        int ary,
        const ObjectIteratorFactory& iteratorFactory ) 
            : SimpleBuiltinClause( 
                new vault::unify::ConsTerm( name,
                    new vault::unify::VarTerm() ) )
            , m_name( name )
            , m_ary( ary )
            , m_iteratorFactory( iteratorFactory ) {}


    virtual ~GenericIteratorClause() {}


    virtual vault::unify::Clause::UnificationState startUnification(
            vault::unify::Engine* pEngine,
            vault::unify::UnifyContext* pUCStackTop,
            vault::unify::UnifyContext* pUCOriginal,
            vault::unify::UnifyContext* pUCCand,
            const vault::unify::Goal*& out_pGoal,
            vault::unify::ClauseContinuationContext*& pCCC ) const
    {
        using namespace vault::unify; 
        out_pGoal = NULL;
        GenericIteratorCCC<ObjectIteratorFactory>* pGenericIteratorCCC;
        vault::unify::Clause::UnificationState subUnificationState = UnificationOK;

        // VAULT_OZW_DI( ALWAYS, "Called.\n" );

        /*
         * look, wether we have a consterm with exactly three arguments
         * to unify with our lhs.
         */
        const ConsTerm* pGoalTerm =
            dynamic_cast<const ConsTerm*>(
                pUCStackTop->m_csTermToUnify.getAbstractTerm() );
        if( !pGoalTerm ) {
            // Wrong syntax.
            // VAULT_UNIFY_DI( ALWAYS, "pGoalTerm==NULL.\n" );
            pUCStackTop->unificationDone( UnifyNot, NULL );
            return UnificationOK;
        }
        int arity = pGoalTerm->getArity();

        if( pGoalTerm->getName() != leftHandTerm()->getName() ) {
            // Not me?
            // VAULT_UNIFY_DI( ALWAYS, "%s != %s.\n",
             //   pGoalTerm->getName().value().c_str(), leftHandTerm()->getName().value().c_str() );
            pUCStackTop->unificationDone( UnifyNot, NULL );
            return UnificationOK;
        } else {
            // me?
            // VAULT_UNIFY_DI( ALWAYS, "%s matches %s.\n",
            //   pGoalTerm->getName().value().c_str(), leftHandTerm()->getName().value().c_str() );
        }

        if( m_ary != arity ) {
            // VAULT_UNIFY_DI( ALWAYS, "%d != arity.\n", m_ary );
            pUCStackTop->unificationDone( UnifyNot, NULL );
            return UnificationOK;
        }

        // The argument can either be an unbound variable or some term.
        // If it is an unbound variable, we start iterating over all possible nodes,
        // using a continuation object.
        // Otherwise, we get the bound term. If the bound term is a consterm
        // then we test wether it matches a node id that exists.
        const vault::unify::AbstractTerm* pTermLeft = pGoalTerm->getTermAt( 0 );
        const vault::unify::VarTerm* pVarLeft = dynamic_cast<const vault::unify::VarTerm*>( pTermLeft );
        const vault::unify::AbstractTerm* pTermLeftBound = NULL;
        // Note, that we do not need the map's unify context.
        vault::unify::UnifyContext* pUCLeft = NULL;

        if( pVarLeft ) {
            // Yes, this is a variable, and it is bound.
            (void) pVarLeft->getBoundTerm(
                pUCStackTop,
                pUCOriginal,
                pTermLeftBound,
                pUCLeft );
            //if( !pTermLeftBound ) {
                // This is an unbound variable. Start iteration.
            //}
        }

        // Always create a continuation context, even if won't really use it.
        if( !pCCC ) {
            pGenericIteratorCCC = new GenericIteratorCCC<ObjectIteratorFactory>( this, m_iteratorFactory );
            pCCC = pGenericIteratorCCC;

            // Determine mode of operation.
            if( pVarLeft && !pTermLeftBound ) {
                pGenericIteratorCCC->m_matchMode = GenericIteratorCCC<ObjectIteratorFactory>::SeveralMatches;
            } else {
                pGenericIteratorCCC->m_matchMode = GenericIteratorCCC<ObjectIteratorFactory>::SingleMatch;          
            }

        } else {
            pGenericIteratorCCC = dynamic_cast<GenericIteratorCCC<ObjectIteratorFactory>*>( pCCC );
            if( GenericIteratorCCC<ObjectIteratorFactory>::UndefinedMatches==pGenericIteratorCCC->m_matchMode ) {
                // VAULT_OZW_DI( ALWAYS, "Undefined match mode?\n" );
                return UnificationError;
            }
        }


        // Operate depending on the results.
        if( GenericIteratorCCC<ObjectIteratorFactory>::SeveralMatches == pGenericIteratorCCC->m_matchMode ) {

            // We iterate over the variables.
#if 1
            if( pGenericIteratorCCC->m_objectIterator==pGenericIteratorCCC->m_objectIteratorEnd ) {
                // No more nodes.
                delete pCCC;
                pCCC = NULL,
                pUCStackTop->unificationDone( UnifyNot, NULL );
                return UnificationOK;
            }
            Object spNode = *(pGenericIteratorCCC->m_objectIterator);
#else
            vault::ozw::NodeInfoPtr spNode = pGenericIteratorCCC->m_spNode;
            if( !spNode ) {
                spNode = m_spManager->getNextNodeInfo(
                    pGenericIteratorCCC->m_lastHomeId, pGenericIteratorCCC->m_lastNodeId );
                // No further match? Delete continuation.
                if( !spNode ) {
                    delete pCCC;
                    pCCC = NULL;
                    pUCStackTop->unificationDone( UnifyNot, NULL );
                    return UnificationOK;
                } 
                pGenericIteratorCCC->m_spNode = spNode;
            }
#endif

            // Unify id with variable.
            vault::unify::ConsTerm *consTerm =
                new vault::unify::ConsTerm(
                    getCombineId( spNode ).c_str() );

            UnifyResult unifyResult = consTerm->unifyTerm( pEngine, pUCStackTop, pUCOriginal, pUCOriginal, 
                pTermLeft );

            // Only call unify for node if the nodeid could be unified.
            if( Unifies( unifyResult ) ) {
                subUnificationState = startUnificationForNode(
                    spNode,
                    pEngine, pUCStackTop, pUCOriginal, pUCCand, out_pGoal,
                    pGenericIteratorCCC->m_pChildCCC );
            } else {
                pUCStackTop->unificationDone( UnifyNot, NULL );
            }

            // Only advance to next node in next iteration, if the child did not create
            // a sub-continuation context.
            if( !pGenericIteratorCCC->m_pChildCCC ) {
#if 1
                ++pGenericIteratorCCC->m_objectIterator;
#else
                // Remember last id for continuation.
                pGenericIteratorCCC->m_lastHomeId = spNode->m_homeId;
                pGenericIteratorCCC->m_lastNodeId = spNode->m_nodeId;
                pGenericIteratorCCC->m_spNode.reset();
#endif
            } else {
                // Otherwise, operate on this node until the child continuation context is gone.
            }

            return subUnificationState;

        } else { // This is single match operation.

            // Do we already know the node?
            Object spNode;

            // Parse the node.
            // lhs either is some term or a var bound to a term.
            const vault::unify::AbstractTerm* pAnyLeft;
            if( pTermLeftBound ) {
                pAnyLeft = pTermLeftBound;
            } else {
                pAnyLeft = pTermLeft;
            }
            // Must be consTerm
            const vault::unify::ConsTerm* pConsTerm = dynamic_cast<const vault::unify::ConsTerm*>( pAnyLeft );
            if( pConsTerm ) {
                pGenericIteratorCCC->m_objectIterator = m_iteratorFactory.find( pConsTerm );
            }
            if( pGenericIteratorCCC->m_objectIterator==pGenericIteratorCCC->m_objectIteratorEnd ) {
                // Does not unify: Node not found.
                delete pCCC;
                pCCC = NULL;
                pUCStackTop->unificationDone( UnifyNot, NULL );
                return UnificationOK;           
            }
            spNode = *(pGenericIteratorCCC->m_objectIterator);

            subUnificationState = startUnificationForNode(
                    spNode,
                    pEngine, pUCStackTop, pUCOriginal, pUCCand, out_pGoal,
                    pGenericIteratorCCC->m_pChildCCC );

            // Do we need to reiterate?
            if( !pGenericIteratorCCC->m_pChildCCC ) {
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



protected:

    /* boost::shared_ptr<vault::ozw::Manager> m_spManager; */

    /**
     * Using a given node id, operate the unification process.
     * Do not touch the GenericIteratorCCC part, currently, I do 
     * not support sub-continuation.
     */
    virtual vault::unify::Clause::UnificationState startUnificationForNode(
        Object spNode,
        vault::unify::Engine* pEngine,
        vault::unify::UnifyContext* pUCStackTop,
        vault::unify::UnifyContext* pUCOriginal,
        vault::unify::UnifyContext* pUCCand,
        const vault::unify::Goal*& out_pGoal,
        vault::unify::ClauseContinuationContext*& pCCC  ) const = 0;

private:
    const char* m_name;
    int m_ary;

    ObjectIteratorFactory m_iteratorFactory;
};

};
};


#endif
