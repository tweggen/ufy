/*
 * Gate G0.1 -- the compile-isolation translation unit.
 *
 * This file includes vault-unify-session.hpp and NOTHING ELSE of ours. If
 * the boundary header ever acquires a dependency on an engine header, a
 * Boost header, or anything else from this project, this target stops
 * compiling and the build fails.
 *
 * It exists because "the abstraction is not leaky" is exactly the kind of
 * claim that is true on the day it is written and quietly false three
 * months later. A grep (G0.2, check-header-hygiene.sh) catches the type
 * names; only a compile catches an #include.
 *
 * Deliberately built with an include path that contains ONLY the directory
 * holding the boundary header -- see test/session/CMakeLists.txt. So even
 * `#include "vault-unify.hpp"` from inside the boundary header would fail
 * to resolve here rather than succeeding by accident.
 */

#include "vault-unify-session.hpp"

namespace {

using namespace vault::unify::session;

/*
 * Touch enough of the surface that the types must actually be complete,
 * not merely declared. A header can parse and still be unusable.
 */
Value makeValue()
{
    Value v;
    v.kind = Value::Kind::Cons;
    v.name = "point";
    v.args.resize( 2 );
    v.args[ 0 ].kind = Value::Kind::Int;
    v.args[ 0 ].i = 3;
    v.args[ 1 ].kind = Value::Kind::Var;
    v.args[ 1 ].name = "$y";
    v.truncated = true;
    return v;
}

Event makeEvent()
{
    Solution s;
    s.index = 0;
    s.bindings.emplace_back( "$p", makeValue() );

    Event e;
    e.header.seq = 1;
    e.header.worldGeneration = 7;
    e.header.query = QueryId( 42 );
    e.header.querySeq = 1;
    e.body = s;
    return e;
}

/* Every event body must be a member of the variant. */
bool visitAll( const Event& e )
{
    return std::visit( []( const auto& ) { return true; }, e.body );
}

/* Every debug command must be a member of its variant. */
DebugCommand makeCommand()
{
    return SetTrace{ QueryId( 1 ), true };
}

/*
 * An abstract Session must be implementable from this header alone. If a
 * pure virtual is added and this is not updated, the build breaks -- which
 * is the point: the interface cannot grow silently.
 */
class NullSession : public Session {
public:
    Capabilities describe() const override { return Capabilities(); }
    void close() override {}
    void subscribe( EventSink&, Seq ) override {}

    RequestId define( std::string, Origin, OverwritePolicy ) override { return 0; }
    RequestId undefine( PredicateKey, ModuleId ) override { return 0; }
    RequestId listing( ListingFilter ) override { return 0; }
    RequestId source( PredicateKey ) override { return 0; }

    QueryId   solve( std::string, QueryOptions ) override { return 0; }
    RequestId demand( QueryId, Stream, std::uint32_t ) override { return 0; }
    RequestId cancel( QueryId ) override { return 0; }
    RequestId release( QueryId ) override { return 0; }
    RequestId inspect( QueryId, std::uint64_t, ValuePath, ValueBudget ) override { return 0; }

    RequestId save( std::string, SaveOptions ) override { return 0; }
    RequestId load( std::string ) override { return 0; }
    RequestId insert( std::string, OverwritePolicy ) override { return 0; }

    RequestId debug( DebugCommand ) override { return 0; }
};

class NullSink : public EventSink {
public:
    void onEvent( const Event& ) override {}
};

} // namespace

int main()
{
    NullSession session;
    NullSink sink;
    session.subscribe( sink, 0 );

    const Event e = makeEvent();
    if ( !visitAll( e ) ) {
        return 1;
    }
    ( void ) makeCommand();

    /* Exercised for real by the contract suite; here we only need it to link. */
    return e.header.seq == 1 ? 0 : 1;
}
