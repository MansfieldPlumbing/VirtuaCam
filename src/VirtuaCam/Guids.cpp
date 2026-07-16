// =============================================================================
// Guids.cpp  --  GUID definitions
// =============================================================================
// INITGUID must be #defined in exactly one translation unit before including
// any header that uses DEFINE_GUID().  Including it here (via initguid.h)
// causes the linker to emit the actual GUID data instead of just a declaration.
// All other .cpp files must NOT include initguid.h, and Guids.cpp must be
// excluded from precompiled headers (see CMakeLists.txt: SKIP_PRECOMPILE_HEADER).
// =============================================================================

#include <windows.h>
#include <initguid.h>
#include "Guids.h"