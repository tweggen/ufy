/**
 * @file vault-unify-clause-builtin-string.cpp
 *
 * @author Timo Weggen
 *
 * ROADMAP Phase 2: "String operations (concat, compare, match)" (SPEC.md).
 * String comparison already exists via `<`/`<=`/`>`/`>=`/`==`/`!=`
 * (`CompareBuiltinClause`, vault-unify-clause-builtin-arith.cpp) -- this
 * file adds the rest:
 *
 *   - `__builtin_concat($args, $out)` <- `$s = concat( $a, $b, ... );`
 *     (2+ args; parser-recognized, vault-unify-parser.cpp)
 *   - `__builtin_strlen($s, $out)`    <- `$n = strlen( $s );`
 *     (exactly 1 arg; parser-recognized, vault-unify-parser.cpp)
 *   - `contains($s, $sub)`, `startswith($s, $prefix)`, `endswith($s, $suffix)`
 *     -- plain goal builtins, no parser involvement at all (see
 *     vault-unify-clause-builtin.hpp's class comments for the soft-
 *     reservation this implies).
 *
 * All four resolve their string operand(s) via resolveStringArg() below,
 * which mirrors vault-unify-clause-builtin-arith.cpp's resolveCompareSide()
 * exactly: follow a VarTerm binding, evaluate a `__builtin_arith` sub-tree
 * numerically (reusing evaluateArith(), declared in
 * vault-unify-clause-builtin.hpp and implemented in that file), or else
 * require a 0-arity ConsTerm atom -- quoted strings, barewords and digit
 * runs are all the very same Atom kind (SPEC.md section 1), so reading the
 * atom's name value handles all of them uniformly.
 */

#include <string>
#include <cstdio>

#include <boost/shared_ptr.hpp>

#include <list>

#include <vault-unification.hpp>

#include <vault-unify-clause-builtin.hpp>


