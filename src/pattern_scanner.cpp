#include "pattern_scanner.h"
#include "vmmProc.h"
#include <cstdio>
#include <cstring>
#include <sstream>

static constexpr uint64_t SCAN_CHUNK = 0x100000;    // 1 MB per read
static constexpr uint64_t SCAN_OVERLAP = 64;         // overlap between chunks

// ---------------------------------------------------------------------------
// ParsePattern
// ---------------------------------------------------------------------------
bool ParsePattern(const std::string& pattern,
                  std::vector<uint8_t>& outBytes,
                  std::vector<bool>&    outMask)
{
    outBytes.clear();
    outMask.clear();

    std::istringstream ss(pattern);
    std::string token;
    while (ss >> token)
    {
        if (token == "??")
        {
            outBytes.push_back(0x00);
            outMask.push_back(false);
        }
        else if (token.size() == 2)
        {
            for (char c : token)
            {
                if (!isxdigit((unsigned char)c))
                    return false;
            }
            outBytes.push_back((uint8_t)strtoul(token.c_str(), nullptr, 16));
            outMask.push_back(true);
        }
        else
        {
            return false;
        }
    }
    return !outBytes.empty();
}

// ---------------------------------------------------------------------------
// PatternScan
// ---------------------------------------------------------------------------
uint64_t PatternScan(uint64_t base, uint64_t size, const std::string& pattern)
{
    std::vector<uint8_t> patBytes;
    std::vector<bool>    patMask;
    if (!ParsePattern(pattern, patBytes, patMask))
    {
        printf("[!] PatternScan: malformed pattern: %s\n", pattern.c_str());
        return 0;
    }

    const size_t patLen = patBytes.size();
    if (patLen == 0)
        return 0;

    std::vector<uint8_t> chunk;
    chunk.resize(SCAN_CHUNK + SCAN_OVERLAP);

    uint64_t offset = 0;
    while (offset < size)
    {
        uint64_t readVA   = base + offset;
        uint64_t readSize = SCAN_CHUNK + SCAN_OVERLAP;
        if (offset + readSize > size)
            readSize = size - offset;

        if (!ReadMemory(readVA, chunk.data(), (DWORD)readSize))
        {
            offset += 0x1000;
            continue;
        }

        for (size_t i = 0; i + patLen <= readSize; i++)
        {
            bool found = true;
            for (size_t j = 0; j < patLen; j++)
            {
                if (patMask[j] && chunk[i + j] != patBytes[j])
                {
                    found = false;
                    break;
                }
            }
            if (found)
                return readVA + i;
        }

        if (readSize <= SCAN_OVERLAP)
            break;
        offset += readSize - SCAN_OVERLAP;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// ResolveRIP
// ---------------------------------------------------------------------------
uint64_t ResolveRIP(uint64_t instrVA, int instrLen, int ripOff)
{
    int32_t disp = 0;
    ReadMemory(instrVA + ripOff, &disp, sizeof(disp));
    return instrVA + instrLen + disp;
}
