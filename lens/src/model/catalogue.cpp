/**
 * @file catalogue.cpp
 */

#include "catalogue.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>

namespace lens {

namespace {

const char* const kBuiltins = "builtins";
const char* const kSynthesized = "synthesized";
const char* const kTranscript = "<transcript>";
const char* const kAsserted = "asserted";

/** Groups in the order a reader wants them: their own work first. */
int groupRank( const std::string& group )
{
    if( group == kTranscript )  { return 0; }
    if( group == kAsserted )    { return 2; }
    if( group == kSynthesized ) { return 3; }
    if( group == kBuiltins )    { return 4; }
    return 1;   /* a module */
}

std::string lowered( const std::string& text )
{
    std::string out( text );
    for( std::size_t i = 0; i < out.size(); ++i ) {
        out[i] = (char) std::tolower( (unsigned char) out[i] );
    }
    return out;
}

std::string keyLabel( const us::CatalogueEntry& entry )
{
    std::ostringstream os;
    os << entry.key.name << "/" << entry.key.arity;
    return os.str();
}

bool isOpen( const CatalogueState& catalogue, const std::string& group )
{
    const bool byDefault = catalogueGroupOpensByDefault( group );
    return catalogue.toggled.count( group ) ? !byDefault : byDefault;
}

} // namespace


std::string catalogueGroupOf( const us::CatalogueEntry& entry )
{
    switch( entry.origin.kind ) {
    case us::Origin::Kind::Builtin:     return kBuiltins;
    case us::Origin::Kind::Synthesized: return kSynthesized;
    case us::Origin::Kind::Transcript:  return kTranscript;
    case us::Origin::Kind::Asserted:    return kAsserted;
    case us::Origin::Kind::Module:
        break;
    }
    /*
     * A module is named by its file, as a reader knows it. The listing
     * carries an id and a path and no module name, so the path's last
     * component is the most human thing on offer; the id is the floor.
     */
    const std::string& file = entry.origin.file;
    if( !file.empty() ) {
        const std::string::size_type slash = file.find_last_of( "/\\" );
        return slash == std::string::npos ? file : file.substr( slash + 1 );
    }
    std::ostringstream os;
    os << "module " << entry.key.module;
    return os.str();
}


bool catalogueGroupOpensByDefault( const std::string& group )
{
    return group != kBuiltins && group != kSynthesized;
}


void indexCatalogue( CatalogueState& catalogue )
{
    const std::vector<us::CatalogueEntry>& entries = catalogue.entries;
    std::vector<int>& order = catalogue.order;
    std::vector<std::string>& groups = catalogue.groups;

    order.resize( entries.size() );
    groups.resize( entries.size() );
    catalogue.lowerNames.resize( entries.size() );
    std::vector<int> ranks( entries.size() );
    for( std::size_t i = 0; i < entries.size(); ++i ) {
        order[i] = (int) i;
        groups[i] = catalogueGroupOf( entries[i] );
        ranks[i] = groupRank( groups[i] );
        catalogue.lowerNames[i] = lowered( entries[i].key.name );
    }
    std::sort( order.begin(), order.end(), [&]( int a, int b ) {
        const std::size_t ia = (std::size_t) a;
        const std::size_t ib = (std::size_t) b;
        if( ranks[ia] != ranks[ib] ) { return ranks[ia] < ranks[ib]; }
        if( groups[ia] != groups[ib] ) { return groups[ia] < groups[ib]; }
        const us::PredicateKey& ka = entries[ia].key;
        const us::PredicateKey& kb = entries[ib].key;
        if( ka.name != kb.name ) { return ka.name < kb.name; }
        return ka.arity < kb.arity;
    } );
}


std::vector<CatalogueRow> catalogueRows( const CatalogueState& catalogue )
{
    std::vector<CatalogueRow> rows;
    const std::vector<us::CatalogueEntry>& entries = catalogue.entries;
    if( catalogue.order.size() != entries.size() ) {
        /* Not indexed (a caller filled `entries` by hand): index a copy. */
        CatalogueState indexed = catalogue;
        indexCatalogue( indexed );
        return catalogueRows( indexed );
    }
    const std::vector<int>& order = catalogue.order;
    const std::vector<std::string>& groups = catalogue.groups;

    if( !catalogue.filter.empty() ) {
        const std::string needle = lowered( catalogue.filter );
        for( std::size_t k = 0; k < order.size(); ++k ) {
            const us::CatalogueEntry& e = entries[ (std::size_t) order[k] ];
            if( catalogue.lowerNames[ (std::size_t) order[k] ].find( needle )
                == std::string::npos ) {
                continue;
            }
            CatalogueRow row;
            row.kind = CatalogueRow::Kind::Predicate;
            row.group = groups[ (std::size_t) order[k] ];
            row.label = keyLabel( e );
            row.count = e.clauseCount;
            row.entry = order[k];
            rows.push_back( row );
        }
        return rows;
    }

    for( std::size_t k = 0; k < order.size(); ) {
        const std::string& group = groups[ (std::size_t) order[k] ];
        std::size_t end = k;
        while( end < order.size() && groups[ (std::size_t) order[end] ] == group ) {
            ++end;
        }

        CatalogueRow head;
        head.kind = CatalogueRow::Kind::Group;
        head.group = group;
        head.label = group;
        head.count = (std::uint32_t)( end - k );
        head.open = isOpen( catalogue, group );
        rows.push_back( head );

        if( head.open ) {
            for( std::size_t j = k; j < end; ++j ) {
                const us::CatalogueEntry& e = entries[ (std::size_t) order[j] ];
                CatalogueRow row;
                row.kind = CatalogueRow::Kind::Predicate;
                row.group = group;
                row.label = keyLabel( e );
                row.count = e.clauseCount;
                row.entry = order[j];
                rows.push_back( row );
            }
        }
        k = end;
    }
    return rows;
}

} // namespace lens
