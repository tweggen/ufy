#if !defined( _VAULT_UNIFY_TERM_VALUE_HPP )
#define _VAULT_UNIFY_TERM_VALUE_HPP

/**
 * @file vault-unify-term-value.hpp
 *
 * Engine item E7.1 (plans/todo/lens/E7-STRUCTURED-VALUES.md): a GROUNDED
 * term tree becomes a session `Value`.
 *
 * This is adapter code, not core. Together with
 * vault-unify-local-session.cpp it is the only place in the tree that sees
 * both `vault-unify.hpp` and `vault-unify-session.hpp`, so it must never be
 * included by a core translation unit -- gate G0.1's isolation target
 * (unify/test/session/isolation-tu.cpp) proves the boundary header itself
 * stays free of the engine, and this header must not be the thing that
 * quietly re-links the two.
 *
 * It lives in `vault::unify::session` beside applyBudget()/toDisplayString()
 * (include/vault-unify-session-value.hpp): everything here is a helper over
 * `Value`, and the engine types it reads are spelled out with their
 * `vault::unify::` scope so that the direction of the dependency -- session
 * side reaching down, never the reverse -- is visible in the signature.
 */

#include "vault-unify-session.hpp"

#include <vault-unify.hpp>

namespace vault {
namespace unify {
namespace session {

/**
 * Convert a GROUNDED term tree into a session `Value`.
 *
 * No `UnifyContext` parameter, deliberately. The caller resolves first --
 * `resolveTermGrounded()` (include/vault-unify.hpp) promises "every node
 * returned is a fresh allocation, safe to outlive pUCStackTop's own arena"
 * -- so by the time a term arrives here there is nothing left to
 * dereference. That is what makes this a pure function of a term, testable
 * on hand-built terms with no engine, no query and no threads
 * (unify/test/engine/term-value-test.cpp), and it is also why a `VarTerm`
 * reaching this function means an UNBOUND variable rather than a binding
 * nobody followed.
 *
 * The engine has no types at term level. There are exactly four concrete
 * term kinds (asserted exhaustive at vault-unify-terms.cpp:230), and `1`,
 * `red` and `"red"` all parse to the SAME thing: a 0-arity `ConsTerm` whose
 * Atom name is the raw text (SPEC.md, "Atoms"). So `Kind::Int` cannot be
 * RECOVERED here, only DECIDED, and the decision is this table:
 *
 *   ConsTerm, arity 0, name parses as int64   -> Int   (`i` set)
 *   ConsTerm, arity 0, otherwise              -> Atom  (`name`)
 *   ConsTerm, arity > 0                       -> Cons  (`name` = functor,
 *                                                       `args` recursed)
 *   ArrayTerm                                 -> Array (`args`)
 *   MapTerm                                   -> Map   (`pairs`, std::map
 *                                                       key order)
 *   VarTerm                                   -> Var   (`name` =
 *                                                       getOriginalVarName(),
 *                                                       else "VT<id>")
 *
 * "Parses as int64" is `parseInt64()` (declared in
 * vault-unify-clause-builtin.hpp, defined in
 * vault-unify-clause-builtin-arith.cpp), the engine's one existing
 * definition of "is this text a number" -- reused rather than restated, so
 * that a value the engine is willing to do arithmetic on is exactly a value
 * the front end is shown as a number. It accepts a leading `-` on purpose:
 * arithmetic produces atoms like "-7" and they must read back as numbers.
 * Its quirks come along with it, and that is the point of having one copy:
 * "007" is Int 7 and " 7" is Int 7, because strtoll says so.
 *
 * `Kind::Float` and `Kind::Str` are NEVER produced, and this is not an
 * oversight to be fixed:
 *
 *  - There are no floats in the language. The number rule is a digit run
 *    (`m_ruleNumber`, vault-unify-parser.hpp) and SPEC.md:50 says so --
 *    "unsigned digit runs only; no floats".
 *  - Quoting is lost at parse time (SPEC.md:70): `red` and `"red"` are
 *    byte-identical afterwards, so nothing downstream can honestly tell a
 *    string from a bareword, and inventing the distinction here would be a
 *    guess presented as a fact.
 *
 * Both kinds stay in the wire format regardless: a remote core may have
 * real types, and the fake session already produces them.
 *
 * Depth is capped at kMaxTermValueDepth. Neither this walk nor
 * `resolveTermGrounded()` has a cycle guard, and a self-referential binding
 * would recurse until the stack ends; at the cap the node is emitted with
 * its own kind, no children, and `truncated` set. This is defence in depth
 * and NOT the value budget -- `applyBudget()` is a separate, later stage
 * with its own, much smaller limits, and this cap exists only so that a
 * malformed tree fails as a marked value instead of as a crash.
 *
 * @param pTerm
 *     The grounded term. NULL is tolerated (it cannot happen if the
 *     four-kind invariant holds) and yields an empty `Atom` marked
 *     `truncated`, so a caller sees "something is missing here" rather than
 *     an atom whose name happens to be empty.
 */
Value toSessionValue( const vault::unify::AbstractTerm* pTerm );

/**
 * The walk's hard recursion limit; see toSessionValue().
 *
 * 1000 rather than something tighter because it must not be reachable by
 * any term a program can legitimately produce -- a value the front end
 * actually shows has been through `applyBudget()`'s maxDepth of 8 long
 * before this matters -- and must still be far below the stack depth at
 * which the recursion itself would fail.
 */
const int kMaxTermValueDepth = 1000;

} // namespace session
} // namespace unify
} // namespace vault

#endif // _VAULT_UNIFY_TERM_VALUE_HPP
