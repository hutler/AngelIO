#pragma once

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "CoreMinimal.h"

class asIScriptModule;
struct FAngelIORegisteredEngine;
class UWorld;

// A synchronous benchmark or a PIE authority world owns each probe.
struct ANGELIO_API FAngelIOStartupProbe
{
    explicit FAngelIOStartupProbe(int32 InCapacity = 0) : Capacity(InCapacity) { check(InCapacity >= 0); }
    static FAngelIOStartupProbe*& Current();

    uint64 FileBegin = 0, CompileBegin = 0, EngineCreated = 0, CoreRegistered = 0, ProvidersInspected = 0;
    uint64 ModuleBegin = 0, ModuleEnd = 0, CompileEnd = 0, StartEnd = 0;
    bool bForcePrivateEngine = false;
    int32 Registrations = 0, Builds = 0, Loads = 0;
    int32 Evictions = 0;
    uint64 ComponentCycles = 0;
    int32 CacheEntries() const { return Entries.Num(); }
    int64 PayloadBytes() const;
    uint64 CacheHeapBytes() const;
    static TWeakPtr<FAngelIOStartupProbe> EnableWorldCapture(UWorld* World, int32 Capacity);
    static TSharedPtr<FAngelIOStartupProbe> WorldCapture(UWorld* World);
    static TWeakPtr<FAngelIORegisteredEngine> WorldEngine(UWorld* World);
    static int32 WorldModuleCount(UWorld* World);

    static void Mark(uint64 FAngelIOStartupProbe::* Field)
    {
        if (auto* Probe = Current()) Probe->*Field = FPlatformTime::Cycles64();
    }
    static int Build(asIScriptModule& Module, UWorld* World, const FString& Label, const FString& Source,
        const FString& Wrappers);

private:
    const int32 Capacity;
    struct FEntry
    {
        TWeakObjectPtr<UWorld> World;
        FString Label, Source, Wrappers;
        TArray<uint8> Bytes;
    };
    // Oldest successful insertion first. Recompiling a label replaces its old revision.
    TArray<FEntry> Entries;
};
#endif
