/**
 * @file help-content.cpp
 */

#include "help-content.hpp"
#include "../modreg/help.hpp"

#include <sstream>

namespace lens {

namespace {

void add( std::vector<HelpTopic>& topics, const char* id, const char* title,
          const std::vector<std::string>& lines )
{
    HelpTopic topic;
    topic.id = id;
    topic.title = title;
    topic.lines = lines;
    topics.push_back( topic );
}

} // namespace

HelpBook::HelpBook( const CommandTable& commands, const Keymap& keymap )
{
    addBuiltinTopics();
    addGeneratedTopics( commands, keymap );
}


void HelpBook::addBuiltinTopics()
{
    add( m_topics, "getting-started", "Getting started", {
        "unify-lens is a text-mode environment for Unify: a place to keep a",
        "world open and work inside it, rather than a program you invoke.",
        "",
        "Everything on screen is a tile. There are no overlapping windows and",
        "no floating palettes; even dialogs are tiles, which is why they can",
        "never land off-screen or clip at 80x24.",
        "",
        "  F1     help on whatever has focus -- you are here",
        "  F10    the menu bar",
        "  M-x    every command, with its description and binding",
        "  C-x 0  close this tile",
        "",
        "See [[tiling]] for the window manager, [[keys]] for every binding,",
        "and [[panels]] for what each tile shows.",
    } );

    add( m_topics, "tiling", "Tiles and splits", {
        "A binary tree of splits with fractional weights: non-overlapping,",
        "resizable, and always covering the tile area exactly.",
        "",
        "  C-x 2   split above/below",
        "  C-x 3   split side by side",
        "  C-x 0   close the focused tile",
        "  C-x 1   maximise -- discards the others, like Emacs",
        "  Tab     next tile        S-Tab  previous tile",
        "",
        "Tiles and buffers are decoupled: any panel can be shown in any tile,",
        "and the same buffer can be open in two tiles at once.",
        "",
        "When the terminal is too small to hold every tile at a readable",
        "size, the least recently focused ones fold into one-line stubs",
        "rather than being drawn too small to read. The tile you are working",
        "in is the last to fold. Nothing is lost: growing the terminal back",
        "restores exactly the arrangement you had, because folding is a",
        "property of how the screen was solved and never of the layout",
        "itself.",
        "",
        "Below 80x24 lens stops rather than drawing something illegible.",
        "",
        "See [[keys]] and [[getting-started]].",
    } );

    add( m_topics, "panels", "The panels", {
        "Each tile shows a panel. None of them is privileged in the code --",
        "the shell knows about tiles, not about what is in them.",
        "",
        "  Transcript    the REPL: goals, results, program output",
        "  Catalogue     module -> (name, arity) -> clause count",
        "  Source        the selected definition",
        "  Diagnostics   parse and runtime errors, navigable to source",
        "  Solutions     the current query's bindings, one row per solution",
        "  Inspector     one binding as an expandable term tree",
        "  Trace         goal stack, breakpoints, step controls",
        "  Image         the current image, and save/load/insert",
        "  Queries       every query in flight or retained",
        "  Help          this",
        "",
        "Most of them are still to come; a tile showing placeholder text",
        "says which gate brings it. See [[getting-started]].",
    } );

    add( m_topics, "help", "Help", {
        "F1 opens help on whatever has focus, and never opens a table of",
        "contents when it could open the answer.",
        "",
        "  Up / Down    move the cursor",
        "  Enter        follow the link on the cursor line",
        "  Backspace    go back",
        "",
        "Help is a tile like any other, so it sits BESIDE your work rather",
        "than covering it -- which is the whole argument for tiling.",
        "",
        "See [[keys]] for the generated list of every command.",
    } );
}


void HelpBook::addGeneratedTopics( const CommandTable& commands,
                                   const Keymap& keymap )
{
    /*
     * The keymap page is generated from the live table on every construction,
     * which is what makes UI.md section 5.3's claim -- "so it cannot disagree
     * with the bindings" -- structurally true rather than a promise.
     */
    const std::string page = generateKeymapPage( commands, keymap );

    HelpTopic topic;
    topic.id = "keys";
    topic.title = "Keys and commands";

    std::istringstream in( page );
    std::string line;
    while( std::getline( in, line ) ) {
        topic.lines.push_back( line );
    }
    topic.lines.push_back( "See [[tiling]] and [[getting-started]]." );

    m_topics.push_back( topic );
}


const HelpTopic* HelpBook::topic( const std::string& id ) const
{
    for( std::size_t i = 0; i < m_topics.size(); ++i ) {
        if( m_topics[i].id == id ) {
            return &m_topics[i];
        }
    }
    return NULL;
}


std::vector<std::string> HelpBook::linksOn( const std::string& line )
{
    std::vector<std::string> links;
    std::string::size_type at = 0;

    for( ;; ) {
        const std::string::size_type open = line.find( "[[", at );
        if( open == std::string::npos ) { break; }
        const std::string::size_type close = line.find( "]]", open + 2 );
        if( close == std::string::npos ) { break; }

        links.push_back( line.substr( open + 2, close - open - 2 ) );
        at = close + 2;
    }
    return links;
}


std::string HelpBook::stripLinkMarkup( const std::string& line )
{
    std::string out;
    std::string::size_type at = 0;

    for( ;; ) {
        const std::string::size_type open = line.find( "[[", at );
        if( open == std::string::npos ) {
            out += line.substr( at );
            break;
        }
        const std::string::size_type close = line.find( "]]", open + 2 );
        if( close == std::string::npos ) {
            out += line.substr( at );
            break;
        }
        out += line.substr( at, open - at );
        out += line.substr( open + 2, close - open - 2 );
        at = close + 2;
    }
    return out;
}

} // namespace lens
