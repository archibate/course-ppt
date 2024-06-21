#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cxxabi.h>
#include <execinfo.h>
#include <map>
#include <new>
#include <string>

static std::string addr2sym(void *addr) {
    char **strings = backtrace_symbols(&addr, 1);
    if (strings == nullptr) {
        return "unknown";
    }
    std::string ret = strings[0];
    free(strings);
    auto pos = ret.find('(');
    if (pos != std::string::npos) {
        auto pos2 = ret.find('+', pos);
        if (pos2 != std::string::npos) {
            ret = ret.substr(pos + 1, pos2 - pos - 1);
            char *demangled =
                abi::__cxa_demangle(ret.data(), nullptr, nullptr, nullptr);
            if (demangled) {
                ret = demangled;
                free(demangled);
            }
        } else {
        }
    }
    return ret;
}

enum class AllocOp {
    New,
    Delete,
    NewArray,
    DeleteArray,
    Malloc,
    Free,
};

struct AllocatedInfo {
    AllocOp op;
    void *ptr;
    size_t size;
    size_t align;
    void *caller;
};

constexpr const char *kAllocOpNames[] = {
    "New",
    "Delete",
    "NewArray",
    "DeleteArray",
    "Malloc",
    "Free",
};

constexpr bool kAllocOpIsAllocation[] = {
    true,
    false,
    true,
    false,
    true,
    false,
};

constexpr AllocOp kAllocOpPair[] = {
    AllocOp::Delete,
    AllocOp::New,
    AllocOp::DeleteArray,
    AllocOp::NewArray,
    AllocOp::Free,
    AllocOp::Malloc,
};

constexpr size_t kNone = (size_t)-1;

struct GlobalData {
    std::map<void *, AllocatedInfo> allocated;
    bool enable = false;

    GlobalData() {
        enable = true;
    }

    ~GlobalData() {
        enable = false;
        for (auto [ptr, info]: allocated) {
            printf("检测到内存泄漏 ptr = %p, size = %zd, caller = %s\n", ptr,
                   info.size, addr2sym(info.caller).c_str());
        }
    }

    void on(AllocOp op, void *ptr, size_t size, size_t align, void *caller) {
        if (ptr) {
            printf("%s(ptr=%p, size=%zd, align=%zd, caller=%p)\n",
                   kAllocOpNames[(size_t)op], ptr, size, align, caller);
            if (kAllocOpIsAllocation[(size_t)op]) {
                if (!allocated.insert({ptr, AllocatedInfo{op, ptr, size, align, caller}}).second) {
                    printf("检测到内存多次分配同一个地址 ptr = %p, size = %zd, caller = %s\n",
                           ptr, size, addr2sym(caller).c_str());
                }
            } else {
                auto it = allocated.find(ptr);
                if (it == allocated.end()) {
                    printf("检测到尝试释放不存在的内存 ptr = %p, size = %zd, caller = %s\n",
                           ptr, size, addr2sym(caller).c_str());
                } else {
                    if (it->second.op != kAllocOpPair[(size_t)op]) {
                        printf("检测到内存释放时使用了错误的释放函数 ptr = %p, size = %zd, caller = %s\n",
                               ptr, size, addr2sym(caller).c_str());
                    }
                    if (size != kNone) {
                        if (it->second.size != size) {
                            printf("检测到内存释放时指定了错误的大小 ptr = %p, size = %zd, caller = %s\n",
                                   ptr, size, addr2sym(caller).c_str());
                        }
                    }
                    if (align != kNone) {
                        if (it->second.align != align) {
                            printf("检测到内存释放时指定了错误的对齐 ptr = %p, size = %zd, align = %zd, caller = %s\n",
                                   ptr, size, align, addr2sym(caller).c_str());
                        }
                    }
                    allocated.erase(it);
                }
            }
        }
    }
} global;

struct EnableGuard {
    bool was_enable;

    EnableGuard() {
        was_enable = global.enable;
        global.enable = false;
    }

    explicit operator bool() {
        return was_enable;
    }

    ~EnableGuard() {
        global.enable = was_enable;
    }
};

void operator delete(void *p) noexcept;
void operator delete[](void *p) noexcept;

void operator delete(void *p, std::nothrow_t const &) noexcept;
void operator delete[](void *p, std::nothrow_t const &) noexcept;

void *operator new(std::size_t n) noexcept(false);
void *operator new[](std::size_t n) noexcept(false);

void *operator new(std::size_t n, std::nothrow_t const &tag) noexcept;
void *operator new[](std::size_t n, std::nothrow_t const &tag) noexcept;

#if (__cplusplus >= 201402L || _MSC_VER >= 1916)
void operator delete(void *p, std::size_t n) noexcept;
void operator delete[](void *p, std::size_t n) noexcept;
#endif

#if (__cplusplus > 201402L || defined(__cpp_aligned_new))
void operator delete(void *p, std::align_val_t al) noexcept;
void operator delete[](void *p, std::align_val_t al) noexcept;
void operator delete(void *p, std::size_t n, std::align_val_t al) noexcept;
void operator delete[](void *p, std::size_t n, std::align_val_t al) noexcept;
void operator delete(void *p, std::align_val_t al,
                     std::nothrow_t const &) noexcept;
void operator delete[](void *p, std::align_val_t al,
                       std::nothrow_t const &) noexcept;

void *operator new(std::size_t n, std::align_val_t al) noexcept(false);
void *operator new[](std::size_t n, std::align_val_t al) noexcept(false);
void *operator new(std::size_t n, std::align_val_t al,
                   std::nothrow_t const &) noexcept;
