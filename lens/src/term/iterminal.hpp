#if !defined( _LENS_TERM_ITERMINAL_HPP )
#define _LENS_TERM_ITERMINAL_HPP

/**
 * @file iterminal.hpp
 *
 * The seam the terminal library sits behind -- ARCHITECTURE.md section 6.1.
 *
 * Two methods, one value type in and one out. FTXUI appears only under
 * `src/term/`, behind this interface, and gate G1.5 greps for that. The
 * containment is the mitigation for having chosen FTXUI from its
 * documentation rather than from use: if it disappoints on Windows, or its
 * input decoding fights the design, swapping it for hand-rolled ANSI is a
 * change to one implementation file.
 *
 * THIS HEADER MUST NOT INCLUDE AN FTXUI HEADER. It is the boundary, not the
 * far side of it -- the same discipline, and the same reason, as
 * vault-unify-session.hpp.
 */

#include "../model/cell-grid.hpp"
#include "../modreg/keymap.hpp"

#include <memory>
#include <string>

namespace lens {

/** Something that happened at the terminal. */
struct TerminalEvent {
    enum class Kind {
        None,      //!< poll timed out; nothing happened
        Key,
        Resize,
        Paste,
        Closed     //!< the terminal went away; the app should exit
    };

    Kind        kind = Kind::None;
    Key         key;
    int         width = 0;    //!< Resize
    int         height = 0;   //!< Resize
    std::string text;         //!< Paste
};

/**
 * What the terminal can do -- ARCHITECTURE.md section 6.2's three tiers.
 *
 * Reported rather than assumed, and the model reacts to it: every panel must
 * be legible in the monochrome tier, so this changes how colour is resolved
 * and never what is drawn.
 */
struct TerminalCapabilities {
    enum class ColourTier {
        Monochrome,   //!< attributes only: bold, reverse, underline
        Ansi256,
        TrueColour
    };

    ColourTier colour = ColourTier::Monochrome;
    bool       unicode = true;
    int        width = 80;
    int        height = 24;
};

class ITerminal {
public:
    virtual ~ITerminal() = default;

    virtual TerminalCapabilities capabilities() const = 0;

    /** Present a frame. The grid is the whole screen. */
    virtual void draw( const CellGrid& grid ) = 0;

    /**
     * Wait up to `timeoutMs` for an event.
     *
     * Returns Kind::None on timeout, which is how the UI loop stays
     * responsive to session events arriving on its other input without
     * needing the terminal to know anything about them.
     */
    virtual TerminalEvent poll( int timeoutMs ) = 0;
};

/**
 * Build the real terminal, or return nothing with a reason.
 *
 * Failure is a first-class outcome here rather than an exception, because
 * one of its causes is expected and has a defined behaviour: under Git Bash
 * (mintty) a native console application gets no real console handle, and
 * ARCHITECTURE.md section 6.3 requires lens to print one line naming the
 * problem and the `winpty` workaround rather than drawing a broken screen.
 */
std::unique_ptr<ITerminal> makeTerminal( std::string& out_error );

} // namespace lens

#endif // _LENS_TERM_ITERMINAL_HPP
