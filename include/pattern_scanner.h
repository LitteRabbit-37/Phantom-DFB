#pragma once
#include <string>
#include <vector>
#include <cstdint>

// Generic IDA-style pattern scanner.
//
// Pattern format: space-separated hex bytes, '??' = wildcard.
// Example: "48 8B 05 ?? ?? ?? ?? 48 85 C0"

// Parse a pattern string into byte/mask vectors.
bool ParsePattern(const std::string& pattern,
                  std::vector<uint8_t>& outBytes,
                  std::vector<bool>&    outMask);

// Scan [base, base+size) for pattern. Returns first match VA or 0.
uint64_t PatternScan(uint64_t base, uint64_t size, const std::string& pattern);

// Resolve RIP-relative address from a scanned instruction.
uint64_t ResolveRIP(uint64_t instrVA, int instrLen, int ripOff);
