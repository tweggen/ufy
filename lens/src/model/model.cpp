/**
 * @file model.cpp
 */

#include "model.hpp"
#include "text-wrap.hpp"

#include <algorithm>
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


namespace {

/**
 * The identifier the cursor is in or just after, or empty.
 *
 * "Just after" counts, because that is where the cursor is when you have
 * just typed a name and want to know what it does. A `$variable` is not a
 * name anything documents, so the `$` ends the word rather than joining it.
 */
std::string wordAt( const std::string& text, std::size_t cursor )
{
    const auto isWord = []( char c ) {
        return std::isalnum( (unsigned char) c ) || c == '_';
    };
    if( cursor > text.size() ) {
        cursor = text.size();
    }
    std::size_t begin = cursor;
    while( begin > 0 && isWord( text[ begin - 1 ] ) ) {
        --begin;
    }
    std::size_t end = cursor;
    while( end < text.size() && isWord( text[end] ) ) {
        ++end;
    }
    if( begin == end || ( begin > 0 && text[ begin - 1 ] == '$' ) ) {
        return std::string();
    }
    return text.substr( begin, end - begin );
}

} // namespace


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
    case PanelKind::Transcript: {
        /*
         * UI.md section 5.2: in the transcript, F1 answers for the word
         * under the cursor -- a builtin, a reserved word -- because that is
         * the question being asked when someone presses it mid-line.
         */
        const std::string word = wordAt( focused->transcript.input,
                                         focused->transcript.cursor );
        if( !word.empty() && m_helpBook ) {
            if( m_helpBook->topic( "builtin." + word ) ) {
                return "builtin." + word;
            }
            if( m_helpBook->topic( "keyword." + word ) ) {
                return "keyword." + word;
            }
        }
        return "transcript";
    }
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

    const us::QueryId eventQuery =
        event.header.query ? *event.header.query : us::kNoQuery;
    const std::vector<us::QueryId>::iterator detachedAt =
        std::find( t.detached.begin(), t.detached.end(), eventQuery );
    const bool fromDetached =
        eventQuery != us::kNoQuery && detachedAt != t.detached.end();

    if( const us::Solution* solution = std::get_if<us::Solution>( &event.body ) ) {
        if( fromDetached ) {
            /* The user stopped this query; see TranscriptState::detached. */
            return;
        }
        TranscriptEntry entry;
        entry.kind = TranscriptEntry::Kind::Solution;
        entry.query = eventQuery;
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
            /*
             * A batch ended and more are waiting for demand. Said in the
             * transcript, because otherwise the last row of a batch looks
             * exactly like the last row of the answer -- and the status
             * line's "running" alone does not tell anyone how to get the
             * rest, or how to stop.
             */
            if( eventQuery == t.liveQuery && eventQuery != us::kNoQuery
                && status->produced > 0 ) {
                TranscriptEntry entry;
                entry.kind = TranscriptEntry::Kind::Info;
                entry.query = eventQuery;
                std::ostringstream os;
                os << "-- " << status->produced
                   << " so far \xc2\xb7 M-x query.more for more \xc2\xb7 "
                      "C-c stops";
                entry.text = os.str();
                t.entries.push_back( entry );
            }
            return;
        }

        /*
         * Only the LIVE query's end ends the live state. A query that was
         * superseded or detached can still report, and its report belongs in
         * the transcript, but it must not mark the current one finished.
         */
        if( eventQuery == t.liveQuery ) {
            t.finished = true;
            t.liveQuery = us::kNoQuery;
        }
        if( fromDetached ) {
            t.detached.erase( detachedAt );
        }

        TranscriptEntry entry;
        entry.kind = TranscriptEntry::Kind::Status;
        entry.query = eventQuery;

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

    /*
     * While a drop-down is open it has the keyboard, so it is what "the
     * focused panel" means to anyone watching the cursor -- the tile under
     * it is not receiving keys and its viewport is not moving.
     */
    if( menuOpen() ) {
        const Rect box = menuDropdownRect();
        view.cursorLine = m_menu.selected;
        view.topLine = m_menu.top;
        view.viewportRows = box.h - 2;
        view.totalLines = (int) menuItems( m_menu.heading ).size();
        out_valid = view.totalLines > 0 && view.viewportRows > 0;
        return view;
    }

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
    case PanelKind::Palette: {
        if( !buffer->palette.prompt.empty() ) {
            break;   /* a question, not a list */
        }
        view.cursorLine = buffer->palette.selected;
        view.topLine = buffer->palette.top;
        /* One row is the prompt; the rest is the list. */
        view.viewportRows = inner.h - 1;
        view.totalLines = (int) paletteMatches().size();
        out_valid = true;
        return view;
    }
    case PanelKind::Transcript: {
        /*
         * Only while a past line is selected. At the prompt the transcript
         * is a log pinned to its newest line with the cursor in the input,
         * not a list with a line cursor, and reporting one would make the
         * invariants assert about a cursor nobody can see.
         */
        const TranscriptState& t = buffer->transcript;
        if( t.selected < 0 ) {
            break;
        }
        view.cursorLine = t.selected;
        view.topLine = t.top;
        view.viewportRows = transcriptHistoryRows( inner.h );
        view.totalLines = (int) t.entries.size();
        out_valid = view.viewportRows > 0;
        return view;
    }
    case PanelKind::Placeholder:
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
    /*
     * Borland's F10: the bar takes the keyboard with the first heading lit
     * and nothing dropped down yet. Always the FIRST heading rather than the
     * last one used, so F10 Enter means the same thing every time and can
     * be learnt with the fingers.
     */
    m_menu = MenuBarState();
    m_menu.mode = MenuBarState::Mode::Bar;
}


