/**
 * @file history-file.cpp
 */

#include "history-file.hpp"

#include "../model/transcript.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lens {

std::string defaultHistoryPath()
{
    const char* home = std::getenv( "HOME" );
    if( !home || !*home ) {
        return std::string();
    }
    return std::string( home ) + "/.unify_history";
}


bool loadHistoryFile( const std::string& path,
                      std::vector<std::string>& out_lines,
                      std::string& out_error )
{
    out_lines.clear();

    std::FILE* file = std::fopen( path.c_str(), "rb" );
    if( !file ) {
        if( errno == ENOENT ) {
            return true;   /* a first run, as read_history() treats it */
        }
        out_error = "cannot read " + path + ": " + std::strerror( errno );
        return false;
    }

    std::string contents;
    char buffer[ 4096 ];
    for( ;; ) {
        const std::size_t got = std::fread( buffer, 1, sizeof( buffer ), file );
        contents.append( buffer, got );
        if( got < sizeof( buffer ) ) {
            break;
        }
    }
    const bool failed = std::ferror( file ) != 0;
    std::fclose( file );

    if( failed ) {
        out_error = "error reading " + path;
        return false;
    }
    out_lines = parseHistory( contents );
    return true;
}


bool saveHistoryFile( const std::string& path,
                      const std::vector<std::string>& lines,
                      std::string& out_error )
{
    const std::string temporary = path + ".lens-tmp";
    const std::string text = formatHistory( lines );

    std::FILE* file = std::fopen( temporary.c_str(), "wb" );
    if( !file ) {
        out_error = "cannot write " + temporary + ": " + std::strerror( errno );
        return false;
    }
    const std::size_t written = std::fwrite( text.data(), 1, text.size(), file );
    const bool closeFailed = std::fclose( file ) != 0;
    if( written != text.size() || closeFailed ) {
        out_error = "writing " + temporary + " failed";
        std::remove( temporary.c_str() );
        return false;
    }

    if( 0 != std::rename( temporary.c_str(), path.c_str() ) ) {
        /*
         * Windows will not rename over an existing file. Removing first
         * opens a window in which the history is gone, so it is only the
         * fallback, never the first thing tried.
         */
        std::remove( path.c_str() );
        if( 0 != std::rename( temporary.c_str(), path.c_str() ) ) {
            out_error = "cannot replace " + path + ": " + std::strerror( errno );
            std::remove( temporary.c_str() );
            return false;
        }
    }
    return true;
}

} // namespace lens
