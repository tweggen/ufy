/**
 * @file vault-unify-clause-builtin-arith.cpp
 *
 * @author Timo Weggen
 *
 * ROADMAP Phase 2: "Arithmetic and comparison builtins with a defined
 * evaluation construct" (SPEC.md). Implements the two builtins
 * `AnyTermFactory` (vault-unify-parser.cpp) desugars to:
 *
 *   - `__builtin_eval($expr, $out)`    <- `$y = $x + 1;` (arithmetic in `=`)
 *   - `__builtin_compare($op, $a, $b)` <- `$x < 5;` and friends
 *
 * Both share the same recursive `__builtin_arith(op, lhs, rhs)` tree
 * evaluator (evaluateArith() below), so both accept a full expression on
 * either side (`$x + 1 < $y * 2;` works as much as `$y = $x + 1;` does).
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
 * Parse s as a signed int64 (v1 semantics: no floats; a leading '-' is
 * accepted here even though m_ruleNumber -- src/vault-unify-parser.hpp --
 * never produces one itself, since an evaluated result CAN be negative,
 * e.g. `3 - 10;`, and its atom text ("-7") must itself be readable back as
 * a number by a later expression). Rejects empty strings, anything with
 * trailing garbage after the digits (strtoll only requires a PREFIX to be
 * numeric; a manual full-string check is required to actually reject
 * something like "12abc"), and out-of-range values. Returns false (and
 * leaves out_value untouched) on any failure.
 */
bool parseInt64( const std::string& s, int64_t& out_value )
{
    if( s.empty() ) {
        return false;
    }
    errno = 0;
    char* pEnd = NULL;
    long long v = strtoll( s.c_str(), &pEnd, 10 );
    if( pEnd != s.c_str() + s.length() ) {
        // Trailing garbage (or no digits consumed at all).
        return false;
    }
    if( ERANGE == errno ) {
        return false;
    }
    out_value = (int64_t) v;
    return true;
}


/**
 * Recursively evaluate an arithmetic expression term to an int64.
 *
 * pTerm may be:
 *  - a VarTerm, resolved via AbstractTerm::getBoundTerm() (which is a
 *    no-op passthrough when pTerm is not actually a VarTerm, so this
 *    function never needs to check first -- see its use throughout the
 *    rest of this module, e.g. SimpleBuiltinClause::getConsTermArg()).
 *    An unbound variable is an error.
 *  - a `__builtin_arith(opAtom, lhs, rhs)` ConsTerm (arity 3, built by
 *    AnyTermFactory::operator()(const ArithTermInput&),
 *    vault-unify-parser.cpp): lhs/rhs are evaluated recursively (bottom-up)
 *    and combined per opAtom's text.
 *  - a plain 0-arity ConsTerm atom, whose name must parse as an int64
 *    (parseInt64() above).
 *
 * Any other shape (a MapTerm, an unbound var, a non-numeric atom, division
 * by zero, an unrecognized operator) is an error: returns false and fills
 * out_error with a human-readable message; the caller is expected to
 * VAULT_UNIFY_DI(ALWAYS, ...) it and turn it into a UnifyError.
 */
bool evaluateArith(
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        const AbstractTerm* pTerm,
        int64_t& out_value,
        std::string& out_error )
{
    const AbstractTerm* pBound = NULL;
    UnifyContext* pUCBound = NULL;
    (void) pTerm->getBoundTerm( pUCStackTop, pUCOriginal, pBound, pUCBound );
    if( !pBound ) {
        out_error = "unbound variable in arithmetic expression";
        return false;
    }

    const ConsTerm* pCons = dynamic_cast<const ConsTerm*>( pBound );
    if( !pCons ) {
        out_error = "arithmetic expression operand is not a term";
        return false;
    }

    if( 3==pCons->getArity() && pCons->getName().value() == "__builtin_arith" ) {
        const ConsTerm* pOpAtom = dynamic_cast<const ConsTerm*>( pCons->getTermAt( 0 ) );
        if( !pOpAtom || 0 != pOpAtom->getArity() ) {
            out_error = "malformed arithmetic operator";
            return false;
        }
        const std::string& op = pOpAtom->getName().value();

        int64_t lhsValue, rhsValue;
        if( !evaluateArith( pUCStackTop, pUCOriginal, pCons->getTermAt( 1 ), lhsValue, out_error ) ) {
            return false;
        }
        if( !evaluateArith( pUCStackTop, pUCOriginal, pCons->getTermAt( 2 ), rhsValue, out_error ) ) {
            return false;
        }

        if( "+" == op ) {
            out_value = lhsValue + rhsValue;
            return true;
        }
        if( "-" == op ) {
            out_value = lhsValue - rhsValue;
            return true;
        }
        if( "*" == op ) {
            out_value = lhsValue * rhsValue;
            return true;
        }
        if( "/" == op ) {
            if( 0 == rhsValue ) {
                out_error = "division by zero";
                return false;
            }
            out_value = lhsValue / rhsValue;
            return true;
        }
        out_error = "unknown arithmetic operator '" + op + "'";
        return false;
    }

    // Plain atom: must be a 0-arity numeric atom.
    if( 0 != pCons->getArity() ) {
        out_error = "expected a number, got '" + pCons->toString() + "'";
        return false;
    }
    if( !parseInt64( pCons->getName().value(), out_value ) ) {
        out_error = "'" + pCons->getName().value() + "' is not a number";
        return false;
    }
    return true;
}


