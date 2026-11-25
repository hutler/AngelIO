#pragma once

#include "CoreMinimal.h"

class UWorld;

struct FAngelIOBytecodeCacheStats
{
    uint64 BudgetBytes = 0, RetainedBytes = 0;
    uint64 Hits = 0, SourceBuilds = 0, Evictions = 0, Oversized = 0;
    int32 Entries = 0;
};

namespace AngelIO
{
    // Game-thread snapshot. Empty before engine registration, on clients and after teardown.
    ANGELIO_API TOptional<FAngelIOBytecodeCacheStats> GetBytecodeCacheStats(UWorld* World);
}
