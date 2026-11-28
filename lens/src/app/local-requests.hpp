#if !defined( _LENS_APP_LOCAL_REQUESTS_HPP )
#define _LENS_APP_LOCAL_REQUESTS_HPP

/**
 * @file local-requests.hpp
 *
 * The requests lens performs itself rather than sending to the core.
 *
 * `fold` stays free of I/O by RETURNING what it wants done (ARCHITECTURE.md
 * section 4), and most of what it wants is the engine's business, which is
 * what SessionBridge is for. A few things are not: writing the transcript
 * to a file is lens's own I/O on lens's own machine, and routing it through
 * the session would make it fail whenever there is no engine -- and, once
 * G7 puts the engine on another host, write the file THERE.
 *
 * So both loops offer each request here first, and pass on what this does
 * not claim.
 */

#include "../model/model.hpp"

namespace lens {

/**
 * Perform `request` if it is one of lens's own, reporting the outcome in
 * the status line. Returns false for a request the session should get.
 */
bool performLocalRequest( Model& model, const CommandRequest& request );

} // namespace lens

#endif // _LENS_APP_LOCAL_REQUESTS_HPP
