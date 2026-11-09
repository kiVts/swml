#pragma once
#include <cstdint>

// Optional physics_fixes.dll interface, not a change to the core SDK ABI.
// One callback pair surrounds all component ticks of ONE vehicle on the same worker.
// Callback must not retain vehicle/scene/body pointers beyond its end notification.
// Different vehicles may run simultaneously. No registration means no callbacks.
using VehicleAeroObserver = void(__cdecl*)(uintptr_t vehicle, bool begin);
using RegisterVehicleAeroObserver = bool(__cdecl*)(uint32_t version, VehicleAeroObserver observer);
constexpr uint32_t kVehicleAeroObserverVersion = 1;
