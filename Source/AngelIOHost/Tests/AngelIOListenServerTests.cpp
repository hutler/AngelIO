#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AngelIOTestSignalObserver.h"
#include "../AngelIOExampleLift.h"
#include "../AngelIOLiftTrip.h"
#include "../AngelIOPlayerController.h"
#include "AngelIOScriptComponent.h"
#include "AngelIOTargetRegistry.h"
#include "AngelIOTraceSubsystem.h"
#include "SAngelIOMissionInspector.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Editor.h"
#include "Engine/LevelStreamingDynamic.h"
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
void StartListenSession(const TCHAR* Map = TEXT("/Game/Demo/TwoLifts"))
{
    auto* Settings = NewObject<ULevelEditorPlaySettings>();
    Settings->SetPlayNetMode(EPlayNetMode::PIE_ListenServer);
    Settings->SetPlayNumberOfClients(2); // Host player and one remote player.
    Settings->SetRunUnderOneProcess(true);
    Settings->bLaunchSeparateServer = false;
    Settings->GameGetsMouseControl = false;
    FRequestPlaySessionParams Params;
    Params.EditorPlaySettings = Settings;
    Params.GlobalMapOverride = Map;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOHandleLifetime, "AngelIO.Network.ListenServer.HandleLifetimeAndTimeout",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOHandleLifetime::RunTest(const FString&)
{
    if (!TestNull(TEXT("PIE must be idle"), GEditor->PlayWorld.Get())) return false;
    StartListenSession();
    const auto Steps = ListenSteps(this);
    const auto Probe = MakeShared<FAngelIOProgram>();
    Steps(TEXT("cache the original actor and reassign its name"), [this, Probe]
    {
        auto* Host = FindPeer(NM_ListenServer);
        auto* Client = FindPeer(NM_Client);
        if (!AtFloor(Host, 2, 2, 1) || !AtFloor(Client, 3, 3, 1)) return false;
        auto* PC = Cast<AAngelIOPlayerController>(Client->GetFirstPlayerController());
        if (!PC || !PC->GetNetConnection()) return false;
        const TCHAR* Script = TEXT(R"(
            enum InputSignal { Rebound, Destroyed }
            bool out_Timed;
            bool out_Arrived;
            bool out_Gone;
            void Run() {
                Lift@ cached = FindLift("FreightLift");
                WaitFor(InputSignal::Rebound);
                LiftTrip@ ride = cached.MoveTo(4, 200);
                TripResult@ poll = ride.Wait(0);
                TripResult@ timed = ride.Wait(0.05f);
                out_Timed = poll.Status == TripStatus::TimedOut && timed.Status == TripStatus::TimedOut;
                TripResult@ arrival = ride.Wait();
                out_Arrived = arrival.Status == TripStatus::Arrived && ride.Wait(0).Status == TripStatus::Arrived
                    && timed.Status == TripStatus::TimedOut && timed.Id == arrival.Id;
                WaitFor(InputSignal::Destroyed);
                out_Gone = cached.MoveTo(8, 200).Wait().Status == TripStatus::Cancelled;
            })");
        if (!TestTrue(TEXT("Cache handle before rename"), Probe->Compile(TEXT("CachedLift.as"), Script) &&
                Probe->Start({}, Host) && Probe->WaitingFor() == TEXT("Rebound"))) return true;
        FindLift(Host, 2)->Address->SetTargetName(TEXT("OriginalLift"));
        FindLift(Host, 3)->Address->SetTargetName(TEXT("FreightLift"));
        TestTrue(TEXT("Cached handle starts old lift"), Probe->Signal(TEXT("Rebound"), 0) && Probe->IsWaitingOnOperation());
        PC->LiftTrip(5, 400);
        return true;
    });
    Steps(TEXT("timeout resumes the reader before arrival"), [this, Probe]
    {
        if (!Probe->ResumeCompletedOperation()) return false;
        FAngelIOValue Timed;
        TestTrue(TEXT("Polling and finite wait timed out, indefinite wait remains active"),
            Probe->GetOutput(TEXT("Timed"), Timed) && Timed.Boolean && Probe->IsWaitingOnOperation());
        return true;
    });
    Steps(TEXT("host and client see separate rides on old and newly resolved actors"), [this, Probe]
    {
        Probe->ResumeCompletedOperation();
        auto* Host = FindPeer(NM_ListenServer);
        auto* Client = FindPeer(NM_Client);
        if (Probe->WaitingFor() != TEXT("Destroyed") || !AtFloor(Host, 2, 4, 2) ||
            !AtFloor(Client, 2, 4, 2) || !AtFloor(Host, 3, 5, 2) || !AtFloor(Client, 3, 5, 2)) return false;
        FAngelIOValue Arrived;
        TestTrue(TEXT("Timed-out result is stable after the same ride arrives"),
            Probe->GetOutput(TEXT("Arrived"), Arrived) && Arrived.Boolean);
        FindLift(Host, 2)->Destroy();
        return true;
    });
    Steps(TEXT("a destroyed handle reports TargetGone"), [this, Probe]
    {
        auto* Host = FindPeer(NM_ListenServer);
        auto* Client = FindPeer(NM_Client);
        if (!Host || !Client || FindLift(Client, 2)) return false;
        FAngelIOValue Gone;
        TestTrue(TEXT("Cached handle reports cancellation"), Probe->Signal(TEXT("Destroyed"), 0) &&
            Probe->GetOutput(TEXT("Gone"), Gone) && Gone.Boolean);
        bool bTargetGone = false;
        Host->GetSubsystem<UAngelIOTraceSubsystem>()->VisitEvents([&](const FJsonObject& Event, const UObject*)
        {
            FString Reason;
            if (Event.TryGetStringField(TEXT("reason"), Reason) && Reason == TEXT("TargetGone"))
                bTargetGone = Event.GetStringField(TEXT("status")) == TEXT("Cancelled");
        });
        TestTrue(TEXT("Trace explains stale call"), bTargetGone);
        TestTrue(TEXT("Replacement still on client-requested floor"), AtFloor(Host, 3, 5, 2));
        return true;
    });
    EndListenSession();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOStreamedTrips, "AngelIO.Network.ListenServer.StreamedTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOStreamedTrips::RunTest(const FString&)
{
    if (!TestNull(TEXT("PIE must be idle"), GEditor->PlayWorld.Get())) return false;
    StartListenSession(TEXT("/Engine/Maps/Entry"));
    struct FRoom
    {
        TWeakObjectPtr<ULevelStreamingDynamic> Host, Client;
        TWeakObjectPtr<AAngelIOExampleLift> Original;
        FVector FloorTwoLocation = FVector::ZeroVector;
        FAngelIOProgram Caller;
        bool Output(const TCHAR* Name) const
        {
            FAngelIOValue V;
            return Caller.GetOutput(Name, V) && V.Boolean;
        }
    };
    const auto Room = MakeShared<FRoom>();
    const auto Steps = ListenSteps(this);
    Steps(TEXT("persistent caller starts before the room streams in"), [this, Room]
    {
        auto* Host = FindPeer(NM_ListenServer);
        auto* Client = FindPeer(NM_Client);
        if (!Host || !Client || !Client->GetFirstPlayerController() || !Client->GetFirstPlayerController()->GetNetConnection()) return false;
        const TCHAR* Source = TEXT(R"(
            enum InputSignal { Loaded, Hidden, Shown, Reloaded }
            bool out_Absent, out_Cancelled, out_Hidden, out_Stale, out_New, out_Arrived;
            void Run() {
                out_Absent = FindLift("FreightLift") is null;
                WaitFor(InputSignal::Loaded);
                Lift@ old = FindLift("FreightLift");
                out_Cancelled = old.MoveTo(8, 10).Wait().Status == TripStatus::Cancelled;
                WaitFor(InputSignal::Hidden);
                out_Hidden = FindLift("FreightLift") is null &&
                    old.MoveTo(8, 100).Wait().Status == TripStatus::Cancelled;
                WaitFor(InputSignal::Shown);
                out_Stale = old.MoveTo(8, 100).Wait().Status == TripStatus::Cancelled;
                Lift@ shown = FindLift("FreightLift");
                out_New = shown.MoveTo(2, 800).Wait().Status == TripStatus::Arrived;
                WaitFor(InputSignal::Reloaded);
                out_Arrived = shown.MoveTo(8, 100).Wait().Status == TripStatus::Cancelled &&
                    FindLift("FreightLift").MoveTo(6, 800).Wait().Status == TripStatus::Arrived;
            })");
        if (!TestTrue(TEXT("Lookup is empty before loading"), Room->Caller.Compile(TEXT("StreamedLift.as"), Source) &&
            Room->Caller.Start({}, Host) && Room->Output(TEXT("Absent")))) return true;
        auto Load = [](UWorld* World)
        {
            bool bSuccess = false;
            return ULevelStreamingDynamic::LoadLevelInstance(World, TEXT("/Game/Demo/TwoLifts"),
                FVector(1200, -1600, 300), FRotator::ZeroRotator, bSuccess, TEXT("AngelIOStreamedRoom"));
        };
        Room->Host = Load(Host);
        Room->Client = Load(Client);
        TestTrue(TEXT("Dynamic room requested on both peers"), Room->Host.IsValid() && Room->Client.IsValid());
        return true;
    });
    Steps(TEXT("hide room during an active ride"), [this, Room]
    {
        auto* Host = FindPeer(NM_ListenServer);
        if (!AtFloor(Host, 2, 2, 1) || !AtFloor(FindPeer(NM_Client), 2, 2, 1)) return false;
        Room->Original = FindLift(Host, 2);
        Room->FloorTwoLocation = Room->Original->GetActorLocation();
        TestTrue(TEXT("Persistent caller is awaiting the ride"), Room->Caller.Signal(TEXT("Loaded"), 0) &&
            Room->Caller.IsWaitingOnOperation());
        Room->Host->SetShouldBeVisible(false);
        Room->Client->SetShouldBeVisible(false);
        return true;
    });
    Steps(TEXT("hidden actors leave the registry and the ride is cancelled"), [this, Room]
    {
        if (Room->Host->IsLevelVisible() || Room->Client->IsLevelVisible()) return false;
        Room->Caller.ResumeCompletedOperation();
        TestTrue(TEXT("Caller observes both cancellation and absence"), Room->Output(TEXT("Cancelled")) &&
            Room->Caller.Signal(TEXT("Hidden"), 0) && Room->Output(TEXT("Hidden")));
        Room->Host->SetShouldBeVisible(true);
        Room->Client->SetShouldBeVisible(true);
        return true;
    });
    Steps(TEXT("a new lifetime on the same UObject requires a fresh handle"), [this, Room]
    {
        auto* Host = FindPeer(NM_ListenServer);
        if (!AtFloor(Host, 2, 2, 1) || !AtFloor(FindPeer(NM_Client), 2, 2, 1)) return false;
        TestTrue(TEXT("Unreal reused the hidden actor"), Room->Original.IsValid() && Room->Original.Get() == FindLift(Host, 2));
        TestTrue(TEXT("Floor two stays at its original height after showing the room"),
            FindLift(Host, 2)->GetActorLocation().Equals(Room->FloorTwoLocation, 0.1));
        TestTrue(TEXT("Only fresh lookup works after showing the room"), Room->Caller.Signal(TEXT("Shown"), 0) &&
            Room->Output(TEXT("Stale")) && Room->Output(TEXT("New")));
        for (auto Stream : {Room->Host, Room->Client})
        {
            Stream->SetShouldBeVisible(false);
            Stream->SetShouldBeLoaded(false);
        }
        return true;
    });
    Steps(TEXT("fully unload the room on both peers"), [this, Room]
    {
        if (Room->Host->GetLoadedLevel() || Room->Client->GetLoadedLevel()) return false;
        TestTrue(TEXT("Registry has no freight lift after unload"),
            FindPeer(NM_ListenServer)->GetSubsystem<UAngelIOTargetRegistry>()->FindTargets(TEXT("FreightLift")).IsEmpty());
        for (auto Stream : {Room->Host, Room->Client})
        {
            Stream->SetShouldBeLoaded(true);
            Stream->SetShouldBeVisible(true);
        }
        return true;
    });
    Steps(TEXT("resolve the reloaded actor and start another ride"), [this, Room]
    {
        if (!AtFloor(FindPeer(NM_ListenServer), 2, 2, 1) || !AtFloor(FindPeer(NM_Client), 2, 2, 1)) return false;
        TestTrue(TEXT("New ride awaits completion"), Room->Caller.Signal(TEXT("Reloaded"), 0) && Room->Caller.IsWaitingOnOperation());
        return true;
    });
    Steps(TEXT("client sees arrival from the reloaded room"), [this, Room]
    {
        auto* Host = FindPeer(NM_ListenServer);
        Room->Caller.ResumeCompletedOperation();
        if (!AtFloor(Host, 2, 6, 2) || !AtFloor(FindPeer(NM_Client), 2, 6, 2)) return false;
        const FVector FloorSix = Room->FloorTwoLocation + FVector(0, 0, 4 * 200);
        TestTrue(TEXT("Reloaded lift arrives at the physical sixth floor"), FindLift(Host, 2)->GetActorLocation().Equals(FloorSix, 0.1));
        if (!FindLift(FindPeer(NM_Client), 2)->GetActorLocation().Equals(FloorSix, 1)) return false;
        TestTrue(TEXT("Persistent caller finishes its new ride"), Room->Output(TEXT("Arrived")));
        int32 Unloaded = 0, Gone = 0;
        Host->GetSubsystem<UAngelIOTraceSubsystem>()->VisitEvents([&](const FJsonObject& Event, const UObject*)
        {
            FString Reason;
            if (!Event.TryGetStringField(TEXT("reason"), Reason) || Event.GetStringField(TEXT("status")) != TEXT("Cancelled")) return;
            if (Reason == TEXT("TargetUnloaded")) ++Unloaded;
            if (Reason == TEXT("TargetGone")) ++Gone;
        });
        TestEqual(TEXT("One interrupted ride"), Unloaded, 1);
        TestEqual(TEXT("Each stale call has its own TargetGone result"), Gone, 3);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOInspectorPIE, "AngelIO.Network.ListenServer.MissionInspector",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOInspectorPIE::RunTest(const FString&)
{
    if (!TestNull(TEXT("PIE must be idle"), GEditor->PlayWorld.Get())) return false;
    auto Tab = FGlobalTabmanager::Get()->TryInvokeTab(SAngelIOMissionInspector::TabId);
    if (!TestTrue(TEXT("Open registered Mission Inspector tab"), Tab.IsValid())) return false;
    auto Inspector = StaticCastSharedRef<SAngelIOMissionInspector>(Tab->GetContent());
    const auto Ride = MakeShared<FString>();
    auto WaitFor = ListenSteps(this);
    StartListenSession();
    WaitFor(TEXT("inspect idle lift and remote panel"), [this, Inspector]
    {
        UWorld* Host = FindPeer(NM_ListenServer);
        UWorld* Client = FindPeer(NM_Client);
        if (!AtFloor(Host, 2, 2, 1) || !AtFloor(Client, 2, 2, 1)) return false;
        auto* PC = Cast<AAngelIOPlayerController>(Client->GetFirstPlayerController());
        if (!PC || PC->TripPanel->GetWaitingSignal() != TEXT("CallLift")) return false;
        Inspector->SelectWorld(Host);
        Inspector->SelectMission(FindLift(Host, 2)->Mission.Get());
        TestEqual(TEXT("Signal wait skips generated API frame"), Inspector->Location, FString(TEXT("Elevator.as:18")));
        TestEqual(TEXT("Signal name"), Inspector->Inspection.Waiting, FString(TEXT("RequestFloor")));
        Inspector->SelectWorld(Client);
        Inspector->SelectMission(PC->TripPanel.Get());
        if (!Inspector->AuthorityMission.IsValid()) return false;
        TestEqual(TEXT("Client panel maps to authoritative wait"), Inspector->Location, FString(TEXT("TypedLift.as:11")));
        PC->LiftTrip(6, 200);
        return true;
    });
    WaitFor(TEXT("inspect active trip from client panel and host lift"), [this, Inspector, Ride]
    {
        Inspector->Refresh();
        if (!Inspector->Inspection.Operation.IsValid() || !Inspector->Origin.Contains(TEXT("LiftTrip.Wait"))) return false;
        UWorld* Host = FindPeer(NM_ListenServer);
        *Ride = Inspector->Inspection.Operation.ToString(EGuidFormats::DigitsWithHyphensLower);
        TestEqual(TEXT("Operation wait site"), Inspector->Location, FString(TEXT("TypedLift.as:22")));
        TestEqual(TEXT("Follow the exact awaited operation"), Inspector->DisplayedId, *Ride);
        TestTrue(TEXT("Caller, waiter and arguments are visible"),
            Inspector->Timeline.Contains(TEXT("TypedLift.as:21")) && Inspector->Timeline.Contains(TEXT("TypedLift.as:22")) &&
            Inspector->Timeline.Contains(TEXT("FreightLift")) && Inspector->Timeline.Contains(TEXT("\"floor\":6")));
        Inspector->SelectMission(FindLift(FindPeer(NM_Client), 2)->Mission.Get());
        TestEqual(TEXT("Client lift maps to the same server trip"), Inspector->DisplayedId, *Ride);
        TestEqual(TEXT("Client lift shows the server source line"), Inspector->Location, FString(TEXT("Elevator.as:25")));
        Inspector->SelectWorld(Host);
        Inspector->SelectMission(FindLift(Host, 3)->Mission.Get());
        TestTrue(TEXT("Other lift has no history for this ride"), Inspector->Operations.IsEmpty());
        Inspector->SelectMission(FindLift(Host, 2)->Mission.Get());
        TestEqual(TEXT("Lift's physical wait site"), Inspector->Location, FString(TEXT("Elevator.as:25")));
        TestEqual(TEXT("Same operation at the executor"), Inspector->DisplayedId, *Ride);
        CastChecked<AAngelIOPlayerController>(Host->GetFirstPlayerController())->LiftTrip(7, 400);
        Inspector->Refresh();
        TestEqual(TEXT("A rejected call cannot replace the moving trip"), Inspector->DisplayedId, *Ride);
        const auto* Rejected = Inspector->Operations.FindByPredicate([](const auto& Op){return Op.Status == TEXT("Rejected");});
        if (!TestNotNull(TEXT("Rejected operation available in picker"), Rejected)) return true;
        Inspector->SelectOperation(Rejected->Id);
        TestTrue(TEXT("Historical result and reason visible"), Inspector->Timeline.Contains(TEXT("LiftBusy")));
        Inspector->SelectOperation(*Ride);
        return true;
    });
    WaitFor(TEXT("selected history survives completion"), [this, Inspector, Ride]
    {
        if (!AtFloor(FindPeer(NM_Client), 2, 6, 2)) return false;
        Inspector->Refresh();
        TestEqual(TEXT("Lift returned to input wait"), Inspector->Location, FString(TEXT("Elevator.as:18")));
        TestEqual(TEXT("Pinned trip remains selected"), Inspector->DisplayedId, *Ride);
        TestTrue(TEXT("Terminal result and resumed waiter visible"),
            Inspector->Timeline.Contains(TEXT("Arrived")) && Inspector->Timeline.Contains(TEXT("RESUME")));
        auto* Selected = FindLift(FindPeer(NM_ListenServer), 2);
        Selected->Destroy();
        Inspector->Refresh();
        TestTrue(TEXT("Destroyed selection releases its history"), Inspector->DisplayedId.IsEmpty() && Inspector->Timeline.IsEmpty());
        return true;
    });
    EndListenSession();
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Inspector, Tab]
    {
        Inspector->Refresh();
        TestTrue(TEXT("PIE ended clears inspector"), !Inspector->World.IsValid() && Inspector->Location.IsEmpty());
        Tab->RequestCloseTab();
        return true;
    }));
    return true;
}
#endif
