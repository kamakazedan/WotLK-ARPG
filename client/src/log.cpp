#include "log.h"

#include <stdarg.h>

#include "win.h"

namespace logfile {
namespace {

void* g_file = INVALID_HANDLE_VALUE;

struct Line {
    char text[512];
    size_t length = 0;

    void put(char c) {
        if (length < sizeof(text) - 2) {  // room for the line ending
            text[length++] = c;
        }
    }

    void str(const char* s) {
        if (s == nullptr) {
            s = "(null)";
        }
        while (*s != '\0') {
            put(*s++);
        }
    }

    void hex(uint32_t value, int digits) {
        static const char kDigits[] = "0123456789ABCDEF";
        for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) {
            put(kDigits[(value >> shift) & 0xF]);
        }
    }

    void dec(uint32_t value) {
        char buffer[10];
        int count = 0;
        do {
            buffer[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count > 0) {
            put(buffer[--count]);
        }
    }
};

}  // namespace

void Open() {
    char path[300];
    uint32_t length = GetModuleFileNameA(nullptr, path, 260);
    while (length > 0 && path[length - 1] != '\\' && path[length - 1] != '/') {
        --length;
    }
    const char* name = "WotLK-ARPG.log";
    while (*name != '\0') {
        path[length++] = *name++;
    }
    path[length] = '\0';
    g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
}

void Write(const char* format, ...) {
    if (g_file == INVALID_HANDLE_VALUE) {
        return;
    }

    Line line;
    va_list args;
    va_start(args, format);
    for (const char* p = format; *p != '\0'; ++p) {
        if (*p != '%') {
            line.put(*p);
            continue;
        }
        ++p;
        switch (*p) {
            case 's':
                line.str(va_arg(args, const char*));
                break;
            case 'd': {
                int value = va_arg(args, int);
                if (value < 0) {
                    line.put('-');
                    line.dec(0u - static_cast<uint32_t>(value));
                } else {
                    line.dec(static_cast<uint32_t>(value));
                }
                break;
            }
            case 'u':
                line.dec(va_arg(args, uint32_t));
                break;
            case 'x':
                line.hex(va_arg(args, uint32_t), 8);
                break;
            case 'B': {
                const uint8_t* bytes = va_arg(args, const uint8_t*);
                int count = va_arg(args, int);
                for (int i = 0; i < count; ++i) {
                    if (i != 0) {
                        line.put(' ');
                    }
                    line.hex(bytes[i], 2);
                }
                break;
            }
            case '\0':
                --p;  // stray % at the end of the format
                break;
            default:
                line.put(*p);
                break;
        }
    }
    va_end(args);

    line.text[line.length++] = '\r';
    line.text[line.length++] = '\n';
    uint32_t written = 0;
    WriteFile(g_file, line.text, static_cast<uint32_t>(line.length), &written, nullptr);
}

}  // namespace logfile
