#if !defined( _LENS_MODEL_RESULTS_HPP )
#define _LENS_MODEL_RESULTS_HPP

/**
 * @file results.hpp
 *
 * What the current query produced, and the three panels that look at it --
 * gate G5, "Inspect". UI.md section 3:
 *
 *   Solutions    the current query's bindings as a table, one row per
 *                solution; scrolling past the last row issues `demand`.
 *   Inspector    the selected binding as an expandable term tree, truncated
 *                nodes marked and expandable on request.
 *   Diagnostics  Diagnostic events as a navigable list; Enter moves the
 *                source panel to file/line/column.
 *
 * The values are held ONCE, here, rather than in each panel. The table and
 * the tree are two views of the same solutions, and two copies would be two
 * answers to "what did the query return" the moment an expansion was grafted
 * into one of them and not the other.
 *
 * Like transcript.hpp this sees `vault-unify-session.hpp` and nothing else of
 * the engine, so every line of it is testable with no engine present.
 */

#include "vault-unify-session.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace lens {

namespace us = vault::unify::session;

/** The query the Solutions and Inspector panels are showing. */
struct ResultsState {
    us::QueryId query = us::kNoQuery;

    /** As delivered, in index order -- with any expansions grafted in. */
    std::vector<us::Solution> solutions;

    /** Binding names in the order first seen: the table's columns. */
    std::vector<std::string> columns;

    /** The last status said more solutions are waiting for a `demand`. */
    bool more = false;

    /**
     * A `demand` has been asked for and nothing has answered it yet.
     *
     * G5.1's "exactly one demand, not one per keystroke". Holding Down at
     * the last row is the natural way to ask for more, and every press of
     * it would otherwise be another batch requested from an engine that has
     * not delivered the first.
     */
    bool demandPending = false;

    /** The query reached a terminal status. */
    bool finished = false;

    /** The user cancelled it; late rows are dropped, as in the transcript. */
    bool detached = false;

    /** The selected solution: the Solutions cursor, the Inspector's root. */
    int selected = 0;

    /** The Solutions panel's first drawn row; see HelpState::top. */
    int top = 0;
};

/**
 * How many rows of a Solutions tile list solutions: all but the header row
 * of column names and the footer that says whether more are coming. One
 * definition for fold, the scroll projection and the renderer, as
 * transcriptHistoryRows() is.
 */
inline int solutionsListRows( int innerHeight )
{
    return innerHeight > 2 ? innerHeight - 2 : 0;
}

/** One `inspect` the model asked for and the core has not answered. */
struct PendingInspect {
    us::QueryId   query = us::kNoQuery;
    std::uint64_t solution = 0;
    us::ValuePath path;
};

/** The Inspector's view of the selected solution. */
struct InspectorState {
    /**
     * Expanded nodes, by path. Path element 0 is the binding's index in the
     * solution, the rest index into the value -- the same addressing
     * Session::inspect() takes, so a row's path IS its inspect request and
     * nothing has to translate between the two.
     */
    std::set<us::ValuePath> open;

    /** Expansions in flight, so a second Enter does not ask twice. */
    std::vector<PendingInspect> pending;

    /** Which solution `open` describes; changing it starts a fresh tree. */
    us::QueryId   forQuery = us::kNoQuery;
    std::uint64_t forSolution = 0;

    int cursor = 0;
    int top = 0;
};

/** One line of the Inspector. */
struct InspectorRow {
    us::ValuePath path;
    int  depth = 0;
    std::string text;         //!< without the indentation and the marker
    bool expandable = false;  //!< has children, or had them cut
    bool open = false;
    bool truncated = false;   //!< the budget cut this node's children
    bool fetching = false;    //!< an inspect for it is in flight
};

/**
 * The rows the Inspector shows for `solution`, in order.
 *
 * Pure, and the single definition used by fold, by the scroll projection and
 * by the renderer -- see transcriptHistoryRows() for why three callers must
 * not each count rows their own way.
 */
std::vector<InspectorRow> inspectorRows( const us::Solution& solution,
                                         const InspectorState& inspector );

/** The node at `path` in `solution`, or null if the path leaves it. */
us::Value* valueAt( us::Solution& solution, const us::ValuePath& path );

/** A diagnostic as received, and the list of them. */
struct DiagnosticsState {
    std::vector<us::Diagnostic> items;
    int selected = 0;
    int top = 0;
};

/**
 * Where the Source panel has been sent, and what it can show of it.
 *
 * A Diagnostic carries its own source line because re-reading the file is
 * not possible for a transcript line or a remote core (SESSION-API section
 * 4), so that line is what is shown. The whole definition is the Source
 * panel's proper job and arrives with gate G3's `source` request; until then
 * the panel says so rather than pretending the one line is the file.
 */
struct SourceLocation {
    bool          set = false;
    std::string   file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string   text;
    std::string   message;
};

/** `file:line:column: message`, as the transcript and the list show it. */
std::string diagnosticHeadline( const us::Diagnostic& diagnostic );

} // namespace lens

#endif // _LENS_MODEL_RESULTS_HPP
