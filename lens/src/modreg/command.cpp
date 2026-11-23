/**
 * @file command.cpp
 */

#include "command.hpp"

#include <cctype>

namespace lens {

namespace {

bool isBlank( const std::string& text )
{
    for( std::size_t i = 0; i < text.size(); ++i ) {
        if( !std::isspace( (unsigned char) text[i] ) ) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace {

/** Menu heading for a command id, from its namespace. See Command::category. */
std::string categoryForId( const std::string& id )
{
    const std::string::size_type dot = id.find( '.' );
    const std::string space = ( dot == std::string::npos )
                                  ? id : id.substr( 0, dot );

    if( space == "window" ) { return "Window"; }
    if( space == "help" )   { return "Help"; }
    if( space == "world" || space == "module" ) { return "World"; }
    if( space == "query" || space == "transcript" ) { return "Query"; }
    if( space == "image" )  { return "Image"; }
    if( space == "debug" || space == "trace" ) { return "Debug"; }
    if( space == "edit" || space == "source" )  { return "Edit"; }
    return "File";
}

} // namespace

Command::Command( std::string id, std::string title, std::string help )
    : m_id( std::move( id ) )
    , m_title( std::move( title ) )
    , m_help( std::move( help ) )
    , m_category( categoryForId( m_id ) )
{
}


Command& Command::category( std::string category )
{
    m_category = std::move( category );
    return *this;
}


Command& Command::helpTopic( std::string topic )
{
    m_helpTopic = std::move( topic );
    return *this;
}


Command& Command::enabledWhen( Predicate predicate )
{
    m_enabled = std::move( predicate );
    return *this;
}


bool Command::enabled( const Model& model ) const
{
    /* No predicate means always applicable, which is the common case. */
    return m_enabled ? m_enabled( model ) : true;
}


Command& Command::onRun( Handler handler )
{
    m_run = std::move( handler );
    return *this;
}


std::vector<CommandRequest> Command::run( Model& model ) const
{
    return m_run ? m_run( model ) : std::vector<CommandRequest>();
}


std::string CommandTable::add( const Command& command )
{
    if( isBlank( command.id() ) ) {
        return "a command needs an id";
    }
    if( isBlank( command.title() ) ) {
        return "command '" + command.id() + "' has no title";
    }
    if( isBlank( command.help() ) ) {
        /*
         * The mechanism that keeps UI.md section 5 true. A command with no
         * help text is unreachable from F1, meaningless in M-x, and has no
         * hint -- so it is refused here rather than allowed to become a
         * feature nobody can discover.
         */
        return "command '" + command.id() + "' has no help text";
    }
    if( m_byId.find( command.id() ) != m_byId.end() ) {
        return "command '" + command.id() + "' is already registered";
    }

    m_byId[ command.id() ] = m_commands.size();
    m_commands.push_back( command );
    return std::string();
}


const Command* CommandTable::find( const std::string& id ) const
{
    const std::map<std::string, std::size_t>::const_iterator it =
        m_byId.find( id );
    if( it == m_byId.end() ) {
        return NULL;
    }
    return &m_commands[ it->second ];
}

} // namespace lens