void Model::openMenuHeading( int heading )
{
    const int count = (int) menuHeadings().size();
    if( heading < 0 || heading >= count ) {
        return;
    }
    m_menu.mode = MenuBarState::Mode::Open;
    m_menu.heading = heading;
    m_menu.selected = 0;
    m_menu.top = 0;
}


void Model::closeMenu()
{
    m_menu = MenuBarState();
}


std::vector<const Command*> Model::menuItems( int heading ) const
{
    std::vector<const Command*> items;

    const std::vector<std::string> categories = menuCategories();
    if( heading < 0 || heading >= (int) categories.size() ) {
        return items;
    }

    const std::vector<Command>& all = m_commands.all();
    for( std::size_t i = 0; i < all.size(); ++i ) {
        if( all[i].inMenu()
            && all[i].category() == categories[ (std::size_t) heading ] ) {
            items.push_back( &all[i] );
        }
    }
    return items;
}


Rect Model::menuDropdownRect() const
{
    if( !menuOpen() ) {
        return Rect();
    }

    const std::vector<MenuHeading> headings = menuHeadings();
    const std::vector<const Command*> items = menuItems( m_menu.heading );

    /*
     * Wide enough for the widest "Title   Key" line plus a column of
     * padding either side, and never narrower than the heading it hangs
     * from, so the box does not look like it belongs to something else.
     */
    int content = displayWidth( headings[ (std::size_t) m_menu.heading ].name );
    if( items.empty() ) {
        content = std::max( content, displayWidth( kMenuEmptyText ) );
    }
    for( std::size_t i = 0; i < items.size(); ++i ) {
        int width = displayWidth( items[i]->title() );
        const KeySeq binding = m_keymap.bindingFor( items[i]->id() );
        if( !binding.empty() ) {
            width += kMenuKeyGap + displayWidth( toString( binding ) );
        }
        content = std::max( content, width );
    }

    int w = content + 2 /* padding */ + 2 /* border */;
    if( w > m_width ) { w = m_width; }

    /* Never taller than the tile area: it may cover tiles, never the
     * status or hint line, which have to stay truthful while it is open. */
    const Rect area = tileArea();
    int h = ( items.empty() ? 1 : (int) items.size() ) + 2;
    if( h > area.h ) { h = area.h; }

    /* The border sits one column left of the heading's first letter, so
     * the item text lines up under the heading text. */
    int x = menuHeadingColumn( (std::size_t) m_menu.heading ) - 1;
    if( x + w > m_width ) { x = m_width - w; }
    if( x < 0 ) { x = 0; }

    return Rect( x, area.y, w, h );
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
        palette->palette.top = 0;
        palette->palette.prompt.clear();
        palette->title = "M-x";
    }
    m_promptAccept = PromptAccept();

    /*
     * A tile, not an overlay. It goes below the focused tile so the work
     * stays visible above it, and at a small geometry the solver folds
     * something else rather than clipping the dialog -- which is why UI.md
     * makes dialogs tiles in the first place.
     */
    m_paletteTile =
        m_layout.splitFocused( splitDirectionForNewPanel(), m_paletteBuffer, 0.5 );
}


