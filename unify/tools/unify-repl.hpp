/**
 * @file unify-repl.hpp
 *
 * @author Timo Weggen
 *
 * The interactive read-eval-print loop `unify-run` drops into when it is
 * given no program to run (or `-i`) -- ROADMAP.md Phase 4 ("REPL ... with
 * query, assert, and inspection"). See unify-repl.cpp for the loop itself
 * and for what the prompt accepts.
 */

#if !defined( _UNIFY_REPL_HPP )
#define _UNIFY_REPL_HPP

#include <vault-unify.hpp>


namespace unifytool {


/**
 * Run the REPL against `rt` until end of input or `:quit`, reading from
 * stdin and writing to stdout.
 *
 * `rt` must already be set up (RuntimeContext::setupDone()) and may
 * already hold clauses -- `unify-run -i program.ufy` runs the program
 * first and then hands the very same, fully populated RuntimeContext to
 * this function, so the session starts with everything the program
 * defined already in the database.
 *
 * Returns the number of errors -- parse errors plus internal unification
 * errors -- encountered over the whole session. Every one of them has
 * already been reported to stderr as it happened; the count exists purely
 * so main() can turn it into unify-run's exit code.
 */
int runRepl( vault::unify::RuntimeContext& rt );


} // namespace unifytool

#endif
