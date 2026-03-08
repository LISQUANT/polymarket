// GCC 14 emits calls to __cxa_call_terminate for noexcept functions,
// but Boost 1.82 from conda was compiled with an older GCC that lacks this symbol.
#include <exception>

extern "C" void __cxa_call_terminate(void*) {
    std::terminate();
}