void Model::openPrompt( const std::string& label, const std::string& initial,
                        PromptAccept accept )
{
    closePalette();
    openPalette();

    Buffer* palette = buffer( m_paletteBuffer );
    if( !palette ) {
        return;
    }
    palette->palette.prompt = label;
    palette->palette.input = initial;
    /* The tile is titled with the question, minus its colon. */
    std::string title = label;
    while( !title.empty()
           && ( title.back() == ' ' || title.back() == ':' ) ) {
        title.pop_back();
    }
    palette->title = title;
    m_promptAccept = accept;
}


bool Model::promptActive() const
{
    if( !paletteActive() ) {
        return false;
    }
    const Buffer* palette = buffer( m_paletteBuffer );
    return palette && !palette->palette.prompt.empty();
}


/**
 * Enter at a prompt: close it FIRST, then hand over the text -- for the
 * reason foldPalette closes before running a command: whatever the handler
 * does to the layout must happen to the tile the user was looking at, not
 * to the prompt's own.
 */
std::vector<CommandRequest> acceptPrompt( Model& model )
{
    const Buffer* palette = model.buffer( model.m_paletteBuffer );
    const std::string text = palette ? palette->palette.input : std::string();
    const PromptAccept accept = model.m_promptAccept;

    model.closePalette();
    model.m_promptAccept = PromptAccept();

    if( !accept ) {
        return std::vector<CommandRequest>();
    }
    return accept( model, text );
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
 * The minibuffer while it is asking a question. One line of text and three
 * keys that mean something: Enter accepts, Esc and C-g cancel, C-u clears --
 * the last because a prompt usually opens with a suggestion in it, and
 * backspacing over a suggestion one character at a time is how people learn
 * to dread a default.
 */
std::vector<CommandRequest> foldPrompt( Model& model, Buffer& palette,
                                        const Key& key )
{
    std::string& input = palette.palette.input;

    if( isCancel( key ) ) {
        model.closePalette();
        return std::vector<CommandRequest>();
    }
    if( key.code == Key::Code::Enter ) {
        return acceptPrompt( model );
    }
    if( key.code == Key::Code::Char && key.ctrl && key.ch == U'u' ) {
        input.clear();
        return std::vector<CommandRequest>();
    }
    if( key.code == Key::Code::Backspace ) {
        /* A codepoint, not a byte -- see foldTranscript. */
        if( !input.empty() ) {
            std::size_t at = input.size() - 1;
            while( at > 0 && ( (unsigned char) input[at] & 0xC0 ) == 0x80 ) {
                --at;
            }
            input.erase( at );
        }
        return std::vector<CommandRequest>();
    }
    if( key.code == Key::Code::Char && !key.ctrl && !key.alt && key.ch >= 0x20 ) {
        input += encodeUtf8( key.ch );
    }
    return std::vector<CommandRequest>();
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

    if( !palette.palette.prompt.empty() ) {
        return foldPrompt( model, palette, key );
    }

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
    case PanelKind::Palette:
        /* One row is the prompt; the rest is the list. */
        buffer.palette.top = ensureVisible( buffer.palette.selected,
                                            (int) model.paletteMatches().size(),
                                            inner.h - 1, buffer.palette.top );
        return;
    case PanelKind::Transcript: {
        TranscriptState& t = buffer.transcript;
        if( t.selected < 0 ) {
            return;
        }
        t.top = ensureVisible( t.selected, (int) t.entries.size(),
                               transcriptHistoryRows( inner.h ), t.top );
        return;
    }
    case PanelKind::Placeholder:
        return;
    }
}


