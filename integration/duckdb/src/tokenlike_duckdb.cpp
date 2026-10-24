// TokenLIKE DuckDB extension (Person A, optional module integration/duckdb).
//
// A loadable DuckDB extension written against DuckDB's C extension API (C_STRUCT ABI, API v1.5.6).
// It registers scalar functions that evaluate SQL LIKE directly on token-id columns with the
// shared TokenLIKE matcher (Matcher::build / match / match_concat, compiled from src/):
//
//   tokenlike_load_vocab(path VARCHAR) -> BIGINT        load the frozen vocabulary (returns n)
//   token_like(tokens BLOB, pattern VARCHAR) -> BOOLEAN
//   token_like_concat(a BLOB, b BLOB, pattern VARCHAR) -> BOOLEAN    (a || b) LIKE pattern
//   tokenize(text VARCHAR) -> BLOB                      greedy longest-match encoding (demo only)
//   detokenize(tokens BLOB) -> VARCHAR                  decode (used for the decode+LIKE baseline)
//   tokenlike_info(pattern VARCHAR) -> VARCHAR          JSON: scan kind, states, table size, build time
//   tokenlike_stats() -> VARCHAR                        JSON: cache and scan counters
//   tokenlike_reset_stats() -> BIGINT
//   tokenlike_cache_clear() -> BIGINT
//   tokenlike_floor_noop(tokens BLOB) -> BOOLEAN       diagnostics: C-API call, no data access
//   tokenlike_floor_copy(tokens BLOB) -> BOOLEAN       diagnostics: token copy + validation, no matcher
//
// Token column representation: one BLOB per row holding the row's token ids as little-endian
// uint16 values, so length(blob) == 2 * token_count. The empty BLOB is the empty string; NULL is
// SQL NULL. Odd lengths and ids >= vocab.n are errors.
//
// Lifetimes:
//   * Extension state (vocabulary + matcher cache + counters) is per DuckDB database instance:
//     it hangs off the functions' extra_info and is destroyed with the catalog entries.
//   * A constant pattern is folded at bind time and its Matcher is built (or taken from the cache)
//     once; the bind data holds a shared_ptr to the immutable Matcher for the life of the bound
//     expression (the query). Execution threads only read it.
//   * Non-constant patterns are resolved per run of equal patterns in a chunk through the same
//     mutex-protected cache, keyed by the exact pattern bytes, inside the current vocabulary
//     snapshot. Loading a new vocabulary creates a new snapshot with an empty cache.
//   * Per-thread scratch buffers (token copies, bitmap) live in the scalar function init state.
//
// No exception may cross the C ABI boundary: every callback catches and reports through the
// C API's error setters.

#define DUCKDB_EXTENSION_NAME tokenlike
#include "duckdb_extension.h"
DUCKDB_EXTENSION_EXTERN  // defined by DUCKDB_EXTENSION_ENTRYPOINT at the end of this file

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "tokenlike/matcher.h"
#include "tokenlike/pattern.h"
#include "tokenlike/vocab.h"

namespace {

// Debug tracing to stderr when the TOKENLIKE_TRACE environment variable is set.
#define TRACE(msg) do { static const bool on_ = std::getenv("TOKENLIKE_TRACE") != nullptr; if (on_) { std::fprintf(stderr, "[tokenlike] %s\n", msg); std::fflush(stderr); } } while (0)

constexpr size_t kMaxCachedMatchers = 64;
constexpr size_t kMaxCacheBytes = size_t(256) << 20;

static_assert(sizeof(bool) == 1, "BOOLEAN vectors are arrays of 1-byte bool");

// ------------------------------------------------------------------ helpers

inline uint32_t slen(const duckdb_string_t& s) { return s.value.inlined.length; }
inline const char* sdata(const duckdb_string_t& s) {
    return s.value.inlined.length <= 12 ? s.value.inlined.inlined : s.value.pointer.ptr;
}
inline bool row_valid(const uint64_t* v, idx_t i) { return !v || ((v[i >> 6] >> (i & 63)) & 1); }

bool host_is_little_endian() {
    uint16_t x = 1;
    uint8_t b;
    std::memcpy(&b, &x, 1);
    return b == 1;
}

// UTF-8 check with an ASCII fast path: 8 bytes at a time while no high bit is set, then the
// strict validator from the TokenLIKE sources on the remainder (which starts on a character
// boundary, because everything before it is ASCII).
bool valid_utf8_fast(const char* p, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        if (w & 0x8080808080808080ull) break;
    }
    for (; i < n && !(static_cast<unsigned char>(p[i]) & 0x80); ++i) {}
    return i == n || tokenlike::valid_utf8(std::string_view(p + i, n - i));
}

std::string json_escape(std::string_view s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o += char(c);
                }
        }
    }
    return o;
}

const char* kind_name(ScanKind k) {
    switch (k) {
        case ScanKind::Forward: return "forward";
        case ScanKind::Backward: return "backward";
        case ScanKind::Anchored: return "anchored";
    }
    return "?";
}

// ------------------------------------------------------------------ state

