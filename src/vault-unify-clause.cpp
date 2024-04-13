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

ClauseId Clause::m_nextUid;


Clause::~Clause()
{
    delete m_pLeftHandTerm;
    m_pLeftHandTerm = NULL;
}

Clause::Clause( ConsTerm* pLeftHandTerm )
        : m_pLeftHandTerm( pLeftHandTerm )
        , m_uid( m_nextUid++ )
{
}


};
};


