#if !defined( _LENS_MODREG_HELP_HPP )
#define _LENS_MODREG_HELP_HPP

/**
 * @file help.hpp
 *
 * The help surfaces that are GENERATED from the command table -- UI.md
 * section 5, and the "H" criterion every gate carries.
 *
 * Two of UI.md's four help layers live here, and they are the two that can
 * be wrong without anyone noticing: the hint line and the keymap page. Both
 * are computed from the live command table and the live keymap on every
 * call, so neither can describe a binding that is not in effect. The
 * alternative -- a hand-written keymap page -- is wrong within a month, and
 * wrong in the specific way that teaches users not to trust the help.
 *
 * The other two layers (compiled-in topic pages, and builtin documentation
 * extracted from unify/LANGUAGE.md at build time) arrive with the Help
 * panel in a later gate.
 */

#include "command.hpp"
#include "keymap.hpp"

#include <string>
#include <vector>

namespace lens {

/** One entry of the hint line: the key as typed, and what it does. */
struct Hint {
    std::string key;
    std::string title;
    std::string commandId;
};

/**
 * The function-key hints, in F1..F12 order.
 *
 * Only function keys, because that is what the hint line of the IDEs this
 * borrows from showed, and what makes them learnable without documentation:
 * one row, always visible, never a mystery. Chords like `C-x 2` are
 * discoverable through `M-x` and the keymap page instead.
 */
std::vector<Hint> functionKeyHints( const CommandTable& table,
                                    const Keymap& keymap );

/**
 * Render hints into one line of exactly `width` columns.
 *
 * Truncates from the right when there is not enough room -- the low-numbered
 * function keys are the ones users reach for -- and never returns a blank
 * line while any command is bound, which is what "the hint line is never
 * blank in any golden" is asserted against.
 */
std::string renderHintLine( const std::vector<Hint>& hints, int width );

/**
 * The generated keymap page: every registered command, its binding, and its
 * help text.
 *
 * Generated at runtime from the live table, "so it cannot disagree with the
 * bindings" (UI.md section 5.3). The G1 criterion asserts that agreement
 * rather than eyeballing it: every command in the table appears here, and
 * every binding shown is the one the keymap actually holds.
 */
std::string generateKeymapPage( const CommandTable& table,
                                const Keymap& keymap );

/** The menu-bar headings, in the order UI.md section 1 draws them. */
std::vector<std::string> menuCategories();

} // namespace lens

#endif // _LENS_MODREG_HELP_HPP