/**
 * One side of a comparison, resolved: either numeric (an int64 value,
 * either read directly from a plain atom or produced by evaluating a
 * `__builtin_arith` tree) or, when non-numeric, whatever the atom's raw
 * string value was. See CompareBuiltinClause::startUnification() for how
 * this feeds the numeric-vs-lexicographic decision (SPEC.md: numeric
 * comparison when BOTH sides are integers, lexicographic string
 * comparison otherwise).
 */
struct ResolvedSide {
    bool isNumeric;
    int64_t numValue;
    std::string strValue;
};

bool resolveCompareSide(
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        const AbstractTerm* pTerm,
        ResolvedSide& out_side,
        std::string& out_error )
{
    const AbstractTerm* pBound = NULL;
    UnifyContext* pUCBound = NULL;
    (void) pTerm->getBoundTerm( pUCStackTop, pUCOriginal, pBound, pUCBound );
    if( !pBound ) {
        out_error = "unbound variable in comparison";
        return false;
    }

    const ConsTerm* pCons = dynamic_cast<const ConsTerm*>( pBound );
    if( !pCons ) {
        out_error = "comparison operand is not a term";
        return false;
    }

    if( 3==pCons->getArity() && pCons->getName().value() == "__builtin_arith" ) {
        int64_t value;
        if( !evaluateArith( pUCStackTop, pUCOriginal, pCons, value, out_error ) ) {
            return false;
        }
        out_side.isNumeric = true;
        out_side.numValue = value;
        char buf[32];
        snprintf( buf, sizeof(buf), "%lld", (long long) value );
        out_side.strValue = buf;
        return true;
    }

    if( 0 != pCons->getArity() ) {
        out_error = "comparison operand '" + pCons->toString() + "' is not an atom";
        return false;
    }
    out_side.strValue = pCons->getName().value();
    int64_t v;
    out_side.isNumeric = parseInt64( out_side.strValue, v );
    if( out_side.isNumeric ) {
        out_side.numValue = v;
    }
    return true;
}

} // anonymous namespace


/**
 * `__builtin_eval($expr, $out)` -- see the file comment and
 * evaluateArith() above. Arity/name-checked manually (same pattern as
 * UnifyBuiltinClause/MemberBuiltinClause).
 */
