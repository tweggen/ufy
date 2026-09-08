/**
 * @file vault-unify-term-value.cpp
 *
 * See the header for the leaf-typing table, why Float and Str are never
 * produced, and why this function takes no UnifyContext.
 *
 * Structurally this is resolveTermGrounded()'s reader
 * (vault-unify-terms.cpp): the same dynamic_cast dispatch over the four
 * concrete term kinds, in the same order, with the same "no other kind
 * exists" tail. It deliberately does NOT use applyVisitor() -- that is a
 * pre-order flat visit and would lose the structure this function exists to
 * preserve -- and it uses the direct accessors rather than
 * abstractTermIterator(), which is a factory whose result the caller must
 * delete (include/vault-unify.hpp) and which has leaked here once already.
 */

#include "vault-unify-term-value.hpp"

#include <vault-unify-clause-builtin.hpp>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace vault {
namespace unify {
namespace session {

namespace {

/**
 * A `VarTerm`'s display name.
 *
 * `getOriginalVarName()` is empty whenever the VarTerm was made by cloning
 * rather than by the parser -- which is exactly what resolveTermGrounded()
 * does to an unbound variable it cannot resolve, and it copies the original
 * name across, so an empty one here means the variable never had a source
 * name at all (a desugared temporary, or a variable cloned twice). The
 * fallback mirrors VarTerm::toString() (include/vault-unify.hpp) so that a
 * front end and a debug dump call the same variable the same thing.
 */
std::string varDisplayName( const VarTerm* pVar )
{
    const std::string strOriginal = pVar->getOriginalVarName();
    if( !strOriginal.empty() ) {
        return strOriginal;
    }
    char s[ 32 ];
    snprintf( s, sizeof( s ), "VT%lld", (long long) pVar->getBinding() );
    return std::string( s );
}

Value convert( const AbstractTerm* pTerm, int depth );

/** The node itself, with its children left off and the cut marked. */
Value truncatedStub( const AbstractTerm* pTerm )
{
    Value value;
    value.truncated = true;

    if( const VarTerm* pVar = dynamic_cast<const VarTerm*>( pTerm ) ) {
        value.kind = Value::Kind::Var;
        value.name = varDisplayName( pVar );
    } else if( const ConsTerm* pCons = dynamic_cast<const ConsTerm*>( pTerm ) ) {
        // Only a compound can be deep enough to be cut, so this is Cons and
        // never Int/Atom -- but say it in terms of the arity anyway, so the
        // stub of a leaf (unreachable today) would still be honest.
        value.kind = pCons->getArity() > 0 ? Value::Kind::Cons : Value::Kind::Atom;
        value.name = pCons->getName().value();
    } else if( dynamic_cast<const MapTerm*>( pTerm ) ) {
        value.kind = Value::Kind::Map;
    } else if( dynamic_cast<const ArrayTerm*>( pTerm ) ) {
        value.kind = Value::Kind::Array;
    }
    return value;
}

Value convertCons( const ConsTerm* pCons, int depth )
{
    Value value;
    const int nArity = pCons->getArity();

    if( 0 == nArity ) {
        // The one place the engine's typelessness has to become a decision;
        // see the header. parseInt64() is the engine's own rule, borrowed
        // rather than restated.
        int64_t i64 = 0;
        if( parseInt64( pCons->getName().value(), i64 ) ) {
            value.kind = Value::Kind::Int;
            value.i = i64;
        } else {
            value.kind = Value::Kind::Atom;
            value.name = pCons->getName().value();
        }
        return value;
    }

    value.kind = Value::Kind::Cons;
    value.name = pCons->getName().value();
    value.args.reserve( (std::size_t) nArity );
    for( int i = 0; i < nArity; ++i ) {
        value.args.push_back( convert( pCons->getTermAt( i ), depth + 1 ) );
    }
    return value;
}

Value convertMap( const MapTerm* pMap, int depth )
{
    Value value;
    value.kind = Value::Kind::Map;

    // getEntries() walks the MapTerm's own std::map, so `pairs` comes out in
    // key order. The keys stay owned by pMap (see ~MapTerm()); this copies
    // the text out and keeps no pointer to them.
    std::vector<std::pair<const Atom*, AbstractTerm*> > entries;
    pMap->getEntries( entries );
    value.pairs.reserve( entries.size() );
    for( std::size_t i = 0; i < entries.size(); ++i ) {
        value.pairs.push_back( std::make_pair(
            entries[ i ].first->value(),
            convert( entries[ i ].second, depth + 1 ) ) );
    }
    return value;
}

Value convertArray( const ArrayTerm* pArray, int depth )
{
    Value value;
    value.kind = Value::Kind::Array;

    const int nElements = pArray->size();
    value.args.reserve( (std::size_t) nElements );
    for( int i = 0; i < nElements; ++i ) {
        value.args.push_back( convert( pArray->getElementAt( i ), depth + 1 ) );
    }
    return value;
}

Value convert( const AbstractTerm* pTerm, int depth )
{
    if( !pTerm ) {
        // Cannot happen while the four-kind invariant holds (asserted at
        // vault-unify-terms.cpp:230) -- and a NULL child is exactly what a
        // half-built term would have, so answer honestly instead of
        // crashing: an empty Atom would be indistinguishable from the atom
        // whose name really is empty, so mark it truncated. That is what
        // `truncated` means to a front end: there is more here than you are
        // being shown.
        Value value;
        value.kind = Value::Kind::Atom;
        value.truncated = true;
        return value;
    }

    if( depth >= kMaxTermValueDepth ) {
        // Defence in depth, not the budget. Nothing guards against a cyclic
        // binding upstream, and one would recurse until the stack ended.
        return truncatedStub( pTerm );
    }

    if( const ConsTerm* pCons = dynamic_cast<const ConsTerm*>( pTerm ) ) {
        return convertCons( pCons, depth );
    }
    if( const VarTerm* pVar = dynamic_cast<const VarTerm*>( pTerm ) ) {
        // Unbound: the caller grounded this tree already, so a VarTerm that
        // survived resolution is a variable with no binding, not one nobody
        // followed.
        Value value;
        value.kind = Value::Kind::Var;
        value.name = varDisplayName( pVar );
        return value;
    }
    if( const MapTerm* pMap = dynamic_cast<const MapTerm*>( pTerm ) ) {
        return convertMap( pMap, depth );
    }
    if( const ArrayTerm* pArray = dynamic_cast<const ArrayTerm*>( pTerm ) ) {
        return convertArray( pArray, depth );
    }

    // Internal error: no other concrete AbstractTerm kind exists in this
    // module, which is why cloneTermTree()/resolveTermGrounded() end the
    // same way. Log it and hand back the term's own text rather than
    // dropping the binding -- a fifth kind is a code change, and its author
    // should see this line rather than an empty solution.
    VAULT_UNIFY_DI( ALWAYS,
        "toSessionValue: term '%s' is neither ConsTerm, VarTerm, MapTerm nor "
        "ArrayTerm; emitting its text as an atom.\n", pTerm->toString().c_str() );
    Value value;
    value.kind = Value::Kind::Atom;
    value.name = pTerm->toString();
    return value;
}

} // namespace

Value toSessionValue( const vault::unify::AbstractTerm* pTerm )
{
    return convert( pTerm, 0 );
}

} // namespace session
} // namespace unify
} // namespace vault
