/**
 * @file view.cpp
 *
 * The four frame regions of UI.md section 1, and the tiles between them.
 */

#include "view.hpp"

#include "../modreg/help.hpp"
#include "text-wrap.hpp"

#include <algorithm>
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
        const bool isPalette = palette && palette->kind == PanelKind::Palette;
        const std::string input = isPalette ? palette->palette.input
                                            : std::string();
        const std::string label =
            ( isPalette && !palette->palette.prompt.empty() )
                ? palette->palette.prompt : std::string( "M-x " );
        text = label + input + "_";
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
    Attr prompt;
    prompt.bold = true;

    if( !buffer.palette.prompt.empty() ) {
        /* A question: the line being answered, and how to answer it. */
        grid.drawText( inner.x, inner.y,
                       buffer.palette.prompt + buffer.palette.input + "_",
                       inner.w, prompt );
        Attr how;
        how.colour = Colour::Dim;
        grid.drawText( inner.x, inner.y + 1,
                       "Enter accepts \xc2\xb7 Esc cancels \xc2\xb7 C-u clears",
                       inner.w, how );
        return;
    }

    const std::vector<const Command*> matches = model.paletteMatches();

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


/** Two lines saying what an empty panel is for and how to fill it. */
void drawEmpty( CellGrid& grid, const Rect& inner, const char* what,
                const char* how )
{
    Attr dim;
    dim.colour = Colour::Dim;
    grid.drawText( inner.x, inner.y, what, inner.w, Attr() );
    if( inner.h > 1 ) {
        grid.drawText( inner.x, inner.y + 1, how, inner.w, dim );
    }
}


/** The widest a Solutions column grows before its values are clipped. */
constexpr int kMaxColumnWidth = 32;

/**
 * G5.1: the current query's bindings, one row per solution.
 *
 * A header of variable names, the rows, and a footer that says whether more
 * are coming and how to get them. Without the footer the last row of a
 * batch looks exactly like the last row of the answer.
 */
void drawSolutions( CellGrid& grid, const Model& model, const Rect& inner,
                    bool focused )
{
    const ResultsState& r = model.results();
    if( r.query == us::kNoQuery ) {
        drawEmpty( grid, inner, "no query yet",
                   "solve a goal in the Transcript" );
        return;
    }

    const int total = (int) r.solutions.size();
    const int listRows = solutionsListRows( inner.h );

    /* Column widths: the index, then each variable, from what is held. */
    std::ostringstream lastIndex;
    lastIndex << std::max( 1, total );
    const int indexWidth = std::max( 1, displayWidth( lastIndex.str() ) );

    std::vector<int> widths;
    for( std::size_t c = 0; c < r.columns.size(); ++c ) {
        int w = displayWidth( r.columns[c] );
        for( std::size_t i = 0; i < r.solutions.size(); ++i ) {
            const us::Solution& sol = r.solutions[i];
            for( std::size_t b = 0; b < sol.bindings.size(); ++b ) {
                if( sol.bindings[b].first == r.columns[c] ) {
                    w = std::max( w, displayWidth( renderValue( sol.bindings[b].second ) ) );
                }
            }
        }
        widths.push_back( std::min( w, kMaxColumnWidth ) );
    }

    Attr header;
    header.bold = true;
    header.underline = true;
    int x = inner.x + indexWidth + 2;
    grid.drawText( inner.x, inner.y, "#", indexWidth, header );
    for( std::size_t c = 0; c < r.columns.size(); ++c ) {
        const int room = inner.x + inner.w - x;
        if( room <= 0 ) { break; }
        grid.drawText( x, inner.y, r.columns[c], std::min( room, widths[c] ), header );
        x += widths[c] + 2;
    }
    if( r.columns.empty() && total > 0 ) {
        grid.drawText( inner.x + indexWidth + 2, inner.y,
                       "(no variables)", inner.w - indexWidth - 2, header );
    }

    const int top = ensureVisible( r.selected, total, listRows, r.top );
    for( int row = 0; row < listRows; ++row ) {
        const int index = top + row;
        if( index >= total ) { break; }
        const us::Solution& sol = r.solutions[ (std::size_t) index ];
        const int y = inner.y + 1 + row;

        Attr attr;
        if( index == r.selected ) {
            attr.reverse = focused;
            attr.bold = !focused;
        }
        if( attr.reverse ) {
            grid.fill( inner.x, y, inner.w, 1, U' ', attr );
        }

        std::ostringstream number;
        number << ( index + 1 );
        const std::string n = number.str();
        grid.drawText( inner.x + indexWidth - displayWidth( n ), y, n,
                       displayWidth( n ), attr );

        int cx = inner.x + indexWidth + 2;
        if( r.columns.empty() ) {
            grid.drawText( cx, y, "yes", inner.x + inner.w - cx, attr );
            continue;
        }
        for( std::size_t c = 0; c < r.columns.size(); ++c ) {
            const int room = inner.x + inner.w - cx;
            if( room <= 0 ) { break; }
            std::string cell;
            for( std::size_t b = 0; b < sol.bindings.size(); ++b ) {
                if( sol.bindings[b].first == r.columns[c] ) {
                    cell = renderValue( sol.bindings[b].second );
                }
            }
            /* The last column may use the rest of the row. */
            const int width = ( c + 1 == r.columns.size() )
                                  ? room : std::min( room, widths[c] );
            grid.drawText( cx, y, cell, width, attr );
            cx += widths[c] + 2;
        }
    }

    std::ostringstream foot;
    if( r.detached ) {
        foot << "-- " << total << " shown \xc2\xb7 detached";
    } else if( r.finished ) {
        if( total == 0 )      { foot << "-- no solutions"; }
        else if( total == 1 ) { foot << "-- 1 solution"; }
        else                  { foot << "-- " << total << " solutions"; }
    } else if( r.demandPending ) {
        foot << "-- " << total << " so far \xc2\xb7 asking for more\xe2\x80\xa6";
    } else if( r.more ) {
        foot << "-- " << total << " so far \xc2\xb7 Down past the end for more";
    } else {
        foot << "-- " << total << " so far";
    }
    if( inner.h >= 2 ) {
        Attr dim;
        dim.colour = Colour::Dim;
        grid.drawText( inner.x, inner.y + inner.h - 1, foot.str(), inner.w, dim );
    }
}


