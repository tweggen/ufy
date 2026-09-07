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
    if( model.paletteActive() ) {
        /*
         * UI.md section 4: "the minibuffer occupies the hint line when
         * active". The palette's own tile lists the candidates; this is the
         * line being typed into, which is where the eye already is.
         */
        const Buffer* palette = model.focusedBuffer();
        const std::string input =
            ( palette && palette->kind == PanelKind::Palette )
                ? palette->palette.input : std::string();
        text = "M-x " + input + "_";
    } else if( !model.pendingKeys().empty() ) {
        text = toString( model.pendingKeys() ) + "-";
    } else {
        const std::vector<Hint> hints =
            functionKeyHints( model.commands(), model.keymap() );
        text = renderHintLine( hints, model.width() );
    }

    grid.fill( 0, y, model.width(), 1, U' ', Attr() );
    grid.drawText( 0, y, text, model.width(), Attr() );
}


/** The Help panel: a small hypertext, scrolled to keep the cursor visible. */
void drawHelp( CellGrid& grid, const Model& model, const Buffer& buffer,
               const Rect& inner )
{
    const HelpBook* book = model.helpBook();
    const HelpTopic* topic = book ? book->topic( buffer.help.topicId ) : NULL;

    if( !topic ) {
        grid.drawText( inner.x, inner.y,
                       "no help topic '" + buffer.help.topicId + "'",
                       inner.w, Attr() );
        return;
    }

    /* Keep the cursor on screen without storing scroll state per frame. */
    int scroll = 0;
    if( buffer.help.cursor >= inner.h ) {
        scroll = buffer.help.cursor - inner.h + 1;
    }

    for( int row = 0; row < inner.h; ++row ) {
        const int index = scroll + row;
        if( index >= (int) topic->lines.size() ) {
            break;
        }

        const std::string& raw = topic->lines[ (std::size_t) index ];

        Attr attr;
        if( index == buffer.help.cursor ) {
            attr.reverse = true;
        } else if( !HelpBook::linksOn( raw ).empty() ) {
            /*
             * A line carrying a link is marked with an attribute, not only a
             * colour: ARCHITECTURE section 6.2 forbids meaning carried by
             * colour alone, and "you can press Enter here" is meaning.
             */
            attr.underline = true;
            attr.colour = Colour::Accent;
        }

        grid.drawText( inner.x, inner.y + row,
                       HelpBook::stripLinkMarkup( raw ), inner.w, attr );
    }
}


/** The command palette: every command, filtered, with its binding. */
void drawPalette( CellGrid& grid, const Model& model, const Buffer& buffer,
                  const Rect& inner )
{
    const std::vector<const Command*> matches = model.paletteMatches();

    Attr prompt;
    prompt.bold = true;
    grid.drawText( inner.x, inner.y, "M-x " + buffer.palette.input + "_",
                   inner.w, prompt );

    if( matches.empty() ) {
        Attr none;
        none.colour = Colour::Dim;
        grid.drawText( inner.x, inner.y + 1, "(no command matches)", inner.w,
                       none );
        return;
    }

    /* Scroll so the selection stays visible in a short tile. */
    const int rows = inner.h - 1;
    int scroll = 0;
    if( rows > 0 && buffer.palette.selected >= rows ) {
        scroll = buffer.palette.selected - rows + 1;
    }

    for( int row = 0; row < rows; ++row ) {
        const int index = scroll + row;
        if( index >= (int) matches.size() ) {
            break;
        }
        const Command& command = *matches[ (std::size_t) index ];
        const KeySeq binding = model.keymap().bindingFor( command.id() );

        std::string line = command.id();
        if( !binding.empty() ) {
            line += "  (" + toString( binding ) + ")";
        }
        line += "  -- " + command.title();

        Attr attr;
        if( index == buffer.palette.selected ) {
            attr.reverse = true;
        } else if( !command.enabled( model ) ) {
            /*
             * Disabled, not hidden: UI.md's rule that discovering a command
             * exists must not require it to be usable at that moment.
             */
            attr.colour = Colour::Dim;
        }

        grid.drawText( inner.x, inner.y + 1 + row, line, inner.w, attr );
    }
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

    const Rect inner( placement.rect.x + 1, placement.rect.y + 1,
                      placement.rect.w - 2, placement.rect.h - 2 );
    if( inner.w <= 0 || inner.h <= 0 ) {
        return;
    }

    switch( buffer->kind ) {
    case PanelKind::Help:
        drawHelp( grid, model, *buffer, inner );
        return;
    case PanelKind::Palette:
        drawPalette( grid, model, *buffer, inner );
        return;
    case PanelKind::Placeholder:
        break;
    }

    for( int i = 0; i < inner.h && i < (int) buffer->lines.size(); ++i ) {
        grid.drawText( inner.x, inner.y + i, buffer->lines[ (std::size_t) i ],
                       inner.w, Attr() );
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
