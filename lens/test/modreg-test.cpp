/**
 * @file modreg-test.cpp
 *
 * The command table, keymaps and the generated help surfaces -- the "H"
 * criterion of ACCEPTANCE.md G1.
 *
 * The criteria this discharges, in the gate's own words: "M-x lists every
 * registered command; the generated keymap page matches the live command
 * table (asserted, not eyeballed); a command registered without help text
 * fails the build"; and "the hint line is non-empty in every golden".
 */

#include "../../unify/test/session/test-harness.hpp"

#include "../src/modreg/command.hpp"
#include "../src/modreg/help.hpp"
#include "../src/modreg/keymap.hpp"

namespace {

using namespace unify_test;
using namespace lens;

/** A table with enough shape to exercise the generated surfaces. */
CommandTable sampleTable()
{
    CommandTable table;
    table.add( Command( "window.split-rows", "Split above/below",
                        "Divide the focused tile into two stacked tiles." ) );
    table.add( Command( "window.focus-next", "Next tile",
                        "Move focus to the next tile, wrapping at the end." ) );
    table.add( Command( "help.contextual", "Help",
                        "Open help on whatever currently has focus." ) );
    table.add( Command( "query.cancel", "Cancel query",
                        "Detach from the running query." ) );
    return table;
}

bool contains( const std::string& haystack, const std::string& needle )
{
    return haystack.find( needle ) != std::string::npos;
}

} // namespace

