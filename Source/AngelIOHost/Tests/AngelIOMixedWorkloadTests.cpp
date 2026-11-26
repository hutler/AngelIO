#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AngelIOScriptComponent.h"
#include "AngelIOTargetRegistry.h"
#include "Tests/AngelIOStartupProbe.h"
#include "Tests/AutomationCommon.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformMemory.h"
#include "HAL/FileManager.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace
{
void ReportSamples(FAutomationTestBase& Test, const TCHAR* Label, TArray<double> Values)
{
    if (!Test.TestTrue(TEXT("Distribution contains samples"), !Values.IsEmpty())) return;
    Values.Sort();
    auto P = [&](double Fraction) { return Values[FMath::CeilToInt(Values.Num() * Fraction) - 1]; };
    Test.AddInfo(FString::Printf(TEXT("AngelIO mixed %s n=%d p50=%.4f p95=%.4f p99=%.4f max=%.4f ms"),
        Label, Values.Num(), P(0.50), P(0.95), P(0.99), Values.Last()));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOMixedWorkload, "AngelIO.Performance.MixedWaits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAngelIOMixedWorkload::RunTest(const FString&)
{
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Name = TEXT("MixedWaits-") + Token + TEXT(".as");
    const FString Script = FPaths::ProjectContentDir() / TEXT("AngelIO") / Name;
    const FString Slot = TEXT("MixedWaits-") + Token;
    const FString Database = FPaths::ProjectSavedDir() / TEXT("AngelIO/Checkpoints/Game") / (TEXT("slot-") + Slot + TEXT(".sqlite3"));
    const TCHAR* Source = TEXT(R"(enum InputSignal { Wake }
        enum OutputSignal { Progress }
        int in_Mode; int out_Count;
        void Observe() { while(true) { WaitFor(InputSignal::Wake); } }
        void Run() {
            if(in_Mode == 2) Fork(Observe);
            while(true) {
                WaitFor(InputSignal::Wake);
                if(in_Mode == 0) WaitSeconds(0.025f);
                if(in_Mode == 1) WaitActor("MixedAbsent", 0.025f);
                out_Count++;
                Emit(OutputSignal::Progress, out_Count);
            }
        })");
    if (!FFileHelper::SaveStringToFile(Source, *Script)) return false;
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*Script); IFileManager::Get().Delete(*Database); };
    FTestWorldWrapper Scene;
    if (!Scene.CreateTestWorld(EWorldType::Game) || !Scene.BeginPlayInTestWorld()) return false;
    auto* World = Scene.GetTestWorld();
    World->URL.AddOption(*(TEXT("AngelIOCheckpointSlot=") + Slot));
    const int64 BeforeRSS = int64(FPlatformMemory::GetStats().UsedPhysical);
    const int64 BeforeSDK = FAngelIOStartupProbe::LiveSDKBytes();
    constexpr int32 Count = 192;
    TArray<UAngelIOScriptComponent*> Missions;
    for (int32 I = 0; I < Count; ++I)
    {
        auto* Owner = World->SpawnActor<AActor>();
        auto* M = NewObject<UAngelIOScriptComponent>(Owner);
        Owner->AddInstanceComponent(M);
        M->Script = Name;
        M->SetTargetName(TEXT("MixedBurst"));
        FAngelIOValue Mode;
        Mode.Integer = I % 3;
        M->InitialInputs.Add(TEXT("Mode"), Mode);
        M->RegisterComponent();
        if (!TestEqual(TEXT("Waiting at startup"), M->GetWaitingSignal(), FString(TEXT("Wake")))) return false;
        Missions.Add(M);
    }
    const int64 SDK = FAngelIOStartupProbe::LiveSDKBytes();
    const int64 RSS = int64(FPlatformMemory::GetStats().UsedPhysical) - BeforeRSS;
    TArray<double> Frame, Fanout, Lag, Commit;
    int32 Queued = 0;
    TGuardValue<int32*> ObserveQueues(FAngelIOStartupProbe::QueuePeak, &Queued);
    for (int32 Burst = 1; Burst <= 24; ++Burst)
    {
        const double Begin = FPlatformTime::Seconds();
        const auto Sent = World->GetSubsystem<UAngelIOTargetRegistry>()->SendTo(TEXT("MixedBurst"), TEXT("Wake"), Burst);
        Fanout.Add((FPlatformTime::Seconds() - Begin) * 1000);
        if (!TestEqual(TEXT("Every endpoint received the burst"), Sent, Count)) return false;
        const double Due = World->GetTimeSeconds() + 0.025;
        bool bRecorded = false;
        for (int32 I = 0; I < 10; ++I)
        {
            const double Start = FPlatformTime::Seconds();
            Scene.TickTestWorld(1.f / 60.f);
            Frame.Add((FPlatformTime::Seconds() - Start) * 1000);
            FAngelIOValue V;
            if (!bRecorded && Missions[1]->GetOutput(TEXT("Count"), V) && V.Integer == Burst)
            {
                Lag.Add(1000 * (World->GetTimeSeconds() - Due));
                bRecorded = true;
            }
        }
        if (!TestTrue(TEXT("Native condition completed before next burst"), bRecorded)) return false;
    }
    for (const auto* M : Missions)
    {
        FAngelIOValue V;
        if (!TestTrue(TEXT("No lost, duplicated or starved wakes"), M->GetOutput(TEXT("Count"), V) && V.Integer == 24)) return false;
    }
    // The first real write includes opening the database and creating its schema.
    FString Error;
    uint8 Byte = 0;
    double Cold = 0;
    for (int32 I = 0; I <= 64; ++I)
    {
        Byte = uint8(I);
        const double Start = FPlatformTime::Seconds();
        const bool Ok = Missions[0]->SaveComponentState(TEXT("profile"), MakeArrayView(&Byte, 1), Error);
        const double Elapsed = (FPlatformTime::Seconds() - Start) * 1000;
        if (!TestTrue(*Error, Ok)) return false;
        if (I == 0) Cold = Elapsed;
        else Commit.Add(Elapsed);
    }
    int32 Contexts = Count;
    for (const auto* M : Missions) Contexts += M->InspectMission().BranchWaits.Num();
    TestEqual(TEXT("192 main contexts and 64 signal observers"), Contexts, 256);
    AddInfo(FString::Printf(TEXT("AngelIO mixed contexts=%d recipients=%d component_queue_peak=%d sdk_delta_bytes=%lld rss_delta_bytes=%lld first_sqlite_ms=%.4f"),
        Contexts, Count, Queued, SDK < 0 ? -1LL : SDK - BeforeSDK, RSS, Cold));
    ReportSamples(*this, TEXT("world_tick"), Frame);
    ReportSamples(*this, TEXT("signal_fanout"), Fanout);
    ReportSamples(*this, TEXT("resume_lag_game"), Lag);
    ReportSamples(*this, TEXT("warm_sqlite"), Commit);
    return true;
}
#endif
