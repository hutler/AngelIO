#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AngelIOTestSignalObserver.h"
#include "../AngelIOExampleLift.h"
#include "../AngelIOLiftTrip.h"
#include "../AngelIOPlayerController.h"
#include "AngelIOScriptComponent.h"
#include "AngelIOTargetRegistry.h"
#include "AngelIOTraceSubsystem.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Misc/AutomationTest.h"

namespace
{
UWorld* FindPeer(ENetMode Mode, UWorld* Exclude = nullptr)
{
    for (const FWorldContext& Context : GEngine->GetWorldContexts())
    {
        UWorld* World = Context.World();
        if (Context.WorldType == EWorldType::PIE && World && World != Exclude &&
            World->GetNetDriver() && World->GetNetMode() == Mode && World->HasBegunPlay())
            return World;
    }
    return nullptr;
}

AAngelIOExampleLift* FindLift(UWorld* World, int32 StartupFloor)
{
    if (World)
        for (TActorIterator<AAngelIOExampleLift> It(World); It; ++It)
            if (It->RequestedFloor == StartupFloor) return *It;
    return nullptr;
}

bool AtFloor(UWorld* World, int32 StartupFloor, int32 Floor, int32 Trips)
{
    const auto* Lift = FindLift(World, StartupFloor);
    FAngelIOValue Current, Count, Speed;
    return Lift && Lift->Mission->GetOutput(TEXT("LastFloor"), Current) && Current.Integer == Floor &&
        Lift->Mission->GetOutput(TEXT("TotalTrips"), Count) && Count.Integer == Trips &&
        Lift->Mission->GetOutput(TEXT("TripSpeed"), Speed) && Speed.Float > 0 &&
        Lift->Mission->GetWaitingSignal() == TEXT("RequestFloor");
}

void Observe(UAngelIOScriptComponent* Mission, UAngelIOTestSignalObserver* Receiver)
{
    Mission->OnPresentationSignal.AddDynamic(Receiver, &UAngelIOTestSignalObserver::OnEffect);
    Mission->OnSignal.AddDynamic(Receiver, &UAngelIOTestSignalObserver::OnGameplay);
}

struct FListenPeers
{
    TWeakObjectPtr<UWorld> Host, Client, Late;
    TStrongObjectPtr<UAngelIOTestSignalObserver> HostProbe{NewObject<UAngelIOTestSignalObserver>()};
    TStrongObjectPtr<UAngelIOTestSignalObserver> ClientProbe{NewObject<UAngelIOTestSignalObserver>()};
};
void StartListenSession()
{
    auto* Settings = NewObject<ULevelEditorPlaySettings>();
    Settings->SetPlayNetMode(EPlayNetMode::PIE_ListenServer);
    Settings->SetPlayNumberOfClients(2); // Host player and one remote player.
    Settings->SetRunUnderOneProcess(true);
    Settings->bLaunchSeparateServer = false;
    Settings->GameGetsMouseControl = false;
    FRequestPlaySessionParams Params;
    Params.EditorPlaySettings = Settings;
    Params.GlobalMapOverride = TEXT("/Game/Demo/TwoLifts");
    Params.bAllowOnlineSubsystem = false;
    GEditor->RequestPlaySession(Params);
}

auto ListenSteps(FAutomationTestBase* Test)
{
    return [Test](FString Step, TFunction<bool()> Check)
    {
        ADD_LATENT_AUTOMATION_COMMAND(FUntilCommand(
            [Test, Check] { return Test->HasAnyErrors() || Check(); },
            [Test, Step] { Test->AddError(TEXT("Timed out: ") + Step); return true; }, 35.f));
    };
}

void EndListenSession()
{
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([] { GEditor->RequestEndPlayMap(); return true; }));
    ADD_LATENT_AUTOMATION_COMMAND(FUntilCommand([] { return !GEditor->PlayWorld; }, 15.f));
}

