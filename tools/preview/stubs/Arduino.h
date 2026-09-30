// Just enough of Arduino.h for screen.h to compile on a PC.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

using std::isnan;
using std::max;
using std::min;

template <typename T, typename L, typename H>
T constrain(T v, L lo, H hi) { return v < lo ? lo : v > hi ? hi : v; }

class String : public std::string {
public:
    String() {}
    String(const char* s) : std::string(s) {}
    String(const std::string& s) : std::string(s) {}
    String(char c) : std::string(1, c) {}
    String(int v) : std::string(std::to_string(v)) {}
    String(long v) : std::string(std::to_string(v)) {}
    String(unsigned v) : std::string(std::to_string(v)) {}
    String(unsigned long v) : std::string(std::to_string(v)) {}
    String(float v, int decimals = 2) {
        char buf[32];
        snprintf(buf, sizeof buf, "%.*f", decimals, v);
        assign(buf);
    }

    unsigned length() const { return (unsigned)size(); }
    String substring(unsigned from) const { return from >= size() ? String() : String(substr(from)); }
    String substring(unsigned from, unsigned to) const {
        if (from > to) std::swap(from, to);
        return from >= size() ? String() : String(substr(from, to - from));
    }
    int indexOf(const String& s, unsigned from = 0) const {
        size_t p = find(s, from);
        return p == npos ? -1 : (int)p;
    }
    int lastIndexOf(const String& s) const {
        size_t p = rfind(s);
        return p == npos ? -1 : (int)p;
    }
    void trim() {
        size_t a = find_first_not_of(" \t\r\n");
        if (a == npos) { clear(); return; }
        size_t b = find_last_not_of(" \t\r\n");
        assign(substr(a, b - a + 1));
    }
    void toLowerCase() { for (auto& c : *this) c = (char)tolower((unsigned char)c); }
    void toUpperCase() { for (auto& c : *this) c = (char)toupper((unsigned char)c); }
    void setCharAt(unsigned i, char c) { if (i < size()) (*this)[i] = c; }
    bool startsWith(const String& s) const { return compare(0, s.size(), s) == 0; }
    bool endsWith(const String& s) const {
        return size() >= s.size() && compare(size() - s.size(), s.size(), s) == 0;
    }
};

inline String operator+(const String& a, const String& b) { return String(static_cast<const std::string&>(a) + b); }
inline String operator+(const char* a, const String& b) { return String(a) + b; }
inline String operator+(const String& a, const char* b) { return a + String(b); }
