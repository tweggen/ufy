#if !defined( _LENS_MODEL_MODEL_HPP )
#define _LENS_MODEL_MODEL_HPP

/**
 * @file model.hpp
 *
 * The environment's state, and the pure state machine over it --
 * ARCHITECTURE.md section 4:
 *
 *     fold : (Model, Event) -> (Model, [Request])
 *     view : (Model, Geometry) -> CellGrid
 *
 * `fold` is total, free of I/O, and returns the requests it wants issued
 * rather than issuing them. `view` is pure. Together they mean everything
 * the environment can do has a deterministic test that needs no terminal, no
 * pty and no timing -- which is not a testing convenience bolted on
 * afterwards but the reason the layering is worth enforcing at all.
 *
 * This is the G1 shape of the model: tiles, buffers, focus, and the four
 * frame regions. The nine v1 panels (UI.md section 3) each add their own
 * sub-state here as they arrive, and each exposes the same two functions
 * over it.
 */

#include "../layout/layout-tree.hpp"
#include "../layout/solver.hpp"
#include "../modreg/command.hpp"
#include "../modreg/keymap.hpp"
#include "cell-grid.hpp"

#include <string>
#include <vector>

namespace lens {

/** What a buffer is: a panel kind and the title it shows. */
struct Buffer {
    BufferId    id = kNoBuffer;
    std::string title;

    /**
     * Placeholder body until the panels arrive.
     *
     * Present so the shell can be built, gated and looked at before any
     * panel exists -- which is the whole point of G1 coming before G2.
     */
    std::vector<std::string> lines;
};

/** Terminal events and session events, folded through one function. */
struct Event {
    enum class Kind {
        None,
        Key,
        Resize
    };

    Kind kind = Kind::None;
    Key  key;
    int  width = 0;
    int  height = 0;
};

class Model {
public:
    Model();

    // -- geometry ----------------------------------------------------------

    /** Below this, lens refuses to render at all (G1.4). */
    static constexpr int kMinWidth = 80;
    static constexpr int kMinHeight = 24;

    /** The pinned message G1.4 asserts, without a trailing newline. */
    static std::string tooSmallMessage( int width, int height );

    void setGeometry( int width, int height );
    int  width()  const { return m_width; }
    int  height() const { return m_height; }

    /** The tile area: everything but the menu bar, status line and hint line. */
    Rect tileArea() const;

    // -- content -----------------------------------------------------------

    LayoutTree&       layout()       { return m_layout; }
    const LayoutTree& layout() const { return m_layout; }

    BufferId addBuffer( const std::string& title,
                        const std::vector<std::string>& lines = {} );
    const Buffer* buffer( BufferId id ) const;

    CommandTable&       commands()       { return m_commands; }
    const CommandTable& commands() const { return m_commands; }

    Keymap&       keymap()       { return m_keymap; }
    const Keymap& keymap() const { return m_keymap; }

    /** The status line's right-hand context: session state, layout name. */
    void setStatus( const std::string& status ) { m_status = status; }
    const std::string& status() const { return m_status; }

    /**
     * Keys typed so far in an unfinished chord, e.g. `C-x` while waiting for
     * the `2`. Shown in the hint line, because a prefix that silently
     * swallowed the next keystroke is indistinguishable from a hang.
     */
    const KeySeq& pendingKeys() const { return m_pending; }

    /** True once a command has asked the app to exit. */
    bool quitting() const { return m_quitting; }
    void requestQuit() { m_quitting = true; }

    /** The last thing that could not be done, shown in the status line. */
    const std::string& message() const { return m_message; }
    void setMessage( const std::string& message ) { m_message = message; }

private:
    friend std::vector<CommandRequest> fold( Model&, const Event& );

    int m_width = 0;
    int m_height = 0;

    LayoutTree   m_layout;
    std::vector<Buffer> m_buffers;
    CommandTable m_commands;
    Keymap       m_keymap;

    std::string m_status;
    std::string m_message;
    KeySeq      m_pending;
    bool        m_quitting = false;
};

/**
 * Fold one event into the model, returning the requests it wants issued.
 *
 * Total: an unrecognised key is not an error, it is a key that is not bound.
 */
std::vector<CommandRequest> fold( Model& model, const Event& event );

/** Register the shell's own commands. Called once, by the composition root. */
void registerShellCommands( Model& model );

} // namespace lens

#endif // _LENS_MODEL_MODEL_HPP
