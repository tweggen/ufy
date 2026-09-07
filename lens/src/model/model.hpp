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
#include "help-content.hpp"

#include <memory>

#include <string>
#include <vector>

namespace lens {

/**
 * Which panel a buffer is.
 *
 * The shell knows about tiles, not about what is in them -- UI.md section 3:
 * "none is privileged in the code". This enum is how a tile decides which
 * fold and which view to use, not a list of things the shell special-cases.
 */
enum class PanelKind {
    Placeholder,   //!< a named box, until the real panel arrives
    Help,
    Palette
};

/** The Help panel's own state (UI.md section 5.3). */
struct HelpState {
    std::string topicId;
    std::vector<std::string> history;   //!< for Backspace
    int cursor = 0;                     //!< line the cursor is on
    int scroll = 0;
};

/** The command palette's own state (UI.md section 4). */
struct PaletteState {
    std::string input;
    int selected = 0;
};

/** What a buffer is: a panel kind and the title it shows. */
struct Buffer {
    BufferId    id = kNoBuffer;
    PanelKind   kind = PanelKind::Placeholder;
    std::string title;

    /**
     * Placeholder body, for panels that have not arrived yet.
     *
     * Present so the shell can be built, gated and looked at before any
     * panel exists -- which is the whole point of G1 coming before G2.
     */
    std::vector<std::string> lines;

    HelpState    help;
    PaletteState palette;
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
                        const std::vector<std::string>& lines = {},
                        PanelKind kind = PanelKind::Placeholder );
    const Buffer* buffer( BufferId id ) const;
    Buffer*       buffer( BufferId id );

    /** The buffer shown in the focused tile, or null. */
    const Buffer* focusedBuffer() const;
    Buffer*       focusedBuffer();

    // -- help --------------------------------------------------------------

    /**
     * Rebuild the help book from the live command table and keymap.
     *
     * Called by the composition root once both are populated. Not built in
     * the constructor because the generated keymap page must reflect the
     * bindings that are actually in effect, and at construction there are
     * none.
     */
    void rebuildHelp();
    const HelpBook* helpBook() const { return m_helpBook.get(); }

    /**
     * Show `topicId` in a Help tile, creating or reusing one, and focus it.
     *
     * Reuses an existing Help tile rather than opening a second, because
     * `F1` pressed twice should answer twice, not fill the screen with help.
     */
    void openHelp( const std::string& topicId );

    /** The topic `F1` should open for whatever currently has focus. */
    std::string contextualTopicId() const;

    // -- the command palette ------------------------------------------------

    /**
     * Which way to split the focused tile so a new panel is readable.
     *
     * Columns when the focused tile is wide enough for both halves to clear
     * the solver's minimum, Rows otherwise. Without this, opening help while
     * a narrow catalogue has focus produces an 18-column help tile -- which
     * is legal, fits, and is useless, and "the help was unreadable" is a
     * worse failure than "the help took the space".
     */
    Split splitDirectionForNewPanel() const;

    void openPalette();
    void closePalette();
    bool paletteActive() const { return m_paletteTile != kNoTile; }

    /** Commands matching the palette's current input, in table order. */
    std::vector<const Command*> paletteMatches() const;

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

    std::unique_ptr<HelpBook> m_helpBook;

    /**
     * The tile the palette occupies while it is open, or kNoTile.
     *
     * A tile, not an overlay -- UI.md section 4. Dialogs obey the same
     * solver as everything else, so they cannot land off-screen or clip at
     * 80x24: a class of bug that simply does not arise.
     */
    TileId m_paletteTile = kNoTile;
    BufferId m_paletteBuffer = kNoBuffer;
    BufferId m_helpBuffer = kNoBuffer;
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
