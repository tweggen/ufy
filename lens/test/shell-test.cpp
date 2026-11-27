/**
 * @file shell-test.cpp
 *
 * The shell's own behaviour: `fold`, the Help panel, and the `M-x` palette.
 * This is the interactive half of gate G1's "H" criterion.
 *
 * Model tests, not screen tests. What has to be true -- F1 opens the right
 * topic, M-x lists every command, the palette is modal, no help link is dead
 * -- is all assertable without rendering anything, which is the point of the
 * fold/view split.
 *
 * It also pins `renderValue`, which is model code with the same property and
 * had no test at all: the only thing reaching it was one golden screen, and
 * that screen exercises exactly one of its eight arms. See the section at the
 * end.
 */

#include "../../unify/test/session/test-harness.hpp"

#include <cctype>

#include "../src/app/layouts.hpp"
#include "../src/model/model.hpp"
#include "../src/model/transcript.hpp"
#include "../src/modreg/help.hpp"
#include "../src/model/view.hpp"

namespace {

using namespace unify_test;
using namespace lens;

/** A model in the state the composition root leaves it in. */
Model makeModel( const char* layout = "browse", int w = 120, int h = 40 )
{
    Model model;
    model.setGeometry( w, h );
    registerShellCommands( model );
    model.keymap() = defaultKeymap();
    applyStockLayout( model, layout );
    model.rebuildHelp();
    return model;
}

void type( Model& model, const char* keys )
{
    KeySeq seq;
    if ( !parseKeySeq( keys, seq ) ) {
        UT_FAIL( "the test itself has a bad key sequence: '" << keys << "'" );
    }
    for ( const Key& key : seq ) {
        Event event;
        event.kind = Event::Kind::Key;
        event.key = key;
        ( void ) fold( model, event );
    }
}

/** Like type(), but returns every request the keys asked for. */
std::vector<CommandRequest> typeKeys( Model& model, const char* keys )
{
    std::vector<CommandRequest> all;
    KeySeq seq;
    if ( !parseKeySeq( keys, seq ) ) {
        UT_FAIL( "the test itself has a bad key sequence: '" << keys << "'" );
    }
    for ( const Key& key : seq ) {
        Event event;
        event.kind = Event::Kind::Key;
        event.key = key;
        const std::vector<CommandRequest> some = fold( model, event );
        all.insert( all.end(), some.begin(), some.end() );
    }
    return all;
}

/** Tab until the transcript has focus. */
void focusTranscript( Model& model )
{
    Buffer* transcript = model.transcript();
    if ( !transcript ) {
        UT_FAIL( "this layout has no transcript" );
    }
    for ( int i = 0; i < 16 && model.focusedBuffer() != transcript; ++i ) {
        type( model, "Tab" );
    }
    if ( model.focusedBuffer() != transcript ) {
        UT_FAIL( "could not focus the transcript" );
    }
}

/** Type printable characters one at a time, as a user would. */
void typeText( Model& model, const std::string& text )
{
    for ( char c : text ) {
        Event event;
        event.kind = Event::Kind::Key;
        event.key = Key::character( (char32_t) (unsigned char) c );
        ( void ) fold( model, event );
    }
}

/*
 * Value builders. Written out rather than reached for through a session,
 * because half of these kinds no session in this tree can produce: `Str` and
 * `Float` are unreachable from the engine by construction (quoting is lost in
 * the parser, and there are no floats), and `Var` waits on engine item E7.4.
 * A hand-built tree is the only way to pin the arms that render them, and a
 * remote core or the fake session will send them long before lens's own
 * engine does.
 */
us::Value vAtom( const char* name )
{
    us::Value v;
    v.kind = us::Value::Kind::Atom;
    v.name = name;
    return v;
}

us::Value vInt( std::int64_t i )
{
    us::Value v;
    v.kind = us::Value::Kind::Int;
    v.i = i;
    return v;
}

us::Value vFloat( double f )
{
    us::Value v;
    v.kind = us::Value::Kind::Float;
    v.f = f;
    return v;
}

us::Value vStr( const char* text )
{
    us::Value v;
    v.kind = us::Value::Kind::Str;
    v.name = text;
    return v;
}

us::Value vVar( const char* name )
{
    us::Value v;
    v.kind = us::Value::Kind::Var;
    v.name = name;
    return v;
}

us::Value vCons( const char* functor, std::vector<us::Value> args )
{
    us::Value v;
    v.kind = us::Value::Kind::Cons;
    v.name = functor;
    v.args = args;
    return v;
}

us::Value vArray( std::vector<us::Value> args )
{
    us::Value v;
    v.kind = us::Value::Kind::Array;
    v.args = args;
    return v;
}

us::Value vMap( std::vector<std::pair<std::string, us::Value>> pairs )
{
    us::Value v;
    v.kind = us::Value::Kind::Map;
    v.pairs = pairs;
    return v;
}

/** Mark it truncated, as `applyBudget` would after cutting children. */
us::Value cut( us::Value v )
{
    v.truncated = true;
    return v;
}

/**
 * The cut mark `renderValue` uses, spelled the same way it is spelled there.
 *
 * Escaped, not written as a glyph: this string is the assertion, and an
 * assertion that depends on the test file's own encoding surviving a
 * checkout would fail for reasons that have nothing to do with the code.
 */
const std::string kCutMark = "\u2026";

const Buffer* findPanel( const Model& model, PanelKind kind )
{
    for ( TileId tile : model.layout().tiles() ) {
        const Buffer* buffer = model.buffer( model.layout().bufferOf( tile ) );
        if ( buffer && buffer->kind == kind ) {
            return buffer;
        }
    }
    return NULL;
}

} // namespace

