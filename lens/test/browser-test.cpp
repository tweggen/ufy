/**
 * @file browser-test.cpp
 *
 * Gate G3 -- the browser -- asserted on the model: the Catalogue panel, the
 * Source panel's definition view, and the refresh that keeps the first one
 * current.
 *
 * Like inspect-test.cpp this builds session events by hand. A catalogue of
 * five thousand keys (G3.5) or one with every provenance kind in it (G3.1)
 * is a list of structs here and a long, slow program anywhere else.
 * `lens-session` repeats the essential cases against the real engine.
 */

#include "../../unify/test/session/test-harness.hpp"

#include "../src/app/layouts.hpp"
#include "../src/model/model.hpp"
#include "../src/model/view.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace {

using namespace unify_test;
using namespace lens;

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

std::vector<CommandRequest> typeText( Model& model, const std::string& text )
{
    std::vector<CommandRequest> all;
    for ( char c : text ) {
        Event event;
        event.kind = Event::Kind::Key;
        event.key = Key::character( (char32_t) (unsigned char) c );
        const std::vector<CommandRequest> some = fold( model, event );
        all.insert( all.end(), some.begin(), some.end() );
    }
    return all;
}

std::size_t countKind( const std::vector<CommandRequest>& requests,
                       const std::string& kind )
{
    return (std::size_t) std::count_if(
        requests.begin(), requests.end(),
        [&]( const CommandRequest& r ) { return r.kind == kind; } );
}

void focusPanel( Model& model, PanelKind kind )
{
    for ( int i = 0; i < 24; ++i ) {
        const Buffer* focused = model.focusedBuffer();
        if ( focused && focused->kind == kind ) {
            return;
        }
        typeKeys( model, "Tab" );
    }
    UT_FAIL( "could not focus the panel" );
}

us::CatalogueEntry entry( const char* name, std::uint32_t arity,
                          us::Origin::Kind kind, std::uint32_t clauses,
                          const char* file = "" )
{
    us::CatalogueEntry e;
    e.key.name = name;
    e.key.arity = arity;
    e.key.module = kind == us::Origin::Kind::Module ? 3 : 1;
    e.origin.kind = kind;
    e.origin.module = e.key.module;
    e.origin.file = file;
    e.clauseCount = clauses;
    return e;
}

void deliverListing( Model& model, std::vector<us::CatalogueEntry> entries,
                     bool more = false )
{
    us::Listing listing;
    listing.entries = std::move( entries );
    listing.more = more;
    us::Event event;
    event.body = listing;
    model.foldSession( event );
}

/** A world with every provenance kind in it, the way a session sees one. */
std::vector<us::CatalogueEntry> mixedWorld()
{
    return {
        entry( "print", 1, us::Origin::Kind::Builtin, 1 ),
        entry( "findall", 3, us::Origin::Kind::Builtin, 1 ),
        entry( "colour", 1, us::Origin::Kind::Transcript, 2 ),
        entry( "warm", 1, us::Origin::Kind::Transcript, 1 ),
        /* A user predicate with a reserved-looking name: provenance decides
         * where it goes, never the name (G3.1's "never by name prefix"). */
        entry( "__cache", 2, us::Origin::Kind::Transcript, 1 ),
        entry( "__fe__1", 2, us::Origin::Kind::Synthesized, 1 ),
        entry( "play", 2, us::Origin::Kind::Module, 4, "lib/mediaplayer.ufy" ),
    };
}

/** The visible rows' labels, for a failure message and for find(). */
std::vector<std::string> labels( const Model& model )
{
    std::vector<std::string> out;
    for ( const CatalogueRow& row : model.catalogueRows() ) {
        out.push_back( row.label );
    }
    return out;
}

int rowLabelled( const Model& model, const std::string& label )
{
    const std::vector<std::string> all = labels( model );
    for ( std::size_t i = 0; i < all.size(); ++i ) {
        if ( all[i] == label ) {
            return (int) i;
        }
    }
    return -1;
}

std::string dumpRows( const Model& model )
{
    std::ostringstream os;
    for ( const CatalogueRow& row : model.catalogueRows() ) {
        os << "\n          | " << ( row.kind == CatalogueRow::Kind::Group ? "G " : "  " )
           << row.label << " (" << row.count << ")";
    }
    return os.str();
}

void cursorTo( Model& model, const std::string& label )
{
    focusPanel( model, PanelKind::Catalogue );
    const int want = rowLabelled( model, label );
    UT_CHECK_MSG( want >= 0, "no catalogue row '" << label << "'" << dumpRows( model ) );
    for ( int i = 0; i < 200 && model.catalogue().selected < want; ++i ) {
        typeKeys( model, "Down" );
    }
    for ( int i = 0; i < 200 && model.catalogue().selected > want; ++i ) {
        typeKeys( model, "Up" );
    }
    UT_CHECK_EQ( model.catalogue().selected, want );
}

} // namespace

