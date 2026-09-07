/**
 * @file model.cpp
 */

#include "model.hpp"

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
    case PanelKind::Help:    return "help";
    case PanelKind::Palette: return "keys";
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


Split Model::splitDirectionForNewPanel() const
{
    const Solution solution = solve( m_layout, tileArea() );
    const Placement* focused = solution.find( m_layout.focused() );
    if( !focused ) {
        return Split::Columns;
    }

    const SolverLimits limits;
    if( focused->rect.w >= limits.minWidth * 2 + 4 ) {
        return Split::Columns;
    }
    return Split::Rows;
}


void Model::openHelp( const std::string& topicId )
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
        help->help.scroll = 0;
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
     * Otherwise open one beside the work rather than over it -- the whole
     * argument for tiling (UI.md section 5.3). Splitting into columns keeps
     * the transcript's width when there is room and folds to a stub when
     * there is not, which is the solver's business rather than this
     * function's.
     */
    m_layout.splitFocused( splitDirectionForNewPanel(), m_helpBuffer, 0.5 );
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
        return requests;
    }
    if( key.code == Key::Code::Down ) {
        if( palette.palette.selected + 1 < (int) matches.size() ) {
            ++palette.palette.selected;
        }
        return requests;
    }

    if( key.code == Key::Code::Backspace ) {
        if( !palette.palette.input.empty() ) {
            palette.palette.input.resize( palette.palette.input.size() - 1 );
            palette.palette.selected = 0;
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
            }
        }
        return requests;
    }

    if( key.code == Key::Code::Char && !key.ctrl && !key.alt && key.ch >= 0x20 ) {
        palette.palette.input += encodeUtf8( key.ch );
        palette.palette.selected = 0;
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
        return;
    }
    if( key.code == Key::Code::Up ) {
        if( help.help.cursor > 0 ) { --help.help.cursor; }
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
        help.help.scroll = 0;
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
        help.help.scroll = 0;
        return;
    }
}

} // namespace

std::vector<CommandRequest> fold( Model& model, const Event& event )
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

    /*
     * An unbound key in an ordinary panel is not an error and gets no
     * message: in a transcript most keys are text, and complaining about
     * every one of them would make the status line useless.
     */
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
                       m.setMessage( "the menu arrives with the shell's "
                                     "dialogs (G2)" );
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