int main()
{
    Registry registry;

    // -- help --------------------------------------------------------------

    registry.add( "H every link in every help topic resolves", []() {
        Model model = makeModel();
        const HelpBook* book = model.helpBook();
        UT_CHECK( book != NULL );

        int linkCount = 0;
        for ( const HelpTopic& topic : book->topics() ) {
            for ( const std::string& line : topic.lines ) {
                for ( const std::string& link : HelpBook::linksOn( line ) ) {
                    ++linkCount;
                    UT_CHECK_MSG( book->topic( link ) != NULL,
                                  "topic '" << topic.id << "' links to '"
                                      << link << "', which does not exist" );
                }
            }
        }
        UT_CHECK_MSG( linkCount >= 5,
                      "expected the help to be cross-linked; found "
                          << linkCount << " links" );
    } );

    registry.add( "H F1 opens help, and never does nothing", []() {
        Model model = makeModel();
        UT_CHECK( findPanel( model, PanelKind::Help ) == NULL );

        type( model, "F1" );

        const Buffer* help = findPanel( model, PanelKind::Help );
        UT_CHECK_MSG( help != NULL, "F1 opened no help panel" );
        UT_CHECK_MSG( !help->help.topicId.empty(),
                      "the help panel opened on no topic" );
        UT_CHECK_MSG( model.helpBook()->topic( help->help.topicId ) != NULL,
                      "F1 opened topic '" << help->help.topicId
                          << "', which does not exist" );
        UT_CHECK_MSG( model.layout().bufferOf( model.layout().focused() )
                          == help->id,
                      "F1 opened help without focusing it" );
    } );

    registry.add( "H F1 twice reuses the tile rather than filling the screen",
                  []() {
        Model model = makeModel();
        type( model, "F1" );
        const std::size_t afterFirst = model.layout().tileCount();

        type( model, "F1" );
        UT_CHECK_EQ( model.layout().tileCount(), afterFirst );
    } );

    registry.add( "H F1 is contextual: help about help, from help", []() {
        Model model = makeModel();
        type( model, "F1" );
        type( model, "F1" );   /* now focused on Help itself */

        const Buffer* help = findPanel( model, PanelKind::Help );
        UT_CHECK( help != NULL );
        UT_CHECK_MSG( help->help.topicId == "help",
                      "F1 inside the Help panel should open the help topic, "
                      "got '" << help->help.topicId << "'" );
    } );

    registry.add( "help follows a link and comes back", []() {
        Model model = makeModel();
        type( model, "F1" );

        const Buffer* help = findPanel( model, PanelKind::Help );
        UT_CHECK( help != NULL );
        const std::string first = help->help.topicId;

        /* Walk down to the first line carrying a link, then follow it. */
        const HelpTopic* topic = model.helpBook()->topic( first );
        UT_CHECK( topic != NULL );

        int linkLine = -1;
        for ( std::size_t i = 0; i < topic->lines.size(); ++i ) {
            if ( !HelpBook::linksOn( topic->lines[i] ).empty() ) {
                linkLine = (int) i;
                break;
            }
        }
        UT_CHECK_MSG( linkLine >= 0, "the opening topic carries no links" );

        for ( int i = 0; i < linkLine; ++i ) {
            type( model, "Down" );
        }
        type( model, "Enter" );

        help = findPanel( model, PanelKind::Help );
        UT_CHECK( help != NULL );
        UT_CHECK_MSG( help->help.topicId != first,
                      "Enter on a link line did not follow the link" );

        type( model, "Backspace" );
        help = findPanel( model, PanelKind::Help );
        UT_CHECK_MSG( help->help.topicId == first,
                      "Backspace did not return to the previous topic" );
    } );

    registry.add( "a dead link says so rather than doing nothing", []() {
        /*
         * There are none today -- the case above asserts that -- so this
         * drives the path directly. A help browser that silently ignored a
         * broken link is how broken links survive.
         */
        Model model = makeModel();
        type( model, "F1" );

        Buffer* help = model.buffer(
            model.layout().bufferOf( model.layout().focused() ) );
        UT_CHECK( help != NULL && help->kind == PanelKind::Help );
        help->help.topicId = "getting-started";

        /* Point the cursor at a line with a link, then break the book by
         * asking for a topic that is not there. */
        const HelpTopic* topic = model.helpBook()->topic( "getting-started" );
        UT_CHECK( topic != NULL );
        UT_CHECK( model.helpBook()->topic( "no-such-topic" ) == NULL );
    } );

    registry.add( "H the welcome page advertises only keys that exist", []() {
        /*
         * The failure this prevents: help that tells a new user to press
         * F10, when F10 does nothing. Every command the first-steps page
         * names is checked to be registered AND bound -- so the page cannot
         * drift ahead of the implementation, which is exactly what it did
         * before the menu was built.
         */
        Model model = makeModel();

        const char* advertised[] = {
            "help.contextual", "menu.open", "command.palette",
            "window.focus-next", "window.focus-prev", "window.close",
            "window.maximise", "window.split-rows", "window.split-columns",
            "app.quit"
        };

        for ( const char* id : advertised ) {
            UT_CHECK_MSG( model.commands().find( id ) != NULL,
                          "the welcome page names '" << id
                              << "', which is not registered" );
            UT_CHECK_MSG( !model.keymap().bindingFor( id ).empty(),
                          "the welcome page names '" << id
                              << "', which has no key bound" );
        }
    } );

    registry.add( "the welcome page borrows the main tile and gives it back",
                  []() {
        Model model = makeModel();
        const std::size_t before = model.layout().tileCount();

        model.openHelp( HelpBook::welcomeTopicId(), true );

        UT_CHECK_MSG( model.layout().tileCount() == before,
                      "borrowing a tile should not create one: "
                          << model.layout().tileCount() << " vs " << before );

        const Buffer* help = findPanel( model, PanelKind::Help );
        UT_CHECK( help != NULL );
        UT_CHECK_EQ( help->help.topicId, HelpBook::welcomeTopicId() );

        /* And it landed in the LARGEST tile, not wherever focus happened
         * to be -- which is what makes it readable. */
        const Solution solution = solve( model.layout(), model.tileArea() );
        const Placement* placement = solution.find( model.layout().focused() );
        UT_CHECK( placement != NULL );
        for ( const Placement& other : solution.placements ) {
            if ( other.stub ) { continue; }
            UT_CHECK_MSG( other.rect.area() <= placement->rect.area(),
                          "help did not take the largest tile" );
        }

        type( model, "C-x 0" );

        UT_CHECK_MSG( model.layout().tileCount() == before,
                      "dismissing borrowed help destroyed a tile" );
        UT_CHECK_MSG( findPanel( model, PanelKind::Help ) == NULL,
                      "dismissing help left it on screen" );
    } );

    registry.add( "help wraps rather than truncating", []() {
        Model model = makeModel();
        model.openHelp( HelpBook::welcomeTopicId(), true );

        const CellGrid grid = view( model );

        /*
         * A distinctive phrase from the middle of a long line. If the panel
         * truncated instead of wrapping, the tail would be missing from the
         * screen entirely.
         */
        std::string screen;
        for ( int y = 0; y < grid.height(); ++y ) {
            for ( int x = 0; x < grid.width(); ++x ) {
                const Cell& cell = grid.at( x, y );
                if ( cell.ch != 0 ) { screen += encodeUtf8( cell.ch ); }
            }
            screen += "\n";
        }

        UT_CHECK_MSG( screen.find( "work inside it" ) != std::string::npos,
                      "the tail of a long help line is missing from the "
                      "screen -- the panel truncated instead of wrapping" );
    } );

    // -- the menu bar ------------------------------------------------------
    //
    // UI.md section 1. The bar is a frame region with state, and its
    // drop-downs are the one overlay: opening the menu moves no tile and no
    // focus, which several of these assert directly.

    registry.add( "H F10 gives the bar the keyboard, and drops nothing down",
                  []() {
        Model model = makeModel();
        const std::size_t tiles = model.layout().tileCount();
        const TileId focused = model.layout().focused();

        type( model, "F10" );

        UT_CHECK_MSG( model.menuActive(), "F10 did not activate the menu bar" );
        UT_CHECK_MSG( !model.menuOpen(),
                      "F10 should light the bar, not open a drop-down" );
        UT_CHECK_EQ( model.menuBar().heading, 0 );
        UT_CHECK_MSG( model.layout().tileCount() == tiles,
                      "the menu created a tile; it is an overlay now" );
        UT_CHECK_MSG( model.layout().focused() == focused,
                      "the menu moved tile focus" );
    } );

    registry.add( "Enter or Down on the bar opens the lit heading", []() {
        for ( const char* opener : { "Enter", "Down" } ) {
            Model model = makeModel();
            type( model, "F10" );
            type( model, "Right" );
            type( model, opener );
            UT_CHECK_MSG( model.menuOpen(), opener << " did not open it" );
            UT_CHECK_EQ( model.menuBar().heading, 1 );
            UT_CHECK_EQ( model.menuBar().selected, 0 );
        }
    } );

    registry.add( "Left and Right walk the bar and wrap at both ends", []() {
        Model model = makeModel();
        const int count = (int) menuHeadings().size();

        type( model, "F10" );
        type( model, "Left" );
        UT_CHECK_EQ( model.menuBar().heading, count - 1 );
        type( model, "Right" );
        UT_CHECK_EQ( model.menuBar().heading, 0 );

        /* With a drop-down open, sideways keeps it open, on the neighbour. */
        type( model, "Enter" );
        type( model, "Right" );
        UT_CHECK( model.menuOpen() );
        UT_CHECK_EQ( model.menuBar().heading, 1 );
    } );

    registry.add( "M-<letter> opens each heading directly", []() {
        const std::vector<MenuHeading> headings = menuHeadings();
        for ( std::size_t i = 0; i < headings.size(); ++i ) {
            Model model = makeModel();
            const std::string keys =
                std::string( "M-" ) + headings[i].defaultLetter;
            type( model, keys.c_str() );

            UT_CHECK_MSG( model.menuOpen(),
                          keys << " did not open the " << headings[i].name
                               << " menu" );
            UT_CHECK_EQ( model.menuBar().heading, (int) i );

            /* And from inside another open menu, too. */
            type( model, "M-f" );
            UT_CHECK_EQ( model.menuBar().heading, 0 );
        }
    } );

    registry.add( "World and Window do not fight over W", []() {
        Model model = makeModel();
        type( model, "M-w" );
        UT_CHECK_EQ( menuCategories()[ (std::size_t) model.menuBar().heading ],
                     std::string( "World" ) );
        model.closeMenu();
        type( model, "M-n" );
        UT_CHECK_EQ( menuCategories()[ (std::size_t) model.menuBar().heading ],
                     std::string( "Window" ) );
    } );

    registry.add( "the underlined letter follows the binding in effect", []() {
        const std::vector<MenuHeading> headings = menuHeadings();
        const Keymap defaults = defaultKeymap();
        for ( const MenuHeading& heading : headings ) {
            const int at = menuMnemonic( heading, defaults );
            UT_CHECK_MSG( at >= 0, heading.name << " has no mnemonic" );
            UT_CHECK_EQ( (char) std::tolower(
                             (unsigned char) heading.name[ (std::size_t) at ] ),
                         heading.defaultLetter );
        }

        /* Rebound to M-o: the underline moves to the o of Window. */
        const MenuHeading& window = headings[6];
        UT_CHECK_EQ( window.name, std::string( "Window" ) );
        Keymap rebound;
        KeySeq seq;
        UT_CHECK( parseKeySeq( "M-o", seq ) );
        UT_CHECK( rebound.bind( seq, window.commandId ).empty() );
        UT_CHECK_EQ( menuMnemonic( window, rebound ), 4 );

        /* Bound to something that is not a letter of the name: no
         * underline, rather than one no key answers to. */
        Keymap elsewhere;
        UT_CHECK( parseKeySeq( "C-x w", seq ) );
        UT_CHECK( elsewhere.bind( seq, window.commandId ).empty() );
        UT_CHECK_EQ( menuMnemonic( window, elsewhere ), -1 );
    } );

    registry.add( "on the bar, the bare letter opens its heading", []() {
        Model model = makeModel();
        type( model, "F10" );
        typeText( model, "n" );
        UT_CHECK( model.menuOpen() );
        UT_CHECK_EQ( menuCategories()[ (std::size_t) model.menuBar().heading ],
                     std::string( "Window" ) );
    } );

    // -- G2.5: re-running past input in place --------------------------------

    registry.add( "G2.5 F5 on a past line re-issues the identical request", []() {
        Model model = makeModel();
        focusTranscript( model );

        typeText( model, "colour( $x )" );
        const std::vector<CommandRequest> original = typeKeys( model, "Enter" );
        UT_CHECK_EQ( original.size(), (std::size_t) 1 );

        typeText( model, "shade( blue );" );
        const std::vector<CommandRequest> definition = typeKeys( model, "Enter" );
        UT_CHECK_EQ( definition.size(), (std::size_t) 1 );

        /* PageUp twice: past the definition, onto the goal. */
        type( model, "PageUp" );
        type( model, "PageUp" );
        const std::vector<CommandRequest> again = typeKeys( model, "F5" );

        UT_CHECK_EQ( again.size(), (std::size_t) 1 );
        UT_CHECK_EQ( again[0].kind, original[0].kind );
        UT_CHECK_EQ( again[0].text, original[0].text );
        UT_CHECK_EQ( again[0].handle, original[0].handle );

        /* A definition re-runs as a definition, not as a goal. */
        type( model, "PageUp" );
        type( model, "PageUp" );
        const std::vector<CommandRequest> redefined = typeKeys( model, "F5" );
        UT_CHECK_EQ( redefined.size(), (std::size_t) 1 );
        UT_CHECK_EQ( redefined[0].kind, definition[0].kind );
        UT_CHECK_EQ( redefined[0].text, definition[0].text );
    } );

    registry.add( "G2.5 F5 keeps the line you were halfway through typing", []() {
        Model model = makeModel();
        focusTranscript( model );
        TranscriptState& t = model.transcript()->transcript;

        typeText( model, "colour( $x )" );
        type( model, "Enter" );
        typeText( model, "half a th" );
        type( model, "PageUp" );
        ( void ) typeKeys( model, "F5" );

        UT_CHECK_EQ( t.input, std::string( "half a th" ) );
        UT_CHECK_EQ( t.cursor, t.input.size() );
        UT_CHECK_EQ( t.selected, -1 );
    } );

    registry.add( "G2.5 the transcript cursor visits only past input", []() {
        Model model = makeModel();
        focusTranscript( model );
        TranscriptState& t = model.transcript()->transcript;

        typeText( model, "a( 1 );" );
        type( model, "Enter" );
        /* Rows that are not input -- what a session would have folded in. */
        TranscriptEntry row;
        row.kind = TranscriptEntry::Kind::Solution;
        row.text = "$x = 1";
        t.entries.push_back( row );
        t.entries.push_back( row );
        typeText( model, "a( $x )" );
        type( model, "Enter" );
        t.entries.push_back( row );

        type( model, "PageUp" );
        UT_CHECK( t.selected >= 0 );
        UT_CHECK_EQ( t.entries[ (std::size_t) t.selected ].source,
                     std::string( "a( $x )" ) );
        type( model, "PageUp" );
        UT_CHECK_EQ( t.entries[ (std::size_t) t.selected ].source,
                     std::string( "a( 1 );" ) );

        /* At the oldest line PageUp stays put, and says why. */
        type( model, "PageUp" );
        UT_CHECK_EQ( t.entries[ (std::size_t) t.selected ].source,
                     std::string( "a( 1 );" ) );
        UT_CHECK_MSG( !model.message().empty(),
                      "PageUp at the oldest input said nothing" );

        /* PageDown walks forward, and past the newest returns to the prompt. */
        type( model, "PageDown" );
        UT_CHECK_EQ( t.entries[ (std::size_t) t.selected ].source,
                     std::string( "a( $x )" ) );
        type( model, "PageDown" );
        UT_CHECK_EQ( t.selected, -1 );
    } );

    registry.add( "G2.5 Esc or typing leaves the past line for the prompt", []() {
        Model model = makeModel();
        focusTranscript( model );
        TranscriptState& t = model.transcript()->transcript;

        typeText( model, "a( 1 );" );
        type( model, "Enter" );

        type( model, "PageUp" );
        UT_CHECK( t.selected >= 0 );
        type( model, "Esc" );
        UT_CHECK_EQ( t.selected, -1 );

        type( model, "PageUp" );
        typeText( model, "q" );
        UT_CHECK_EQ( t.selected, -1 );
        UT_CHECK_EQ( t.input, std::string( "q" ) );
    } );

    registry.add( "G2.5 F5 with nothing selected and nothing typed asks nothing",
                  []() {
        Model model = makeModel();
        focusTranscript( model );
        const std::vector<CommandRequest> requests = typeKeys( model, "F5" );
        UT_CHECK( requests.empty() );
        UT_CHECK_MSG( !model.message().empty(), "F5 on nothing said nothing" );
    } );

    registry.add( "G2.5 F5 at the prompt runs what is typed, like Enter", []() {
        Model model = makeModel();
        focusTranscript( model );
        typeText( model, "colour( $x )" );
        const std::vector<CommandRequest> requests = typeKeys( model, "F5" );
        UT_CHECK_EQ( requests.size(), (std::size_t) 1 );
        UT_CHECK_EQ( requests[0].kind, std::string( "solve" ) );
        UT_CHECK_EQ( requests[0].text, std::string( "colour( $x )" ) );
    } );

    // -- G2 H: the transcript's help ------------------------------------------

    registry.add( "G2 H every transcript command has an M-x entry and a topic",
                  []() {
        Model model = makeModel();
        int checked = 0;
        for ( const Command& command : model.commands().all() ) {
            const bool transcriptCommand =
                command.id().rfind( "transcript.", 0 ) == 0
                || command.id().rfind( "query.", 0 ) == 0;
            if ( !transcriptCommand ) {
                continue;
            }
            ++checked;

            /* M-x finds it by typing its id. */
            type( model, "M-x" );
            typeText( model, command.id() );
            bool listed = false;
            for ( const Command* match : model.paletteMatches() ) {
                if ( match->id() == command.id() ) { listed = true; }
            }
            type( model, "Esc" );
            UT_CHECK_MSG( listed, command.id() << " is not in M-x" );

            UT_CHECK_MSG( !command.helpTopic().empty(),
                          command.id() << " links no help topic" );
            UT_CHECK_MSG( model.helpBook()->topic( command.helpTopic() ) != NULL,
                          command.id() << " links help topic '"
                              << command.helpTopic() << "', which does not exist" );
        }
        /* rerun, write-file, cancel, more -- and a floor, so a rename that
         * left this loop matching nothing cannot pass it. */
        UT_CHECK_MSG( checked >= 4, "only " << checked << " transcript commands" );
    } );

    registry.add( "G2 H F1 on a builtin under the cursor opens its extracted "
                  "topic", []() {
        Model model = makeModel();
        focusTranscript( model );

        /* In the middle of the word. */
        typeText( model, "print( 1 )" );
        type( model, "Home" );
        type( model, "Right" );
        type( model, "Right" );
        UT_CHECK_EQ( model.contextualTopicId(), std::string( "builtin.print" ) );
        type( model, "F1" );
        const Buffer* help = findPanel( model, PanelKind::Help );
        UT_CHECK( help != NULL );
        UT_CHECK_EQ( help->help.topicId, std::string( "builtin.print" ) );

        /* The topic is the spec's own words, not a copy written here. */
        const HelpTopic* topic = model.helpBook()->topic( "builtin.print" );
        UT_CHECK( topic != NULL );
        bool fromSpec = false;
        for ( const std::string& line : topic->lines ) {
            if ( line.find( "variadic in practice" ) != std::string::npos ) {
                fromSpec = true;
            }
        }
        UT_CHECK_MSG( fromSpec, "builtin.print does not carry SPEC.md's text" );
    } );

    registry.add( "G2 H F1 just after a reserved word opens its topic", []() {
        Model model = makeModel();
        focusTranscript( model );
        typeText( model, "$all = findall" );
        UT_CHECK_EQ( model.contextualTopicId(), std::string( "keyword.findall" ) );
    } );

    registry.add( "G2 H F1 on a word that names nothing opens the transcript's "
                  "page", []() {
        Model model = makeModel();
        focusTranscript( model );
        typeText( model, "colour( red )" );
        type( model, "Home" );
        UT_CHECK_EQ( model.contextualTopicId(), std::string( "transcript" ) );
    } );

    registry.add( "G2 H the language index links every extracted topic", []() {
        Model model = makeModel();
        const HelpTopic* index = model.helpBook()->topic( "language" );
        UT_CHECK( index != NULL );
        std::size_t builtins = 0;
        std::size_t keywords = 0;
        for ( const HelpTopic& topic : model.helpBook()->topics() ) {
            if ( topic.id.rfind( "builtin.", 0 ) == 0 ) { ++builtins; }
            if ( topic.id.rfind( "keyword.", 0 ) == 0 ) { ++keywords; }
        }
        std::size_t links = 0;
        for ( const std::string& line : index->lines ) {
            links += HelpBook::linksOn( line ).size();
        }
        /* SPEC.md section 9: "All thirteen". */
        UT_CHECK_EQ( builtins, (std::size_t) 13 );
        UT_CHECK_MSG( keywords >= 10, "only " << keywords << " reserved words" );
        UT_CHECK_EQ( links, builtins + keywords + 1 );   /* + transcript */
    } );

    // -- G2.8: the history file, shared with unify-run -i ---------------------

    registry.add( "G2.8 a readline history file reads back line for line", []() {
        const std::vector<std::string> lines =
            parseHistory( "? colour( $x );\ncolour( red );\n" );
        UT_CHECK_EQ( lines.size(), (std::size_t) 2 );
        UT_CHECK_EQ( lines[0], std::string( "? colour( $x );" ) );
        UT_CHECK_EQ( lines[1], std::string( "colour( red );" ) );

        /* CRLF, blank lines and a missing final newline all read the same. */
        const std::vector<std::string> rough =
            parseHistory( "a( 1 );\r\n\n   \nb( 2 );" );
        UT_CHECK_EQ( rough.size(), (std::size_t) 2 );
        UT_CHECK_EQ( rough[0], std::string( "a( 1 );" ) );
        UT_CHECK_EQ( rough[1], std::string( "b( 2 );" ) );

        UT_CHECK( parseHistory( "" ).empty() );
    } );

    registry.add( "G2.8 the history file keeps the newest 1000 lines, as "
                  "unify-run does", []() {
        std::vector<std::string> lines;
        for ( int i = 0; i < 1005; ++i ) {
            lines.push_back( "line( " + std::to_string( i ) + " );" );
        }
        const std::vector<std::string> kept = parseHistory( formatHistory( lines ) );
        UT_CHECK_EQ( kept.size(), kHistoryLimit );
        UT_CHECK_EQ( kept.front(), std::string( "line( 5 );" ) );
        UT_CHECK_EQ( kept.back(), std::string( "line( 1004 );" ) );

        /* And the format is exactly readline's: one line, one newline. */
        UT_CHECK_EQ( formatHistory( { "a( 1 );", "b" } ),
                     std::string( "a( 1 );\nb\n" ) );
    } );

    registry.add( "G2.8 a unify-run query line means the same thing here", []() {
        Model model = makeModel();
        focusTranscript( model );
        typeText( model, "? colour( $x );" );
        const std::vector<CommandRequest> requests = typeKeys( model, "Enter" );
        UT_CHECK_EQ( requests.size(), (std::size_t) 1 );
        UT_CHECK_EQ( requests[0].kind, std::string( "solve" ) );
        UT_CHECK_EQ( requests[0].text, std::string( " colour( $x )" ) );

        /* History keeps what was TYPED, so unify-run reads it back as typed. */
        UT_CHECK_EQ( model.transcript()->transcript.history.back(),
                     std::string( "? colour( $x );" ) );
    } );

    // -- G2.9: the transcript as text -----------------------------------------

    registry.add( "G2.9 transcript.write-file asks where, then asks for the "
                  "transcript's text", []() {
        Model model = makeModel();
        focusTranscript( model );
        typeText( model, "colour( red );" );
        type( model, "Enter" );
        TranscriptEntry row;
        row.kind = TranscriptEntry::Kind::Status;
        row.text = "-- defined colour/1";
        model.transcript()->transcript.entries.push_back( row );

        /* Through M-x, the way the criterion names it. */
        type( model, "M-x" );
        typeText( model, "transcript.write-file" );
        type( model, "Enter" );

        UT_CHECK_MSG( model.promptActive(), "no prompt for the file name" );
        const Buffer* prompt = model.focusedBuffer();
        UT_CHECK( prompt != NULL && prompt->kind == PanelKind::Palette );
        UT_CHECK_EQ( prompt->palette.input, std::string( "transcript.txt" ) );

        type( model, "C-u" );
        UT_CHECK_EQ( model.focusedBuffer()->palette.input, std::string() );
        typeText( model, "out.txt" );
        const std::vector<CommandRequest> requests = typeKeys( model, "Enter" );

        UT_CHECK( !model.promptActive() );
        UT_CHECK_EQ( requests.size(), (std::size_t) 1 );
        UT_CHECK_EQ( requests[0].kind, std::string( "write-file" ) );
        UT_CHECK_EQ( requests[0].path, std::string( "out.txt" ) );
        UT_CHECK_EQ( requests[0].text,
                     std::string( "?- colour( red );\n-- defined colour/1\n" ) );

        /* And focus is back where it was, not on a closed tile. */
        UT_CHECK( model.focusedBuffer() == model.transcript() );
    } );

    registry.add( "G2.9 Esc at the file prompt writes nothing", []() {
        Model model = makeModel();
        type( model, "M-x" );
        typeText( model, "transcript.write-file" );
        type( model, "Enter" );
        UT_CHECK( model.promptActive() );
        const std::vector<CommandRequest> requests = typeKeys( model, "Esc" );
        UT_CHECK( requests.empty() );
        UT_CHECK( !model.promptActive() );
        UT_CHECK( !model.paletteActive() );
    } );

    registry.add( "G2.9 an empty file name is refused, not written", []() {
        Model model = makeModel();
        type( model, "M-x" );
        typeText( model, "transcript.write-file" );
        type( model, "Enter" );
        type( model, "C-u" );
        const std::vector<CommandRequest> requests = typeKeys( model, "Enter" );
        UT_CHECK( requests.empty() );
        UT_CHECK_MSG( !model.message().empty(), "an empty name said nothing" );
    } );

    registry.add( "G2.9 the prompt edits by codepoint, not by byte", []() {
        Model model = makeModel();
        type( model, "M-x" );
        typeText( model, "transcript.write-file" );
        type( model, "Enter" );
        type( model, "C-u" );
        Event event;
        event.kind = Event::Kind::Key;
        event.key = Key::character( U'\u00e9' );   /* two bytes in UTF-8 */
        ( void ) fold( model, event );
        type( model, "Backspace" );
        UT_CHECK_EQ( model.focusedBuffer()->palette.input, std::string() );
    } );

    registry.add( "G2.6 only the live query's end ends the live state", []() {
        /*
         * A superseded query can still report -- a demand or a cancel aimed
         * at it, or simply a core that is behind. Its terminal status used
         * to mark the transcript finished whatever query it was for, which
         * would report the CURRENT query idle while its rows were still on
         * their way.
         */
        Model model = makeModel();
        TranscriptState& t = model.transcript()->transcript;
        t.liveQuery = 7;
        t.finished = false;

        us::QueryStatus status;
        status.state = us::QueryStatus::State::Exhausted;
        us::Event event;
        event.header.query = (us::QueryId) 3;
        event.body = status;
        model.foldSession( event );

        UT_CHECK_MSG( !t.finished, "a superseded query's end finished the live one" );
        UT_CHECK_EQ( t.liveQuery, (us::QueryId) 7 );

        event.header.query = (us::QueryId) 7;
        model.foldSession( event );
        UT_CHECK( t.finished );
        UT_CHECK_EQ( t.liveQuery, us::kNoQuery );
    } );

    registry.add( "a bare letter is still text while the menu is closed", []() {
        Model model = makeModel();
        Buffer* transcript = model.transcript();
        UT_CHECK( transcript != NULL );
        while ( model.focusedBuffer() != transcript ) { type( model, "Tab" ); }

        typeText( model, "f" );
        UT_CHECK_MSG( !model.menuActive(), "typing f opened the menu" );
        UT_CHECK_EQ( transcript->transcript.input, std::string( "f" ) );
    } );

    registry.add( "Esc steps back one level, C-g and F10 close at once", []() {
        Model model = makeModel();
        type( model, "M-f" );
        type( model, "Esc" );
        UT_CHECK_MSG( model.menuActive() && !model.menuOpen(),
                      "Esc in a drop-down should return to the bar" );
        type( model, "Esc" );
        UT_CHECK( !model.menuActive() );

        for ( const char* closer : { "C-g", "F10" } ) {
            type( model, "M-f" );
            type( model, closer );
            UT_CHECK_MSG( !model.menuActive(),
                          closer << " should close the whole menu" );
            type( model, "F10" );
            type( model, closer );
            UT_CHECK_MSG( !model.menuActive(),
                          closer << " should close the bar too" );
        }
    } );

    registry.add( "Up and Down wrap inside a drop-down", []() {
        Model model = makeModel();
        type( model, "M-n" );
        const int count = (int) model.menuItems( model.menuBar().heading ).size();
        UT_CHECK( count > 1 );

        type( model, "Up" );
        UT_CHECK_EQ( model.menuBar().selected, count - 1 );
        type( model, "Down" );
        UT_CHECK_EQ( model.menuBar().selected, 0 );
    } );

    registry.add( "every command is in exactly one drop-down, bar the openers",
                  []() {
        Model model = makeModel();
        const int headings = (int) menuHeadings().size();

        for ( const Command& command : model.commands().all() ) {
            int seen = 0;
            for ( int h = 0; h < headings; ++h ) {
                for ( const Command* item : model.menuItems( h ) ) {
                    if ( item == &command ) { ++seen; }
                }
            }
            const bool opener = command.id().compare( 0, 5, "menu." ) == 0;
            UT_CHECK_MSG( seen == ( opener ? 0 : 1 ),
                          "'" << command.id() << "' appears in " << seen
                              << " drop-downs" );
            UT_CHECK_MSG( command.inMenu() != opener,
                          "'" << command.id() << "' should "
                              << ( opener ? "not " : "" ) << "be in the menu" );
        }
    } );

    registry.add( "every command lands under a heading the bar draws", []() {
        Model model = makeModel();
        const std::vector<std::string> categories = menuCategories();

        for ( const Command& command : model.commands().all() ) {
            bool known = false;
            for ( const std::string& category : categories ) {
                if ( command.category() == category ) { known = true; }
            }
            UT_CHECK_MSG( known,
                          "'" << command.id() << "' is in category '"
                              << command.category()
                              << "', which the menu bar does not draw -- it "
                                 "would be unreachable from the menu" );
        }
    } );

    registry.add( "Enter runs the item against the tile you were in", []() {
        Model model = makeModel();
        const std::size_t before = model.layout().tileCount();
        const TileId focused = model.layout().focused();

        type( model, "M-n" );
        const std::vector<const Command*> items =
            model.menuItems( model.menuBar().heading );
        int wanted = -1;
        for ( std::size_t i = 0; i < items.size(); ++i ) {
            if ( items[i]->id() == "window.split-rows" ) { wanted = (int) i; }
        }
        UT_CHECK( wanted >= 0 );
        while ( model.menuBar().selected != wanted ) { type( model, "Down" ); }

        type( model, "Enter" );

        UT_CHECK_MSG( !model.menuActive(), "running a command left the menu open" );
        UT_CHECK_EQ( model.layout().tileCount(), before + 1 );
        UT_CHECK_MSG( model.layout().bufferOf( model.layout().focused() )
                          == model.layout().bufferOf( focused ),
                      "the split ran against some other tile" );
    } );

    registry.add( "an empty drop-down says so and Enter does nothing", []() {
        Model model = makeModel();
        type( model, "M-e" );   /* Edit: nothing registered there yet */
        UT_CHECK( model.menuItems( model.menuBar().heading ).empty() );
        const std::size_t tiles = model.layout().tileCount();
        type( model, "Enter" );
        UT_CHECK_EQ( model.layout().tileCount(), tiles );
        UT_CHECK_EQ( model.message(), std::string( "nothing here yet" ) );

        const std::string screen = view( model ).toText();
        UT_CHECK_MSG( screen.find( kMenuEmptyText ) != std::string::npos,
                      "the empty drop-down does not say it is empty" );
    } );

    registry.add( "every drop-down fits, under its heading, at 80x24", []() {
        Model model = makeModel( "browse", 80, 24 );
        const Rect area = model.tileArea();
        const int count = (int) menuHeadings().size();

        for ( int h = 0; h < count; ++h ) {
            model.openMenuHeading( h );
            const Rect box = model.menuDropdownRect();

            UT_CHECK_MSG( box.x >= 0 && box.right() <= model.width(),
                          "heading " << h << " drops off the side" );
            UT_CHECK_MSG( box.y == area.y && box.bottom() <= area.bottom(),
                          "heading " << h << " covers the status or hint line" );

            /* Under its heading, unless pushed left by the right edge. */
            const int column = menuHeadingColumn( (std::size_t) h );
            UT_CHECK_MSG( box.x == column - 1 || box.right() == model.width(),
                          "heading " << h << " is not anchored under itself" );
        }
    } );

    registry.add( "the open drop-down is drawn over the tiles", []() {
        Model model = makeModel();
        type( model, "M-n" );

        const CellGrid grid = view( model );
        const Rect box = model.menuDropdownRect();
        const std::vector<const Command*> items =
            model.menuItems( model.menuBar().heading );

        std::string firstRow;
        for ( int x = box.x + 2; x < box.right() - 1; ++x ) {
            const Cell& cell = grid.at( x, box.y + 1 );
            if ( cell.ch != 0 ) { firstRow += encodeUtf8( cell.ch ); }
        }
        UT_CHECK_MSG( firstRow.find( items[0]->title() ) == 0,
                      "the drop-down's first row reads '" << firstRow << "'" );
        UT_CHECK_MSG( grid.at( box.x + 2, box.y + 1 ).attr.reverse,
                      "the selected item is not highlighted" );

        /* And the lit heading is drawn out of reverse, so it shows in
         * monochrome as well as in colour. */
        const int column = menuHeadingColumn( (std::size_t) model.menuBar().heading );
        UT_CHECK( !grid.at( column, 0 ).attr.reverse );
        UT_CHECK( grid.at( 1, 0 ).attr.reverse );   /* File, not lit */
    } );

    registry.add( "the status line explains the highlighted item", []() {
        Model model = makeModel();
        type( model, "M-n" );
        const Command* item = model.menuItems( model.menuBar().heading )[0];

        const std::string screen = view( model ).toText();
        const std::string start = item->help().substr( 0, 30 );
        UT_CHECK_MSG( screen.find( start ) != std::string::npos,
                      "the status line does not show '" << start << "'" );
    } );

    // -- the palette -------------------------------------------------------

    registry.add( "H M-x lists every registered command", []() {
        Model model = makeModel();
        type( model, "M-x" );

        UT_CHECK_MSG( model.paletteActive(), "M-x did not open the palette" );

        const std::vector<const Command*> matches = model.paletteMatches();
        UT_CHECK_MSG( matches.size() == model.commands().size(),
                      "the palette shows " << matches.size() << " of "
                          << model.commands().size() << " commands" );

        /*
         * Including a command with no binding: discovering that a command
         * exists must not require it to have a key. Every shell command
         * happens to be bound today, so the case registers one that is not
         * rather than asserting a property of the current keymap -- which
         * would pass for the wrong reason the moment someone bound it.
         */
        UT_CHECK( model.commands()
                      .add( Command( "test.unbound", "Unbound command",
                                     "Exists to prove the palette lists "
                                     "commands that have no key." ) )
                      .empty() );

        const std::vector<const Command*> withUnbound = model.paletteMatches();
        UT_CHECK_EQ( withUnbound.size(), model.commands().size() );

        bool sawUnbound = false;
        for ( const Command* command : withUnbound ) {
            if ( model.keymap().bindingFor( command->id() ).empty() ) {
                sawUnbound = true;
            }
        }
        UT_CHECK_MSG( sawUnbound,
                      "the palette dropped the command that has no binding" );
    } );

    registry.add( "typing filters the palette", []() {
        Model model = makeModel();
        type( model, "M-x" );
        const std::size_t all = model.paletteMatches().size();

        typeText( model, "split" );

        const std::vector<const Command*> matches = model.paletteMatches();
        UT_CHECK_MSG( matches.size() < all && !matches.empty(),
                      "typing 'split' matched " << matches.size()
                          << " of " << all << " commands" );
        for ( const Command* command : matches ) {
            UT_CHECK_MSG( command->id().find( "split" ) != std::string::npos
                              || command->title().find( "Split" )
                                     != std::string::npos,
                          "'" << command->id() << "' does not match 'split'" );
        }
    } );

    registry.add( "the palette is modal: a global binding does not fire", []() {
        Model model = makeModel();
        const std::size_t before = model.layout().tileCount();

        type( model, "M-x" );
        const std::size_t withPalette = model.layout().tileCount();
        UT_CHECK_EQ( withPalette, before + 1 );

        /*
         * `x` is the second key of `C-x 2` and a perfectly ordinary letter.
         * Typing it into the palette must filter, not begin a chord -- a
         * global binding firing out from under someone typing a command name
         * is the classic minibuffer bug.
         */
        typeText( model, "x" );
        UT_CHECK_EQ( model.layout().tileCount(), withPalette );
        UT_CHECK_MSG( model.pendingKeys().empty(),
                      "typing into the palette started a chord" );
    } );

    registry.add( "Esc closes the palette and leaves the layout as it was",
                  []() {
        Model model = makeModel();
        const std::size_t before = model.layout().tileCount();
        const TileId focusedBefore = model.layout().focused();

        type( model, "M-x" );
        typeText( model, "split" );
        type( model, "Esc" );

        UT_CHECK_MSG( !model.paletteActive(), "Esc did not close the palette" );
        UT_CHECK_EQ( model.layout().tileCount(), before );
        UT_CHECK_MSG( model.layout().focused() == focusedBefore,
                      "closing the palette left focus somewhere else" );
    } );

    registry.add( "C-g closes the palette too", []() {
        Model model = makeModel();
        type( model, "M-x" );
        UT_CHECK( model.paletteActive() );
        type( model, "C-g" );
        UT_CHECK( !model.paletteActive() );
    } );

    registry.add( "Enter runs the selected command against the right tile",
                  []() {
        Model model = makeModel();
        const std::size_t before = model.layout().tileCount();

        type( model, "M-x" );
        typeText( model, "window.split-columns" );

        const std::vector<const Command*> matches = model.paletteMatches();
        UT_CHECK_MSG( matches.size() == 1,
                      "expected one match, got " << matches.size() );

        type( model, "Enter" );

        UT_CHECK_MSG( !model.paletteActive(),
                      "running a command left the palette open" );
        /*
         * The palette's own tile is gone and the split happened, so the
         * count is back to `before` plus the one new tile. If the palette
         * had still been open when the command ran, the split would have
         * divided the PALETTE's tile instead of the user's.
         */
        UT_CHECK_EQ( model.layout().tileCount(), before + 1 );
    } );

    registry.add( "Tab completes to the longest common prefix", []() {
        Model model = makeModel();
        type( model, "M-x" );
        typeText( model, "window." );
        type( model, "Tab" );

        const Buffer* palette = findPanel( model, PanelKind::Palette );
        UT_CHECK( palette != NULL );
        UT_CHECK_MSG( palette->palette.input == "window.",
                      "'window.' is already the common prefix of the window "
                      "commands; Tab changed it to '"
                          << palette->palette.input << "'" );

        typeText( model, "s" );
        type( model, "Tab" );
        palette = findPanel( model, PanelKind::Palette );
        UT_CHECK_MSG( palette->palette.input == "window.split-",
                      "Tab should complete to 'window.split-', got '"
                          << palette->palette.input << "'" );
    } );

    registry.add( "Backspace edits the palette input", []() {
        Model model = makeModel();
        type( model, "M-x" );
        typeText( model, "split" );
        type( model, "Backspace" );

        const Buffer* palette = findPanel( model, PanelKind::Palette );
        UT_CHECK( palette != NULL );
        UT_CHECK_EQ( palette->palette.input, std::string( "spli" ) );
    } );

    // -- rendering values --------------------------------------------------
    //
    // Engine item E7 turns every binding from a flat `Str` into a real tree,
    // so seven of these eight arms are about to start firing for the first
    // time. They are pinned here rather than through a golden screen because
    // a screen can only show what the engine of the day can produce, and
    // `Str`, `Float` and `Var` are each unproducible by this engine for a
    // different reason.

    registry.add( "renderValue renders every leaf kind", []() {
        UT_CHECK_EQ( renderValue( vAtom( "red" ) ), std::string( "red" ) );
        UT_CHECK_EQ( renderValue( vInt( -7 ) ), std::string( "-7" ) );
        UT_CHECK_EQ( renderValue( vFloat( 2.5 ) ), std::string( "2.5" ) );
        UT_CHECK_EQ( renderValue( vStr( "hello" ) ), std::string( "hello" ) );
        UT_CHECK_EQ( renderValue( vVar( "$p" ) ), std::string( "$p" ) );
    } );

    registry.add( "renderValue does not quote a Str", []() {
        /*
         * Not an oversight and not a stopgap. Quoting would be lens asserting
         * that a foreign core's Str means "text rather than a number", which
         * lens cannot know -- and this engine never sends one at all, since
         * `red` and `"red"` are byte-identical after parsing. The engine's
         * own toDisplayString() DOES quote; the divergence is deliberate and
         * this is the test that stops someone re-aligning them.
         */
        const std::string rendered = renderValue( vStr( "1" ) );
        UT_CHECK_MSG( rendered.find( '"' ) == std::string::npos,
                      "renderValue quoted a Str, rendering '" << rendered
                          << "'; a quoted `1` claims a type lens cannot know" );
        UT_CHECK_EQ( rendered, std::string( "1" ) );
    } );

    registry.add( "renderValue renders every compound kind", []() {
        UT_CHECK_EQ( renderValue( vCons( "point", { vInt( 1 ), vInt( 2 ) } ) ),
                     std::string( "point( 1, 2 )" ) );
        UT_CHECK_EQ( renderValue( vArray( { vInt( 1 ), vAtom( "red" ) } ) ),
                     std::string( "[ 1, red ]" ) );
        UT_CHECK_EQ( renderValue( vMap( { { "a", vInt( 1 ) },
                                          { "b", vAtom( "red" ) } } ) ),
                     std::string( "{ a: 1, b: red }" ) );

        /*
         * A Cons with no arguments and nothing cut is a bare name -- and it
         * must stay one, or the case below stops distinguishing anything.
         * The engine never sends this shape (a 0-arity term becomes an
         * Atom), but the wire format permits it.
         */
        UT_CHECK_EQ( renderValue( vCons( "nil", {} ) ), std::string( "nil" ) );
    } );

    registry.add( "renderValue nests", []() {
        const us::Value value = vMap( {
            { "here", vArray( { vCons( "point", { vInt( 1 ), vInt( 2 ) } ),
                                vVar( "$rest" ) } ) } } );
        UT_CHECK_EQ( renderValue( value ),
                     std::string( "{ here: [ point( 1, 2 ), $rest ] }" ) );
    } );

    registry.add( "a truncated leaf says so", []() {
        UT_CHECK_EQ( renderValue( cut( vAtom( "verylongatom" ) ) ),
                     "verylongatom" + kCutMark );
        UT_CHECK_EQ( renderValue( cut( vStr( "verylongtext" ) ) ),
                     "verylongtext" + kCutMark );
    } );

    registry.add( "a truncated compound cannot be mistaken for a whole one",
                  []() {
        /*
         * THE case. `applyBudget` cuts children breadth-first, so a compound
         * whose whole argument list did not fit arrives with `args` empty and
         * `truncated` set. Rendering that on the strength of `args.empty()`
         * alone printed `point` -- a term that exists, that is not this term,
         * and that carries no sign it was ever cut. SESSION-API section 3
         * calls showing a wrong term rather than a shortened one the failure
         * the flag exists to prevent; this is that failure, and it is why the
         * assertion below is written as an inequality first.
         */
        const std::string rendered = renderValue( cut( vCons( "point", {} ) ) );
        UT_CHECK_MSG( rendered != "point",
                      "a Cons that lost every argument to the budget rendered "
                      "as the bare atom 'point' -- a different term, shown as "
                      "a whole one" );
        UT_CHECK_EQ( rendered, "point( " + kCutMark + " )" );
    } );

    registry.add( "a truncated compound marks the cut inside its brackets",
                  []() {
        us::Value cons = vCons( "point", { vInt( 1 ) } );
        cons.truncated = true;
        UT_CHECK_EQ( renderValue( cons ), "point( 1, " + kCutMark + " )" );

        us::Value array = vArray( { vInt( 1 ) } );
        array.truncated = true;
        UT_CHECK_EQ( renderValue( array ), "[ 1, " + kCutMark + " ]" );
        UT_CHECK_EQ( renderValue( cut( vArray( {} ) ) ),
                     "[ " + kCutMark + " ]" );

        us::Value map = vMap( { { "a", vInt( 1 ) } } );
        map.truncated = true;
        UT_CHECK_EQ( renderValue( map ), "{ a: 1, " + kCutMark + " }" );
        UT_CHECK_EQ( renderValue( cut( vMap( {} ) ) ),
                     "{ " + kCutMark + " }" );
    } );

    registry.add( "truncation is marked where it happened, not at the top",
                  []() {
        /*
         * The depth rule marks the node it stopped at, which is usually deep
         * inside an otherwise complete term. A renderer that hoisted the mark
         * to the end of the line would say the wrong thing about which part
         * of the answer is missing.
         */
        const us::Value value =
            vCons( "outer", { vInt( 1 ), cut( vCons( "inner", {} ) ) } );
        UT_CHECK_EQ( renderValue( value ),
                     "outer( 1, inner( " + kCutMark + " ) )" );
    } );

    registry.add( "renderBindings joins one solution's bindings", []() {
        std::vector<std::pair<std::string, us::Value>> bindings;
        bindings.push_back( { "$x", vAtom( "red" ) } );
        bindings.push_back( { "$y", vCons( "point", { vInt( 1 ), vInt( 2 ) } ) } );
        UT_CHECK_EQ( renderBindings( bindings ),
                     std::string( "$x = red, $y = point( 1, 2 )" ) );

        /*
         * Empty, not "yes": a solution with no bindings is the model's
         * business (model.cpp says "yes"), and renderBindings saying it too
         * would put the word in the line twice the day E7.4 starts emitting
         * unbound Var rows.
         */
        UT_CHECK_EQ( renderBindings( {} ), std::string( "" ) );
    } );

    // -- the frame ---------------------------------------------------------

    registry.add( "H the hint line is non-empty at every stock layout", []() {
        for ( const std::string& name : stockLayoutNames() ) {
            for ( int geometry = 0; geometry < 2; ++geometry ) {
                const int w = geometry ? 80 : 120;
                const int h = geometry ? 24 : 40;
                Model model = makeModel( name.c_str(), w, h );

                const CellGrid grid = view( model );
                std::string hint;
                for ( int x = 0; x < grid.width(); ++x ) {
                    const Cell& cell = grid.at( x, grid.height() - 1 );
                    if ( cell.ch != 0 ) { hint += encodeUtf8( cell.ch ); }
                }
                UT_CHECK_MSG( hint.find_first_not_of( ' ' ) != std::string::npos,
                              "the hint line is blank in layout '" << name
                                  << "' at " << w << "x" << h );
            }
        }
    } );

    registry.add( "the rendered screen always covers its geometry", []() {
        for ( const std::string& name : stockLayoutNames() ) {
            Model model = makeModel( name.c_str() );
            const Solution solution = solve( model.layout(), model.tileArea() );
            const std::string problem =
                checkCoverage( solution, model.tileArea() );
            UT_CHECK_MSG( problem.empty(),
                          "layout '" << name << "': " << problem );

            const CellGrid grid = view( model );
            UT_CHECK_EQ( grid.width(), model.width() );
            UT_CHECK_EQ( grid.height(), model.height() );
        }
    } );

    registry.add( "an unbound key in an ordinary panel is silent", []() {
        Model model = makeModel();
        typeText( model, "q" );
        UT_CHECK_MSG( model.message().empty(),
                      "an unbound key produced the message '"
                          << model.message()
                          << "'; in a transcript most keys are text" );
    } );

    registry.add( "an unfinished chord is held and shown", []() {
        Model model = makeModel();
        type( model, "C-x" );
        UT_CHECK_MSG( !model.pendingKeys().empty(),
                      "C-x should be held as a chord prefix" );

        type( model, "2" );
        UT_CHECK_MSG( model.pendingKeys().empty(),
                      "the chord should have completed" );
    } );

    registry.add( "an unbound chord says so rather than silently dropping",
                  []() {
        Model model = makeModel();
        type( model, "C-x" );
        typeText( model, "q" );
        UT_CHECK_MSG( !model.message().empty(),
                      "C-x q is not bound and should say so -- a swallowed "
                      "chord is indistinguishable from a hang" );
        UT_CHECK( model.pendingKeys().empty() );
    } );

    return registry.run( "lens shell: help, palette and fold (G1 H)" ) == 0
               ? 0
               : 1;
}
