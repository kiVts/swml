#pragma once
#include <cstdint>

template<class Native, class Observe, class Dispatch>
inline void native_ui_update(Native native, void* object, uintptr_t traversal, uintptr_t application,
                             Observe observe, Dispatch dispatch)
{
    native(object, traversal, application);
    observe(object, application);
    dispatch(object, application);
}
