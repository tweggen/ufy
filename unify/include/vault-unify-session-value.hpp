#if !defined( _VAULT_UNIFY_SESSION_VALUE_HPP )
#define _VAULT_UNIFY_SESSION_VALUE_HPP

/**
 * @file vault-unify-session-value.hpp
 *
 * Value-tree helpers shared by every Session implementation.
 *
 * Truncation and path lookup are not adapter details: `truncated` and
 * `ValuePath` are part of the contract (SESSION-API.md section 3), and if
 * LocalSession and FakeSession implemented them differently the contract
 * suite would be testing two different contracts and passing both. So the
 * budget rule lives here once, and `inspect` is defined as "walk the path,
 * re-apply the budget" for everyone.
 *
 * Depends on vault-unify-session.hpp and the standard library, nothing else.
 * It is NOT part of the boundary -- a front end never needs it -- so it is
 * a separate header rather than more surface on the one gate G0.1 guards.
 */

#include "vault-unify-session.hpp"

#include <cstdint>
#include <optional>

namespace vault {
namespace unify {
namespace session {

/**
 * Copy `value`, cutting it down to `budget` and marking every node that lost
 * something.
 *
 * The rules, stated because "truncate" has more than one reasonable meaning:
 *
 *  - Depth is counted from the value's own root, so a budget of 1 keeps the
 *    root's immediate children as truncated stubs, not the root alone.
 *  - `maxNodes` is a whole-tree budget spent breadth-first, so a wide shallow
 *    term degrades by losing its tail rather than by losing everything below
 *    its first argument. A depth-first spend would make the first branch of a
 *    100-argument term look complete and the rest vanish, which reads as data
 *    loss rather than as elision.
 *  - A node that lost children, arguments, pairs or string bytes is marked
 *    `truncated`. A node that lost nothing is not -- so `truncated` on the
 *    root means "there is more here", never merely "a budget was applied".
 */
Value applyBudget( const Value& value, const ValueBudget& budget );

/**
 * Follow `path` into `value`.
 *
 * Returns nothing if the path leaves the tree, which is a normal outcome:
 * a front end may hold a path built against a value it has since replaced,
 * and answering `Failed` beats crashing or silently returning the root.
 */
std::optional<Value> valueAtPath( const Value& value, const ValuePath& path );

/** Total node count of a value tree, for budgeting and for tests. */
std::uint64_t countNodes( const Value& value );

/** True if this node or any node beneath it is marked truncated. */
bool hasTruncation( const Value& value );

/**
 * Render a value the way the engine's `toString()` would -- used by the
 * transcript and by tests that want one comparable string rather than a
 * tree walk. Truncated nodes render as `...`.
 */
std::string toDisplayString( const Value& value );

} // namespace session
} // namespace unify
} // namespace vault

#endif // _VAULT_UNIFY_SESSION_VALUE_HPP
