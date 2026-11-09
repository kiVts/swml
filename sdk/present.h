#pragma once
#include <windows.h>

// Render thread, right before each SwapBuffers, with the game's GL context current (core event).
using PresentCallback = void (*)(HDC dc);
void present_add_callback(PresentCallback cb);
