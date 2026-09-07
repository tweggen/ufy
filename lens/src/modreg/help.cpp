/**
 * @file help.cpp
 */

#include "help.hpp"

#include <sstream>

namespace lens {

namespace {

const Key::Code kFunctionKeys[] = {
    Key::Code::F1, Key::Code::F2, Key::Code::F3, Key::Code::F4,
    Key::Code::F5, Key::Code::F6, Key::Code::F7, Key::Code::F8,
    Key::Code::F9, Key::Code::F10, Key::Code::F11, Key::Code::F12
};

const std::size_t kFunctionKeyCount =
    sizeof( kFunctionKeys ) / sizeof( kFunctionKeys[0] );

} // namespace

std::vector<Hint> functionKeyHints( const CommandTable& table,
                                    const Keymap& keymap )
{
    std::vector<Hint> hints;

    for( std::size_t i = 0; i < kFunctionKeyCount; ++i ) {
        KeySeq seq;
        seq.push_back( Key::named( kFunctionKeys[i] ) );

        const std::string commandId = keymap.lookup( seq );
        if( commandId.empty() ) {
            continue;
        }
        const Command* command = table.find( commandId );
        if( !command ) {
            /*
             * A binding to a command nobody registered. Shown with the id
             * rather than skipped, because a silently missing hint is how a
             * typo in a config file becomes an hour of confusion.
             */
            Hint hint;
            hint.key = toString( seq );
            hint.title = commandId + " (not registered)";
            hint.commandId = commandId;
            hints.push_back( hint );
            continue;
        }

        Hint hint;
        hint.key = toString( seq );
        hint.title = command->title();
        hint.commandId = commandId;
        hints.push_back( hint );
    }

    return hints;
}


std::string renderHintLine( const std::vector<Hint>& hints, int width )
{
    if( width <= 0 ) {
        return std::string();
    }

    std::string line;
    for( std::size_t i = 0; i < hints.size(); ++i ) {
        std::string entry = hints[i].key + " " + hints[i].title;
        const std::string candidate = line.empty() ? entry : line + "  " + entry;
        if( (int) candidate.size() > width ) {
            break;
        }
        line = candidate;
    }

    if( (int) line.size() > width ) {
        line.resize( (std::size_t) width );
    }
    line.resize( (std::size_t) width, ' ' );
    return line;
}


std::string generateKeymapPage( const CommandTable& table,
                                const Keymap& keymap )
{
    std::ostringstream page;

    page << "# Keys and commands\n\n"
         << "Generated from the live command table, so it cannot disagree\n"
         << "with the bindings actually in effect.\n\n";

    const std::vector<Command>& commands = table.all();
    if( commands.empty() ) {
        page << "(no commands are registered)\n";
        return page.str();
    }

    /* Widest id, so the columns line up without a layout engine. */
    std::size_t idWidth = 0;
    for( std::size_t i = 0; i < commands.size(); ++i ) {
        if( commands[i].id().size() > idWidth ) {
            idWidth = commands[i].id().size();
        }
    }

    for( std::size_t i = 0; i < commands.size(); ++i ) {
        const Command& command = commands[i];
        const KeySeq seq = keymap.bindingFor( command.id() );
        const std::string binding = seq.empty() ? "(unbound)" : toString( seq );

        page << command.id();
        page << std::string( idWidth - command.id().size() + 2, ' ' );
        page << binding << "\n";
        page << "    " << command.title() << "\n";
        page << "    " << command.help() << "\n";
        if( !command.helpTopic().empty() ) {
            page << "    see [[" << command.helpTopic() << "]]\n";
        }
        page << "\n";
    }

    return page.str();
}

std::vector<std::string> menuCategories()
{
    /*
     * Fixed, and in the bar's order. Deriving them from the registered
     * commands instead would make the menu's headings appear and disappear
     * as modules load, so a user could not learn where anything lives.
     */
    std::vector<std::string> categories;
    categories.push_back( "File" );
    categories.push_back( "Edit" );
    categories.push_back( "World" );
    categories.push_back( "Query" );
    categories.push_back( "Image" );
    categories.push_back( "Debug" );
    categories.push_back( "Window" );
    categories.push_back( "Help" );
    return categories;
}

} // namespace lens
