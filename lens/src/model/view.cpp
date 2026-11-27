/**
 * @file view.cpp
 *
 * The four frame regions of UI.md section 1, and the tiles between them.
 */

#include "view.hpp"

#include "../modreg/help.hpp"
#include "text-wrap.hpp"

#include <sstream>

namespace lens {

namespace {

/**
 * UI.md section 1: the menu bar.
 *
 * Every heading has its mnemonic underlined, always -- not only while the
 * bar has the keyboard -- because the point of a mnemonic is that you can
 * use it without first opening the menu to find out what it is. The active
 * heading is drawn OUT of reverse video, so it stands out in the monochrome
 * tier as well as in colour (ARCHITECTURE section 6.2).
 */
void drawMenuBar( CellGrid& grid, const Model& model )
{
    Attr bar;
    bar.reverse = true;
    grid.fill( 0, 0, model.width(), 1, U' ', bar );

    const std::vector<MenuHeading> headings = menuHeadings();
    for( std::size_t i = 0; i < headings.size(); ++i ) {
        const int column = menuHeadingColumn( i );
        const bool active = model.menuActive()
                         && (int) i == model.menuBar().heading;

        Attr attr = bar;
        if( active ) {
            attr.reverse = false;
            attr.bold = true;
            /* One column of margin either side, so the lit heading reads
             * as a selected cell rather than as recoloured letters. */
            grid.fill( column - 1, 0, (int) headings[i].name.size() + 2, 1,
                       U' ', attr );
        }
        grid.drawText( column, 0, headings[i].name,
                       model.width() - column, attr );

        const int mnemonic = menuMnemonic( headings[i], model.keymap() );
        if( mnemonic >= 0 ) {
            Attr underlined = attr;
            underlined.underline = true;
            grid.drawText( column + mnemonic, 0,
                           headings[i].name.substr( (std::size_t) mnemonic, 1 ),
                           model.width() - column - mnemonic, underlined );
        }
    }
}


/**
 * The open drop-down: the one overlay (UI.md section 1).
 *
 * Drawn after the tiles, over them, inside the rectangle the model computed
 * -- so it is the model, not the renderer, that guarantees it fits.
 */
void drawMenuDropdown( CellGrid& grid, const Model& model )
{
    const Rect box = model.menuDropdownRect();
    if( box.w < 3 || box.h < 3 ) {
        return;
    }
    const MenuBarState& menu = model.menuBar();
    const std::vector<const Command*> items = model.menuItems( menu.heading );

    /* An overlay must erase what is under it, or tile text shows through
     * the gaps between items. */
    grid.fill( box.x, box.y, box.w, box.h, U' ', Attr() );

    Attr border;
    border.bold = true;
    grid.drawBox( box.x, box.y, box.w, box.h, std::string(), border );

    const Rect inner( box.x + 1, box.y + 1, box.w - 2, box.h - 2 );

    if( items.empty() ) {
        Attr empty;
        empty.colour = Colour::Dim;
        grid.drawText( inner.x + 1, inner.y, kMenuEmptyText, inner.w - 2, empty );
        return;
    }

    const int scroll = ensureVisible( menu.selected, (int) items.size(),
                                      inner.h, menu.top );

    for( int row = 0; row < inner.h; ++row ) {
        const int index = scroll + row;
        if( index >= (int) items.size() ) { break; }
        const Command& command = *items[ (std::size_t) index ];

        Attr attr;
        if( index == menu.selected ) {
            attr.reverse = true;
        } else if( !command.enabled( model ) ) {
            /* Disabled, not hidden -- same rule as the palette. */
            attr.colour = Colour::Dim;
        }

        grid.fill( inner.x, inner.y + row, inner.w, 1, U' ', attr );
        grid.drawText( inner.x + 1, inner.y + row, command.title(),
                       inner.w - 2, attr );

        const KeySeq binding = model.keymap().bindingFor( command.id() );
        if( !binding.empty() ) {
            const std::string keys = toString( binding );
            const int at = inner.x + inner.w - 1 - displayWidth( keys );
            grid.drawText( at, inner.y + row, keys, displayWidth( keys ), attr );
        }
    }

    /* Say when there is more, on the border, where it covers no item. */
    if( scroll > 0 ) {
        grid.drawText( box.x + box.w - 3, box.y, "\xe2\x96\xb4", 1, border );
    }
    if( scroll + inner.h < (int) items.size() ) {
        grid.drawText( box.x + box.w - 3, box.y + box.h - 1, "\xe2\x96\xbe", 1,
                       border );
    }
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
    if( model.message().empty() && !model.capabilities().coreName.empty() ) {
        text = model.sessionStatus();
    }

    /*
     * While a drop-down is open, what the highlighted item DOES -- the
     * status line of the Borland IDEs, which is how their menus explained
     * themselves without a manual. Generated from the command's own help
     * text, so it cannot drift from the command.
     */
    if( model.message().empty() && model.menuOpen() ) {
        const std::vector<const Command*> items =
            model.menuItems( model.menuBar().heading );
        const int selected = model.menuBar().selected;
        if( selected >= 0 && selected < (int) items.size() ) {
            text = items[ (std::size_t) selected ]->help();
        }
    }
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
    } else if( model.menuOpen() ) {
        /* The menu's own keys, which no command table entry describes:
         * they are fixed by foldMenu, not bindings. */
        text = "Up/Down Choose  Left/Right Next menu  Enter Run  Esc Back";
    } else if( model.menuActive() ) {
        text = "Left/Right Choose  Enter Open  Esc Close";
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

    /*
     * Wrap first, then scroll, so the cursor is still counted in SOURCE
     * lines -- Up/Down and Enter address the topic's lines, not the
     * accidents of how wide the tile happens to be.
     */
    struct Rendered {
        std::string text;
        int source;
    };
    std::vector<Rendered> rendered;
    int cursorRow = 0;

    for( std::size_t i = 0; i < topic->lines.size(); ++i ) {
        const std::string display = HelpBook::stripLinkMarkup( topic->lines[i] );
        const std::vector<std::string> wrapped = wrapLine( display, inner.w );

        if( (int) i == buffer.help.cursor ) {
            cursorRow = (int) rendered.size();
        }
        for( std::size_t k = 0; k < wrapped.size(); ++k ) {
            Rendered row;
            row.text = wrapped[k];
            row.source = (int) i;
            rendered.push_back( row );
        }
        if( wrapped.empty() ) {
            Rendered row;
            row.text.clear();
            row.source = (int) i;
            rendered.push_back( row );
        }
    }

    /*
     * The viewport is MODEL state (buffer.help.top), maintained by fold.
     * The view only clamps it, so that a resize between keystrokes cannot
     * leave the cursor off screen -- it never decides where the view should
     * be, because that decision needs to know where it was.
     */
    const int scroll = ensureVisible( cursorRow, (int) rendered.size(),
                                      inner.h, buffer.help.top );

    for( int row = 0; row < inner.h; ++row ) {
        const int index = scroll + row;
        if( index >= (int) rendered.size() ) {
            break;
        }

        const Rendered& line = rendered[ (std::size_t) index ];
        const std::string& raw =
            topic->lines[ (std::size_t) line.source ];

        Attr attr;
        if( line.source == buffer.help.cursor ) {
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

        grid.drawText( inner.x, inner.y + row, line.text, inner.w, attr );
    }

    /*
     * Say when there is more.
     *
     * At 80x24 the welcome page shows its first eight lines, and without
     * this a first-time reader sees three keys, no sign of the rest, and no
     * reason to press Down -- so they never find out how to quit. The
     * marker is drawn over the right edge of the last row, which is the one
     * place guaranteed not to be the start of a sentence.
     */
    const int shown = (int) rendered.size() - scroll;
    if( shown > inner.h ) {
        const std::string marker = " more \xe2\x96\xbe Down ";
        const int markerWidth = displayWidth( marker );
        if( markerWidth < inner.w ) {
            Attr attr;
            attr.reverse = true;
            grid.drawText( inner.x + inner.w - markerWidth,
                           inner.y + inner.h - 1, marker, markerWidth, attr );
        }
    }
    if( scroll > 0 ) {
        const std::string marker = " Up \xe2\x96\xb4 more ";
        const int markerWidth = displayWidth( marker );
        if( markerWidth < inner.w ) {
            Attr attr;
            attr.reverse = true;
            grid.drawText( inner.x + inner.w - markerWidth, inner.y,
                           marker, markerWidth, attr );
        }
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
    const int scroll = ensureVisible( buffer.palette.selected,
                                      (int) matches.size(), rows,
                                      buffer.palette.top );

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


/** The Transcript: what happened, then the prompt at the bottom. */
void drawTranscript( CellGrid& grid, const Buffer& buffer, const Rect& inner,
                     bool focused )
{
    const TranscriptState& t = buffer.transcript;

    /*
     * The prompt owns the last row and the history fills upward from it, so
     * the newest line is always adjacent to where you are typing. A
     * transcript that scrolled the prompt off the bottom would be a log, not
     * a REPL.
     */
    const int promptRow = inner.y + inner.h - 1;
    const int historyRows = transcriptHistoryRows( inner.h );

    /*
     * Pinned to the newest line at the prompt; while a past line is
     * selected, wherever fold left the viewport -- clamped, so a resize
     * between keystrokes cannot leave the cursor off screen. The same rule
     * as every other list panel (scroll.hpp).
     */
    const int total = (int) t.entries.size();
    const bool browsing = t.selected >= 0 && t.selected < total;
    int first = total - historyRows;
    if( browsing ) {
        first = ensureVisible( t.selected, total, historyRows, t.top );
    }
    if( first < 0 ) { first = 0; }

    for( int row = 0; row < historyRows; ++row ) {
        const int index = first + row;
        if( index >= total ) { break; }

        const TranscriptEntry& entry = t.entries[ (std::size_t) index ];

        Attr attr;
        switch( entry.kind ) {
        case TranscriptEntry::Kind::Input:
            attr.bold = true;
            break;
        case TranscriptEntry::Kind::Diagnostic:
            /* Underlined as well as coloured: no meaning by colour alone. */
            attr.colour = Colour::Error;
            attr.underline = true;
            break;
        case TranscriptEntry::Kind::Status:
        case TranscriptEntry::Kind::Info:
            attr.colour = Colour::Dim;
            break;
        case TranscriptEntry::Kind::Output:
        case TranscriptEntry::Kind::Solution:
            break;
        }
        if( browsing && index == t.selected ) {
            attr.reverse = true;
        }

        grid.drawText( inner.x, inner.y + row, entry.text, inner.w, attr );
    }

    /* The prompt, the input, and a block cursor when this tile has focus. */
    Attr promptAttr;
    promptAttr.bold = true;
    const int promptWidth =
        grid.drawText( inner.x, promptRow, kTranscriptPrompt, inner.w,
                       promptAttr );

    const std::string before = t.input.substr( 0, t.cursor );
    const std::string after = t.input.substr( t.cursor );

    int at = promptWidth;
    at += grid.drawText( inner.x + at, promptRow, before, inner.w - at, Attr() );

    /* One cursor on screen at a time: while a past line is selected, the
     * highlight is on it and the prompt shows no block. */
    if( focused && !browsing ) {
        Attr cursor;
        cursor.reverse = true;
        const std::string under = after.empty() ? " " : after.substr( 0, 1 );
        at += grid.drawText( inner.x + at, promptRow, under, inner.w - at,
                             cursor );
        if( !after.empty() ) {
            grid.drawText( inner.x + at, promptRow, after.substr( 1 ),
                           inner.w - at, Attr() );
        }
    } else {
        grid.drawText( inner.x + at, promptRow, after, inner.w - at, Attr() );
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
    case PanelKind::Transcript:
        drawTranscript( grid, *buffer, inner, focused );
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

    const Rect area = model.tileArea();
    const Solution solution = solve( model.layout(), area );

    for( std::size_t i = 0; i < solution.placements.size(); ++i ) {
        const Placement& placement = solution.placements[i];
        drawTile( grid, model, placement,
                  placement.tile == model.layout().focused() );
    }

    drawStatusLine( grid, model );
    drawHintLine( grid, model );

    /* Last, because the drop-down is drawn OVER the tiles. */
    drawMenuBar( grid, model );
    drawMenuDropdown( grid, model );

    return grid;
}

} // namespace lens
