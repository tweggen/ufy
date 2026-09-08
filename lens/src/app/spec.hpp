#if !defined( _LENS_APP_SPEC_HPP )
#define _LENS_APP_SPEC_HPP

/**
 * @file spec.hpp
 *
 * `unify-lens --spec FILE.ufy` -- interaction test cases written in Unify.
 *
 * WHY THE SPEC IS A UNIFY PROGRAM. lens is the front end for a logic engine
 * and ships with one linked in. A test case here is a sequence and a set of
 * things that must hold after each step -- which is data, and stating data
 * in the language the product exists to run is the one form of dogfooding
 * that costs nothing and proves something. It also means a user can write a
 * case for a bug they hit, in a language the project already documents, and
 * send the file rather than a description.
 *
 * WHAT IT DELIBERATELY DOES NOT COVER. Only what the model can observe. The
 * terminal-level suite (test/pty-interaction-test.cpp) stays in C++ because
 * its subject is bytes on a pseudo-terminal, which the engine has no way to
 * see, and because a spec runner that needed the terminal to work could not
 * report that the terminal does not work. There is a bootstrapping rule
 * behind that: nothing whose failure would break the engine may be specified
 * in the engine. Interaction cases qualify -- they exercise the model, not
 * the engine -- so if the engine breaks, these fail loudly rather than
 * quietly passing.
 *
 * THE VOCABULARY, which is closed on purpose -- an expectation this does not
 * recognise is an error, never a silent pass:
 *
 *   case( Name, Steps )
 *   case( Name, Options, Steps )
 *       Options: layout( browse | run | debug | full ), geometry( W, H )
 *
 *   Steps: a list of
 *       step( Keys, Expectations )            one key sequence
 *       repeat( N, Keys, Expectations )       N times, checked after EACH
 *
 *   Keys: a string in the keymap's own grammar -- "Down", "C-x 2", "M-x".
 *
 *   Expectations, checked after the step:
 *       panel( Name )            the focused panel
 *       cursor( N )  top( N )  row( N )  rows( N )  lines( N )
 *       tiles( N )
 *       visible                  the cursor is on screen
 *       moved( What, Dir, N )    What: cursor | top | row | tiles
 *                                Dir:  up | down   (never a negative
 *                                literal: see the note in the spec file)
 *       still( What )            unchanged since the previous step
 *       message( Text )          the status line contains Text
 */

#include <iosfwd>
#include <string>

namespace lens {

/**
 * Run every case in `path`.
 *
 * Returns 0 if every case passed, 1 if any failed, and 2 for a spec that
 * could not be run at all -- unreadable, unparsable, or containing no cases.
 * The last of those is a failure and not a pass: a suite that silently runs
 * nothing is worse than one that is red.
 */
int runSpec( const std::string& path, std::ostream& out );

} // namespace lens

#endif // _LENS_APP_SPEC_HPP
