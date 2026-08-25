/**
 * @file vault-unify-clause-builtin-array.cpp
 *
 * @author Timo Weggen
 *
 * ROADMAP ("for"/"foreach" loops, language owner request 2026-08-21).
 * Implements the one builtin `foreach`'s synthesized `__fe__N` clause's own
 * body relies on:
 *
 *   - `__builtin_array_at($arr, $idx, $out)` <- __fe__N's own
 *     `__builtin_array_at($arr, $i, $x);` goal (AnyTermFactory::
 *     operator()(const ForeachStatementInput&), vault-unify-parser.cpp) --
 *     never written directly in a user program.
 *
 * See vault-unify-clause-builtin.hpp for the full contract.
 */

#include <string>
#include <cerrno>
#include <cstdlib>
#include <cstdio>

#include <boost/shared_ptr.hpp>

#include <list>

#include <vault-unification.hpp>

#include <vault-unify-clause-builtin.hpp>


namespace vault {
namespace unify {

namespace {

/**
 * Parse s as a signed int64, requiring the WHOLE string to be consumed
 * (rejecting e.g. "12abc") -- same check as
 * vault-unify-clause-builtin-arith.cpp's parseInt64() and
 * vault-unify-parser.cpp's parseRangeLiteralInt64(), duplicated locally
 * (this module's established per-file local-helper style; see either of
 * those for the identical rationale).
 */
bool parseArrayAtInt64( const std::string& s, int64_t& out_value )
{
    if( s.empty() ) {
        return false;
    }
    errno = 0;
    char* pEnd = NULL;
    long long v = strtoll( s.c_str(), &pEnd, 10 );
    if( pEnd != s.c_str() + s.length() ) {
        return false;
    }
    if( ERANGE == errno ) {
        return false;
    }
    out_value = (int64_t) v;
    return true;
}

} // anonymous namespace


/**
 * `__builtin_array_at($arr, $idx, $out)` -- see the file comment and
 * vault-unify-clause-builtin.hpp's class comment.
 */
vault::unify::Clause::UnificationState ArrayAtBuiltinClause::startUnification(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* /*pUCCand*/,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& /*inout_pCCC*/ ) const
{
    out_pGoal = NULL;

    const ConsTerm* pGoalTerm =
        dynamic_cast<const ConsTerm*>(
            pUCStackTop->m_csTermToUnify.getAbstractTerm() );
    if( !pGoalTerm ) {
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;
    }

    if( 3 != pGoalTerm->getArity() || pGoalTerm->getName() != leftHandTerm()->getName() ) {
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;
    }

    // Argument 0 must resolve to an ArrayTerm.
    const AbstractTerm* pArrBound = NULL;
    UnifyContext* pUCArrBound = NULL;
    (void) pGoalTerm->getTermAt( 0 )->getBoundTerm(
        pUCStackTop, pUCOriginal, pArrBound, pUCArrBound );
    const ArrayTerm* pArray = pArrBound ? dynamic_cast<const ArrayTerm*>( pArrBound ) : NULL;
    if( !pArray ) {
        // Unbound, or bound to something that isn't an array -- fail
        // silently, same as MemberBuiltinClause's non-map left side
        // (SPEC.md section 9).
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;
    }

    // Argument 1 must resolve to a bound, 0-arity, integer-parseable atom.
    const AbstractTerm* pIdxBound = NULL;
    UnifyContext* pUCIdxBound = NULL;
    (void) pGoalTerm->getTermAt( 1 )->getBoundTerm(
        pUCStackTop, pUCOriginal, pIdxBound, pUCIdxBound );
    const ConsTerm* pIdxCons = pIdxBound ? dynamic_cast<const ConsTerm*>( pIdxBound ) : NULL;
    int64_t idx;
    if( !pIdxCons || 0 != pIdxCons->getArity() || !parseArrayAtInt64( pIdxCons->getName().value(), idx ) ) {
        VAULT_UNIFY_DI( ALWAYS, "__builtin_array_at: index argument is not a bound integer.\n" );
        pUCStackTop->unificationDone( UnifyError, NULL );
        return UnificationError;
    }

    if( idx < 0 || idx >= (int64_t) pArray->size() ) {
        // Out of bounds -- the loop's own termination condition (foreach:
        // "no more elements"). An ordinary goal failure, not an error.
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;
    }

    const AbstractTerm* pElement = pArray->getElementAt( (int) idx );
    const AbstractTerm* pOutTerm = pGoalTerm->getTermAt( 2 );

    // Mirrors MemberBuiltinClause's own unify call shape exactly: "mine" is
    // the scope the matched value (here, the array element) was bound in
    // (pUCArrBound), "other" is the scope of the output argument
    // (pUCOriginal).
    UnifyResult unifyResult = pElement->unifyTerm(
        pEngine,
        pUCStackTop,
        pUCOriginal,
        pUCArrBound,
        pOutTerm );

    pUCStackTop->unificationDone( unifyResult, NULL );

    if( (int) unifyResult < 0 ) {
        return UnificationError;
    } else {
        return UnificationOK;
    }
}


// See OutputBuiltinClause::~OutputBuiltinClause() (vault-unify-clause-builtin-output.cpp)
// for why the head term is not freed here.
ArrayAtBuiltinClause::~ArrayAtBuiltinClause()
{
}


ArrayAtBuiltinClause::ArrayAtBuiltinClause()
        : SimpleBuiltinClause(
             new vault::unify::ConsTerm( "__builtin_array_at",
                 new vault::unify::VarTerm(),
                 new vault::unify::VarTerm(),
                 new vault::unify::VarTerm() ) )
{
    setDebugLocation( "file://" __FILE__, __LINE__ );
}


};
};
