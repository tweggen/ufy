#ifndef _VAULT_HPP_
#define _VAULT_HPP_

#include <string>
#include <map>
#include <set>

#include <sys/time.h>

#include <boost/version.hpp>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <boost/thread/lock_guard.hpp>
#include <boost/thread.hpp>
#include <boost/lexical_cast.hpp>

/*
 * Nothing in this header itself uses pplx, but several legacy modules
 * (player2, alsa, ...) rely on getting <pplx/pplxtasks.h> transitively from
 * here, so it stays by default. Builds that must not depend on cpprest
 * (e.g. the standalone vault-unify-core CMake build) define VAULT_NO_PPLX
 * to skip it.
 */
#if !defined( VAULT_NO_PPLX )
#include <pplx/pplxtasks.h>
#endif

namespace vault {

class Timestamp {
public:
    Timestamp() 
            : m_seconds( 0 )
            , m_nanoseconds( 0 ) {}
    Timestamp( int64_t seconds ) 
            : m_seconds( seconds )
            , m_nanoseconds( 0 ) 
    {}
    Timestamp( int64_t seconds, int32_t nanoseconds ) 
            : m_seconds( seconds )
            , m_nanoseconds( nanoseconds )
    {}
    Timestamp( const std::string& str ) 
    {
        long long sec;
        int nano;
        int nResults = sscanf( str.c_str(), "%lld.%d", &sec, &nano );
        if( 2 != nResults ) {
            throw std::runtime_error( "Unable to parse timestamp from string." );
        }
        m_seconds = (int64_t) sec;
        m_nanoseconds = (int32_t) nano;
    }

    static Timestamp now() {
        struct timespec ts;
        (void) clock_gettime( CLOCK_MONOTONIC_RAW, &ts );
        return Timestamp( ts.tv_sec, ts.tv_nsec );
    }

    int64_t seconds() const { return m_seconds; }
    int32_t nanoseconds() const { return m_nanoseconds; }
    Timestamp& seconds( int64_t seconds ) { m_seconds = seconds; return *this; }
    Timestamp& nanoseconds( int32_t nanoseconds ) { m_nanoseconds = nanoseconds; return *this; }


    Timestamp& operator-() {
        m_seconds = -m_seconds;
        m_nanoseconds = -m_nanoseconds;
        normalize();
        return *this;
    }


    void normalize() {
        while(1) {
            if( m_seconds>=0 ) {
                if( m_nanoseconds<0 ) {
                    m_seconds -= 1;
                    m_nanoseconds += 1000000000;
                } else if( m_nanoseconds>=1000000000 ) {
                    m_seconds += 1;
                    m_nanoseconds -= 1000000000;
                } else {
                    break;
                }
            } else {
#if 1
                if( m_nanoseconds<0 ) {
                    m_seconds -= 1;
                    m_nanoseconds += 1000000000;
                } else if( m_nanoseconds>=1000000000 ) {
                    m_seconds += 1;
                    m_nanoseconds -= 1000000000;
                } else {
                    break;
                }
#else
                if( m_nanoseconds<-1000000000 ) {
                    m_nanoseconds += 1000000000;
                    m_seconds -= 1;
                } else if( m_nanoseconds>=0 ) {
                    m_nanoseconds -= 1000000000;
                    m_seconds += 1;
                } else {
                    break;
                }
#endif
            }
        }
    }

    Timestamp& operator-= ( const Timestamp& o ) {
        m_nanoseconds -= o.m_nanoseconds;
        m_seconds -= o.m_seconds;
        normalize();
        return *this;
    }

    Timestamp& operator+= ( const Timestamp& o ) {
        m_nanoseconds += o.m_nanoseconds;
        m_seconds += o.m_seconds;
        normalize();
        return *this;
    }

    Timestamp& operator/= ( const size_t d ) {
        if( !d ) return *this;
        int64_t sec;
        int64_t rem;
        uint64_t nsec = 0;
        normalize();
        if( m_seconds>=0 ) {
            sec = m_seconds/d;
            rem = m_seconds % d;
            nsec = rem*1000000000ull + m_nanoseconds;
            nsec /= d;
            m_seconds = sec;
            m_nanoseconds = nsec;
        } else {
            m_seconds = -m_seconds;
            m_nanoseconds = -m_nanoseconds;
            // Now seconds is positive and nanoseconds negative
            normalize();
            // Now seconds has been decreased and nanoseconds is positive.

            sec = m_seconds/d;
            rem = m_seconds % d;
            nsec = rem*1000000000ull + m_nanoseconds;
            nsec /= d;
            m_seconds = sec;
            m_nanoseconds = nsec;

            m_seconds = -m_seconds;
            m_nanoseconds = -m_nanoseconds;
            normalize();
        }
        return *this;
    }