namespace vault {
namespace unify {

namespace {

/**
 * Resolve one string-operation operand (a concat array element, strlen's
 * sole argument, or a contains/startswith/endswith argument) to its string
 * value. Same operand shapes evaluateArith()/resolveCompareSide()
 * (vault-unify-clause-builtin-arith.cpp) already accept:
 *  - a VarTerm, resolved via AbstractTerm::getBoundTerm() (a no-op
 *    passthrough when pTerm is already bound/not a VarTerm). Unbound is an
 *    error.
 *  - a `__builtin_arith(opAtom, lhs, rhs)` ConsTerm: evaluated numerically
 *    via evaluateArith() and formatted back to its decimal (possibly
 *    `-`-prefixed) text -- so `concat("n=", 2 + 3)` reads as "n=5".
 *  - a plain 0-arity ConsTerm atom (quoted string, bareword, or digit run
 *    -- all the same Atom kind, SPEC.md section 1): its name value IS the
 *    string.
 * Any other shape (a MapTerm/ArrayTerm operand, a non-0-arity ConsTerm
 * other than `__builtin_arith`, or an evaluation error inside an arith
 * sub-tree) is an error, mirroring resolveCompareSide()'s own checks.
 */
bool resolveStringArg(
        UnifyContext* pUCStackTop,
        UnifyContext* pUCOriginal,
        const AbstractTerm* pTerm,
        std::string& out_value,
        std::string& out_error )
{
    const AbstractTerm* pBound = NULL;
    UnifyContext* pUCBound = NULL;
    (void) pTerm->getBoundTerm( pUCStackTop, pUCOriginal, pBound, pUCBound );
    if( !pBound ) {
        out_error = "unbound variable in string argument";
        return false;
    }

    const ConsTerm* pCons = dynamic_cast<const ConsTerm*>( pBound );
    if( !pCons ) {
        out_error = "string argument is not a term";
        return false;
    }

    if( 3==pCons->getArity() && pCons->getName().value() == "__builtin_arith" ) {
        int64_t value;
        if( !evaluateArith( pUCStackTop, pUCOriginal, pCons, value, out_error ) ) {
            return false;
        }
        char buf[32];
        snprintf( buf, sizeof(buf), "%lld", (long long) value );
        out_value = buf;
        return true;
    }

    if( 0 != pCons->getArity() ) {
        out_error = "string argument '" + pCons->toString() + "' is not an atom";
        return false;
    }
    out_value = pCons->getName().value();
    return true;
}

} // anonymous namespace


/**
 * `__builtin_concat([a, b, ...], out)` -- see the file comment and
 * resolveStringArg() above. Argument 0 is always, in practice, a literal
 * `ArrayTerm` built directly by the parser (vault-unify-parser.cpp) right
 * next to this goal, never behind a variable -- but it is still resolved
 * via `getBoundTerm()` (a no-op passthrough in that case) for defensive
 * symmetry with `ArrayAtBuiltinClause` (vault-unify-clause-builtin-array.cpp),
 * which faces the same "always literal today, resolve anyway" situation for
 * its own array argument.
 */
vault::unify::Clause::UnificationState ConcatBuiltinClause::startUnification(
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

    const AbstractTerm* pArgsBound = NULL;
    UnifyContext* pUCArgsBound = NULL;
    (void) pGoalTerm->getTermAt( 0 )->getBoundTerm(
        pUCStackTop, pUCOriginal, pArgsBound, pUCArgsBound );
    const ArrayTerm* pArgsArray = pArgsBound ? dynamic_cast<const ArrayTerm*>( pArgsBound ) : NULL;
    if( !pArgsArray ) {
        VAULT_UNIFY_DI( ALWAYS, "__builtin_concat: argument list is not an array.\n" );
        pUCStackTop->unificationDone( UnifyError, NULL );
        return UnificationError;
    }

    std::string strResult;
    std::string strError;
    int n = pArgsArray->size();
    for( int i = 0; i < n; ++i ) {
        std::string strArg;
        if( !resolveStringArg( pUCStackTop, pUCOriginal, pArgsArray->getElementAt( i ), strArg, strError ) ) {
            VAULT_UNIFY_DI( ALWAYS, "__builtin_concat: %s\n", strError.c_str() );
            pUCStackTop->unificationDone( UnifyError, NULL );
            return UnificationError;
        }
        strResult += strArg;
    }

    // The concatenated result: a fresh, adopted 0-arity ConsTerm atom --
    // same ownership idiom as ArithEvalBuiltinClause's evaluated result
    // (vault-unify-clause-builtin-arith.cpp).
    AbstractTerm* pResultTerm = pUCStackTop->adoptTerm( new vault::unify::ConsTerm( strResult.c_str() ) );

    const AbstractTerm* pOutTerm = pGoalTerm->getTermAt( 1 );
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
ConcatBuiltinClause::~ConcatBuiltinClause()
{
}


ConcatBuiltinClause::ConcatBuiltinClause()
        : SimpleBuiltinClause(
             new vault::unify::ConsTerm( "__builtin_concat",
                 new vault::unify::VarTerm(),
                 new vault::unify::VarTerm() ) )
{
    setDebugLocation( "file://" __FILE__, __LINE__ );
}


/**
 * `__builtin_strlen(arg, out)` -- see the file comment and
 * resolveStringArg() above. The result is the resolved string's BYTE
 * length (`std::string::size()`): v1 counts UTF-8 bytes, not Unicode
 * codepoints -- an honest, documented limitation (SPEC.md), not a bug: a
 * multi-byte codepoint (e.g. most non-ASCII text) counts as more than one.
 */
vault::unify::Clause::UnificationState StrlenBuiltinClause::startUnification(
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

    std::string strArg, strError;
    if( !resolveStringArg( pUCStackTop, pUCOriginal, pGoalTerm->getTermAt( 0 ), strArg, strError ) ) {
        VAULT_UNIFY_DI( ALWAYS, "__builtin_strlen: %s\n", strError.c_str() );
        pUCStackTop->unificationDone( UnifyError, NULL );
        return UnificationError;
    }

    char buf[32];
    snprintf( buf, sizeof(buf), "%lld", (long long) strArg.size() );
    AbstractTerm* pResultTerm = pUCStackTop->adoptTerm( new vault::unify::ConsTerm( buf ) );

    const AbstractTerm* pOutTerm = pGoalTerm->getTermAt( 1 );
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
StrlenBuiltinClause::~StrlenBuiltinClause()
{
}


StrlenBuiltinClause::StrlenBuiltinClause()
        : SimpleBuiltinClause(
             new vault::unify::ConsTerm( "__builtin_strlen",
                 new vault::unify::VarTerm(),
                 new vault::unify::VarTerm() ) )
{
    setDebugLocation( "file://" __FILE__, __LINE__ );
}


/**
 * `contains($s, $sub)` -- see vault-unify-clause-builtin.hpp's class
 * comment for the soft-reservation this plain-goal-name registration
 * implies. Succeeds (`UnifyLast`) iff $sub's resolved string occurs
 * anywhere inside $s's; an unbound argument is a `UnifyError`.
 */
vault::unify::Clause::UnificationState ContainsBuiltinClause::startUnification(
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

    if( 2 != pGoalTerm->getArity() || pGoalTerm->getName() != leftHandTerm()->getName() ) {
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;
    }

    std::string strS, strSub, strError;
    if( !resolveStringArg( pUCStackTop, pUCOriginal, pGoalTerm->getTermAt( 0 ), strS, strError )
     || !resolveStringArg( pUCStackTop, pUCOriginal, pGoalTerm->getTermAt( 1 ), strSub, strError ) ) {
        VAULT_UNIFY_DI( ALWAYS, "contains: %s\n", strError.c_str() );
        pUCStackTop->unificationDone( UnifyError, NULL );
        return UnificationError;
    }

    bool found = std::string::npos != strS.find( strSub );
    pUCStackTop->unificationDone( found ? UnifyLast : UnifyNot, NULL );
    return UnificationOK;
}


// See OutputBuiltinClause::~OutputBuiltinClause() (vault-unify-clause-builtin-output.cpp)
// for why the head term is not freed here.
ContainsBuiltinClause::~ContainsBuiltinClause()
{
}


ContainsBuiltinClause::ContainsBuiltinClause()
        : SimpleBuiltinClause(
             new vault::unify::ConsTerm( "contains",
                 new vault::unify::VarTerm(),
                 new vault::unify::VarTerm() ) )
{
    setDebugLocation( "file://" __FILE__, __LINE__ );
}


/**
 * `startswith($s, $prefix)` -- see ContainsBuiltinClause above (same shape,
 * same soft reservation). Succeeds iff $s's resolved string starts with
 * $prefix's.
 */
vault::unify::Clause::UnificationState StartswithBuiltinClause::startUnification(
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

    if( 2 != pGoalTerm->getArity() || pGoalTerm->getName() != leftHandTerm()->getName() ) {
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;
    }

    std::string strS, strPrefix, strError;
    if( !resolveStringArg( pUCStackTop, pUCOriginal, pGoalTerm->getTermAt( 0 ), strS, strError )
     || !resolveStringArg( pUCStackTop, pUCOriginal, pGoalTerm->getTermAt( 1 ), strPrefix, strError ) ) {
        VAULT_UNIFY_DI( ALWAYS, "startswith: %s\n", strError.c_str() );
        pUCStackTop->unificationDone( UnifyError, NULL );
        return UnificationError;
    }

    bool found = strS.size() >= strPrefix.size()
        && 0 == strS.compare( 0, strPrefix.size(), strPrefix );
    pUCStackTop->unificationDone( found ? UnifyLast : UnifyNot, NULL );
    return UnificationOK;
}


// See OutputBuiltinClause::~OutputBuiltinClause() (vault-unify-clause-builtin-output.cpp)
// for why the head term is not freed here.
StartswithBuiltinClause::~StartswithBuiltinClause()
{
}


StartswithBuiltinClause::StartswithBuiltinClause()
        : SimpleBuiltinClause(
             new vault::unify::ConsTerm( "startswith",
                 new vault::unify::VarTerm(),
                 new vault::unify::VarTerm() ) )
{
    setDebugLocation( "file://" __FILE__, __LINE__ );
}


/**
 * `endswith($s, $suffix)` -- see ContainsBuiltinClause above (same shape,
 * same soft reservation). Succeeds iff $s's resolved string ends with
 * $suffix's.
 */
vault::unify::Clause::UnificationState EndswithBuiltinClause::startUnification(
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

    if( 2 != pGoalTerm->getArity() || pGoalTerm->getName() != leftHandTerm()->getName() ) {
        pUCStackTop->unificationDone( UnifyNot, NULL );
        return UnificationOK;
    }

    std::string strS, strSuffix, strError;
    if( !resolveStringArg( pUCStackTop, pUCOriginal, pGoalTerm->getTermAt( 0 ), strS, strError )
     || !resolveStringArg( pUCStackTop, pUCOriginal, pGoalTerm->getTermAt( 1 ), strSuffix, strError ) ) {
        VAULT_UNIFY_DI( ALWAYS, "endswith: %s\n", strError.c_str() );
        pUCStackTop->unificationDone( UnifyError, NULL );
        return UnificationError;
    }

    bool found = strS.size() >= strSuffix.size()
        && 0 == strS.compare( strS.size() - strSuffix.size(), strSuffix.size(), strSuffix );
    pUCStackTop->unificationDone( found ? UnifyLast : UnifyNot, NULL );
    return UnificationOK;
}


// See OutputBuiltinClause::~OutputBuiltinClause() (vault-unify-clause-builtin-output.cpp)
// for why the head term is not freed here.
EndswithBuiltinClause::~EndswithBuiltinClause()
{
}


EndswithBuiltinClause::EndswithBuiltinClause()
        : SimpleBuiltinClause(
             new vault::unify::ConsTerm( "endswith",
                 new vault::unify::VarTerm(),
                 new vault::unify::VarTerm() ) )
{
    setDebugLocation( "file://" __FILE__, __LINE__ );
}


};
};
