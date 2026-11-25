#pragma once

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "CoreMinimal.h"

class asIScriptModule;

// Active only during synchronous registration of a benchmark component on this thread.
struct ANGELIO_API FAngelIOStartupProbe
{
    static FAngelIOStartupProbe*& Current();

    uint64 FileBegin = 0, CompileBegin = 0, EngineCreated = 0, CoreRegistered = 0, ProvidersInspected = 0;
    uint64 ModuleBegin = 0, ModuleEnd = 0, CompileEnd = 0, StartEnd = 0;
    bool bReuseBytecode = false;
    int32 Builds = 0, Loads = 0;
    TArray<uint8> Bytecode;

    static void Mark(uint64 FAngelIOStartupProbe::* Field)
    {
        if (auto* Probe = Current()) Probe->*Field = FPlatformTime::Cycles64();
    }
    static int Build(asIScriptModule& Module);
};
#endif