    operator std::string() const {
        char s[40]; 
        if( m_seconds>=0 ) {
            snprintf( s, 39, "%lld.%09d",
                (long long) m_seconds,
                m_nanoseconds );
        } else {
            snprintf( s, 39, "-%lld.%09d",
                (long long) -(m_seconds+1), 
                1000000000-m_nanoseconds );
        }
        return std::string( s );
    }

    bool operator<( const Timestamp& other ) const {
        if( m_seconds > other.m_seconds ) {
            return false;
        } else {
            if( m_seconds == other.m_seconds ) {
                if( m_nanoseconds >= other.m_nanoseconds ) {
                    return false;
                }
            }
        }
        return true;
    }

    bool operator>( const Timestamp& other ) const {
        if( m_seconds < other.m_seconds ) {
            return false;
        } else {
            if( m_seconds == other.m_seconds ) {
                if( m_nanoseconds <= other.m_nanoseconds ) {
                    return false;
                }
            }
        }
        return true;
    }

    bool operator==( const Timestamp& other ) const {
        return m_seconds==other.m_seconds && m_nanoseconds==other.m_nanoseconds;
    }

    bool operator!=( const Timestamp& other ) const {
        return !(m_seconds==other.m_seconds && m_nanoseconds==other.m_nanoseconds);
    }

private:
    int64_t m_seconds;
    int32_t m_nanoseconds;
};

class StatePart;
class StateHolder {
public:
    StateHolder() {}
    virtual ~StateHolder() {}

protected:
    boost::mutex& mutex() const { return m_mutex; }

private:
    StateHolder( StateHolder& ) {}
    friend class StatePart;
    mutable boost::mutex m_mutex;
};

class StatePart {
public:
    StatePart( StateHolder& parentHolder )
            : m_mutex( parentHolder.mutex() ) {}
    StatePart( StatePart& parentPart )
            : m_mutex( parentPart.mutex() ) {}
    virtual ~StatePart() {}

protected:
    boost::mutex& mutex() const { return m_mutex; }

private:
    boost::mutex& m_mutex;
};

// Convenience
typedef boost::unique_lock<boost::mutex> Guard;
typedef boost::unique_lock<boost::recursive_mutex> RGuard;

class UUID {
public:
    static UUID& getInstance() {
        static UUID uuid;
        return uuid;
    }

    UUID();
    uint64_t get64() const;
    std::string getString() const;
private:
};

class IDFactory {
public:
    static std::string create() {
        static boost::mutex mutex;

        /*
         * Create the initial seed from our uuid.
         */
        static uint64_t id = UUID::getInstance().get64();
        std::string strId;
        {
            Guard g( mutex );
            ++id;
            strId = boost::lexical_cast<std::string>( id );
        }
        return strId;
    }
    static std::string create( const std::string& pfx )
        { return pfx+create(); }
};

/**
 * Singleton class to provide one system-wide boost io service.
 */
class BoostAsioIoService {
public:
    /*
     * Boost.Asio renamed io_service to io_context in 1.66 (the old name
     * staying a typedef of the new one) and removed io_service and
     * io_service::work outright in 1.87. Alias both names here so this
     * header builds against both a modern Boost and the pre-1.66 Boost the
     * legacy Boost.Jam build still uses. Callers are unaffected: they only
     * ever pass get() / getLocal() into sockets and timers, which take the
     * same type under either name.
     */
#if BOOST_VERSION >= 106600
    typedef boost::asio::io_context IoService;
    typedef boost::asio::executor_work_guard<
            boost::asio::io_context::executor_type > IoServiceWork;
#else
    typedef boost::asio::io_service IoService;
    typedef boost::asio::io_service::work IoServiceWork;
#endif

    static IoService& get() {
        static BoostAsioIoService instance;
        return instance.m_ioService;
    }

    IoService& getLocal() {
        return m_ioService;
    }

    BoostAsioIoService() 
            : m_ioServiceWork( makeWork( m_ioService ) )
    {
        /*
         * run() is overloaded (the deprecated error_code overload is still
         * declared unless BOOST_ASIO_NO_DEPRECATED), so the member pointer
         * needs an explicit signature to be unambiguous. Both io_service and
         * io_context return std::size_t from the no-argument overload.
         */
        boost::thread bt( boost::bind(
            static_cast< std::size_t (IoService::*)() >( &IoService::run ),
            &m_ioService ) );
    }

private:

