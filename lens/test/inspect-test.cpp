/**
 * @file inspect-test.cpp
 *
 * Gate G5 -- Inspect -- asserted on the model.
 *
 * The Solutions table, the Inspector's term tree and the Diagnostics list are
 * all functions of session EVENTS, so every case here builds the events by
 * hand and folds them in. That is what lets G5.2's truncation be tested at
 * all: the real engine truncates only past a depth of eight, and the
 * interesting cases -- a node cut with some children kept, a node cut with
 * none, an expansion that is itself cut again -- are easiest to state as the
 * tree they are. `lens-session` repeats the essential one against the real
 * engine, so nothing here is only true of a hand-built tree.
 */

#include "../../unify/test/session/test-harness.hpp"

#include "../src/app/layouts.hpp"
#include "../src/model/model.hpp"
#include "../src/model/results.hpp"
#include "../src/model/view.hpp"
#include "value-builders.hpp"

#include <algorithm>

namespace {

using namespace unify_test;
using namespace lens;
using namespace lens_test;

Model makeModel( const char* layout = "full", int w = 160, int h = 50 )
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

// -- events, built as the core would send them ------------------------------

us::Event queryEvent( us::QueryId q, us::EventBody body )
{
    us::Event event;
    event.header.query = q;
    event.body = std::move( body );
    return event;
}

void deliver( Model& model, us::QueryId q, std::uint64_t index,
              std::vector<std::pair<std::string, us::Value>> bindings )
{
    us::Solution solution;
    solution.index = index;
    solution.bindings = std::move( bindings );
    model.foldSession( queryEvent( q, solution ) );
}

void status( Model& model, us::QueryId q, us::QueryStatus::State state,
             std::uint64_t produced )
{
    us::QueryStatus s;
    s.state = state;
    s.produced = produced;
    s.retained = state != us::QueryStatus::State::Aborted;
    model.foldSession( queryEvent( q, s ) );
}

/** `count` solutions binding $x to 0..count-1, then a batch-end status. */
void deliverBatch( Model& model, us::QueryId q, std::uint64_t from,
                   std::uint64_t count, bool more )
{
    for ( std::uint64_t i = from; i < from + count; ++i ) {
        deliver( model, q, i, { { "$x", vInt( (std::int64_t) i ) } } );
    }
    status( model, q,
            more ? us::QueryStatus::State::Running
                 : us::QueryStatus::State::Exhausted,
            from + count );
}

void expand( Model& model, us::QueryId q, us::ValuePath path, us::Value value )
{
    us::Expanded expanded;
    expanded.path = std::move( path );
    expanded.value = std::move( value );
    model.foldSession( queryEvent( q, expanded ) );
}

/** The inspector row whose path is `path`, or -1. */
int rowOf( const Model& model, const us::ValuePath& path )
{
    const std::vector<InspectorRow> rows = model.inspectorRows();
    for ( std::size_t i = 0; i < rows.size(); ++i ) {
        if ( rows[i].path == path ) {
            return (int) i;
        }
    }
    return -1;
}

/** Put the inspector cursor on `path`, by pressing keys as a user would. */
void cursorTo( Model& model, const us::ValuePath& path )
{
    focusPanel( model, PanelKind::Inspector );
    const int want = rowOf( model, path );
    UT_CHECK_MSG( want >= 0, "no inspector row for that path" );
    for ( int i = 0; i < 64 && model.inspector().cursor < want; ++i ) {
        typeKeys( model, "Down" );
    }
    for ( int i = 0; i < 64 && model.inspector().cursor > want; ++i ) {
        typeKeys( model, "Up" );
    }
    UT_CHECK_EQ( model.inspector().cursor, want );
}

std::string screen( const Model& model )
{
    return view( model ).toText();
}

} // namespace

