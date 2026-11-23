#if WITH_DEV_AUTOMATION_TESTS
#include "AngelIOExampleLift.h"
#include "AngelIOScriptComponent.h"
#include "AngelIOTargetRegistry.h"
#include "Tests/AngelIOTestSignalObserver.h"
#include "Tests/AutomationCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOLifts, "AngelIO.Missions.TwoLifts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOLifts::RunTest(const FString&)
{
    FTestWorldWrapper Scene;
    if (!TestTrue(TEXT("Create game world"), Scene.CreateTestWorld(EWorldType::Game))) return false;
    auto* A = Scene.GetTestWorld()->SpawnActor<AAngelIOExampleLift>();
    auto* B = Scene.GetTestWorld()->SpawnActor<AAngelIOExampleLift>();
    if (!A || !B) return false;
    A->bRequestOnStart = B->bRequestOnStart = false;
    FAngelIOValue StartFloor;
    StartFloor.Integer = 4;
    B->Mission->InitialInputs.Add(TEXT("StartingFloor"), StartFloor);
    if (!TestTrue(TEXT("Begin play"), Scene.BeginPlayInTestWorld())) return false;
    auto Read = [&](AAngelIOExampleLift* Lift, const FString& Name)
    {
        FAngelIOValue Value;
        TestTrue(*Name, Lift->Mission->GetOutput(Name, Value));
        return Value.Integer;
    };
    TestEqual(TEXT("First instance default"), Read(A, TEXT("LastFloor")), 1);
    TestEqual(TEXT("Second instance override"), Read(B, TEXT("LastFloor")), 4);
    TestFalse(TEXT("Unmatched notification"), A->Mission->SendSignal(TEXT("MotionFinished"), 3));
    TestTrue(TEXT("Immediate response from external module"), A->RequestFloor(1));
    TestEqual(TEXT("Trip counted once"), Read(A, TEXT("TotalTrips")), 1);
    TestEqual(TEXT("Other lift independent"), Read(B, TEXT("TotalTrips")), 0);
    TestFalse(TEXT("Reject wrong input type"), B->Mission->SetInput(TEXT("Enabled"), StartFloor));
    FAngelIOValue Enabled;
    Enabled.Type = EAngelIOType::Boolean;
    TestTrue(TEXT("Change bool while waiting"), B->Mission->SetInput(TEXT("Enabled"), Enabled));
    TestTrue(TEXT("Disabled lift consumes request"), B->RequestFloor(6));
    TestEqual(TEXT("Still waiting for a request"), B->Mission->GetWaitingSignal(), FString(TEXT("RequestFloor")));
    Enabled.Boolean = true;
    B->Mission->SetInput(TEXT("Enabled"), Enabled);
    TestTrue(TEXT("Start delayed trip"), B->RequestFloor(5));
    TestTrue(TEXT("Start independent trip"), A->RequestFloor(2));
    TestEqual(TEXT("Script suspended during motion"), B->Mission->GetWaitingSignal(), FString(TEXT("MotionFinished")));
    A->Destroy();
    TestFalse(TEXT("Destroyed lift rejects late event"), A->Mission->SendSignal(TEXT("MotionFinished"), 2));
    for (int32 Frame = 0; Frame < 100; ++Frame) Scene.TickTestWorld(0.02f);
    TestEqual(TEXT("Surviving lift completed motion"), Read(B, TEXT("LastFloor")), 5);
    TestEqual(TEXT("Surviving lift counted one trip"), Read(B, TEXT("TotalTrips")), 1);
    Scene.ForwardErrorMessages(this);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIONamedTargets, "AngelIO.Addressing.SpawnRenameDestroy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIONamedTargets::RunTest(const FString&)
{
    FTestWorldWrapper Scene;
    if (!Scene.CreateTestWorld(EWorldType::Game) || !Scene.BeginPlayInTestWorld()) return false;
    UWorld* World = Scene.GetTestWorld();
    auto* Registry = World->GetSubsystem<UAngelIOTargetRegistry>();
    auto* Owner = World->SpawnActor<AActor>();
    auto* Endpoint = NewObject<UAngelIOScriptComponent>(Owner);
    Endpoint->bStartOnBeginPlay = false;
    Endpoint->SetTargetName(TEXT("Hall"));
    TStrongObjectPtr<UAngelIOTestSignalObserver> Listener(NewObject<UAngelIOTestSignalObserver>());
    Endpoint->OnNamedInput.AddDynamic(Listener.Get(), &UAngelIOTestSignalObserver::OnGameplay);
    TestEqual(TEXT("No delivery before registration"), Registry->SendTo(TEXT("Hall"), TEXT("Arrived"), 4), 0);
    Endpoint->RegisterComponent();
    TestEqual(TEXT("Spawn registers component"), Registry->FindTargets(TEXT("HALL")).Num(), 1);
    TestEqual(TEXT("Past events are not replayed"), Listener->GameplayEvents, 0);
    TestEqual(TEXT("Name selects a native endpoint"), Registry->SendTo(TEXT("Hall"), TEXT("Arrived"), 4), 1);
    TestEqual(TEXT("Native listener with no running script"), Listener->GameplayEvents, 1);
    Endpoint->SetTargetName(TEXT("Lobby"));
    TestTrue(TEXT("Rename releases old address"), Registry->FindTargets(TEXT("Hall")).IsEmpty());
    TestEqual(TEXT("New address resolves the same instance"), Registry->FindTargets(TEXT("Lobby")).Find(Endpoint), 0);
    Owner->Destroy();
    TestEqual(TEXT("EndPlay releases address"), Registry->SendTo(TEXT("Lobby"), TEXT("Arrived"), 4), 0);
    Scene.ForwardErrorMessages(this);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOCompiler, "AngelIO.Compiler.Contract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOCompiler::RunTest(const FString&)
{
    FAngelIOProgram P;
    TestFalse(TEXT("Missing entry point"), P.Compile(TEXT("test"), TEXT("void Other() {}")));
    TestFalse(TEXT("Unsupported public type"), P.Compile(TEXT("test"), TEXT("double in_X; void Run() {}")));
    TestFalse(TEXT("Duplicate signal IDs"), P.Compile(TEXT("test"), TEXT("enum InputSignal { A, B = 0 } void Run() {}")));
    TestFalse(TEXT("Wrong signal direction fails validation"), P.Compile(TEXT("test"),
        TEXT("enum InputSignal { A } enum OutputSignal { B } void Run() { WaitFor(OutputSignal::B); }")));
    TestFalse(TEXT("Compile error"), P.Compile(TEXT("test"), TEXT("void Run() {\nmissing();\n}")));
    TestTrue(TEXT("Compiler reports source line"), P.Diagnostics().ContainsByPredicate(
        [](const auto& D) { return D.Severity == TEXT("error") && D.Line == 2; }));
    TestTrue(TEXT("Validation does not execute globals"), P.Compile(TEXT("test"),
        TEXT("int Spin() { while(true) {} return 0; } int g = Spin(); void Run() {}")));
    TestFalse(TEXT("Global initializer budget"), P.Start({}));
    TestTrue(TEXT("Compile bounded execution test"), P.Compile(TEXT("test"), TEXT("void Run() { while(true) {} }")));
    TestFalse(TEXT("Runaway script stops"), P.Start({}));
    return true;
}
#endif
