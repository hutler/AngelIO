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

int FAngelIOStartupProbe::Build(asIScriptModule& Module, UWorld* World, const FString& Label,
    const FString& Source, const FString& Wrappers)
{
    auto* Active = Current();
    if (!Active) return Module.Build();
    auto& Probe = *Active;
    Probe.ModuleBegin = FPlatformTime::Cycles64();
    int Result;
    if (Probe.bReuseBytecode && !Probe.Bytecode.IsEmpty() && Probe.CachedWorld == TWeakObjectPtr<UWorld>(World) &&
        Probe.CachedLabel.Equals(Label, ESearchCase::CaseSensitive) &&
        Probe.CachedSource.Equals(Source, ESearchCase::CaseSensitive) &&
        Probe.CachedWrappers.Equals(Wrappers, ESearchCase::CaseSensitive))
    {
        ++Probe.Loads;
        FBytecodeStream Stream(Probe.Bytecode);
        Result = Module.LoadByteCode(&Stream);
    }
    else
    {
        ++Probe.Builds;
        Result = Module.Build();
        if (Result >= 0 && Probe.bReuseBytecode)
        {
            TArray<uint8> Bytes;
            FBytecodeStream Stream(Bytes);
            Result = Module.SaveByteCode(&Stream, /*stripDebugInfo=*/false);
            if (Result >= 0)
            {
                Probe.Bytecode = MoveTemp(Bytes);
                Probe.CachedWorld = World;
                Probe.CachedLabel = Label;
                Probe.CachedSource = Source;
                Probe.CachedWrappers = Wrappers;
            }
        }
    }
    Probe.ModuleEnd = FPlatformTime::Cycles64();
    return Result;
}
#endif
