#if !defined( _LENS_LAYOUT_GEOMETRY_HPP )
#define _LENS_LAYOUT_GEOMETRY_HPP

/**
 * @file geometry.hpp
 *
 * Integer screen rectangles. Deliberately tiny and deliberately its own
 * header: `layout/` is pure and must not acquire a dependency on a terminal
 * library's rectangle type, which is exactly how a framework escapes its
 * containment (ACCEPTANCE.md G1.5).
 */

#include <cstdint>

namespace lens {

/**
 * A half-open rectangle in character cells: columns [x, x+w), rows
 * [y, y+h).
 *
 * Half-open because every adjacency test in the solver -- "do these two
 * tiles overlap", "do they cover the area exactly" -- is off by one in at
 * least one place if the convention is inclusive, and the coverage test in
 * G1.2 is the whole reason this type exists.
 */
struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    constexpr Rect() = default;
    constexpr Rect( int ax, int ay, int aw, int ah )
        : x( ax ), y( ay ), w( aw ), h( ah ) {}

    constexpr int right()  const { return x + w; }
    constexpr int bottom() const { return y + h; }
    constexpr int area()   const { return w * h; }
    constexpr bool empty() const { return w <= 0 || h <= 0; }

    constexpr bool contains( int px, int py ) const
    {
        return px >= x && px < right() && py >= y && py < bottom();
    }

    /** True if the two rectangles share at least one cell. */
    constexpr bool intersects( const Rect& o ) const
    {
        return !( o.x >= right() || x >= o.right()
               || o.y >= bottom() || y >= o.bottom() );
    }

    constexpr bool operator == ( const Rect& o ) const
    {
        return x == o.x && y == o.y && w == o.w && h == o.h;
    }
    constexpr bool operator != ( const Rect& o ) const { return !( *this == o ); }
};

/** A size with no position, for the solver's minimum-size arithmetic. */
struct Size {
    int w = 0;
    int h = 0;

    constexpr Size() = default;
    constexpr Size( int aw, int ah ) : w( aw ), h( ah ) {}

    constexpr bool fitsIn( const Rect& r ) const { return w <= r.w && h <= r.h; }
    constexpr bool operator == ( const Size& o ) const
    {
        return w == o.w && h == o.h;
    }
};

} // namespace lens

#endif // _LENS_LAYOUT_GEOMETRY_HPP
