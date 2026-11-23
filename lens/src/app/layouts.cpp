/**
 * @file layouts.cpp
 */

#include "layouts.hpp"

#include <string>

namespace lens {

namespace {

/*
 * Placeholder content, so the shell can be built, gated and LOOKED AT before
 * any panel exists. Each line says what the panel will show rather than
 * pretending to show it -- a mock that looked real would make the goldens
 * feel finished and would have to be re-recorded the moment a panel landed.
 */
struct PanelSeed {
    const char* title;
    const char* line1;
    const char* line2;
};

BufferId seed( Model& model, const PanelSeed& panel )
{
    /*
     * The transcript is a real panel now, so it gets no placeholder text:
     * an empty transcript with a prompt is what a REPL looks like before you
     * type, and inventing content for it would be inventing history.
     */
    if( std::string( panel.title ) == "Transcript" ) {
        return model.addBuffer( panel.title, {}, PanelKind::Transcript );
    }

    std::vector<std::string> lines;
    lines.push_back( panel.line1 );
    if( panel.line2 && *panel.line2 ) {
        lines.push_back( panel.line2 );
    }
    return model.addBuffer( panel.title, lines );
}

const PanelSeed kCatalogue  = { "Catalogue",   "module -> (name, arity)",
                                "awaiting listing (G3)" };
const PanelSeed kSource     = { "Source",      "the selected definition",
                                "awaiting source (G3)" };
const PanelSeed kDiagnostics= { "Diagnostics", "(none)", "" };
const PanelSeed kTranscript = { "Transcript",  "?- ", "" };
const PanelSeed kSolutions  = { "Solutions",   "bindings, one row per solution",
                                "awaiting solve (G5)" };
const PanelSeed kTrace      = { "Trace",       "goal stack and breakpoints",
                                "unavailable: core reports debug: false" };
const PanelSeed kInspector  = { "Inspector",   "the selected binding",
                                "awaiting inspect (G5)" };
const PanelSeed kQueries    = { "Queries",     "queries in flight or retained",
                                "(none)" };

} // namespace

std::vector<std::string> stockLayoutNames()
{
    std::vector<std::string> names;
    names.push_back( "browse" );
    names.push_back( "run" );
    names.push_back( "debug" );
    names.push_back( "full" );
    return names;
}


bool applyStockLayout( Model& model, const std::string& name )
{
    if( name == "browse" ) {
        /*
         * The system-browser stance: catalogue on the left, source above
         * diagnostics on the right, transcript across the bottom.
         */
        const BufferId catalogue = seed( model, kCatalogue );
        const BufferId source = seed( model, kSource );
        const BufferId diagnostics = seed( model, kDiagnostics );
        const BufferId transcript = seed( model, kTranscript );

        LayoutTree tree( catalogue );
        const TileId catalogueTile = tree.focused();

        /* Top band over the transcript. */
        tree.splitFocused( Split::Rows, transcript, 0.72 );

        /* Split the top band into catalogue | (source over diagnostics). */
        tree.focus( catalogueTile );
        tree.splitFocused( Split::Columns, source, 0.28 );
        tree.splitFocused( Split::Rows, diagnostics, 0.70 );

        tree.focus( catalogueTile );
        model.layout() = tree;
        return true;
    }

    if( name == "run" ) {
        /* Transcript maximised, solutions beside it, catalogue as a stub. */
        const BufferId transcript = seed( model, kTranscript );
        const BufferId solutions = seed( model, kSolutions );
        const BufferId catalogue = seed( model, kCatalogue );

        LayoutTree tree( transcript );
        const TileId transcriptTile = tree.focused();

        tree.splitFocused( Split::Rows, catalogue, 0.86 );
        tree.focus( transcriptTile );
        tree.splitFocused( Split::Columns, solutions, 0.62 );

        tree.focus( transcriptTile );
        model.layout() = tree;
        return true;
    }

    if( name == "debug" ) {
        /* Why does it do that: trace and queries left, source and bindings right. */
        const BufferId trace = seed( model, kTrace );
        const BufferId queries = seed( model, kQueries );
        const BufferId source = seed( model, kSource );
        const BufferId inspector = seed( model, kInspector );

        LayoutTree tree( trace );
        const TileId traceTile = tree.focused();

        tree.splitFocused( Split::Columns, source, 0.45 );
        tree.splitFocused( Split::Rows, inspector, 0.60 );

        tree.focus( traceTile );
        tree.splitFocused( Split::Rows, queries, 0.65 );

        tree.focus( traceTile );
        model.layout() = tree;
        return true;
    }

    if( name == "full" ) {
        /*
         * Everything at once. Only sensible above ~150 columns, and present
         * because someone will have that screen -- at 80x24 the solver folds
         * most of it into stubs, which is the honest answer rather than a
         * refusal.
         */
        const BufferId catalogue = seed( model, kCatalogue );
        const BufferId source = seed( model, kSource );
        const BufferId diagnostics = seed( model, kDiagnostics );
        const BufferId transcript = seed( model, kTranscript );
        const BufferId solutions = seed( model, kSolutions );
        const BufferId inspector = seed( model, kInspector );

        LayoutTree tree( catalogue );
        const TileId catalogueTile = tree.focused();

        tree.splitFocused( Split::Rows, transcript, 0.68 );
        const TileId transcriptTile = tree.focused();
        tree.splitFocused( Split::Columns, solutions, 0.55 );
        tree.splitFocused( Split::Columns, inspector, 0.55 );

        tree.focus( catalogueTile );
        tree.splitFocused( Split::Columns, source, 0.24 );
        tree.splitFocused( Split::Rows, diagnostics, 0.74 );

        tree.focus( transcriptTile );
        model.layout() = tree;
        return true;
    }

    return false;
}

} // namespace lens
