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

namespace {

/// See isDebugTraceEnabled()/setDebugTraceEnabled() (include/vault-unify.hpp).
/// Written once, before any worker thread is started, by whoever wants the
/// engine quiet; read from every thread afterwards.
bool s_isDebugTraceEnabled = true;

} // anonymous namespace


bool isDebugTraceEnabled()
{
    return s_isDebugTraceEnabled;
}


void setDebugTraceEnabled( bool enabled )
{
    s_isDebugTraceEnabled = enabled;
}


GoalPart::GoalPart(
        const Goal* pGoal, 
        GoalPart* pParentGoalPart,
        UnifyContext* pOriginUnifyContext,
        const vault::unify::Goal::GoalIterator& nextTerm )
    : m_pNewGoal( pGoal )
    , m_pParentGoalPart( pParentGoalPart )
    , m_itNextTermInParent( nextTerm )
    , m_pOriginUnifyContext( pOriginUnifyContext )
{
    std::string str( "GoalPart => { " );
    str = "idUCOrigin=" 
        + boost::lexical_cast<std::string>(
            m_pOriginUnifyContext?pOriginUnifyContext->getUnifyContextId():0 )
        + ", goalNew="
        + (m_pNewGoal?m_pNewGoal->toString():"\"\"")
        + ", goalPartParent=" + (m_pParentGoalPart?
            boost::lexical_cast<std::string>(
                (long long) m_pParentGoalPart ):"NUL" )
        + ", idxParentPart=" + boost::lexical_cast<std::string>( m_itNextTermInParent.getIndex() )
        + " }";
}


const std::string GoalPart::toString() const
{
    return std::string( "GoalPart::toString() empty result string" );
}


std::string Goal::toString() const
{
    std::string strGoal;
    if( isEmpty() ) { return "true"; }
    bool isFirst = true;
    for( GoalIterator it = termIterator(); it.isValid(); it.next() ) {
        if( !isFirst ) {
            strGoal += ", ";
        } else {
            isFirst = false;
        }
        const AbstractTerm* term = it.getAbstractTerm();
        if( term ) {
            strGoal += term->toString();
        } else {
            strGoal += "NULL";
        }
    }
    return strGoal;
}


std::string Goal::toJSON( bool useContent, const UnifyContext* pUCStackTop, const UnifyContext* pUCTerm ) const
{
    std::string strGoal;
    if( isEmpty() ) { return "true"; }
    bool isFirst = true;
    strGoal += "[";
    for( GoalIterator it = termIterator(); it.isValid(); it.next() ) {
        if( !isFirst ) {
            strGoal += ", ";
        } else {
            isFirst = false;
        }
        const AbstractTerm* term = it.getAbstractTerm();
        if( term ) {
            strGoal += term->toJSON( useContent, pUCStackTop, pUCTerm );
        } else {
            strGoal += "null";
        }
    }
    strGoal += "]";
    return strGoal;
}

ClauseContinuationContext::~ClauseContinuationContext()
{
}

};
};