struct Counters {
    std::atomic<uint64_t> matcher_builds{0};   // Matcher::build calls
    std::atomic<uint64_t> cache_hits{0};       // cache lookups that found a matcher
    std::atomic<uint64_t> bind_constant{0};    // patterns folded and resolved at bind time
    std::atomic<uint64_t> runtime_lookups{0};  // pattern runs resolved during execution
    std::atomic<uint64_t> chunks{0};
    std::atomic<uint64_t> rows{0};
    std::atomic<uint64_t> tokens_total{0};     // tokens in the rows handed to the matcher
    std::atomic<uint64_t> tokens_read{0};      // tokens the matcher actually consumed
    std::atomic<uint64_t> matches{0};
    std::atomic<uint64_t> decoded_rows{0};     // detokenize() rows

    void reset() {
        for (auto* c : {&matcher_builds, &cache_hits, &bind_constant, &runtime_lookups, &chunks, &rows,
                        &tokens_total, &tokens_read, &matches, &decoded_rows})
            c->store(0, std::memory_order_relaxed);
    }
};

using MatcherPtr = std::shared_ptr<const Matcher>;

// One loaded vocabulary and everything derived from it. Immutable except the mutex-guarded cache.
struct VocabState {
    Vocab v;
    std::string path;
    uint64_t generation = 0;
    // Greedy encoder: token bytes -> smallest id with those bytes (empty tokens excluded).
    std::unordered_map<std::string_view, uint16_t> enc;
    uint32_t max_len = 0;
    // detokenize(): token bytes followed by 16 bytes of padding, so a token (<= 16 bytes) can be
    // copied with one fixed-size 16-byte move.
    std::vector<char> padded;

    mutable std::mutex mu;
    mutable std::unordered_map<std::string, MatcherPtr> cache;
    mutable size_t cache_bytes = 0;

    MatcherPtr get_matcher(std::string_view pattern, Counters& c) const {
        std::string key(pattern);
        {
            std::lock_guard<std::mutex> g(mu);
            auto it = cache.find(key);
            if (it != cache.end()) {
                c.cache_hits.fetch_add(1, std::memory_order_relaxed);
                return it->second;
            }
        }
        // Built outside the lock: a slow pattern must not block other queries. Two threads may
        // build the same pattern concurrently; the first insert wins and both results are equal.
        auto m = std::make_shared<const Matcher>(Matcher::build(pattern, v));
        c.matcher_builds.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> g(mu);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        size_t b = m->table_bytes();
        if (cache.size() >= kMaxCachedMatchers || cache_bytes + b > kMaxCacheBytes) {
            cache.clear();  // in-flight queries keep their matchers alive through shared_ptr
            cache_bytes = 0;
        }
        cache.emplace(std::move(key), m);
        cache_bytes += b;
        return m;
    }

    size_t clear_cache() const {
        std::lock_guard<std::mutex> g(mu);
        size_t n = cache.size();
        cache.clear();
        cache_bytes = 0;
        return n;
    }
};

struct ExtState {
    std::mutex mu;
    std::shared_ptr<const VocabState> vocab;
    uint64_t next_generation = 1;
    Counters counters;

    std::shared_ptr<const VocabState> current() {
        std::lock_guard<std::mutex> g(mu);
        return vocab;
    }
    std::shared_ptr<const VocabState> require() {
        auto v = current();
        if (!v)
            throw std::runtime_error(
                "tokenlike: no vocabulary loaded; run SELECT tokenlike_load_vocab('<path>/vocab.bin') first");
        return v;
    }
    uint32_t load(const std::string& path) {
        auto t0 = std::chrono::steady_clock::now();
        auto vs = std::make_shared<VocabState>();
        vs->v = load_vocab(path);  // Person B's validating loader (copied source)
        vs->path = path;
        auto t1 = std::chrono::steady_clock::now();
        for (uint32_t t = 0; t < vs->v.n; ++t) {
            std::string_view s = vs->v.token(static_cast<uint16_t>(t));
            if (s.empty()) continue;
            vs->enc.emplace(s, static_cast<uint16_t>(t));  // keeps the first (smallest) id
            vs->max_len = std::max<uint32_t>(vs->max_len, static_cast<uint32_t>(s.size()));
        }
        vs->padded.assign(vs->v.bytes.begin(), vs->v.bytes.end());
        vs->padded.resize(vs->v.bytes.size() + 16, 0);
        for (int b = 0; b < 256; ++b) {
            char c = static_cast<char>(b);
            if (!vs->enc.count(std::string_view(&c, 1)))
                throw std::runtime_error("tokenlike: vocabulary lacks single-byte token " + std::to_string(b) +
                                         "; tokenize() would be incomplete");
        }
        auto t2 = std::chrono::steady_clock::now();
        {
            char buf[160];
            std::snprintf(buf, sizeof buf, "load_vocab %.2f ms, encoder %.2f ms",
                          std::chrono::duration<double, std::milli>(t1 - t0).count(),
                          std::chrono::duration<double, std::milli>(t2 - t1).count());
            TRACE(buf);
        }
        std::lock_guard<std::mutex> g(mu);
        vs->generation = next_generation++;
        vocab = std::move(vs);
        return vocab->v.n;
    }
};

