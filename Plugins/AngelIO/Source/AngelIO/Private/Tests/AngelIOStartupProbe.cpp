#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Tests/AngelIOStartupProbe.h"
#include "Engine/World.h"

THIRD_PARTY_INCLUDES_START
#include "angelscript.h"
THIRD_PARTY_INCLUDES_END

namespace
{
    class FBytecodeStream final : public asIBinaryStream
    {
    public:
        explicit FBytecodeStream(TArray<uint8>& InBytes) : Bytes(InBytes) {}

        int Read(void* Destination, asUINT Size) override
        {
            if (Size > static_cast<asUINT>(Bytes.Num() - Offset)) return -1;
            if (Size) FMemory::Memcpy(Destination, Bytes.GetData() + Offset, Size);
            Offset += Size;
            return 0;
        }

        int Write(const void* Source, asUINT Size) override
        {
            if (Size > static_cast<asUINT>(MAX_int32 - Bytes.Num())) return -1;
            Bytes.Append(static_cast<const uint8*>(Source), Size);
            return 0;
        }

    private:
        TArray<uint8>& Bytes;
        int32 Offset = 0;
    };
}

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

uint64 FAngelIOStartupProbe::CacheHeapBytes() const
{
    uint64 Total = Entries.GetAllocatedSize();
    for (const auto& Entry : Entries)
        Total += Entry.Label.GetAllocatedSize() + Entry.Source.GetAllocatedSize() +
            Entry.Wrappers.GetAllocatedSize() + Entry.Bytes.GetAllocatedSize();
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
        FBytecodeStream Stream(Probe.Entries[Index].Bytes);
        Result = Module.LoadByteCode(&Stream);
    }
    else
    {
        ++Probe.Builds;
        Result = Module.Build();
        if (Result >= 0 && Probe.Capacity > 0)
        {
            TArray<uint8> Bytes;
            FBytecodeStream Stream(Bytes);
            Result = Module.SaveByteCode(&Stream, /*stripDebugInfo=*/false);
            if (Result >= 0)
            {
                if (Index != INDEX_NONE)
                    Probe.Entries.RemoveAt(Index, EAllowShrinking::No);
                else if (Probe.Entries.Num() == Probe.Capacity)
                {
                    Probe.Entries.RemoveAt(0, EAllowShrinking::No);
                    ++Probe.Evictions;
                }
                Probe.Entries.Add({World, Label, Source, Wrappers, MoveTemp(Bytes)});
            }
        }
    }
    Probe.ModuleEnd = FPlatformTime::Cycles64();
    return Result;
}
#endif
