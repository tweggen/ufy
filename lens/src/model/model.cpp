/**
 * @file model.cpp
 */

#include "model.hpp"
#include "text-wrap.hpp"

#include <cctype>
#include <sstream>

namespace lens {

Model::Model()
    : m_layout( kNoBuffer )
{
}


std::string Model::tooSmallMessage( int width, int height )
{
    std::ostringstream os;
    os << "unify-lens: this terminal is " << width << "x" << height
       << "; lens needs at least " << kMinWidth << "x" << kMinHeight << ".\n"
       << "  Resize the window, or use `unify-run -i` for the plain REPL.";
    return os.str();
}


void Model::setGeometry( int width, int height )
{
    m_width = width;
    m_height = height;
}


Rect Model::tileArea() const
{
    /*
     * UI.md section 1: row 0 is the menu bar, row h-2 the status line, row
     * h-1 the hint line. Everything between is tiles.
     */
    const int h = m_height - 3;
    return Rect( 0, 1, m_width, h > 0 ? h : 0 );
}


BufferId Model::addBuffer( const std::string& title,
                           const std::vector<std::string>& lines,
                           PanelKind kind )
{
    Buffer buffer;
    buffer.id = (BufferId) ( m_buffers.size() + 1 );
    buffer.kind = kind;
    buffer.title = title;
    buffer.lines = lines;
    m_buffers.push_back( buffer );
    return buffer.id;
}


const Buffer* Model::buffer( BufferId id ) const
{
    for( std::size_t i = 0; i < m_buffers.size(); ++i ) {
        if( m_buffers[i].id == id ) {
            return &m_buffers[i];
        }
    }
    return NULL;
}


Buffer* Model::buffer( BufferId id )
{
    for( std::size_t i = 0; i < m_buffers.size(); ++i ) {
        if( m_buffers[i].id == id ) {
            return &m_buffers[i];
        }
    }
    return NULL;
}


const Buffer* Model::focusedBuffer() const
{
    return buffer( m_layout.bufferOf( m_layout.focused() ) );
}


Buffer* Model::focusedBuffer()
{
    return buffer( m_layout.bufferOf( m_layout.focused() ) );
}


void Model::rebuildHelp()
{
    m_helpBook.reset( new HelpBook( m_commands, m_keymap ) );
}


std::string Model::contextualTopicId() const
{
    /*
     * UI.md section 5.2: F1 opens the topic for whatever has focus, and
     * "never does nothing and never opens a table of contents when it could
     * open the answer". So a panel's own page wins, and getting-started is
     * the floor rather than the usual answer.
     */
    const Buffer* focused = focusedBuffer();
    if( !focused ) {
        return HelpBook::defaultTopicId();
    }

    switch( focused->kind ) {
    case PanelKind::Help:       return "help";
    case PanelKind::Palette:    return "keys";
    case PanelKind::Menu:       return "keys";
    case PanelKind::Transcript: return "getting-started";
    case PanelKind::Placeholder:
        break;
    }

    /*
     * A placeholder panel has a per-panel page once the panel exists; until
     * then the tiling page is the most useful true answer, since what the
     * user can actually do with that tile is move and split it.
     */
    if( m_helpBook ) {
        const std::string byTitle = focused->title;
        std::string lowered;
        for( std::size_t i = 0; i < byTitle.size(); ++i ) {
            lowered += (char) std::tolower( (unsigned char) byTitle[i] );
        }
        if( m_helpBook->topic( lowered ) ) {
            return lowered;
        }
    }
    return "panels";
}


const Buffer* Model::transcript() const
{
    for( std::size_t i = 0; i < m_buffers.size(); ++i ) {
        if( m_buffers[i].kind == PanelKind::Transcript ) {
            return &m_buffers[i];
        }
    }
    return NULL;
}


Buffer* Model::transcript()
{
    for( std::size_t i = 0; i < m_buffers.size(); ++i ) {
        if( m_buffers[i].kind == PanelKind::Transcript ) {
            return &m_buffers[i];
        }
    }
    return NULL;
}


void Model::foldSession( const us::Event& event )
{
    Buffer* buffer = transcript();
    if( !buffer ) {
        return;
    }
    TranscriptState& t = buffer->transcript;

    if( const us::Solution* solution = std::get_if<us::Solution>( &event.body ) ) {
        TranscriptEntry entry;
        entry.kind = TranscriptEntry::Kind::Solution;
        entry.query = event.header.query ? *event.header.query : us::kNoQuery;
        entry.text = renderBindings( solution->bindings );
        if( entry.text.empty() ) {
            /*
             * A goal with no variables succeeded. `unify-run` prints nothing
             * per solution and only a count; here the row still has to exist
             * or the solutions would be invisible, so it says what happened.
             */
            entry.text = "yes";
        }
        t.entries.push_back( entry );
        ++t.produced;
        return;
    }

    if( const us::QueryStatus* status =
            std::get_if<us::QueryStatus>( &event.body ) ) {
        const bool terminal = status->state != us::QueryStatus::State::Running
                           && status->state != us::QueryStatus::State::Blocked;
        if( !terminal ) {
            return;
        }

        t.finished = true;
        t.liveQuery = us::kNoQuery;

        TranscriptEntry entry;
        entry.kind = TranscriptEntry::Kind::Status;
        entry.query = event.header.query ? *event.header.query : us::kNoQuery;

        std::ostringstream text;
        switch( status->state ) {
        case us::QueryStatus::State::Exhausted:
        case us::QueryStatus::State::Complete:
            if( 0 == status->produced ) {
                text << "-- no solutions";
            } else if( 1 == status->produced ) {
                text << "-- 1 solution";
            } else {
                text << "-- " << status->produced << " solutions";
            }
            break;
        case us::QueryStatus::State::Failed:
            text << "-- failed";
            if( !status->detail.empty() ) { text << ": " << status->detail; }
            break;
        case us::QueryStatus::State::Aborted:
            text << "-- aborted";
            if( !status->detail.empty() ) { text << " (" << status->detail << ")"; }
            break;
        default:
            text << "-- done";
            break;
        }
        entry.text = text.str();
        t.entries.push_back( entry );
        return;
    }

    if( const us::Output* output = std::get_if<us::Output>( &event.body ) ) {
        /*
         * Engine item E4's user-visible proof (G2.3): program output arrives
         * as an EVENT and lands here, not on lens's stdout. Split on
         * newlines because `print` ends every line with one and a transcript
         * entry is a line.
         */
        std::string text = output->text;
        std::string line;
        for( std::size_t i = 0; i < text.size(); ++i ) {
            if( text[i] == '\n' ) {
                TranscriptEntry entry;
                entry.kind = TranscriptEntry::Kind::Output;
                entry.query = event.header.query ? *event.header.query : us::kNoQuery;
                entry.text = line;
                t.entries.push_back( entry );
                line.clear();
            } else {
                line += text[i];
            }
        }
        if( !line.empty() ) {
            TranscriptEntry entry;
            entry.kind = TranscriptEntry::Kind::Output;
            entry.query = event.header.query ? *event.header.query : us::kNoQuery;
            entry.text = line;
            t.entries.push_back( entry );
        }

        if( output->droppedBytes > 0 ) {
            TranscriptEntry elided;
            elided.kind = TranscriptEntry::Kind::Info;
            std::ostringstream os;
            os << "… " << output->droppedBytes << " bytes elided";
            elided.text = os.str();
            t.entries.push_back( elided );
        }
        return;
    }

    if( const us::Diagnostic* diagnostic =
            std::get_if<us::Diagnostic>( &event.body ) ) {
        const std::vector<TranscriptEntry> rendered =
            renderDiagnostic( *diagnostic );
        for( std::size_t i = 0; i < rendered.size(); ++i ) {
            TranscriptEntry entry = rendered[i];
            entry.query = event.header.query ? *event.header.query : us::kNoQuery;
            t.entries.push_back( entry );
        }
        return;
    }

    if( const us::Defined* defined = std::get_if<us::Defined>( &event.body ) ) {
        if( defined->errorCount > 0 ) {
            /* The diagnostics themselves already arrived as their own
             * events; this is the summary, so the count is never silent. */
            TranscriptEntry entry;
            entry.kind = TranscriptEntry::Kind::Status;
            std::ostringstream os;
            os << "-- " << defined->errorCount
               << ( defined->errorCount == 1 ? " error" : " errors" );
            entry.text = os.str();
            t.entries.push_back( entry );
            return;
        }

        std::ostringstream os;
        const std::size_t added = defined->added.size();
        const std::size_t replaced = defined->replaced.size();
        if( 0 == added && 0 == replaced ) {
            return;
        }
        os << "-- defined";
        for( std::size_t i = 0; i < defined->added.size(); ++i ) {
            os << " " << defined->added[i].name << "/" << defined->added[i].arity;
        }
        for( std::size_t i = 0; i < defined->replaced.size(); ++i ) {
            os << " " << defined->replaced[i].name << "/"
               << defined->replaced[i].arity << " (replaced)";
        }

        TranscriptEntry entry;
        entry.kind = TranscriptEntry::Kind::Status;
        entry.text = os.str();
        t.entries.push_back( entry );
        return;
    }

    if( const us::Failed* failed = std::get_if<us::Failed>( &event.body ) ) {
        TranscriptEntry entry;
        entry.kind = TranscriptEntry::Kind::Diagnostic;
        entry.text = std::string( "-- " ) + failed->reason;
        t.entries.push_back( entry );
        if( failed->fatal ) {
            m_message = "the session has failed: " + failed->reason;
        }
        return;
    }
}


std::string Model::sessionStatus() const
{
    const Buffer* buffer = transcript();

    std::ostringstream os;
    os << "gen 0 \xc2\xb7 ";

    if( buffer && !buffer->transcript.finished ) {
        os << "running \xc2\xb7 " << buffer->transcript.produced << " so far";
    } else {
        os << "idle";
    }

    /*
     * G2.7: say "buffered" while engine item E11 is outstanding, rather than
     * implying flow control that does not exist. The core reports this
     * itself through describe(), so the day E11 lands the status line stops
     * saying it without anyone editing this line.
     */
    os << " \xc2\xb7 " << ( m_caps.realDemand ? "demand" : "buffered" );
    if( !m_caps.realCancel ) {
        os << " \xc2\xb7 cancel detaches only";
    }
    os << " \xc2\xb7 " << ( m_caps.coreName.empty() ? "no session"
                                              : m_caps.location );
    return os.str();
}


Rect Model::innerRectOf( TileId tile ) const
{
    const Solution solution = solve( m_layout, tileArea() );
    const Placement* placement = solution.find( tile );
    if( !placement || placement->stub ) {
        return Rect();
    }
    /* One cell of border on every side. */
    return Rect( placement->rect.x + 1, placement->rect.y + 1,
                 placement->rect.w - 2, placement->rect.h - 2 );
}


ScrollView Model::observeFocusedScroll( bool& out_valid ) const
{
    out_valid = false;
    ScrollView view;

    const Buffer* buffer = focusedBuffer();
    if( !buffer ) {
        return view;
    }

    const Rect inner = innerRectOf( m_layout.focused() );
    if( inner.w <= 0 || inner.h <= 0 ) {
        return view;
    }

    switch( buffer->kind ) {
    case PanelKind::Help: {
        const HelpTopic* topic =
            m_helpBook ? m_helpBook->topic( buffer->help.topicId ) : NULL;
        if( !topic ) { return view; }

        /* Rendered rows, matching followCursor() and the renderer. If these
         * three disagreed the tests would assert something the user never
         * sees, which is worse than not testing at all. */
        const WrapMap map = wrapMap( topic->lines, inner.w );
        view.cursorLine =
            ( buffer->help.cursor >= 0
              && buffer->help.cursor < (int) map.rowOfLine.size() )
                ? map.rowOfLine[ (std::size_t) buffer->help.cursor ] : 0;
        view.topLine = buffer->help.top;
        view.viewportRows = inner.h;
        view.totalLines = map.totalRows;
        out_valid = true;
        return view;
    }
    case PanelKind::Menu: {
        view.cursorLine = buffer->menu.selected;
        view.topLine = buffer->menu.top;
        view.viewportRows = inner.h;
        view.totalLines = (int) menuRows().size();
        out_valid = true;
        return view;
    }
    case PanelKind::Palette: {
        view.cursorLine = buffer->palette.selected;
        view.topLine = buffer->palette.top;
        /* One row is the prompt; the rest is the list. */
        view.viewportRows = inner.h - 1;
        view.totalLines = (int) paletteMatches().size();
        out_valid = true;
        return view;
    }
    case PanelKind::Placeholder:
    case PanelKind::Transcript:
        break;
    }
    return view;
}


Split Model::splitDirectionForNewPanel() const
{
    const Solution solution = solve( m_layout, tileArea() );
    const Placement* focused = solution.find( m_layout.focused() );
    if( !focused ) {
        return Split::Columns;
    }

    /*
     * Chosen for READABILITY, not for fitting.
     *
     * The obvious rule -- split into columns whenever both halves clear the
     * solver's 20-column minimum -- produces panels that are legal and
     * unreadable: help beside a 84-column Source gives 51 columns, and the
     * help text is written for about 70. So a column split has to leave the
     * new panel genuinely wide enough for prose; otherwise a row split gives
     * it the full width and takes the height instead, which for text is the
     * better trade every time.
     */
    static const int kComfortableTextWidth = 60;
    const int wouldGet = (int) ( focused->rect.w * 0.62 );
    if( wouldGet >= kComfortableTextWidth ) {
        return Split::Columns;
    }
    return Split::Rows;
}


TileId Model::largestTile() const
{
    const Solution solution = solve( m_layout, tileArea() );

    TileId best = m_layout.focused();
    int bestArea = -1;
    for( std::size_t i = 0; i < solution.placements.size(); ++i ) {
        const Placement& placement = solution.placements[i];
        if( placement.stub ) {
            continue;
        }
        const int area = placement.rect.area();
        if( area > bestArea ) {
            bestArea = area;
            best = placement.tile;
        }
    }
    return best;
}


bool Model::returnBorrowedTile()
{
    if( kNoTile == m_borrowedTile || kNoBuffer == m_displacedBuffer ) {
        return false;
    }
    if( m_layout.focused() != m_borrowedTile ) {
        return false;
    }

    m_layout.setBuffer( m_borrowedTile, m_displacedBuffer );
    m_borrowedTile = kNoTile;
    m_displacedBuffer = kNoBuffer;
    return true;
}


void Model::openHelp( const std::string& topicId, bool takeLargestTile )
{
    if( kNoBuffer == m_helpBuffer ) {
        m_helpBuffer = addBuffer( "Help", {}, PanelKind::Help );
    }

    Buffer* help = buffer( m_helpBuffer );
    if( help ) {
        if( !help->help.topicId.empty() && help->help.topicId != topicId ) {
            help->help.history.push_back( help->help.topicId );
        }
        help->help.topicId = topicId;
        help->help.cursor = 0;
        help->help.top = 0;
    }

    /* Reuse the tile already showing help, if there is one. */
    const std::vector<TileId> tiles = m_layout.tiles();
    for( std::size_t i = 0; i < tiles.size(); ++i ) {
        if( m_layout.bufferOf( tiles[i] ) == m_helpBuffer ) {
            m_layout.focus( tiles[i] );
            return;
        }
    }

    /*
     * Beside the work rather than over it -- the whole argument for tiling
     * (UI.md section 5.3) -- but BIG. Help that arrives as a sliver is help
     * nobody reads, and the first person to run lens said so.
     *
     * Two decisions make it big and keep it predictable. It splits the
     * LARGEST tile rather than whichever happened to have focus, so opening
     * help from a narrow catalogue does not produce a narrow help; and it
     * takes the larger share of it, so it lands about the size of the main
     * working area rather than half of something small.
     */
    const TileId host = largestTile();

    if( takeLargestTile ) {
        /*
         * Borrow it outright. The welcome page is the main thing on screen
         * when it is on screen, so it gets the main area rather than a share
         * of it -- and C-x 0 hands the tile back (returnBorrowedTile).
         */
        m_displacedBuffer = m_layout.bufferOf( host );
        m_borrowedTile = host;
        m_layout.setBuffer( host, m_helpBuffer );
        m_layout.focus( host );
        return;
    }

    m_layout.focus( host );
    m_layout.splitFocused( splitDirectionForNewPanel(), m_helpBuffer, 0.38 );
}


void Model::openMenu()
{
    if( menuActive() ) {
        return;
    }
    if( kNoBuffer == m_menuBuffer ) {
        m_menuBuffer = addBuffer( "Menu", {}, PanelKind::Menu );
    }
    Buffer* menu = buffer( m_menuBuffer );
    if( menu ) {
        menu->menu.selected = 0;
        /* Start on the first real command, not on the File heading. */
        const std::vector<MenuRow> rows = menuRows();
        for( std::size_t i = 0; i < rows.size(); ++i ) {
            if( !rows[i].isHeading() ) {
                menu->menu.selected = (int) i;
                break;
            }
        }
    }

    m_menuTile = m_layout.splitFocused( splitDirectionForNewPanel(),
                                        m_menuBuffer, 0.5 );
}


void Model::closeMenu()
{
    if( !menuActive() ) {
        return;
    }
    if( m_layout.focus( m_menuTile ) ) {
        m_layout.closeFocused();
    }
    m_menuTile = kNoTile;
}


std::vector<Model::MenuRow> Model::menuRows() const
{
    std::vector<MenuRow> rows;

    const std::vector<std::string> categories = menuCategories();
    const std::vector<Command>& all = m_commands.all();

    for( std::size_t c = 0; c < categories.size(); ++c ) {
        MenuRow heading;
        heading.heading = categories[c];
        rows.push_back( heading );

        for( std::size_t i = 0; i < all.size(); ++i ) {
            if( all[i].category() != categories[c] ) {
                continue;
            }
            MenuRow row;
            row.command = &all[i];
            rows.push_back( row );
        }
    }
    return rows;
}


void Model::openPalette()
{
    if( paletteActive() ) {
        return;
    }

    if( kNoBuffer == m_paletteBuffer ) {
        m_paletteBuffer = addBuffer( "M-x", {}, PanelKind::Palette );
    }
    Buffer* palette = buffer( m_paletteBuffer );
    if( palette ) {
        palette->palette.input.clear();
        palette->palette.selected = 0;
    }

    /*
     * A tile, not an overlay. It goes below the focused tile so the work
     * stays visible above it, and at a small geometry the solver folds
     * something else rather than clipping the dialog -- which is why UI.md
     * makes dialogs tiles in the first place.
     */
    m_paletteTile =
        m_layout.splitFocused( splitDirectionForNewPanel(), m_paletteBuffer, 0.5 );
}


void Model::closePalette()
{
    if( !paletteActive() ) {
        return;
    }
    if( m_layout.focus( m_paletteTile ) ) {
        m_layout.closeFocused();
    }
    m_paletteTile = kNoTile;
}


std::vector<const Command*> Model::paletteMatches() const
{
    std::vector<const Command*> matches;

    const Buffer* palette = buffer( m_paletteBuffer );
    std::string needle = palette ? palette->palette.input : std::string();
    for( std::size_t i = 0; i < needle.size(); ++i ) {
        needle[i] = (char) std::tolower( (unsigned char) needle[i] );
    }

    const std::vector<Command>& all = m_commands.all();
    for( std::size_t i = 0; i < all.size(); ++i ) {
        if( needle.empty() ) {
            matches.push_back( &all[i] );
            continue;
        }

        std::string haystack = all[i].id() + " " + all[i].title();
        for( std::size_t k = 0; k < haystack.size(); ++k ) {
            haystack[k] = (char) std::tolower( (unsigned char) haystack[k] );
        }
        if( haystack.find( needle ) != std::string::npos ) {
            matches.push_back( &all[i] );
        }
    }
    return matches;
}


namespace {

/** Defined below; used by every panel's key handler. */
void followCursor( Model& model, Buffer& buffer );


bool isCancel( const Key& key )
{
    /* UI.md section 4: Esc cancels the innermost thing, C-g aborts. */
    return key.code == Key::Code::Escape
        || ( key.code == Key::Code::Char && key.ctrl && key.ch == U'g' );
}


/**
 * The palette's keys. Modal on purpose: while a minibuffer is open, almost
 * every key is input, and a global binding firing out from under someone who
 * is typing a command name is the classic minibuffer bug.
 */
std::vector<CommandRequest> foldPalette( Model& model, Buffer& palette,
                                         const Key& key )
{
    std::vector<CommandRequest> requests;
    const std::vector<const Command*> matches = model.paletteMatches();

    if( isCancel( key ) ) {
        model.closePalette();
        return requests;
    }

    if( key.code == Key::Code::Enter ) {
        const Command* chosen = NULL;
        if( palette.palette.selected >= 0
            && palette.palette.selected < (int) matches.size() ) {
            chosen = matches[ (std::size_t) palette.palette.selected ];
        }

        /*
         * Closed BEFORE the command runs. A command that changes the layout
         * -- and most of them do -- would otherwise be operating with the
         * palette's own tile focused, and would split or close that instead
         * of the tile the user was looking at.
         */
        model.closePalette();

        if( chosen ) {
            if( !chosen->enabled( model ) ) {
                model.setMessage( chosen->title() + " is not available here" );
                return requests;
            }
            return chosen->run( model );
        }
        model.setMessage( "no command matches" );
        return requests;
    }

    if( key.code == Key::Code::Up ) {
        if( palette.palette.selected > 0 ) { --palette.palette.selected; }
        followCursor( model, palette );
        return requests;
    }
    if( key.code == Key::Code::Down ) {
        if( palette.palette.selected + 1 < (int) matches.size() ) {
            ++palette.palette.selected;
        }
        followCursor( model, palette );
        return requests;
    }

    if( key.code == Key::Code::Backspace ) {
        if( !palette.palette.input.empty() ) {
            palette.palette.input.resize( palette.palette.input.size() - 1 );
            palette.palette.selected = 0;
            palette.palette.top = 0;
        }
        return requests;
    }

    if( key.code == Key::Code::Tab ) {
        /* Complete to the longest common prefix of the matching ids. */
        if( !matches.empty() ) {
            std::string prefix = matches[0]->id();
            for( std::size_t i = 1; i < matches.size(); ++i ) {
                const std::string& other = matches[i]->id();
                std::size_t k = 0;
                while( k < prefix.size() && k < other.size()
                       && prefix[k] == other[k] ) {
                    ++k;
                }
                prefix.resize( k );
            }
            if( prefix.size() > palette.palette.input.size() ) {
                palette.palette.input = prefix;
                palette.palette.selected = 0;
                palette.palette.top = 0;
            }
        }
        return requests;
    }

    if( key.code == Key::Code::Char && !key.ctrl && !key.alt && key.ch >= 0x20 ) {
        palette.palette.input += encodeUtf8( key.ch );
        palette.palette.selected = 0;
        palette.palette.top = 0;
        return requests;
    }

    return requests;
}


/**
 * Bring the focused panel's viewport back onto its cursor.
 *
 * Called after every key that moves a selection. `top` is model state, not
 * a number the renderer invents, precisely so this can be MINIMAL: it needs
 * to know where the view was in order to decide whether it has to move at
 * all. See scroll.hpp for what happens when it cannot.
 */
void followCursor( Model& model, Buffer& buffer )
{
    const Rect inner = model.innerRectOf( model.layout().focused() );
    if( inner.w <= 0 || inner.h <= 0 ) {
        return;
    }

    switch( buffer.kind ) {
    case PanelKind::Help: {
        const HelpBook* book = model.helpBook();
        const HelpTopic* topic = book ? book->topic( buffer.help.topicId ) : NULL;
        if( !topic ) { return; }

        /*
         * In SCREEN rows, not source lines: a wrapped line is three rows
         * tall, and a viewport measured in source lines would let the
         * cursor slide off the bottom of a topic full of long paragraphs.
         */
        const WrapMap map = wrapMap( topic->lines, inner.w );
        const int cursorRow =
            ( buffer.help.cursor >= 0
              && buffer.help.cursor < (int) map.rowOfLine.size() )
                ? map.rowOfLine[ (std::size_t) buffer.help.cursor ] : 0;

        buffer.help.top = ensureVisible( cursorRow, map.totalRows, inner.h,
                                         buffer.help.top );
        return;
    }
    case PanelKind::Menu:
        buffer.menu.top = ensureVisible( buffer.menu.selected,
                                         (int) model.menuRows().size(),
                                         inner.h, buffer.menu.top );
        return;
    case PanelKind::Palette:
        /* One row is the prompt; the rest is the list. */
        buffer.palette.top = ensureVisible( buffer.palette.selected,
                                            (int) model.paletteMatches().size(),
                                            inner.h - 1, buffer.palette.top );
        return;
    case PanelKind::Placeholder:
    case PanelKind::Transcript:
        return;
    }
}


/** The menu's keys. Modal while open, exactly like the palette. */
std::vector<CommandRequest> foldMenu( Model& model, Buffer& menu, const Key& key )
{
    std::vector<CommandRequest> requests;
    const std::vector<Model::MenuRow> rows = model.menuRows();

    if( isCancel( key ) || key.code == Key::Code::F10 ) {
        /* F10 closes as well as opens: a menu key that only opened would
         * make the menu a trap for anyone who pressed it by accident. */
        model.closeMenu();
        return requests;
    }

    const auto step = [ & ]( int direction ) {
        int at = menu.menu.selected;
        for( int guard = 0; guard < (int) rows.size(); ++guard ) {
            at += direction;
            if( at < 0 || at >= (int) rows.size() ) {
                return;   /* stop at the ends rather than wrapping past a
                           * heading, which reads as skipping an entry */
            }
            if( !rows[ (std::size_t) at ].isHeading() ) {
                menu.menu.selected = at;
                return;
            }
        }
    };

    if( key.code == Key::Code::Down ) {
        step( 1 );
        followCursor( model, menu );
        return requests;
    }
    if( key.code == Key::Code::Up ) {
        step( -1 );
        followCursor( model, menu );
        return requests;
    }

    if( key.code == Key::Code::Enter ) {
        const Command* chosen = NULL;
        if( menu.menu.selected >= 0 && menu.menu.selected < (int) rows.size() ) {
            chosen = rows[ (std::size_t) menu.menu.selected ].command;
        }

        /* Closed before the command runs, for the same reason the palette
         * is: otherwise a layout command operates on the menu's own tile. */
        model.closeMenu();

        if( chosen ) {
            if( !chosen->enabled( model ) ) {
                model.setMessage( chosen->title() + " is not available here" );
                return requests;
            }
            return chosen->run( model );
        }
        return requests;
    }

    return requests;
}


/** The Help panel's keys: a small hypertext browser. */
void foldHelp( Model& model, Buffer& help, const Key& key )
{
    const HelpBook* book = model.helpBook();
    if( !book ) {
        return;
    }
    const HelpTopic* topic = book->topic( help.help.topicId );
    const int lineCount = topic ? (int) topic->lines.size() : 0;

    if( key.code == Key::Code::Down ) {
        if( help.help.cursor + 1 < lineCount ) { ++help.help.cursor; }
        followCursor( model, help );
        return;
    }
    if( key.code == Key::Code::Up ) {
        if( help.help.cursor > 0 ) { --help.help.cursor; }
        followCursor( model, help );
        return;
    }

    if( key.code == Key::Code::Backspace ) {
        if( help.help.history.empty() ) {
            model.setMessage( "no earlier help topic" );
            return;
        }
        help.help.topicId = help.help.history.back();
        help.help.history.pop_back();
        help.help.cursor = 0;
        help.help.top = 0;
        return;
    }

    if( key.code == Key::Code::Enter ) {
        if( !topic || help.help.cursor >= lineCount ) {
            return;
        }
        const std::vector<std::string> links =
            HelpBook::linksOn( topic->lines[ (std::size_t) help.help.cursor ] );
        if( links.empty() ) {
            model.setMessage( "no link on this line" );
            return;
        }
        if( !book->topic( links[0] ) ) {
            /* A dead link is a bug in the help, and says so rather than
             * doing nothing -- silence is how dead links survive. */
            model.setMessage( "help topic '" + links[0] + "' does not exist" );
            return;
        }
        help.help.history.push_back( help.help.topicId );
        help.help.topicId = links[0];
        help.help.cursor = 0;
        help.help.top = 0;
        return;
    }
}

/**
 * The transcript's keys.
 *
 * Most keys in a transcript are text, so this runs for anything the global
 * keymap did not claim -- which is why the shell binds chords and function
 * keys and leaves the alphabet alone.
 */
std::vector<CommandRequest> foldTranscript( Model& model, Buffer& buffer,
                                            const Key& key )
{
    std::vector<CommandRequest> requests;
    TranscriptState& t = buffer.transcript;

    if( key.code == Key::Code::Enter ) {
        const std::string line = t.input;
        if( line.find_first_not_of( " \t" ) == std::string::npos ) {
            t.input.clear();
            t.cursor = 0;
            return requests;
        }

        return submitTranscriptLine( model, buffer, line );
    }

    if( key.code == Key::Code::Backspace ) {
        if( t.cursor > 0 ) {
            /* Back over a whole codepoint, not a byte: deleting half of a
             * multi-byte character leaves the buffer holding invalid UTF-8. */
            std::size_t at = t.cursor - 1;
            while( at > 0 && ( (unsigned char) t.input[at] & 0xC0 ) == 0x80 ) {
                --at;
            }
            t.input.erase( at, t.cursor - at );
            t.cursor = at;
        }
        return requests;
    }

    if( key.code == Key::Code::Delete ) {
        if( t.cursor < t.input.size() ) {
            std::size_t end = t.cursor + 1;
            while( end < t.input.size()
                   && ( (unsigned char) t.input[end] & 0xC0 ) == 0x80 ) {
                ++end;
            }
            t.input.erase( t.cursor, end - t.cursor );
        }
        return requests;
    }

    if( key.code == Key::Code::Left ) {
        while( t.cursor > 0 ) {
            --t.cursor;
            if( ( (unsigned char) t.input[t.cursor] & 0xC0 ) != 0x80 ) { break; }
        }
        return requests;
    }
    if( key.code == Key::Code::Right ) {
        while( t.cursor < t.input.size() ) {
            ++t.cursor;
            if( t.cursor >= t.input.size()
                || ( (unsigned char) t.input[t.cursor] & 0xC0 ) != 0x80 ) {
                break;
            }
        }
        return requests;
    }
    if( key.code == Key::Code::Home ) { t.cursor = 0; return requests; }
    if( key.code == Key::Code::End ) { t.cursor = t.input.size(); return requests; }

    if( key.code == Key::Code::Up ) {
        if( t.historyIndex > 0 ) {
            --t.historyIndex;
            t.input = t.history[ t.historyIndex ];
            t.cursor = t.input.size();
        }
        return requests;
    }
    if( key.code == Key::Code::Down ) {
        if( t.historyIndex < t.history.size() ) {
            ++t.historyIndex;
            t.input = ( t.historyIndex < t.history.size() )
                          ? t.history[ t.historyIndex ]
                          : std::string();
            t.cursor = t.input.size();
        }
        return requests;
    }

    if( key.code == Key::Code::Char && !key.ctrl && !key.alt && key.ch >= 0x20 ) {
        const std::string encoded = encodeUtf8( key.ch );
        t.input.insert( t.cursor, encoded );
        t.cursor += encoded.size();
        return requests;
    }

    return requests;
}

} // namespace

std::vector<CommandRequest> submitTranscriptLine( Model& model, Buffer& buffer,
                                                  const std::string& line )
{
    ( void ) model;   /* the transcript's own state is all this needs today */

    std::vector<CommandRequest> requests;
    TranscriptState& t = buffer.transcript;

    TranscriptEntry echo;
    echo.kind = TranscriptEntry::Kind::Input;
    echo.text = std::string( kTranscriptPrompt ) + line;
    echo.source = line;
    t.entries.push_back( echo );

    /* Duplicate consecutive entries are not worth remembering. */
    if( t.history.empty() || t.history.back() != line ) {
        t.history.push_back( line );
    }
    t.historyIndex = t.history.size();
    t.input.clear();
    t.cursor = 0;

    /*
     * One ingestion path, two shapes. A line ending in `;` or containing
     * `{` is a definition; anything else is a goal. That is a heuristic, and
     * it is the same one the existing REPL uses -- `define` and `solve` are
     * the same operation with different intent (SESSION-API section 2.1),
     * and getting it wrong costs a diagnostic rather than a wrong answer.
     */
    CommandRequest request;
    const bool looksLikeDefinition =
        line.find( '{' ) != std::string::npos
        || ( !line.empty() && line[ line.size() - 1 ] == ';' );

    request.kind = looksLikeDefinition ? "define" : "solve";
    request.text = line;
    requests.push_back( request );

    if( !looksLikeDefinition ) {
        t.finished = false;
        t.produced = 0;
    }
    return requests;
}


std::vector<CommandRequest> Model::foldInner( Model& model,
                                              const Event& event )
{
    std::vector<CommandRequest> requests;

    switch( event.kind ) {
    case Event::Kind::None:
        return requests;

    case Event::Kind::Resize:
        model.setGeometry( event.width, event.height );
        return requests;

    case Event::Kind::Key:
        break;
    }

    model.m_message.clear();

    /*
     * The palette is modal while it is open. Nothing else gets a look at the
     * key -- see foldPalette().
     */
    if( model.paletteActive() ) {
        Buffer* palette = model.buffer( model.m_paletteBuffer );
        if( palette ) {
            return foldPalette( model, *palette, event.key );
        }
    }
    if( model.menuActive() ) {
        Buffer* menu = model.buffer( model.m_menuBuffer );
        if( menu ) {
            return foldMenu( model, *menu, event.key );
        }
    }

    KeySeq attempt = model.m_pending;
    attempt.push_back( event.key );

    const std::string commandId = model.m_keymap.lookup( attempt );
    if( !commandId.empty() ) {
        model.m_pending.clear();

        const Command* command = model.m_commands.find( commandId );
        if( !command ) {
            /*
             * Bound to something nobody registered -- a typo in the config.
             * Said out loud, for the same reason the hint line says it: a
             * key that silently does nothing is indistinguishable from a
             * key that is broken.
             */
            model.setMessage( "no such command: " + commandId );
            return requests;
        }
        if( !command->enabled( model ) ) {
            model.setMessage( command->title() + " is not available here" );
            return requests;
        }
        return command->run( model );
    }

    if( model.m_keymap.isPrefix( attempt ) ) {
        /* An unfinished chord. Keep reading; the hint line shows what is held. */
        model.m_pending = attempt;
        return requests;
    }

    if( !model.m_pending.empty() ) {
        model.setMessage( toString( attempt ) + " is not bound" );
        model.m_pending.clear();
        return requests;
    }

    /*
     * Unbound, and no chord in progress: the focused panel gets it. This is
     * the general rule -- the shell binds what is global, and everything
     * else is the panel's input -- rather than a special case for help.
     */
    Buffer* focused = model.focusedBuffer();
    if( focused && focused->kind == PanelKind::Help ) {
        foldHelp( model, *focused, event.key );
        return requests;
    }
    if( focused && focused->kind == PanelKind::Transcript ) {
        return foldTranscript( model, *focused, event.key );
    }

    /*
     * An unbound key in an ordinary panel is not an error and gets no
     * message: in a transcript most keys are text, and complaining about
     * every one of them would make the status line useless.
     */
    return requests;
}


std::vector<CommandRequest> fold( Model& model, const Event& event )
{
    const std::vector<CommandRequest> requests =
        Model::foldInner( model, event );

    /*
     * Re-follow the cursor on the way out, whatever the key did.
     *
     * Moving a selection is not the only thing that can put the cursor off
     * screen: splitting a tile halves the viewport, closing one doubles it,
     * a resize changes it, and moving focus lands on a panel whose stored
     * viewport was computed for a different geometry. Handling those
     * one by one is how three of them get forgotten -- so it happens once,
     * here, at the single exit.
     *
     * Found by the interaction suite's random walk: `C-x 2` with the help
     * cursor at row 4 left it off the bottom of a 3-row tile, and no
     * hand-written case would have thought to split a tile while a help
     * page was scrolled.
     */
    Buffer* focused = model.focusedBuffer();
    if( focused ) {
        followCursor( model, *focused );
    }
    return requests;
}


void registerShellCommands( Model& model )
{
    CommandTable& table = model.commands();

    table.add( Command( "window.split-rows", "Split above/below",
                        "Divide the focused tile into two tiles, one above "
                        "the other, both showing the same buffer." )
                   .onRun( []( Model& m ) {
                       const BufferId current =
                           m.layout().bufferOf( m.layout().focused() );
                       m.layout().splitFocused( Split::Rows, current );
                       return std::vector<CommandRequest>();
                   } ) );

    table.add( Command( "window.split-columns", "Split side by side",
                        "Divide the focused tile into two tiles side by "
                        "side, both showing the same buffer." )
                   .onRun( []( Model& m ) {
                       const BufferId current =
                           m.layout().bufferOf( m.layout().focused() );
                       m.layout().splitFocused( Split::Columns, current );
                       return std::vector<CommandRequest>();
                   } ) );

    table.add( Command( "window.close", "Close tile",
                        "Close the focused tile and give its space to its "
                        "neighbour. The last tile cannot be closed." )
                   .onRun( []( Model& m ) {
                       /* A borrowed tile is given back, not destroyed. */
                       if( m.returnBorrowedTile() ) {
                           return std::vector<CommandRequest>();
                       }
                       if( !m.layout().closeFocused() ) {
                           m.setMessage( "the last tile cannot be closed" );
                       }
                       return std::vector<CommandRequest>();
                   } ) );

    table.add( Command( "window.maximise", "Maximise tile",
                        "Discard every other tile, leaving the focused one "
                        "alone. The arrangement is gone, not hidden." )
                   .onRun( []( Model& m ) {
                       if( !m.layout().maximiseFocused() ) {
                           m.setMessage( "already the only tile" );
                       }
                       return std::vector<CommandRequest>();
                   } ) );

    table.add( Command( "window.focus-next", "Next tile",
                        "Move focus to the next tile, wrapping at the end." )
                   .onRun( []( Model& m ) {
                       m.layout().focusNext();
                       return std::vector<CommandRequest>();
                   } ) );

    table.add( Command( "window.focus-prev", "Previous tile",
                        "Move focus to the previous tile, wrapping at the "
                        "start." )
                   .onRun( []( Model& m ) {
                       m.layout().focusPrev();
                       return std::vector<CommandRequest>();
                   } ) );

    table.add( Command( "help.contextual", "Help",
                        "Open help on whatever currently has focus. Never "
                        "does nothing, and never opens a table of contents "
                        "when it could open the answer." )
                   .helpTopic( "getting-started" )
                   .onRun( []( Model& m ) {
                       m.openHelp( m.contextualTopicId() );
                       return std::vector<CommandRequest>();
                   } ) );

    table.add( Command( "menu.open", "Menu",
                        "Open the menu bar. Every menu entry is a command "
                        "from this same table." )
                   .onRun( []( Model& m ) {
                       m.openMenu();
                       return std::vector<CommandRequest>();
                   } ) );

    table.add( Command( "command.palette", "Commands…",
                        "Show every registered command with its description "
                        "and binding, bound or not." )
                   .onRun( []( Model& m ) {
                       m.openPalette();
                       return std::vector<CommandRequest>();
                   } ) );

    table.add( Command( "app.abort", "Abort",
                        "Cancel the innermost thing and return to the "
                        "transcript." )
                   .onRun( []( Model& m ) {
                       /* Esc and C-g both reach the palette directly while
                        * it is open; this is the case where nothing modal is
                        * running and there is simply nothing to abort. */
                       m.setMessage( "nothing to abort" );
                       return std::vector<CommandRequest>();
                   } ) );

    table.add( Command( "app.quit", "Quit",
                        "Leave lens. Retained queries are released and the "
                        "session is closed." )
                   .onRun( []( Model& m ) {
                       m.requestQuit();
                       return std::vector<CommandRequest>();
                   } ) );
}

} // namespace lens
