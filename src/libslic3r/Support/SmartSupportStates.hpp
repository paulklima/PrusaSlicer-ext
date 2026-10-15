#ifndef slic3r_SmartSupportStates_hpp_
#define slic3r_SmartSupportStates_hpp_

// Smart support painting (fork feature smart-auto-supports): encoding of the automatically painted
// ("auto", "smart") support enforcer states inside ModelVolume::supported_facets.
//
// Manual paint keeps the stock states NONE (0), ENFORCER (1) and BLOCKER (2). Smart paint uses the states
// 3..9 = 2 + mask, where mask is a non-empty bit set of the causes that requested support at that facet
// (cbIsland, cbCantilever, cbLongSpan). States 3..9 fit into painting version 1 (states <= 16).
// The slicer treats all auto states as enforcers (enforcer_like_states()); the 3MF export bakes them
// to ENFORCER for stock PrusaSlicer (bake_map()).
//
// Header only, all functions are pure and thread-safe.

#include <cstdint>
#include <map>
#include <vector>

#include "libslic3r/TriangleSelector.hpp"

namespace Slic3r::SmartSupports {

// Cause bits of an auto state. Bit i corresponds to SmartSupports::Cause(i) (SmartSupports.hpp).
enum CauseBit : uint8_t { cbIsland = 1, cbCantilever = 2, cbLongSpan = 4, cbAll = 7 };

// States 3..9.
inline constexpr bool    is_auto_state(TriangleStateType s) { return uint8_t(s) >= 3 && uint8_t(s) <= 9; }
// Cause mask of an auto state, 0 for any other state.
inline constexpr uint8_t auto_mask(TriangleStateType s)     { return is_auto_state(s) ? uint8_t(uint8_t(s) - 2) : 0; }
// State of a cause mask (only the bits of cbAll are allowed); mask 0 gives NONE.
inline constexpr TriangleStateType auto_state(uint8_t mask) { return mask == 0 ? TriangleStateType::NONE : TriangleStateType(uint8_t(mask + 2)); }
// Does the slicer use a facet of this state as a support enforcer (manual ENFORCER or any auto state)?
inline constexpr bool    acts_as_enforcer(TriangleStateType s) { return s == TriangleStateType::ENFORCER || is_auto_state(s); }

// {ENFORCER, 3..9}: the states the slicer projects as enforcers.
inline std::vector<TriangleStateType> enforcer_like_states()
{
    std::vector<TriangleStateType> out { TriangleStateType::ENFORCER };
    for (uint8_t mask = 1; mask <= cbAll; ++ mask)
        out.emplace_back(auto_state(mask));
    return out;
}

// Map for TriangleSelector::remap_states(): auto(m) -> auto(m & ~causes); an empty mask becomes NONE.
// Manual states are not in the map, thus kept. clear_causes_map(cbAll) removes all smart paint.
inline std::map<TriangleStateType, TriangleStateType> clear_causes_map(uint8_t causes)
{
    std::map<TriangleStateType, TriangleStateType> out;
    for (uint8_t mask = 1; mask <= cbAll; ++ mask)
        out.emplace(auto_state(mask), auto_state(uint8_t(mask & ~causes & cbAll)));
    return out;
}

// Map for TriangleSelector::remap_states(): auto(m) -> ENFORCER (3MF export for stock PrusaSlicer).
inline std::map<TriangleStateType, TriangleStateType> bake_map()
{
    std::map<TriangleStateType, TriangleStateType> out;
    for (uint8_t mask = 1; mask <= cbAll; ++ mask)
        out.emplace(auto_state(mask), TriangleStateType::ENFORCER);
    return out;
}

// Paint rule of the smart painter for one cause bit (TriangleSelector::set_paint_rule()):
// NONE -> auto(bit); auto(m) -> auto(m | bit); manual states (ENFORCER, BLOCKER) and any other state unchanged.
// The requested state of select_patch() is ignored.
inline TriangleSelector::PaintRule tool_paint_rule(uint8_t cause_bit)
{
    return [cause_bit](TriangleStateType old_state, TriangleStateType /* requested */) {
        if (old_state == TriangleStateType::NONE)
            return auto_state(uint8_t(cause_bit & cbAll));
        if (is_auto_state(old_state))
            return auto_state(uint8_t((auto_mask(old_state) | cause_bit) & cbAll));
        return old_state;
    };
}

} // namespace Slic3r::SmartSupports

#endif // slic3r_SmartSupportStates_hpp_