int main()
{
    Registry registry;

    // -- G3.1: the catalogue --------------------------------------------------

    registry.add( "G3.1 the catalogue groups by provenance, never by name", []() {
        Model model = makeModel();
        deliverListing( model, mixedWorld() );

        const std::vector<CatalogueRow> rows = model.catalogueRows();
        UT_CHECK_MSG( rowLabelled( model, "colour/1" ) >= 0, dumpRows( model ) );
        UT_CHECK_MSG( rowLabelled( model, "play/2" ) >= 0, dumpRows( model ) );

        /* The user's __cache is the user's, listed beside colour/1. */
        UT_CHECK_MSG( rowLabelled( model, "__cache/2" ) >= 0,
                      "a user predicate was hidden for its name" << dumpRows( model ) );
        for ( const CatalogueRow& row : rows ) {
            if ( row.label == "__cache/2" ) {
                UT_CHECK_EQ( row.group, std::string( "<transcript>" ) );
            }
        }

        /* Builtins and synthesized clauses are collapsed, counted stubs. */
        UT_CHECK_MSG( rowLabelled( model, "print/1" ) < 0,
                      "builtins are not collapsed" << dumpRows( model ) );
        UT_CHECK_MSG( rowLabelled( model, "__fe__1/2" ) < 0,
                      "synthesized clauses are not collapsed" << dumpRows( model ) );
        bool builtins = false, synthesized = false;
        for ( const CatalogueRow& row : rows ) {
            if ( row.kind != CatalogueRow::Kind::Group ) { continue; }
            if ( row.label == "builtins" ) {
                builtins = true;
                UT_CHECK( !row.open );
                UT_CHECK_EQ( row.count, std::uint32_t( 2 ) );
            }
            if ( row.label == "synthesized" ) {
                synthesized = true;
                UT_CHECK( !row.open );
                UT_CHECK_EQ( row.count, std::uint32_t( 1 ) );
            }
        }
        UT_CHECK_MSG( builtins && synthesized, "a stub is missing" << dumpRows( model ) );

        /* And the clause count is on the predicate's row. */
        for ( const CatalogueRow& row : rows ) {
            if ( row.label == "play/2" ) {
                UT_CHECK_EQ( row.count, std::uint32_t( 4 ) );
                UT_CHECK_EQ( row.group, std::string( "mediaplayer.ufy" ) );
            }
        }
    } );

    registry.add( "G3.1 a stub opens to show what it counted", []() {
        Model model = makeModel();
        deliverListing( model, mixedWorld() );
        cursorTo( model, "builtins" );
        typeKeys( model, "Right" );
        UT_CHECK_MSG( rowLabelled( model, "print/1" ) >= 0, dumpRows( model ) );
        typeKeys( model, "Left" );
        UT_CHECK( rowLabelled( model, "print/1" ) < 0 );
    } );

    registry.add( "G3.1 the catalogue renders", []() {
        Model model = makeModel();
        deliverListing( model, mixedWorld() );
        const std::string text = view( model ).toText();
        UT_CHECK_MSG( text.find( "colour/1" ) != std::string::npos
                          && text.find( "builtins (2)" ) != std::string::npos,
                      "the Catalogue panel does not show its rows:\n" << text );
    } );

    registry.add( "G3.1 a listing in pages asks for the next page", []() {
        Model model = makeModel();
        deliverListing( model, { entry( "a", 1, us::Origin::Kind::Transcript, 1 ) }, true );
        const std::vector<CommandRequest> asked = model.takeRequests();
        UT_CHECK_EQ( countKind( asked, "listing" ), std::size_t( 1 ) );
        UT_CHECK_EQ( asked[0].index, std::uint64_t( 1 ) );   /* the offset */
        deliverListing( model, { entry( "b", 1, us::Origin::Kind::Transcript, 1 ) } );
        UT_CHECK( rowLabelled( model, "a/1" ) >= 0 );
        UT_CHECK( rowLabelled( model, "b/1" ) >= 0 );
        UT_CHECK_EQ( model.takeRequests().size(), std::size_t( 0 ) );
    } );

    // -- G3.2: selecting a key loads its source -------------------------------

    registry.add( "G3.2 Enter on a predicate asks source for exactly that key", []() {
        Model model = makeModel();
        deliverListing( model, mixedWorld() );
        cursorTo( model, "play/2" );
        const std::vector<CommandRequest> requests = typeKeys( model, "Enter" );
        UT_CHECK_EQ( requests.size(), std::size_t( 1 ) );
        UT_CHECK_EQ( requests[0].kind, std::string( "source" ) );
        UT_CHECK_EQ( requests[0].text, std::string( "play" ) );
        UT_CHECK_EQ( requests[0].index, std::uint64_t( 2 ) );
        UT_CHECK_EQ( requests[0].handle, std::uint64_t( 3 ) );

        /* The browser stance: the catalogue keeps the keyboard. */
        UT_CHECK( model.focusedBuffer()->kind == PanelKind::Catalogue );
    } );

    registry.add( "G3.2 the source arrives in the Source panel", []() {
        Model model = makeModel();
        deliverListing( model, mixedWorld() );
        cursorTo( model, "colour/1" );
        typeKeys( model, "Enter" );

        us::SourceText text;
        text.key.name = "colour";
        text.key.arity = 1;
        text.key.module = 1;
        text.text = "colour( red );\ncolour( green );\n";
        text.origin.kind = us::Origin::Kind::Transcript;
        us::Event event;
        event.body = text;
        model.foldSession( event );

        const SourceDefinition& shown = model.sourceDefinition();
        UT_CHECK( shown.set );
        UT_CHECK( shown.lines == ( std::vector<std::string>{ "colour( red );",
                                                             "colour( green );" } ) );
        const std::string screen = view( model ).toText();
        UT_CHECK_MSG( screen.find( "colour( green );" ) != std::string::npos,
                      "the Source panel does not show the definition:\n" << screen );
    } );

    registry.add( "G3.2 a source for a key nobody selected is not shown", []() {
        Model model = makeModel();
        deliverListing( model, mixedWorld() );
        cursorTo( model, "colour/1" );
        typeKeys( model, "Enter" );
        cursorTo( model, "warm/1" );
        typeKeys( model, "Enter" );

        /* The first answer arrives after the second question: stale. */
        us::SourceText late;
        late.key.name = "colour";
        late.key.arity = 1;
        late.key.module = 1;
        late.text = "colour( red );\n";
        us::Event event;
        event.body = late;
        model.foldSession( event );
        UT_CHECK( !model.sourceDefinition().set );
    } );

    registry.add( "G3.2 with no Source tile, Enter opens one beside the catalogue", []() {
        Model model = makeModel( "run" );
        deliverListing( model, mixedWorld() );
        model.showPanel( PanelKind::Catalogue );
        cursorTo( model, "colour/1" );
        typeKeys( model, "Enter" );
        bool source = false;
        for ( TileId tile : model.layout().tiles() ) {
            const Buffer* b = model.buffer( model.layout().bufferOf( tile ) );
            source = source || ( b && b->kind == PanelKind::Source );
        }
        UT_CHECK_MSG( source, "Enter loaded a source nobody can see" );
        UT_CHECK( model.focusedBuffer()->kind == PanelKind::Catalogue );
    } );

    // -- G3.4: WorldChanged refreshes, nothing polls -------------------------

    registry.add( "G3.4 a WorldChanged asks for one listing, and keys ask none", []() {
        Model model = makeModel();
        deliverListing( model, mixedWorld() );
        UT_CHECK_EQ( model.takeRequests().size(), std::size_t( 0 ) );

        /* A hundred keystrokes and no world change: no polling. */
        focusPanel( model, PanelKind::Catalogue );
        std::vector<CommandRequest> asked;
        for ( int i = 0; i < 25; ++i ) {
            const std::vector<CommandRequest> some = typeKeys( model, "Down Up Down Up" );
            asked.insert( asked.end(), some.begin(), some.end() );
        }
        const std::vector<CommandRequest> outbox = model.takeRequests();
        asked.insert( asked.end(), outbox.begin(), outbox.end() );
        UT_CHECK_EQ( countKind( asked, "listing" ), std::size_t( 0 ) );

        us::WorldChanged changed;
        changed.generation = 2;
        us::Event event;
        event.body = changed;
        model.foldSession( event );
        UT_CHECK_EQ( countKind( model.takeRequests(), "listing" ), std::size_t( 1 ) );
    } );

    registry.add( "G3.4 changes during a refresh coalesce into one more", []() {
        Model model = makeModel();
        deliverListing( model, mixedWorld() );
        us::WorldChanged changed;
        us::Event event;
        event.body = changed;
        model.foldSession( event );
        UT_CHECK_EQ( countKind( model.takeRequests(), "listing" ), std::size_t( 1 ) );

        /* Three more changes while that listing is in flight. */
        model.foldSession( event );
        model.foldSession( event );
        model.foldSession( event );
        UT_CHECK_EQ( model.takeRequests().size(), std::size_t( 0 ) );

        /* It answers; the changes it may have missed get exactly one more. */
        deliverListing( model, mixedWorld() );
        UT_CHECK_EQ( countKind( model.takeRequests(), "listing" ), std::size_t( 1 ) );
        deliverListing( model, mixedWorld() );
        UT_CHECK_EQ( model.takeRequests().size(), std::size_t( 0 ) );
    } );

    registry.add( "G3.4 a session start asks for the first listing", []() {
        Model model = makeModel();
        us::Capabilities caps;
        caps.coreName = "test";
        model.setCapabilities( caps );
        UT_CHECK_EQ( countKind( model.takeRequests(), "listing" ), std::size_t( 1 ) );
    } );

    // -- G3.5: filter-as-you-type over 5,000 keys -----------------------------

    registry.add( "G3.5 typing filters the catalogue", []() {
        Model model = makeModel();
        deliverListing( model, mixedWorld() );
        focusPanel( model, PanelKind::Catalogue );
        typeText( model, "co" );
        UT_CHECK_EQ( model.catalogue().filter, std::string( "co" ) );
        UT_CHECK_MSG( rowLabelled( model, "colour/1" ) >= 0, dumpRows( model ) );
        UT_CHECK_MSG( rowLabelled( model, "warm/1" ) < 0, dumpRows( model ) );

        /* A filter reaches into the stubs: it is how you find a builtin. */
        typeKeys( model, "Backspace Backspace" );
        typeText( model, "find" );
        UT_CHECK_MSG( rowLabelled( model, "findall/3" ) >= 0, dumpRows( model ) );

        typeKeys( model, "Esc" );
        UT_CHECK_EQ( model.catalogue().filter, std::string() );
        UT_CHECK( rowLabelled( model, "findall/3" ) < 0 );
    } );

    registry.add( "G3.5 filtering 5,000 keys stays inside the frame budget", []() {
#if !defined( NDEBUG )
        /*
         * A frame budget measured on an unoptimised or sanitizer build
         * measures the build. CI's lens jobs are RelWithDebInfo, which is
         * where this is asserted; 20 ms against 50 on the machine that
         * wrote it (2026-11-28).
         */
        UT_SKIP( "a timing bound needs an optimised build (NDEBUG unset)" );
#endif
        Model model = makeModel();
        std::vector<us::CatalogueEntry> many;
        for ( int i = 0; i < 5000; ++i ) {
            std::ostringstream name;
            name << "pred_" << ( i % 97 ) << "_" << i;
            many.push_back( entry( name.str().c_str(), (std::uint32_t)( i % 4 ),
                                   i % 10 == 0 ? us::Origin::Kind::Builtin
                                               : us::Origin::Kind::Transcript,
                                   (std::uint32_t)( 1 + i % 7 ) ) );
        }
        deliverListing( model, many );
        focusPanel( model, PanelKind::Catalogue );

        /*
         * The budget is one frame of the interactive loop, which polls the
         * terminal every 50 ms (main.cpp): a keystroke that takes longer
         * than that to fold AND draw is one the user sees lag. Measured as
         * the worst keystroke, not the average -- lag is what the worst one
         * feels like.
         */
        const auto budget = std::chrono::milliseconds( 50 );
        std::chrono::steady_clock::duration worst{};
        for ( const char c : std::string( "pred_42_" ) ) {
            const auto start = std::chrono::steady_clock::now();
            Event event;
            event.kind = Event::Kind::Key;
            event.key = Key::character( (char32_t) c );
            ( void ) fold( model, event );
            ( void ) view( model );
            worst = std::max( worst, std::chrono::steady_clock::now() - start );
        }
        UT_CHECK_MSG( !model.catalogueRows().empty(), "the filter matched nothing" );
        UT_CHECK_MSG( worst < budget,
                      "the slowest keystroke took "
                          << std::chrono::duration_cast<std::chrono::milliseconds>(
                                 worst ).count()
                          << " ms" );
    } );

    // -- H --------------------------------------------------------------------

    registry.add( "G3 H F1 in the catalogue and the source open their pages", []() {
        Model model = makeModel();
        focusPanel( model, PanelKind::Catalogue );
        UT_CHECK_EQ( model.contextualTopicId(), std::string( "catalogue" ) );
        const HelpTopic* topic = model.helpBook()->topic( "catalogue" );
        UT_CHECK( topic != NULL );
        focusPanel( model, PanelKind::Source );
        UT_CHECK_EQ( model.contextualTopicId(), std::string( "source" ) );
    } );

    return registry.run( "lens browser: catalogue and source (G3)" ) == 0 ? 0 : 1;
}
