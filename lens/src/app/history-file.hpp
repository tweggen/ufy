#if !defined( _LENS_APP_HISTORY_FILE_HPP )
#define _LENS_APP_HISTORY_FILE_HPP

/**
 * @file history-file.hpp
 *
 * `$HOME/.unify_history` on disk -- gate G2.8.
 *
 * The format is the model's business (parseHistory/formatHistory in
 * model/transcript.hpp, pure and tested without a file system); this is only
 * the reading and writing, which is I/O and therefore belongs in `app/`.
 */

#include <string>
#include <vector>

namespace lens {

/**
 * The default path: `$HOME/.unify_history`, exactly where `unify-run -i`
 * keeps it (unify-repl.cpp), or empty when HOME is unset -- which disables
 * persistence, as it does there.
 */
std::string defaultHistoryPath();

/**
 * Read the file. A missing file is not an error -- it is a first run -- and
 * leaves `out_lines` empty. Returns false, with a reason, on anything else.
 */
bool loadHistoryFile( const std::string& path,
                      std::vector<std::string>& out_lines,
                      std::string& out_error );

/**
 * Replace the file with `lines` (the newest kHistoryLimit of them).
 *
 * Written to a temporary beside it and renamed over it, so lens killed
 * halfway through a save leaves the previous history rather than half of
 * one. The file is shared with unify-run; truncating it would cost the user
 * the other program's history too.
 */
bool saveHistoryFile( const std::string& path,
                      const std::vector<std::string>& lines,
                      std::string& out_error );

} // namespace lens

#endif // _LENS_APP_HISTORY_FILE_HPP
