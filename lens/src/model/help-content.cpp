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
    add( m_topics, "first-steps", "First steps", {
        "unify-lens -- a place to keep a Unify world open and work inside it.",
        "",
        "THE KEYS THAT MATTER",
        "",
        "  F1          help on whatever has focus. You are here.",
        "  F10         the menu.",
        "  M-x         every command there is, searchable.",
        "",
        "  Tab         move to the next tile     S-Tab   the previous one",
        "  C-x 0       close this tile           C-x 1   make it the only one",
        "  C-x 2       split it top/bottom       C-x 3   split it left/right",
        "",
        "  C-x C-c     leave lens.",
        "",
        "TRY SOMETHING",
        "",
        "Move to the Transcript with Tab, and type at the ?- prompt:",
        "",
        "    colour( red );        define a fact  (note the semicolon)",
        "    colour( $x )          ask a question -- $x is a variable",
        "",
        "The answer appears under what you typed. Up and Down walk back",
        "through what you have typed before.",
        "",
        "WHERE TO GO NEXT",
        "",
        "  [[tutorial]]         a first session, step by step",
        "  [[tiling]]           the windows: splitting, closing, folding",
        "  [[keys]]             every command and its key",
        "  [[panels]]           what each kind of tile shows",
        "",
        "Close this tile with C-x 0 when you are done with it. F1 brings it",
        "back.",
    } );

    add( m_topics, "tutorial", "A first session", {
        "Five minutes, from an empty world to a question answered.",
        "",
        "1. FIND THE PROMPT",
        "",
        "   Press Tab until the Transcript tile has the highlighted border.",
        "   It shows `?-` and a cursor.",
        "",
        "2. TEACH IT SOMETHING",
        "",
        "   Type, pressing Enter after each line:",
        "",
        "       colour( red );",
        "       colour( green );",
        "",
        "   Each line ends in a semicolon, which is how Unify knows you are",
        "   stating a fact rather than asking a question. lens answers",
        "   `-- defined colour/1`: one predicate named colour, taking one",
        "   argument.",
        "",
        "3. ASK A QUESTION",
        "",
        "       colour( $x )",
        "",
        "   No semicolon this time. `$x` is a variable, so this asks: what",
        "   are the colours? You get both answers and a count:",
        "",
        "       $x = red",
        "       $x = green",
        "       -- 2 solutions",
        "",
        "4. ASK A NARROWER ONE",
        "",
        "       colour( blue )",
        "",
        "   No variable, so this is a yes/no question -- and the answer is",
        "   `-- no solutions`, because nothing said blue was a colour.",
        "",
        "5. WRITE A RULE",
        "",
        "       warm( $c ) { colour( $c ); $c = red; }",
        "",
        "   A rule holds when everything inside the braces holds. Ask",
        "   `warm( $c )` and you get red alone.",
        "",
        "That is the whole idea: state facts, write rules over them, ask",
        "questions. The language itself is documented in unify/LANGUAGE.md.",
        "",
        "See [[first-steps]] for the keys, [[panels]] for what else is on",
        "screen.",
    } );

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
        "See [[first-steps]] for the keys worth knowing on day one,",
        "[[tutorial]] for a guided first session, [[tiling]] for the window",
        "manager, [[keys]] for every binding, and [[panels]] for what each",
        "tile shows.",
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
