// Stub implementations for C++ ABI symbols present in host's libstdc++ (GCC 13)
// but missing from the tg5050 device's libstdc++ (GCC 10).
// These satisfy the linker during cross-compilation. At runtime, if these code
// paths are actually hit, they provide minimal fallback behavior.

#include <cstdlib>
#include <cstring>

namespace std {
  void __throw_bad_array_new_length() { abort(); }
}

// Mangled name for: std::__cxx11::basic_string<char>::_M_replace_cold(char*, unsigned long, char const*, unsigned long, unsigned long)
extern "C" char* _ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE15_M_replace_coldEPcmPKcmm(
    void* _this, char* p, unsigned long len1, const char* s, unsigned long len2, unsigned long) {
    memmove(p, s, len2);
    return p;
}