/**
 * G5.2: the selected solution as a tree.
 *
 * `\xe2\x96\xb8` closed, `\xe2\x96\xbe` open, nothing for a leaf. A node the budget cut
 * keeps the transcript's U+2026 mark in its text and says it is being
 * fetched while an `inspect` is in flight.
 */
void drawInspector( CellGrid& grid, const Model& model, const Rect& inner,
                    bool focused )
{
    const ResultsState& r = model.results();
    if( r.selected < 0 || r.selected >= (int) r.solutions.size() ) {
        drawEmpty( grid, inner, "nothing to inspect",
                   "solve a goal; its solutions open here" );
        return;
    }
    const std::vector<InspectorRow> rows = model.inspectorRows();
    if( rows.empty() ) {
        drawEmpty( grid, inner, "yes",
                   "the goal holds, and binds no variables" );
        return;
    }

    const InspectorState& in = model.inspector();
    const int total = (int) rows.size();
    const int top = ensureVisible( in.cursor, total, inner.h, in.top );
    for( int row = 0; row < inner.h; ++row ) {
        const int index = top + row;
        if( index >= total ) { break; }
        const InspectorRow& line = rows[ (std::size_t) index ];

        std::string text( (std::size_t) line.depth * 2, ' ' );
        text += line.expandable ? ( line.open ? "\xe2\x96\xbe " : "\xe2\x96\xb8 " ) : "  ";
        text += line.text;
        if( line.fetching ) {
            text += "  (fetching\xe2\x80\xa6)";
        }

        Attr attr;
        if( index == in.cursor ) {
            attr.reverse = focused;
            attr.bold = !focused;
        }
        grid.drawText( inner.x, inner.y + row, text, inner.w, attr );
    }
}


/** G5.3: every diagnostic received, newest last. */
void drawDiagnostics( CellGrid& grid, const Model& model, const Rect& inner,
                      bool focused )
{
    const DiagnosticsState& d = model.diagnostics();
    if( d.items.empty() ) {
        grid.drawText( inner.x, inner.y, "(none)", inner.w, Attr() );
        return;
    }
    const int total = (int) d.items.size();
    const int top = ensureVisible( d.selected, total, inner.h, d.top );
    for( int row = 0; row < inner.h; ++row ) {
        const int index = top + row;
        if( index >= total ) { break; }
        Attr attr;
        attr.colour = Colour::Error;
        if( index == d.selected ) {
            attr.reverse = focused;
            attr.bold = !focused;
        }
        grid.drawText( inner.x, inner.y + row,
                       diagnosticHeadline( d.items[ (std::size_t) index ] ),
                       inner.w, attr );
    }
}


