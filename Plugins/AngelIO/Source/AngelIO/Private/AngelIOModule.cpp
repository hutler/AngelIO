#include "Modules/ModuleManager.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Tests/AngelIOStartupProbe.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include <atomic>
#include <cstdlib>
#include <malloc.h>
THIRD_PARTY_INCLUDES_START
#include "angelscript.h"
THIRD_PARTY_INCLUDES_END

namespace
{
    std::atomic<int64> LiveScriptBytes{0};
    bool bMeasureAllocator = false;
    void* MeasureAlloc(size_t Size)
    {
        void* Memory = std::malloc(Size);
        if (Memory) LiveScriptBytes.fetch_add(int64(_msize(Memory)), std::memory_order_relaxed);
        return Memory;
    }
    void MeasureFree(void* Memory)
    {
        if (Memory) LiveScriptBytes.fetch_sub(int64(_msize(Memory)), std::memory_order_relaxed);
        std::free(Memory);
    }
}
int64 FAngelIOStartupProbe::LiveSDKBytes() { return bMeasureAllocator ? LiveScriptBytes.load() : -1; }
int32* FAngelIOStartupProbe::QueuePeak = nullptr;

class FAngelIOModule : public FDefaultModuleImpl
{
    void StartupModule() override
    {
        if (FParse::Param(FCommandLine::Get(), TEXT("AngelIOProfileAllocator")))
            bMeasureAllocator = asSetGlobalMemoryFunctions(MeasureAlloc, MeasureFree) >= 0;
    }
    void ShutdownModule() override
    {
        if (bMeasureAllocator) asResetGlobalMemoryFunctions();
    }
};
IMPLEMENT_MODULE(FAngelIOModule, AngelIO)
#else
IMPLEMENT_MODULE(FDefaultModuleImpl, AngelIO)
#endif
