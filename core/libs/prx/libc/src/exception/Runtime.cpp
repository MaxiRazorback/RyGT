#include "prx/libc/include/exceptions/Runtime.hpp"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#endif

namespace LibcException {

// Writes straight to the stderr handle: terminate paths must not wait on the CRT stream lock,
// which another (possibly suspended) thread may hold.
static void RawLog(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    int length = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length <= 0) return;
    if (length > static_cast<int>(sizeof(buffer)) - 1) length = sizeof(buffer) - 1;
#ifdef _WIN32
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), buffer, static_cast<DWORD>(length), &written, nullptr);
#else
    std::fwrite(buffer, 1, length, stderr);
#endif
}

[[noreturn]] static void DefaultTerminate() {
    if (globals.caught && Native(globals.caught->unwind.exception_class)) {
        Header* primary = Primary(globals.caught);
        const char* typeName = primary->type ? primary->type->name() : nullptr;
        const char* what = nullptr;
        void* exception = primary->adjusted;
        if (exception && primary->type && Match(&typeid(std::exception), primary->type, exception)) {
            struct VtableLayout { std::ptrdiff_t offset; const void* type; void (*destroy)(void*); void (*del)(void*); const char* (*whatFn)(const void*); };
            const void* vtable = *static_cast<const void* const*>(exception);
            auto* layout = reinterpret_cast<const VtableLayout*>(static_cast<const char*>(vtable) - offsetof(VtableLayout, destroy));
#ifdef _WIN32
            if (!primary->_pad) {
                using GuestWhat = const char* (__attribute__((sysv_abi)) *)(const void*);
                what = reinterpret_cast<GuestWhat>(layout->whatFn)(exception);
            } else
#endif
            what = layout->whatFn(exception);
        }
        int status = 0;
        char* demangled = abi::__cxa_demangle(typeName, nullptr, nullptr, &status);
        const char* displayName = (status == 0 && demangled) ? demangled : typeName;
        if (what)
            RawLog("terminate called after throwing an instance of '%s'\n  what():  %s\n", displayName, what);
        else
            RawLog("terminate called after throwing an instance of '%s'\n", displayName);
        std::free(demangled);
    } else {
        RawLog("terminate called without an active exception\n");
    }
    std::abort();
}

inline std::atomic<void(*)()> terminateHandler {DefaultTerminate};
[[noreturn]] void InvokeTerminate(void (*handler)()) {
    try { if (handler) handler(); } catch (...) {}
    std::abort();
}
[[noreturn]] void Terminate() { InvokeTerminate(terminateHandler.load(std::memory_order_acquire)); }

_Unwind_Exception* UnwindOf(Header* header) {
    return Native(header->unwind.exception_class) ? &header->unwind : reinterpret_cast<_Unwind_Exception*>(header->landing);
}
void DeleteForeignHeader(Header* header) { header->~Header(); std::free(header); }

void Release(void* object) {
    if (!object) return;
    auto* header = FromObject(object);
    auto* allocation = AllocationOf(header);
    if (allocation->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        try {
            if (header->destructor) {
#ifdef _WIN32
                if (!header->_pad) {
                    using GuestDestructor = void (__attribute__((sysv_abi)) *)(void*);
                    reinterpret_cast<GuestDestructor>(header->destructor)(object);
                } else
#endif
                header->destructor(object);
            }
        }
        catch (...) { Terminate(); }
        allocation->~Allocation();
        std::free(allocation);
    }
}

void Cleanup(_Unwind_Reason_Code reason, _Unwind_Exception* exception) {
    if (reason != _URC_FOREIGN_EXCEPTION_CAUGHT) Terminate();
    auto* header = FromUnwind(exception);
    if (exception->exception_class == DependentClass) {
        Release(reinterpret_cast<void*>(header->type));
        header->~Header();
        std::free(header);
    } else Release(header + 1);
}

bool Equal(const std::type_info* a, const std::type_info* b) {
    return a == b || (a && b && std::strcmp(a->name(), b->name()) == 0);
}

const char* Kind(const std::type_info* type) {
    auto table = *reinterpret_cast<const std::type_info* const* const*>(type);
    return table[-1]->name();
}