using StateRef = std::shared_ptr<ExtState>;  // what extra_info points to

ExtState& ext_from(void* extra) { return **static_cast<StateRef*>(extra); }

// Per-thread scratch, reused across chunks.
struct Scratch {
    Column a, b;
    Bitmap bm;
    std::string text;
    std::vector<uint8_t> blob;
};

void destroy_scratch(void* p) { delete static_cast<Scratch*>(p); }

void init_scratch(duckdb_init_info info) {
    try {
        duckdb_scalar_function_init_set_state(info, new Scratch, destroy_scratch);
    } catch (const std::exception& e) {
        duckdb_scalar_function_init_set_error(info, e.what());
    } catch (...) {
        duckdb_scalar_function_init_set_error(info, "tokenlike: unknown error in init");
    }
}

// ------------------------------------------------------------------ token blobs -> Column

// Copies the token ids of rows [i0, i1) into c (NULL rows become empty rows). Throws on odd
// lengths and out-of-vocabulary ids. The matcher never sees a row the caller did not validate.
void fill_column(Column& c, const duckdb_string_t* d, const uint64_t* valid, idx_t i0, idx_t i1,
                 uint32_t vocab_n) {
    idx_t n = i1 - i0;
    c.rows = n;
    c.off.resize(n + 1);
    c.off[0] = 0;
    uint64_t total = 0;
    for (idx_t r = 0; r < n; ++r) {
        idx_t i = i0 + r;
        if (row_valid(valid, i)) {
            uint32_t len = slen(d[i]);
            if (len & 1)
                throw std::invalid_argument("tokenlike: token BLOB has odd length " + std::to_string(len) +
                                            " (must hold little-endian uint16 token ids)");
            total += len / 2;
        }
        c.off[r + 1] = total;
    }
    c.tok.resize(total);
    uint16_t* out = c.tok.data();
    for (idx_t r = 0; r < n; ++r) {
        idx_t i = i0 + r;
        size_t cnt = c.off[r + 1] - c.off[r];
        if (cnt) std::memcpy(out + c.off[r], sdata(d[i]), cnt * 2);
    }
    if (vocab_n < 65536 && total) {
        uint16_t mx = *std::max_element(c.tok.begin(), c.tok.end());
        if (mx >= vocab_n)
            throw std::out_of_range("tokenlike: token id " + std::to_string(mx) + " is outside the vocabulary (n=" +
                                    std::to_string(vocab_n) + ")");
    }
}

// ------------------------------------------------------------------ token_like / token_like_concat

struct LikeBind {
    std::shared_ptr<const VocabState> vocab;
    MatcherPtr matcher;     // set when the pattern was a foldable non-NULL VARCHAR
    std::string pattern;    // the folded pattern (for the runtime equality check)
    bool null_pattern = false;
};

void* copy_like_bind(void* p) {
    try {
        return new LikeBind(*static_cast<LikeBind*>(p));
    } catch (...) {
        return nullptr;
    }
}
void destroy_like_bind(void* p) { delete static_cast<LikeBind*>(p); }

struct ExprGuard {
    duckdb_expression e = nullptr;
    ~ExprGuard() {
        if (e) duckdb_destroy_expression(&e);
    }
};
struct ContextGuard {
    duckdb_client_context c = nullptr;
    ~ContextGuard() {
        if (c) duckdb_destroy_client_context(&c);
    }
};
struct ValueGuard {
    duckdb_value v = nullptr;
    ~ValueGuard() {
        if (v) duckdb_destroy_value(&v);
    }
};
struct ErrorGuard {
    duckdb_error_data e = nullptr;
    ~ErrorGuard() {
        if (e) duckdb_destroy_error_data(&e);
    }
};
struct TypeGuard {
    duckdb_logical_type t = nullptr;
    ~TypeGuard() {
        if (t) duckdb_destroy_logical_type(&t);
    }
};

duckdb_type arg_type(duckdb_bind_info info, idx_t i) {
    ExprGuard e;
    e.e = duckdb_scalar_function_bind_get_argument(info, i);
    if (!e.e) return DUCKDB_TYPE_INVALID;
    TypeGuard t;
    t.t = duckdb_expression_return_type(e.e);
    return t.t ? duckdb_get_type_id(t.t) : DUCKDB_TYPE_INVALID;
}

