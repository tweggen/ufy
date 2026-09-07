/**
 * @file grid-test.cpp
 *
 * The character grid: the value `view` produces and the only thing the
 * terminal layer ever draws.
 *
 * Worth testing carefully because it sits on BOTH sides of the seam FTXUI
 * hides behind. Everything asserted here is asserted without a terminal,
 * which is what keeps the untestable part of lens down to one file.
 */

#include "../../unify/test/session/test-harness.hpp"

#include "../src/model/cell-grid.hpp"

namespace {

using namespace unify_test;
using namespace lens;

std::string row( const CellGrid& grid, int y )
{
    std::string out;
    for ( int x = 0; x < grid.width(); ++x ) {
        const Cell& cell = grid.at( x, y );
        if ( 0 == cell.ch ) { continue; }
        out += encodeUtf8( cell.ch );
    }
    while ( !out.empty() && out.back() == ' ' ) { out.pop_back(); }
    return out;
}

} // namespace

int main()
{
    Registry registry;

    registry.add( "UTF-8 round-trips, and bad bytes become U+FFFD", []() {
        const std::string text = "a\xc3\xa9\xe2\x94\x8c";   /* a, e-acute, box */
        const std::vector<char32_t> cps = decodeUtf8( text );
        UT_CHECK_EQ( cps.size(), std::size_t( 3 ) );
        UT_CHECK_EQ( (std::uint32_t) cps[0], std::uint32_t( 0x61 ) );
        UT_CHECK_EQ( (std::uint32_t) cps[1], std::uint32_t( 0xE9 ) );
        UT_CHECK_EQ( (std::uint32_t) cps[2], std::uint32_t( 0x250C ) );

        std::string rebuilt;
        for ( char32_t cp : cps ) { rebuilt += encodeUtf8( cp ); }
        UT_CHECK_EQ( rebuilt, text );

        const std::vector<char32_t> bad = decodeUtf8( "\xff\xfe" );
        UT_CHECK_EQ( bad.size(), std::size_t( 2 ) );
        UT_CHECK_EQ( (std::uint32_t) bad[0], std::uint32_t( 0xFFFD ) );
    } );

    registry.add( "display width: wide is 2, combining is 0", []() {
        UT_CHECK_EQ( displayWidth( U'a' ), 1 );
        UT_CHECK_EQ( displayWidth( U'┌' ), 1 );      /* box drawing */
        UT_CHECK_EQ( displayWidth( U'世' ), 2 );      /* CJK */
        UT_CHECK_EQ( displayWidth( U'あ' ), 2 );      /* hiragana */
        UT_CHECK_EQ( displayWidth( U'́' ), 0 );      /* combining acute */
        UT_CHECK_EQ( displayWidth( U'\n' ), 0 );          /* control */

        /* The case UI.md names: a wide character must not shift the columns
         * to its right by one. */
        UT_CHECK_EQ( displayWidth( std::string( "ab" ) ), 2 );
        UT_CHECK_EQ( displayWidth( std::string( "\xe4\xb8\x96" ) ), 2 );
        UT_CHECK_EQ( displayWidth( std::string( "a\xe4\xb8\x96\x62" ) ), 4 );
    } );

    registry.add( "out-of-range access is harmless", []() {
        CellGrid grid( 4, 2 );
        grid.put( -1, 0, U'x' );
        grid.put( 0, -1, U'x' );
        grid.put( 99, 99, U'x' );
        UT_CHECK_EQ( (std::uint32_t) grid.at( -5, -5 ).ch, std::uint32_t( U' ' ) );
        UT_CHECK_EQ( (std::uint32_t) grid.at( 0, 0 ).ch, std::uint32_t( U' ' ) );
    } );

    registry.add( "drawText clips to the width it was given", []() {
        CellGrid grid( 10, 1 );
        const int drawn = grid.drawText( 0, 0, "abcdefghijklm", 5 );
        UT_CHECK_EQ( drawn, 5 );
        UT_CHECK_EQ( row( grid, 0 ), std::string( "abcde" ) );
    } );

    registry.add( "a wide character straddling the edge is dropped, not halved",
                  []() {
        CellGrid grid( 10, 1 );
        /* Three columns of room, then a two-column character: it must not be
         * drawn half in and half out -- half a wide glyph is a different
         * character on most terminals. */
        const int drawn = grid.drawText( 0, 0, "ab\xe4\xb8\x96", 3 );
        UT_CHECK_EQ( drawn, 2 );
        UT_CHECK_EQ( row( grid, 0 ), std::string( "ab" ) );

        CellGrid roomy( 10, 1 );
        UT_CHECK_EQ( roomy.drawText( 0, 0, "ab\xe4\xb8\x96", 4 ), 4 );
        UT_CHECK_EQ( row( roomy, 0 ), std::string( "ab\xe4\xb8\x96" ) );
        /* The continuation cell is marked, not blanked. */
        UT_CHECK_EQ( (std::uint32_t) roomy.at( 3, 0 ).ch, std::uint32_t( 0 ) );
    } );

    registry.add( "a box has corners, edges and a titled top", []() {
        CellGrid grid( 12, 4 );
        grid.drawBox( 0, 0, 12, 4, "Src" );

        UT_CHECK_EQ( (std::uint32_t) grid.at( 0, 0 ).ch, std::uint32_t( U'┌' ) );
        UT_CHECK_EQ( (std::uint32_t) grid.at( 11, 0 ).ch, std::uint32_t( U'┐' ) );
        UT_CHECK_EQ( (std::uint32_t) grid.at( 0, 3 ).ch, std::uint32_t( U'└' ) );
        UT_CHECK_EQ( (std::uint32_t) grid.at( 11, 3 ).ch, std::uint32_t( U'┘' ) );
        UT_CHECK_EQ( (std::uint32_t) grid.at( 0, 1 ).ch, std::uint32_t( U'│' ) );

        /* Exactly the box width in COLUMNS -- not bytes, which is the trap
         * a box-drawing character sets for any test written carelessly. */
        UT_CHECK_EQ( displayWidth( row( grid, 0 ) ), 12 );
        UT_CHECK_EQ( row( grid, 0 ), std::string( "┌ Src ─────┐" ) );
    } );

    registry.add( "a title too long is truncated with an ellipsis", []() {
        CellGrid grid( 12, 3 );
        grid.drawBox( 0, 0, 12, 3, "a-very-long-panel-title" );

        const std::string top = row( grid, 0 );
        UT_CHECK_MSG( top.find( "…" ) != std::string::npos,
                      "an over-long title should be elided, got: " << top );
        /* And it must not have grown the box. */
        UT_CHECK_EQ( (std::uint32_t) grid.at( 11, 0 ).ch, std::uint32_t( U'┐' ) );
        UT_CHECK_EQ( grid.height(), 3 );
    } );

    registry.add( "a box too small for a border is filled, not corrupted", []() {
        CellGrid grid( 4, 2 );
        grid.drawBox( 0, 0, 1, 1, "x" );
        UT_CHECK_EQ( (std::uint32_t) grid.at( 0, 0 ).ch, std::uint32_t( U' ' ) );
    } );

    registry.add( "toText strips trailing blanks and keeps every row", []() {
        CellGrid grid( 8, 3 );
        grid.drawText( 0, 0, "hi", 8 );
        grid.drawText( 0, 2, "there", 8 );

        const std::string text = grid.toText();
        UT_CHECK_EQ( text, std::string( "hi\n\nthere\n" ) );
    } );

    registry.add( "grids compare by content, not by identity", []() {
        CellGrid a( 4, 2 );
        CellGrid b( 4, 2 );
        UT_CHECK( a == b );

        a.put( 1, 1, U'x' );
        UT_CHECK( !( a == b ) );

        b.put( 1, 1, U'x' );
        UT_CHECK( a == b );

        /* Attributes are part of the content, even though goldens ignore them. */
        Attr bold;
        bold.bold = true;
        b.put( 1, 1, U'x', bold );
        UT_CHECK( !( a == b ) );
    } );

    registry.add( "resize clears, so a stale frame cannot show through", []() {
        CellGrid grid( 4, 2 );
        grid.drawText( 0, 0, "abcd", 4 );
        grid.resize( 4, 2 );
        UT_CHECK_EQ( grid.toText(), std::string( "\n\n" ) );
    } );

    return registry.run( "lens cell grid" ) == 0 ? 0 : 1;
}
