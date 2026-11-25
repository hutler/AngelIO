#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Tests/AngelIOStartupProbe.h"
#include "AngelIOBytecodeStream.h"
#include "Engine/World.h"

FAngelIOStartupProbe*& FAngelIOStartupProbe::Current()
{
    static thread_local FAngelIOStartupProbe* Active = nullptr;
    return Active;
}

int64 FAngelIOStartupProbe::PayloadBytes() const
{
    int64 Total = 0;
    for (const auto& Entry : Entries) Total += Entry.Bytes.Num();
    return Total;
}

int FAngelIOStartupProbe::Build(asIScriptModule& Module, UWorld* World, const FString& Label,
    const FString& Source, const FString& Wrappers)
{
    auto* Active = Current();
    if (!Active) return Module.Build();
    auto& Probe = *Active;
    Probe.ModuleBegin = FPlatformTime::Cycles64();
    int Result;
    const int32 Index = Probe.Entries.IndexOfByPredicate([&](const FEntry& Entry)
    {
        return Entry.World == TWeakObjectPtr<UWorld>(World) && Entry.Label.Equals(Label, ESearchCase::CaseSensitive);
    });
    if (Index != INDEX_NONE && Probe.Entries[Index].Source.Equals(Source, ESearchCase::CaseSensitive) &&
        Probe.Entries[Index].Wrappers.Equals(Wrappers, ESearchCase::CaseSensitive))
    {
        ++Probe.Loads;
        FAngelIOBytecodeStream Stream(Probe.Entries[Index].Bytes);
        Result = Module.LoadByteCode(&Stream);
    }
    else
    {
        ++Probe.Builds;
        Result = Module.Build();
        if (Result >= 0 && Probe.Capacity > 0)
        {
            TArray<uint8> Bytes;
            FAngelIOBytecodeStream Stream(Bytes);
            Result = Module.SaveByteCode(&Stream, /*stripDebugInfo=*/false);
            if (Result >= 0)
            {
                if (Index != INDEX_NONE)
                    Probe.Entries.RemoveAt(Index, EAllowShrinking::No);
                else if (Probe.Entries.Num() == Probe.Capacity)
                    Probe.Entries.RemoveAt(0, EAllowShrinking::No);
                Probe.Entries.Add({World, Label, Source, Wrappers, MoveTemp(Bytes)});
            }
        }
    }
    Probe.ModuleEnd = FPlatformTime::Cycles64();
    return Result;
}
#endif
