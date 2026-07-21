#ifndef UTILS_H_
#define UTILS_H_
#include <Windows.h>
class Utils {
public:
    Utils(){};
public:
    static BYTE* allocate_buffer(size_t size, size_t alignment);
};
#endif
