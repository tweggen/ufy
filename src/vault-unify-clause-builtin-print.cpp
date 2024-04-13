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

#include <vault-unify-clause-builtin.hpp>

namespace vault {
namespace unify {


void PrintBuiltinClause::output( 
        Engine* pEngine,
        const std::string& outputString ) const
{
    std::cout << "print: " << outputString << std::endl;
}


std::string PrintBuiltinClause::convertTerm(
        const AbstractTerm* term,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal ) const
{  
    return term->toContextString( pUCStackTop, pUCOriginal );
}


PrintBuiltinClause::~PrintBuiltinClause()
{
}


PrintBuiltinClause::PrintBuiltinClause()
        : OutputBuiltinClause( "print" )
{
    setDebugLocation( "file://" __FILE__, __LINE__ );
}


};
};