void bind_like(duckdb_bind_info info) {
    try {
        ExtState& st = ext_from(duckdb_scalar_function_bind_get_extra_info(info));
        auto bd = std::make_unique<LikeBind>();
        bd->vocab = st.require();
        idx_t nargs = duckdb_scalar_function_bind_get_argument_count(info);
        // Binding runs before DuckDB inserts implicit casts. Refuse text where token ids are
        // expected, instead of reinterpreting UTF-8 bytes as uint16 ids.
        for (idx_t i = 0; i + 1 < nargs; ++i) {
            duckdb_type t = arg_type(info, i);
            if (t == DUCKDB_TYPE_VARCHAR)
                throw std::invalid_argument(
                    "tokenlike: token columns must be BLOBs of uint16 token ids, got VARCHAR "
                    "(use tokenize(text) to encode a string)");
        }
        TRACE("bind: arg types checked");
        ExprGuard pe;
        pe.e = duckdb_scalar_function_bind_get_argument(info, nargs - 1);
        TRACE("bind: got pattern expression");
        if (pe.e && duckdb_expression_is_foldable(pe.e)) {
            TRACE("bind: foldable");
            ContextGuard ctx;
            duckdb_scalar_function_get_client_context(info, &ctx.c);
            TRACE("bind: got client context");
            ValueGuard val;
            ErrorGuard err;
            err.e = duckdb_expression_fold(ctx.c, pe.e, &val.v);
            TRACE("bind: folded");
            if (err.e && duckdb_error_data_has_error(err.e))
                throw std::runtime_error(std::string("tokenlike: cannot fold pattern: ") +
                                         duckdb_error_data_message(err.e));
            if (val.v) {
                // Borrowed from the value: the C API forbids destroying it.
                duckdb_logical_type vt = duckdb_get_value_type(val.v);
                if (duckdb_is_null_value(val.v)) {
                    bd->null_pattern = true;
                } else if (vt && duckdb_get_type_id(vt) == DUCKDB_TYPE_VARCHAR) {
                    TRACE("bind: varchar value");
                    char* s = duckdb_get_varchar(val.v);
                    if (s) {
                        bd->pattern = s;
                        duckdb_free(s);
                        // Build now: invalid patterns and TooComplex fail the query at bind time.
                        bd->matcher = bd->vocab->get_matcher(bd->pattern, st.counters);
                        st.counters.bind_constant.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                // Other constant types are left to the runtime path, which sees the cast VARCHAR.
            }
        }
        TRACE("bind: setting bind data");
        duckdb_scalar_function_set_bind_data(info, bd.release(), destroy_like_bind);
        duckdb_scalar_function_set_bind_data_copy(info, copy_like_bind);
    } catch (const std::exception& e) {
        duckdb_scalar_function_bind_set_error(info, e.what());
    } catch (...) {
        duckdb_scalar_function_bind_set_error(info, "tokenlike: unknown error in bind");
    }
}

// tokenize / detokenize: pin the vocabulary snapshot for the whole query, so a concurrent
// tokenlike_load_vocab() cannot mix two vocabularies inside one statement.
void bind_vocab(duckdb_bind_info info) {
    try {
        ExtState& st = ext_from(duckdb_scalar_function_bind_get_extra_info(info));
        auto bd = std::make_unique<LikeBind>();
        bd->vocab = st.require();
        duckdb_scalar_function_set_bind_data(info, bd.release(), destroy_like_bind);
        duckdb_scalar_function_set_bind_data_copy(info, copy_like_bind);
    } catch (const std::exception& e) {
        duckdb_scalar_function_bind_set_error(info, e.what());
    } catch (...) {
        duckdb_scalar_function_bind_set_error(info, "tokenlike: unknown error in bind");
    }
}

const VocabState& bound_vocab(duckdb_function_info info) {
    auto* bd = static_cast<const LikeBind*>(duckdb_scalar_function_get_bind_data(info));
    if (!bd || !bd->vocab) throw std::runtime_error("tokenlike: missing bind data");
    return *bd->vocab;
}

void run_like(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output, bool concat) {
    ExtState& st = ext_from(duckdb_scalar_function_get_extra_info(info));
    auto* bd = static_cast<const LikeBind*>(duckdb_scalar_function_get_bind_data(info));
    auto* sc = static_cast<Scratch*>(duckdb_scalar_function_get_state(info));
    TRACE("exec: start");
    if (!bd || !sc) throw std::runtime_error("tokenlike: missing bind data or thread state");

    const idx_t n = duckdb_data_chunk_get_size(input);
    auto* res = static_cast<bool*>(duckdb_vector_get_data(output));
    duckdb_vector_ensure_validity_writable(output);
    uint64_t* rvalid = duckdb_vector_get_validity(output);
    if (n == 0) return;

    duckdb_vector va = duckdb_data_chunk_get_vector(input, 0);
    duckdb_vector vb = concat ? duckdb_data_chunk_get_vector(input, 1) : nullptr;
    duckdb_vector vp = duckdb_data_chunk_get_vector(input, concat ? 2 : 1);
    auto* a = static_cast<const duckdb_string_t*>(duckdb_vector_get_data(va));
    auto* b = concat ? static_cast<const duckdb_string_t*>(duckdb_vector_get_data(vb)) : nullptr;
    auto* p = static_cast<const duckdb_string_t*>(duckdb_vector_get_data(vp));
    const uint64_t* av = duckdb_vector_get_validity(va);
    const uint64_t* bv = concat ? duckdb_vector_get_validity(vb) : nullptr;
    const uint64_t* pv = duckdb_vector_get_validity(vp);

    // SQL semantics: NULL if any argument is NULL.
    for (idx_t i = 0; i < n; ++i) {
        bool ok = row_valid(av, i) && row_valid(pv, i) && (!concat || row_valid(bv, i)) && !bd->null_pattern;
        res[i] = false;
        if (!ok) duckdb_validity_set_row_invalid(rvalid, i);
    }
    if (bd->null_pattern) return;

    const VocabState& vs = *bd->vocab;  // the snapshot the query was bound against
    st.counters.chunks.fetch_add(1, std::memory_order_relaxed);

    auto run = [&](const Matcher& m, idx_t i0, idx_t i1) {
        fill_column(sc->a, a, av, i0, i1, vs.v.n);
        Stats s{};
        if (concat) {
            fill_column(sc->b, b, bv, i0, i1, vs.v.n);
            m.match_concat(sc->a, sc->b, sc->bm, s);
        } else {
            m.match(sc->a, sc->bm, s);
        }
        for (idx_t r = 0; r < i1 - i0; ++r) res[i0 + r] = bitmap_bit(sc->bm, r);
        st.counters.rows.fetch_add(i1 - i0, std::memory_order_relaxed);
        st.counters.tokens_total.fetch_add(sc->a.tok.size() + (concat ? sc->b.tok.size() : 0),
                                           std::memory_order_relaxed);
        st.counters.tokens_read.fetch_add(s.tokens_read, std::memory_order_relaxed);
        st.counters.matches.fetch_add(s.matches, std::memory_order_relaxed);
    };

    // Constant pattern bound at bind time. The equality check guards the (theoretical) case of
    // a folded value whose C string differs from the runtime bytes, e.g. an embedded NUL.
    if (bd->matcher) {
        bool same = true;
        for (idx_t i = 0; i < n && same; ++i)
            if (row_valid(pv, i))
                same = slen(p[i]) == bd->pattern.size() && std::memcmp(sdata(p[i]), bd->pattern.data(), slen(p[i])) == 0;
        if (same) {
            run(*bd->matcher, 0, n);
            return;
        }
    }

    // Runtime patterns: resolve once per run of equal patterns.
    idx_t i = 0;
    while (i < n) {
        if (!row_valid(pv, i)) {
            ++i;
            continue;
        }
        std::string_view pat(sdata(p[i]), slen(p[i]));
        idx_t j = i + 1;
        while (j < n && row_valid(pv, j) && std::string_view(sdata(p[j]), slen(p[j])) == pat) ++j;
        MatcherPtr m = vs.get_matcher(pat, st.counters);
        st.counters.runtime_lookups.fetch_add(1, std::memory_order_relaxed);
        run(*m, i, j);  // NULL token rows were filled as empty rows; their output stays NULL
        i = j;
    }
}

void fn_token_like(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    try {
        run_like(info, input, output, false);
    } catch (const std::exception& e) {
        duckdb_scalar_function_set_error(info, e.what());
    } catch (...) {
        duckdb_scalar_function_set_error(info, "tokenlike: unknown error");
    }
}

void fn_token_like_concat(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    try {
        run_like(info, input, output, true);
    } catch (const std::exception& e) {
        duckdb_scalar_function_set_error(info, e.what());
    } catch (...) {
        duckdb_scalar_function_set_error(info, "tokenlike: unknown error");
    }
}

// ------------------------------------------------------------------ detokenize / tokenize

void fn_detokenize(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    try {
        ExtState& st = ext_from(duckdb_scalar_function_get_extra_info(info));
        auto* sc = static_cast<Scratch*>(duckdb_scalar_function_get_state(info));
        const VocabState& vs = bound_vocab(info);
        const Vocab& v = vs.v;
        const uint32_t* voff = v.off.data();
        const char* vbytes = vs.padded.data();
        const bool short_tokens = vs.max_len <= 16;
        idx_t n = duckdb_data_chunk_get_size(input);
        duckdb_vector vin = duckdb_data_chunk_get_vector(input, 0);
        auto* d = static_cast<const duckdb_string_t*>(duckdb_vector_get_data(vin));
        const uint64_t* iv = duckdb_vector_get_validity(vin);
        duckdb_vector_ensure_validity_writable(output);
        uint64_t* ov = duckdb_vector_get_validity(output);
        std::string& buf = sc->text;
        for (idx_t i = 0; i < n; ++i) {
            if (!row_valid(iv, i)) {
                duckdb_validity_set_row_invalid(ov, i);
                continue;
            }
            uint32_t len = slen(d[i]);
            if (len & 1)
                throw std::invalid_argument("detokenize: token BLOB has odd length " + std::to_string(len));
            const char* src = sdata(d[i]);
            // Decode into a buffer sized for the worst case (every token max_len bytes): no
            // per-token capacity checks, one memcpy per token.
            size_t need = size_t(len / 2) * vs.max_len + 16;
            if (buf.size() < need) buf.resize(need);
            char* w0 = buf.data();
            char* w = w0;
            for (uint32_t k = 0; k < len; k += 2) {
                uint16_t t;
                std::memcpy(&t, src + k, 2);
                if (t >= v.n)
                    throw std::out_of_range("detokenize: token id " + std::to_string(t) + " is outside the vocabulary");
                uint32_t a = voff[t], sz = voff[t + 1] - a;
                if (short_tokens)
                    std::memcpy(w, vbytes + a, 16);  // fixed size: a single unaligned 16-byte move
                else
                    std::memcpy(w, vbytes + a, sz);
                w += sz;
            }
            size_t out_len = static_cast<size_t>(w - w0);
            if (!valid_utf8_fast(w0, out_len))
                throw std::invalid_argument("detokenize: decoded bytes are not valid UTF-8");
            duckdb_unsafe_vector_assign_string_element_len(output, i, w0, out_len);
        }
        st.counters.decoded_rows.fetch_add(n, std::memory_order_relaxed);
    } catch (const std::exception& e) {
        duckdb_scalar_function_set_error(info, e.what());
    } catch (...) {
        duckdb_scalar_function_set_error(info, "detokenize: unknown error");
    }
}

// Greedy longest match. Any segmentation into vocabulary tokens is a valid encoding (TokenLIKE is
// segmentation-invariant); this one is not guaranteed to equal Person B's encoder output.
void fn_tokenize(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    try {
        auto* sc = static_cast<Scratch*>(duckdb_scalar_function_get_state(info));
        const VocabState* vs = &bound_vocab(info);
        idx_t n = duckdb_data_chunk_get_size(input);
        duckdb_vector vin = duckdb_data_chunk_get_vector(input, 0);
        auto* d = static_cast<const duckdb_string_t*>(duckdb_vector_get_data(vin));
        const uint64_t* iv = duckdb_vector_get_validity(vin);
        duckdb_vector_ensure_validity_writable(output);
        uint64_t* ov = duckdb_vector_get_validity(output);
        std::vector<uint8_t>& out = sc->blob;
        for (idx_t i = 0; i < n; ++i) {
            if (!row_valid(iv, i)) {
                duckdb_validity_set_row_invalid(ov, i);
                continue;
            }
            std::string_view s(sdata(d[i]), slen(d[i]));
            out.clear();
            size_t pos = 0;
            while (pos < s.size()) {
                size_t lim = std::min<size_t>(vs->max_len, s.size() - pos);
                for (size_t L = lim; L >= 1; --L) {
                    auto it = vs->enc.find(s.substr(pos, L));
                    if (it != vs->enc.end()) {
                        uint16_t t = it->second;
                        out.push_back(static_cast<uint8_t>(t & 0xFF));
                        out.push_back(static_cast<uint8_t>(t >> 8));
                        pos += L;
                        break;
                    }
                    // L == 1 always hits: load() checked all 256 single-byte tokens exist.
                }
            }
            duckdb_vector_assign_string_element_len(output, i, reinterpret_cast<const char*>(out.data()), out.size());
        }
    } catch (const std::exception& e) {
        duckdb_scalar_function_set_error(info, e.what());
    } catch (...) {
        duckdb_scalar_function_set_error(info, "tokenize: unknown error");
    }
}

// ------------------------------------------------------------------ admin functions

void assign_varchar(duckdb_vector out, idx_t i, const std::string& s) {
    duckdb_vector_assign_string_element_len(out, i, s.data(), s.size());
}

void fn_load_vocab(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    try {
        ExtState& st = ext_from(duckdb_scalar_function_get_extra_info(info));
        idx_t n = duckdb_data_chunk_get_size(input);
        duckdb_vector vin = duckdb_data_chunk_get_vector(input, 0);
        auto* d = static_cast<const duckdb_string_t*>(duckdb_vector_get_data(vin));
        const uint64_t* iv = duckdb_vector_get_validity(vin);
        auto* res = static_cast<int64_t*>(duckdb_vector_get_data(output));
        duckdb_vector_ensure_validity_writable(output);
        uint64_t* ov = duckdb_vector_get_validity(output);
        for (idx_t i = 0; i < n; ++i) {
            if (!row_valid(iv, i)) {
                duckdb_validity_set_row_invalid(ov, i);
                continue;
            }
            res[i] = st.load(std::string(sdata(d[i]), slen(d[i])));
        }
    } catch (const std::exception& e) {
        duckdb_scalar_function_set_error(info, e.what());
    } catch (...) {
        duckdb_scalar_function_set_error(info, "tokenlike_load_vocab: unknown error");
    }
}

void fn_info(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    try {
        ExtState& st = ext_from(duckdb_scalar_function_get_extra_info(info));
        auto vs = st.require();
        idx_t n = duckdb_data_chunk_get_size(input);
        duckdb_vector vin = duckdb_data_chunk_get_vector(input, 0);
        auto* d = static_cast<const duckdb_string_t*>(duckdb_vector_get_data(vin));
        const uint64_t* iv = duckdb_vector_get_validity(vin);
        duckdb_vector_ensure_validity_writable(output);
        uint64_t* ov = duckdb_vector_get_validity(output);
        for (idx_t i = 0; i < n; ++i) {
            if (!row_valid(iv, i)) {
                duckdb_validity_set_row_invalid(ov, i);
                continue;
            }
            std::string_view pat(sdata(d[i]), slen(d[i]));
            MatcherPtr m = vs->get_matcher(pat, st.counters);
            Column empty;
            empty.off.assign(1, 0);
            Bitmap bm;
            Stats s{};
            m->match(empty, bm, s);  // reports the matcher's stored build time and table size
            char buf[512];
            std::snprintf(buf, sizeof buf,
                          "\"kind\":\"%s\",\"dir\":\"%s\",\"states\":%u,\"exception_tokens\":%u,"
                          "\"table_kb\":%.1f,\"dense_table_kb\":%.1f,\"build_ms\":%.3f,\"vocab_n\":%u}",
                          kind_name(m->kind()),
                          tokenlike::detail::MatcherAccess::dir(*m) == tokenlike::Dir::Backward ? "backward" : "forward",
                          tokenlike::detail::MatcherAccess::states(*m),
                          tokenlike::detail::MatcherAccess::table(*m).exc, m->table_bytes() / 1024.0,
                          m->dense_table_bytes() / 1024.0, s.build_us / 1000.0, vs->v.n);
            assign_varchar(output, i, "{\"pattern\":\"" + json_escape(pat) + "\"," + buf);
        }
    } catch (const std::exception& e) {
        duckdb_scalar_function_set_error(info, e.what());
    } catch (...) {
        duckdb_scalar_function_set_error(info, "tokenlike_info: unknown error");
    }
}

void fn_stats(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    try {
        ExtState& st = ext_from(duckdb_scalar_function_get_extra_info(info));
        auto vs = st.current();
        size_t cached = 0;
        if (vs) {
            std::lock_guard<std::mutex> g(vs->mu);
            cached = vs->cache.size();
        }
        auto& c = st.counters;
        auto L = [](const std::atomic<uint64_t>& x) { return static_cast<unsigned long long>(x.load()); };
        char buf[1024];
        std::snprintf(buf, sizeof buf,
                      "{\"vocab_loaded\":%s,\"vocab_n\":%u,\"vocab_generation\":%llu,\"cached_matchers\":%zu,"
                      "\"matcher_builds\":%llu,\"cache_hits\":%llu,\"bind_constant\":%llu,\"runtime_lookups\":%llu,"
                      "\"chunks\":%llu,\"rows\":%llu,\"tokens_total\":%llu,\"tokens_read\":%llu,\"matches\":%llu,"
                      "\"decoded_rows\":%llu}",
                      vs ? "true" : "false", vs ? vs->v.n : 0u,
                      static_cast<unsigned long long>(vs ? vs->generation : 0), cached, L(c.matcher_builds),
                      L(c.cache_hits), L(c.bind_constant), L(c.runtime_lookups), L(c.chunks), L(c.rows),
                      L(c.tokens_total), L(c.tokens_read), L(c.matches), L(c.decoded_rows));
        idx_t n = duckdb_data_chunk_get_size(input);
        for (idx_t i = 0; i < n; ++i) assign_varchar(output, i, buf);
    } catch (const std::exception& e) {
        duckdb_scalar_function_set_error(info, e.what());
    } catch (...) {
        duckdb_scalar_function_set_error(info, "tokenlike_stats: unknown error");
    }
}

void fn_reset_stats(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    ExtState& st = ext_from(duckdb_scalar_function_get_extra_info(info));
    st.counters.reset();
    auto* res = static_cast<int64_t*>(duckdb_vector_get_data(output));
    idx_t n = duckdb_data_chunk_get_size(input);
    for (idx_t i = 0; i < n; ++i) res[i] = 0;
}

void fn_cache_clear(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    ExtState& st = ext_from(duckdb_scalar_function_get_extra_info(info));
    auto vs = st.current();
    int64_t cleared = vs ? static_cast<int64_t>(vs->clear_cache()) : 0;
    auto* res = static_cast<int64_t*>(duckdb_vector_get_data(output));
    idx_t n = duckdb_data_chunk_get_size(input);
    for (idx_t i = 0; i < n; ++i) res[i] = cleared;
}

// ------------------------------------------------------------------ scan-floor diagnostics
//
// Both return TRUE for every non-NULL row (NULL for NULL). They isolate the cost of the SQL path
// around the matcher: floor_noop is DuckDB's scan + one C-API scalar call per chunk; floor_copy
// adds exactly the per-row work token_like does before matching (validate the BLOB, copy its ids
// into the matcher's Column).

void floor_fill(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output, bool copy) {
    auto* sc = static_cast<Scratch*>(duckdb_scalar_function_get_state(info));
    idx_t n = duckdb_data_chunk_get_size(input);
    duckdb_vector vin = duckdb_data_chunk_get_vector(input, 0);
    const uint64_t* iv = duckdb_vector_get_validity(vin);
    auto* res = static_cast<bool*>(duckdb_vector_get_data(output));
    duckdb_vector_ensure_validity_writable(output);
    uint64_t* ov = duckdb_vector_get_validity(output);
    if (copy) {
        auto* d = static_cast<const duckdb_string_t*>(duckdb_vector_get_data(vin));
        fill_column(sc->a, d, iv, 0, n, bound_vocab(info).v.n);
    }
    for (idx_t i = 0; i < n; ++i) {
        res[i] = true;
        if (!row_valid(iv, i)) duckdb_validity_set_row_invalid(ov, i);
    }
}

void fn_floor_noop(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    try {
        floor_fill(info, input, output, false);
    } catch (const std::exception& e) {
        duckdb_scalar_function_set_error(info, e.what());
    } catch (...) {
        duckdb_scalar_function_set_error(info, "tokenlike_floor_noop: unknown error");
    }
}

void fn_floor_copy(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    try {
        floor_fill(info, input, output, true);
    } catch (const std::exception& e) {
        duckdb_scalar_function_set_error(info, e.what());
    } catch (...) {
        duckdb_scalar_function_set_error(info, "tokenlike_floor_copy: unknown error");
    }
}

// ------------------------------------------------------------------ registration

void destroy_state_ref(void* p) { delete static_cast<StateRef*>(p); }

struct FnSpec {
    const char* name;
    std::vector<duckdb_type> params;
    duckdb_type ret;
    duckdb_scalar_function_t fn;
    duckdb_scalar_function_bind_t bind;
    bool scratch;
    bool is_volatile;
};

bool register_fn(duckdb_connection con, const StateRef& st, const FnSpec& f) {
    duckdb_scalar_function sf = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(sf, f.name);
    for (duckdb_type t : f.params) {
        duckdb_logical_type lt = duckdb_create_logical_type(t);
        duckdb_scalar_function_add_parameter(sf, lt);
        duckdb_destroy_logical_type(&lt);
    }
    duckdb_logical_type rt = duckdb_create_logical_type(f.ret);
    duckdb_scalar_function_set_return_type(sf, rt);
    duckdb_destroy_logical_type(&rt);
    duckdb_scalar_function_set_function(sf, f.fn);
    duckdb_scalar_function_set_extra_info(sf, new StateRef(st), destroy_state_ref);
    if (f.bind) duckdb_scalar_function_set_bind(sf, f.bind);
    if (f.scratch) duckdb_scalar_function_set_init(sf, init_scratch);
    if (f.is_volatile) duckdb_scalar_function_set_volatile(sf);
    duckdb_state ok = duckdb_register_scalar_function(con, sf);
    duckdb_destroy_scalar_function(&sf);
    return ok == DuckDBSuccess;
}

}  // namespace

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection connection, duckdb_extension_info info,
                            struct duckdb_extension_access* access) {
    try {
        if (!host_is_little_endian()) {
            access->set_error(info, "tokenlike: token BLOBs are little-endian; big-endian hosts are unsupported");
            return false;
        }
        auto st = std::make_shared<ExtState>();
        if (const char* path = std::getenv("TOKENLIKE_VOCAB"); path && *path) st->load(path);

        const FnSpec specs[] = {
            {"tokenlike_load_vocab", {DUCKDB_TYPE_VARCHAR}, DUCKDB_TYPE_BIGINT, fn_load_vocab, nullptr, false, true},
            {"token_like", {DUCKDB_TYPE_BLOB, DUCKDB_TYPE_VARCHAR}, DUCKDB_TYPE_BOOLEAN, fn_token_like, bind_like, true, false},
            {"token_like_concat", {DUCKDB_TYPE_BLOB, DUCKDB_TYPE_BLOB, DUCKDB_TYPE_VARCHAR}, DUCKDB_TYPE_BOOLEAN,
             fn_token_like_concat, bind_like, true, false},
            {"tokenize", {DUCKDB_TYPE_VARCHAR}, DUCKDB_TYPE_BLOB, fn_tokenize, bind_vocab, true, false},
            {"detokenize", {DUCKDB_TYPE_BLOB}, DUCKDB_TYPE_VARCHAR, fn_detokenize, bind_vocab, true, false},
            {"tokenlike_info", {DUCKDB_TYPE_VARCHAR}, DUCKDB_TYPE_VARCHAR, fn_info, nullptr, false, true},
            {"tokenlike_stats", {}, DUCKDB_TYPE_VARCHAR, fn_stats, nullptr, false, true},
            {"tokenlike_reset_stats", {}, DUCKDB_TYPE_BIGINT, fn_reset_stats, nullptr, false, true},
            {"tokenlike_cache_clear", {}, DUCKDB_TYPE_BIGINT, fn_cache_clear, nullptr, false, true},
            {"tokenlike_floor_noop", {DUCKDB_TYPE_BLOB}, DUCKDB_TYPE_BOOLEAN, fn_floor_noop, bind_vocab, true, false},
            {"tokenlike_floor_copy", {DUCKDB_TYPE_BLOB}, DUCKDB_TYPE_BOOLEAN, fn_floor_copy, bind_vocab, true, false},
        };
        for (const FnSpec& f : specs) {
            if (!register_fn(connection, st, f)) {
                access->set_error(info, (std::string("tokenlike: failed to register ") + f.name).c_str());
                return false;
            }
        }
        return true;
    } catch (const std::exception& e) {
        access->set_error(info, e.what());
        return false;
    } catch (...) {
        access->set_error(info, "tokenlike: unknown error during load");
        return false;
    }
}
