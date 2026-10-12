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
#include <mutex>
#include <map>
#include <set>
#include <memory>
#include <algorithm>
#include <cstring>
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
ReaderTableCore::Config<wchar_t> config;
struct Card { std::unique_ptr<CCasProxy> proxy; std::wstring name; };
std::map<SCARDHANDLE, Card> cards;
std::set<SCARDCONTEXT> contexts;
std::set<SCARDHANDLE> localCards;
std::set<const void*> allocations;
const SCARDCONTEXT dummyContext = static_cast<SCARDCONTEXT>(-1);
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
template<class C> LONG output(const std::basic_string<C>& data, C* dst, LPDWORD count) {
    if (!count) return SCARD_E_INVALID_PARAMETER;
    DWORD needed = static_cast<DWORD>(data.size());
    DWORD capacity = *count;
    *count = needed;
    if (!dst) return SCARD_S_SUCCESS;
    if (capacity == SCARD_AUTOALLOCATE) {
        std::unique_ptr<unsigned char[]> mem(new unsigned char[needed * sizeof(C)]);
        std::memcpy(mem.get(), data.data(), needed * sizeof(C));
        allocations.insert(mem.get());
        *reinterpret_cast<C**>(dst) = reinterpret_cast<C*>(mem.release());
        return SCARD_S_SUCCESS;
    }
    if (capacity < needed) return SCARD_E_INSUFFICIENT_BUFFER;
    std::copy(data.begin(), data.end(), dst);
    return SCARD_S_SUCCESS;
}
template<class C, class F> LONG list(SCARDCONTEXT context, const C* groups, C* dst, LPDWORD count, F fn, F extraFn, std::unique_lock<std::mutex>& guard) {
    if (!count) return SCARD_E_INVALID_PARAMETER;
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
    }
    if (!guard.owns_lock()) guard.lock();
    return output(ReaderTableCore::multiString(names), dst, count);
}
LONG connectVirtual(const ReaderTableCore::Reader<wchar_t>& reader, LPSCARDHANDLE result, LPDWORD protocol) {
    Card card;
    card.name = reader.name;
    card.proxy.reset(new CCasProxy(nullptr));
    card.proxy->Setting(config.explicitReaders ? ipValue(reader.host) : g_dwIP,
                        config.explicitReaders ? reader.port : g_wPort);
    if (!card.proxy->Connect()) return SCARD_E_READER_UNAVAILABLE;
    // Tokens never expose pointers and are never reused. Avoid known OS handles.
    if (nextCard == 0 || nextCard == (std::numeric_limits<SCARDHANDLE>::max)()) return SCARD_E_NO_MEMORY;
    while (localCards.count(nextCard)) {
        if (++nextCard == 0 || nextCard == (std::numeric_limits<SCARDHANDLE>::max)()) return SCARD_E_NO_MEMORY;
    }
    SCARDHANDLE handle = nextCard++;
    cards.emplace(handle, std::move(card));
    *result = handle;
    if (protocol) *protocol = SCARD_PROTOCOL_T1;
    return SCARD_S_SUCCESS;
}
template<class C, class F> LONG connect(SCARDCONTEXT context, const C* name, const std::wstring& wname,
    DWORD share, DWORD protocols, LPSCARDHANDLE result, LPDWORD active, F fn, std::unique_lock<std::mutex>& guard) {
    if (!result) return SCARD_E_INVALID_PARAMETER;
    *result = 0;
    auto r = ReaderTableCore::find(config, name ? wname.c_str() : nullptr);
    if (r) return connectVirtual(*r, result, active);
    if (name && fn && contexts.count(context)) {
        guard.unlock();
        LONG rc = fn(context, name, share, protocols, result, active);
        guard.lock();
        if (rc == SCARD_S_SUCCESS) {
            if (cards.count(*result)) {
                osDisconnect(*result, SCARD_LEAVE_CARD); *result = 0;
                return SCARD_F_INTERNAL_ERROR;
            }
            try { localCards.insert(*result); }
            catch (...) { osDisconnect(*result, SCARD_LEAVE_CARD); *result = 0; throw; }
            return rc;
        }
        if (config.explicitReaders) return rc;
    }
    if (!config.explicitReaders) return connectVirtual(config.readers.front(), result, active);
    return SCARD_E_UNKNOWN_READER;
}
template<class C, class F> LONG status(SCARDHANDLE handle, C* name, LPDWORD length, LPDWORD state,
    LPDWORD protocol, LPBYTE atr, LPDWORD atrLength, F fn, std::unique_lock<std::mutex>& guard) {
    auto it = cards.find(handle);
    if (it == cards.end()) return fn ? (guard.unlock(), fn(handle,name,length,state,protocol,atr,atrLength)) : SCARD_E_INVALID_HANDLE;
    if (!length || !state || !protocol || !atrLength) return SCARD_E_INVALID_PARAMETER;
    *state = SCARD_SPECIFIC; *protocol = SCARD_PROTOCOL_T1;
    if (atr && *atrLength == SCARD_AUTOALLOCATE) *reinterpret_cast<LPBYTE*>(atr) = nullptr;
    *atrLength = 0;
    std::basic_string<C> text;
    if (sizeof(C) == sizeof(wchar_t)) text.assign(it->second.name.begin(), it->second.name.end());
    else { auto a = narrow(it->second.name); text.assign(a.begin(), a.end()); }
    return output(ReaderTableCore::multiString(std::vector<std::basic_string<C> >(1,text)), name, length);
}
template<class S, class F> LONG change(SCARDCONTEXT context, DWORD timeout, S* states, DWORD count, F fn, std::unique_lock<std::mutex>& guard) {
    if (count && !states) return SCARD_E_INVALID_PARAMETER;
    std::vector<S> local;
    std::vector<DWORD> indices;
    bool changed = false;
    for (DWORD i = 0; i < count; ++i) {
        if (!states[i].szReader) return SCARD_E_INVALID_PARAMETER;
        std::wstring name = wide(states[i].szReader);
        if (ReaderTableCore::find(config, name.c_str())) {
            if (states[i].dwCurrentState & SCARD_STATE_IGNORE) { states[i].dwEventState = SCARD_STATE_IGNORE; continue; }
            DWORD event = SCARD_STATE_PRESENT;
            if ((states[i].dwCurrentState & ~SCARD_STATE_CHANGED) != event) { event |= SCARD_STATE_CHANGED; changed = true; }
            states[i].dwEventState = event; states[i].cbAtr = 0;
            ZeroMemory(states[i].rgbAtr, sizeof(states[i].rgbAtr));
        } else { local.push_back(states[i]); indices.push_back(i); }
    }
    if (!local.empty()) {
        if (!fn || !contexts.count(context)) return SCARD_E_UNKNOWN_READER;
        // Do not hold the registry lock during OS waits: Cancel must be callable.
        guard.unlock();
        LONG rc = fn(context, changed ? 0 : timeout, local.data(), static_cast<DWORD>(local.size()));
        if (rc != SCARD_S_SUCCESS && rc != SCARD_E_TIMEOUT) return rc;
        for (std::size_t i = 0; i < local.size(); ++i) states[indices[i]] = local[i];
        return changed && rc == SCARD_E_TIMEOUT ? SCARD_S_SUCCESS : rc;
    }
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
    *result = dummyContext;
    if (osEstablishContext) {
        SCARDCONTEXT context = 0;
        guard.unlock();
        LONG rc = osEstablishContext(scope,a,b,&context);
        guard.lock();
        if (rc == SCARD_S_SUCCESS) {
            SCARDCONTEXT extraContext = 0;
            if (extraEstablishContext && extraEstablishContext(scope,a,b,&extraContext) == SCARD_S_SUCCESS) {
                try { extraContexts[context] = extraContext; }
                catch (...) { extraReleaseContext(extraContext); osReleaseContext(context); throw; }
            }
            try { contexts.insert(context); }
            catch (...) {
                auto extra = extraContexts.find(context);
                if (extra != extraContexts.end()) { extraReleaseContext(extra->second); extraContexts.erase(extra); }
                osReleaseContext(context);
                throw;
            }
            *result = context;
        }
    }
    return SCARD_S_SUCCESS;
    LEAVE
}
API CasLinkReleaseContext(SCARDCONTEXT context) {
    ENTER
    if (contexts.count(context)) {
        auto extra = extraContexts.find(context);
        if (extra != extraContexts.end()) {
            extraReleaseContext(extra->second);
            extraContexts.erase(extra);
        }
        guard.unlock();
        LONG rc = osReleaseContext(context);
        guard.lock();
        if (rc == SCARD_S_SUCCESS) contexts.erase(context);
        return rc;
    }
    return context == dummyContext ? SCARD_S_SUCCESS : SCARD_E_INVALID_HANDLE;
    LEAVE
}
API CasLinkFreeMemory(SCARDCONTEXT context, LPCVOID memory) {
    ENTER
    auto it = allocations.find(memory);
    if (it != allocations.end()) {
        // All allocations use byte storage (see output).
        delete[] static_cast<const unsigned char*>(memory);
        allocations.erase(it); return SCARD_S_SUCCESS;
    }
    return osFreeMemory ? (guard.unlock(), osFreeMemory(context,memory)) : SCARD_E_INVALID_PARAMETER;
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
    if (it == cards.end()) return osTransmit ? (guard.unlock(), osTransmit(handle,sendPci,send,sendLength,recvPci,recv,recvLength)) : SCARD_E_INVALID_HANDLE;
    if (!send || !recvLength || !recv || !sendLength || sendLength > 255) return SCARD_E_INVALID_PARAMETER;
    if (recvPci && recvPci->cbPciLength < sizeof(SCARD_IO_REQUEST)) return SCARD_E_INSUFFICIENT_BUFFER;
    BYTE response[255];
    DWORD n = it->second.proxy->TransmitCommand(send,sendLength,response);
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
    auto it = cards.find(handle);
    if (it != cards.end()) { cards.erase(it); return SCARD_S_SUCCESS; }
    if (!osDisconnect) return SCARD_E_INVALID_HANDLE;
    guard.unlock();
    LONG rc = osDisconnect(handle,disposition);
    guard.lock();
    if (rc == SCARD_S_SUCCESS) localCards.erase(handle);
    return rc;
    LEAVE
}
API CasLinkBeginTransaction(SCARDHANDLE handle) {
    ENTER return cards.count(handle) ? SCARD_S_SUCCESS : (osBeginTransaction ? (guard.unlock(), osBeginTransaction(handle)) : SCARD_E_INVALID_HANDLE); LEAVE
}
API CasLinkEndTransaction(SCARDHANDLE handle, DWORD disposition) {
    ENTER return cards.count(handle) ? SCARD_S_SUCCESS : (osEndTransaction ? (guard.unlock(), osEndTransaction(handle,disposition)) : SCARD_E_INVALID_HANDLE); LEAVE
}
API CasLinkReconnect(SCARDHANDLE handle, DWORD share, DWORD protocols, DWORD initialization, LPDWORD active) {
    ENTER
    if (cards.count(handle)) { if (active) *active = SCARD_PROTOCOL_T1; return SCARD_S_SUCCESS; }
    return osReconnect ? (guard.unlock(), osReconnect(handle,share,protocols,initialization,active)) : SCARD_E_INVALID_HANDLE;
    LEAVE
}
API CasLinkIsValidContext(SCARDCONTEXT context) {
    ENTER
    if (context == dummyContext) return SCARD_S_SUCCESS;
    return osIsValidContext ? (guard.unlock(), osIsValidContext(context)) : SCARD_E_INVALID_HANDLE;
    LEAVE
}
API CasLinkCancel(SCARDCONTEXT context) {
    ENTER
    if (context == dummyContext) return SCARD_S_SUCCESS;
    return osCancel ? (guard.unlock(), osCancel(context)) : SCARD_E_INVALID_HANDLE;
    LEAVE
}
API CasLinkGetAttrib(SCARDHANDLE handle, DWORD attr, LPBYTE data, LPDWORD length) {
    ENTER return cards.count(handle) ? SCARD_E_UNSUPPORTED_FEATURE : (osGetAttrib ? (guard.unlock(), osGetAttrib(handle,attr,data,length)) : SCARD_E_INVALID_HANDLE); LEAVE
}
API CasLinkControl(SCARDHANDLE handle, DWORD code, LPCVOID input, DWORD inputLength, LPVOID outputBuffer, DWORD outputLength, LPDWORD returned) {
    ENTER return cards.count(handle) ? SCARD_E_UNSUPPORTED_FEATURE : (osControl ? (guard.unlock(), osControl(handle,code,input,inputLength,outputBuffer,outputLength,returned)) : SCARD_E_INVALID_HANDLE); LEAVE
}
