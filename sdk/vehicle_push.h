#pragma once
#include <cstdint>
// Physics-thread contact reaction, in the contacted body's local frame. Force is in newtons.
struct SwmlVehiclePushSample {
    const void* body;
    float point[3], force[3];
};
static_assert(sizeof(SwmlVehiclePushSample)==32);
