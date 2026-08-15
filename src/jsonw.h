// showdsp — minimal JSON writer. MIT license.
#pragma once
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>

namespace showdsp {

class JsonW {
public:
    std::string s;
    JsonW() { s.reserve(1 << 16); }
    void beginObj() { pre(); s += '{'; _first = true; _stack.push_back('}'); }
    void endObj() { s += '}'; _stack.pop_back(); _first = false; }
    void beginArr() { pre(); s += '['; _first = true; _stack.push_back(']'); }
    void endArr() { s += ']'; _stack.pop_back(); _first = false; }
    void key(const char* k) { pre(); s += '"'; s += k; s += "\":"; _key = true; }
    void str(const char* v) { pre(); s += '"'; esc(v); s += '"'; }
    void num(double v, int prec = 4) {
        pre();
        if (!std::isfinite(v)) { s += "null"; return; }
        char b[40]; snprintf(b, sizeof b, "%.*g", prec + 3, v); s += b;
    }
    void boolean(bool v) { pre(); s += v ? "true" : "false"; }
    void kv(const char* k, double v, int prec = 4) { key(k); num(v, prec); }
    void kv(const char* k, const char* v) { key(k); str(v); }
    void kv(const char* k, const std::string& v) { key(k); str(v.c_str()); }
    void kvb(const char* k, bool v) { key(k); boolean(v); }
    void arr(const char* k, const std::vector<float>& v, int prec = 4) {
        key(k); beginArr(); for (float x : v) num(x, prec); endArr();
    }
private:
    bool _first = true, _key = false;
    std::vector<char> _stack;
    void pre() {
        if (_key) { _key = false; return; }
        if (!_first && !_stack.empty()) s += ',';
        _first = false;
    }
    void esc(const char* v) {
        for (; *v; v++) {
            if (*v == '"' || *v == '\\') { s += '\\'; s += *v; }
            else if (*v == '\n') s += "\\n";
            else s += *v;
        }
    }
};

} // namespace showdsp
