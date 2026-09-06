/**
 * @file vault-unification.cpp
 *
 * @author Timo Weggen
 *
 * Implementation of unification engine.
 */

#include <string>
#include <iostream>

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
    // Engine item E4: through the engine's sink, not to this process's
    // stdout. The default sink reproduces the previous
    //     std::cout << "print: " << outputString << std::endl
    // exactly -- prefix, newline and flush -- because the golden corpus
    // pins every byte of it.
    if( pEngine ) {
        pEngine->writeOutput( "stdout", "print: " + outputString + "\n" );
    } else {
        std::cout << "print: " << outputString << std::endl;
    }
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


