/**
 * @file results.cpp
 */

#include "results.hpp"
#include "transcript.hpp"

#include <algorithm>
#include <sstream>

namespace lens {

namespace {

const char* const kCut = "…";

/** Cons, Array and Map have children; so does anything the budget cut. */
bool hasChildren( const us::Value& v )
{
    switch( v.kind ) {
    case us::Value::Kind::Cons:
        /*
         * A Cons with no arguments is how the engine spells an atom-like
         * constant, and has nothing to open -- UNLESS it was cut, in which
         * case the missing arguments are exactly what the user wants to see.
         * The transcript's renderValue() makes the same distinction for the
         * same reason: `g` and `g( ... )` are different terms.
         */
        return !v.args.empty() || v.truncated;
    case us::Value::Kind::Array:
        return !v.args.empty() || v.truncated;
    case us::Value::Kind::Map:
        return !v.pairs.empty() || v.truncated;
    default:
        return v.truncated;
    }
}

std::size_t childCount( const us::Value& v )
{
    return v.kind == us::Value::Kind::Map ? v.pairs.size() : v.args.size();
}

/** What an OPEN node says about itself; its children say the rest. */
std::string openText( const us::Value& v )
{
    std::ostringstream os;
    switch( v.kind ) {
    case us::Value::Kind::Cons:
        os << v.name << "/" << v.args.size();
        break;
    case us::Value::Kind::Array:
        os << "array of " << v.args.size();
        break;
    case us::Value::Kind::Map:
        os << "map of " << v.pairs.size();
        break;
    default:
        os << renderValue( v );
        break;
    }
    /*
     * An open node that was cut says so on its own row too. Its children are
     * the ones that survived, and a reader counting them against `f/2`
     * would otherwise take two shown arguments for the whole term.
     */
    if( v.truncated ) {
        os << " " << kCut << " more not fetched";
    }
    return os.str();
}

bool isPending( const InspectorState& inspector, const us::ValuePath& path )
{
    for( std::size_t i = 0; i < inspector.pending.size(); ++i ) {
        if( inspector.pending[i].query == inspector.forQuery
            && inspector.pending[i].solution == inspector.forSolution
            && inspector.pending[i].path == path ) {
            return true;
        }
    }
    return false;
}

void addRows( std::vector<InspectorRow>& rows, const us::Value& value,
              const us::ValuePath& path, int depth, const std::string& label,
              const InspectorState& inspector )
{
    InspectorRow row;
    row.path = path;
    row.depth = depth;
    row.expandable = hasChildren( value );
    row.open = row.expandable && inspector.open.count( path ) > 0;
    row.truncated = value.truncated;
    row.fetching = isPending( inspector, path );
    row.text = label + ( row.open ? openText( value ) : renderValue( value ) );
    rows.push_back( row );

    if( !row.open ) {
        return;
    }
    const std::size_t n = childCount( value );
    for( std::size_t i = 0; i < n; ++i ) {
        us::ValuePath child = path;
        child.push_back( (std::uint32_t) i );
        if( value.kind == us::Value::Kind::Map ) {
            addRows( rows, value.pairs[i].second, child, depth + 1,
                     value.pairs[i].first + ": ", inspector );
        } else {
            addRows( rows, value.args[i], child, depth + 1, "", inspector );
        }
    }
}

} // namespace


std::vector<InspectorRow> inspectorRows( const us::Solution& solution,
                                         const InspectorState& inspector )
{
    std::vector<InspectorRow> rows;
    for( std::size_t b = 0; b < solution.bindings.size(); ++b ) {
        addRows( rows, solution.bindings[b].second,
                 us::ValuePath( 1, (std::uint32_t) b ), 0,
                 solution.bindings[b].first + " = ", inspector );
    }
    return rows;
}


us::Value* valueAt( us::Solution& solution, const us::ValuePath& path )
{
    if( path.empty() || path[0] >= solution.bindings.size() ) {
        return NULL;
    }
    us::Value* node = &solution.bindings[ path[0] ].second;
    for( std::size_t i = 1; i < path.size(); ++i ) {
        const std::uint32_t k = path[i];
        if( node->kind == us::Value::Kind::Map ) {
            if( k >= node->pairs.size() ) { return NULL; }
            node = &node->pairs[k].second;
        } else {
            if( k >= node->args.size() ) { return NULL; }
            node = &node->args[k];
        }
    }
    return node;
}


std::string diagnosticHeadline( const us::Diagnostic& diagnostic )
{
    /* The transcript's own first line, so the two cannot spell it apart. */
    return renderDiagnostic( diagnostic ).front().text;
}

} // namespace lens