struct Search {
    const std::type_info* wanted;
    void* found {};
    unsigned count {};
};

struct BaseLocation {
    void* object;
    std::uintptr_t identity;
};

template<class Visitor>
void VisitBases(const std::type_info* type, BaseLocation location, bool publicPath, Visitor& visitor, unsigned depth = 0) {
    if (!type) return;
    if (depth > 128) Terminate();
    visitor(type, location, publicPath);
    const char* kind = Kind(type);
    if (std::strcmp(kind, "N10__cxxabiv120__si_class_type_infoE") == 0) {
        auto* si = static_cast<const __cxxabiv1::__si_class_type_info*>(type);
        VisitBases(si->__base_type, location, publicPath, visitor, depth + 1);
    } else if (std::strcmp(kind, "N10__cxxabiv121__vmi_class_type_infoE") == 0) {
        auto* vmi = static_cast<const __cxxabiv1::__vmi_class_type_info*>(type);
        for (unsigned i = 0; i < vmi->__base_count; ++i) {
            const auto& base = vmi->__base_info[i];
            auto offset = base.__offset_flags >> 8;
            BaseLocation next = location;
            if (base.__offset_flags & 1) {
                if (location.object) {
                    auto* vtable = *static_cast<const unsigned char* const*>(location.object);
                    std::memcpy(&offset, vtable + offset, sizeof(offset));
                } else {
                    next.identity = reinterpret_cast<std::uintptr_t>(base.__base_type);
                    offset = 0;
                }
            }
            if (location.object) next.object = static_cast<unsigned char*>(location.object) + offset;
            else next.identity += offset;
            VisitBases(base.__base_type, next, publicPath && (base.__offset_flags & 2), visitor, depth + 1);
        }
    }
}

void Bases(const std::type_info* type, void* object, Search& search) {
    std::uintptr_t identity = 0;
    auto visitor = [&](const std::type_info* candidate, BaseLocation location, bool publicPath) {
        if (!publicPath || !Equal(candidate, search.wanted)) return;
        if (!search.count) { search.found = location.object; identity = location.identity; search.count = 1; }
        else if (search.found != location.object || identity != location.identity) search.count = 2;
    };
    VisitBases(type, {object, 0}, true, visitor);
}

bool PointerMatch(const __cxxabiv1::__pbase_type_info* caught, const __cxxabiv1::__pbase_type_info* thrown,
                  void*& object, unsigned depth, bool constPath) {
    if (depth > 64) return false;
    unsigned added = (caught->__flags & 7) & ~(thrown->__flags & 7);
    if (((thrown->__flags & 7) & ~(caught->__flags & 7)) || (depth && added && !constPath)) return false;
    if ((caught->__flags & 0x60) & ~(thrown->__flags & 0x60)) return false;
    if (Equal(caught->__pointee, thrown->__pointee)) return true;
    if (std::strcmp(Kind(caught->__pointee), "N10__cxxabiv119__pointer_type_infoE") == 0 &&
        std::strcmp(Kind(thrown->__pointee), "N10__cxxabiv119__pointer_type_infoE") == 0)
        return PointerMatch(static_cast<const __cxxabiv1::__pbase_type_info*>(caught->__pointee),
                            static_cast<const __cxxabiv1::__pbase_type_info*>(thrown->__pointee), object,
                            depth + 1, constPath && (caught->__flags & 1));
    if (depth) return false;
    if (std::strcmp(caught->__pointee->name(), "v") == 0 &&
        std::strcmp(Kind(thrown->__pointee), "N10__cxxabiv120__function_type_infoE") != 0) return true;
    Search search {caught->__pointee};
    Bases(thrown->__pointee, object, search);
    if (search.count != 1) return false;
    object = search.found;
    return true;
}

