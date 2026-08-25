/**
 * @file unify-tool-support.hpp
 *
 * @author Timo Weggen
 *
 * Helpers shared by the two front ends `unify-run` puts on the engine:
 * the batch file runner (unify-run.cpp) and the interactive REPL
 * (unify-repl.cpp). Both have to load a source file and, above all, both
 * have to know when the engine has finished everything they asked it to do
 * -- see waitForEngineIdle() for why that is not a one-liner.
 *
 * This is deliberately a tools/-private header: none of it is part of the
 * engine's public API (include/vault-unify.hpp), it just packages the two
 * bits of glue an embedding CLI needs today.
 */

#if !defined( _UNIFY_TOOL_SUPPORT_HPP )
#define _UNIFY_TOOL_SUPPORT_HPP

#include <string>

#include <vault-unify.hpp>


namespace unifytool {


/**
 * Block until every job currently queued on `rt`'s engine has run to
 * completion.
 *
 * There is no "wait until idle" call on Engine (ROADMAP.md notes there is
 * no engine shutdown path yet either), so instead this exploits two
 * properties of the current scheduler:
 *
 *  - Engine::executionLoop() drains m_lsReadyJobs strictly FIFO from a
 *    single worker thread (there is exactly one; RuntimeContext::
 *    setupDone() adds only one).
 *  - Under the default (non-debugging) REGULAR target state, a SolveJob
 *    runs its performSlice() to completion in a single call -- it never
 *    re-queues itself as READY (see SolveJob::performSlice() in
 *    vault-unify-solvejob.cpp; only debugger-halt states do that).
 *
 * So one more, trivial SolveJob -- an empty Goal, which "solves"
 * immediately -- queued after the real ones is guaranteed to run only once
 * every job queued before it has already run to completion. This waits for
 * that barrier job instead of the real ones.
 *
 * The REPL calls this once per submitted input, which is what makes an
 * interactive session sequential: nothing is printed to the next prompt's
 * left while a query is still producing output.
 */
void waitForEngineIdle( vault::unify::RuntimeContext& rt );


/**
 * Read `pPath` wholly into `out_content`. Returns true on success; on
 * failure returns false and leaves `out_content` untouched (the caller
 * reports the error -- it knows whether that is a fatal usage error or,
 * in the REPL, just a bad `:load`).
 */
bool readWholeFile( const char* pPath, std::string& out_content );


} // namespace unifytool

#endif
