/**
 * @file term-value-test.cpp
 *
 * Engine item E7.1 (plans/todo/lens/E7-STRUCTURED-VALUES.md): the walker
 * that turns a grounded term tree into a session `Value`.
 *
 * There is no engine here, no query and no thread, and that is the point of
 * the phase. `toSessionValue()` takes no `UnifyContext` because its input is
 * already grounded, which makes it a pure function of a term -- so it can be
 * tested on terms built by hand, and a failure here names the conversion
 * rather than the solver that produced the term. The other half of the item
 * (a real query, real bindings, a real arena that dies) is
 * nested-binding-test.cpp, deliberately a separate target: when both go red,
 * the pair says which half broke.
 *
 * The interesting assertions are the leaf-typing ones. The engine has no
 * types at term level -- `1`, `red` and `"red"` are all a 0-arity ConsTerm
 * whose Atom name is the raw text -- so `Kind::Int` is DECIDED here, not
 * recovered, and the cases below pin every corner of that decision,
 * including the ones nobody would choose from scratch ("007" is 7) but which
 * follow from reusing the engine's one existing rule.
 */

#include <vault-unify.hpp>

#include "vault-unify-term-value.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "../session/test-harness.hpp"

namespace {

using namespace unify_test;

namespace vu = vault::unify;
namespace us = vault::unify::session;

using us::Value;

/**
 * Owns every hand-built term and frees them all through ONE std::set.
 *
 * Not a convenience: terms can legitimately alias (the ownership essay at
 * include/vault-unify.hpp), and deleting root by root with deleteTermTree()
 * would double-free the moment a case shares a sub-term between two roots.
 * collectTermTree() into a single set de-duplicates by pointer identity,
 * which is the pattern ~World() and ~SolveJob() use for the same reason.
 * CI gates on the leak count being zero, so this has to be right rather
 * than approximately right.
 */
class TermArena
{
public:
    ~TermArena()
    {
        std::set<const vu::AbstractTerm*> visited;
        for( std::size_t i = 0; i < m_roots.size(); ++i ) {
            vu::collectTermTree( m_roots[ i ], visited );
        }
        std::set<const vu::AbstractTerm*>::const_iterator it, itEnd = visited.end();
        for( it = visited.begin(); it != itEnd; ++it ) {
            delete *it;
        }
    }

