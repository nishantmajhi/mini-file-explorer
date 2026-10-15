#include "PtyBackend.h"

#ifdef __WXMSW__
#include "WinConPtyBackend.h"
#else
#include "PosixPtyBackend.h"
#endif

std::unique_ptr<IPtyBackend> CreatePtyBackend()
{
#ifdef __WXMSW__
    return std::make_unique<WinConPtyBackend>();
#else
    return std::make_unique<PosixPtyBackend>();
#endif
}