void *operator new[](std::size_t n, std::align_val_t al,
                     std::nothrow_t const &) noexcept;
#endif

void operator delete(void *ptr) noexcept {
    EnableGuard ena;
    if (ena) {
        global.on(AllocOp::Delete, ptr, kNone, kNone, __builtin_return_address(0));
    }
    free(ptr);
}

void operator delete[](void *ptr) noexcept {
    EnableGuard ena;
    if (ena) {
        global.on(AllocOp::DeleteArray, ptr, kNone, kNone, __builtin_return_address(0));
    }
    free(ptr);
}

void operator delete(void *ptr, std::nothrow_t const &) noexcept {
    EnableGuard ena;
    if (ena) {
        global.on(AllocOp::Delete, ptr, kNone, kNone, __builtin_return_address(0));
    }
    free(ptr);
}

void operator delete[](void *ptr, std::nothrow_t const &) noexcept {
    EnableGuard ena;
    if (ena) {
        global.on(AllocOp::DeleteArray, ptr, kNone, kNone, __builtin_return_address(0));
    }
    free(ptr);
}

void *operator new(size_t size) noexcept(false) {
    EnableGuard ena;
    void *ptr = malloc(size);
    if (ena) {
        global.on(AllocOp::New, ptr, size, kNone, __builtin_return_address(0));
    }
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void *operator new[](size_t size) noexcept(false) {
    EnableGuard ena;
    void *ptr = malloc(size);
    if (ena) {
        global.on(AllocOp::NewArray, ptr, size, kNone, __builtin_return_address(0));
    }
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void *operator new(size_t size, std::nothrow_t const &) noexcept {
    EnableGuard ena;
    void *ptr = malloc(size);
    if (ena) {
        global.on(AllocOp::New, ptr, size, kNone, __builtin_return_address(0));
    }
    return ptr;
}

void *operator new[](size_t size, std::nothrow_t const &) noexcept {
    EnableGuard ena;
    void *ptr = malloc(size);
    if (ena) {
        global.on(AllocOp::NewArray, ptr, size, kNone, __builtin_return_address(0));
    }
    return ptr;
}

#if (__cplusplus >= 201402L || _MSC_VER >= 1916)
void operator delete(void *ptr, size_t size) noexcept {
    EnableGuard ena;
    if (ena) {
        global.on(AllocOp::Delete, ptr, size, kNone, __builtin_return_address(0));
    }
    free(ptr);
}

void operator delete[](void *ptr, size_t size) noexcept {
    EnableGuard ena;
    if (ena) {
        global.on(AllocOp::DeleteArray, ptr, size, kNone, __builtin_return_address(0));
    }
    free(ptr);
}
#endif

#if (__cplusplus > 201402L || defined(__cpp_aligned_new))
void operator delete(void *ptr, std::align_val_t al) noexcept {
    EnableGuard ena;
    if (ena) {
        global.on(AllocOp::Delete, ptr, kNone, (size_t)al, __builtin_return_address(0));
    }
    free(ptr);
}

void operator delete[](void *ptr, std::align_val_t al) noexcept {
    EnableGuard ena;
    if (ena) {
        global.on(AllocOp::DeleteArray, ptr, kNone, (size_t)al, __builtin_return_address(0));
    }
    free(ptr);
}

void operator delete(void *ptr, size_t size, std::align_val_t al) noexcept {
    EnableGuard ena;
    if (ena) {
        global.on(AllocOp::Delete, ptr, size, (size_t)al, __builtin_return_address(0));
    }
    free(ptr);
}

void operator delete[](void *ptr, size_t size, std::align_val_t al) noexcept {
    EnableGuard ena;
    if (ena) {
        global.on(AllocOp::DeleteArray, ptr, size, (size_t)al, __builtin_return_address(0));
    }
    free(ptr);
}

void operator delete(void *ptr, std::align_val_t al, std::nothrow_t const &) noexcept {
    EnableGuard ena;
    if (ena) {
        global.on(AllocOp::Delete, ptr, kNone, (size_t)al, __builtin_return_address(0));
    }
    free(ptr);
}

void operator delete[](void *ptr, std::align_val_t al, std::nothrow_t const &) noexcept {
    EnableGuard ena;
    if (ena) {
        global.on(AllocOp::DeleteArray, ptr, kNone, (size_t)al, __builtin_return_address(0));
    }
    free(ptr);
}

void *operator new(size_t size, std::align_val_t al) noexcept(false) {
    EnableGuard ena;
    void *ptr = aligned_alloc((size_t)al, size);
    if (ena) {
        global.on(AllocOp::New, ptr, size, (size_t)al, __builtin_return_address(0));
    }
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void *operator new[](size_t size, std::align_val_t al) noexcept(false) {
    EnableGuard ena;
    void *ptr = aligned_alloc((size_t)al, size);
    if (ena) {
        global.on(AllocOp::NewArray, ptr, size, (size_t)al, __builtin_return_address(0));
    }
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void *operator new(size_t size, std::align_val_t al, std::nothrow_t const &) noexcept {
    EnableGuard ena;
    void *ptr = aligned_alloc((size_t)al, size);
    if (ena) {
        global.on(AllocOp::New, ptr, size, (size_t)al, __builtin_return_address(0));
    }
    return ptr;
}

void *operator new[](size_t size, std::align_val_t al, std::nothrow_t const &) noexcept {
    EnableGuard ena;
    void *ptr = aligned_alloc((size_t)al, size);
    if (ena) {
        global.on(AllocOp::NewArray, ptr, size, (size_t)al, __builtin_return_address(0));
    }
    return ptr;
}
#endif