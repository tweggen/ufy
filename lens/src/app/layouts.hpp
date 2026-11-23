#if !defined( _LENS_APP_LAYOUTS_HPP )
#define _LENS_APP_LAYOUTS_HPP

/**
 * @file layouts.hpp
 *
 * The four stock layouts -- UI.md section 2.1, recorded as goldens at both
 * 120x40 and 80x24 (gate G1.1).
 *
 * Each is a STANCE rather than an arrangement of boxes: browse is the
 * Smalltalk system-browser stance, run is for driving a program, debug is
 * for finding out why it does that, full is for the person who has 200
 * columns. Naming them for the stance is what makes "switch layout" a
 * meaningful gesture rather than a shuffle.
 *
 * None of them special-cases the small geometry. At 80x24 each degrades
 * through the ordinary solver -- some tiles become one-line stubs -- which
 * is why there is no second set of presets to keep in step with the first.
 */

#include "../model/model.hpp"

#include <string>
#include <vector>

namespace lens {

/** Names accepted by `--layout`, in the order help lists them. */
std::vector<std::string> stockLayoutNames();

/**
 * Install a stock layout into `model`, creating its buffers.
 *
 * @return false if there is no such layout, leaving the model untouched.
 */
bool applyStockLayout( Model& model, const std::string& name );

} // namespace lens

#endif // _LENS_APP_LAYOUTS_HPP
