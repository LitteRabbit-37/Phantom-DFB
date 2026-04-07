// auto_scan.cpp — Offset cache + auto-discovery
//
// Persist discovered offsets to phantom_cache.ini.
// Detect game updates by module size change.

#include "auto_scan.h"
#include "vmmProc.h"
#include "Offset.h"
#include "stealth.h"
#include <fstream>
#include <sstream>
#include <cmath>

// ============================================================================
// OffsetCache — simple INI-like persistence
// ============================================================================

bool OffsetCache::Load(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open())
        return false;

    std::string line;
    while (std::getline(f, line))
    {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == '[')
            continue;

        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "module_size")
            cachedModuleSize = strtoull(val.c_str(), nullptr, 0);
        else
            offsets[key] = strtoull(val.c_str(), nullptr, 0);
    }

    DBG("[Cache] Loaded %zu offsets from %s\n", offsets.size(), path.c_str());
    return true;
}

bool OffsetCache::Save(const std::string& path)
{
    std::ofstream f(path);
    if (!f.is_open())
        return false;

    f << "# Phantom DFB offset cache — auto-generated\n";
    f << "# Delete this file to force a full rescan.\n\n";
    f << "[meta]\n";
    f << "module_size=" << cachedModuleSize << "\n\n";
    f << "[offsets]\n";

    for (auto& [key, val] : offsets)
        f << key << "=0x" << std::hex << val << "\n";

    DBG("[Cache] Saved %zu offsets to %s\n", offsets.size(), path.c_str());
    return true;
}

bool OffsetCache::IsStale(uint64_t currentModuleSize) const
{
    if (cachedModuleSize == 0)
        return true;  // No cache yet
    return cachedModuleSize != currentModuleSize;
}

uint64_t OffsetCache::Get(const std::string& key, uint64_t defaultVal) const
{
    auto it = offsets.find(key);
    return (it != offsets.end()) ? it->second : defaultVal;
}

void OffsetCache::Set(const std::string& key, uint64_t value)
{
    offsets[key] = value;
}

bool OffsetCache::Has(const std::string& key) const
{
    return offsets.find(key) != offsets.end();
}

void OffsetCache::InvalidateAll()
{
    offsets.clear();
    DBG("[Cache] All offsets invalidated (game update detected)\n");
}

void OffsetCache::SetModuleSize(uint64_t size)
{
    cachedModuleSize = size;
}

uint64_t OffsetCache::GetModuleSize() const
{
    return cachedModuleSize;
}

// ============================================================================
// AutoScan_VPMatrix — cached VP matrix discovery
// ============================================================================

uint64_t AutoScan_VPMatrix(OffsetCache& cache, uint64_t gameBase, uint32_t gamePid)
{
    // Check cache first
    uint64_t cached = cache.Get("vp_cb_address");
    if (cached)
    {
        // Validate: read 64 bytes, check it's a plausible VP matrix
        float vp[16] = {};
        if (ReadMemory(cached, vp, 64))
        {
            int nonzero = 0;
            int negatives = 0;
            bool valid = true;
            float minV = vp[0], maxV = vp[0];

            for (int i = 0; i < 16; i++)
            {
                if (vp[i] != vp[i] || fabsf(vp[i]) > 100000.f) { valid = false; break; }
                if (fabsf(vp[i]) > 0.001f) nonzero++;
                if (vp[i] < -0.01f) negatives++;
                if (vp[i] < minV) minV = vp[i];
                if (vp[i] > maxV) maxV = vp[i];
            }

            if (valid && nonzero >= 10 && negatives >= 2 && (maxV - minV) > 1.0f)
            {
                // Freshness test: VP should change between reads (camera moves)
                Sleep(150);
                float vp2[16] = {};
                if (ReadMemory(cached, vp2, 64))
                {
                    bool changed = false;
                    for (int i = 0; i < 16; i++)
                        if (fabsf(vp[i] - vp2[i]) > 0.0001f) { changed = true; break; }

                    if (!changed)
                    {
                        DBG("[Cache] VP at 0x%llX is STATIC (not updating) -- discarding, will rescan\n",
                            (unsigned long long)cached);
                        return 0;  // Force full rescan with freshness validation
                    }
                }

                DBG("[Cache] VP at 0x%llX still valid (%d/16 nonzero, dynamic)\n",
                    (unsigned long long)cached, nonzero);
                return cached;
            }
        }
        DBG("[Cache] VP at 0x%llX is stale, rescanning...\n", (unsigned long long)cached);
    }

    // Cache miss or stale — need the Game object to call ScanForVPMatrix.
    // Return 0 to signal that GameStart should call ScanForVPMatrix().
    return 0;
}
