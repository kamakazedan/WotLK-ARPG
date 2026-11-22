// The few C runtime symbols a freestanding build still needs: the compiler is
// free to emit calls to these for copies and zero-fills, and the MSVC ABI
// expects _fltused in any image that uses floating point.
//
// This file must be compiled with -fno-builtin, or the loops below would be
// turned back into calls to the functions they implement.

#include <stddef.h>

extern "C" {

int _fltused = 0x9875;

void* memcpy(void* destination, const void* source, size_t count) {
    unsigned char* out = static_cast<unsigned char*>(destination);
    const unsigned char* in = static_cast<const unsigned char*>(source);
    for (size_t i = 0; i < count; ++i) {
        out[i] = in[i];
    }
    return destination;
}

void* memmove(void* destination, const void* source, size_t count) {
    unsigned char* out = static_cast<unsigned char*>(destination);
    const unsigned char* in = static_cast<const unsigned char*>(source);
    if (out <= in) {
        for (size_t i = 0; i < count; ++i) {
            out[i] = in[i];
        }
    } else {
        for (size_t i = count; i > 0; --i) {
            out[i - 1] = in[i - 1];
        }
    }
    return destination;
}

void* memset(void* destination, int value, size_t count) {
    unsigned char* out = static_cast<unsigned char*>(destination);
    for (size_t i = 0; i < count; ++i) {
        out[i] = static_cast<unsigned char>(value);
    }
    return destination;
}

int memcmp(const void* left, const void* right, size_t count) {
    const unsigned char* a = static_cast<const unsigned char*>(left);
    const unsigned char* b = static_cast<const unsigned char*>(right);
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

}  // extern "C"
