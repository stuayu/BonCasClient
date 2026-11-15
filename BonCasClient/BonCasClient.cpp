#include "stdafx.h"
// Keep SDK dllimport declarations separate from our PCI data definitions.
#define g_rgSCardT0Pci SDK_g_rgSCardT0Pci
#define g_rgSCardT1Pci SDK_g_rgSCardT1Pci
#define g_rgSCardRawPci SDK_g_rgSCardRawPci
#include <winscard.h>
#undef g_rgSCardT0Pci
#undef g_rgSCardT1Pci
#undef g_rgSCardRawPci
#include "CasProxy.h"
#include "../Common/ReaderTableCore.h"
#include <condition_variable>
#include <mutex>
#include <map>
#include <set>
#include <memory>
#include <algorithm>
#include <cstring>
#include <cstddef>
#include <chrono>
#include <cwchar>
#include <limits>
#include <utility>

extern "C" {
extern IMAGE_DOS_HEADER __ImageBase;
static_assert(sizeof(SCARD_IO_REQUEST) == 8, "Unexpected PCI layout");
SCARD_IO_REQUEST g_rgSCardT0Pci = {SCARD_PROTOCOL_T0, 8};
SCARD_IO_REQUEST g_rgSCardT1Pci = {SCARD_PROTOCOL_T1, 8};
SCARD_IO_REQUEST g_rgSCardRawPci = {SCARD_PROTOCOL_RAW, 8};
}
DWORD g_dwIP = 0x7F000001UL;
WORD g_wPort = 6900;
namespace {
std::once_flag initFlag;
std::mutex lock;
std::condition_variable contextReleaseChanged;
std::condition_variable extraReaderCallChanged;
std::condition_variable statusChangeFinished;
std::condition_variable localCardReleaseChanged;
ReaderTableCore::Config<wchar_t> config;
struct ContextIdentity {};
using ContextIdentityPtr = std::shared_ptr<const ContextIdentity>;
std::map<SCARDCONTEXT, ContextIdentityPtr> contextIdentities;
struct Card {
    std::unique_ptr<CCasProxy> proxy;
    std::wstring name;
    SCARDCONTEXT context;
    std::mutex ioLock;
};
std::map<SCARDHANDLE, std::shared_ptr<Card> > cards;
std::set<SCARDCONTEXT> contexts;
std::set<SCARDCONTEXT> virtualContexts;
std::set<SCARDCONTEXT> releasingContexts;
std::map<SCARDCONTEXT, std::size_t> activeExtraReaderCalls;
std::map<SCARDCONTEXT, std::size_t> activeStatusChanges;
std::set<SCARDHANDLE> releasingLocalCards;
std::map<SCARDHANDLE, SCARDCONTEXT> localCards;
struct LocalCardIdentity {};
using LocalCardIdentityPtr = std::shared_ptr<const LocalCardIdentity>;
std::map<SCARDHANDLE, LocalCardIdentityPtr> localCardIdentities;
std::map<const void*, SCARDCONTEXT> allocations;
SCARDCONTEXT nextContext = (std::numeric_limits<SCARDCONTEXT>::max)() - 1;
SCARDHANDLE nextCard = 1;
HMODULE systemModule = nullptr;
HMODULE extraModule = nullptr;
std::map<SCARDCONTEXT, SCARDCONTEXT> extraContexts;
decltype(&::SCardEstablishContext) extraEstablishContext = nullptr;
decltype(&::SCardReleaseContext) extraReleaseContext = nullptr;
decltype(&::SCardListReadersA) extraListReadersA = nullptr;
decltype(&::SCardListReadersW) extraListReadersW = nullptr;
decltype(&::SCardEstablishContext) osEstablishContext = nullptr;
decltype(&::SCardReleaseContext) osReleaseContext = nullptr;
decltype(&::SCardListReadersA) osListReadersA = nullptr;
decltype(&::SCardListReadersW) osListReadersW = nullptr;
decltype(&::SCardConnectA) osConnectA = nullptr;
decltype(&::SCardConnectW) osConnectW = nullptr;
decltype(&::SCardTransmit) osTransmit = nullptr;
decltype(&::SCardDisconnect) osDisconnect = nullptr;
decltype(&::SCardFreeMemory) osFreeMemory = nullptr;
decltype(&::SCardStatusA) osStatusA = nullptr;
decltype(&::SCardStatusW) osStatusW = nullptr;
decltype(&::SCardGetStatusChangeA) osGetStatusChangeA = nullptr;
decltype(&::SCardGetStatusChangeW) osGetStatusChangeW = nullptr;
decltype(&::SCardBeginTransaction) osBeginTransaction = nullptr;
decltype(&::SCardEndTransaction) osEndTransaction = nullptr;
decltype(&::SCardReconnect) osReconnect = nullptr;
decltype(&::SCardIsValidContext) osIsValidContext = nullptr;
decltype(&::SCardCancel) osCancel = nullptr;
decltype(&::SCardGetAttrib) osGetAttrib = nullptr;
decltype(&::SCardControl) osControl = nullptr;

SCARDCONTEXT createVirtualContext() {
    while (nextContext && (contexts.count(nextContext) || virtualContexts.count(nextContext) ||
                           releasingContexts.count(nextContext))) --nextContext;
    if (!nextContext) throw std::bad_alloc();
    const SCARDCONTEXT context = nextContext--;
    virtualContexts.insert(context);
    try {
        contextIdentities.emplace(context, ContextIdentityPtr(new ContextIdentity));
    } catch (...) {
        virtualContexts.erase(context);
        throw;
    }
    return context;
}
bool validContext(SCARDCONTEXT context) {
    return virtualContexts.count(context) || (contexts.count(context) && !releasingContexts.count(context));
}
ContextIdentityPtr contextIdentity(SCARDCONTEXT context) {
    auto it = contextIdentities.find(context);
    return it == contextIdentities.end() ? ContextIdentityPtr() : it->second;
}
bool sameContext(SCARDCONTEXT context, const ContextIdentityPtr& identity) {
    auto it = contextIdentities.find(context);
    return validContext(context) && identity && it != contextIdentities.end() && it->second == identity;
}
bool settledContext(SCARDCONTEXT context, const ContextIdentityPtr& identity,
    std::unique_lock<std::mutex>& guard) {
    while (releasingContexts.count(context) && contextIdentity(context) == identity)
        contextReleaseChanged.wait(guard);
    return sameContext(context, identity);
}
void waitForLocalCardRelease(SCARDHANDLE handle, std::unique_lock<std::mutex>& guard) {
    while (releasingLocalCards.count(handle)) localCardReleaseChanged.wait(guard);
}
void finishExtraReaderCall(SCARDCONTEXT context) {
    auto it = activeExtraReaderCalls.find(context);
    if (it != activeExtraReaderCalls.end() && --it->second == 0) activeExtraReaderCalls.erase(it);
    extraReaderCallChanged.notify_all();
}
class ExtraReaderCall {
public:
    ExtraReaderCall(SCARDCONTEXT context, std::unique_lock<std::mutex>& guard)
        : context_(context), guard_(guard), active_(true) {
        ++activeExtraReaderCalls[context_];
    }
    ~ExtraReaderCall() { finish(); }
    void finish() {
        if (!active_) return;
        if (!guard_.owns_lock()) guard_.lock();
        finishExtraReaderCall(context_);
        active_ = false;
    }
private:
    SCARDCONTEXT context_;
    std::unique_lock<std::mutex>& guard_;
    bool active_;
};
void finishStatusChange(SCARDCONTEXT context) {
    auto it = activeStatusChanges.find(context);
    if (it != activeStatusChanges.end() && --it->second == 0) activeStatusChanges.erase(it);
    statusChangeFinished.notify_all();
}
class StatusChangeCall {
public:
    StatusChangeCall(SCARDCONTEXT context, std::unique_lock<std::mutex>& guard)
        : context_(context), guard_(guard), active_(true) {
        ++activeStatusChanges[context_];
    }
    ~StatusChangeCall() { finish(); }
    void finish() {
        if (!active_) return;
        if (!guard_.owns_lock()) guard_.lock();
        finishStatusChange(context_);
        active_ = false;
    }
private:
    SCARDCONTEXT context_;
    std::unique_lock<std::mutex>& guard_;
    bool active_;
};
bool waitForLocalCardContext(SCARDHANDLE handle, std::unique_lock<std::mutex>& guard) {
    auto it = localCards.find(handle);
    if (it == localCards.end()) return true;
    const SCARDCONTEXT context = it->second;
    const ContextIdentityPtr identity = contextIdentity(context);
    if (!identity) return false;
    while (releasingContexts.count(context) && contextIdentity(context) == identity)
        contextReleaseChanged.wait(guard);
    it = localCards.find(handle);
    if (it == localCards.end() || it->second != context || !sameContext(context, identity)) return false;
    return true;
}
void waitForLocalCardHandleRelease(SCARDHANDLE handle, std::unique_lock<std::mutex>& guard) {
    for (;;) {
        waitForLocalCardRelease(handle, guard);
        auto it = localCards.find(handle);
        if (it == localCards.end()) return;
        const SCARDCONTEXT context = it->second;
        const ContextIdentityPtr identity = contextIdentity(context);
        if (!identity) return;
        if (!releasingContexts.count(context) || contextIdentity(context) != identity) return;
        contextReleaseChanged.wait(guard);
    }
}
void releaseVirtualResources(SCARDCONTEXT context) {
    contextIdentities.erase(context);
    for (auto it = cards.begin(); it != cards.end();) {
        if (it->second->context == context) it = cards.erase(it);
        else ++it;
    }
    for (auto it = localCards.begin(); it != localCards.end();) {
        if (it->second == context) {
            localCardIdentities.erase(it->first);
            it = localCards.erase(it);
        }
        else ++it;
    }
    for (auto it = allocations.begin(); it != allocations.end();) {
        if (it->second == context) {
            delete[] static_cast<const unsigned char*>(it->first);
            it = allocations.erase(it);
        } else ++it;
    }
}
DWORD ipValue(const std::wstring& host) {
    unsigned long parts[4] = {};
    std::size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        auto end = host.find(L'.', start);
        ReaderTableCore::number(host.substr(start, end == std::wstring::npos ? end : end-start), parts[i], 255);
        start = end + 1;
    }
    return (parts[0]<<24) | (parts[1]<<16) | (parts[2]<<8) | parts[3];
}
void initialize() {
    wchar_t path[MAX_PATH] = {};
    std::wstring ini;
    DWORD n = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
    if (n && n < MAX_PATH) {
        ini.assign(path, n);
        auto slash = ini.find_last_of(L"\\/");
        auto dot = ini.find_last_of(L'.');
        if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) ini.resize(dot);
        ini += L".ini";
    }
    wchar_t host[128] = {};
    GetPrivateProfileStringW(L"Server", L"IP", L"127.0.0.1", host, 128, ini.c_str());
    if (!ReaderTableCore::ipv4(std::wstring(host))) wcscpy_s(host, L"127.0.0.1");
    UINT port = GetPrivateProfileIntW(L"Server", L"Port", 6900, ini.c_str());
    if (port < 1 || port > 65535) port = 6900;
    std::vector<wchar_t> sections(256), entries(256);
    while (GetPrivateProfileSectionNamesW(sections.data(), static_cast<DWORD>(sections.size()), ini.c_str()) == sections.size()-2) {
        if (sections.size() >= 1048576) throw std::bad_alloc();
        sections.resize(sections.size()*2);
    }
    bool hasReaders = false;
    for (const wchar_t* p = sections.data(); *p; p += wcslen(p)+1) if (_wcsicmp(p, L"Readers") == 0) hasReaders = true;
    while (GetPrivateProfileSectionW(L"Readers", entries.data(), static_cast<DWORD>(entries.size()), ini.c_str()) == entries.size()-2) {
        if (entries.size() >= 1048576) throw std::bad_alloc();
        entries.resize(entries.size()*2);
    }
    std::vector<std::wstring> lines;
    for (const wchar_t* p = entries.data(); *p; p += wcslen(p)+1) lines.push_back(p);
    config = ReaderTableCore::parse(lines, std::wstring(host), static_cast<WORD>(port), hasReaders,
        GetPrivateProfileIntW(L"Local", L"Enable", 0, ini.c_str()) == 1);
    // Preserve the legacy SetConfig endpoint for the single-reader mode.
    if (!hasReaders && g_dwIP == 0x7F000001UL && g_wPort == 6900) {
        g_dwIP = ipValue(host); g_wPort = static_cast<WORD>(port);
    }
    if (!config.localEnabled) return;
    wchar_t systemPath[MAX_PATH] = {};
    n = GetSystemDirectoryW(systemPath, MAX_PATH);
    if (!n || n >= MAX_PATH-14) return;
    std::wstring dll(systemPath, n);
    dll += L"\\winscard.dll";
    HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) return;
    if (module == reinterpret_cast<HMODULE>(&__ImageBase)) { FreeLibrary(module); return; }
    osEstablishContext = reinterpret_cast<decltype(osEstablishContext)>(GetProcAddress(module, "SCardEstablishContext"));
    osReleaseContext = reinterpret_cast<decltype(osReleaseContext)>(GetProcAddress(module, "SCardReleaseContext"));
    osListReadersA = reinterpret_cast<decltype(osListReadersA)>(GetProcAddress(module, "SCardListReadersA"));
    osListReadersW = reinterpret_cast<decltype(osListReadersW)>(GetProcAddress(module, "SCardListReadersW"));
    osConnectA = reinterpret_cast<decltype(osConnectA)>(GetProcAddress(module, "SCardConnectA"));
    osConnectW = reinterpret_cast<decltype(osConnectW)>(GetProcAddress(module, "SCardConnectW"));
    osTransmit = reinterpret_cast<decltype(osTransmit)>(GetProcAddress(module, "SCardTransmit"));
    osDisconnect = reinterpret_cast<decltype(osDisconnect)>(GetProcAddress(module, "SCardDisconnect"));
    osFreeMemory = reinterpret_cast<decltype(osFreeMemory)>(GetProcAddress(module, "SCardFreeMemory"));
    osStatusA = reinterpret_cast<decltype(osStatusA)>(GetProcAddress(module, "SCardStatusA"));
    osStatusW = reinterpret_cast<decltype(osStatusW)>(GetProcAddress(module, "SCardStatusW"));
    osGetStatusChangeA = reinterpret_cast<decltype(osGetStatusChangeA)>(GetProcAddress(module, "SCardGetStatusChangeA"));
    osGetStatusChangeW = reinterpret_cast<decltype(osGetStatusChangeW)>(GetProcAddress(module, "SCardGetStatusChangeW"));
    osBeginTransaction = reinterpret_cast<decltype(osBeginTransaction)>(GetProcAddress(module, "SCardBeginTransaction"));
    osEndTransaction = reinterpret_cast<decltype(osEndTransaction)>(GetProcAddress(module, "SCardEndTransaction"));
    osReconnect = reinterpret_cast<decltype(osReconnect)>(GetProcAddress(module, "SCardReconnect"));
    osIsValidContext = reinterpret_cast<decltype(osIsValidContext)>(GetProcAddress(module, "SCardIsValidContext"));
    osCancel = reinterpret_cast<decltype(osCancel)>(GetProcAddress(module, "SCardCancel"));
    osGetAttrib = reinterpret_cast<decltype(osGetAttrib)>(GetProcAddress(module, "SCardGetAttrib"));
    osControl = reinterpret_cast<decltype(osControl)>(GetProcAddress(module, "SCardControl"));
    if (!osEstablishContext || !osReleaseContext || !osListReadersA || !osListReadersW || !osConnectA || !osConnectW || !osTransmit || !osDisconnect || !osFreeMemory || !osStatusA || !osStatusW || !osGetStatusChangeA || !osGetStatusChangeW || !osBeginTransaction || !osEndTransaction || !osReconnect || !osIsValidContext || !osCancel || !osGetAttrib || !osControl) {
        osEstablishContext = nullptr;
        osReleaseContext = nullptr;
        osListReadersA = nullptr;
        osListReadersW = nullptr;
        osConnectA = nullptr;
        osConnectW = nullptr;
        osTransmit = nullptr;
        osDisconnect = nullptr;
        osFreeMemory = nullptr;
        osStatusA = nullptr;
        osStatusW = nullptr;
        osGetStatusChangeA = nullptr;
        osGetStatusChangeW = nullptr;
        osBeginTransaction = nullptr;
        osEndTransaction = nullptr;
        osReconnect = nullptr;
        osIsValidContext = nullptr;
        osCancel = nullptr;
        osGetAttrib = nullptr;
        osControl = nullptr;
        FreeLibrary(module); return;
    }
    // Retained for the DLL lifetime; never unload while another call uses it.
    systemModule = module;
    wchar_t extraPath[MAX_PATH] = {};
    GetPrivateProfileStringW(L"Local", L"Path", L"", extraPath, MAX_PATH, ini.c_str());
    if (extraPath[0] && _wcsicmp(extraPath, dll.c_str()) != 0) {
        HMODULE extra = LoadLibraryExW(extraPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (extra && extra != reinterpret_cast<HMODULE>(&__ImageBase)) {
            auto establish = reinterpret_cast<decltype(extraEstablishContext)>(GetProcAddress(extra, "SCardEstablishContext"));
            auto release = reinterpret_cast<decltype(extraReleaseContext)>(GetProcAddress(extra, "SCardReleaseContext"));
            auto listA = reinterpret_cast<decltype(extraListReadersA)>(GetProcAddress(extra, "SCardListReadersA"));
            auto listW = reinterpret_cast<decltype(extraListReadersW)>(GetProcAddress(extra, "SCardListReadersW"));
            if (establish && release && listA && listW) {
                extraModule = extra;
                extraEstablishContext = establish;
                extraReleaseContext = release;
                extraListReadersA = listA;
                extraListReadersW = listW;
            } else FreeLibrary(extra);
        }
    }
}
void ensure() { std::call_once(initFlag, initialize); }
std::wstring wide(const wchar_t* p) { return p ? std::wstring(p) : std::wstring(); }
std::wstring wide(const char* p) {
    if (!p) return std::wstring();
    int n = MultiByteToWideChar(CP_ACP, 0, p, -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::vector<wchar_t> out(n);
    MultiByteToWideChar(CP_ACP, 0, p, -1, out.data(), n);
    return std::wstring(out.data());
}
std::string narrow(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::vector<char> out(n);
    WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
    return std::string(out.data());
}
// Own all returned memory, including Status strings. Register before publishing.
template<class C> LONG output(const std::basic_string<C>& data, C* dst, LPDWORD count, SCARDCONTEXT context) {
    if (!count) return SCARD_E_INVALID_PARAMETER;
    DWORD needed = static_cast<DWORD>(data.size());
    DWORD capacity = *count;
    *count = needed;
    if (!dst) return SCARD_S_SUCCESS;
    if (capacity == SCARD_AUTOALLOCATE) {
        std::unique_ptr<unsigned char[]> mem(new unsigned char[needed * sizeof(C)]);
        std::memcpy(mem.get(), data.data(), needed * sizeof(C));
        allocations.emplace(mem.get(), context);
        *reinterpret_cast<C**>(dst) = reinterpret_cast<C*>(mem.release());
        return SCARD_S_SUCCESS;
    }
    if (capacity < needed) return SCARD_E_INSUFFICIENT_BUFFER;
    std::copy(data.begin(), data.end(), dst);
    return SCARD_S_SUCCESS;
}
template<class C, class F> LONG list(SCARDCONTEXT context, const C* groups, C* dst, LPDWORD count, F fn, F extraFn, std::unique_lock<std::mutex>& guard) {
    if (!count) return SCARD_E_INVALID_PARAMETER;
    const ContextIdentityPtr identity = contextIdentity(context);
    if (!identity || !settledContext(context, identity, guard)) return SCARD_E_INVALID_HANDLE;
    std::vector<std::basic_string<C> > names;
    for (const auto& r : config.readers) {
        std::basic_string<C> name;
        if (sizeof(C) == sizeof(wchar_t)) name.assign(r.name.begin(), r.name.end());
        else { auto a = narrow(r.name); name.assign(a.begin(), a.end()); }
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    if ((fn || extraFn) && contexts.count(context)) {
        auto extraIt = extraContexts.find(context);
        const bool hasExtraContext = extraIt != extraContexts.end();
        const SCARDCONTEXT extraContext = hasExtraContext ? extraIt->second : 0;
        std::unique_ptr<ExtraReaderCall> extraOperation;
        if (hasExtraContext && extraFn) extraOperation.reset(new ExtraReaderCall(context, guard));
        guard.unlock();
        F sources[2] = {fn, extraFn};
        for (int source = 0; source < 2; ++source) {
            if (!sources[source]) continue;
            SCARDCONTEXT sourceContext = context;
            if (source == 1) {
                if (!hasExtraContext) continue;
                sourceContext = extraContext;
            }
            DWORD n = 0;
            if (sources[source](sourceContext, groups, nullptr, &n) == SCARD_S_SUCCESS && n > 0) {
                std::vector<C> local(n);
                if (sources[source](sourceContext, groups, local.data(), &n) == SCARD_S_SUCCESS && n <= local.size()) {
                    std::size_t pos = 0;
                    while (pos < n && local[pos]) {
                        auto end = pos;
                        while (end < n && local[end]) ++end;
                        if (end == n) break;
                        std::basic_string<C> name(local.data()+pos, end-pos);
                        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
                        pos = end+1;
                    }
                }
            }
        }
        if (!guard.owns_lock()) guard.lock();
        if (extraOperation) extraOperation->finish();
    }
    if (!guard.owns_lock()) guard.lock();
    if (!settledContext(context, identity, guard)) return SCARD_E_INVALID_HANDLE;
    return output(ReaderTableCore::multiString(names), dst, count, context);
}
LONG connectVirtual(const ReaderTableCore::Reader<wchar_t>& reader, SCARDCONTEXT context,
    LPSCARDHANDLE result, LPDWORD protocol, std::unique_lock<std::mutex>& guard,
    const ContextIdentityPtr& identity) {
    std::shared_ptr<Card> card(new Card);
    card->context = context;
    card->name = reader.name;
    card->proxy.reset(new CCasProxy(nullptr));
    const DWORD ip = config.explicitReaders ? ipValue(reader.host) : g_dwIP;
    const WORD port = config.explicitReaders ? reader.port : g_wPort;
    card->proxy->Setting(ip, port);
    guard.unlock();
    BOOL connected = FALSE;
    try {
        connected = card->proxy->Connect();
    } catch (...) {
        guard.lock();
        throw;
    }
    guard.lock();
    if (!settledContext(context, identity, guard)) return SCARD_E_INVALID_HANDLE;
    if (!connected) return SCARD_E_READER_UNAVAILABLE;
    // Tokens never expose pointers and are never reused. Avoid known OS handles.
    if (nextCard == 0 || nextCard == (std::numeric_limits<SCARDHANDLE>::max)()) return SCARD_E_NO_MEMORY;
    while (localCards.count(nextCard)) {
        if (++nextCard == 0 || nextCard == (std::numeric_limits<SCARDHANDLE>::max)()) return SCARD_E_NO_MEMORY;
    }
    SCARDHANDLE handle = nextCard++;
    cards.emplace(handle, card);
    *result = handle;
    if (protocol) *protocol = SCARD_PROTOCOL_T1;
    return SCARD_S_SUCCESS;
}
template<class C, class F> LONG connect(SCARDCONTEXT context, const C* name, const std::wstring& wname,
    DWORD share, DWORD protocols, LPSCARDHANDLE result, LPDWORD active, F fn, std::unique_lock<std::mutex>& guard) {
    if (!result) return SCARD_E_INVALID_PARAMETER;
    *result = 0;
    const ContextIdentityPtr identity = contextIdentity(context);
    if (!identity || !settledContext(context, identity, guard)) return SCARD_E_INVALID_HANDLE;
    auto r = ReaderTableCore::find(config, name ? wname.c_str() : nullptr);
    if (r) return connectVirtual(*r, context, result, active, guard, identity);
    if (name && fn && contexts.count(context)) {
        guard.unlock();
        LONG rc = fn(context, name, share, protocols, result, active);
        guard.lock();
        if (rc == SCARD_S_SUCCESS) waitForLocalCardHandleRelease(*result, guard);
        if (!settledContext(context, identity, guard)) {
            *result = 0;
            return SCARD_E_INVALID_HANDLE;
        }
        if (rc == SCARD_S_SUCCESS) {
            if (cards.count(*result) || localCards.count(*result)) {
                guard.unlock();
                osDisconnect(*result, SCARD_LEAVE_CARD);
                guard.lock();
                *result = 0;
                return SCARD_F_INTERNAL_ERROR;
            }
            try {
                if (!localCards.emplace(*result, context).second ||
                    !localCardIdentities.emplace(*result, LocalCardIdentityPtr(new LocalCardIdentity)).second)
                    throw std::bad_alloc();
            }
            catch (...) {
                localCards.erase(*result);
                localCardIdentities.erase(*result);
                guard.unlock();
                osDisconnect(*result, SCARD_LEAVE_CARD);
                guard.lock();
                *result = 0;
                throw;
            }
            return rc;
        }
        if (config.explicitReaders) return rc;
    }
    if (!config.explicitReaders) return connectVirtual(config.readers.front(), context, result, active, guard, identity);
    return SCARD_E_UNKNOWN_READER;
}
template<class C, class F> LONG status(SCARDHANDLE handle, C* name, LPDWORD length, LPDWORD state,
    LPDWORD protocol, LPBYTE atr, LPDWORD atrLength, F fn, std::unique_lock<std::mutex>& guard) {
    auto it = cards.find(handle);
    if (it == cards.end()) {
        if (!fn) return SCARD_E_INVALID_HANDLE;
        if (!waitForLocalCardContext(handle, guard)) return SCARD_E_INVALID_HANDLE;
        guard.unlock();
        LONG rc = fn(handle,name,length,state,protocol,atr,atrLength);
        guard.lock();
        return rc;
    }
    if (!length || !state || !protocol || !atrLength) return SCARD_E_INVALID_PARAMETER;
    *state = SCARD_SPECIFIC; *protocol = SCARD_PROTOCOL_T1;
    if (atr && *atrLength == SCARD_AUTOALLOCATE) *reinterpret_cast<LPBYTE*>(atr) = nullptr;
    *atrLength = 0;
    std::basic_string<C> text;
    if (sizeof(C) == sizeof(wchar_t)) text.assign(it->second->name.begin(), it->second->name.end());
    else { auto a = narrow(it->second->name); text.assign(a.begin(), a.end()); }
    return output(ReaderTableCore::multiString(std::vector<std::basic_string<C> >(1,text)), name, length,
                  it->second->context);
}
template<class S, class F> LONG change(SCARDCONTEXT context, DWORD timeout, S* states, DWORD count, F fn, std::unique_lock<std::mutex>& guard) {
    const ContextIdentityPtr identity = contextIdentity(context);
    if (!identity || !settledContext(context, identity, guard)) return SCARD_E_INVALID_HANDLE;
    if (count && !states) return SCARD_E_INVALID_PARAMETER;
    std::vector<S> resultStates;
    if (count) resultStates.assign(states, states + count);
    std::vector<S> local;
    std::vector<DWORD> indices;
    bool changed = false;
    for (DWORD i = 0; i < count; ++i) {
        if (!states[i].szReader) return SCARD_E_INVALID_PARAMETER;
        std::wstring name = wide(states[i].szReader);
        if (ReaderTableCore::find(config, name.c_str())) {
            if (resultStates[i].dwCurrentState & SCARD_STATE_IGNORE) {
                resultStates[i].dwEventState = SCARD_STATE_IGNORE;
                continue;
            }
            DWORD event = SCARD_STATE_PRESENT;
            if ((resultStates[i].dwCurrentState & ~SCARD_STATE_CHANGED) != event) {
                event |= SCARD_STATE_CHANGED;
                changed = true;
            }
            resultStates[i].dwEventState = event;
            resultStates[i].cbAtr = 0;
            ZeroMemory(resultStates[i].rgbAtr, sizeof(resultStates[i].rgbAtr));
        } else {
            local.push_back(resultStates[i]);
            indices.push_back(i);
        }
    }
    if (!local.empty()) {
        if (!fn || !contexts.count(context)) return SCARD_E_UNKNOWN_READER;
        // Do not hold the registry lock during OS waits: Cancel must be callable.
        StatusChangeCall statusChange(context, guard);
        guard.unlock();
        LONG rc = fn(context, changed ? 0 : timeout, local.data(), static_cast<DWORD>(local.size()));
        guard.lock();
        statusChange.finish();
        if (rc != SCARD_S_SUCCESS && rc != SCARD_E_TIMEOUT) return rc;
        if (!settledContext(context, identity, guard)) return SCARD_E_INVALID_HANDLE;
        for (std::size_t i = 0; i < local.size(); ++i) resultStates[indices[i]] = local[i];
        std::copy(resultStates.begin(), resultStates.end(), states);
        return changed && rc == SCARD_E_TIMEOUT ? SCARD_S_SUCCESS : rc;
    }
    if (count) std::copy(resultStates.begin(), resultStates.end(), states);
    return changed || !count ? SCARD_S_SUCCESS : SCARD_E_TIMEOUT;
}
}
#define API extern "C" __declspec(dllexport) LONG WINAPI
#define ENTER try { ensure(); std::unique_lock<std::mutex> guard(lock);
#define LEAVE } catch (const std::bad_alloc&) { return SCARD_E_NO_MEMORY; } catch (...) { return SCARD_F_INTERNAL_ERROR; }
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
extern "C" __declspec(dllexport) BOOL WINAPI SetConfig(DWORD ip, WORD port) {
    try { ensure(); std::lock_guard<std::mutex> guard(lock); g_dwIP = ip; g_wPort = port; return TRUE; }
    catch (...) { return FALSE; }
}
API CasLinkEstablishContext(DWORD scope, LPCVOID a, LPCVOID b, LPSCARDCONTEXT result) {
    ENTER
    if (!result) return SCARD_E_INVALID_PARAMETER;
    *result = 0;
    if (osEstablishContext) {
        SCARDCONTEXT context = 0;
        guard.unlock();
        LONG rc = osEstablishContext(scope,a,b,&context);
        guard.lock();
        if (rc == SCARD_S_SUCCESS) {
            while (releasingContexts.count(context)) contextReleaseChanged.wait(guard);
            if (virtualContexts.count(context) || contexts.count(context) ||
                contextIdentities.count(context)) {
                guard.unlock();
                osReleaseContext(context);
                guard.lock();
                return SCARD_F_INTERNAL_ERROR;
            }
            SCARDCONTEXT extraContext = 0;
            if (extraEstablishContext && extraEstablishContext(scope,a,b,&extraContext) == SCARD_S_SUCCESS) {
                try { extraContexts[context] = extraContext; }
                catch (...) { extraReleaseContext(extraContext); osReleaseContext(context); throw; }
            }
            try {
                contexts.insert(context);
                if (!contextIdentities.emplace(context, ContextIdentityPtr(new ContextIdentity)).second)
                    throw std::bad_alloc();
            }
            catch (...) {
                contexts.erase(context);
                contextIdentities.erase(context);
                auto extra = extraContexts.find(context);
                if (extra != extraContexts.end()) { extraReleaseContext(extra->second); extraContexts.erase(extra); }
                osReleaseContext(context);
                throw;
            }
            *result = context;
            return SCARD_S_SUCCESS;
        }
    }
    *result = createVirtualContext();
    return SCARD_S_SUCCESS;
    LEAVE
}
API CasLinkReleaseContext(SCARDCONTEXT context) {
    ENTER
    if (virtualContexts.erase(context)) {
        releaseVirtualResources(context);
        return SCARD_S_SUCCESS;
    }
    if (contexts.count(context)) {
        const ContextIdentityPtr identity = contextIdentity(context);
        if (!identity) return SCARD_E_INVALID_HANDLE;
        while (releasingContexts.count(context) && contextIdentity(context) == identity)
            contextReleaseChanged.wait(guard);
        if (!sameContext(context, identity)) return SCARD_E_INVALID_HANDLE;
        try { releasingContexts.insert(context); }
        catch (...) { return SCARD_E_NO_MEMORY; }
        while (activeStatusChanges.count(context)) {
            if (!osCancel) {
                releasingContexts.erase(context);
                contextReleaseChanged.notify_all();
                return SCARD_F_INTERNAL_ERROR;
            }
            // Repeat cancellation because a counted status call may not have entered winscard yet.
            guard.unlock();
            osCancel(context);
            guard.lock();
            if (activeStatusChanges.count(context))
                statusChangeFinished.wait_for(guard, std::chrono::milliseconds(50));
        }
        guard.unlock();
        LONG rc = osReleaseContext(context);
        guard.lock();
        SCARDCONTEXT extraContext = 0;
        if (rc == SCARD_S_SUCCESS) {
            contexts.erase(context);
            auto extra = extraContexts.find(context);
            extraContext = extra == extraContexts.end() ? 0 : extra->second;
            if (extra != extraContexts.end()) extraContexts.erase(extra);
            releaseVirtualResources(context);
            while (activeExtraReaderCalls.count(context)) extraReaderCallChanged.wait(guard);
        }
        if (extraContext) {
            guard.unlock();
            extraReleaseContext(extraContext);
            guard.lock();
        }
        releasingContexts.erase(context);
        contextReleaseChanged.notify_all();
        return rc;
    }
    return SCARD_E_INVALID_HANDLE;
    LEAVE
}
API CasLinkFreeMemory(SCARDCONTEXT context, LPCVOID memory) {
    ENTER
    auto it = allocations.find(memory);
    if (it != allocations.end()) {
        if (it->second != context) return SCARD_E_INVALID_PARAMETER;
        // All allocations use byte storage (see output).
        delete[] static_cast<const unsigned char*>(memory);
        allocations.erase(it); return SCARD_S_SUCCESS;
    }
    if (!osFreeMemory) return SCARD_E_INVALID_PARAMETER;
    guard.unlock();
    return osFreeMemory(context, memory);
    LEAVE
}
API CasLinkListReadersA(SCARDCONTEXT context, LPCSTR groups, char* dst, LPDWORD count) {
    ENTER return list(context,groups,dst,count,osListReadersA,extraListReadersA,guard); LEAVE
}
API CasLinkConnectA(SCARDCONTEXT context, LPCSTR name, DWORD share, DWORD protocols, LPSCARDHANDLE result, LPDWORD active) {
    ENTER return connect(context,name,wide(name),share,protocols,result,active,osConnectA,guard); LEAVE
}
API CasLinkStatusA(SCARDHANDLE handle, char* name, LPDWORD length, LPDWORD state, LPDWORD protocol, LPBYTE atr, LPDWORD atrLength) {
    ENTER return status(handle,name,length,state,protocol,atr,atrLength,osStatusA,guard); LEAVE
}
API CasLinkGetStatusChangeA(SCARDCONTEXT context, DWORD timeout, SCARD_READERSTATEA* states, DWORD count) {
    ENTER return change(context,timeout,states,count,osGetStatusChangeA,guard); LEAVE
}
API CasLinkListReadersW(SCARDCONTEXT context, LPCWSTR groups, wchar_t* dst, LPDWORD count) {
    ENTER return list(context,groups,dst,count,osListReadersW,extraListReadersW,guard); LEAVE
}
API CasLinkConnectW(SCARDCONTEXT context, LPCWSTR name, DWORD share, DWORD protocols, LPSCARDHANDLE result, LPDWORD active) {
    ENTER return connect(context,name,wide(name),share,protocols,result,active,osConnectW,guard); LEAVE
}
API CasLinkStatusW(SCARDHANDLE handle, wchar_t* name, LPDWORD length, LPDWORD state, LPDWORD protocol, LPBYTE atr, LPDWORD atrLength) {
    ENTER return status(handle,name,length,state,protocol,atr,atrLength,osStatusW,guard); LEAVE
}
API CasLinkGetStatusChangeW(SCARDCONTEXT context, DWORD timeout, SCARD_READERSTATEW* states, DWORD count) {
    ENTER return change(context,timeout,states,count,osGetStatusChangeW,guard); LEAVE
}
API CasLinkTransmit(SCARDHANDLE handle, LPCSCARD_IO_REQUEST sendPci, LPCBYTE send, DWORD sendLength,
    LPSCARD_IO_REQUEST recvPci, LPBYTE recv, LPDWORD recvLength) {
    ENTER
    auto it = cards.find(handle);
    if (it == cards.end()) {
        if (!osTransmit) return SCARD_E_INVALID_HANDLE;
        if (!waitForLocalCardContext(handle, guard)) return SCARD_E_INVALID_HANDLE;
        guard.unlock();
        LONG rc = osTransmit(handle,sendPci,send,sendLength,recvPci,recv,recvLength);
        guard.lock();
        return rc;
    }
    if (!send || !recvLength || !recv || !sendLength || sendLength > 255) return SCARD_E_INVALID_PARAMETER;
    if (recvPci && recvPci->cbPciLength < sizeof(SCARD_IO_REQUEST)) return SCARD_E_INSUFFICIENT_BUFFER;
    std::shared_ptr<Card> card = it->second;
    BYTE response[255];
    guard.unlock();
    std::lock_guard<std::mutex> io(card->ioLock);
    DWORD n = card->proxy->TransmitCommand(send,sendLength,response);
    if (!n) { *recvLength = 0; return SCARD_E_TIMEOUT; }
    DWORD capacity = *recvLength; *recvLength = n;
    if (capacity < n) return SCARD_E_INSUFFICIENT_BUFFER;
    CopyMemory(recv,response,n);
    if (recvPci) {
        recvPci->dwProtocol = SCARD_PROTOCOL_T1; recvPci->cbPciLength = sizeof(SCARD_IO_REQUEST);
    }
    return SCARD_S_SUCCESS;
    LEAVE
}
API CasLinkDisconnect(SCARDHANDLE handle, DWORD disposition) {
    ENTER
    bool localCard = false;
    LocalCardIdentityPtr requestedIdentity;
    auto initialIdentity = localCardIdentities.find(handle);
    if (initialIdentity != localCardIdentities.end()) requestedIdentity = initialIdentity->second;
    for (;;) {
        const bool waitedForLocalCard = releasingLocalCards.count(handle) != 0;
        waitForLocalCardRelease(handle, guard);
        auto currentIdentity = localCardIdentities.find(handle);
        if (waitedForLocalCard &&
            (currentIdentity == localCardIdentities.end() || currentIdentity->second != requestedIdentity))
            return SCARD_E_INVALID_HANDLE;
        auto it = cards.find(handle);
        if (it != cards.end()) { cards.erase(it); return SCARD_S_SUCCESS; }
        if (!osDisconnect) return SCARD_E_INVALID_HANDLE;
        localCard = localCards.count(handle) != 0;
        if (!localCard) break;
        if (!waitForLocalCardContext(handle, guard)) return SCARD_E_INVALID_HANDLE;
        if (!localCards.count(handle)) return SCARD_E_INVALID_HANDLE;
        currentIdentity = localCardIdentities.find(handle);
        if (currentIdentity == localCardIdentities.end() || currentIdentity->second != requestedIdentity)
            return SCARD_E_INVALID_HANDLE;
        if (releasingLocalCards.count(handle)) continue;
        try { releasingLocalCards.insert(handle); }
        catch (...) { return SCARD_E_NO_MEMORY; }
        break;
    }
    guard.unlock();
    LONG rc = osDisconnect(handle,disposition);
    guard.lock();
    if (localCard) {
        if (rc == SCARD_S_SUCCESS) {
            auto currentIdentity = localCardIdentities.find(handle);
            if (currentIdentity != localCardIdentities.end() && currentIdentity->second == requestedIdentity) {
                localCards.erase(handle);
                localCardIdentities.erase(currentIdentity);
            }
        }
        releasingLocalCards.erase(handle);
        localCardReleaseChanged.notify_all();
    }
    return rc;
    LEAVE
}
API CasLinkBeginTransaction(SCARDHANDLE handle) {
    ENTER
    if (cards.count(handle)) return SCARD_S_SUCCESS;
    if (!osBeginTransaction) return SCARD_E_INVALID_HANDLE;
    if (!waitForLocalCardContext(handle, guard)) return SCARD_E_INVALID_HANDLE;
    guard.unlock();
    LONG rc = osBeginTransaction(handle);
    guard.lock();
    return rc;
    LEAVE
}
API CasLinkEndTransaction(SCARDHANDLE handle, DWORD disposition) {
    ENTER
    if (cards.count(handle)) return SCARD_S_SUCCESS;
    if (!osEndTransaction) return SCARD_E_INVALID_HANDLE;
    if (!waitForLocalCardContext(handle, guard)) return SCARD_E_INVALID_HANDLE;
    guard.unlock();
    LONG rc = osEndTransaction(handle,disposition);
    guard.lock();
    return rc;
    LEAVE
}
API CasLinkReconnect(SCARDHANDLE handle, DWORD share, DWORD protocols, DWORD initialization, LPDWORD active) {
    ENTER
    if (cards.count(handle)) { if (active) *active = SCARD_PROTOCOL_T1; return SCARD_S_SUCCESS; }
    if (!osReconnect) return SCARD_E_INVALID_HANDLE;
    if (!waitForLocalCardContext(handle, guard)) return SCARD_E_INVALID_HANDLE;
    guard.unlock();
    LONG rc = osReconnect(handle,share,protocols,initialization,active);
    guard.lock();
    return rc;
    LEAVE
}
API CasLinkIsValidContext(SCARDCONTEXT context) {
    ENTER
    if (virtualContexts.count(context)) return SCARD_S_SUCCESS;
    if (!contexts.count(context)) return SCARD_E_INVALID_HANDLE;
    if (!osIsValidContext) return SCARD_E_INVALID_HANDLE;
    const ContextIdentityPtr identity = contextIdentity(context);
    if (!identity) return SCARD_E_INVALID_HANDLE;
    while (releasingContexts.count(context) && contextIdentity(context) == identity)
        contextReleaseChanged.wait(guard);
    if (!sameContext(context, identity)) return SCARD_E_INVALID_HANDLE;
    guard.unlock();
    LONG rc = osIsValidContext(context);
    guard.lock();
    if (rc != SCARD_S_SUCCESS) return rc;
    if (!settledContext(context, identity, guard)) return SCARD_E_INVALID_HANDLE;
    return rc;
    LEAVE
}
API CasLinkCancel(SCARDCONTEXT context) {
    ENTER
    if (virtualContexts.count(context)) return SCARD_S_SUCCESS;
    if (!contexts.count(context)) return SCARD_E_INVALID_HANDLE;
    if (!osCancel) return SCARD_E_INVALID_HANDLE;
    const ContextIdentityPtr identity = contextIdentity(context);
    if (!identity) return SCARD_E_INVALID_HANDLE;
    while (releasingContexts.count(context) && contextIdentity(context) == identity)
        contextReleaseChanged.wait(guard);
    if (!sameContext(context, identity)) return SCARD_E_INVALID_HANDLE;
    guard.unlock();
    LONG rc = osCancel(context);
    guard.lock();
    if (rc == SCARD_S_SUCCESS && !settledContext(context, identity, guard)) return SCARD_E_INVALID_HANDLE;
    return rc;
    LEAVE
}
API CasLinkGetAttrib(SCARDHANDLE handle, DWORD attr, LPBYTE data, LPDWORD length) {
    ENTER
    if (cards.count(handle)) return SCARD_E_UNSUPPORTED_FEATURE;
    if (!osGetAttrib) return SCARD_E_INVALID_HANDLE;
    if (!waitForLocalCardContext(handle, guard)) return SCARD_E_INVALID_HANDLE;
    guard.unlock();
    LONG rc = osGetAttrib(handle,attr,data,length);
    guard.lock();
    return rc;
    LEAVE
}
API CasLinkControl(SCARDHANDLE handle, DWORD code, LPCVOID input, DWORD inputLength, LPVOID outputBuffer, DWORD outputLength, LPDWORD returned) {
    ENTER
    if (cards.count(handle)) return SCARD_E_UNSUPPORTED_FEATURE;
    if (!osControl) return SCARD_E_INVALID_HANDLE;
    if (!waitForLocalCardContext(handle, guard)) return SCARD_E_INVALID_HANDLE;
    guard.unlock();
    LONG rc = osControl(handle,code,input,inputLength,outputBuffer,outputLength,returned);
    guard.lock();
    return rc;
    LEAVE
}