/**
 * Where a diagnostic sent the Source panel.
 *
 * The reported line, numbered, with a caret under the column -- which is all
 * a Diagnostic carries. Showing the whole definition needs gate G3's
 * `source` request, and the panel says so on its last line rather than
 * letting one line pass for the file.
 */
void drawDefinition( CellGrid& grid, const Model& model, const Rect& inner,
                     bool focused )
{
    const SourceDefinition& d = model.sourceDefinition();
    Attr bold;
    bold.bold = true;
    Attr dim;
    dim.colour = Colour::Dim;

    std::ostringstream head;
    head << d.key.name << "/" << d.key.arity << " \xc2\xb7 " << d.clauses
         << ( d.clauses == 1 ? " clause" : " clauses" );
    if( d.redefinePending )  { head << " \xc2\xb7 redefining\xe2\x80\xa6"; }
    else if( d.dirty )       { head << " \xc2\xb7 modified"; }
    grid.drawText( inner.x, inner.y, head.str(), inner.w, bold );

    /*
     * The policy selector, on its own line so it can have its own focus
     * (Up from the first line) and F1 on it can explain the policies.
     * Reverse video while it has the keyboard, and the arrows say how to
     * change it -- a selector that looked like a label would never be used.
     */
    if( inner.h > 1 ) {
        Attr selector = dim;
        if( focused && d.selectorFocused ) {
            selector = Attr();
            selector.reverse = true;
        }
        const std::string text = std::string( "F3 redefines \xc2\xb7 policy: \xe2\x97\x82 " )
                               + policyName( d.policy ) + " \xe2\x96\xb8";
        grid.drawText( inner.x, inner.y + 1, text, inner.w, selector );
    }

    const int rows = sourceTextRows( inner.h );
    const int total = (int) d.lines.size();
    const int top = ensureVisible( d.cursor, total, rows, d.top );
    const bool editing = focused && !d.selectorFocused;
    for( int row = 0; row < rows; ++row ) {
        const int index = top + row;
        if( index >= total ) { break; }
        const std::string& line = d.lines[ (std::size_t) index ];
        const int y = inner.y + 2 + row;
        if( !editing || index != d.cursor ) {
            grid.drawText( inner.x, y, line, inner.w, Attr() );
            continue;
        }
        /* The editing line: a block cursor at the column, as the prompt has. */
        const std::size_t column = std::min( d.column, line.size() );
        int at = grid.drawText( inner.x, y, line.substr( 0, column ), inner.w, Attr() );
        Attr cursor;
        cursor.reverse = true;
        std::string under = " ";
        std::size_t next = column;
        if( column < line.size() ) {
            next = column + 1;
            while( next < line.size()
                   && ( (unsigned char) line[next] & 0xC0 ) == 0x80 ) { ++next; }
            under = line.substr( column, next - column );
        }
        at += grid.drawText( inner.x + at, y, under, inner.w - at, cursor );
        if( next < line.size() ) {
            grid.drawText( inner.x + at, y, line.substr( next ), inner.w - at, Attr() );
        }
    }
}


