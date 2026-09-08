/**
 * @file term-value-test.cpp
 *
 * Scaffold. The case that belongs here is being written; see
 * plans/todo/lens/E7-STRUCTURED-VALUES.md.
 */

#include "../session/test-harness.hpp"

int main()
{
    unify_test::Registry registry;

    registry.add( "scaffold: replaced by the E7 case for this file", []() {
        UT_SKIP( "not written yet" );
    } );

    return registry.run( "term-value (E7, scaffold)" ) == 0 ? 0 : 1;
}