int main()
{
    Registry registry;

    // -- G5.1: the Solutions table, and demand -------------------------------

    registry.add( "G5.1 the table holds one row per delivered solution", []() {
        Model model = makeModel();
        UT_CHECK_EQ( model.beginQuery( 7 ), us::kNoQuery );
        deliver( model, 7, 0, { { "$x", vAtom( "red" ) }, { "$y", vInt( 1 ) } } );
        deliver( model, 7, 1, { { "$x", vAtom( "green" ) }, { "$y", vInt( 2 ) } } );
        status( model, 7, us::QueryStatus::State::Exhausted, 2 );

        UT_CHECK_EQ( model.results().solutions.size(), std::size_t( 2 ) );
        UT_CHECK( model.results().columns
                  == ( std::vector<std::string>{ "$x", "$y" } ) );
        UT_CHECK( model.results().finished );

        /* "this panel renders" -- one look at the grid, not the oracle. */
        const std::string text = screen( model );
        UT_CHECK_MSG( text.find( "green" ) != std::string::npos,
                      "the Solutions panel does not show its rows:\n" << text );
    } );

    registry.add( "G5.1 Down past the last row issues exactly one demand", []() {
        Model model = makeModel();
        model.beginQuery( 7 );
        deliverBatch( model, 7, 0, 20, true );
        focusPanel( model, PanelKind::Solutions );

        std::vector<CommandRequest> requests = typeKeys( model, "End" );
        UT_CHECK_EQ( model.results().selected, 19 );
        UT_CHECK_EQ( requests.size(), std::size_t( 0 ) );

        /* Held down: five presses past the end, one batch asked for. */
        requests = typeKeys( model, "Down Down Down Down Down" );
        UT_CHECK_EQ( countKind( requests, "demand" ), std::size_t( 1 ) );
        UT_CHECK_EQ( requests.size(), std::size_t( 1 ) );
        UT_CHECK_EQ( requests[0].handle, std::uint64_t( 7 ) );

        /* The batch arrives; the next press past the end may ask again. */
        deliverBatch( model, 7, 20, 20, true );
        UT_CHECK( !model.results().demandPending );
        requests = typeKeys( model, "Down" );
        UT_CHECK_EQ( model.results().selected, 20 );
        UT_CHECK_EQ( requests.size(), std::size_t( 0 ) );
        requests = typeKeys( model, "End Down Down" );
        UT_CHECK_EQ( countKind( requests, "demand" ), std::size_t( 1 ) );
    } );

    registry.add( "G5.1 an exhausted query is never asked for more", []() {
        Model model = makeModel();
        model.beginQuery( 7 );
        deliverBatch( model, 7, 0, 3, false );
        focusPanel( model, PanelKind::Solutions );
        const std::vector<CommandRequest> requests =
            typeKeys( model, "End Down Down" );
        UT_CHECK_EQ( requests.size(), std::size_t( 0 ) );
    } );

    registry.add( "G5.1 query.more and scrolling share the one pending demand", []() {
        Model model = makeModel();
        model.beginQuery( 7 );
        deliverBatch( model, 7, 0, 20, true );
        focusPanel( model, PanelKind::Solutions );

        std::vector<CommandRequest> requests =
            typeKeys( model, "M-x q u e r y . m o r e Enter" );
        UT_CHECK_EQ( countKind( requests, "demand" ), std::size_t( 1 ) );
        focusPanel( model, PanelKind::Solutions );
        requests = typeKeys( model, "End Down" );
        UT_CHECK_EQ( countKind( requests, "demand" ), std::size_t( 0 ) );
    } );

    registry.add( "G5.1 a new query replaces the table and owes a release", []() {
        Model model = makeModel();
        model.beginQuery( 7 );
        deliverBatch( model, 7, 0, 3, false );

        /* The query lens stops showing is the one it hands back to release. */
        UT_CHECK_EQ( model.beginQuery( 8 ), us::QueryId( 7 ) );
        UT_CHECK_EQ( model.results().solutions.size(), std::size_t( 0 ) );
        UT_CHECK_EQ( model.results().query, us::QueryId( 8 ) );

        /* A late row from the old query is not shown under the new one. */
        deliver( model, 7, 3, { { "$x", vAtom( "late" ) } } );
        UT_CHECK_EQ( model.results().solutions.size(), std::size_t( 0 ) );
    } );

    registry.add( "G5.1 a cancelled query's late rows are dropped", []() {
        Model model = makeModel();
        model.beginQuery( 7 );
        Buffer* transcript = model.transcript();
        UT_CHECK( transcript != NULL );
        transcript->transcript.finished = false;
        deliverBatch( model, 7, 0, 2, true );

        typeKeys( model, "C-c" );
        deliver( model, 7, 2, { { "$x", vAtom( "late" ) } } );
        UT_CHECK_EQ( model.results().solutions.size(), std::size_t( 2 ) );

        focusPanel( model, PanelKind::Solutions );
        const std::vector<CommandRequest> requests = typeKeys( model, "End Down" );
        UT_CHECK_EQ( countKind( requests, "demand" ), std::size_t( 0 ) );
    } );

    // -- G5.2: the Inspector ------------------------------------------------

    registry.add( "G5.2 the inspector shows the selected solution as a tree", []() {
        Model model = makeModel();
        model.beginQuery( 7 );
        deliver( model, 7, 0,
                 { { "$p", vCons( "point", { vInt( 1 ), vInt( 2 ) } ) },
                   { "$c", vAtom( "red" ) } } );
        status( model, 7, us::QueryStatus::State::Exhausted, 1 );

        std::vector<InspectorRow> rows = model.inspectorRows();
        UT_CHECK_EQ( rows.size(), std::size_t( 2 ) );
        UT_CHECK( rows[0].expandable );
        UT_CHECK( !rows[0].open );
        UT_CHECK( !rows[1].expandable );

        cursorTo( model, { 0 } );
        typeKeys( model, "Right" );
        rows = model.inspectorRows();
        UT_CHECK_EQ( rows.size(), std::size_t( 4 ) );
        UT_CHECK( rows[0].open );
        UT_CHECK( rows[1].path == ( us::ValuePath{ 0, 0 } ) );
        UT_CHECK_EQ( rows[1].depth, 1 );
        UT_CHECK_EQ( rows[1].text, std::string( "1" ) );
        UT_CHECK( rows[3].path == ( us::ValuePath{ 1 } ) );

        typeKeys( model, "Left" );
        UT_CHECK_EQ( model.inspectorRows().size(), std::size_t( 2 ) );
    } );

    registry.add( "G5.2 a truncated node is marked", []() {
        Model model = makeModel();
        model.beginQuery( 7 );
        /* Some children kept, and one with every child cut. */
        deliver( model, 7, 0,
                 { { "$t", cut( vCons( "f", { vInt( 1 ), cut( vCons( "g", {} ) ) } ) ) } } );
        status( model, 7, us::QueryStatus::State::Exhausted, 1 );

        std::vector<InspectorRow> rows = model.inspectorRows();
        UT_CHECK( rows[0].truncated );
        UT_CHECK_MSG( rows[0].text.find( kCutMark ) != std::string::npos,
                      "a truncated node is not marked: " << rows[0].text );

        cursorTo( model, { 0 } );
        typeKeys( model, "Right" );
        rows = model.inspectorRows();
        const int g = rowOf( model, { 0, 1 } );
        UT_CHECK( g >= 0 );
        UT_CHECK( rows[ (std::size_t) g ].truncated );
        UT_CHECK_MSG( rows[ (std::size_t) g ].expandable,
                      "a node with every child cut must still be expandable "
                      "-- it is a compound, not the atom `g`" );

        /* And on screen, not only in the model. */
        const std::string text = screen( model );
        UT_CHECK_MSG( text.find( "g( " + kCutMark + " )" ) != std::string::npos,
                      "the cut compound is not drawn with its mark:\n" << text );
    } );

    registry.add( "G5.2 expanding a truncated node asks inspect, never solve", []() {
        Model model = makeModel();
        model.beginQuery( 7 );
        deliver( model, 7, 0, { { "$a", vAtom( "x" ) } } );
        deliver( model, 7, 1,
                 { { "$a", vAtom( "y" ) },
                   { "$t", vCons( "f", { cut( vCons( "g", {} ) ) } ) } } );
        status( model, 7, us::QueryStatus::State::Exhausted, 2 );

        focusPanel( model, PanelKind::Solutions );
        typeKeys( model, "Down" );
        UT_CHECK_EQ( model.results().selected, 1 );

        cursorTo( model, { 1 } );
        typeKeys( model, "Right" );
        cursorTo( model, { 1, 0 } );
        std::vector<CommandRequest> requests = typeKeys( model, "Right" );
        UT_CHECK_EQ( requests.size(), std::size_t( 1 ) );
        UT_CHECK_EQ( requests[0].kind, std::string( "inspect" ) );
        UT_CHECK_EQ( requests[0].handle, std::uint64_t( 7 ) );
        UT_CHECK_EQ( requests[0].index, std::uint64_t( 1 ) );
        UT_CHECK( requests[0].valuePath == ( std::vector<std::uint32_t>{ 1, 0 } ) );

        /* Asked once. A second press while it is in flight asks nothing. */
        typeKeys( model, "Left" );
        requests = typeKeys( model, "Right Enter Right" );
        UT_CHECK_EQ( countKind( requests, "inspect" ), std::size_t( 0 ) );
        UT_CHECK_EQ( countKind( requests, "solve" ), std::size_t( 0 ) );
    } );

    registry.add( "G5.2 the expansion is grafted where it was asked for", []() {
        Model model = makeModel();
        model.beginQuery( 7 );
        deliver( model, 7, 0, { { "$t", vCons( "f", { cut( vCons( "g", {} ) ) } ) } } );
        status( model, 7, us::QueryStatus::State::Exhausted, 1 );

        cursorTo( model, { 0 } );
        typeKeys( model, "Right" );
        cursorTo( model, { 0, 0 } );
        typeKeys( model, "Right" );

        expand( model, 7, { 0, 0 }, vCons( "g", { vAtom( "deep" ) } ) );

        const int g = rowOf( model, { 0, 0 } );
        const int deep = rowOf( model, { 0, 0, 0 } );
        UT_CHECK( g >= 0 );
        UT_CHECK_MSG( deep >= 0, "the expansion's child is not shown" );
        const std::vector<InspectorRow> rows = model.inspectorRows();
        UT_CHECK( !rows[ (std::size_t) g ].truncated );
        UT_CHECK( !rows[ (std::size_t) g ].fetching );
        UT_CHECK_EQ( rows[ (std::size_t) deep ].text, std::string( "deep" ) );

        /* The table shows the same value: there is one copy, not two. */
        UT_CHECK_MSG( screen( model ).find( "f( g( deep ) )" ) != std::string::npos,
                      "the Solutions table did not see the graft" );
    } );

    registry.add( "G5.2 an expansion nobody asked for is not grafted", []() {
        Model model = makeModel();
        model.beginQuery( 7 );
        deliver( model, 7, 0, { { "$t", vCons( "f", { cut( vCons( "g", {} ) ) } ) } } );
        status( model, 7, us::QueryStatus::State::Exhausted, 1 );

        /* Another query's answer, and an unrequested path of this one. */
        expand( model, 6, { 0, 0 }, vCons( "g", { vAtom( "wrong" ) } ) );
        expand( model, 7, { 0, 0 }, vCons( "g", { vAtom( "wrong" ) } ) );
        UT_CHECK_MSG( model.results().solutions[0].bindings[0].second.args[0].truncated,
                      "an unrequested expansion was grafted" );
    } );

    registry.add( "G5.2 moving to another solution re-roots the inspector", []() {
        Model model = makeModel();
        model.beginQuery( 7 );
        deliver( model, 7, 0, { { "$p", vCons( "a", { vInt( 1 ) } ) } } );
        deliver( model, 7, 1, { { "$p", vCons( "b", { vInt( 2 ), vInt( 3 ) } ) } } );
        status( model, 7, us::QueryStatus::State::Exhausted, 2 );

        cursorTo( model, { 0 } );
        typeKeys( model, "Right" );
        UT_CHECK_EQ( model.inspectorRows().size(), std::size_t( 2 ) );

        focusPanel( model, PanelKind::Solutions );
        typeKeys( model, "Down" );
        const std::vector<InspectorRow> rows = model.inspectorRows();
        UT_CHECK_EQ( rows.size(), std::size_t( 1 ) );
        UT_CHECK_MSG( rows[0].text.find( "b( 2, 3 )" ) != std::string::npos,
                      "the inspector is not showing solution 2: " << rows[0].text );
        UT_CHECK_EQ( model.inspector().cursor, 0 );
    } );

    registry.add( "G5.2 the inspector fills in a layout with no transcript", []() {
        /* The debug layout: Inspector, Source, Trace, Queries -- no REPL. */
        Model model = makeModel( "debug", 120, 40 );
        UT_CHECK( model.transcript() == NULL );
        model.beginQuery( 7 );
        deliver( model, 7, 0, { { "$x", vAtom( "red" ) } } );
        UT_CHECK_EQ( model.inspectorRows().size(), std::size_t( 1 ) );
    } );

    // -- G5.3: Diagnostics ---------------------------------------------------

    registry.add( "G5.3 Enter moves the Source panel to the diagnostic", []() {
        Model model = makeModel( "browse", 120, 40 );
        us::Diagnostic first;
        first.file = "<session>";
        first.line = 1;
        first.column = 3;
        first.message = "parse error";
        first.sourceLine = "f( ;";
        us::Diagnostic second = first;
        second.file = "lib/route.ufy";
        second.line = 12;
        second.column = 7;
        second.message = "unknown predicate";
        second.sourceLine = "    route( $a, $b );";

        us::Event e1;
        e1.body = first;
        us::Event e2;
        e2.body = second;
        model.foldSession( e1 );
        model.foldSession( e2 );
        UT_CHECK_EQ( model.diagnostics().items.size(), std::size_t( 2 ) );

        focusPanel( model, PanelKind::Diagnostics );
        typeKeys( model, "Down Enter" );

        const SourceLocation& at = model.sourceLocation();
        UT_CHECK( at.set );
        UT_CHECK_EQ( at.file, std::string( "lib/route.ufy" ) );
        UT_CHECK_EQ( at.line, std::uint32_t( 12 ) );
        UT_CHECK_EQ( at.column, std::uint32_t( 7 ) );
        UT_CHECK_EQ( at.text, std::string( "    route( $a, $b );" ) );

        const Buffer* focused = model.focusedBuffer();
        UT_CHECK( focused && focused->kind == PanelKind::Source );
        const std::string text = screen( model );
        UT_CHECK_MSG( text.find( "lib/route.ufy:12:7" ) != std::string::npos,
                      "the Source panel does not show where it was sent:\n" << text );
    } );

    registry.add( "G5.3 with no Source tile on screen, Enter opens one", []() {
        Model model = makeModel( "run", 120, 40 );
        UT_CHECK( findPanel( model, PanelKind::Source ) == NULL );
        us::Diagnostic d;
        d.file = "<session>";
        d.line = 1;
        d.column = 1;
        d.message = "parse error";
        us::Event e;
        e.body = d;
        model.foldSession( e );

        model.showPanel( PanelKind::Diagnostics );
        focusPanel( model, PanelKind::Diagnostics );
        typeKeys( model, "Enter" );
        UT_CHECK_MSG( findPanel( model, PanelKind::Source ) != NULL,
                      "Enter on a diagnostic went nowhere" );
        UT_CHECK( model.sourceLocation().set );
    } );

    // -- H -------------------------------------------------------------------

    registry.add( "G5 H F1 in the inspector explains term kinds and the cut mark", []() {
        Model model = makeModel();
        focusPanel( model, PanelKind::Inspector );
        UT_CHECK_EQ( model.contextualTopicId(), std::string( "inspector" ) );

        const HelpTopic* topic = model.helpBook()->topic( "inspector" );
        UT_CHECK( topic != NULL );
        std::string all;
        for ( const std::string& line : topic->lines ) {
            all += line + "\n";
        }
        for ( const char* word : { "atom", "integer", "string", "variable",
                                   "compound", "array", "map" } ) {
            UT_CHECK_MSG( all.find( word ) != std::string::npos,
                          "the inspector page does not explain " << word );
        }
        UT_CHECK_MSG( all.find( kCutMark ) != std::string::npos,
                      "the inspector page does not show the truncation mark" );
    } );

    registry.add( "G5 H F1 in Solutions and Diagnostics opens their own pages", []() {
        Model model = makeModel();
        focusPanel( model, PanelKind::Solutions );
        UT_CHECK_EQ( model.contextualTopicId(), std::string( "solutions" ) );
        focusPanel( model, PanelKind::Diagnostics );
        UT_CHECK_EQ( model.contextualTopicId(), std::string( "diagnostics" ) );
        focusPanel( model, PanelKind::Source );
        UT_CHECK_EQ( model.contextualTopicId(), std::string( "source" ) );
    } );

    return registry.run( "lens inspect: solutions, inspector, diagnostics (G5)" ) == 0
               ? 0
               : 1;
}