vault::unify::Clause::UnificationState ArithEvalBuiltinClause::startUnification(
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

    if( 2 != pGoalTerm->getArity() || pGoalTerm->getName() != leftHandTerm()->getName() ) {
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;
    }

    const AbstractTerm* pExprTerm = pGoalTerm->getTermAt( 0 );
    const AbstractTerm* pOutTerm = pGoalTerm->getTermAt( 1 );

    int64_t value;
    std::string strError;
    if( !evaluateArith( pUCStackTop, pUCOriginal, pExprTerm, value, strError ) ) {
        VAULT_UNIFY_DI( ALWAYS, "__builtin_eval: %s\n", strError.c_str() );
        pUCStackTop->unificationDone( UnifyError, NULL );
        return UnificationError;
    }

    // The evaluated result: a fresh, adopted 0-arity ConsTerm atom (see
    // UnifyContext::adoptTerm()'s comment, include/vault-unify.hpp, for why
    // this specific allocation needs a new ownership mechanism).
    char buf[32];
    snprintf( buf, sizeof(buf), "%lld", (long long) value );
    AbstractTerm* pResultTerm = pUCStackTop->adoptTerm( new vault::unify::ConsTerm( buf ) );

    UnifyResult unifyResult = pResultTerm->unifyTerm(
        pEngine,
        pUCStackTop,
        pUCOriginal,
        pUCOriginal,
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
ArithEvalBuiltinClause::~ArithEvalBuiltinClause()
{
}


ArithEvalBuiltinClause::ArithEvalBuiltinClause()
        : SimpleBuiltinClause(
             new vault::unify::ConsTerm( "__builtin_eval",
                 new vault::unify::VarTerm(),
                 new vault::unify::VarTerm() ) )
{
    setDebugLocation( "file://" __FILE__, __LINE__ );
}


/**
 * `__builtin_compare($op, $a, $b)` -- see the file comment and
 * resolveCompareSide() above.
 */
vault::unify::Clause::UnificationState CompareBuiltinClause::startUnification(
        Engine* pEngine,
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        UnifyContext* /*pUCCand*/,
        const Goal*& out_pGoal,
        ClauseContinuationContext*& /*inout_pCCC*/ ) const
{
    (void) pEngine;
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

    std::string strError;

    const AbstractTerm* pOpTermBound = NULL;
    UnifyContext* pUCOpBound = NULL;
    (void) pGoalTerm->getTermAt( 0 )->getBoundTerm(
        pUCStackTop, pUCOriginal, pOpTermBound, pUCOpBound );
    const ConsTerm* pOpCons = pOpTermBound
        ? dynamic_cast<const ConsTerm*>( pOpTermBound ) : NULL;
    if( !pOpCons || 0 != pOpCons->getArity() ) {
        VAULT_UNIFY_DI( ALWAYS, "__builtin_compare: operator argument is not a bound atom.\n" );
        pUCStackTop->unificationDone( UnifyError, NULL );
        return UnificationError;
    }
    const std::string& op = pOpCons->getName().value();

    ResolvedSide sideA, sideB;
    if( !resolveCompareSide( pUCStackTop, pUCOriginal, pGoalTerm->getTermAt( 1 ), sideA, strError )
     || !resolveCompareSide( pUCStackTop, pUCOriginal, pGoalTerm->getTermAt( 2 ), sideB, strError ) ) {
        VAULT_UNIFY_DI( ALWAYS, "__builtin_compare: %s\n", strError.c_str() );
        pUCStackTop->unificationDone( UnifyError, NULL );
        return UnificationError;
    }

    int cmp;
    if( sideA.isNumeric && sideB.isNumeric ) {
        cmp = (sideA.numValue < sideB.numValue) ? -1 : (sideA.numValue > sideB.numValue ? 1 : 0);
    } else {
        cmp = sideA.strValue.compare( sideB.strValue );
    }

    bool result;
    if( "<" == op ) {
        result = cmp < 0;
    } else if( "<=" == op ) {
        result = cmp <= 0;
    } else if( ">" == op ) {
        result = cmp > 0;
    } else if( ">=" == op ) {
        result = cmp >= 0;
    } else if( "==" == op ) {
        result = 0 == cmp;
    } else if( "!=" == op ) {
        result = 0 != cmp;
    } else {
        VAULT_UNIFY_DI( ALWAYS, "__builtin_compare: unknown operator '%s'.\n", op.c_str() );
        pUCStackTop->unificationDone( UnifyError, NULL );
        return UnificationError;
    }

    pUCStackTop->unificationDone( result ? UnifyLast : UnifyNot, NULL );
    return UnificationOK;
}


// See OutputBuiltinClause::~OutputBuiltinClause() (vault-unify-clause-builtin-output.cpp)
// for why the head term is not freed here.
CompareBuiltinClause::~CompareBuiltinClause()
{
}


CompareBuiltinClause::CompareBuiltinClause()
        : SimpleBuiltinClause(
             new vault::unify::ConsTerm( "__builtin_compare",
                 new vault::unify::VarTerm(),
                 new vault::unify::VarTerm(),
                 new vault::unify::VarTerm() ) )
{
    setDebugLocation( "file://" __FILE__, __LINE__ );
}


};
};
