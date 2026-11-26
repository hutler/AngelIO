#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "../AngelIOExampleLift.h"
#include "AngelIOScriptComponent.h"
#include "AngelIOTraceSubsystem.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "Tests/AutomationCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOValidatedRestart, "AngelIO.Editor.ValidatedRestart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAngelIOValidatedRestart::RunTest(const FString&)
{
    FTestWorldWrapper Scene;
    if (!Scene.CreateTestWorld(EWorldType::Game) || !Scene.BeginPlayInTestWorld()) return false;
    auto* World = Scene.GetTestWorld();
    World->URL.AddOption(TEXT("AngelIOCheckpointSlot="));
    auto* Lift = World->SpawnActorDeferred<AAngelIOExampleLift>(AAngelIOExampleLift::StaticClass(), FTransform::Identity);
    Lift->Address->SetTargetName(TEXT("Receiver"));
    Lift->bRequestOnStart = false;
    Lift->FinishSpawning(FTransform::Identity);
    const FString Name = TEXT("Restart-") + FGuid::NewGuid().ToString() + TEXT(".as");
    const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("AngelIO") / Name);
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*File); };
    const FString Old = TEXT("int out_Seen;\nvoid Run(){\n SaveCheckpoint(\"Moving\",1);\n")
        TEXT(" FindLift(\"Receiver\").MoveTo(4,800).Wait();\n out_Seen=1;\n}");
    if (!FFileHelper::SaveStringToFile(Old, *File)) return false;
    auto* Actor = World->SpawnActor<AActor>();
    auto* M = NewObject<UAngelIOScriptComponent>(Actor);
    Actor->AddInstanceComponent(M);
    M->Script = Name;
    M->RegisterComponent();
    const auto Before = M->InspectMission();
    if (!TestTrue(TEXT("Running at the original ride wait"), Before.Operation.IsValid() && Before.Line == 4)) return false;
    if (!FFileHelper::SaveStringToFile(TEXT("void Run() { unavailable(); }"), *File)) return false;
    FString Result, Path, Text;
    TestTrue(TEXT("Invalid candidate reports diagnostics and preserves live run and native wait"), !M->RestartValidated(Result) &&
        Result.Contains(TEXT("unavailable")) && M->InspectMission().Instance == Before.Instance && M->InspectMission().Operation == Before.Operation);
    int32 Line = 0;
    TestTrue(TEXT("Open the exact old revision and line despite edited source"), M->ResolveRunningSource(Path, Line) && Path != File &&
        Line == 4 && FFileHelper::LoadFileToString(Text, *Path) && Text == Old);
    const FString New = TEXT("enum InputSignal { Confirm } int out_Seen;\nvoid Run(){out_Seen=RequireCheckpoint(1)==\"Moving\"?2:0;\n WaitFor(InputSignal::Confirm);}");
    if (!FFileHelper::SaveStringToFile(New, *File)) return false;
    FAngelIOValue V;
    TestTrue(TEXT("Only a valid candidate replaces the instance and restores its checkpoint"), M->RestartValidated(Result) &&
        M->InspectMission().Instance != Before.Instance && M->GetOutput(TEXT("Seen"), V) && V.Integer == 2 &&
        M->InspectMission().SourceRevision != Before.SourceRevision && M->GetWaitingSignal() == TEXT("Confirm"));
    TestTrue(TEXT("Matching working copy opens directly"), M->ResolveRunningSource(Path, Line) && FPaths::IsSamePath(Path, File) && Line == 3);
    for (int32 I = 0; I < 90; ++I) Scene.TickTestWorld(1.f/60);
    TestTrue(TEXT("Original lift finished once after its reader detached"), Lift->Mission->GetOutput(TEXT("LastFloor"), V) &&
        V.Integer == 4 && Lift->Mission->GetOutput(TEXT("TotalTrips"), V) && V.Integer == 1);
    int32 Detaches = 0;
    World->GetSubsystem<UAngelIOTraceSubsystem>()->VisitEvents([&](const FJsonObject& E, const UObject*)
    {
        FString Id;
        if (E.GetStringField(TEXT("type")) == TEXT("detach") && E.TryGetStringField(TEXT("operationId"), Id) &&
            Id == Before.Operation.ToString(EGuidFormats::DigitsWithHyphensLower)) ++Detaches;
    });
    TestEqual(TEXT("Exactly one old reader detached"), Detaches, 1);
    return true;
}
#endif
