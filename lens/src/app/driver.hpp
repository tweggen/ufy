#if !defined( _LENS_APP_DRIVER_HPP )
#define _LENS_APP_DRIVER_HPP

/**
 * @file driver.hpp
 *
 * A model, a stream of keys, and one observation per key.
 *
 * This is the apparatus the interaction tests were built on, moved out of
 * the test file so that a second caller could not grow a second, subtly
 * different idea of what "the cursor is on line 3" means. There are now two
 * callers: test/interaction-test.cpp, which asserts in C++, and the `--spec`
 * runner, which asserts against expectations written in Unify. They must
 * observe the same model in the same way or a spec case and a C++ case with
 * the same words in them could disagree, and nobody would know which was
 * right.
 *
 * No terminal, no engine, no subprocess. The point of driving the model
 * directly is that a thousand key sequences cost milliseconds -- an
 * interaction bug that needs a pty to reproduce is an interaction bug nobody
 * will write a test for. (One class of bug does need the pty; that is what
 * test/pty-interaction-test.cpp is for, and it is three cases, not a
 * thousand.)
 */

#include "../model/model.hpp"
#include "../model/scroll.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace lens {

class Driver {
public:
    /** What a user would perceive after one key. */
    struct Step {
        std::string key;
        ScrollView  scroll;
        bool        scrollValid = false;
        std::size_t tiles = 0;
        std::string focusedPanel;
        std::string message;
    };

    explicit Driver( const std::string& layout = "browse",
                     int width = 120, int height = 40 );

    Model& model() { return m_model; }
    const Model& model() const { return m_model; }

    /**
     * Feed one key sequence, recording an observation per KEY.
     *
     * Per key rather than per sequence on purpose: `C-x 2` is two keys and
     * the state between them -- the pending chord -- is a state a user can
     * see and get stuck in.
     *
     * Returns false if `keys` is not a key sequence at all, which is a bug
     * in the caller's test rather than in lens; the caller says so in its
     * own vocabulary.
     */
    bool press( const std::string& keys );

    /** Feed one literal character, bypassing the key-sequence grammar. */
    void pressChar( char32_t ch );

    const std::vector<Step>& steps() const { return m_steps; }
    const Step& last() const { return m_steps.back(); }
    const Step& previous() const { return m_steps[ m_steps.size() - 2 ]; }

    /** A readable film of the run, for a failure message. */
    std::string trace( std::size_t tail = 8 ) const;

private:
    void record( const std::string& key );

    Model m_model;
    std::vector<Step> m_steps;
};

} // namespace lens

#endif // _LENS_APP_DRIVER_HPP
