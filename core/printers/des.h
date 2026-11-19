// Single DES, encryption of one 8-byte block (FIPS 46-3). Only used for the HALOT token,
// which the printer expects in this exact form; DES is not used to protect anything here.
#pragma once

#include <array>
#include <cstdint>

namespace resindeck::des {

using Block = std::array<std::uint8_t, 8>;

Block encrypt(const Block &key, const Block &plain);

}  // namespace resindeck::des