    static IoServiceWork makeWork( IoService& ioService ) {
#if BOOST_VERSION >= 106600
        return boost::asio::make_work_guard( ioService );
#else
        return IoServiceWork( ioService );
#endif
    }

    IoService m_ioService;
    IoServiceWork m_ioServiceWork;

};

#if 0
class RGuard 
{
public:
    RGuard( boost::recursive_mutex& r )
            : m_r( r ) {
        m_r.lock();
    }
    ~RGuard() {
        m_r.unlock();
    }
private:
    boost::recursive_mutex& m_r;
};
#endif


template <typename Iterator> inline const std::string& diffMapsGetKey( Iterator& it ) { return it->first; }
inline const std::string& diffMapsGetKey( std::set<std::string>::iterator& it ) { return *it; }

/**
 * Iterate through a set or a map. Diff the data structures and call the given functions
 * for elements that are added, removed or still remain in the map.
 * Please note, that the functions must not modify the data structure index, i.e. they must
 * not add or remove any members of the dataq structure while iterating.
 */
template <class Map, typename AddedFunc, typename RemovedFunc, typename RemainFunc>
void diffMaps(
        Map* pA, Map *pB, 
        AddedFunc addedFunc, RemovedFunc removedFunc,
        RemainFunc remainFunc,
        bool debug=false )
{
    if( pA ) {
        if( pB ) {

            // Dump for debugging
            if( debug ) {
                std::string strList( "Old members: ");
                typename Map::iterator itEnd = pA->end(), it;
                for( it=pA->begin(); it != itEnd; ++it ) {
                    if( pA->begin() != it ) {
                        strList += ", ";
                    }
                    strList += diffMapsGetKey( it );
                }
                fprintf( stderr, "Comparing: %s.\n", strList.c_str() );
            }
            if( debug ) {
                std::string strList( "New members: ");
                typename Map::iterator itEnd = pB->end(), it;
                for( it=pB->begin(); it != itEnd; ++it ) {
                    if( pB->begin() != it ) {
                        strList += ", ";
                    }
                    strList += diffMapsGetKey( it );
                }
                fprintf( stderr, "Comparing: %s.\n", strList.c_str() );
            }


            // Both configs exist, iterate for changes.
            typename Map::iterator 
                itepA = pA->begin(),
                itepAEnd = pA->end(),
                itepB = pB->begin(),
                itepBEnd = pB->end();

            while(1) {
                if( itepA!=itepAEnd ) {
                    if( itepB!=itepBEnd ) {
                        // Both iterators are not at the end, so we can compare
                        // elements and possibly emit changes.
                        int relation = diffMapsGetKey( itepB ).compare( diffMapsGetKey( itepA ) );
                        if( relation<0 ) {
                            // The current B value is less than the current A value.
                            // So the current B element is new.
                            addedFunc( itepB );
                            ++itepB;
                        } else if( relation==0 ) {
                            // No change, both elements do exist.
                            remainFunc( itepA );
                            ++itepA;
                            ++itepB;
                        } else {
                            // The current B value is greater than the current A value.
                            // So the current A element is lost.
                            removedFunc( itepA );
                            ++itepA;
                        }
                    } else {
                        // B is over, A is not. So all elements
                        // from this position in A are removed.
                        do {
                            removedFunc( itepA );
                            ++itepA;
                        } while( itepA != itepAEnd );
                        break;
                    }
                } else {
                    if( itepB!=itepBEnd ) {
                        // A does have any more elements, B does.
                        // So all elements that we find in B are new.
                        do {
                            addedFunc( itepB );
                            ++itepB;
                        } while( itepB != itepBEnd );
                        break;
                    } else {
                        // Neither A nor B do have any more elements.
                        // So we are done.
                        break;
                    }
                }
            }

        } else {
            // New one does not exist -> all devices gone.
            typename Map::iterator 
                itepA = pA->begin(),
                itepAEnd = pA->end();
            for( ; itepA != itepAEnd; ++itepA ) {
                removedFunc( itepA );
            }
        }
    } else {
        if( pB ) {
            // Old one did not exist -> all devices new.
            // New one does not exist -> all devices gone.
            typename Map::iterator 
                itepB = pB->begin(),
                itepBEnd = pB->end();
            for( ; itepB != itepBEnd; ++itepB ) {
                addedFunc( itepB );
            }
        } else {
            // both do not exist -> no change.
        }
    }
}


}

#endif