    /** Hand a freshly-built term over; returns it, so calls can nest. */
    template <typename TermType>
    TermType* own( TermType* pTerm )
    {
        m_roots.push_back( pTerm );
        return pTerm;
    }

private:
    std::vector<const vu::AbstractTerm*> m_roots;
};

/** A 0-arity ConsTerm -- what a number, a bareword and a quoted string all are. */
vu::ConsTerm* atom( TermArena& arena, const char* pName )
{
    return arena.own( new vu::ConsTerm( pName ) );
}

/** Named kinds, so a failure says "Atom, expected Int" instead of "0, 1". */
std::string kindName( Value::Kind kind )
{
    switch( kind ) {
    case Value::Kind::Atom:  return "Atom";
    case Value::Kind::Int:   return "Int";
    case Value::Kind::Float: return "Float";
    case Value::Kind::Str:   return "Str";
    case Value::Kind::Var:   return "Var";
    case Value::Kind::Cons:  return "Cons";
    case Value::Kind::Array: return "Array";
    case Value::Kind::Map:   return "Map";
    }
    return "<unknown>";
}

/** True if any node in the tree has one of the two kinds the engine cannot mean. */
bool hasFloatOrStr( const Value& value )
{
    if( Value::Kind::Float == value.kind || Value::Kind::Str == value.kind ) {
        return true;
    }
    for( std::size_t i = 0; i < value.args.size(); ++i ) {
        if( hasFloatOrStr( value.args[ i ] ) ) {
            return true;
        }
    }
    for( std::size_t i = 0; i < value.pairs.size(); ++i ) {
        if( hasFloatOrStr( value.pairs[ i ].second ) ) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The table in section 2 of the plan, row by row.
// ---------------------------------------------------------------------------

void caseAtom()
{
    TermArena arena;
    const Value value = us::toSessionValue( atom( arena, "red" ) );

    UT_CHECK_EQ( kindName( value.kind ), std::string( "Atom" ) );
    UT_CHECK_EQ( value.name, std::string( "red" ) );
    UT_CHECK( value.args.empty() );
    UT_CHECK( value.pairs.empty() );
    UT_CHECK( !value.truncated );
    UT_CHECK_EQ( value.i, (std::int64_t) 0 );
}

void caseInt()
{
    TermArena arena;
    const Value value = us::toSessionValue( atom( arena, "42" ) );

    UT_CHECK_EQ( kindName( value.kind ), std::string( "Int" ) );
    UT_CHECK_EQ( value.i, (std::int64_t) 42 );
    /*
     * `name` is left empty on purpose: an Int carries its value in `i`, and
     * a front end that reads `name` for every binding (lens's spec runner
     * does today) must be made to fail loudly rather than silently show an
     * empty string. Plan section E7.5 is where that gets fixed.
     */
    UT_CHECK_EQ( value.name, std::string( "" ) );
}

void caseNegativeInt()
{
    TermArena arena;
    /*
     * Not a parser product: m_ruleNumber never produces a sign. This is what
     * ARITHMETIC produces -- `3 - 10;` unifies its output with a fresh atom
     * whose text is "-7" -- and the whole reason parseInt64() accepts a
     * leading '-'. If this row ever became an Atom, every computed negative
     * number would reach the front end as a bareword.
     */
    const Value value = us::toSessionValue( atom( arena, "-7" ) );

    UT_CHECK_EQ( kindName( value.kind ), std::string( "Int" ) );
    UT_CHECK_EQ( value.i, (std::int64_t) -7 );
}

void caseLeadingZeros()
{
    TermArena arena;
    /*
     * Pinned, not endorsed. "007" is the number 7 here because strtoll says
     * so, and the alternative -- a second, stricter definition of "is this
     * text a number" living in the adapter -- is worse than the quirk: the
     * engine would happily do arithmetic on an atom the front end refused to
     * show as a number. The text is not recoverable from the Value; that is
     * the price, and it is paid once, here.
     */
    const Value value = us::toSessionValue( atom( arena, "007" ) );

    UT_CHECK_EQ( kindName( value.kind ), std::string( "Int" ) );
    UT_CHECK_EQ( value.i, (std::int64_t) 7 );
}

void caseTrailingGarbage()
{
    TermArena arena;
    /*
     * strtoll only requires a PREFIX to be numeric, so "12x" would be 12 to
     * a careless reading. parseInt64() checks that the whole string was
     * consumed, which is exactly the check being relied on here.
     */
    const Value value = us::toSessionValue( atom( arena, "12x" ) );

    UT_CHECK_EQ( kindName( value.kind ), std::string( "Atom" ) );
    UT_CHECK_EQ( value.name, std::string( "12x" ) );
    UT_CHECK_EQ( value.i, (std::int64_t) 0 );
}

void caseEmptyName()
{
    TermArena arena;
    /*
     * An atom with an empty name: an Atom with an empty name, NOT truncated.
     * The distinction matters because a NULL term also produces an empty
     * Atom, and it marks it truncated -- see caseNullTerm() below. Without
     * that difference a front end could not tell "the empty atom" from
     * "something went missing here".
     */
    const Value value = us::toSessionValue( atom( arena, "" ) );

    UT_CHECK_EQ( kindName( value.kind ), std::string( "Atom" ) );
    UT_CHECK_EQ( value.name, std::string( "" ) );
    UT_CHECK( !value.truncated );
}

void caseCons()
{
    TermArena arena;
    vu::ConsTerm* pPoint = arena.own( new vu::ConsTerm(
        "point", atom( arena, "1" ), atom( arena, "red" ) ) );

    const Value value = us::toSessionValue( pPoint );

    UT_CHECK_EQ( kindName( value.kind ), std::string( "Cons" ) );
    UT_CHECK_EQ( value.name, std::string( "point" ) );
    UT_CHECK_EQ( value.args.size(), (std::size_t) 2 );
    UT_CHECK_EQ( kindName( value.args[ 0 ].kind ), std::string( "Int" ) );
    UT_CHECK_EQ( value.args[ 0 ].i, (std::int64_t) 1 );
    UT_CHECK_EQ( kindName( value.args[ 1 ].kind ), std::string( "Atom" ) );
    UT_CHECK_EQ( value.args[ 1 ].name, std::string( "red" ) );
}

void caseArray()
{
    TermArena arena;
    vu::ArrayTerm* pArray = arena.own( new vu::ArrayTerm() );
    pArray->append( atom( arena, "a" ) );
    pArray->append( atom( arena, "2" ) );

    const Value value = us::toSessionValue( pArray );

    UT_CHECK_EQ( kindName( value.kind ), std::string( "Array" ) );
    UT_CHECK( value.name.empty() );
    UT_CHECK_EQ( value.args.size(), (std::size_t) 2 );
    UT_CHECK_EQ( value.args[ 0 ].name, std::string( "a" ) );
    UT_CHECK_EQ( value.args[ 1 ].i, (std::int64_t) 2 );
}

void caseMapKeyOrder()
{
    TermArena arena;

    /*
     * Built in the WRONG order deliberately: MapTerm stores its entries in a
     * std::map, so getEntries() -- and therefore `pairs` -- comes out in key
     * order regardless of construction order. A front end may rely on that
     * being stable; a ValuePath into `pairs` certainly does.
     *
     * The MapTerm constructor takes ownership of the Atom* keys (~MapTerm()
     * deletes them), so they are NOT registered with the arena; the value
     * terms are, since ~MapTerm() does not touch those.
     */
    const vu::Atom* keys[ 2 ];
    keys[ 0 ] = new vu::Atom( "zebra" );
    keys[ 1 ] = new vu::Atom( "apple" );
    vu::AbstractTerm* values[ 2 ];
    values[ 0 ] = atom( arena, "striped" );
    values[ 1 ] = atom( arena, "3" );

    vu::MapTerm* pMap = arena.own( new vu::MapTerm( keys, values, 2 ) );

    const Value value = us::toSessionValue( pMap );

    UT_CHECK_EQ( kindName( value.kind ), std::string( "Map" ) );
    UT_CHECK( value.args.empty() );
    UT_CHECK_EQ( value.pairs.size(), (std::size_t) 2 );
    UT_CHECK_EQ( value.pairs[ 0 ].first, std::string( "apple" ) );
    UT_CHECK_EQ( value.pairs[ 0 ].second.i, (std::int64_t) 3 );
    UT_CHECK_EQ( value.pairs[ 1 ].first, std::string( "zebra" ) );
    UT_CHECK_EQ( value.pairs[ 1 ].second.name, std::string( "striped" ) );
}

void caseNested()
{
    TermArena arena;

    /*
     * The shape the whole item exists for: { shapes: [ point( 1, 2 ) ] }.
     * A flattened solution renders this as text a front end has to re-parse,
     * which is precisely what lens's spec files avoid by being flat facts.
     */
    vu::ConsTerm* pPoint = arena.own( new vu::ConsTerm(
        "point", atom( arena, "1" ), atom( arena, "2" ) ) );
    vu::ArrayTerm* pArray = arena.own( new vu::ArrayTerm() );
    pArray->append( pPoint );

    const vu::Atom* keys[ 1 ];
    keys[ 0 ] = new vu::Atom( "shapes" );
    vu::AbstractTerm* values[ 1 ];
    values[ 0 ] = pArray;
    vu::MapTerm* pMap = arena.own( new vu::MapTerm( keys, values, 1 ) );

    const Value value = us::toSessionValue( pMap );

    UT_CHECK_EQ( kindName( value.kind ), std::string( "Map" ) );
    UT_CHECK_EQ( value.pairs.size(), (std::size_t) 1 );
    UT_CHECK_EQ( value.pairs[ 0 ].first, std::string( "shapes" ) );

    const Value& array = value.pairs[ 0 ].second;
    UT_CHECK_EQ( kindName( array.kind ), std::string( "Array" ) );
    UT_CHECK_EQ( array.args.size(), (std::size_t) 1 );

    const Value& point = array.args[ 0 ];
    UT_CHECK_EQ( kindName( point.kind ), std::string( "Cons" ) );
    UT_CHECK_EQ( point.name, std::string( "point" ) );
    UT_CHECK_EQ( point.args.size(), (std::size_t) 2 );
    UT_CHECK_EQ( point.args[ 0 ].i, (std::int64_t) 1 );
    UT_CHECK_EQ( point.args[ 1 ].i, (std::int64_t) 2 );

    /* Nothing in a term-derived tree may ever be a Float or a Str. */
    UT_CHECK( !hasFloatOrStr( value ) );
}

void caseVarNamed()
{
    TermArena arena;
    vu::VarTerm* pVar = arena.own( new vu::VarTerm() );
    pVar->setOriginalVarName( "$p" );

    const Value value = us::toSessionValue( pVar );

    UT_CHECK_EQ( kindName( value.kind ), std::string( "Var" ) );
    UT_CHECK_EQ( value.name, std::string( "$p" ) );
}

void caseVarAnonymous()
{
    TermArena arena;
    /*
     * A VarTerm with no original name is what cloning produces (and what a
     * desugared temporary can be). The display name then has to come from
     * the id, in exactly the form VarTerm::toString() uses, so that a
     * transcript line and a debug dump call the same variable the same
     * thing.
     */
    vu::VarTerm* pVar = arena.own( new vu::VarTerm() );
    UT_CHECK( pVar->getOriginalVarName().empty() );

    const Value value = us::toSessionValue( pVar );

    UT_CHECK_EQ( kindName( value.kind ), std::string( "Var" ) );
    UT_CHECK_EQ( value.name,
                 std::string( "VT" ) + std::to_string( (long long) pVar->getBinding() ) );
}

// ---------------------------------------------------------------------------
// The two things that must not crash.
// ---------------------------------------------------------------------------

void caseDepthCap()
{
    TermArena arena;

    /*
     * Nothing upstream guards against a cyclic binding, and a term tree
     * deeper than the stack is the same failure with a different cause.
     * Build one past the cap and require a marked value rather than a crash.
     */
    vu::AbstractTerm* pTerm = atom( arena, "leaf" );
    const int nDepth = us::kMaxTermValueDepth + 5;
    for( int i = 0; i < nDepth; ++i ) {
        pTerm = arena.own( new vu::ConsTerm( "f", pTerm ) );
    }

    const Value root = us::toSessionValue( pTerm );

    /* Descend: kMaxTermValueDepth full Cons nodes, then a marked stub. */
    const Value* pCursor = &root;
    for( int i = 0; i < us::kMaxTermValueDepth; ++i ) {
        UT_CHECK_MSG( Value::Kind::Cons == pCursor->kind,
                      "level " << i << " is " << kindName( pCursor->kind ) );
        UT_CHECK_MSG( !pCursor->truncated, "level " << i << " is marked truncated" );
        UT_CHECK_MSG( 1 == pCursor->args.size(),
                      "level " << i << " has " << pCursor->args.size() << " args" );
        pCursor = &pCursor->args[ 0 ];
    }

    UT_CHECK_EQ( kindName( pCursor->kind ), std::string( "Cons" ) );
    UT_CHECK_EQ( pCursor->name, std::string( "f" ) );
    UT_CHECK( pCursor->truncated );
    UT_CHECK( pCursor->args.empty() );
}

void caseNullTerm()
{
    /*
     * Cannot happen while the four-kind invariant holds, which is why it is
     * worth one case: "cannot happen" is how a crash gets shipped. The
     * answer is an empty Atom marked truncated -- honest about there being
     * something it could not show, and distinguishable from the atom whose
     * name genuinely is empty (caseEmptyName above).
     */
    const Value value = us::toSessionValue( NULL );

    UT_CHECK_EQ( kindName( value.kind ), std::string( "Atom" ) );
    UT_CHECK( value.name.empty() );
    UT_CHECK( value.truncated );
}

} // namespace

int main()
{
    Registry registry;

    registry.add( "atom: a non-numeric 0-arity ConsTerm is Kind::Atom", caseAtom );
    registry.add( "int: a digit-run atom is Kind::Int", caseInt );
    registry.add( "int: arithmetic's \"-7\" atom is Int -7", caseNegativeInt );
    registry.add( "int: \"007\" is Int 7 -- the engine's one parse rule", caseLeadingZeros );
    registry.add( "atom: \"12x\" is not a number", caseTrailingGarbage );
    registry.add( "atom: the empty name stays an untruncated Atom", caseEmptyName );
    registry.add( "cons: arity > 0 keeps functor and recursed args", caseCons );
    registry.add( "array: elements become args", caseArray );
    registry.add( "map: pairs come out in std::map key order", caseMapKeyOrder );
    registry.add( "nested: Cons inside Array inside Map, and never Float/Str", caseNested );
    registry.add( "var: an unbound VarTerm keeps its original name", caseVarNamed );
    registry.add( "var: a nameless VarTerm falls back to VT<id>", caseVarAnonymous );
    registry.add( "depth: the hard cap marks the node it cut", caseDepthCap );
    registry.add( "null: a NULL term is answered, not crashed on", caseNullTerm );

    return registry.run( "term-value (engine item E7.1)" ) == 0 ? 0 : 1;
}
