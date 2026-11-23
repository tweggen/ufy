/**
 * @file vault-unify-session-value.cpp
 *
 * Implementation of the shared value-tree helpers. See the header for the
 * truncation rules and why they live in one place.
 */

#include "vault-unify-session-value.hpp"

#include <deque>
#include <sstream>

namespace vault {
namespace unify {
namespace session {

namespace {

/** Children of a node, in the order the budget spends on them. */
std::size_t childCount( const Value& v )
{
    switch ( v.kind ) {
    case Value::Kind::Cons:
    case Value::Kind::Array:
        return v.args.size();
    case Value::Kind::Map:
        return v.pairs.size();
    default:
        return 0;
    }
}

const Value& childAt( const Value& v, std::size_t index )
{
    if ( v.kind == Value::Kind::Map ) {
        return v.pairs[ index ].second;
    }
    return v.args[ index ];
}

/** Shallow copy: the node itself, with no children attached. */
Value shallow( const Value& v, const ValueBudget& budget )
{
    Value out;
    out.kind = v.kind;
    out.i = v.i;
    out.f = v.f;
    out.truncated = v.truncated;

    out.name = v.name;
    if ( out.name.size() > budget.maxStringBytes ) {
        out.name.resize( budget.maxStringBytes );
        out.truncated = true;
    }
    return out;
}

/**
 * One entry of the breadth-first frontier: where to read from and where the
 * copy should be attached.
 */
struct Pending {
    const Value* source;
    Value*       target;
    std::uint32_t depth;
};

} // namespace

Value applyBudget( const Value& value, const ValueBudget& budget )
{
    Value root = shallow( value, budget );

    /*
     * maxNodes counts the root, so a budget of 0 or 1 yields the root alone.
     * Spending it breadth-first is what keeps a wide term recognisable; see
     * the header for why that matters more than it looks.
     */
    std::uint64_t spent = 1;

    std::deque<Pending> frontier;
    frontier.push_back( Pending{ &value, &root, 0 } );

    while ( !frontier.empty() ) {
        const Pending job = frontier.front();
        frontier.pop_front();

        const Value& src = *job.source;
        Value&       dst = *job.target;

        const std::size_t n = childCount( src );
        if ( n == 0 ) {
            continue;
        }

        if ( job.depth >= budget.maxDepth ) {
            dst.truncated = true;
            continue;
        }

        /* How many children fit in what is left of the node budget. */
        std::size_t fit = n;
        if ( spent + n > budget.maxNodes ) {
            fit = ( budget.maxNodes > spent ) ? ( budget.maxNodes - spent ) : 0;
        }
        if ( fit < n ) {
            dst.truncated = true;
        }

        if ( src.kind == Value::Kind::Map ) {
            dst.pairs.reserve( fit );
            for ( std::size_t i = 0; i < fit; ++i ) {
                dst.pairs.emplace_back( src.pairs[ i ].first,
                                        shallow( src.pairs[ i ].second, budget ) );
            }
        } else {
            dst.args.reserve( fit );
            for ( std::size_t i = 0; i < fit; ++i ) {
                dst.args.push_back( shallow( childAt( src, i ), budget ) );
            }
        }
        spent += fit;

        /*
         * Queue the children only after the whole level is copied, so the
         * pointers into dst.args / dst.pairs are not invalidated by a
         * later push_back into the same vector.
         */
        for ( std::size_t i = 0; i < fit; ++i ) {
            Value& childTarget = ( src.kind == Value::Kind::Map )
                                     ? dst.pairs[ i ].second
                                     : dst.args[ i ];
            frontier.push_back(
                Pending{ &childAt( src, i ), &childTarget, job.depth + 1 } );
        }
    }

    return root;
}

std::optional<Value> valueAtPath( const Value& value, const ValuePath& path )
{
    const Value* cursor = &value;
    for ( const std::uint32_t index : path ) {
        if ( index >= childCount( *cursor ) ) {
            return std::nullopt;
        }
        cursor = &childAt( *cursor, index );
    }
    return *cursor;
}

std::uint64_t countNodes( const Value& value )
{
    std::uint64_t total = 1;
    const std::size_t n = childCount( value );
    for ( std::size_t i = 0; i < n; ++i ) {
        total += countNodes( childAt( value, i ) );
    }
    return total;
}

bool hasTruncation( const Value& value )
{
    if ( value.truncated ) {
        return true;
    }
    const std::size_t n = childCount( value );
    for ( std::size_t i = 0; i < n; ++i ) {
        if ( hasTruncation( childAt( value, i ) ) ) {
            return true;
        }
    }
    return false;
}

std::string toDisplayString( const Value& value )
{
    std::ostringstream os;

    switch ( value.kind ) {
    case Value::Kind::Atom:
    case Value::Kind::Var:
        os << value.name;
        break;
    case Value::Kind::Int:
        os << value.i;
        break;
    case Value::Kind::Float:
        os << value.f;
        break;
    case Value::Kind::Str:
        os << '"' << value.name << '"';
        break;
    case Value::Kind::Cons:
        os << value.name;
        if ( !value.args.empty() || value.truncated ) {
            os << "( ";
            for ( std::size_t i = 0; i < value.args.size(); ++i ) {
                if ( i ) { os << ", "; }
                os << toDisplayString( value.args[ i ] );
            }
            if ( value.truncated ) {
                os << ( value.args.empty() ? "..." : ", ..." );
            }
            os << " )";
        }
        break;
    case Value::Kind::Array:
        os << "[ ";
        for ( std::size_t i = 0; i < value.args.size(); ++i ) {
            if ( i ) { os << ", "; }
            os << toDisplayString( value.args[ i ] );
        }
        if ( value.truncated ) {
            os << ( value.args.empty() ? "..." : ", ..." );
        }
        os << " ]";
        break;
    case Value::Kind::Map:
        os << "{ ";
        for ( std::size_t i = 0; i < value.pairs.size(); ++i ) {
            if ( i ) { os << ", "; }
            os << value.pairs[ i ].first << ": "
               << toDisplayString( value.pairs[ i ].second );
        }
        if ( value.truncated ) {
            os << ( value.pairs.empty() ? "..." : ", ..." );
        }
        os << " }";
        break;
    }

    /* A truncated leaf still has to say so; compounds said it above. */
    if ( value.truncated
         && value.kind != Value::Kind::Cons
         && value.kind != Value::Kind::Array
         && value.kind != Value::Kind::Map ) {
        os << "...";
    }

    return os.str();
}

} // namespace session
} // namespace unify
} // namespace vault
