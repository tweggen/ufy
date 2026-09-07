/**
 * @file model.cpp
 */

#include "model.hpp"

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
                           const std::vector<std::string>& lines )
{
    Buffer buffer;
    buffer.id = (BufferId) ( m_buffers.size() + 1 );
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
     * An unbound single key is not an error and gets no message: in a
     * transcript most keys are text, and complaining about every one of them
     * would make the status line useless. Panels take unbound keys as input
     * once they exist.
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
                       m.setMessage( "help arrives with the Help panel (G2)" );
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
                       m.setMessage( "the command palette arrives with the "
                                     "minibuffer (G2)" );
                       return std::vector<CommandRequest>();
                   } ) );

    table.add( Command( "app.abort", "Abort",
                        "Cancel the innermost thing and return to the "
                        "transcript." )
                   .onRun( []( Model& m ) {
                       m.setMessage( "abort" );
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
