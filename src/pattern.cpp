#include "tokenlike/pattern.h"

#include <stdexcept>

namespace tokenlike {

bool Pattern::has_any1() const {
    for (const Elem& e : elems)
        if (e.kind == ElemKind::Any1) return true;
    return false;
}

size_t Pattern::interior_stars() const {
    size_t k = 0;
    for (size_t i = 1; i + 1 < elems.size(); ++i)
        if (elems[i].kind == ElemKind::Star) ++k;
    return k;
}

bool valid_utf8(std::string_view s) {
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    size_t n = s.size(), i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x80) { ++i; continue; }
        size_t len;
        uint8_t lo = 0x80, hi = 0xBF;  // allowed range of the second byte
        if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            if (c == 0xE0) lo = 0xA0;
            if (c == 0xED) hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            if (c == 0xF0) lo = 0x90;
            if (c == 0xF4) hi = 0x8F;
        } else return false;
        if (i + len > n) return false;
        if (p[i + 1] < lo || p[i + 1] > hi) return false;
        for (size_t k = 2; k < len; ++k)
            if (p[i + k] < 0x80 || p[i + k] > 0xBF) return false;
        i += len;
    }
    return true;
}

Pattern parse_like(std::string_view pattern) {
    if (!valid_utf8(pattern)) throw std::invalid_argument("LIKE pattern is not valid UTF-8");
    Pattern p;
    for (char ch : pattern) {
        if (ch == '%') {
            if (p.elems.empty() || p.elems.back().kind != ElemKind::Star)
                p.elems.push_back({ElemKind::Star, {}});
        } else if (ch == '_') {
            p.elems.push_back({ElemKind::Any1, {}});
        } else {
            if (p.elems.empty() || p.elems.back().kind != ElemKind::Lit)
                p.elems.push_back({ElemKind::Lit, {}});
            p.elems.back().bytes.push_back(ch);
        }
    }
    return p;
}

std::string to_string(const Pattern& p) {
    std::string s;
    for (const Elem& e : p.elems) {
        switch (e.kind) {
            case ElemKind::Lit: s += e.bytes; break;
            case ElemKind::Any1: s += '_'; break;
            case ElemKind::Star: s += '%'; break;
        }
    }
    return s;
}

}  // namespace tokenlike
