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


std::string EmitBuiltinClause::convertTerm(
        const AbstractTerm* term,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal ) const
{  
    return term->toJSON( true, pUCStackTop, pUCOriginal );
}


void EmitBuiltinClause::output( 
        Engine* pEngine,
        const std::string& outputString ) const
{
    // Engine item E4. Note that `emit` has always had TWO sinks: the text
    // line, and the programmatic fan-out to UserEventListeners. Only the
    // first is output; the second is an event bus and stays as it is.
    if( pEngine ) {
        pEngine->writeOutput( "stdout", "emit: " + outputString + "\n" );
        pEngine->emitEvent( outputString );
    } else {
        std::cout << "emit: " << outputString << std::endl;
    }
}


EmitBuiltinClause::~EmitBuiltinClause()
{
}


EmitBuiltinClause::EmitBuiltinClause()
        : OutputBuiltinClause( "emit" )
{
    setDebugLocation( "file://" __FILE__, __LINE__ );
}


};
};


