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
    addLanguageTopics();
}


void HelpBook::addLanguageTopics()
{
    /*
     * Generated at build time by tools/help-extract.cpp from unify/SPEC.md
     * and unify/LANGUAGE.md. Not written here: see that file for why lens
     * extracts the language reference rather than restating it.
     */
    static const std::vector<HelpTopic> kLanguage = {
#include "language-help.inc"
    };
    m_topics.insert( m_topics.end(), kLanguage.begin(), kLanguage.end() );
}


void HelpBook::addBuiltinTopics()
{
    add( m_topics, "first-steps", "First steps", {
        "unify-lens -- a place to keep a Unify world open and work inside it.",
        "",
        "THE KEYS THAT MATTER",
        "",
        "  F1          help on whatever has focus. You are here.",
        "  F10         the menu bar.  M-<letter> opens one menu.",
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
        "never land off-screen or clip at 80x24. The one exception is the",
        "menu bar's drop-downs, which open over the tiles and close again.",
        "",
        "  F1     help on whatever has focus -- you are here",
        "  F10    the menu bar  (Left/Right, Enter, Esc; or M-<letter>)",
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
        "Some are still to come; a tile showing placeholder text says",
        "which gate brings it. See [[transcript]] for the one you type into,",
        "[[catalogue]], [[solutions]], [[inspector]], [[diagnostics]],",
        "[[source]], and",
        "[[getting-started]].",
    } );

    add( m_topics, "transcript", "The transcript", {
        "The REPL. Type a goal and press Enter to solve it; type a",
        "definition -- a line ending in ; or containing { -- to add it to",
        "the world. What comes back is shown underneath: one row per",
        "solution, the program's own output, and any errors.",
        "",
        "  Enter        solve or define the line",
        "  Up / Down    walk back and forth through earlier input",
        "  PageUp       put the cursor on an earlier input line",
        "  PageDown     a later one; past the newest, back to the prompt",
        "  F5           run the line under the cursor again, exactly as it",
        "               was first submitted; at the prompt, like Enter",
        "  Esc          leave the earlier line, back to the prompt",
        "  C-c          stop the running query",
        "  C-g          abort: stops the running query too",
        "",
        "Text is executable: any line you typed before can be run again",
        "where it stands, without retyping it. Up brings an old line into",
        "the prompt instead, for when you want to change it first. What you",
        "were halfway through typing is kept either way.",
        "",
        "Solutions arrive one batch at a time, so a goal with ten thousand",
        "answers cannot freeze the screen. When a batch ends with more to",
        "come, the transcript says so; M-x query.more fetches the next one.",
        "",
        "Stopping a query detaches the transcript at once. Whether the",
        "engine can actually stop the goal is a separate question, and the",
        "status line answers it: \"cancel detaches only\" means this engine",
        "keeps computing a goal it has started until lens exits.",
        "",
        "History is kept in $HOME/.unify_history -- the same file, in the",
        "same format, as unify-run -i -- so Up recalls what you typed last",
        "time in either program. unify-run's query shorthand works here",
        "too: ? colour( $x ); is the goal colour( $x ).",
        "",
        "M-x transcript.write-file saves the transcript as plain text,",
        "exactly the lines on screen, to a file you name.",
        "",
        "F1 with the cursor on a builtin or a reserved word -- print,",
        "findall, foreach -- opens that word's page, taken from the",
        "language's own documents. [[language]] lists them all.",
        "",
        "See [[keys]] for every binding and [[getting-started]].",
    } );

    add( m_topics, "solutions", "Solutions", {
        "The current query's answers as a table: one row per solution, one",
        "column per variable. Each new goal you solve replaces the table.",
        "",
        "  Up / Down        move between solutions",
        "  PageUp/PageDown  a screenful at a time",
        "  Home / End       the first and the last row",
        "  Down at the end  ask for the next batch, when there is one",
        "  Enter            open the selected solution in the Inspector",
        "",
        "Solutions arrive a batch at a time, so a goal with ten thousand",
        "answers cannot freeze the screen. The bottom line says which it is:",
        "",
        "  -- 20 so far \xc2\xb7 Down past the end for more",
        "  -- 20 so far \xc2\xb7 asking for more\xe2\x80\xa6",
        "  -- 37 solutions                      (that is all of them)",
        "",
        "Holding Down at the end asks once, not once per key: the next",
        "batch is asked for when the last one has arrived. M-x query.more",
        "does the same from anywhere.",
        "",
        "A long value is cut to fit its column; the Inspector shows all of",
        "it. See [[inspector]] and [[transcript]].",
    } );

    add( m_topics, "inspector", "The inspector", {
        "The selected solution's bindings as a tree, one node per line. A",
        "value too big to read on one line is easier to read opened up.",
        "",
        "  Up / Down     move between nodes",
        "  Right         open the node",
        "  Left          close it; on a closed node, go to its parent",
        "  Enter         open or close",
        "",
        "  \xe2\x96\xb8 closed      \xe2\x96\xbe open      (no mark) a leaf",
        "",
        "WHAT A NODE CAN BE",
        "",
        "  atom          a bare name:  red, hall",
        "  integer       42",
        "  string        text, from a core that has a string type",
        "  variable      $p -- unbound: the solution holds for any value",
        "  compound      point( 1, 2 ) -- open, it reads point/2",
        "  array         [ 1, 2, 3 ] -- open, it reads array of 3",
        "  map           { a: 1 } -- open, one line per key",
        "",
        "THE CUT MARK",
        "",
        "A big value crosses from the engine in part, so a runaway term",
        "cannot flood the screen. Where something was left out, the node",
        "shows \xe2\x80\xa6 -- f( 1, \xe2\x80\xa6 ) is a term with more arguments than",
        "the two shown, and g( \xe2\x80\xa6 ) is a compound whose arguments were",
        "all left out, NOT the atom g.",
        "",
        "Opening a cut node fetches the rest from the engine; the line says",
        "(fetching\xe2\x80\xa6) until it arrives. The goal is never run again to",
        "get it: a goal can answer differently the second time, or do",
        "something twice. Only the answer to this query can be expanded,",
        "so solving a new goal makes the last one's values final.",
        "",
        "See [[solutions]] for choosing which solution is shown.",
    } );

    add( m_topics, "diagnostics", "Diagnostics", {
        "Every error and warning the engine has reported, newest last:",
        "parse errors in what you typed, unknown predicates, runtime",
        "failures. The transcript shows each one where it happened too.",
        "",
        "  Up / Down     move between diagnostics",
        "  Enter         send the Source panel to where it was reported",
        "",
        "If no Source tile is on screen, Enter opens one. See [[source]].",
    } );

    add( m_topics, "catalogue", "The catalogue", {
        "Every predicate in the world, grouped by where it came from, with",
        "how many clauses each has. It refreshes by itself whenever the",
        "world changes; nothing has to be reloaded.",
        "",
        "  Up / Down     move            PageUp/PageDown, Home/End",
        "  Enter         on a predicate: show its definition in Source",
        "                on a group: open or close it",
        "  Right / Left  open / close a group; Left on a predicate goes to",
        "                its group",
        "  type          filter by name -- anywhere in the name, any case",
        "  Backspace     take a letter off the filter",
        "  Esc           clear the filter",
        "",
        "THE GROUPS",
        "",
        "  <transcript>  what you typed at the ?- prompt",
        "  a file name   what a module file defined",
        "  asserted      what a running program added with assert",
        "  synthesized   helpers the engine made from for, foreach and if",
        "  builtins      what the engine provides",
        "",
        "The last two start closed, as one line with a count, because they",
        "are rarely what you are looking for. A filter searches them too:",
        "it is how you find a builtin.",
        "",
        "Where a predicate goes is decided by where it came from, never by",
        "its name -- a predicate of yours called __cache is yours.",
        "",
        "See [[source]].",
    } );

    add( m_topics, "source", "Source", {
        "The text of a definition, or the place an error points to --",
        "whichever it was last sent to.",
        "",
        "  Up / Down     move through the definition's lines",
        "",
        "Enter on a predicate in the Catalogue shows its definition here.",
        "The engine keeps clauses, not the text you typed, so what you see",
        "is the clauses printed back: a rule that used for, foreach or if",
        "shows the helper predicate the loop became. Keeping the text as",
        "typed is engine item E5, and the panel says so.",
        "",
        "Enter on a line in Diagnostics shows the place it reports -- the",
        "file, the line and the column, the line itself and a ^ under the",
        "column -- which is everything an error report carries.",
        "",
        "Editing a definition in place comes with the editor (gate G3).",
        "",
        "See [[catalogue]] and [[diagnostics]].",
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
