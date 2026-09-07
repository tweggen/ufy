/**
 * @file view.cpp
 *
 * The four frame regions of UI.md section 1, and the tiles between them.
 */

#include "view.hpp"

#include "../modreg/help.hpp"

#include <sstream>

namespace lens {

namespace {

/** UI.md section 1: the menu bar's fixed entries. */
const char* const kMenuBar =
    " File  Edit  World  Query  Image  Debug  Window  Help";


void drawMenuBar( CellGrid& grid, const Model& model )
{
    Attr attr;
    attr.reverse = true;

    grid.fill( 0, 0, model.width(), 1, U' ', attr );
    grid.drawText( 0, 0, kMenuBar, model.width(), attr );
}


void drawStatusLine( CellGrid& grid, const Model& model )
{
    const int y = model.height() - 2;

    Attr attr;
    attr.reverse = true;
    grid.fill( 0, y, model.width(), 1, U' ', attr );

    /*
     * A message displaces the status for as long as it is set. It is the
     * answer to something the user just did, so it wins over standing
     * information they can get back by doing nothing.
     */
    std::string text = model.message().empty() ? model.status() : model.message();
    grid.drawText( 1, y, text, model.width() - 2, attr );
}


void drawHintLine( CellGrid& grid, const Model& model )
{
    const int y = model.height() - 1;

    /*
     * UI.md section 5.1: never blank and never stale, because it is
     * generated from the command table rather than written down. An
     * unfinished chord displaces it -- a prefix that silently swallowed the
     * next keystroke is indistinguishable from a hang.
     */
    std::string text;
    if( !model.pendingKeys().empty() ) {
        text = toString( model.pendingKeys() ) + "-";
    } else {
        const std::vector<Hint> hints =
            functionKeyHints( model.commands(), model.keymap() );
        text = renderHintLine( hints, model.width() );
    }

    grid.fill( 0, y, model.width(), 1, U' ', Attr() );
    grid.drawText( 0, y, text, model.width(), Attr() );
}


/** One tile: a titled box, with the buffer's placeholder lines inside. */
void drawTile( CellGrid& grid, const Model& model, const Placement& placement,
               bool focused )
{
    const BufferId bufferId = model.layout().bufferOf( placement.tile );
    const Buffer* buffer = model.buffer( bufferId );
    const std::string title = buffer ? buffer->title : std::string( "(empty)" );

    if( placement.stub ) {
        /*
         * A demoted tile: one row, its title, and a marker saying it is
         * folded rather than empty. Still focusable -- Tab reaches it, and
         * focusing it is what un-demotes it on the next solve.
         */
        Attr attr;
        attr.colour = Colour::Dim;
        if( focused ) {
            attr.reverse = true;
            attr.colour = Colour::Accent;
        }
        grid.fill( placement.rect.x, placement.rect.y, placement.rect.w, 1,
                   U' ', attr );
        grid.drawText( placement.rect.x, placement.rect.y,
                       "\xe2\x96\xb8 " + title, placement.rect.w, attr );
        return;
    }

    Attr border;
    if( focused ) {
        border.colour = Colour::Accent;
        border.bold = true;
    } else {
        border.colour = Colour::Dim;
    }

    grid.drawBox( placement.rect.x, placement.rect.y,
                  placement.rect.w, placement.rect.h, title, border );

    if( !buffer ) {
        return;
    }

    const int innerX = placement.rect.x + 1;
    const int innerY = placement.rect.y + 1;
    const int innerW = placement.rect.w - 2;
    const int innerH = placement.rect.h - 2;

    for( int i = 0; i < innerH && i < (int) buffer->lines.size(); ++i ) {
        grid.drawText( innerX, innerY + i, buffer->lines[ (std::size_t) i ],
                       innerW, Attr() );
    }
}

} // namespace

CellGrid view( const Model& model )
{
    CellGrid grid( model.width(), model.height() );

    if( model.width() <= 0 || model.height() <= 0 ) {
        return grid;
    }

    drawMenuBar( grid, model );

    const Rect area = model.tileArea();
    const Solution solution = solve( model.layout(), area );

    for( std::size_t i = 0; i < solution.placements.size(); ++i ) {
        const Placement& placement = solution.placements[i];
        drawTile( grid, model, placement,
                  placement.tile == model.layout().focused() );
    }

    drawStatusLine( grid, model );
    drawHintLine( grid, model );

    return grid;
}

} // namespace lens
