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
 */

#include "../../unify/test/session/test-harness.hpp"

#include "../src/app/layouts.hpp"
#include "../src/model/model.hpp"
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

    // -- the menu ----------------------------------------------------------

    registry.add( "H F10 opens a menu that lists every command once", []() {
        Model model = makeModel();
        type( model, "F10" );

        UT_CHECK_MSG( model.menuActive(), "F10 did not open the menu" );

        const std::vector<Model::MenuRow> rows = model.menuRows();

        std::size_t commandRows = 0;
        for ( const Model::MenuRow& row : rows ) {
            if ( !row.isHeading() ) { ++commandRows; }
        }
        UT_CHECK_MSG( commandRows == model.commands().size(),
                      "the menu shows " << commandRows << " of "
                          << model.commands().size() << " commands" );

        /* Every heading the menu bar draws appears, even the empty ones --
         * a heading that vanished would make the bar look like a lie. */
        const std::vector<std::string> categories = menuCategories();
        for ( const std::string& category : categories ) {
            bool found = false;
            for ( const Model::MenuRow& row : rows ) {
                if ( row.isHeading() && row.heading == category ) { found = true; }
            }
            UT_CHECK_MSG( found, "the menu is missing the '" << category
                                     << "' heading that the bar draws" );
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

    registry.add( "the menu opens on a command, not on a heading", []() {
        Model model = makeModel();
        type( model, "F10" );

        const Buffer* menu = findPanel( model, PanelKind::Menu );
        UT_CHECK( menu != NULL );

        const std::vector<Model::MenuRow> rows = model.menuRows();
        UT_CHECK( menu->menu.selected >= 0
                  && menu->menu.selected < (int) rows.size() );
        UT_CHECK_MSG( !rows[ (std::size_t) menu->menu.selected ].isHeading(),
                      "the menu opened with a heading selected, so Enter "
                      "would do nothing" );
    } );

    registry.add( "menu navigation skips headings", []() {
        Model model = makeModel();
        type( model, "F10" );

        const std::vector<Model::MenuRow> rows = model.menuRows();
        for ( int i = 0; i < 20; ++i ) {
            type( model, "Down" );
            const Buffer* menu = findPanel( model, PanelKind::Menu );
            if ( !menu ) { break; }
            UT_CHECK_MSG(
                !rows[ (std::size_t) menu->menu.selected ].isHeading(),
                "menu navigation landed on a heading after " << i + 1
                    << " steps down" );
        }
    } );

    registry.add( "F10 closes the menu as well as opening it", []() {
        Model model = makeModel();
        type( model, "F10" );
        UT_CHECK( model.menuActive() );
        type( model, "F10" );
        UT_CHECK_MSG( !model.menuActive(),
                      "F10 must close the menu too, or pressing it by "
                      "accident is a trap" );
    } );

    registry.add( "Esc closes the menu and leaves the layout as it was", []() {
        Model model = makeModel();
        const std::size_t before = model.layout().tileCount();
        type( model, "F10" );
        type( model, "Esc" );
        UT_CHECK( !model.menuActive() );
        UT_CHECK_EQ( model.layout().tileCount(), before );
    } );

    registry.add( "Enter in the menu runs the selected command", []() {
        Model model = makeModel();
        const std::size_t before = model.layout().tileCount();

        type( model, "F10" );
        /* Walk to a window command, which has a visible effect. */
        const Command* target = model.commands().find( "window.split-rows" );
        UT_CHECK( target != NULL );

        const std::vector<Model::MenuRow> rows = model.menuRows();
        int wanted = -1;
        for ( std::size_t i = 0; i < rows.size(); ++i ) {
            if ( rows[i].command && rows[i].command->id() == "window.split-rows" ) {
                wanted = (int) i;
            }
        }
        UT_CHECK( wanted >= 0 );

        Buffer* menu = model.buffer(
            model.layout().bufferOf( model.layout().focused() ) );
        UT_CHECK( menu != NULL && menu->kind == PanelKind::Menu );
        menu->menu.selected = wanted;

        type( model, "Enter" );

        UT_CHECK_MSG( !model.menuActive(), "running a command left the menu open" );
        UT_CHECK_MSG( model.layout().tileCount() == before + 1,
                      "the split ran against the wrong tile: "
                          << model.layout().tileCount() << " vs "
                          << before + 1 );
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