bool Match(const std::type_info* caught, const std::type_info* thrown, void*& object) {
    if (!caught || Equal(caught, thrown)) return true;
    if (!thrown) return false;
    const char* caughtKind = Kind(caught);
    const char* thrownKind = Kind(thrown);
    if (std::strcmp(caughtKind, "N10__cxxabiv119__pointer_type_infoE") == 0 &&
        std::strcmp(thrownKind, "N10__cxxabiv119__pointer_type_infoE") == 0) {
        return PointerMatch(static_cast<const __cxxabiv1::__pointer_type_info*>(caught),
                            static_cast<const __cxxabiv1::__pointer_type_info*>(thrown), object, 0, true);
    }
    if (std::strcmp(caughtKind, "N10__cxxabiv119__pointer_type_infoE") == 0 && std::strcmp(thrown->name(), "Dn") == 0) {
        object = nullptr;
        return true;
    }
    if (std::strcmp(caughtKind, "N10__cxxabiv129__pointer_to_member_type_infoE") == 0 &&
        std::strcmp(thrownKind, "N10__cxxabiv129__pointer_to_member_type_infoE") == 0) {
        auto* c = static_cast<const __cxxabiv1::__pointer_to_member_type_info*>(caught);
        auto* t = static_cast<const __cxxabiv1::__pointer_to_member_type_info*>(thrown);
        return Equal(c->__context, t->__context) && PointerMatch(c, t, object, 0, true);
    }
    Search search {caught};
    Bases(thrown, object, search);
    if (search.count == 1) { object = search.found; return true; }
    return false;
}

#ifdef _WIN32
// DIAG-162: the game's own C++ exceptions are unwound here, not by Windows, so the crash reporter's
// throw trace never sees them. GT Sport catches many of them and carries on, or quietly ends (a
// failed boot script returns from the main loop and the game shuts down). The first 40 are
// reported to stderr: type, any text the exception object leads to, and the executable's code
// addresses on the stack. APS5_TRACE_GUEST_THROW=1 reports every one, =0 none.
bool Readable(const void* address, std::size_t size) {
    MEMORY_BASIC_INFORMATION memory{};
    if (!address || VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory)) return false;
    if (memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const auto end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    return reinterpret_cast<std::uintptr_t>(address) + size <= end;
}

// Printable text at `address` (at least 4 characters, cut at 160), or nothing.
bool TextAt(std::uintptr_t address, char* out, std::size_t size) {
    std::size_t length = 0;
    for (; length + 1 < size && length < 160; ++length) {
        const auto* at = reinterpret_cast<const unsigned char*>(address + length);
        if (!Readable(at, 1)) break;
        if (*at == 0) break;
        if (*at < 0x20 || *at > 0x7e) { if (*at != '\n' && *at != '\t') return false; }
        out[length] = *at == '\n' || *at == '\t' ? ' ' : static_cast<char>(*at);
    }
    out[length] = 0;
    return length >= 4;
}

// Texts the exception object leads to: inline ones and those behind its first pointers, two levels
// deep (the game's exceptions hold a string object or a pointer to an object holding one).
void CollectTexts(std::uintptr_t object, int depth, char* out, std::size_t size, int& found) {
    if (found >= 3 || !Readable(reinterpret_cast<const void*>(object), 8 * 8)) return;
    char text[168];
    if (depth > 0 && TextAt(object + 8, text, sizeof(text)) && found < 3) {
        const auto used = std::strlen(out);
        std::snprintf(out + used, size - used, " \"%s\"", text);
        ++found;
    }
    for (int slot = 0; slot < 8 && found < 3; ++slot) {
        const auto value = reinterpret_cast<const std::uintptr_t*>(object)[slot];
        if (value < 0x10000 || (value & 7) != 0) continue;
        if (TextAt(value, text, sizeof(text))) {
            const auto used = std::strlen(out);
            std::snprintf(out + used, size - used, " \"%s\"", text);
            ++found;
        } else if (depth < 2) {
            CollectTexts(value, depth + 1, out, size, found);
        }
    }
}