/** Keep the open drop-down's selection inside its visible rows. */
void followMenu( Model& model )
{
    if( !model.menuOpen() ) {
        return;
    }
    MenuBarState& menu = model.menuBar();
    const Rect box = model.menuDropdownRect();
    const int count = (int) model.menuItems( menu.heading ).size();
    menu.top = ensureVisible( menu.selected, count, box.h - 2, menu.top );
}


/**
 * The menu bar's keys. Modal while the bar has the keyboard, exactly like
 * the palette: nothing else sees a key until it closes.
 *
 * The keys are Borland's. On the bar, Left/Right choose a heading and
 * Enter or Down drops it; in a drop-down, Up/Down choose an item (wrapping,
 * since the list is short and the top and bottom are one key apart either
 * way), Left/Right move to the neighbouring drop-down, Enter runs. Esc steps
 * back one level; C-g and the menu key itself close everything.
 */
std::vector<CommandRequest> foldMenu( Model& model, const Key& key )
{
    std::vector<CommandRequest> requests;
    MenuBarState& menu = model.menuBar();
    const int headingCount = (int) menuHeadings().size();

    /*
     * The bindings that open the menu still work inside it, read from the
     * live keymap so a rebinding in the config is honoured here too. The
     * key that opens the bar closes it -- a menu key that only opened would
     * make the menu a trap for anyone who pressed it by accident -- and
     * M-<letter> jumps straight to that heading from anywhere in the menu.
     */
    KeySeq single;
    single.push_back( key );
    const std::string bound = model.keymap().lookup( single );
    if( bound == "menu.open"
        || ( key.code == Key::Code::Char && key.ctrl && key.ch == U'g' ) ) {
        model.closeMenu();
        return requests;
    }
    if( bound.compare( 0, 5, "menu." ) == 0 ) {
        const Command* command = model.commands().find( bound );
        if( command ) {
            return command->run( model );
        }
    }

    if( key.code == Key::Code::Escape ) {
        if( model.menuOpen() ) {
            menu.mode = MenuBarState::Mode::Bar;   /* back to the bar */
        } else {
            model.closeMenu();
        }
        return requests;
    }

    if( key.code == Key::Code::Left || key.code == Key::Code::Right ) {
        const int step = ( key.code == Key::Code::Left ) ? -1 : 1;
        const int next = ( menu.heading + step + headingCount ) % headingCount;
        if( model.menuOpen() ) {
            /* Moving sideways keeps the drop-down open, on the neighbour. */
            model.openMenuHeading( next );
        } else {
            menu.heading = next;
        }
        return requests;
    }

    if( !model.menuOpen() ) {
        if( key.code == Key::Code::Enter || key.code == Key::Code::Down ) {
            model.openMenuHeading( menu.heading );
            return requests;
        }

        /*
         * On the bar, the underlined letter alone opens its heading, as in
         * the IDEs this borrows from. Only on the bar: inside a drop-down a
         * letter has no meaning yet, and guessing one would be worse than
         * doing nothing.
         */
        if( key.code == Key::Code::Char && !key.ctrl && !key.alt
            && key.ch < 0x80 ) {
            const std::vector<MenuHeading> headings = menuHeadings();
            const char wanted = (char) std::tolower( (int) key.ch );
            for( std::size_t i = 0; i < headings.size(); ++i ) {
                const int at = menuMnemonic( headings[i], model.keymap() );
                if( at >= 0
                    && (char) std::tolower(
                           (unsigned char) headings[i].name[ (std::size_t) at ] )
                           == wanted ) {
                    model.openMenuHeading( (int) i );
                    return requests;
                }
            }
        }
        return requests;
    }

    /* A drop-down is open. */
    const std::vector<const Command*> items = model.menuItems( menu.heading );
    const int count = (int) items.size();

    if( key.code == Key::Code::Down || key.code == Key::Code::Up ) {
        if( count > 0 ) {
            const int step = ( key.code == Key::Code::Up ) ? -1 : 1;
            menu.selected = ( menu.selected + step + count ) % count;
        }
        followMenu( model );
        return requests;
    }
    if( key.code == Key::Code::Home || key.code == Key::Code::PageUp ) {
        menu.selected = 0;
        followMenu( model );
        return requests;
    }
    if( key.code == Key::Code::End || key.code == Key::Code::PageDown ) {
        menu.selected = count > 0 ? count - 1 : 0;
        followMenu( model );
        return requests;
    }

    if( key.code == Key::Code::Enter ) {
        if( menu.selected < 0 || menu.selected >= count ) {
            model.setMessage( "nothing here yet" );
            return requests;
        }
        const Command* chosen = items[ (std::size_t) menu.selected ];

        /* Closed before the command runs, for the same reason the palette
         * is: a command that opens a dialog or a menu of its own must find
         * the keyboard free, not still held by this one. */
        model.closeMenu();

        if( !chosen->enabled( model ) ) {
            model.setMessage( chosen->title() + " is not available here" );
            return requests;
        }
        return chosen->run( model );
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

/** The Input entry before `from`, or -1. */
int previousInput( const TranscriptState& t, int from )
{
    for( int i = from - 1; i >= 0; --i ) {
        if( t.entries[ (std::size_t) i ].kind == TranscriptEntry::Kind::Input ) {
            return i;
        }
    }
    return -1;
}


/** The Input entry after `from`, or -1 -- which is the prompt. */
int nextInput( const TranscriptState& t, int from )
{
    for( int i = from + 1; i < (int) t.entries.size(); ++i ) {
        if( t.entries[ (std::size_t) i ].kind == TranscriptEntry::Kind::Input ) {
            return i;
        }
    }
    return -1;
}


/** Where the view sits when it is pinned to the newest line. */
int pinnedTop( const TranscriptState& t, int rows )
{
    const int top = (int) t.entries.size() - rows;
    return top > 0 ? top : 0;
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

    /*
     * G2.5: the cursor over past input. PageUp and PageDown rather than the
     * arrows, because Up and Down already walk the history into the prompt
     * -- which is the other way to get an old line back, for when you want
     * to edit it first.
     */
    if( key.code == Key::Code::PageUp && !key.ctrl && !key.alt ) {
        const int from = t.selected >= 0 ? t.selected
                                         : (int) t.entries.size();
        const int found = previousInput( t, from );
        if( found < 0 ) {
            model.setMessage( t.selected < 0 ? "no earlier input to re-run"
                                             : "this is the oldest input" );
            return requests;
        }
        if( t.selected < 0 ) {
            /* Start from where the eye already is: the pinned bottom. */
            t.top = pinnedTop( t, transcriptHistoryRows(
                model.innerRectOf( model.layout().focused() ).h ) );
        }
        t.selected = found;
        return requests;
    }
    if( key.code == Key::Code::PageDown && !key.ctrl && !key.alt ) {
        if( t.selected >= 0 ) {
            t.selected = nextInput( t, t.selected );
        }
        return requests;
    }
    if( t.selected >= 0 ) {
        if( key.code == Key::Code::Escape ) {
            t.selected = -1;
            return requests;
        }
        /* Anything else is aimed at the prompt: go back there, then do it. */
        t.selected = -1;
    }

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

    /*
     * `? goal;` is unify-run's query shorthand, and the history file is
     * shared with it (G2.8) -- so a line recalled from a unify-run session
     * has to mean here what it meant there. Without this it would end in
     * `;`, be taken for a definition, and fail to parse.
     */
    const std::string::size_type first = line.find_first_not_of( " \t" );
    if( first != std::string::npos && line[first] == '?' ) {
        std::string goal = line.substr( first + 1 );
        std::string::size_type last = goal.find_last_not_of( " \t" );
        if( last != std::string::npos && goal[last] == ';' ) {
            goal.erase( last );
        }
        request.kind = "solve";
        request.text = goal;
        requests.push_back( request );
        t.finished = false;
        t.produced = 0;
        return requests;
    }

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
        return foldMenu( model, event.key );
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
    /* The drop-down too: a resize can shrink it under its selection. */
    followMenu( model );
    return requests;
}


namespace {

/** True while the transcript has a query whose rows are still to come. */
bool hasLiveQuery( const Model& model )
{
    const Buffer* buffer = model.transcript();
    return buffer && !buffer->transcript.finished
        && buffer->transcript.liveQuery != us::kNoQuery;
}


/**
 * Detach from the live query and ask the core to cancel it -- G2.6.
 *
 * The UI detaches FIRST and unconditionally, rather than waiting for the
 * core's `Aborted`: a front end that waited would be exactly as stuck as the
 * engine when the goal does not terminate, which is the case cancel is for.
 * The core's answer still arrives and lands in the transcript, because it is
 * the core's own statement of what happened.
 */
std::vector<CommandRequest> cancelLiveQuery( Model& model )
{
    std::vector<CommandRequest> requests;
    Buffer* buffer = model.transcript();
    if( !hasLiveQuery( model ) ) {
        model.setMessage( "no query is running" );
        return requests;
    }

    TranscriptState& t = buffer->transcript;
    CommandRequest request;
    request.kind = "cancel";
    request.handle = (std::uint64_t) t.liveQuery;
    requests.push_back( request );

    t.detached.push_back( t.liveQuery );
    t.liveQuery = us::kNoQuery;
    t.finished = true;

    /*
     * Plan section 6: do not ship a cancel that silently lies. This core
     * cannot stop a goal that is still computing, so the message says what
     * actually happened rather than "cancelled".
     */
    model.setMessage( model.capabilities().realCancel
                          ? "query cancelled"
                          : "query detached \xc2\xb7 this engine cannot stop "
                            "a running goal yet" );
    return requests;
}

/**
 * `F5` -- G2.5. Re-run the selected past line through the SAME function
 * Enter uses, so the request is identical by construction rather than by a
 * reconstruction that happens to agree today.
 */
std::vector<CommandRequest> rerunTranscriptLine( Model& model )
{
    std::vector<CommandRequest> requests;
    Buffer* buffer = model.transcript();
    if( !buffer ) {
        model.setMessage( "this layout has no transcript" );
        return requests;
    }
    TranscriptState& t = buffer->transcript;

    if( t.selected >= 0 && t.selected < (int) t.entries.size()
        && t.entries[ (std::size_t) t.selected ].kind
               == TranscriptEntry::Kind::Input ) {
        const std::string source = t.entries[ (std::size_t) t.selected ].source;

        /*
         * Whatever was half-typed at the prompt survives. Re-running an old
         * line is a side trip; losing the line you were in the middle of
         * writing would make it an expensive one.
         */
        const std::string keptInput = t.input;
        const std::size_t keptCursor = t.cursor;
        requests = submitTranscriptLine( model, *buffer, source );
        t.input = keptInput;
        t.cursor = keptCursor;
        t.selected = -1;
        return requests;
    }

    t.selected = -1;
    if( t.input.find_first_not_of( " \t" ) == std::string::npos ) {
        model.setMessage( "nothing to run \xc2\xb7 PageUp picks an earlier line" );
        return requests;
    }
    const std::string line = t.input;
    return submitTranscriptLine( model, *buffer, line );
}

} // namespace


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
                        "Move to the menu bar. Left and Right choose a "
                        "heading, Enter opens it, Esc steps back. Every menu "
                        "entry is a command from this same table." )
                   .notInMenu()
                   .onRun( []( Model& m ) {
                       m.openMenu();
                       return std::vector<CommandRequest>();
                   } ) );

    /* One command per heading, so M-<letter> is an ordinary rebindable
     * binding rather than a key the menu special-cases. */
    const std::vector<MenuHeading> headings = menuHeadings();
    for( std::size_t i = 0; i < headings.size(); ++i ) {
        const int index = (int) i;
        table.add( Command( headings[i].commandId, headings[i].name + " menu",
                            "Open the " + headings[i].name + " menu on the "
                            "menu bar." )
                       .notInMenu()
                       .onRun( [ index ]( Model& m ) {
                           m.openMenuHeading( index );
                           return std::vector<CommandRequest>();
                       } ) );
    }

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
                        * it is open. Outside it, the innermost thing that
                        * can be aborted is a live query (UI.md section 4). */
                       if( hasLiveQuery( m ) ) {
                           return cancelLiveQuery( m );
                       }
                       m.setMessage( "nothing to abort" );
                       return std::vector<CommandRequest>();
                   } ) );

    table.add( Command( "transcript.rerun", "Run line",
                        "Run the past input line under the transcript cursor "
                        "again -- PageUp and PageDown move it -- exactly as it "
                        "was first submitted. With no past line selected, run "
                        "what is typed at the prompt." )
                   .helpTopic( "transcript" )
                   .onRun( []( Model& m ) { return rerunTranscriptLine( m ); } ) );

    table.add( Command( "transcript.write-file", "Write transcript\xe2\x80\xa6",
                        "Save the transcript as plain text, one line per "
                        "line on screen: what was typed, what came back, and "
                        "what went wrong. Asks for the file name." )
                   .helpTopic( "transcript" )
                   .enabledWhen( []( const Model& m ) { return m.transcript() != NULL; } )
                   .onRun( []( Model& m ) {
                       m.openPrompt( "Write transcript to: ", "transcript.txt",
                           []( Model& model, const std::string& path ) {
                               std::vector<CommandRequest> requests;
                               if( path.find_first_not_of( " \t" )
                                   == std::string::npos ) {
                                   model.setMessage( "no file name given; "
                                                     "nothing written" );
                                   return requests;
                               }
                               const Buffer* transcript = model.transcript();
                               if( !transcript ) {
                                   return requests;
                               }
                               CommandRequest request;
                               request.kind = "write-file";
                               request.path = path;
                               request.text =
                                   transcriptText( transcript->transcript );
                               requests.push_back( request );
                               return requests;
                           } );
                       return std::vector<CommandRequest>();
                   } ) );

    table.add( Command( "query.cancel", "Cancel query",
                        "Stop waiting for the running query's solutions. The "
                        "transcript detaches at once; whether the engine can "
                        "actually stop the goal is shown in the status line." )
                   .helpTopic( "transcript" )
                   .onRun( []( Model& m ) { return cancelLiveQuery( m ); } ) );

    table.add( Command( "query.more", "More solutions",
                        "Ask the running query for its next batch of "
                        "solutions. The transcript shows one batch at a time "
                        "so a goal with thousands of answers cannot freeze "
                        "it." )
                   .helpTopic( "transcript" )
                   .enabledWhen( []( const Model& m ) { return hasLiveQuery( m ); } )
                   .onRun( []( Model& m ) {
                       std::vector<CommandRequest> requests;
                       CommandRequest request;
                       request.kind = "demand";
                       request.handle =
                           (std::uint64_t) m.transcript()->transcript.liveQuery;
                       requests.push_back( request );
                       return requests;
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