/** G3.1: the catalogue, grouped by provenance, or filtered flat. */
void drawCatalogue( CellGrid& grid, const Model& model, const Rect& inner,
                    bool focused )
{
    const CatalogueState& c = model.catalogue();
    if( !c.loaded ) {
        drawEmpty( grid, inner, "no listing yet",
                   c.listingPending ? "asking the engine\xe2\x80\xa6"
                                    : "the catalogue fills when lens has an engine" );
        return;
    }

    int y = inner.y;
    if( !c.filter.empty() ) {
        Attr filterAttr;
        filterAttr.bold = true;
        grid.drawText( inner.x, y, "filter: " + c.filter, inner.w, filterAttr );
        ++y;
    }

    const std::vector<CatalogueRow> rows = model.catalogueRows();
    const int listRows = catalogueListRows( inner.h, c );
    if( rows.empty() ) {
        Attr dim;
        dim.colour = Colour::Dim;
        grid.drawText( inner.x, y, c.filter.empty() ? "(empty world)" : "(no match)",
                       inner.w, dim );
        return;
    }

    const int total = (int) rows.size();
    const int top = ensureVisible( c.selected, total, listRows, c.top );
    for( int row = 0; row < listRows; ++row ) {
        const int index = top + row;
        if( index >= total ) { break; }
        const CatalogueRow& r = rows[ (std::size_t) index ];

        Attr attr;
        std::string text;
        std::string count;
        if( r.kind == CatalogueRow::Kind::Group ) {
            attr.bold = true;
            text = ( r.open ? "\xe2\x96\xbe " : "\xe2\x96\xb8 " ) + r.label;
            if( !r.open ) {
                std::ostringstream os;
                os << " (" << r.count << ")";
                text += os.str();
            }
        } else {
            text = ( c.filter.empty() ? "    " : "" ) + r.label;
            std::ostringstream os;
            os << r.count;
            count = os.str();
            if( !c.filter.empty() ) {
                text += "  (" + r.group + ")";
            }
        }
        if( index == c.selected ) {
            attr.reverse = focused;
            attr.bold = attr.bold || !focused;
        }
        if( attr.reverse ) {
            grid.fill( inner.x, y + row, inner.w, 1, U' ', attr );
        }
        grid.drawText( inner.x, y + row, text, inner.w, attr );
        if( !count.empty() ) {
            const int cw = displayWidth( count );
            if( cw + 1 < inner.w ) {
                grid.drawText( inner.x + inner.w - cw, y + row, count, cw, attr );
            }
        }
    }
}


void drawSource( CellGrid& grid, const Model& model, const Rect& inner,
                 bool focused )
{
    if( model.sourceShows() == Model::SourceShows::Definition
        && model.sourceDefinition().set ) {
        drawDefinition( grid, model, inner, focused );
        return;
    }
    const SourceLocation& at = model.sourceLocation();
    if( !at.set || model.sourceShows() != Model::SourceShows::Location ) {
        drawEmpty( grid, inner, "the selected definition",
                   "awaiting source (G3)" );
        return;
    }

    Attr dim;
    dim.colour = Colour::Dim;

    std::ostringstream where;
    where << ( at.file.empty() ? "<input>" : at.file ) << ":" << at.line;
    if( at.column > 0 ) { where << ":" << at.column; }
    Attr bold;
    bold.bold = true;
    grid.drawText( inner.x, inner.y, where.str(), inner.w, bold );
    if( inner.h > 1 ) {
        grid.drawText( inner.x, inner.y + 1, at.message, inner.w, dim );
    }

    if( inner.h > 3 && !at.text.empty() ) {
        std::ostringstream number;
        number << at.line;
        const std::string gutter = " " + number.str() + " \xe2\x94\x82 ";
        const int gutterWidth = displayWidth( gutter );
        grid.drawText( inner.x, inner.y + 3, gutter, inner.w, dim );
        grid.drawText( inner.x + gutterWidth, inner.y + 3, at.text,
                       inner.w - gutterWidth, Attr() );
        if( inner.h > 4 ) {
            const std::string pad( (std::size_t) std::max( 0, gutterWidth - 2 ), ' ' );
            grid.drawText( inner.x, inner.y + 4, pad + "\xe2\x94\x82 ", inner.w, dim );
            const int caret = inner.x + gutterWidth
                            + ( at.column > 1 ? (int) at.column - 1 : 0 );
            if( caret < inner.x + inner.w ) {
                Attr mark;
                mark.colour = Colour::Error;
                mark.bold = true;
                grid.drawText( caret, inner.y + 4, "^", 1, mark );
            }
        }
    }

    if( inner.h > 6 ) {
        grid.drawText( inner.x, inner.y + inner.h - 1,
                       "only the reported line is known; the whole "
                       "definition arrives with G3",
                       inner.w, dim );
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
    case PanelKind::Solutions:
        drawSolutions( grid, model, inner, focused );
        return;
    case PanelKind::Inspector:
        drawInspector( grid, model, inner, focused );
        return;
    case PanelKind::Diagnostics:
        drawDiagnostics( grid, model, inner, focused );
        return;
    case PanelKind::Source:
        drawSource( grid, model, inner, focused );
        return;
    case PanelKind::Catalogue:
        drawCatalogue( grid, model, inner, focused );
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
