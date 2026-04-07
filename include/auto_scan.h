#pragma once
#include <string>
#include <cstdint>
#include <unordered_map>

// auto_scan.h — Offset cache + auto-discovery system
//
// Persists discovered offsets to phantom_cache.ini so subsequent launches
// skip the expensive memory scan (5-30 seconds).
// Detects game updates by comparing module size.

class OffsetCache
{
public:
    // Load/save the cache file (next to the exe)
    bool Load(const std::string& path = "phantom_cache.ini");
    bool Save(const std::string& path = "phantom_cache.ini");

    // Check if cache is stale (game module size changed = update)
    bool IsStale(uint64_t currentModuleSize) const;

    // Get/set offset values
    uint64_t Get(const std::string& key, uint64_t defaultVal = 0) const;
    void     Set(const std::string& key, uint64_t value);
    bool     Has(const std::string& key) const;
    size_t   Size() const { return offsets.size(); }

    // Invalidate all cached offsets (keeps meta)
    void InvalidateAll();

    // Store module size for staleness detection
    void SetModuleSize(uint64_t size);
    uint64_t GetModuleSize() const;

private:
    uint64_t cachedModuleSize = 0;
    std::unordered_map<std::string, uint64_t> offsets;
};

// Auto-scan wrappers (use cache, fallback to full scan)
// These are called from Game::GameStart()

// Wraps ScanForVPMatrix: checks cache first, validates, full scan if needed.
// Returns the VP address (0 if not found).
uint64_t AutoScan_VPMatrix(OffsetCache& cache, uint64_t gameBase, uint32_t gamePid);
