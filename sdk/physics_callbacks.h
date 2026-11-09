#pragma once
#include <cstdint>

// Multiplayer work that runs inside physics_fixes.dll's physics step and solver hooks. network_fixes.dll exports
// swml_physics_callbacks; physics_fixes.dll looks it up and calls these. Neither mod needs the other to load.
// All on the physics thread except solver_setup and solver_finish, which may run on its workers for disjoint bodies.
struct SwmlPhysicsCallbacks {
    uint32_t size;
    void(__cdecl* step_begin)(const void* world);
    double(__cdecl* fixed_step)(double elapsed, int max_substeps, double native_fixed, int local_walking); // the step to use
    void(__cdecl* launch_begin)(const void* world, double elapsed, double fixed);
    void(__cdecl* launch_end)(const void* world);
    void(__cdecl* step_end)(const void* world, double fixed, int steps);
    void(__cdecl* solver_begin)(const void* world);
    int(__cdecl* solver_native)(); // nonzero: leave the solver to the game
    void(__cdecl* solver_setup)(void* solver, void** bodies, int nbodies, void** manifolds, int nmanifolds);
    void(__cdecl* solver_finish)(void* solver);
    void(__cdecl* solver_end)();
};
using SwmlPhysicsCallbacksFn = const SwmlPhysicsCallbacks*(__cdecl*)();
