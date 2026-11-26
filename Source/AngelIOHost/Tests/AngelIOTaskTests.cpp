#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AngelIOProgram.h"
#include "AngelIOTraceSubsystem.h"
#include "Dom/JsonObject.h"
#include "Tests/AutomationCommon.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Engine/World.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOOwnedTasks, "AngelIO.Missions.OwnedTasks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAngelIOOwnedTasks::RunTest(const FString&)
{
    FTestWorldWrapper Scene;
    if (!Scene.CreateTestWorld(EWorldType::Game) || !Scene.BeginPlayInTestWorld()) return false;
    auto* World = Scene.GetTestWorld();
    const FString Source = TEXT(R"(
        enum InputSignal { Finish, Release }
        Task@ a; Task@ b; int out_Code; int out_Readers;
        int Work(int i) { NextFrame(); return i * 2; }
        int Reader(int i) { if (a.Join().Ready) out_Readers++; return i; }
        int Bad(int i) { int zero = 0; return i / zero; }
        int Long(int i) { WaitActor("NeverSpawned"); return i; }
        int Gate(int i) { EndOn(InputSignal::Finish); WaitFor(InputSignal::Release); return i; }
        void Run() {
            @a = StartTask(Work, 3); @b = StartTask(Work, 4);
            if(a.Join(0).Ready) return;
            Task@ reader1=StartTask(Reader, 1); Task@ reader2=StartTask(Reader, 2);
            TaskJoin@ first=WatchFirst(a,b,0.015625f);
            if(!first.Ready || first.Index!=0 || a.Status!=TaskStatus::Succeeded) return;
            TaskJoin@ all=WatchAll(a,b); int x,y;
            if(!all.Ready || !a.TryGetResult(x) || !b.TryGetResult(y)) return;
            out_Code=x+y;
            if (!WatchAll(reader1,reader2).Ready || !WatchFirst(a,a).Ready) return;
            Task@ broken=StartTask(Bad, 1);
            if(broken.Status!=TaskStatus::Failed || broken.TryGetResult(x) || broken.Reason.length()==0) return;
            Task@ held=StartTask(Long,1);
            if(held.Join(0).Ready || !held.Cancel() || held.Join().First!=TaskStatus::Cancelled) return;
            Task@ gate=StartTask(Gate,7);
            Notify(InputSignal::Finish);
            WaitFor(InputSignal::Finish);
            if(gate.Join().First==TaskStatus::Cancelled) out_Code++;
            StartTask(Long, 1);
        })");
    FAngelIOProgram P;
    if (!TestTrue(TEXT("Compile task handles and watches"), P.Compile(TEXT("OwnedTasks.as"), Source, World)) ||
        !TestTrue(TEXT("Start root"), P.Start({}, World))) return false;
    FAngelIOValue V;
    TestTrue(TEXT("NextFrame does not resume in the starting tick"), !P.ResumeCompletedOperation() &&
        P.GetOutput(TEXT("Code"), V) && V.Integer == 0);
    for (int32 I = 0; I < 4; ++I)
    {
        Scene.TickTestWorld(1.f/64);
        P.ResumeCompletedOperation();
        for (const auto& Event : P.TakeEvents())
            if (Event.Name == TEXT("Finish")) P.Signal(Event.Name, 0);
    }
    TestTrue(TEXT("Two results, same-frame priority, isolated error and exact EndOn"), P.GetOutput(TEXT("Code"), V) &&
        V.Integer == 15 && P.State() == EAngelIOState::Finished);
    TestTrue(TEXT("Two readers got the same result"), P.GetOutput(TEXT("Readers"), V) && V.Integer == 2);
    TestTrue(TEXT("All contexts and waits released"), !P.IsWaitingOnOperation() && P.InspectMission().BranchWaits.IsEmpty());
    int32 Tasks = 0, Failed = 0, Cancelled = 0;
    TSet<FString> Ids;
    World->GetSubsystem<UAngelIOTraceSubsystem>()->VisitEvents([&](const FJsonObject& E, const UObject*)
    {
        FString Name, Status;
        if (E.TryGetStringField(TEXT("operation"), Name) && Name == TEXT("Task.Start"))
        { ++Tasks; Ids.Add(E.GetStringField(TEXT("operationId"))); }
        if (Ids.Contains(E.GetStringField(TEXT("operationId"))) && E.TryGetStringField(TEXT("status"), Status))
        {
            Failed += Status == TEXT("Failed");
            Cancelled += Status == TEXT("Cancelled");
        }
    });
    TestTrue(TEXT("Each task has its own trace ID, one failed and three were cancelled"), Tasks == 8 && Ids.Num() == 8 &&
        Failed == 1 && Cancelled == 3);
    FString Export;
    TestTrue(TEXT("Export bounded task history before the branch-limit probe"), World->GetSubsystem<UAngelIOTraceSubsystem>()->Export(Export));
    AddInfo(TEXT("Task trace export: ") + FPaths::GetCleanFilename(Export));
    FAngelIOProgram Limit;
    TestTrue(TEXT("The 65th live child hits the shared branch limit and tears down all children"), Limit.Compile(TEXT("Limit.as"),
        TEXT("int Work(int i){ NextFrame();return i; } void Run(){ for(int i=0;i<65;i++) StartTask(Work,i); }"), World) &&
        !Limit.Start({}, World) && Limit.InspectMission().BranchWaits.IsEmpty() && Limit.Diagnostics().ContainsByPredicate(
            [](const auto& D){return D.Message.Contains(TEXT("branch limit"));}));
    return true;
}
#endif
