#pragma once
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>

namespace ReaderTableCore {
template<class C> struct Reader { std::basic_string<C> name, host; std::uint16_t port; };
template<class C> struct Config { std::vector<Reader<C> > readers; bool explicitReaders; bool localEnabled; };
template<class C> std::basic_string<C> literal(const char* p) {
    std::basic_string<C> s;
    while (*p) s.push_back(static_cast<C>(*p++));
    return s;
}
template<class C> std::basic_string<C> trim(const std::basic_string<C>& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t')) --b;
    return s.substr(a, b-a);
}
template<class C> bool number(const std::basic_string<C>& s, unsigned long& n, unsigned long max = 65535) {
    if (s.empty()) return false;
    n = 0;
    for (C c : s) {
        if (c < '0' || c > '9' || n > max / 10) return false;
        n = n * 10 + (c - '0');
        if (n > max) return false;
    }
    return true;
}
template<class C> bool ipv4(const std::basic_string<C>& s) {
    std::size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        std::size_t end = s.find('.', start);
        if ((i < 3) == (end == std::basic_string<C>::npos)) return false;
        unsigned long n;
        if (!number(s.substr(start, end == std::basic_string<C>::npos ? end : end-start), n, 255)) return false;
        start = end + 1;
    }
    return true;
}
template<class C> bool endpoint(const std::basic_string<C>& raw, const std::basic_string<C>& defaultHost,
                                std::basic_string<C>& host, std::uint16_t& port) {
    auto s = trim(raw);
    auto colon = s.find(':');
    host = colon == std::basic_string<C>::npos ? defaultHost : trim(s.substr(0, colon));
    auto p = colon == std::basic_string<C>::npos ? s : trim(s.substr(colon+1));
    unsigned long n;
    if (!ipv4(host) || !number(p, n) || n == 0) return false;
    port = static_cast<std::uint16_t>(n);
    return true;
}
template<class C> Config<C> parse(const std::vector<std::basic_string<C> >& lines,
    const std::basic_string<C>& serverHost, std::uint16_t serverPort, bool hasReaders, bool local) {
    Config<C> out;
    out.explicitReaders = hasReaders;
    out.localEnabled = local;
    for (const auto& line : lines) {
        auto e = line.find('=');
        if (e == std::basic_string<C>::npos) continue;
        auto name = trim(line.substr(0,e));
        std::basic_string<C> host;
        std::uint16_t port;
        if (name.empty() || name[0] == ';' || name[0] == '#' ||
            !endpoint(line.substr(e+1), serverHost, host, port)) continue;
        bool duplicate = false;
        for (const auto& r : out.readers) if (r.name == name) duplicate = true;
        if (!duplicate) out.readers.push_back(Reader<C>{name,host,port});
    }
    if (out.readers.empty()) out.readers.push_back(Reader<C>{literal<C>("BonCasLink Client Card Reader"),serverHost,serverPort});
    return out;
}
template<class C> const Reader<C>* find(const Config<C>& c, const C* name) {
    if (name) for (const auto& r : c.readers) if (r.name == name) return &r;
    return nullptr;
}
template<class C> std::basic_string<C> multiString(const std::vector<std::basic_string<C> >& names) {
    std::basic_string<C> out;
    for (const auto& name : names) { out += name; out.push_back(0); }
    out.push_back(0);
    if (names.empty()) out.push_back(0);
    return out;
}
}
