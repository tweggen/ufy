/**
 * @file local-requests.cpp
 */

#include "local-requests.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace lens {

namespace {

/**
 * `write-file`: the text, byte for byte, to the path -- G2.9.
 *
 * Binary mode, so the file holds exactly the transcript's bytes on every
 * platform; a transcript written on Windows and diffed on Linux should not
 * differ in every line.
 */
void writeFile( Model& model, const CommandRequest& request )
{
    std::FILE* file = std::fopen( request.path.c_str(), "wb" );
    if( !file ) {
        model.setMessage( "cannot write " + request.path + ": "
                          + std::strerror( errno ) );
        return;
    }

    const std::size_t written =
        std::fwrite( request.text.data(), 1, request.text.size(), file );
    const bool failed = written != request.text.size();
    const int closeError = std::fclose( file );

    if( failed || closeError != 0 ) {
        model.setMessage( "writing " + request.path + " failed: "
                          + std::strerror( errno ) );
        return;
    }

    std::size_t lines = 0;
    for( std::size_t i = 0; i < request.text.size(); ++i ) {
        if( request.text[i] == '\n' ) { ++lines; }
    }
    std::ostringstream os;
    os << "wrote " << lines << ( lines == 1 ? " line" : " lines" )
       << " to " << request.path;
    model.setMessage( os.str() );
}

} // namespace


bool performLocalRequest( Model& model, const CommandRequest& request )
{
    if( request.kind == "write-file" ) {
        writeFile( model, request );
        return true;
    }
    return false;
}

} // namespace lens
