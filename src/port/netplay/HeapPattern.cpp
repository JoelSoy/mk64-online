// Online play needs uninitialised heap memory to read the same on every device. On macOS and Linux the allocator
// fills new blocks with 0xAA when asked at process start (NetplayGame_ParseArgs re-execs with MallocPreScribble /
// MALLOC_PERTURB_). Android's allocator has no such switch an app can set, so the replaceable C++ allocation
// functions do the same fill here. The game's C code allocates nothing itself: everything it keeps on the heap
// comes through operator new, so this covers what the desktop setting covers.
// Always on, not only in online sessions: on the desktop the setting is in force from the first allocation too, and
// on Android whether a launch is a session is only known after the static constructors have run.

#ifdef __ANDROID__

#include <cstdlib>
#include <cstring>
#include <new>

namespace {

constexpr unsigned char kFill = 0xAA;

void* Allocate(std::size_t size) {
    if (size == 0) {
        size = 1;
    }
    void* p = std::malloc(size);
    if (p != nullptr) {
        std::memset(p, kFill, size);
    }
    return p;
}

void* AllocateAligned(std::size_t size, std::align_val_t align) {
    if (size == 0) {
        size = 1;
    }
    std::size_t a = (std::size_t) align;
    if (a < sizeof(void*)) {
        a = sizeof(void*);
    }
    void* p = nullptr;
    if (posix_memalign(&p, a, size) != 0) {
        return nullptr;
    }
    std::memset(p, kFill, size);
    return p;
}

} // namespace

// Every block still comes from malloc, so libc++'s own operator delete (free) stays the right way to release it.
void* operator new(std::size_t size) {
    void* p = Allocate(size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new[](std::size_t size) {
    return operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return Allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return Allocate(size);
}

void* operator new(std::size_t size, std::align_val_t align) {
    void* p = AllocateAligned(size, align);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new[](std::size_t size, std::align_val_t align) {
    return operator new(size, align);
}

void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return AllocateAligned(size, align);
}

void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return AllocateAligned(size, align);
}

#endif
