#if !defined( _LENS_MODEL_VIEW_HPP )
#define _LENS_MODEL_VIEW_HPP

/**
 * @file view.hpp
 *
 * `view : (Model, Geometry) -> CellGrid` -- ARCHITECTURE.md section 4.
 *
 * Pure. The same model and geometry always render the same grid, which is
 * what makes a golden screen a golden screen rather than a snapshot of
 * whatever the machine felt like doing.
 */

#include "cell-grid.hpp"
#include "model.hpp"

namespace lens {

/** Render the whole screen. */
CellGrid view( const Model& model );

} // namespace lens

#endif // _LENS_MODEL_VIEW_HPP
