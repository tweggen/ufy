#if !defined( _VAULT_UNIFY_DEBUG_HPP )
#define _VAULT_UNIFY_DEBUG_HPP

#include <string>

namespace vault {
namespace unify {


class FileDebugInfo
{
public:
    FileDebugInfo( std::string uri )
        : m_uri( uri ) {}

    std::string getFileUri() const {
        return m_uri;
    }

private:
    std::string m_uri;
};


class TermDebugInfo
{
public:
    TermDebugInfo( 
            const AbstractTerm* pTerm,
            const FileDebugInfo* pFileDebugInfo,
            int line )
        : m_pTerm( pTerm )
        , m_pFileDebugInfo( pFileDebugInfo )
        , m_line( line )
    {}

    const AbstractTerm* getTerm() const 
        { return m_pTerm; }
    const FileDebugInfo* getFileDebugInfo() const 
        { return m_pFileDebugInfo; }
    int getLine() const
        { return m_line; }

private:
    const AbstractTerm* m_pTerm;
    const FileDebugInfo* m_pFileDebugInfo;
    int m_line;
};


}
}

#endif
