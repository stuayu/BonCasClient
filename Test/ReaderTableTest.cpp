#include "ReaderTableCore.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

template<class C> void run() {
    using namespace ReaderTableCore;
    auto text = [](const char* p) { return literal<C>(p); };
    const auto host = text("192.168.1.10");
    std::vector<std::basic_string<C> > lines = {
        text("BonCasLink B-CAS=6900"), text("BonCasLink A-CAS=6900"),
        text("BonCasLink C-CAS=6901"), text("Other Server=192.168.1.20:65535"),
        text(" =33"), text("Bad=0"), text("Too big=65536"),
        text("Overflow=9999999999999999999999999"), text("Negative=-1"),
        text("Empty="), text("No equals"), text("Bad IP=300.1.2.3:6900"),
        text("DNS=example.com:6900"), text("IPv6=::1:6900"),
        text("Short=1.2.3:6900"), text("Extra=1.2.3.4.5:6900"),
        text("Blank IP=:6900"), text(";Comment=6900"),
        text("BonCasLink B-CAS=9999"), text(" Trimmed =  127.0.0.1 : 1 ")
    };
    auto c = parse(lines, host, 6900, true, true);
    assert(c.readers.size() == 5 && c.explicitReaders && c.localEnabled);
    assert(c.readers[0].name == text("BonCasLink B-CAS") && c.readers[0].host == host);
    assert(c.readers[1].name == text("BonCasLink A-CAS") && c.readers[1].port == 6900);
    assert(c.readers[2].port == 6901);
    assert(c.readers[3].host == text("192.168.1.20") && c.readers[3].port == 65535);
    assert(c.readers[4].name == text("Trimmed") && c.readers[4].port == 1);
    assert(find(c, text("Other Server").c_str()) == &c.readers[3]);
    assert(!find(c, text("missing").c_str()));
    assert(!find(c, static_cast<const C*>(nullptr)));
    assert(!find(c, text("boncaslink B-CAS").c_str()));
    std::vector<std::basic_string<C> > empty;
    auto legacy = parse(empty,host,6902,false,false);
    assert(legacy.readers.size() == 1 && !legacy.explicitReaders && !legacy.localEnabled);
    assert(legacy.readers[0].name == text("BonCasLink Client Card Reader"));
    assert(legacy.readers[0].host == host && legacy.readers[0].port == 6902);
    auto explicitEmpty = parse(empty,host,6902,true,false);
    assert(explicitEmpty.readers.size() == 1 && explicitEmpty.explicitReaders);
    auto invalid = parse(std::vector<std::basic_string<C> >{text("Invalid=0")},host,6902,true,false);
    assert(invalid.readers.size() == 1 && invalid.explicitReaders);
    auto ms = multiString(std::vector<std::basic_string<C> >{text("Virtual"),text("Local")});
    assert(ms.size() == 15 && ms[7] == 0 && ms[13] == 0 && ms[14] == 0);
    assert(multiString(empty).size() == 2 && multiString(empty)[0] == 0);
    auto one = multiString(std::vector<std::basic_string<C> >{text("X")});
    assert(one.size() == 3 && one[0] == 'X' && one[1] == 0 && one[2] == 0);
    assert(ipv4(text("0.0.0.0")) && ipv4(text("255.255.255.255")));
    assert(!ipv4(text("1..2.3")) && !ipv4(text("1.2.3.4.")));
    assert(!ipv4(text("1.2.3.-1")) && !ipv4(text("1.2.3.256")));
}
int main() {
    run<char>();
    run<wchar_t>();
    std::cout << "ReaderTableTest: char/wchar_t tests passed\n";
}
