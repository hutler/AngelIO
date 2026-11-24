#if WITH_DEV_AUTOMATION_TESTS
#include "AngelIOExampleLift.h"
#include "AngelIOLiftTrip.h"
#include "AngelIOScriptComponent.h"
#include "AngelIOTargetRegistry.h"
#if WITH_EDITOR
#include "AngelIOTraceSubsystem.h"
#include "Dom/JsonObject.h"
#endif
#include "Tests/AngelIOTestSignalObserver.h"
#include "Tests/AutomationCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOCheckpointContract, "AngelIO.Missions.CheckpointContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOCheckpointContract::RunTest(const FString&)
{
    FTestWorldWrapper Scene;
    if (!Scene.CreateTestWorld(EWorldType::Game)) return false;
    UWorld* World = Scene.GetTestWorld();
    auto* Lift = World->SpawnActor<AAngelIOExampleLift>();
    if (!Lift) return false;
    Lift->bRequestOnStart = false;
    if (!Scene.BeginPlayInTestWorld()) return false;
    auto Run = [World](const TCHAR* File, UActorComponent* Component, const TCHAR* Body)
    {
        FAngelIOProgram P;
        FAngelIOValue Ok;
        return P.Compile(File, FString(TEXT("bool out_Ok; void Run() { ")) + Body + TEXT(" }")) &&
            P.Start({}, World, Component) && P.GetOutput(TEXT("Ok"), Ok) && Ok.Boolean;
    };
    const TCHAR* File = TEXT("Mission.as");
    TestTrue(TEXT("New checkpoints default to version one"), Run(File, Lift->Address,
        TEXT("out_Ok = LoadCheckpoint() == \"\"; SaveCheckpoint(\"Arrived\"); out_Ok = out_Ok && LoadCheckpoint() == \"Arrived\";")));
    TestTrue(TEXT("A new VM reads version one and can replace it with version two"), Run(File, Lift->Address,
        TEXT("out_Ok = LoadCheckpoint(2) == \"\" && LoadCheckpoint(1) == \"Arrived\" && LoadCheckpoint() == \"Arrived\"; SaveCheckpoint(\"AtDock\", 2);")));
    TestTrue(TEXT("Another script on the same component starts fresh"), Run(TEXT("Other.as"), Lift->Address,
        TEXT("out_Ok = LoadCheckpoint(2) == \"\";")));
    TestTrue(TEXT("Another component of the same actor starts fresh"), Run(File, Lift->Mission,
        TEXT("out_Ok = LoadCheckpoint(2) == \"\";")));
    auto Fails = [World](const TCHAR* Source, UActorComponent* Component, const TCHAR* Error)
    {
        FAngelIOProgram P;
        return P.Compile(TEXT("Mission.as"), Source) && !P.Start({}, World, Component) &&
            P.Diagnostics().ContainsByPredicate([Error](const auto& D) { return D.Message.Contains(Error); });
    };
    TestTrue(TEXT("Reject empty stage"), Fails(TEXT("void Run() { SaveCheckpoint(\"\", 2); }"), Lift->Address, TEXT("non-empty")));
    TestTrue(TEXT("Reject zero version"), Fails(TEXT("void Run() { LoadCheckpoint(0); }"), Lift->Address, TEXT("positive")));
    TestTrue(TEXT("A component is required"), Fails(TEXT("void Run() { LoadCheckpoint(); }"), nullptr, TEXT("mission component")));
    TestTrue(TEXT("Rejected writes leave the latest version intact"), Run(File, Lift->Address,
        TEXT("out_Ok = LoadCheckpoint() == \"\" && LoadCheckpoint(2) == \"AtDock\";")));
#if WITH_EDITOR
    TArray<FString> Stages;
    TSet<FString> Checkpoints;
    World->GetSubsystem<UAngelIOTraceSubsystem>()->VisitEvents([&](const FJsonObject& Event, const UObject*)
    {
        FString Id;
        if (!Event.TryGetStringField(TEXT("checkpointId"), Id)) return;
        Checkpoints.Add(Id);
        Stages.Add(FString::Printf(TEXT("%s %s v%d"), *Event.GetStringField(TEXT("type")),
            *Event.GetStringField(TEXT("stage")), Event.GetIntegerField(TEXT("version"))));
    });
    TestEqual(TEXT("Replacing the version retains the identity of the checkpoint"), Checkpoints.Num(), 1);
    TestEqual(TEXT("Only successful writes and the first matching read from an earlier run are traced"),
        FString::Join(Stages, TEXT(", ")), FString(TEXT("checkpointSaved Arrived v1, checkpointRestored Arrived v1, ")
            TEXT("checkpointSaved AtDock v2, checkpointRestored AtDock v2")));
#endif
    return true;
}

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOOverlappingTrips, "AngelIO.Missions.OverlappingTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOOverlappingTrips::RunTest(const FString&)
{
    FTestWorldWrapper Scene;
    if (!Scene.CreateTestWorld(EWorldType::Game)) return false;
    auto* World = Scene.GetTestWorld();
    auto* A = World->SpawnActor<AAngelIOExampleLift>();
    auto* B = World->SpawnActor<AAngelIOExampleLift>();
    if (!A || !B) return false;
    A->bRequestOnStart = B->bRequestOnStart = false;
    A->Address->SetTargetName(TEXT("Slow"));
    B->Address->SetTargetName(TEXT("Fast"));
    if (!Scene.BeginPlayInTestWorld()) return false;
    FAngelIOProgram Caller;
    const TCHAR* Script = TEXT(R"(
        int out_SlowStatus = -1, out_FastStatus = -1;
        int out_SlowFloor, out_FastFloor;
        void Run() {
            LiftTrip@ slow = FindLift("Slow").MoveTo(3, 100);
            LiftTrip@ fast = FindLift("Fast").MoveTo(2, 800);
            TripResult@ a = slow.Wait();
            TripResult@ b = fast.Wait();
            out_SlowStatus = int(a.Status);
            out_SlowFloor = a.Floor;
            out_FastStatus = int(b.Status);
            out_FastFloor = b.Floor;
        })");
    if (!TestTrue(TEXT("Script starts two independent trips"), Caller.Compile(TEXT("overlap"), Script) &&
        Caller.Start({}, World) && Caller.IsWaitingOnOperation())) return false;
    for (int32 Frame = 0; Frame < 50; ++Frame) Scene.TickTestWorld(0.02f);
    FAngelIOValue Value;
    TestTrue(TEXT("Fast trip has arrived"), B->Mission->GetOutput(TEXT("LastFloor"), Value) && Value.Integer == 2);
    TestFalse(TEXT("Fast arrival cannot resume slow wait"), Caller.ResumeCompletedOperation());
    for (int32 Frame = 0; Frame < 200; ++Frame) Scene.TickTestWorld(0.02f);
    TestTrue(TEXT("Slow arrival resumes the caller"), Caller.ResumeCompletedOperation());
    TestEqual(TEXT("Both reads finish in one resume"), int(Caller.State()), int(EAngelIOState::Finished));
    for (const auto& D : Caller.Diagnostics())
        if (D.Severity == TEXT("error")) AddError(D.Message);
    auto CheckResult = [&](const TCHAR* Name, int32 Expected)
    {
        if (TestTrue(Name, Caller.GetOutput(Name, Value))) TestEqual(Name, Value.Integer, Expected);
    };
    CheckResult(TEXT("SlowFloor"), 3);
    CheckResult(TEXT("FastFloor"), 2);
    CheckResult(TEXT("SlowStatus"), 0);
    CheckResult(TEXT("FastStatus"), 0);
    const double StartZ = A->GetActorLocation().Z;
    auto SlowTrip = A->MoveTo(4, 0.001f);
    for (int32 Frame = 0; Frame < 50; ++Frame) Scene.TickTestWorld(0.02f);
    TestTrue(TEXT("Small per-frame movements accumulate"), A->GetActorLocation().Z > StartZ + 0.0005);
    A->Mission->StopScript();
    TestTrue(TEXT("Stopping the lift mission cancels its active trip"),
        SlowTrip->IsComplete() && SlowTrip->Status == ELiftTripStatus::Cancelled);
    Scene.ForwardErrorMessages(this);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOCompiler, "AngelIO.Compiler.Contract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOCompiler::RunTest(const FString&)
{
    FAngelIOProgram P;
    TestTrue(TEXT("Typed lift contract"), P.CompileFile(TEXT("TypedLift.as")));
    TestFalse(TEXT("MoveTo requires floor and speed"), P.Compile(TEXT("test"),
        TEXT("void Run() { FindLift(\"A\").MoveTo(2); }")));
    TestFalse(TEXT("MoveTo rejects a text floor"), P.Compile(TEXT("test"),
        TEXT("void Run() { FindLift(\"A\").MoveTo(\"two\", 200.f); }")));
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
