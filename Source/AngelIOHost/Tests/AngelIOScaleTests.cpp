#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AngelIOScriptComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMemory.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOScale, "AngelIO.Performance.WaitingMissions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOScale::RunTest(const FString&)
{
    for (const int32 Count : {1, 100, 1000})
    {
        FTestWorldWrapper Scene;
        if (!Scene.CreateTestWorld(EWorldType::Game) || !Scene.BeginPlayInTestWorld()) return false;
        UWorld* World = Scene.GetTestWorld();
        World->URL.AddOption(TEXT("AngelIOCheckpointSlot="));
        auto FrameMs = [&Scene]
        {
            constexpr int32 Frames = 120;
            const double Start = FPlatformTime::Seconds();
            for (int32 I = 0; I < Frames; ++I) Scene.TickTestWorld(1.f / 60.f);
            return (FPlatformTime::Seconds() - Start) * 1000 / Frames;
        };
        const double Baseline = FrameMs();
        const int64 Before = FPlatformMemory::GetStats().UsedPhysical;
        const double Start = FPlatformTime::Seconds();
        TArray<UAngelIOScriptComponent*> Missions;
        for (int32 I = 0; I < Count; ++I)
        {
            auto* Actor = World->SpawnActor<AActor>();
            auto* Mission = NewObject<UAngelIOScriptComponent>(Actor);
            Actor->AddInstanceComponent(Mission);
            Mission->Script = TEXT("Examples/TimedGate.as");
            FAngelIOValue Delay;
            Delay.Type = EAngelIOType::Float;
            Delay.Float = 60.f;
            Mission->InitialInputs.Add(TEXT("Delay"), Delay);
            Mission->RegisterComponent();
            Missions.Add(Mission);
        }
        const double StartMs = (FPlatformTime::Seconds() - Start) * 1000;
        const int64 Bytes = int64(FPlatformMemory::GetStats().UsedPhysical) - Before;
        const double WaitingMs = FrameMs();
        if (!TestTrue(TEXT("Every independent VM is still waiting"), !Missions.ContainsByPredicate([](const auto* M)
            { return M->GetWaitingSignal() != TEXT("WaitSeconds") || !M->IsComponentTickEnabled(); }))) return false;
        AddInfo(FString::Printf(TEXT("AngelIO benchmark count=%d startup_ms=%.3f resident_delta_mib=%.2f baseline_frame_ms=%.4f waiting_frame_ms=%.4f"),
            Count, StartMs, double(Bytes) / (1024.0 * 1024.0), Baseline, WaitingMs));
    }
    return true;
}
#endif
