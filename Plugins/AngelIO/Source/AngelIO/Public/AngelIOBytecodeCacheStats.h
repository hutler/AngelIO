#pragma once

#include "CoreMinimal.h"

class UWorld;

struct FAngelIOBytecodeCacheStats
{
    uint64 BudgetBytes = 0, RetainedBytes = 0;
    uint64 Hits = 0, SourceBuilds = 0, Evictions = 0, Oversized = 0;
    uint64 InspectionRevision = 0; // Changes when residency, LRU order or activity changes.
    int32 Entries = 0;
};

struct FAngelIOBytecodeCacheEntry
{
    FString Script;
    uint64 RetainedBytes = 0;
};

enum class EAngelIOBytecodeCacheAction { Hit, Eviction };

struct FAngelIOBytecodeCacheActivity
{
    uint64 Sequence = 0;
    EAngelIOBytecodeCacheAction Action = EAngelIOBytecodeCacheAction::Hit;
    FAngelIOBytecodeCacheEntry Entry;
};

struct FAngelIOBytecodeCacheSnapshot : FAngelIOBytecodeCacheStats
{
    TArray<FAngelIOBytecodeCacheEntry> ResidentScripts; // Most recently used first.
    TArray<FAngelIOBytecodeCacheActivity> RecentActivity; // Oldest first.
    uint64 DroppedActivity = 0;
};

namespace AngelIO
{
    // Game-thread snapshot. Empty before engine registration, on clients and after teardown.
    ANGELIO_API TOptional<FAngelIOBytecodeCacheStats> GetBytecodeCacheStats(UWorld* World);
    // Detailed non-shipping snapshot. Entry sizes use the same allocator charges as RetainedBytes.
    ANGELIO_API TOptional<FAngelIOBytecodeCacheSnapshot> GetBytecodeCacheSnapshot(UWorld* World);
}
