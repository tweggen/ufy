#if !defined( _UNIFY_TEST_HARNESS_HPP )
#define _UNIFY_TEST_HARNESS_HPP

/**
 * @file test-harness.hpp
 *
 * A deliberately tiny test harness for the session contract suite.
 *
 * Why not a framework: the engine's only dependencies are Boost and
 * Threads (unify/CMakeLists.txt says so in as many words), and the golden
 * suite is plain shell. Adding GoogleTest to run about eighty assertions
 * would be the largest new dependency in the repository, introduced for the
 * least demanding consumer of it. This header is what the suite actually
 * needs: named cases, an assertion that reports file and line, and a runner
 * that keeps going after a failure so one run tells you everything that is
 * broken rather than only the first thing.
 *
 * Output format is deliberately close to the golden runner's, so a CI log
 * reads the same way whichever suite produced it.
 */

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace unify_test {

/** Thrown by CHECK-style macros; caught by the runner, never by a test. */
class AssertionFailure : public std::exception {
public:
    explicit AssertionFailure( std::string what ) : m_what( std::move( what ) ) {}
    const char* what() const noexcept override { return m_what.c_str(); }
private:
    std::string m_what;
};

struct TestCase {
    std::string           name;
    std::function<void()> body;
};

/**
 * The registry.
 *
 * Cases are added by name at runtime rather than by a static-initialiser
 * macro, because the contract suite runs the SAME cases against several
 * Session implementations and a static registry would make that awkward:
 * the suite is a function that takes a factory and registers its cases
 * against it.
 */
class Registry {
public:
    void add( std::string name, std::function<void()> body )
    {
        m_cases.push_back( TestCase{ std::move( name ), std::move( body ) } );
    }

    /** Runs everything; returns the number of failures. */
    int run( const std::string& banner )
    {
        int failures = 0;
        int skipped = 0;

        std::cout << "== " << banner << " (" << m_cases.size() << " cases)\n";

        for ( const TestCase& c : m_cases ) {
            std::string failure;
            try {
                c.body();
            }
            catch ( const AssertionFailure& e ) {
                failure = e.what();
            }
            catch ( const std::exception& e ) {
                failure = std::string( "unexpected exception: " ) + e.what();
            }
            catch ( ... ) {
                failure = "unexpected non-std exception";
            }

            if ( failure.empty() ) {
                std::cout << "   ok   " << c.name << "\n";
            } else {
                ++failures;
                std::cout << "   FAIL " << c.name << "\n";
                std::cout << failure << "\n";
            }
        }

        std::cout << "== " << banner << ": "
                  << ( m_cases.size() - failures - skipped ) << " passed, "
                  << failures << " failed\n";
        return failures;
    }

    std::size_t size() const { return m_cases.size(); }

private:
    std::vector<TestCase> m_cases;
};

/** Formats a value for a failure message; specialised where useful. */
template <typename T>
std::string describe( const T& value )
{
    std::ostringstream os;
    os << value;
    return os.str();
}

inline std::string describe( bool value ) { return value ? "true" : "false"; }

inline std::string describe( const std::string& value )
{
    return "\"" + value + "\"";
}

} // namespace unify_test

/*
 * The macros. They throw rather than return, so a failing precondition
 * cannot let the rest of a case run against nonsense state and produce a
 * second, misleading failure.
 */

#define UT_FAIL( message )                                                    \
    do {                                                                      \
        std::ostringstream _ut_os;                                            \
        _ut_os << "        at " << __FILE__ << ":" << __LINE__ << "\n"        \
               << "        " << message;                                      \
        throw ::unify_test::AssertionFailure( _ut_os.str() );                 \
    } while ( false )

#define UT_CHECK( expression )                                                \
    do {                                                                      \
        if ( !( expression ) ) {                                              \
            UT_FAIL( "expected: " #expression );                              \
        }                                                                     \
    } while ( false )

#define UT_CHECK_MSG( expression, message )                                   \
    do {                                                                      \
        if ( !( expression ) ) {                                              \
            UT_FAIL( message << "\n        expected: " #expression );         \
        }                                                                     \
    } while ( false )

#define UT_CHECK_EQ( actual, expected )                                       \
    do {                                                                      \
        const auto& _ut_a = ( actual );                                       \
        const auto& _ut_e = ( expected );                                     \
        if ( !( _ut_a == _ut_e ) ) {                                          \
            UT_FAIL( "expected " #actual " == " #expected                     \
                     << "\n          actual:   "                              \
                     << ::unify_test::describe( _ut_a )                       \
                     << "\n          expected: "                              \
                     << ::unify_test::describe( _ut_e ) );                    \
        }                                                                     \
    } while ( false )

#endif // _UNIFY_TEST_HARNESS_HPP