void TraceGuestThrow(const void* object, const std::type_info* type, const void* thrower) {
    static const char* setting = std::getenv("APS5_TRACE_GUEST_THROW");
    static const bool all = setting != nullptr && setting[0] == '1';
    static const bool none = setting != nullptr && setting[0] == '0';
    static std::atomic<unsigned> reported{0};
    if (none) return;
    const unsigned number = reported.fetch_add(1);
    if (!all && number >= 40) return;
    char name[160] = "?";
    if (type && Readable(type, 16)) {
        const char* raw = reinterpret_cast<const char* const*>(type)[1];
        if (raw && *raw == '*') ++raw;
        if (!raw || !TextAt(reinterpret_cast<std::uintptr_t>(raw), name, sizeof(name))) std::strcpy(name, "?");
    }
    char texts[640] = "";
    int found = 0;
    auto start = reinterpret_cast<std::uintptr_t>(object);
    // A thrown pointer: look at what it points to.
    if (name[0] == 'P' && Readable(object, 8)) start = *static_cast<const std::uintptr_t*>(object);
    CollectTexts(start, 0, texts, sizeof(texts), found);
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
    const auto imageEnd = image + headers->OptionalHeader.SizeOfImage;
    auto inImage = [&](std::uintptr_t value) { return value >= image && value < imageEnd; };
    char stack[400] = "";
    int frames = 0;
    const auto top = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(top), &region, sizeof(region)) != sizeof(region)) region.RegionSize = 0;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
    for (std::uintptr_t slot = top; frames < 20 && slot + 8 <= regionEnd && slot < top + 0x4000; slot += 8) {
        const auto value = *reinterpret_cast<const std::uintptr_t*>(slot);
        MEMORY_BASIC_INFORMATION memory{};
        if (!inImage(value) || VirtualQuery(reinterpret_cast<const void*>(value), &memory, sizeof(memory)) != sizeof(memory) ||
            (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) continue;
        const auto used = std::strlen(stack);
        std::snprintf(stack + used, sizeof(stack) - used, " +0x%llx", static_cast<unsigned long long>(value - image));
        ++frames;
    }
    const auto from = reinterpret_cast<std::uintptr_t>(thrower);
    RawLog("[guest-throw] #%u thread %lu %s from %s+0x%llx;%s; stack:%s\n", number + 1, GetCurrentThreadId(), name,
           inImage(from) ? "GTSport.exe" : "?", static_cast<unsigned long long>(inImage(from) ? from - image : from),
           texts, stack);
}
#endif
}

