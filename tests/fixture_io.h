// TEST FIXTURES ONLY. Writes/reads small synthetic datasets in the agreed
// .tok/.off/.raw/.rawoff layout so the baselines and CLI can be exercised
// before Person B's loaders land. This is NOT the official data pipeline:
// the vocab.bin layout used here (uint32 n, uint32 off[n+1], bytes) is a
// placeholder. When B's vocab.cpp/column.cpp exist the CLI uses those instead.
#pragma once

#include "tokenlike/common.h"

#include <string>
#include <vector>

// Vocab of all 256 single bytes (ids 0..255) plus the given multi-byte tokens.
Vocab fixture_vocab(const std::vector<std::string>& extra_tokens);

// Greedy longest-match tokenization of `rows` with `v`.
Column fixture_column(const Vocab& v, const std::vector<std::string>& rows);

void write_vocab(const std::string& path, const Vocab& v);
void write_column(const std::string& prefix, const Column& c);

// Same signatures as Person B's loaders.
Vocab load_vocab(const std::string& path);
Column load_column(const std::string& prefix);
