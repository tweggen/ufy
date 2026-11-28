#if !defined( _LENS_TEST_VALUE_BUILDERS_HPP )
#define _LENS_TEST_VALUE_BUILDERS_HPP

/**
 * @file value-builders.hpp
 *
 * Hand-built `Value` trees, shared by the model tests that need them.
 */

#include "vault-unify-session.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lens_test {

namespace us = vault::unify::session;

/*
 * Value builders. Written out rather than reached for through a session,
 * because half of these kinds no session in this tree can produce: `Str` and
 * `Float` are unreachable from the engine by construction (quoting is lost in
 * the parser, and there are no floats), and `Var` waits on engine item E7.4.
 * A hand-built tree is the only way to pin the arms that render them, and a
 * remote core or the fake session will send them long before lens's own
 * engine does.
 */
inline us::Value vAtom( const char* name )
{
    us::Value v;
    v.kind = us::Value::Kind::Atom;
    v.name = name;
    return v;
}

inline us::Value vInt( std::int64_t i )
{
    us::Value v;
    v.kind = us::Value::Kind::Int;
    v.i = i;
    return v;
}

inline us::Value vFloat( double f )
{
    us::Value v;
    v.kind = us::Value::Kind::Float;
    v.f = f;
    return v;
}

inline us::Value vStr( const char* text )
{
    us::Value v;
    v.kind = us::Value::Kind::Str;
    v.name = text;
    return v;
}

inline us::Value vVar( const char* name )
{
    us::Value v;
    v.kind = us::Value::Kind::Var;
    v.name = name;
    return v;
}

inline us::Value vCons( const char* functor, std::vector<us::Value> args )
{
    us::Value v;
    v.kind = us::Value::Kind::Cons;
    v.name = functor;
    v.args = args;
    return v;
}

inline us::Value vArray( std::vector<us::Value> args )
{
    us::Value v;
    v.kind = us::Value::Kind::Array;
    v.args = args;
    return v;
}

inline us::Value vMap( std::vector<std::pair<std::string, us::Value>> pairs )
{
    us::Value v;
    v.kind = us::Value::Kind::Map;
    v.pairs = pairs;
    return v;
}

/** Mark it truncated, as `applyBudget` would after cutting children. */
inline us::Value cut( us::Value v )
{
    v.truncated = true;
    return v;
}

/**
 * The cut mark `renderValue` uses, spelled the same way it is spelled there.
 *
 * Escaped, not written as a glyph: this string is the assertion, and an
 * assertion that depends on the test file's own encoding surviving a
 * checkout would fail for reasons that have nothing to do with the code.
 */
inline const std::string kCutMark = "\u2026";

} // namespace lens_test

#endif // _LENS_TEST_VALUE_BUILDERS_HPP