extern "C" {
void* APS5_VABI __cxa_allocate_exception_nid_postfix(std::size_t size) {
    using namespace LibcException;
    if (size > std::numeric_limits<std::size_t>::max() - sizeof(Allocation)) Terminate();
    void* storage = std::malloc(sizeof(Allocation) + size);
    if (!storage) Terminate();
    static_assert(offsetof(Allocation, header) + sizeof(Header) == sizeof(Allocation));
    auto* allocation = new (storage) Allocation;
    return &allocation->header + 1;
}
void APS5_VABI __cxa_free_exception_nid_postfix(void* object) {
    if (!object) return;
    auto* allocation = LibcException::AllocationOf(LibcException::FromObject(object));
    allocation->~Allocation();
    std::free(allocation);
}

[[noreturn]] void APS5_VABI __cxa_throw_nid_postfix(void* object, std::type_info* type, void (*destructor)(void*)) {
    using namespace LibcException;
#ifdef _WIN32
    TraceGuestThrow(object, type, __builtin_return_address(0)); // DIAG-162
#endif
    auto* header = FromObject(object);
    header->type = type;
    header->destructor = destructor;
    header->terminate = terminateHandler.load(std::memory_order_acquire);
    header->adjusted = object;
    header->unwind.exception_class = PrimaryClass;
    header->unwind.exception_cleanup = Cleanup;
    ++globals.uncaught;
    _Unwind_RaiseException_nid_postfix(&header->unwind);
    RawLog("[libc] unhandled exception '%s' thrown from %p\n", type ? type->name() : "?", __builtin_return_address(0));
    __cxa_begin_catch_nid_postfix(&header->unwind);
    InvokeTerminate(header->terminate);
}

LibcException::Header* __cxa_init_primary_exception_nid_postfix(void* object, std::type_info* type, void (*destructor)(void*)) {
    using namespace LibcException;
    auto* header = FromObject(object);
    header->type = type;
    header->destructor = destructor;
    header->terminate = terminateHandler.load(std::memory_order_acquire);
    header->adjusted = object;
    header->unwind.exception_class = PrimaryClass;
    header->unwind.exception_cleanup = Cleanup;
    return header;
}

void* APS5_VABI __cxa_begin_catch_nid_postfix(void* exception) {
    using namespace LibcException;
    auto* unwind = static_cast<_Unwind_Exception*>(exception);
    if (!unwind) Terminate();
    Header* header;
    if (Native(unwind->exception_class)) {
        header = FromUnwind(unwind);
        if (globals.uncaught) --globals.uncaught;
    } else if (globals.caught && UnwindOf(globals.caught) == unwind) {
        header = globals.caught;
    } else {
        void* storage = std::malloc(sizeof(Header));
        if (!storage) Terminate();
        header = new (storage) Header;
        header->unwind.exception_class = unwind->exception_class;
        header->landing = reinterpret_cast<std::uintptr_t>(unwind);
        header->adjusted = unwind + 1;
    }
    header->handlers = header->handlers < 0 ? -header->handlers + 1 : header->handlers + 1;
    if (globals.caught != header) { header->next = globals.caught; globals.caught = header; }
    return header->adjusted;
}

void APS5_VABI __cxa_end_catch_nid_postfix() {
    using namespace LibcException;
    auto* header = globals.caught;
    if (!header) return;
    if (header->handlers < 0) {
        if (++header->handlers == 0) {
            globals.caught = header->next;
            if (!Native(header->unwind.exception_class)) DeleteForeignHeader(header);
        }
    } else if (--header->handlers == 0) {
        globals.caught = header->next;
        bool foreign = !Native(header->unwind.exception_class);
        _Unwind_DeleteException_nid_postfix(UnwindOf(header));
        if (foreign) DeleteForeignHeader(header);
    }
}

[[noreturn]] void APS5_VABI __cxa_rethrow_nid_postfix() {
    using namespace LibcException;
    auto* header = globals.caught;
    if (!header) Terminate();
    header->handlers = -header->handlers;
    if (Native(header->unwind.exception_class)) ++globals.uncaught;
    _Unwind_RaiseException_nid_postfix(UnwindOf(header));
    __cxa_begin_catch_nid_postfix(UnwindOf(header));
    InvokeTerminate(header->terminate ? header->terminate : terminateHandler.load());
}

void* APS5_VABI __cxa_get_exception_ptr_nid_postfix(void* exception) {
    auto* unwind = static_cast<_Unwind_Exception*>(exception);
    return LibcException::Native(unwind->exception_class) ? LibcException::FromUnwind(unwind)->adjusted : unwind + 1;
}
std::type_info* APS5_VABI __cxa_current_exception_type_nid_postfix() {
    using namespace LibcException;
    return globals.caught && Native(globals.caught->unwind.exception_class) ? Primary(globals.caught)->type : nullptr;
}
void* APS5_VABI __cxa_get_globals_nid_postfix() { return &LibcException::globals; }
void* APS5_VABI __cxa_get_globals_fast_nid_postfix() { return &LibcException::globals; }
bool APS5_VABI __cxa_uncaught_exception_nid_postfix() { return LibcException::globals.uncaught != 0; }
unsigned APS5_VABI __cxa_uncaught_exceptions_nid_postfix() { return LibcException::globals.uncaught; }
bool APS5_VABI _ZSt18uncaught_exceptionv_nid_postfix() { return __cxa_uncaught_exception_nid_postfix(); }
int APS5_VABI _ZSt19uncaught_exceptionsv_nid_postfix() { return static_cast<int>(__cxa_uncaught_exceptions_nid_postfix()); }
[[noreturn]] void APS5_VABI _ZSt9terminatev_nid_postfix() { LibcException::Terminate(); }
using LibcTerminateHandler = void(*)();
LibcTerminateHandler APS5_VABI _ZSt13set_terminatePFvvE_nid_postfix(LibcTerminateHandler handler) {
    return LibcException::terminateHandler.exchange(handler ? handler : std::abort);
}
LibcTerminateHandler APS5_VABI _ZSt13get_terminatev_nid_postfix() { return LibcException::terminateHandler.load(); }

void APS5_VABI __cxa_increment_exception_refcount_nid_postfix(void* object) {
    if (object) LibcException::AllocationOf(LibcException::FromObject(object))->references.fetch_add(1, std::memory_order_relaxed);
}
void APS5_VABI __cxa_decrement_exception_refcount_nid_postfix(void* object) { LibcException::Release(object); }
void* APS5_VABI __cxa_current_primary_exception_nid_postfix() {
    using namespace LibcException;
    if (!globals.caught || !Native(globals.caught->unwind.exception_class)) return nullptr;
    void* object = Primary(globals.caught) + 1;
    __cxa_increment_exception_refcount_nid_postfix(object);
    return object;
}
void* APS5_VABI __cxa_allocate_dependent_exception_nid_postfix() {
    void* storage = std::malloc(sizeof(LibcException::Header));
    if (!storage) LibcException::Terminate();
    return new (storage) LibcException::Header;
}
void APS5_VABI __cxa_free_dependent_exception_nid_postfix(void* storage) {
    if (!storage) return;
    static_cast<LibcException::Header*>(storage)->~Header();
    std::free(storage);
}
void APS5_VABI __cxa_rethrow_primary_exception_nid_postfix(void* object) {
    using namespace LibcException;
    if (!object) return;
    auto* header = static_cast<Header*>(__cxa_allocate_dependent_exception_nid_postfix());
    header->type = reinterpret_cast<std::type_info*>(object);
    header->adjusted = object;
    header->terminate = terminateHandler.load();
    header->unwind.exception_class = DependentClass;
    header->unwind.exception_cleanup = Cleanup;
    __cxa_increment_exception_refcount_nid_postfix(object);
    ++globals.uncaught;
    _Unwind_RaiseException_nid_postfix(&header->unwind);
    __cxa_begin_catch_nid_postfix(&header->unwind);
    InvokeTerminate(header->terminate);
}

void* __dynamic_cast_nid_postfix(const void* source, const __cxxabiv1::__class_type_info* sourceType,
                               const __cxxabiv1::__class_type_info* destinationType, std::ptrdiff_t) {
    using namespace LibcException;
    if (!source) return nullptr;
    auto* vtable = *static_cast<const std::uintptr_t* const*>(source);
    auto* complete = const_cast<unsigned char*>(static_cast<const unsigned char*>(source)) + static_cast<std::intptr_t>(vtable[-2]);
    auto* dynamicType = reinterpret_cast<const std::type_info*>(vtable[-1]);
    Search downcastTarget {destinationType};
    auto visitor = [&](const std::type_info* candidate, BaseLocation location, bool) {
        if (!Equal(candidate, destinationType)) return;
        bool containsSource = false;
        auto sourceVisitor = [&](const std::type_info* base, BaseLocation baseLocation, bool publicPath) {
            if (publicPath && Equal(base, sourceType) && baseLocation.object == source) containsSource = true;
        };
        VisitBases(candidate, location, true, sourceVisitor);
        if (!containsSource) return;
        if (!downcastTarget.count) { downcastTarget.found = location.object; downcastTarget.count = 1; }
        else if (downcastTarget.found != location.object) downcastTarget.count = 2;
    };
    VisitBases(dynamicType, {complete, 0}, true, visitor);
    if (downcastTarget.count == 1) return downcastTarget.found;
    bool sourceIsPublic = false;
    auto sourceVisitor = [&](const std::type_info* candidate, BaseLocation location, bool publicPath) {
        if (publicPath && Equal(candidate, sourceType) && location.object == source) sourceIsPublic = true;
    };
    VisitBases(dynamicType, {complete, 0}, true, sourceVisitor);
    if (!sourceIsPublic) return nullptr;
    Search publicTarget {destinationType};
    Bases(dynamicType, complete, publicTarget);
    return publicTarget.count == 1 ? publicTarget.found : nullptr;
}

[[noreturn]] void APS5_VABI __cxa_call_terminate_nid_postfix(void* exception) {
    __cxa_begin_catch_nid_postfix(exception);
    LibcException::Terminate();
}
[[noreturn]] void APS5_VABI __cxa_pure_virtual_nid_postfix() { LibcException::Terminate(); }
[[noreturn]] void APS5_VABI __cxa_deleted_virtual_nid_postfix() { LibcException::Terminate(); }
}