int main()
{
    Registry registry;

    // -- the command table -------------------------------------------------

    registry.add( "H a command with no help text is refused", []() {
        CommandTable table;

        const std::string problem =
            table.add( Command( "bad.command", "Bad", "" ) );
        UT_CHECK_MSG( !problem.empty(),
                      "a command with empty help text was accepted" );
        UT_CHECK_MSG( contains( problem, "help" ),
                      "the rejection should say what is missing, got: "
                          << problem );
        UT_CHECK_EQ( table.size(), std::size_t( 0 ) );

        /* Whitespace is not help text either. */
        UT_CHECK( !table.add( Command( "bad.two", "Bad", "   \t " ) ).empty() );
        UT_CHECK_EQ( table.size(), std::size_t( 0 ) );
    } );

    registry.add( "H a command with no title or id is refused", []() {
        CommandTable table;
        UT_CHECK( !table.add( Command( "", "Title", "Help." ) ).empty() );
        UT_CHECK( !table.add( Command( "an.id", "", "Help." ) ).empty() );
        UT_CHECK_EQ( table.size(), std::size_t( 0 ) );
    } );

    registry.add( "a duplicate command id is refused", []() {
        CommandTable table;
        UT_CHECK( table.add( Command( "a.b", "A B", "Does a b." ) ).empty() );
        const std::string problem =
            table.add( Command( "a.b", "Other", "Different help." ) );
        UT_CHECK_MSG( !problem.empty(), "a duplicate id was accepted" );
        UT_CHECK_EQ( table.size(), std::size_t( 1 ) );
    } );

    registry.add( "commands are found by id and keep registration order", []() {
        const CommandTable table = sampleTable();
        UT_CHECK_EQ( table.size(), std::size_t( 4 ) );
        UT_CHECK( table.find( "help.contextual" ) != NULL );
        UT_CHECK( table.find( "no.such.command" ) == NULL );
        UT_CHECK_EQ( table.all()[0].id(), std::string( "window.split-rows" ) );
        UT_CHECK_EQ( table.all()[3].id(), std::string( "query.cancel" ) );
    } );

    // -- keys --------------------------------------------------------------

    registry.add( "keys round-trip through their printed form", []() {
        const char* spellings[] = {
            "a", "C-x", "M-x", "F1", "F12", "Tab", "S-Tab", "Esc",
            "C-M-a", "PageDown", "Enter"
        };
        for ( const char* spelling : spellings ) {
            Key key;
            UT_CHECK_MSG( Key::parse( spelling, key ),
                          "could not parse key '" << spelling << "'" );
            UT_CHECK_MSG( key.toString() == spelling,
                          "'" << spelling << "' printed back as '"
                              << key.toString() << "'" );
        }

        Key unused;
        UT_CHECK( !Key::parse( "", unused ) );
        UT_CHECK( !Key::parse( "Nonsense", unused ) );
    } );

    registry.add( "key sequences round-trip", []() {
        KeySeq seq;
        UT_CHECK( parseKeySeq( "C-x 2", seq ) );
        UT_CHECK_EQ( seq.size(), std::size_t( 2 ) );
        UT_CHECK_EQ( toString( seq ), std::string( "C-x 2" ) );

        UT_CHECK( parseKeySeq( "C-x C-c", seq ) );
        UT_CHECK_EQ( toString( seq ), std::string( "C-x C-c" ) );
    } );

    // -- the keymap --------------------------------------------------------

    registry.add( "a binding that is a prefix of another is refused", []() {
        Keymap map;
        KeySeq chord;
        KeySeq prefix;
        UT_CHECK( parseKeySeq( "C-x 2", chord ) );
        UT_CHECK( parseKeySeq( "C-x", prefix ) );

        UT_CHECK( map.bind( chord, "window.split-rows" ).empty() );

        /*
         * The ambiguity that matters: with both bound, the input loop would
         * have to decide on seeing C-x whether to fire or wait, and either
         * choice is wrong half the time. Refusing here turns a subtle input
         * bug into a startup message.
         */
        const std::string problem = map.bind( prefix, "something.else" );
        UT_CHECK_MSG( !problem.empty(),
                      "binding a prefix of an existing chord was accepted" );
        UT_CHECK_MSG( contains( problem, "prefix" ),
                      "the rejection should explain the conflict, got: "
                          << problem );

        /* And the other direction: extending an existing single-key binding. */
        Keymap other;
        UT_CHECK( other.bind( prefix, "a.command" ).empty() );
        UT_CHECK( !other.bind( chord, "b.command" ).empty() );
    } );

    registry.add( "rebinding the same sequence twice is refused", []() {
        Keymap map;
        KeySeq seq;
        UT_CHECK( parseKeySeq( "F5", seq ) );
        UT_CHECK( map.bind( seq, "query.run" ).empty() );
        UT_CHECK( !map.bind( seq, "query.other" ).empty() );
        UT_CHECK_EQ( map.lookup( seq ), std::string( "query.run" ) );
    } );

    registry.add( "prefixes are recognised so a chord can be read", []() {
        Keymap map;
        KeySeq chord;
        UT_CHECK( parseKeySeq( "C-x 2", chord ) );
        UT_CHECK( map.bind( chord, "window.split-rows" ).empty() );

        KeySeq partial;
        partial.push_back( chord[0] );
        UT_CHECK_MSG( map.isPrefix( partial ),
                      "C-x must read as a prefix while C-x 2 is bound" );
        UT_CHECK_MSG( map.lookup( partial ).empty(),
                      "a bare prefix must not resolve to a command" );
        UT_CHECK_MSG( !map.isPrefix( chord ),
                      "a complete binding is not a prefix of itself" );
    } );

    registry.add( "the default keymap binds the tiling commands of UI.md", []() {
        const Keymap map = defaultKeymap();

        struct Expected { const char* keys; const char* command; };
        const Expected expected[] = {
            { "C-x 2", "window.split-rows" },
            { "C-x 3", "window.split-columns" },
            { "C-x 0", "window.close" },
            { "C-x 1", "window.maximise" },
            { "Tab",   "window.focus-next" },
            { "F1",    "help.contextual" },
            { "F10",   "menu.open" },
            { "M-x",   "command.palette" },
        };
        for ( const Expected& e : expected ) {
            KeySeq seq;
            UT_CHECK( parseKeySeq( e.keys, seq ) );
            UT_CHECK_MSG( map.lookup( seq ) == e.command,
                          "'" << e.keys << "' should run " << e.command
                              << ", got '" << map.lookup( seq ) << "'" );
        }
    } );

    // -- generated help ----------------------------------------------------

    registry.add( "H the generated keymap page matches the live table", []() {
        const CommandTable table = sampleTable();

        Keymap map;
        KeySeq seq;
        UT_CHECK( parseKeySeq( "C-x 2", seq ) );
        UT_CHECK( map.bind( seq, "window.split-rows" ).empty() );
        UT_CHECK( parseKeySeq( "Tab", seq ) );
        UT_CHECK( map.bind( seq, "window.focus-next" ).empty() );
        UT_CHECK( parseKeySeq( "F1", seq ) );
        UT_CHECK( map.bind( seq, "help.contextual" ).empty() );
        /* query.cancel is deliberately left unbound. */

        const std::string page = generateKeymapPage( table, map );

        /*
         * Asserted, not eyeballed. Every command in the table appears, with
         * its help text, and with the binding the KEYMAP reports -- so a
         * page that fell behind a rebinding fails here rather than misleading
         * a user.
         */
        for ( const Command& command : table.all() ) {
            UT_CHECK_MSG( contains( page, command.id() ),
                          "command '" << command.id()
                              << "' is missing from the generated keymap page" );
            UT_CHECK_MSG( contains( page, command.help() ),
                          "the help text of '" << command.id()
                              << "' is missing from the generated page" );

            const KeySeq binding = map.bindingFor( command.id() );
            if ( binding.empty() ) {
                continue;
            }
            UT_CHECK_MSG( contains( page, toString( binding ) ),
                          "the binding " << toString( binding ) << " of '"
                              << command.id() << "' is missing from the page" );
        }

        UT_CHECK_MSG( contains( page, "(unbound)" ),
                      "an unbound command must be shown as unbound rather than "
                      "omitted -- discovering a command exists must not require "
                      "it to have a key" );
    } );

    registry.add( "H the hint line is never blank while commands are bound",
                  []() {
        const CommandTable table = sampleTable();
        Keymap map;
        KeySeq seq;
        UT_CHECK( parseKeySeq( "F1", seq ) );
        UT_CHECK( map.bind( seq, "help.contextual" ).empty() );

        const std::vector<Hint> hints = functionKeyHints( table, map );
        UT_CHECK_MSG( hints.size() == 1,
                      "expected one function-key hint, got " << hints.size() );
        UT_CHECK_EQ( hints[0].key, std::string( "F1" ) );
        UT_CHECK_EQ( hints[0].title, std::string( "Help" ) );

        const std::string line = renderHintLine( hints, 80 );
        UT_CHECK_EQ( (int) line.size(), 80 );
        UT_CHECK_MSG( contains( line, "F1 Help" ),
                      "the hint line should read 'F1 Help', got: '"
                          << line << "'" );
        UT_CHECK_MSG( line.find_first_not_of( ' ' ) != std::string::npos,
                      "the hint line is blank" );
    } );

    registry.add( "H the hint line is exactly the requested width", []() {
        const CommandTable table = sampleTable();
        Keymap map;
        KeySeq seq;
        for ( const char* k : { "F1", "F2", "F3", "F4" } ) {
            UT_CHECK( parseKeySeq( k, seq ) );
            map.bind( seq, "help.contextual" );
        }

        const std::vector<Hint> hints = functionKeyHints( table, map );
        for ( int width : { 10, 20, 40, 80, 120 } ) {
            const std::string line = renderHintLine( hints, width );
            UT_CHECK_MSG( (int) line.size() == width,
                          "at width " << width << " the hint line was "
                              << line.size() << " columns" );
        }
        UT_CHECK( renderHintLine( hints, 0 ).empty() );
    } );

    registry.add( "H a binding to an unregistered command is visible, not silent",
                  []() {
        CommandTable table;
        Keymap map;
        KeySeq seq;
        UT_CHECK( parseKeySeq( "F7", seq ) );
        UT_CHECK( map.bind( seq, "typo.in.the.config" ).empty() );

        const std::vector<Hint> hints = functionKeyHints( table, map );
        UT_CHECK_MSG( hints.size() == 1,
                      "a binding to an unknown command should still produce a "
                      "hint, so a config typo is visible rather than silently "
                      "doing nothing" );
        UT_CHECK_MSG( contains( hints[0].title, "not registered" ),
                      "the hint should say the command is unknown, got: "
                          << hints[0].title );
    } );

    registry.add( "an empty table generates a page that says so", []() {
        const CommandTable table;
        const Keymap map;
        const std::string page = generateKeymapPage( table, map );
        UT_CHECK( !page.empty() );
        UT_CHECK( contains( page, "no commands" ) );
    } );

    return registry.run( "lens command table and help (G1 H)" ) == 0 ? 0 : 1;
}
