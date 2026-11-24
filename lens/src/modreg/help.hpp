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

/**
 * One menu-bar heading: its name, the command that opens it, and the letter
 * its default `M-` binding uses.
 *
 * The letter is the DEFAULT only. What the bar underlines is derived from
 * the binding actually in effect (menuMnemonic), so rebinding `menu.window`
 * in the config moves the underline with it instead of leaving the bar
 * advertising a key that no longer works.
 */
struct MenuHeading {
    std::string name;
    std::string commandId;      //!< "menu.file", "menu.window", ...
    char        defaultLetter;  //!< lower case; `M-<letter>` opens it
};

/** The menu-bar headings, in the order UI.md section 1 draws them. */
std::vector<MenuHeading> menuHeadings();

/** The headings' names alone, in the same order. */
std::vector<std::string> menuCategories();

/**
 * Column of the heading's first letter in row 0.
 *
 * The bar's layout lives here, next to the table it lays out, because both
 * the bar and the drop-down anchored under it need it, and a drop-down that
 * computed its own column would drift from the heading it belongs to.
 */
int menuHeadingColumn( std::size_t index );

/**
 * Index into `heading.name` of the letter to underline, or -1.
 *
 * Read off the live binding: if the heading's command is bound to a single
 * `M-<letter>` and that letter occurs in the name, it is the first such
 * occurrence (case-insensitive). Otherwise nothing is underlined, because an
 * underline that no key answers to is a control that lies.
 */
int menuMnemonic( const MenuHeading& heading, const Keymap& keymap );

} // namespace lens

#endif // _LENS_MODREG_HELP_HPP
