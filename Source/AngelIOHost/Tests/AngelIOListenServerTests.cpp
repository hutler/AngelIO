#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AngelIOTestSignalObserver.h"
#include "../AngelIOExampleLift.h"
#include "../AngelIOPlayerController.h"
#include "AngelIOScriptComponent.h"
#include "AngelIOTargetRegistry.h"
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOListen, "AngelIO.Network.ListenServer.NamedGroupAndLateJoin",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOListen::RunTest(const FString&)
{
    if (!TestNull(TEXT("PIE must be idle"), GEditor->PlayWorld.Get())) return false;
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

    const auto Peers = MakeShared<FListenPeers>();
    auto WaitFor = [this](FString Step, TFunction<bool()> Check)
    {
        ADD_LATENT_AUTOMATION_COMMAND(FUntilCommand(
            [this, Check] { return HasAnyErrors() || Check(); },
            [this, Step] { AddError(TEXT("Timed out: ") + Step); return true; }, 35.f));
    };

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
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([] { GEditor->RequestEndPlayMap(); return true; }));
    ADD_LATENT_AUTOMATION_COMMAND(FUntilCommand([] { return !GEditor->PlayWorld; }, 15.f));
    return true;
}
#endif