bool TripOutput(UAngelIOScriptComponent* Panel, const TCHAR* Name, int32 Expected)
{
    FAngelIOValue Value;
    return Panel && Panel->GetOutput(Name, Value) && Value.Integer == Expected;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOListen, "AngelIO.Network.ListenServer.NamedGroupAndLateJoin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOListen::RunTest(const FString&)
{
    if (!TestNull(TEXT("PIE must be idle"), GEditor->PlayWorld.Get())) return false;
    StartListenSession();
    const auto Peers = MakeShared<FListenPeers>();
    auto WaitFor = ListenSteps(this);
    WaitFor(TEXT("host and client finish both initial trips"), [this, Peers]
    {
        auto* Host = FindPeer(NM_ListenServer);
        auto* Client = FindPeer(NM_Client);
        if (!AtFloor(Host, 2, 2, 1) || !AtFloor(Client, 2, 2, 1) || !AtFloor(Client, 3, 3, 1)) return false;
        auto* PC = Cast<AAngelIOPlayerController>(Client->GetFirstPlayerController());
        if (!PC || !PC->IsLocalController() || !PC->GetNetConnection()) return false;
        Peers->Host = Host;
        Peers->Client = Client;
        auto* Proxy = FindLift(Client, 2)->Mission.Get();
        Observe(FindLift(Host, 2)->Mission, Peers->HostProbe.Get());
        Observe(Proxy, Peers->ClientProbe.Get());
        TestEqual(TEXT("Shared logical address"), Proxy->GetTargetName(), FName(TEXT("DemoLifts")));
        TestFalse(TEXT("Client cannot start a VM"), Proxy->StartScript());
        TestFalse(TEXT("Client cannot change input"), Proxy->SetInput(TEXT("StartingFloor"), {}));
        TestFalse(TEXT("Client cannot advance the VM"), Proxy->SendSignal(TEXT("MotionFinished"), 88));
        Proxy->StopScript();
        TestTrue(TEXT("Local stop preserves received state"), AtFloor(Client, 2, 2, 1));
        TestEqual(TEXT("Client cannot send via registry"),
            Client->GetSubsystem<UAngelIOTargetRegistry>()->SendTo(TEXT("DemoLifts"), TEXT("RequestFloor"), 8), 0);
        PC->ServerRequestLiftFloor(0); // Invalid before a valid request on the same ordered RPC channel.
        PC->ConsoleCommand(TEXT("LiftFloor 4"));
        return true;
    });

    WaitFor(TEXT("one client request reaches both lifts by name"), [this, Peers]
    {
        for (auto* World : {Peers->Host.Get(), Peers->Client.Get()})
            if (!AtFloor(World, 2, 4, 2) || !AtFloor(World, 3, 4, 2)) return false;
        if (Peers->ClientProbe->Presentations != 1) return false;
        const FVector A = FindLift(Peers->Host.Get(), 2)->GetActorLocation();
        const FVector B = FindLift(Peers->Client.Get(), 2)->GetActorLocation();
        if (!A.Equals(B, 1.0)) return false;
        TestEqual(TEXT("One host effect"), Peers->HostProbe->Presentations, 1);
        TestEqual(TEXT("One authoritative arrival"), Peers->HostProbe->GameplayEvents, 1);
        TestEqual(TEXT("Client only observes presentation"), Peers->ClientProbe->GameplayEvents, 0);
        GEditor->RequestLateJoin();
        return true;
    });

    WaitFor(TEXT("late client receives current group state"), [this, Peers]
    {
        UWorld* Late = FindPeer(NM_Client, Peers->Client.Get());
        if (!AtFloor(Late, 2, 4, 2) || !AtFloor(Late, 3, 4, 2)) return false;
        Peers->Late = Late;
        UWorld* Host = Peers->Host.Get();
        FindLift(Host, 2)->Mission->SetTargetName(TEXT("ServiceLift"));
        auto* Registry = Host->GetSubsystem<UAngelIOTargetRegistry>();
        TestEqual(TEXT("Missing address"), Registry->SendTo(TEXT("Absent"), TEXT("RequestFloor"), 6), 0);
        auto* PC = Cast<AAngelIOPlayerController>(Host->GetFirstPlayerController());
        if (!TestNotNull(TEXT("Host player controller"), PC)) return true;
        PC->ServerRequestLiftFloor(5);
        return true;
    });

    WaitFor(TEXT("host request follows updated membership on both clients"), [Peers]
    {
        if (!AtFloor(Peers->Client.Get(), 3, 5, 3) || !AtFloor(Peers->Late.Get(), 3, 5, 3) ||
            !AtFloor(Peers->Late.Get(), 2, 4, 2)) return false;
        if (FindLift(Peers->Late.Get(), 2)->Mission->GetTargetName() != TEXT("ServiceLift")) return false;
        FindLift(Peers->Host.Get(), 2)->Mission->StopScript();
        return true;
    });
    WaitFor(TEXT("both clients clear stopped outputs"), [Peers]
    {
        for (UWorld* World : {Peers->Client.Get(), Peers->Late.Get()})
        {
            FAngelIOValue Value;
            auto* Lift = FindLift(World, 2);
            if (!Lift || Lift->Mission->GetOutput(TEXT("LastFloor"), Value) ||
                !Lift->Mission->GetWaitingSignal().IsEmpty()) return false;
        }
        return true;
    });
    EndListenSession();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOTypedListen, "AngelIO.Network.ListenServer.TypedTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOTypedListen::RunTest(const FString&)
{
    if (!TestNull(TEXT("PIE must be idle"), GEditor->PlayWorld.Get())) return false;
    StartListenSession();
    auto WaitFor = ListenSteps(this);
    auto Server = [] { return FindPeer(NM_ListenServer); };
    auto LocalPC = [](ENetMode Mode) -> AAngelIOPlayerController*
    {
        auto* World = FindPeer(Mode);
        return World ? Cast<AAngelIOPlayerController>(World->GetFirstPlayerController()) : nullptr;
    };

    WaitFor(TEXT("client calls freight lift from its own controller"), [this, Server, LocalPC]
    {
        auto* Client = LocalPC(NM_Client);
        if (!Client || !AtFloor(Client->GetWorld(), 2, 2, 1) || !AtFloor(Server(), 3, 3, 1)) return false;
        FAngelIOProgram Probe;
        const TCHAR* Source = TEXT(R"(
            bool out_Ok;
            void Run() {
                Lift@ a = FindLift("FREIGHTLIFT");
                Lift@ b = FindLift("ServiceLiftB");
                LiftTrip@ same = a.MoveTo(2, 150);
                TripResult@ first = same.Wait();
                LiftTrip@ bad = b.MoveTo(5, -1);
                out_Ok = same.Id != bad.Id && same.Id == first.Id &&
                    first.Status == TripStatus::Arrived && same.Wait().Floor == 2 &&
                    bad.Wait().Status == TripStatus::Rejected && FindLift("Missing") is null &&
                    FindLift("DemoLifts") is null;
            })");
        FAngelIOValue Ok;
        TestTrue(TEXT("Inline, repeated wait, invalid speed and unique names"), Probe.Compile(TEXT("probe"), Source) &&
            Probe.Start({}, Server()) && Probe.State() == EAngelIOState::Finished && Probe.GetOutput(TEXT("Ok"), Ok) && Ok.Boolean);
        TestFalse(TEXT("Client cannot start typed mission locally"), Client->TripPanel->StartScript());
        Client->ConsoleCommand(TEXT("LiftTrip 6 400"));
        return true;
    });

    auto OtherLiftCaller = MakeShared<FAngelIOProgram>();
    WaitFor(TEXT("remote trip is pending when host requests another ride"), [this, Server, LocalPC, OtherLiftCaller]
    {
        auto* Client = LocalPC(NM_Client);
        auto* Host = LocalPC(NM_ListenServer);
        if (!Client || !Host || Client->TripPanel->GetWaitingSignal() != TEXT("LiftTrip.Wait")) return false;
        auto* Freight = FindLift(Server(), 2);
        FAngelIOValue Speed;
        TestTrue(TEXT("Requested speed used by physical lift"), Freight->Mission->GetOutput(TEXT("TripSpeed"), Speed) && Speed.Float == 400);
        Host->LiftTrip(7, 800);
        TestTrue(TEXT("Host's busy request has its own rejected result"),
            TripOutput(Host->TripPanel, TEXT("LastStatus"), int32(ELiftTripStatus::Rejected)) &&
            TripOutput(Host->TripPanel, TEXT("LastFloor"), 7) && TripOutput(Host->TripPanel, TEXT("Completed"), 1));
        TestTrue(TEXT("Second named lift accepts concurrent trip"), OtherLiftCaller->Compile(TEXT("second lift"),
            TEXT("bool out_Ok; void Run() { TripResult@ r = FindLift(\"ServiceLiftB\").MoveTo(4, 1800).Wait(); ")
            TEXT("out_Ok = r.Status == TripStatus::Arrived && r.Floor == 4; }")) &&
            OtherLiftCaller->Start({}, Server()) && OtherLiftCaller->IsWaitingOnOperation());
        for (FConstPlayerControllerIterator It = Server()->GetPlayerControllerIterator(); It; ++It)
        {
            auto* PC = Cast<AAngelIOPlayerController>(It->Get());
            if (PC && !PC->IsLocalController())
                TestFalse(TEXT("Matching signal cannot complete a trip"), PC->TripPanel->SendSignal(TEXT("LiftTrip.Wait"), 7));
        }
        return true;
    });

    WaitFor(TEXT("short ride completes while remote caller still awaits freight lift"), [this, OtherLiftCaller, LocalPC]
    {
        if (!OtherLiftCaller->ResumeCompletedOperation()) return false;
        FAngelIOValue Ok;
        TestTrue(TEXT("Short ride returns its own arrival"), OtherLiftCaller->GetOutput(TEXT("Ok"), Ok) && Ok.Boolean);
        TestTrue(TEXT("Remote caller is still waiting for floor six"),
            LocalPC(NM_Client)->TripPanel->GetWaitingSignal() == TEXT("LiftTrip.Wait") &&
            TripOutput(LocalPC(NM_Client)->TripPanel, TEXT("Completed"), 0));
        return true;
    });

    WaitFor(TEXT("owner receives arrival for floor six"), [this, Server, LocalPC]
    {
        auto* Client = LocalPC(NM_Client);
        if (!Client || !TripOutput(Client->TripPanel, TEXT("Completed"), 1) || !AtFloor(Client->GetWorld(), 2, 6, 3)) return false;
        TestTrue(TEXT("Other named lift reached its own floor"), AtFloor(Server(), 3, 4, 2));
        TestTrue(TEXT("Server result replicated to calling player"),
            TripOutput(Client->TripPanel, TEXT("LastFloor"), 6) &&
            TripOutput(Client->TripPanel, TEXT("LastStatus"), int32(ELiftTripStatus::Arrived)));
        Client->LiftTrip(1, 180);
        return true;
    });
    WaitFor(TEXT("remove lift during the next remote ride"), [Server, LocalPC]
    {
        auto* Client = LocalPC(NM_Client);
        if (!Client || Client->TripPanel->GetWaitingSignal() != TEXT("LiftTrip.Wait")) return false;
        return FindLift(Server(), 2)->Destroy();
    });
    WaitFor(TEXT("caller receives one cancellation for floor one"), [LocalPC]
    {
        auto* Client = LocalPC(NM_Client);
        return Client && TripOutput(Client->TripPanel, TEXT("Completed"), 2) &&
            TripOutput(Client->TripPanel, TEXT("LastFloor"), 1) &&
            TripOutput(Client->TripPanel, TEXT("LastStatus"), int32(ELiftTripStatus::Cancelled)) &&
            Client->TripPanel->GetWaitingSignal() == TEXT("CallLift");
    });
    WaitFor(TEXT("export the authoritative operation history"), [this, Server]
    {
        FString Path, Text;
        TestFalse(TEXT("Client has no duplicate operation history"),
            FindPeer(NM_Client)->GetSubsystem<UAngelIOTraceSubsystem>()->Export(Path));
        TSharedPtr<FJsonObject> Trace;
        if (!TestTrue(TEXT("Export parses as JSON"), Server()->GetSubsystem<UAngelIOTraceSubsystem>()->Export(Path) &&
            FFileHelper::LoadFileToString(Text, *Path) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Trace)))
            return true;
        TestEqual(TEXT("Server capture"), Trace->GetStringField(TEXT("netMode")), FString(TEXT("listenServer")));
        TMap<int32, FString> Trips;
        TMap<FString, FString> Timeline, Callers, Status;
        int32 ExpectedSequence = 0;
        for (const auto& Value : Trace->GetArrayField(TEXT("events")))
        {
            const auto Event = Value->AsObject();
            const FString Id = Event->GetStringField(TEXT("operationId"));
            const FString Type = Event->GetStringField(TEXT("type"));
            TestEqual(TEXT("Ordered sequence"), Event->GetIntegerField(TEXT("sequence")), ++ExpectedSequence);
            Timeline.FindOrAdd(Id) += Type + TEXT(" ");
            if (Type == TEXT("call"))
            {
                const int32 Floor = Event->GetObjectField(TEXT("arguments"))->GetIntegerField(TEXT("floor"));
                Trips.Add(Floor, Id);
                const auto Site = Event->GetObjectField(TEXT("caller"));
                TestFalse(TEXT("Each invocation owns a new ID"), Callers.Contains(Id));
                Callers.Add(Id, Site->GetStringField(TEXT("instanceId")));
                if (Floor == 6 || Floor == 7)
                {
                    TestEqual(TEXT("Named lift"), Event->GetStringField(TEXT("target")), FString(TEXT("FreightLift")));
                    TestEqual(TEXT("Calling script"), Site->GetStringField(TEXT("script")), FString(TEXT("TypedLift.as")));
                    TestTrue(TEXT("Call has source location"), Site->GetIntegerField(TEXT("line")) > 0);
                    TestEqual(TEXT("Owning controller"), Site->GetStringField(TEXT("controller")),
                        FString(Floor == 6 ? TEXT("remote") : TEXT("local")));
                }
            }
            else if (Type == TEXT("wait") || Type == TEXT("resume"))
            {
                const auto Site = Event->GetObjectField(TEXT("waiter"));
                TestTrue(TEXT("Wait has source location"), Site->GetIntegerField(TEXT("line")) > 0);
                TestEqual(TEXT("Wait belongs to the correct instance"), Site->GetStringField(TEXT("instanceId")), Callers.FindRef(Id));
            }
            else if (Type == TEXT("complete")) Status.Add(Id, Event->GetStringField(TEXT("status")));
        }
        TestEqual(TEXT("Six distinct trip IDs"), Callers.Num(), 6);
        TestNotEqual(TEXT("Host and remote run separate instances of TypedLift.as"),
            Callers.FindRef(Trips.FindRef(7)), Callers.FindRef(Trips.FindRef(6)));
        TestEqual(TEXT("Remote delayed trip"), Timeline.FindRef(Trips.FindRef(6)), FString(TEXT("call wait complete resume ")));
        TestEqual(TEXT("Host busy trip returns inline"), Timeline.FindRef(Trips.FindRef(7)), FString(TEXT("call complete wait ")));
        TestEqual(TEXT("Completed trip can be read twice"), Timeline.FindRef(Trips.FindRef(2)), FString(TEXT("call complete wait wait ")));
        TestEqual(TEXT("Remote arrival"), Status.FindRef(Trips.FindRef(6)), FString(TEXT("Arrived")));
        TestEqual(TEXT("Concurrent short ride"), Status.FindRef(Trips.FindRef(4)), FString(TEXT("Arrived")));
        TestEqual(TEXT("Host rejection"), Status.FindRef(Trips.FindRef(7)), FString(TEXT("Rejected")));
        TestEqual(TEXT("Remote cancellation"), Status.FindRef(Trips.FindRef(1)), FString(TEXT("Cancelled")));
        return true;
    });
    EndListenSession();
    return true;
}
#endif
