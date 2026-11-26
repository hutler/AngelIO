#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AngelIOTestSignalObserver.h"
#include "../AngelIOExampleLift.h"
#include "../AngelIOLiftTrip.h"
#include "../AngelIOPlayerController.h"
#include "../AngelIORoomDoor.h"
#include "../AngelIORoomTerminal.h"
#include "AngelIOScriptAPI.h"
#include "AngelIOPersistenceIdentity.h"
#include "AngelIOBytecodeCacheStats.h"
#include "AngelIOScriptComponent.h"
#include "AngelIOTargetRegistry.h"
#include "AngelIOTraceSubsystem.h"
#include "SAngelIOMissionInspector.h"
#include "Tests/AngelIOStartupProbe.h"
#include "Features/IModularFeatures.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/NetConnection.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/Pawn.h"
#include "WorldPartition/WorldPartition.h"
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
        Lift->Mission->GetOutput(TEXT("TripSpeed"), Speed) && (Trips ? Speed.Float > 0 : Speed.Float == 0) &&
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
TUniquePtr<TGuardValue<FString>> ListenPlayURL;

void StartListenSession(const TCHAR* Map = TEXT("/Game/Demo/TwoLifts"), const FString& Slot = FString(), const FString& Options = {})
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
    ListenPlayURL = MakeUnique<TGuardValue<FString>>(GEditor->UserEditedPlayWorldURL, FString(Map) + TEXT("?AngelIOCheckpointSlot=") + Slot + Options);
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
    ADD_LATENT_AUTOMATION_COMMAND(FUntilCommand([]
    {
        if (GEditor->PlayWorld) return false;
        ListenPlayURL.Reset();
        return true;
    }, 15.f));
}

bool TripOutput(UAngelIOScriptComponent* Panel, const TCHAR* Name, int32 Expected)
{
    FAngelIOValue Value;
    return Panel && Panel->GetOutput(Name, Value) && Value.Integer == Expected;
}

ULevelStreamingDynamic* LoadLiftRoom(UWorld* World, bool bInitiallyVisible = true, const FString& RoomName = TEXT("AngelIOStreamedRoom"))
{
    bool bSuccess = false;
    ULevelStreamingDynamic::FLoadLevelInstanceParams Params(World, TEXT("/Game/Demo/TwoLifts"), FTransform(FVector(1200, -1600, 300)));
    Params.OptionalLevelNameOverride = &RoomName;
    Params.bInitiallyVisible = bInitiallyVisible;
    auto* Stream = ULevelStreamingDynamic::LoadLevelInstance(Params, bSuccess);
    return bSuccess ? Stream : nullptr;
}

UAngelIOScriptComponent* DeliveryPanel(ULevelStreamingDynamic* Room, int32 StartupFloor = 3)
{
    if (Room && Room->GetLoadedLevel())
        for (const auto& Actor : Room->GetLoadedLevel()->Actors)
            if (auto* Lift = Cast<AAngelIOExampleLift>(Actor); Lift && Lift->RequestedFloor == StartupFloor) return Lift->Address;
    return nullptr;
}
}

namespace
{
struct FDoorRoomView
{
    TWeakObjectPtr<ALevelInstance> Instance;
    TWeakObjectPtr<AAngelIORoomDoor> Door;
    TWeakObjectPtr<AAngelIORoomTerminal> Terminal;
    TWeakObjectPtr<AAngelIOExampleLift> Lift;

    bool Resolve(UWorld* World, double X)
    {
        if (!World) return false;
        for (TActorIterator<ALevelInstance> It(World); It; ++It)
        {
            if (!FMath::IsNearlyEqual(It->GetActorLocation().X, X, 1.0)) continue;
            Instance = *It;
            if (ULevel* Room = It->GetLoadedLevel())
                for (AActor* Actor : Room->Actors)
                {
                    if (auto* D = Cast<AAngelIORoomDoor>(Actor)) Door = D;
                    if (auto* T = Cast<AAngelIORoomTerminal>(Actor)) Terminal = T;
                    if (auto* L = Cast<AAngelIOExampleLift>(Actor)) Lift = L;
                }
            return It->GetLoadedLevel() && Door.IsValid() && Terminal.IsValid();
        }
        return false;
    }

    bool Find(UWorld* World, double X) { return Resolve(World, X) && At(false); }

    bool DeliveryAt(int32 Phase, FString Wait, int32 Floor, int32 Trips) const
    {
        if (!Terminal.IsValid() || !Lift.IsValid() || !Door.IsValid()) return false;
        FAngelIOValue Lamp;
        const bool Open = Phase != 0;
        return Terminal->Mission->GetWaitingSignal() == Wait && TripOutput(Terminal->Mission, TEXT("Phase"), Phase) &&
            TripOutput(Lift->Mission, TEXT("LastFloor"), Floor) && TripOutput(Lift->Mission, TEXT("TotalTrips"), Trips) &&
            Door->IsOpen() == Open && FMath::IsNearlyEqual(Door->Leaf->GetRelativeLocation().Z, Open ? 340.0 : 110.0, 0.1) &&
            FMath::IsNearlyEqual(Lift->GetActorLocation().Z, 10.0 + 200.0 * (Floor - 1), 1.0) &&
            Terminal->Mission->GetOutput(TEXT("Lamp"), Lamp) && !Lamp.Boolean && !Terminal->Lamp->IsVisible();
    }

    bool At(bool bOpened) const
    {
        FAngelIOValue Result;
        return Door.IsValid() && Terminal.IsValid() && Door->IsOpen() == bOpened &&
            FMath::IsNearlyEqual(Door->Leaf->GetRelativeLocation().Z, bOpened ? 340.0 : 110.0, 0.01) &&
            Terminal->Mission->GetWaitingSignal() == TEXT("Activate") &&
            Terminal->Mission->GetOutput(TEXT("Opened"), Result) && Result.Boolean == bOpened;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIODoorRooms, "AngelIO.Network.ListenServer.DoorRooms",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAngelIODoorRooms::RunTest(const FString&)
{
    struct FSession
    {
        FDoorRoomView HostA, HostB, ClientA, ClientB;
        TWeakObjectPtr<AAngelIORoomPlayerController> HostRemote, Client;
        FAngelIOProgram HeldDoor;
    };
    auto S = MakeShared<FSession>();
    StartListenSession(TEXT("/Game/Demo/TwoDoorRooms"));
    const auto Steps = ListenSteps(this);
    Steps(TEXT("both peers load two copies of DoorRoom and the remote player joins"), [this, S]
    {
        auto* Host = FindPeer(NM_ListenServer);
        auto* Client = FindPeer(NM_Client);
        if (!Host || !Client) return false;
        for (auto It = Host->GetPlayerControllerIterator(); It; ++It)
            if (auto* PC = Cast<AAngelIORoomPlayerController>(It->Get()); PC && !PC->IsLocalController()) S->HostRemote = PC;
        S->Client = Cast<AAngelIORoomPlayerController>(Client->GetFirstPlayerController());
        if (!S->HostRemote.IsValid() || !S->HostRemote->GetPawn() || !S->Client.IsValid() ||
            !S->Client->GetNetConnection() || !S->Client->GetPawn() ||
            !S->HostA.Find(Host, 0) || !S->HostB.Find(Host, 850) ||
            !S->ClientA.Find(Client, 0) || !S->ClientB.Find(Client, 850)) return false;
        TestEqual(TEXT("One room asset"), S->HostA.Instance->GetWorldAsset(), S->HostB.Instance->GetWorldAsset());
        TestEqual(TEXT("Both terminal instances use the same mission"), S->HostA.Terminal->Mission->Script, S->HostB.Terminal->Mission->Script);
        FAngelIOProgram Unscoped;
        FAngelIOValue Found;
        TestTrue(TEXT("Unscoped duplicate names and missing names resolve to null"), Unscoped.Compile(TEXT("AmbiguousDoor.as"),
            TEXT("bool out_Found; void Run() { out_Found = FindDoor(\"RoomDoor\") !is null || FindDoor(\"Missing\") !is null; }"), Host) &&
            Unscoped.Start({}, Host) && Unscoped.GetOutput(TEXT("Found"), Found) && !Found.Boolean);
        TestTrue(TEXT("Capture a typed handle from A before unloading"), S->HeldDoor.Compile(TEXT("HeldDoor.as"),
            TEXT("enum InputSignal { Probe } bool out_Found; bool out_Rejected; void Run() { Door@ held = FindDoor(\"Door\"); ")
            TEXT("out_Found = held !is null; WaitFor(InputSignal::Probe); DoorOpen@ request = held.Open(); ")
            TEXT("request.Wait(); out_Rejected = !request.Opened; }"), Host) &&
            S->HeldDoor.Start({}, Host, S->HostA.Terminal->Mission) && S->HeldDoor.GetOutput(TEXT("Found"), Found) && Found.Boolean);
        return true;
    });
    Steps(TEXT("remote player uses A from within range"), [S]
    {
        if (!S->ClientA.Terminal->IsInReach(S->Client->GetPawn())) return false;
        S->Client->ServerUseTerminal(S->ClientB.Terminal.Get()); // Same reliable channel, but out of reach.
        S->Client->UseTerminal();
        return true;
    });
    Steps(TEXT("only A opens on the server and connected client"), [this, S]
    {
        if (!S->HostA.At(true) || !S->ClientA.At(true)) return false;
        TestTrue(TEXT("Both peers still see B closed and waiting"), S->HostB.At(false) && S->ClientB.At(false));
        AddInfo(TEXT("DoorRoom test: A opened on both peers, B stayed closed."));
        return true;
    });
    Steps(TEXT("remote player walks across the walkway into reach of B on both peers"), [S]
    {
        if (!S->ClientB.Terminal->IsInReach(S->Client->GetPawn()))
            S->Client->GetPawn()->AddMovementInput(FVector::ForwardVector, 1);
        if (!S->HostB.Terminal->IsInReach(S->HostRemote->GetPawn()) ||
            !S->ClientB.Terminal->IsInReach(S->Client->GetPawn())) return false;
        S->HostA.Instance->UnloadLevelInstance();
        S->ClientA.Instance->UnloadLevelInstance();
        return true;
    });
    Steps(TEXT("A unloads and its saved handle cannot operate B"), [this, S]
    {
        if (S->HostA.Instance->GetLoadedLevel() || S->ClientA.Instance->GetLoadedLevel()) return false;
        FAngelIOValue Rejected;
        TestTrue(TEXT("B is the only registered door"), S->HostRemote->GetWorld()->GetSubsystem<UAngelIOTargetRegistry>()
            ->FindUniqueActor(TEXT("RoomDoor"), AAngelIORoomDoor::StaticClass()) == S->HostB.Door.Get());
        TestTrue(TEXT("The previous handle returns a failed DoorOpen"), S->HeldDoor.Signal(TEXT("Probe"), 0) &&
            S->HeldDoor.GetOutput(TEXT("Rejected"), Rejected) && Rejected.Boolean);
        TestTrue(TEXT("Original B terminal and door survived on both peers"), S->HostB.At(false) && S->ClientB.At(false));
        return true;
    });
    Steps(TEXT("remote player uses the surviving B terminal"), [S]
    {
        if (!S->ClientB.Terminal->IsInReach(S->Client->GetPawn())) return false;
        S->Client->UseTerminal();
        return true;
    });
    Steps(TEXT("B opens on both peers while A remains unloaded"), [this, S]
    {
        if (!S->HostB.At(true) || !S->ClientB.At(true)) return false;
        TestTrue(TEXT("A remains unloaded"), !S->HostA.Instance->GetLoadedLevel() && !S->ClientA.Instance->GetLoadedLevel());
        AddInfo(TEXT("DoorRoom test: B opened on both peers after A unloaded."));
        return true;
    });
    EndListenSession();
    return true;
}

namespace
{
int32 DeliveryCalls(UWorld* World, const TCHAR* Operation)
{
    int32 Count = 0;
    World->GetSubsystem<UAngelIOTraceSubsystem>()->VisitEvents([&](const FJsonObject& E, const UObject*)
    {
        FString Name;
        if (E.TryGetStringField(TEXT("operation"), Name) && Name == Operation) ++Count;
    });
    return Count;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOTwoRoomDelivery, "AngelIO.Network.ListenServer.TwoRoomDelivery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOTwoRoomDelivery::RunTest(const FString&)
{
    struct FSession
    {
        FDoorRoomView A, B, ClientA, ClientB;
        TWeakObjectPtr<AAngelIORoomPlayerController> Player;
        TWeakObjectPtr<APawn> RemotePawn;
        FGuid ARun, BRun, ACheckpoint;
        FString AOwner;
        TStrongObjectPtr<UAngelIOTestSignalObserver> ClientMotion{NewObject<UAngelIOTestSignalObserver>()};
        bool Approached(const FDoorRoomView& Room, float Direction) const
        {
            const auto& ServerRoom = Direction > 0 ? B : A;
            if (Room.Terminal->IsInReach(Player->GetPawn()) && ServerRoom.Terminal->IsInReach(RemotePawn.Get())) return true;
            Player->GetPawn()->AddMovementInput(FVector::ForwardVector, Direction);
            return false;
        }
    };
    const auto S = MakeShared<FSession>();
    StartListenSession(TEXT("/Game/Demo/TwoDeliveryRooms"));
    const auto Steps = ListenSteps(this);
    Steps(TEXT("both instances start independently on both peers"), [S]
    {
        auto* Host = FindPeer(NM_ListenServer);
        auto* Client = FindPeer(NM_Client);
        if (!S->A.Resolve(Host, 0) || !S->B.Resolve(Host, 850) || !S->ClientA.Resolve(Client, 0) ||
            !S->ClientB.Resolve(Client, 850)) return false;
        for (auto* Room : {&S->A, &S->B, &S->ClientA, &S->ClientB})
            if (!Room->DeliveryAt(0, TEXT("Dispatch"), 1, 0)) return false;
        S->Player = Cast<AAngelIORoomPlayerController>(Client->GetFirstPlayerController());
        for (auto It = Host->GetPlayerControllerIterator(); It; ++It)
            if (auto* PC = It->Get(); PC && !PC->IsLocalController()) S->RemotePawn = PC->GetPawn();
        if (!S->RemotePawn.IsValid() || !S->Player.IsValid() || !S->ClientA.Terminal->IsInReach(S->Player->GetPawn())) return false;
        S->ARun = S->A.Terminal->Mission->InspectMission().Instance;
        S->BRun = S->B.Terminal->Mission->InspectMission().Instance;
        S->AOwner = FAngelIOPersistenceIdentity::From(*S->A.Terminal->Mission).Owner;
        Observe(S->ClientA.Lift->Mission, S->ClientMotion.Get());
        S->Player->UseTerminal();
        return true;
    });
    Steps(TEXT("client sees the active lamp and A delivery arrives exactly once"), [S]
    {
        return S->ClientA.Terminal->Lamp->IsVisible();
    });
    Steps(TEXT("A awaits confirmation; B still awaits dispatch"), [this, S]
    {
        if (!S->A.DeliveryAt(3, TEXT("Confirm"), 4, 1) || !S->ClientA.DeliveryAt(3, TEXT("Confirm"), 4, 1) ||
            S->ClientMotion->Presentations != 1) return false;
        TestTrue(TEXT("B is untouched on both peers"), S->B.DeliveryAt(0, TEXT("Dispatch"), 1, 0) &&
            S->ClientB.DeliveryAt(0, TEXT("Dispatch"), 1, 0));
        S->ACheckpoint = S->A.Terminal->Mission->InspectMission().CheckpointId;
        return true;
    });
    Steps(TEXT("walk to B and unload A"), [S]
    {
        if (!S->Approached(S->ClientB, 1)) return false;
        S->A.Instance->UnloadLevelInstance();
        S->ClientA.Instance->UnloadLevelInstance();
        return true;
    });
    Steps(TEXT("dispatch B from the connected client while A is absent"), [S]
    {
        if (S->A.Instance->GetLoadedLevel() || S->ClientA.Instance->GetLoadedLevel()) return false;
        S->Player->UseTerminal();
        return true;
    });
    Steps(TEXT("reload A while B has an active ride"), [S]
    {
        if (S->B.Terminal->Mission->GetWaitingSignal() != TEXT("LiftTrip.Wait")) return false;
        S->A.Instance->LoadLevelInstance();
        S->ClientA.Instance->LoadLevelInstance();
        return true;
    });
    Steps(TEXT("A resumes Delivered with floor 4 and zero new trips; B finishes in its original run"), [this, S]
    {
        auto* Host = FindPeer(NM_ListenServer);
        if (!S->A.Resolve(Host, 0) || !S->ClientA.Resolve(S->Player->GetWorld(), 0) ||
            !S->A.DeliveryAt(3, TEXT("Confirm"), 4, 0) || !S->ClientA.DeliveryAt(3, TEXT("Confirm"), 4, 0) ||
            !S->B.DeliveryAt(3, TEXT("Confirm"), 4, 1) || !S->ClientB.DeliveryAt(3, TEXT("Confirm"), 4, 1)) return false;
        const auto A = S->A.Terminal->Mission->InspectMission();
        const auto B = S->B.Terminal->Mission->InspectMission();
        const auto KeyA = FAngelIOPersistenceIdentity::From(*S->A.Terminal->Mission);
        const auto KeyB = FAngelIOPersistenceIdentity::From(*S->B.Terminal->Mission);
        TestTrue(TEXT("A retained its stable address; B shares the template but has its own room instance"), KeyA.Placement.IsValid() &&
            KeyA.Placement == KeyB.Placement && KeyA.Room != KeyB.Room && KeyA.Owner == S->AOwner && KeyA.Owner != KeyB.Owner);
        TestEqual(TEXT("A reads its Delivered checkpoint"), A.RestoredStage, FString(TEXT("Delivered")));
        TestTrue(TEXT("Replacement VM retains A's checkpoint"), A.Instance != S->ARun && A.CheckpointId == S->ACheckpoint && A.CheckpointId.IsValid());
        TestTrue(TEXT("B keeps its original VM and independent checkpoint"), B.Instance == S->BRun && B.CheckpointId != A.CheckpointId);
        TestEqual(TEXT("Each delivery issued exactly one MoveTo"), DeliveryCalls(Host, TEXT("Lift.MoveTo")), 2);
        TestEqual(TEXT("Each door was opened once"), DeliveryCalls(Host, TEXT("Door.Open")), 2);
        GEditor->RequestLateJoin();
        return true;
    });
    Steps(TEXT("late client sees both restored physical and mission states"), [S]
    {
        auto* Late = FindPeer(NM_Client, S->Player->GetWorld());
        FDoorRoomView A, B;
        if (!A.Resolve(Late, 0) || !B.Resolve(Late, 850) || !A.DeliveryAt(3, TEXT("Confirm"), 4, 0) ||
            !B.DeliveryAt(3, TEXT("Confirm"), 4, 1)) return false;
        S->Player->UseTerminal();
        return true;
    });
    Steps(TEXT("confirm B, then walk back and confirm the restored A mission"), [S]
    {
        if (!S->ClientB.DeliveryAt(4, TEXT(""), 4, 1) || !S->Approached(S->ClientA, -1)) return false;
        S->Player->UseTerminal();
        return true;
    });
    Steps(TEXT("both rooms complete with only their original deliveries"), [this, S]
    {
        if (!S->ClientA.DeliveryAt(4, TEXT(""), 4, 0) || !S->A.DeliveryAt(4, TEXT(""), 4, 0)) return false;
        TestEqual(TEXT("Confirmation never issues another ride"), DeliveryCalls(FindPeer(NM_ListenServer), TEXT("Lift.MoveTo")), 2);
        TestTrue(TEXT("Both terminals replicated one independent delivery credit"), TripOutput(S->ClientA.Terminal->Mission, TEXT("Credits"), 1) &&
            TripOutput(S->ClientB.Terminal->Mission, TEXT("Credits"), 1));
        return true;
    });
    EndListenSession();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIODeliveryProfile, "AngelIO.Network.ListenServer.DeliveryProfile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAngelIODeliveryProfile::RunTest(const FString&)
{
    struct FSample
    {
        FDoorRoomView A, B, ClientA, ClientB;
        TWeakObjectPtr<UNetConnection> Connection;
        FDelegateHandle Begin, End;
        double Tick = 0, Start = 0;
        int32 Received = 0, Sent = 0;
        TArray<double> HostMs, FrameMs;
        ~FSample()
        {
            FWorldDelegates::OnWorldTickStart.Remove(Begin);
            FWorldDelegates::OnWorldTickEnd.Remove(End);
        }
    };
    auto S = MakeShared<FSample>();
    StartListenSession(TEXT("/Game/Demo/TwoDeliveryRooms"));
    const auto Steps = ListenSteps(this);
    Steps(TEXT("profile the host and loopback client during two concurrent deliveries"), [S]
    {
        auto* Host = FindPeer(NM_ListenServer);
        auto* Client = FindPeer(NM_Client);
        if (!S->A.Resolve(Host, 0) || !S->B.Resolve(Host, 850) || !S->ClientA.Resolve(Client, 0) ||
            !S->ClientB.Resolve(Client, 850) || !S->ClientB.DeliveryAt(0, TEXT("Dispatch"), 1, 0) ||
            !S->ClientA.DeliveryAt(0, TEXT("Dispatch"), 1, 0)) return false;
        auto* PC = Cast<AAngelIORoomPlayerController>(Client->GetFirstPlayerController());
        if (!PC || !S->ClientA.Terminal->IsInReach(PC->GetPawn()) || !PC->GetNetConnection()) return false;
        S->Connection = PC->GetNetConnection();
        S->Received = S->Connection->InTotalBytes;
        S->Sent = S->Connection->OutTotalBytes;
        S->Start = FPlatformTime::Seconds();
        // Delegate lifetime is bounded by the latent test state on success, failure and PIE teardown.
        S->Begin = FWorldDelegates::OnWorldTickStart.AddLambda([Sample=&S.Get(), Host](UWorld* W, ELevelTick, float)
        {
            if (W != Host) return;
            const double Now = FPlatformTime::Seconds();
            if (Sample->Tick) Sample->FrameMs.Add((Now - Sample->Tick) * 1000);
            Sample->Tick = Now;
        });
        S->End = FWorldDelegates::OnWorldTickEnd.AddLambda([Sample=&S.Get(), Host](UWorld* W, ELevelTick, float)
        {
            if (W == Host && Sample->Tick) Sample->HostMs.Add((FPlatformTime::Seconds() - Sample->Tick) * 1000);
        });
        PC->UseTerminal();
        S->B.Terminal->Mission->SendSignal(TEXT("Dispatch"), 0);
        return true;
    });
    Steps(TEXT("sample up to replicated arrival and report percentiles"), [this, S]
    {
        if (!S->ClientA.DeliveryAt(3, TEXT("Confirm"), 4, 1) || !S->ClientB.DeliveryAt(3, TEXT("Confirm"), 4, 1)) return false;
        const double Elapsed = FPlatformTime::Seconds() - S->Start;
        for (auto* Samples : {&S->HostMs, &S->FrameMs})
        {
            if (!TestTrue(TEXT("Recorded completed host frames"), !Samples->IsEmpty())) return true;
            Samples->Sort();
            auto P = [&](double Q) { return (*Samples)[FMath::CeilToInt(Q * Samples->Num()) - 1]; };
            AddInfo(FString::Printf(TEXT("AngelIO mixed %s n=%d p50=%.4f p95=%.4f p99=%.4f max=%.4f ms"),
                Samples == &S->HostMs ? TEXT("listen_world_tick") : TEXT("editor_frame_interval"),
                Samples->Num(), P(.50), P(.95), P(.99), Samples->Last()));
        }
        const int32 In = S->Connection->InTotalBytes - S->Received;
        const int32 Out = S->Connection->OutTotalBytes - S->Sent;
        TestTrue(TEXT("Both traffic directions measured"), In > 0 && Out > 0);
        AddInfo(FString::Printf(TEXT("AngelIO mixed delivery_wall_ms=%.2f client_rx_Bps=%.1f client_tx_Bps=%.1f"),
            Elapsed * 1000, In / Elapsed, Out / Elapsed));
        return true;
    });
    EndListenSession();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FAngelIODeliveryRecovery, "AngelIO.Network.ListenServer.DeliveryRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FAngelIODeliveryRecovery::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Case : {TEXT("DoorInFlight"), TEXT("DoorCommitted"), TEXT("RideInFlight"), TEXT("LandingCommitted"), TEXT("TimeoutRetry")})
    {
        Names.Add(Case);
        Commands.Add(Case);
    }
}
bool FAngelIODeliveryRecovery::RunTest(const FString& Case)
{
    struct FSession
    {
        FDoorRoomView A, ClientA;
        TWeakObjectPtr<AAngelIORoomPlayerController> Player;
        bool Connect()
        {
            auto* Client = FindPeer(NM_Client);
            if (!A.Resolve(FindPeer(NM_ListenServer), 0) || !ClientA.Resolve(Client, 0)) return false;
            Player = Cast<AAngelIORoomPlayerController>(Client->GetFirstPlayerController());
            return Player.IsValid() && ClientA.Terminal->IsInReach(Player->GetPawn());
        }
    };
    auto S = MakeShared<FSession>();
    const auto Steps = ListenSteps(this);
    const bool bDoor = Case.StartsWith(TEXT("Door"));
    const bool bCommitted = Case.EndsWith(TEXT("Committed"));
    const bool bTimeout = Case == TEXT("TimeoutRetry");
    const FString Slot = TEXT("Delivery-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    StartListenSession(TEXT("/Game/Demo/TwoDeliveryRooms"), Slot);
    Steps(TEXT("activate a fresh delivery from the client"), [this, S, bTimeout]
    {
        if (!S->Connect() || !S->ClientA.DeliveryAt(0, TEXT("Dispatch"), 1, 0)) return false;
        if (bTimeout)
        {
            FAngelIOValue Timeout;
            Timeout.Type = EAngelIOType::Float;
            Timeout.Float = 0.15f;
            TestTrue(TEXT("Shorten only the reader deadline"), S->A.Terminal->Mission->SetInput(TEXT("Timeout"), Timeout));
        }
        S->Player->UseTerminal();
        return true;
    });
    if (bTimeout)
    {
        Steps(TEXT("reader times out; player retries while the original cab is moving"), [this, S]
        {
            if (S->ClientA.Terminal->Mission->GetWaitingSignal() != TEXT("Retry") ||
                !TripOutput(S->ClientA.Terminal->Mission, TEXT("Failure"), 4)) return false;
            TestTrue(TEXT("Deadline did not end the native ride"), S->A.Lift->IsActorTickEnabled());
            FAngelIOValue Timeout;
            Timeout.Type = EAngelIOType::Float;
            Timeout.Float = 8.f;
            TestTrue(TEXT("Allow time for the in-flight ride to land"), S->A.Terminal->Mission->SetInput(TEXT("Timeout"), Timeout));
            S->Player->UseTerminal();
            return true;
        });
    }
    else
    {
        Steps(TEXT("interrupt the terminal at the selected native wait"), [this, S, bDoor, bCommitted]
        {
            if (S->A.Terminal->Mission->GetWaitingSignal() != (bDoor ? TEXT("DoorOpen.Wait") : TEXT("LiftTrip.Wait"))) return false;
            TestEqual(TEXT("Durable intent precedes the action"), S->A.Terminal->Mission->InspectMission().SavedStage,
                FString(bDoor ? TEXT("Opening") : TEXT("Dispatched")));
            if (bCommitted) S->A.Terminal->Mission->StopScript(); // Let the producer commit without advancing its caller.
            return true;
        });
        if (bCommitted)
            Steps(TEXT("native completion reached the client before ending the server"), [S, bDoor]
            {
                return bDoor ? S->ClientA.Door->IsOpen() : TripOutput(S->ClientA.Lift->Mission, TEXT("LastFloor"), 4);
            });
        EndListenSession();
        Steps(TEXT("start a new listen world with the same disk slot"), [S, Slot]
        {
            S->A = {};
            S->ClientA = {};
            StartListenSession(TEXT("/Game/Demo/TwoDeliveryRooms"), Slot);
            return true;
        });
    }
    Steps(TEXT("confirm the durable delivery, with no repeat of committed actions"), [this, S, bDoor, bCommitted, bTimeout]
    {
        const int32 Rides = !bDoor && bCommitted ? 0 : 1;
        if (!S->Connect() || !S->A.DeliveryAt(3, TEXT("Confirm"), 4, Rides) ||
            !S->ClientA.DeliveryAt(3, TEXT("Confirm"), 4, Rides)) return false;
        auto* World = S->A.Terminal->GetWorld();
        TestEqual(TEXT("Recovered world issued only the necessary door command"),
            DeliveryCalls(World, TEXT("Door.Open")), int32(bTimeout || (bDoor && !bCommitted)));
        TestEqual(TEXT("No duplicate ride for a committed landing"), DeliveryCalls(World, TEXT("Lift.MoveTo")), Rides);
        if (!bTimeout)
            TestEqual(TEXT("Recovered from the interrupted intent"), S->A.Terminal->Mission->InspectMission().RestoredStage,
                FString(bDoor ? TEXT("Opening") : TEXT("Dispatched")));
        S->Player->UseTerminal();
        return true;
    });
    Steps(TEXT("client sees the completed mission"), [S, bCommitted, bDoor]
    {
        return S->ClientA.DeliveryAt(4, TEXT(""), 4, !bDoor && bCommitted ? 0 : 1);
    });
    EndListenSession();
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Slot]
    {
        const FString File = FPaths::ProjectSavedDir() / TEXT("AngelIO/Checkpoints/PIE") / (TEXT("slot-") + Slot + TEXT(".sqlite3"));
        if (!HasAnyErrors()) TestTrue(TEXT("Remove the test's isolated SQLite slot"), IFileManager::Get().Delete(*File, true));
        return true;
    }));
    return true;
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
    struct FRoom final : IAngelIOScriptAPI
    {
        FRoom() { IModularFeatures::Get().RegisterModularFeature(FeatureName(), this); }
        ~FRoom() { IModularFeatures::Get().UnregisterModularFeature(FeatureName(), this); }
        bool RegisterBindings(asIScriptEngine&) override { ++Registrations; return true; }
        int32 Registrations = 0, PersistentModules = 0;
        TWeakPtr<FAngelIORegisteredEngine> OriginalEngine;
        TWeakObjectPtr<UWorld> OriginalWorld;
        FGuid OriginalMission;
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
    StartListenSession(TEXT("/Engine/Maps/Entry"));
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
        auto* PC = Cast<AAngelIOPlayerController>(Client->GetFirstPlayerController());
        if (!PC || PC->TripPanel->GetWaitingSignal() != TEXT("CallLift")) return false;
        if (!TestTrue(TEXT("Lookup is empty before loading"), Room->Caller.Compile(TEXT("StreamedLift.as"), Source, Host) &&
            Room->Caller.Start({}, Host) && Room->Output(TEXT("Absent")))) return true;
        Room->OriginalWorld = Host;
        Room->OriginalEngine = FAngelIOStartupProbe::WorldEngine(Host);
        Room->PersistentModules = FAngelIOStartupProbe::WorldModuleCount(Host);
        TestTrue(TEXT("Host owns a registered engine"), Room->OriginalEngine.IsValid());
        TestFalse(TEXT("Client replica has no script engine"), FAngelIOStartupProbe::WorldEngine(Client).IsValid());
        Room->Host = LoadLiftRoom(Host);
        Room->Client = LoadLiftRoom(Client);
        TestTrue(TEXT("Dynamic room requested on both peers"), Room->Host.IsValid() && Room->Client.IsValid());
        return true;
    });
    Steps(TEXT("hide room during an active ride"), [this, Room]
    {
        auto* Host = FindPeer(NM_ListenServer);
        if (!AtFloor(Host, 2, 2, 1) || !AtFloor(FindPeer(NM_Client), 2, 2, 1)) return false;
        Room->Original = FindLift(Host, 2);
        Room->OriginalMission = Room->Original->Mission->InspectMission().Instance;
        TestTrue(TEXT("Room missions have their own modules"), FAngelIOStartupProbe::WorldModuleCount(Host) > Room->PersistentModules);
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
        if (!AtFloor(Host, 2, 2, 0) || !AtFloor(FindPeer(NM_Client), 2, 2, 0)) return false;
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
        TestEqual(TEXT("Unloading the room discards only its script modules"),
            FAngelIOStartupProbe::WorldModuleCount(FindPeer(NM_ListenServer)), Room->PersistentModules);
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
        if (!AtFloor(FindPeer(NM_ListenServer), 2, 2, 0) || !AtFloor(FindPeer(NM_Client), 2, 2, 0)) return false;
        auto* Host = FindPeer(NM_ListenServer);
        TestTrue(TEXT("Reload uses the original world engine"), Room->OriginalEngine.Pin() == FAngelIOStartupProbe::WorldEngine(Host).Pin());
        TestEqual(TEXT("Room hide, unload and reload registered the API once in total"), Room->Registrations, 1);
        TestNotEqual(TEXT("Reloaded lift has a fresh mission"), FindLift(Host, 2)->Mission->InspectMission().Instance, Room->OriginalMission);
        TestTrue(TEXT("New ride awaits completion"), Room->Caller.Signal(TEXT("Reloaded"), 0) && Room->Caller.IsWaitingOnOperation());
        return true;
    });
    Steps(TEXT("client sees arrival from the reloaded room"), [this, Room]
    {
        auto* Host = FindPeer(NM_ListenServer);
        Room->Caller.ResumeCompletedOperation();
        if (!AtFloor(Host, 2, 6, 1) || !AtFloor(FindPeer(NM_Client), 2, 6, 1)) return false;
        const FVector FloorSix = Room->FloorTwoLocation + FVector(0, 0, 4 * 200);
        TestTrue(TEXT("Reloaded lift arrives at the physical sixth floor"), FindLift(Host, 2)->GetActorLocation().Equals(FloorSix, 0.1));
        if (!FindLift(FindPeer(NM_Client), 2)->GetActorLocation().Equals(FloorSix, 1)) return false;
        TestTrue(TEXT("Persistent caller finishes its new ride"), Room->Output(TEXT("Arrived")));
        const auto State = FindLift(Host, 2)->Mission->InspectMission().PersistedIntegers;
        if (TestEqual(TEXT("One persisted freight floor"), State.Num(), 1))
        {
            const auto& Floor = State.CreateConstIterator().Value();
            TestEqual(TEXT("New arrival updates the saved floor"), Floor.Saved, 6);
            TestTrue(TEXT("Original restore stays at floor two"), Floor.Restored.IsSet() && *Floor.Restored == 2);
        }
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
        // Relative travel carries the current PIE listen port into the replacement world.
        TestTrue(TEXT("Listen host accepts world travel"), Host->ServerTravel(TEXT("/Game/Demo/TwoLifts")));
        return true;
    });
    Steps(TEXT("reconnect to the replacement world and request a ride"), [this, Room]
    {
        auto* Host = FindPeer(NM_ListenServer);
        auto* Client = FindPeer(NM_Client);
        if (!Host || Host == Room->OriginalWorld.Get() || !AtFloor(Host, 2, 2, 1) || !AtFloor(Client, 2, 2, 1)) return false;
        auto* PC = Cast<AAngelIOPlayerController>(Client->GetFirstPlayerController());
        if (!PC || PC->TripPanel->GetWaitingSignal() != TEXT("CallLift") || !PC->GetNetConnection()) return false;
        TestFalse(TEXT("World teardown released the original engine"), Room->OriginalEngine.IsValid());
        TestTrue(TEXT("Native caller retained across travel is invalidated"), Room->Caller.State() == EAngelIOState::Failed);
        TestTrue(TEXT("Replacement world owns an engine"), FAngelIOStartupProbe::WorldEngine(Host).IsValid());
        TestFalse(TEXT("Reconnected client has no script engine"), FAngelIOStartupProbe::WorldEngine(Client).IsValid());
        TestEqual(TEXT("Each authority world registered the provider exactly once"), Room->Registrations, 2);
        PC->LiftTrip(4, 800);
        return true;
    });
    Steps(TEXT("replacement world executes the client trip"), []
    {
        return AtFloor(FindPeer(NM_ListenServer), 2, 4, 2) && AtFloor(FindPeer(NM_Client), 2, 4, 2);
    });
    EndListenSession();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FAngelIOStreamedBytecode, "AngelIO.Network.ListenServer.StreamedBytecode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FAngelIOStreamedBytecode::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    Names.Append({TEXT("Disabled"), TEXT("Eviction"), TEXT("Fits")});
    Commands.Append({TEXT("0"), TEXT("3"), TEXT("32")});
}

bool FAngelIOStreamedBytecode::RunTest(const FString& Parameter)
{
    if (!TestNull(TEXT("PIE must be idle"), GEditor->PlayWorld.Get())) return false;
    const int32 BudgetKiB = FCString::Atoi(*Parameter);
    const auto Inspector = SNew(SAngelIOMissionInspector);
    struct FRoom
    {
        TWeakObjectPtr<ULevelStreamingDynamic> Host, Client;
        TWeakPtr<FAngelIORegisteredEngine> Engine;
        int32 PersistentModules = 0;
        FAngelIOBytecodeCacheStats Before;
        FGuid FirstMission;
    };
    const auto Room = MakeShared<FRoom>();
    StartListenSession(TEXT("/Engine/Maps/Entry"), {}, FString::Printf(TEXT("?AngelIOBytecodeCacheKiB=%d"), BudgetKiB));
    const auto Steps = ListenSteps(this);
    Steps(TEXT("read the runtime cache after the remote player joins"), [this, Room, BudgetKiB, Inspector]
    {
        auto* Host = FindPeer(NM_ListenServer);
        auto* Client = FindPeer(NM_Client);
        auto* PC = Client ? Cast<AAngelIOPlayerController>(Client->GetFirstPlayerController()) : nullptr;
        if (!Host || !PC || !PC->GetNetConnection() || PC->TripPanel->GetWaitingSignal() != TEXT("CallLift")) return false;
        auto Stats = AngelIO::GetBytecodeCacheStats(Host);
        if (!TestTrue(TEXT("Host has a runtime cache"), Stats.IsSet()) ||
            !TestEqual(TEXT("Travel URL selects this world's budget"), Stats->BudgetBytes, uint64(BudgetKiB) * 1024)) return true;
        Room->Before = *Stats;
        Inspector->SelectWorld(Host);
        Room->Engine = FAngelIOStartupProbe::WorldEngine(Host);
        Room->PersistentModules = FAngelIOStartupProbe::WorldModuleCount(Host);
        Room->Host = LoadLiftRoom(Host, false);
        Room->Client = LoadLiftRoom(Client, false);
        TestTrue(TEXT("Requested a hidden room on both peers"), Room->Host.IsValid() && Room->Client.IsValid());
        return true;
    });
    for (bool bReload : {false, true})
    {
        if (bReload)
        {
            Steps(TEXT("verify full unload and retained cache before re-entry"), [this, Room]
            {
                if (Room->Host->GetLoadedLevel() || Room->Client->GetLoadedLevel()) return false;
                auto Stats = AngelIO::GetBytecodeCacheStats(Room->Host->GetWorld());
                if (!TestTrue(TEXT("World engine survives room unload"), Stats.IsSet())) return true;
                TestEqual(TEXT("Only persistent modules survive unload"),
                    FAngelIOStartupProbe::WorldModuleCount(Room->Host->GetWorld()), Room->PersistentModules);
                TestEqual(TEXT("Heap retained across unload"), Stats->RetainedBytes, Room->Before.RetainedBytes);
                TestEqual(TEXT("No entries lost during unload"), Stats->Entries, Room->Before.Entries);
                Room->Host->SetShouldBeLoaded(true);
                Room->Client->SetShouldBeLoaded(true);
                return true;
            });
        }
        Steps(TEXT("assign the same two panel scripts before room BeginPlay"), [this, Room]
        {
            if (!Room->Host->GetLoadedLevel() || !Room->Client->GetLoadedLevel()) return false;
            auto* Blink = DeliveryPanel(Room->Host.Get(), 2);
            auto* Delivery = DeliveryPanel(Room->Host.Get(), 3);
            if (!TestTrue(TEXT("Both placed lifts have panel components"), Blink && Delivery)) return true;
            Blink->Script = TEXT("Examples/BlinkPanel.as");
            Delivery->Script = TEXT("Examples/RoomDelivery.as");
            Blink->bStartOnBeginPlay = Delivery->bStartOnBeginPlay = true;
            Room->Host->SetShouldBeVisible(true);
            Room->Client->SetShouldBeVisible(true);
            return true;
        });
        Steps(TEXT("measure the four visible room missions"), [this, Room, BudgetKiB, bReload, Inspector]
        {
            if (!Room->Host->IsLevelVisible() || !Room->Client->IsLevelVisible()) return false;
            auto Stats = AngelIO::GetBytecodeCacheStats(Room->Host->GetWorld());
            if (!TestTrue(TEXT("Host runtime stats are available"), Stats.IsSet())) return true;
            Inspector->Refresh();
            const FString Expected = FString::Printf(TEXT("Retained %llu B / budget %llu B  ·  Hits %llu  ·  Evictions %llu"),
                Stats->RetainedBytes, Stats->BudgetBytes, Stats->Hits, Stats->Evictions);
            TestEqual(TEXT("World view shows live cache totals without selecting a mission"), Inspector->BytecodeCacheSummary(),
                BudgetKiB ? Expected : TEXT("Disabled · ") + Expected);
            FString Path, Json;
            TSharedPtr<FJsonObject> Export;
            if (!TestTrue(TEXT("Export the live world snapshot"), Room->Host->GetWorld()->GetSubsystem<UAngelIOTraceSubsystem>()->Export(Path) &&
                FFileHelper::LoadFileToString(Json, *Path) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Export))) return true;
            const TSharedPtr<FJsonObject>* Cache = nullptr;
            if (!TestTrue(TEXT("Trace includes the world's bytecode cache"), Export->TryGetObjectField(TEXT("bytecodeCache"), Cache))) return true;
            TestEqual(TEXT("Trace memory limit"), (*Cache)->GetNumberField(TEXT("budgetBytes")), double(Stats->BudgetBytes));
            TestEqual(TEXT("Trace retained heap"), (*Cache)->GetNumberField(TEXT("retainedBytes")), double(Stats->RetainedBytes));
            TestEqual(TEXT("Trace cumulative hits"), (*Cache)->GetNumberField(TEXT("hits")), double(Stats->Hits));
            TestEqual(TEXT("Trace cumulative evictions"), (*Cache)->GetNumberField(TEXT("evictions")), double(Stats->Evictions));
            const FString Detail = Inspector->BytecodeCacheDetails().ToString();
            uint64 ResidentBytes = 0;
            const auto& Residents = (*Cache)->GetArrayField(TEXT("residentScripts"));
            TestEqual(TEXT("Every resident script has a row"), Residents.Num(), Stats->Entries);
            for (const auto& Value : Residents)
            {
                const auto& Row = Value->AsObject();
                const FString Script = Row->GetStringField(TEXT("script"));
                const uint64 Bytes = uint64(Row->GetNumberField(TEXT("retainedBytes")));
                TestTrue(TEXT("Inspector shows the exported script and charge under resident scripts"),
                    Detail.Left(Detail.Find(TEXT("Recent cache activity"))).Contains(FString::Printf(TEXT("\n%s  ·  %llu B"), *Script, Bytes)));
                ResidentBytes += Bytes;
            }
            TestEqual(TEXT("Resident rows account for all retained bytes"), ResidentBytes, Stats->RetainedBytes);
            TArray<FString> VisitScripts;
            for (const auto& Value : (*Cache)->GetArrayField(TEXT("recentActivity")))
            {
                const auto& Row = Value->AsObject();
                const uint64 Sequence = uint64(Row->GetNumberField(TEXT("sequence")));
                const FString Script = Row->GetStringField(TEXT("script"));
                const bool bHit = Row->GetStringField(TEXT("type")) == TEXT("hit");
                TestTrue(TEXT("Inspector identifies exported hits and victims with their charges"),
                    Detail.Contains(FString::Printf(TEXT("#%llu  %s  ·  %s  ·  %llu B"), Sequence, bHit ? TEXT("Hit") : TEXT("Evicted"),
                        *Script, uint64(Row->GetNumberField(TEXT("retainedBytes"))))));
                if (bReload && Sequence > Room->Before.Hits + Room->Before.Evictions)
                {
                    TestEqual(TEXT("Reload activity agrees with the world budget"), bHit, BudgetKiB == 32);
                    VisitScripts.Add(Script);
                }
            }
            if (bReload)
            {
                TArray<FString> ExpectedScripts;
                if (BudgetKiB) ExpectedScripts = {TEXT("Elevator.as"), TEXT("Elevator.as"), TEXT("Examples/BlinkPanel.as"), TEXT("Examples/RoomDelivery.as")};
                VisitScripts.Sort();
                TestEqual(TEXT("Reload attributes all four accesses to the right scripts"),
                    FString::Join(VisitScripts, TEXT(", ")), FString::Join(ExpectedScripts, TEXT(", ")));
            }
            AddInfo(FString::Printf(TEXT("Cache trace budget_kib=%d phase=%s file=%s"), BudgetKiB,
                bReload ? TEXT("reload") : TEXT("first"), *FPaths::GetCleanFilename(Path)));
            const uint64 Hits = Stats->Hits - Room->Before.Hits, Builds = Stats->SourceBuilds - Room->Before.SourceBuilds;
            const uint64 Evictions = Stats->Evictions - Room->Before.Evictions;
            TestEqual(TEXT("Four room starts per visit"), Hits + Builds, uint64(4));
            TestEqual(TEXT("Four fresh room modules"), FAngelIOStartupProbe::WorldModuleCount(Room->Host->GetWorld()), Room->PersistentModules + 4);
            TestFalse(TEXT("No cache or VM on the client"), AngelIO::GetBytecodeCacheStats(Room->Client->GetWorld()).IsSet());
            TestTrue(TEXT("All retained allocations fit in the world budget"), Stats->RetainedBytes <= Stats->BudgetBytes);
            if (BudgetKiB == 32)
            {
                TestEqual(TEXT("Working set produces the expected hits"), Hits, bReload ? uint64(4) : uint64(1));
                TestEqual(TEXT("All room entries fit"), Evictions, uint64(0));
                if (bReload) TestEqual(TEXT("All hits use existing storage"), Stats->RetainedBytes, Room->Before.RetainedBytes);
            }
            else if (BudgetKiB)
                TestTrue(TEXT("Pressure evicts and rebuilds, while admitting each room script"),
                    Evictions > 0 && Builds > 0 && Stats->Oversized == Room->Before.Oversized && Stats->RetainedBytes > 0);
            else
                TestTrue(TEXT("Zero budget compiles all four and retains nothing"), Builds == 4 && !Stats->RetainedBytes && !Stats->Entries);
            AddInfo(FString::Printf(TEXT("AngelIO runtime_cache phase=%s budget_bytes=%llu hits=%llu builds=%llu evictions=%llu oversized=%llu entries=%d retained_bytes=%llu"),
                bReload ? TEXT("reload") : TEXT("first"), Stats->BudgetBytes, Hits, Builds, Evictions,
                Stats->Oversized - Room->Before.Oversized, Stats->Entries, Stats->RetainedBytes));
            Room->Before = *Stats;
            return true;
        });
        Steps(TEXT("client receives both lift floors and both panel waits"), [this, Room, bReload, Inspector]
        {
            auto* Client = Room->Client->GetWorld();
            if (!AtFloor(Client, 2, 2, bReload ? 0 : 1) || !AtFloor(Client, 3, 3, bReload ? 0 : 1)) return false;
            auto* Blink = DeliveryPanel(Room->Client.Get(), 2);
            auto* Delivery = DeliveryPanel(Room->Client.Get(), 3);
            if (!Blink || !Delivery || Blink->GetWaitingSignal() != TEXT("Dispatch") || Delivery->GetWaitingSignal() != TEXT("Dispatch") ||
                !TripOutput(Blink, TEXT("Pulses"), 0) || !TripOutput(Delivery, TEXT("Stage"), 0)) return false;
            const auto Site = DeliveryPanel(Room->Host.Get())->InspectMission();
            const FString HostSummary = Inspector->BytecodeCacheSummary();
            const FText HostDetails = Inspector->BytecodeCacheDetails();
            Inspector->SelectWorld(Client);
            TestTrue(TEXT("Unresolved replica has no cache details"), Inspector->BytecodeCacheDetails().IsEmpty());
            TestTrue(TEXT("Unresolved client selection clears the host counters"), Inspector->BytecodeCacheSummary().StartsWith(TEXT("Client world")));
            Inspector->SelectMission(Delivery);
            TestEqual(TEXT("Replicated panel identifies the server cache"), Inspector->BytecodeCacheSummary(), TEXT("Server world · ") + HostSummary);
            TestTrue(TEXT("Replicated panel resolves the same cache records"), Inspector->BytecodeCacheDetails().EqualTo(HostDetails));
            Inspector->SelectWorld(Room->Host->GetWorld());
            TestEqual(TEXT("Delivery wait keeps its source section"), Site.Script, FString(TEXT("Examples/RoomDelivery.as")));
            TestEqual(TEXT("Delivery wait keeps its debug line"), Site.Line, 9);
            const FGuid Instance = Site.Instance;
            TestTrue(TEXT("New mission context on each visit"), Instance.IsValid() && Instance != Room->FirstMission);
            if (!bReload)
            {
                Room->FirstMission = Instance;
                for (auto Stream : {Room->Host, Room->Client})
                {
                    Stream->SetShouldBeVisible(false);
                    Stream->SetShouldBeLoaded(false);
                }
            }
            return true;
        });
    }
    EndListenSession();
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Room, Inspector]
    {
        TestFalse(TEXT("World teardown releases the engine that owns the cache"), Room->Engine.IsValid());
        Inspector->Refresh();
        TestTrue(TEXT("PIE shutdown clears the cache display"), Inspector->BytecodeCacheSummary().IsEmpty());
        TestTrue(TEXT("PIE shutdown clears per-script details"), Inspector->BytecodeCacheDetails().IsEmpty());
        return true;
    }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOWaitActorListen, "AngelIO.Network.ListenServer.WaitActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOWaitActorListen::RunTest(const FString&)
{
    if (!TestNull(TEXT("PIE must be idle"), GEditor->PlayWorld.Get())) return false;
    StartListenSession(TEXT("/Engine/Maps/Entry"));
    struct FScenario
    {
        TWeakObjectPtr<AAngelIOPlayerController> Remote, Client;
        TWeakObjectPtr<ULevelStreamingDynamic> ServerRoom, ClientRoom;
        bool SetTimeout(float Seconds) const
        {
            FAngelIOValue Value;
            Value.Type = EAngelIOType::Float;
            Value.Float = Seconds;
            return Remote->TripPanel->SetInput(TEXT("TimeoutSeconds"), Value);
        }
    };
    const auto S = MakeShared<FScenario>();
    const auto Steps = ListenSteps(this);
    Steps(TEXT("connected client requests a lift before its room exists"), [this, S]
    {
        auto* Host = FindPeer(NM_ListenServer);
        auto* Client = FindPeer(NM_Client);
        if (!Host || !Client) return false;
        S->Client = Cast<AAngelIOPlayerController>(Client->GetFirstPlayerController());
        for (auto It = Host->GetPlayerControllerIterator(); It; ++It)
            if (auto* PC = Cast<AAngelIOPlayerController>(It->Get()); PC && !PC->IsLocalController()) S->Remote = PC;
        if (!S->Remote.IsValid() || !S->Client.IsValid() || !S->Client->GetNetConnection()) return false;
        S->Remote->TripPanel->Script = TEXT("Examples/StreamedLift.as");
        if (!TestTrue(TEXT("Start streamed lift mission"), S->Remote->TripPanel->StartScript() && S->SetTimeout(0.15f))) return true;
        S->Client->LiftTrip(4, 800);
        FAngelIOProgram Stopped;
        TestTrue(TEXT("Unbounded wait suspends until its caller stops"), Stopped.Compile(TEXT("Stopped.as"),
            TEXT("void Run() { Lift@ lift = WaitActor(\"NeverLoaded\"); }")) && Stopped.Start({}, Host) && Stopped.IsWaitingOnOperation());
        return true;
    });
    Steps(TEXT("timeout returns null to the script and replicates its output"), [this, S]
    {
        if (!TripOutput(S->Client->TripPanel, TEXT("Completed"), 1)) return false;
        TestTrue(TEXT("Script handled a timed-out lookup"), TripOutput(S->Client->TripPanel, TEXT("LastStatus"), 3));
        TestTrue(TEXT("Set a longer deadline"), S->SetTimeout(10.f));
        S->Client->LiftTrip(6, 800);
        return true;
    });
    Steps(TEXT("load the room while the panel is waiting by name"), [this, S]
    {
        if (S->Client->TripPanel->GetWaitingSignal() != TEXT("WaitActor")) return false;
        const auto Site = S->Remote->TripPanel->InspectMission();
        TestTrue(TEXT("Inspector identifies the waiting mission and operation"),
            Site.Script == TEXT("Examples/StreamedLift.as") && Site.Line > 0 && Site.Operation.IsValid());
        S->ServerRoom = LoadLiftRoom(S->Remote->GetWorld());
        S->ClientRoom = LoadLiftRoom(S->Client->GetWorld());
        TestTrue(TEXT("Room loads on both peers"), S->ServerRoom.IsValid() && S->ClientRoom.IsValid());
        return true;
    });
    Steps(TEXT("WaitActor waits through the startup ride, then MoveTo reaches floor six"), [this, S]
    {
        if (!AtFloor(S->Client->GetWorld(), 2, 6, 2) || !TripOutput(S->Client->TripPanel, TEXT("Completed"), 2)) return false;
        TestTrue(TEXT("Client observes successful first ride"), TripOutput(S->Client->TripPanel, TEXT("LastFloor"), 6) &&
            TripOutput(S->Client->TripPanel, TEXT("LastStatus"), int32(ELiftTripStatus::Arrived)));
        FAngelIOProgram Probe;
        FAngelIOValue Ok;
        TestTrue(TEXT("Ready is inline, ambiguous and missing zero-time polls return null"), Probe.Compile(TEXT("PollActor.as"),
            TEXT("bool out_Ok; void Run() { out_Ok = WaitActor(\"FREIGHTLIFT\") !is null && ")
            TEXT("WaitActor(\"FreightLift\", 0) !is null && WaitActor(\"DemoLifts\", 0) is null && ")
            TEXT("WaitActor(\"Missing\", 0) is null; }")) && Probe.Start({}, S->Remote->GetWorld()) &&
            Probe.State() == EAngelIOState::Finished && Probe.GetOutput(TEXT("Ok"), Ok) && Ok.Boolean);
        FAngelIOProgram Invalid;
        TestTrue(TEXT("Invalid timeout fails even for an already-ready actor"), Invalid.Compile(TEXT("InvalidWaitActor.as"),
            TEXT("void Run() { WaitActor(\"FreightLift\", -1); }")) && !Invalid.Start({}, S->Remote->GetWorld()) &&
            Invalid.Diagnostics().ContainsByPredicate([](const auto& D) { return D.Message.Contains(TEXT("non-negative")); }));
        for (auto Level : {S->ServerRoom, S->ClientRoom})
        {
            Level->SetShouldBeVisible(false);
            Level->SetShouldBeLoaded(false);
        }
        return true;
    });
    Steps(TEXT("request a new ride while the room is fully unloaded"), [S]
    {
        if (S->ServerRoom->GetLoadedLevel() || S->ClientRoom->GetLoadedLevel()) return false;
        S->Client->LiftTrip(4, 800);
        return true;
    });
    Steps(TEXT("reload while waiting without sending a room-ready signal"), [S]
    {
        if (S->Client->TripPanel->GetWaitingSignal() != TEXT("WaitActor")) return false;
        for (auto Level : {S->ServerRoom, S->ClientRoom})
        {
            Level->SetShouldBeLoaded(true);
            Level->SetShouldBeVisible(true);
        }
        return true;
    });
    Steps(TEXT("client sees floor four and the trace explains all three waits"), [this, S]
    {
        if (!TripOutput(S->Client->TripPanel, TEXT("Completed"), 3) || !AtFloor(S->Client->GetWorld(), 2, 4, 1)) return false;
        TestTrue(TEXT("Reloaded actor completed the new ride"), TripOutput(S->Client->TripPanel, TEXT("LastFloor"), 4) &&
            TripOutput(S->Client->TripPanel, TEXT("LastStatus"), int32(ELiftTripStatus::Arrived)));
        TSet<FString> Requests;
        int32 Ready = 0, TimedOut = 0, Rides = 0, Detached = 0;
        S->Remote->GetWorld()->GetSubsystem<UAngelIOTraceSubsystem>()->VisitEvents([&](const FJsonObject& Event, const UObject*)
        {
            const auto Type = Event.GetStringField(TEXT("type"));
            FString Id;
            if (!Event.TryGetStringField(TEXT("operationId"), Id)) return;
            if (Type == TEXT("call"))
            {
                const auto Caller = Event.GetObjectField(TEXT("caller"));
                FString Script;
                if (!Caller->TryGetStringField(TEXT("script"), Script) || Script != TEXT("Examples/StreamedLift.as")) return;
                if (Event.GetStringField(TEXT("operation")) == TEXT("WaitActor"))
                {
                    Requests.Add(Id);
                    TestTrue(TEXT("Lookup trace points to the remote mission"),
                        Caller->GetStringField(TEXT("controller")) == TEXT("remote") && Caller->GetIntegerField(TEXT("line")) > 0);
                }
                else if (Event.GetStringField(TEXT("operation")) == TEXT("Lift.MoveTo")) ++Rides;
            }
            else if (Type == TEXT("complete"))
            {
                FString Reason;
                if (Event.TryGetStringField(TEXT("reason"), Reason) && Reason == TEXT("WaiterDetached")) ++Detached;
                if (!Requests.Contains(Id)) return;
                const auto Status = Event.GetStringField(TEXT("status"));
                if (Status == TEXT("Ready")) ++Ready;
                if (Status == TEXT("TimedOut")) ++TimedOut;
            }
        });
        TestEqual(TEXT("Distinct lookups"), Requests.Num(), 3);
        TestEqual(TEXT("Two ready lookups"), Ready, 2);
        TestEqual(TEXT("One lookup timed out"), TimedOut, 1);
        TestEqual(TEXT("Only ready lookups called MoveTo"), Rides, 2);
        TestEqual(TEXT("Stopped unbounded wait cleaned up"), Detached, 1);
        return true;
    });
    EndListenSession();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOCheckpointListen, "AngelIO.Network.ListenServer.MissionCheckpoints",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOPartitionListen, "AngelIO.Network.ListenServer.PartitionLanding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOPartitionListen::RunTest(const FString&)
{
    StartListenSession(TEXT("/Game/Tests/PartitionLift"));
    const auto Steps = ListenSteps(this);
    struct FCell
    {
        TArray<TWeakObjectPtr<APawn>> Players;
        TSharedPtr<FLiftTrip> Ride;
        bool bReportedLoad = false;
        void MovePlayers(double X) const
        {
            for (auto Player : Players)
                if (Player.IsValid()) Player->SetActorLocation(FVector(X, 0, 180), false, nullptr, ETeleportType::TeleportPhysics);
        }
    };
    auto Cell = MakeShared<FCell>();
    Steps(TEXT("move the connected players near the distant spatial lift"), [this, Cell]
    {
        auto* Host = FindPeer(NM_ListenServer);
        auto* Client = FindPeer(NM_Client);
        if (!Host || !Client || Host->GetNumPlayerControllers() != 2 || !Client->GetFirstPlayerController() ||
            !Client->GetFirstPlayerController()->GetPawn()) return false;
        for (auto It = Host->GetPlayerControllerIterator(); It; ++It)
            if (!It->IsValid() || !It->Get()->GetPawn()) return false;
        for (auto* World : {Host, Client})
        {
            auto* Partition = World->GetWorldPartition();
            if (!TestTrue(TEXT("Spatial streaming enabled in partitioned PIE world"), Partition && Partition->IsInitialized() && Partition->IsStreamingEnabled())) return true;
            TestNull(TEXT("Distant lift starts unloaded"), FindLift(World, 2));
            for (auto It = World->GetPlayerControllerIterator(); It; ++It)
                if (APawn* Pawn = It->Get()->GetPawn()) Cell->Players.Add(Pawn);
        }
        Cell->MovePlayers(500000);
        return true;
    });
    Steps(TEXT("ride the spatially loaded lift"), [this, Cell]
    {
        auto* Lift = FindLift(FindPeer(NM_ListenServer), 2);
        if (!Cell->bReportedLoad && FindPeer(NM_ListenServer)->GetTimeSeconds() > 4)
        {
            Cell->bReportedLoad = true;
            for (auto* World : {FindPeer(NM_ListenServer), FindPeer(NM_Client)})
            {
                auto* Cab = FindLift(World, 2);
                AddInfo(FString::Printf(TEXT("Partition %s player %s, lift %s, wait %s"), *World->GetName(),
                    *World->GetFirstPlayerController()->GetPawn()->GetActorLocation().ToString(), *GetNameSafe(Cab),
                    Cab ? *Cab->Mission->GetWaitingSignal() : TEXT("unloaded")));
            }
        }
        if (!Lift || !Lift->IsReadyForTrip() || !AtFloor(FindPeer(NM_Client), 2, 1, 0)) return false;
        Cell->Ride = Lift->MoveTo(4, 800);
        return true;
    });
    Steps(TEXT("unload the cell after the client sees the committed arrival"), [Cell]
    {
        if (!Cell->Ride->IsComplete() || !AtFloor(FindPeer(NM_Client), 2, 4, 1)) return false;
        Cell->MovePlayers(0);
        return true;
    });
    Steps(TEXT("wait for cell eviction on both peers"), [this, Cell]
    {
        if (FindLift(FindPeer(NM_ListenServer), 2) || FindLift(FindPeer(NM_Client), 2)) return false;
        TestNull(TEXT("Registry releases the evicted cell endpoint"), FindPeer(NM_ListenServer)->GetSubsystem<UAngelIOTargetRegistry>()
            ->FindUniqueActor(TEXT("FreightLift"), AAngelIOExampleLift::StaticClass()));
        Cell->MovePlayers(500000);
        return true;
    });
    Steps(TEXT("cell reload restores floor and position on both peers"), []
    {
        for (auto* World : {FindPeer(NM_ListenServer), FindPeer(NM_Client)})
        {
            if (!AtFloor(World, 2, 4, 0)) return false;
            if (!FMath::IsNearlyEqual(FindLift(World, 2)->GetActorLocation().Z, 700.0, 1.0)) return false;
        }
        return true;
    });
    EndListenSession();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOEndOnCase, "AngelIO.Network.ListenServer.EndOnCase",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOEndOnCase::RunTest(const FString&)
{
    if (!TestNull(TEXT("PIE must be idle"), GEditor->PlayWorld.Get())) return false;
    StartListenSession();
    const auto Steps = ListenSteps(this);
    auto Panel = [](ENetMode Mode) -> UAngelIOScriptComponent*
    {
        const auto* Lift = FindLift(FindPeer(Mode), 2);
        return Lift ? Lift->Address.Get() : nullptr;
    };
    Steps(TEXT("start two pulse counters on the replicated panel"), [this, Panel]
    {
        auto* Host = Panel(NM_ListenServer);
        if (!Host || !Panel(NM_Client)) return false;
        const FString Script = TEXT("EndOnCase-") + FGuid::NewGuid().ToString() + TEXT(".as");
        const FString Path = FPaths::ProjectContentDir() / TEXT("AngelIO") / Script;
        const TCHAR* Source = TEXT(R"(
enum InputSignal { Cancel, cancel, Pulse, Done }
int out_Guarded;
int out_Sibling;
void Guarded() {
    EndOn(InputSignal::Cancel);
    while (true) { WaitFor(InputSignal::Pulse); out_Guarded++; }
}
void Sibling() {
    while (true) { WaitFor(InputSignal::Pulse); out_Sibling++; }
}
void Run() {
    Fork(Guarded);
    Fork(Sibling);
    WaitFor(InputSignal::Done);
}
)");
        if (!TestTrue(TEXT("Write isolated mission fixture"), FFileHelper::SaveStringToFile(Source, *Path))) return true;
        ON_SCOPE_EXIT { IFileManager::Get().Delete(*Path); };
        Host->Script = Script;
        TestTrue(TEXT("Both branches start and receive their first pulse"), Host->StartScript() && Host->SendSignal(TEXT("Pulse"), 0));
        return true;
    });
    Steps(TEXT("lowercase cancel leaves the guarded branch running"), [this, Panel]
    {
        auto* Client = Panel(NM_Client);
        if (!TripOutput(Client, TEXT("Guarded"), 1) || !TripOutput(Client, TEXT("Sibling"), 1)) return false;
        auto* Host = Panel(NM_ListenServer);
        if (!TestNotNull(TEXT("Host panel after first pulse"), Host)) return true;
        TestFalse(TEXT("WaitFor ignores a differently cased pulse"), Host->SendSignal(TEXT("pulse"), 0));
        TestFalse(TEXT("EndOn(Cancel) ignores lowercase cancel"), Host->SendSignal(TEXT("cancel"), 0));
        TestTrue(TEXT("Both branches process the next pulse"), Host->SendSignal(TEXT("Pulse"), 0) &&
            TripOutput(Host, TEXT("Guarded"), 2) && TripOutput(Host, TEXT("Sibling"), 2));
        return true;
    });
    Steps(TEXT("exact Cancel ends only the subscribed branch"), [this, Panel]
    {
        auto* Client = Panel(NM_Client);
        if (!TripOutput(Client, TEXT("Guarded"), 2) || !TripOutput(Client, TEXT("Sibling"), 2)) return false;
        auto* Host = Panel(NM_ListenServer);
        if (!TestNotNull(TEXT("Host panel after lowercase cancel"), Host)) return true;
        TestTrue(TEXT("Exact Cancel is consumed"), Host->SendSignal(TEXT("Cancel"), 0));
        TestFalse(TEXT("Completed branch has no remaining subscription"), Host->SendSignal(TEXT("Cancel"), 0));
        TestTrue(TEXT("Sibling alone processes the final pulse"), Host->SendSignal(TEXT("Pulse"), 0) &&
            TripOutput(Host, TEXT("Guarded"), 2) && TripOutput(Host, TEXT("Sibling"), 3));
        return true;
    });
    Steps(TEXT("client observes the stopped and surviving counters"), [Panel]
    {
        auto* Client = Panel(NM_Client);
        return TripOutput(Client, TEXT("Guarded"), 2) && TripOutput(Client, TEXT("Sibling"), 3) &&
            Client->GetWaitingSignal().Equals(TEXT("Done"), ESearchCase::CaseSensitive);
    });
    EndListenSession();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOBranchListen, "AngelIO.Network.ListenServer.BlinkBranches",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOBranchListen::RunTest(const FString&)
{
    StartListenSession(TEXT("/Engine/Maps/Entry"));
    const auto Steps = ListenSteps(this);
    struct FRoom
    {
        TWeakObjectPtr<ULevelStreamingDynamic> Host, Client;
        FGuid FirstRun;
        int32 FinalPulses = 0;
        double CheckAfter = 0;
    };
    const auto Room = MakeShared<FRoom>();
    Steps(TEXT("load branch panel for both peers"), [Room]
    {
        auto* Host = FindPeer(NM_ListenServer);
        auto* Client = FindPeer(NM_Client);
        if (!Host || !Client || !Client->GetFirstPlayerController()) return false;
        Room->Host = LoadLiftRoom(Host, false);
        Room->Client = LoadLiftRoom(Client, false);
        return true;
    });
    auto StartRoom = [&]
    {
        Steps(TEXT("show panel mission"), [Room]
        {
            auto* Panel = DeliveryPanel(Room->Host.Get());
            if (!Panel || !Room->Client->GetLoadedLevel()) return false;
            Panel->Script = TEXT("Examples/BlinkPanel.as");
            Panel->bStartOnBeginPlay = true;
            Room->Host->SetShouldBeVisible(true);
            Room->Client->SetShouldBeVisible(true);
            return true;
        });
        Steps(TEXT("dispatch the panel"), [this, Room]
        {
            auto* Panel = DeliveryPanel(Room->Client.Get());
            if (!Panel || Panel->GetWaitingSignal() != TEXT("Dispatch")) return false;
            TestTrue(TEXT("New panel starts with zero pulses"), TripOutput(Panel, TEXT("Pulses"), 0));
            return DeliveryPanel(Room->Host.Get())->SendSignal(TEXT("Dispatch"), 4);
        });
        Steps(TEXT("inspect a live blinking branch during the ride"), [this, Room]
        {
            auto* Panel = DeliveryPanel(Room->Host.Get());
            FAngelIOValue Pulses;
            if (!DeliveryPanel(Room->Client.Get())->GetOutput(TEXT("Pulses"), Pulses) || Pulses.Integer < 2) return false;
            auto Info = Panel->InspectMission();
            TestEqual(TEXT("Parent awaits the specific ride"), Info.Waiting, FString(TEXT("LiftTrip.Wait")));
            TestTrue(TEXT("Inspector identifies child, source and timer"), Info.BranchWaits.Num() == 1 &&
                Info.BranchWaits[0] == TEXT("Blink · Examples/BlinkPanel.as:12 · WaitSeconds"));
            if (!Room->FirstRun.IsValid()) Room->FirstRun = Info.Instance;
            else TestNotEqual(TEXT("Reloaded room has a fresh branch owner"), Info.Instance, Room->FirstRun);
            return true;
        });
    };
    StartRoom();
    Steps(TEXT("unload before the first blinking ride finishes"), [Room]
    {
        for (auto* Stream : {Room->Host.Get(), Room->Client.Get()})
        {
            Stream->SetShouldBeVisible(false);
            Stream->SetShouldBeLoaded(false);
        }
        return true;
    });
    Steps(TEXT("wait for full unload and recreate the room"), [Room]
    {
        if (Room->Host->GetLoadedLevel() || Room->Client->GetLoadedLevel()) return false;
        Room->Host->SetShouldBeLoaded(true);
        Room->Client->SetShouldBeLoaded(true);
        return true;
    });
    StartRoom();
    Steps(TEXT("hold the panel until its timer and ride are both ready"), [Room]
    {
        auto* Panel = DeliveryPanel(Room->Host.Get());
        Panel->SetComponentTickEnabled(false);
        Room->CheckAfter = Panel->GetWorld()->GetTimeSeconds() + 0.2;
        return true;
    });
    Steps(TEXT("resume the overdue blink and arrival in one tick"), [Room]
    {
        auto* Panel = DeliveryPanel(Room->Host.Get());
        auto* Lift = FindLift(Panel->GetWorld(), 2);
        if (Panel->GetWorld()->GetTimeSeconds() < Room->CheckAfter || !Lift || !Lift->IsReadyForTrip() ||
            !TripOutput(Lift->Mission.Get(), TEXT("LastFloor"), 4)) return false;
        Panel->SetComponentTickEnabled(true);
        return true;
    });
    Steps(TEXT("arrival ends blinking"), [this, Room]
    {
        auto* Panel = DeliveryPanel(Room->Client.Get());
        if (!Panel || Panel->GetWaitingSignal() != TEXT("Confirm")) return false;
        FAngelIOValue Count, Lamp;
        if (!Panel->GetOutput(TEXT("Pulses"), Count) || !Panel->GetOutput(TEXT("Lamp"), Lamp)) return false;
        TestFalse(TEXT("Client sees extinguished lamp"), Lamp.Boolean);
        TestTrue(TEXT("EndOn removed the child"), DeliveryPanel(Room->Host.Get())->InspectMission().BranchWaits.IsEmpty());
        Room->FinalPulses = Count.Integer;
        Room->CheckAfter = Panel->GetWorld()->GetTimeSeconds() + 0.35;
        return true;
    });
    Steps(TEXT("cancelled timer stays silent"), [this, Room]
    {
        auto* Panel = DeliveryPanel(Room->Client.Get());
        if (Panel->GetWorld()->GetTimeSeconds() < Room->CheckAfter) return false;
        TestTrue(TEXT("No pulses after cancellation"), TripOutput(Panel, TEXT("Pulses"), Room->FinalPulses));
        return true;
    });
    EndListenSession();
    return true;
}

bool FAngelIOCheckpointListen::RunTest(const FString&)
{
    if (!TestNull(TEXT("PIE must be idle"), GEditor->PlayWorld.Get())) return false;
    const auto Inspector = SNew(SAngelIOMissionInspector);
    StartListenSession(TEXT("/Engine/Maps/Entry"));
    struct FRoom
    {
        TWeakObjectPtr<ULevelStreamingDynamic> Host, Client, Late, CompletedRoom;
        TWeakObjectPtr<UAngelIOScriptComponent> SavedPanel;
        FGuid SavedInstance, CheckpointId;

        void Unload() const
        {
            for (auto Stream : {Host, Client})
            {
                Stream->SetShouldBeVisible(false);
                Stream->SetShouldBeLoaded(false);
            }
        }
    };
    const auto Room = MakeShared<FRoom>();
    const auto Steps = ListenSteps(this);
    Steps(TEXT("load a hidden room for the host and a connected client"), [this, Room]
    {
        auto* Host = FindPeer(NM_ListenServer);
        auto* Client = FindPeer(NM_Client);
        if (!Host || !Client || !Client->GetFirstPlayerController() || !Client->GetFirstPlayerController()->GetNetConnection()) return false;
        Room->Host = LoadLiftRoom(Host, false);
        Room->Client = LoadLiftRoom(Client, false);
        TestTrue(TEXT("Created room on both peers"), Room->Host.IsValid() && Room->Client.IsValid());
        return true;
    });
    auto ShowRoom = [&]
    {
        Steps(TEXT("assign the room mission before BeginPlay and show it"), [Room]
        {
            if (!Room->Host->GetLoadedLevel() || !Room->Client->GetLoadedLevel()) return false;
            for (const int32 Floor : {2, 3})
            {
                auto* Panel = DeliveryPanel(Room->Host.Get(), Floor);
                if (!Panel) return false;
                Panel->Script = TEXT("Examples/RoomDelivery.as");
                Panel->bStartOnBeginPlay = true;
                Panel->RoomTargets.Add(TEXT("FreightLift"), Room->CompletedRoom.IsValid() ? TEXT("ServiceLiftB") : TEXT("FreightLift"));
            }
            Room->Host->SetShouldBeVisible(true);
            Room->Client->SetShouldBeVisible(true);
            return true;
        });
    };
    auto ReloadRoom = [&]
    {
        Steps(TEXT("both peers fully unloaded the room before loading it again"), [Room]
        {
            if (Room->Host->GetLoadedLevel() || Room->Client->GetLoadedLevel()) return false;
            // Exercise reconstruction from a fresh UObject, including when the engine would normally defer GC.
            if (Room->SavedPanel.IsValid()) CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
            Room->Host->SetShouldBeLoaded(true);
            Room->Client->SetShouldBeLoaded(true);
            return true;
        });
        ShowRoom();
    };
    ShowRoom();
    Steps(TEXT("a room with no committed delivery accepts dispatch"), [this, Room, Inspector]
    {
        auto* Client = DeliveryPanel(Room->Client.Get());
        if (!Client || Client->GetWaitingSignal() != TEXT("Dispatch") || !TripOutput(Client, TEXT("Stage"), 0)) return false;
        Inspector->SelectWorld(FindPeer(NM_ListenServer));
        Inspector->SelectMission(DeliveryPanel(Room->Host.Get()));
        TestTrue(TEXT("Fresh delivery shows no checkpoint"), Inspector->CheckpointSummary().IsEmpty());
        TestFalse(TEXT("Client cannot advance the authoritative mission"), Client->SendSignal(TEXT("Confirm"), 0));
        TestTrue(TEXT("Dispatch the freight lift"), DeliveryPanel(Room->Host.Get())->SendSignal(TEXT("Dispatch"), 0));
        return true;
    });
    Steps(TEXT("unload before the ride can commit its checkpoint"), [Room]
    {
        if (DeliveryPanel(Room->Host.Get())->GetWaitingSignal() != TEXT("LiftTrip.Wait")) return false;
        Room->Unload();
        return true;
    });
    ReloadRoom();
    Steps(TEXT("successful arrival commits Delivered"), [this, Room, Inspector]
    {
        auto* Client = DeliveryPanel(Room->Client.Get());
        if (!Client || Client->GetWaitingSignal() != TEXT("Confirm") || !TripOutput(Client, TEXT("Stage"), 1)) return false;
        Room->SavedPanel = DeliveryPanel(Room->Host.Get());
        Room->SavedInstance = Room->SavedPanel->InspectMission().Instance;
        Inspector->SelectMission(Room->SavedPanel.Get());
        Room->CheckpointId = Inspector->Inspection.CheckpointId;
        TestTrue(TEXT("First delivery has a checkpoint ID"), Room->CheckpointId.IsValid());
        TestEqual(TEXT("Inspector shows the stage that retried the delivery"), Inspector->CheckpointSummary(),
            FString(TEXT("Saved stage: Delivered (v1)\nRestored this run: Dispatched (v1)")));
        Room->Unload();
        return true;
    });
    ReloadRoom();
    Steps(TEXT("a new VM restores Delivered and waits for confirmation"), [this, Room, Inspector]
    {
        auto* Client = DeliveryPanel(Room->Client.Get());
        if (!Client || Client->GetWaitingSignal() != TEXT("Confirm") || !TripOutput(Client, TEXT("Stage"), 1)) return false;
        auto* Panel = DeliveryPanel(Room->Host.Get());
        TestTrue(TEXT("Restored stage in a fresh VM"), Room->SavedInstance.IsValid() &&
            Panel->InspectMission().Instance != Room->SavedInstance);
        TestTrue(TEXT("Checkpoint survives replacement of the original component"), Room->SavedPanel.Get() != Panel);
        Inspector->SelectWorld(Client->GetWorld());
        Inspector->SelectMission(Client);
        if (!Inspector->AuthorityMission.IsValid()) return false;
        TestTrue(TEXT("Client inspector resolves the replacement server component"), Inspector->AuthorityMission.Get() == Panel);
        TestEqual(TEXT("Reload retains the checkpoint ID"), Inspector->Inspection.CheckpointId, Room->CheckpointId);
        TestEqual(TEXT("Reloaded client selection shows Delivered from the server VM"), Inspector->CheckpointSummary(),
            FString(TEXT("Saved stage: Delivered (v1)\nRestored this run: Delivered (v1)")));
        TestTrue(TEXT("A restore alone does not add an operation to the picker"), Inspector->Operations.IsEmpty());
        TestEqual(TEXT("Second component using the same script has independent progress"),
            DeliveryPanel(Room->Host.Get(), 2)->GetWaitingSignal(), FString(TEXT("Dispatch")));
        TestTrue(TEXT("Restored signal subscription accepts confirmation"), Panel->SendSignal(TEXT("Confirm"), 0));
        Inspector->Refresh();
        TestEqual(TEXT("New save keeps the stage this VM resumed from"), Inspector->CheckpointSummary(),
            FString(TEXT("Saved stage: Complete (v1)\nRestored this run: Delivered (v1)")));
        return true;
    });
    Steps(TEXT("client observes completion before unload"), [Room]
    {
        if (!TripOutput(DeliveryPanel(Room->Client.Get()), TEXT("Stage"), 2)) return false;
        Room->Unload();
        return true;
    });
    ReloadRoom();
    Steps(TEXT("restore terminal progress and verify the delivery history"), [this, Room, Inspector]
    {
        if (!TripOutput(DeliveryPanel(Room->Client.Get()), TEXT("Stage"), 2)) return false;
        TestTrue(TEXT("Completed mission finishes immediately"), DeliveryPanel(Room->Host.Get())->InspectMission().State == EAngelIOState::Finished);
        Inspector->SelectWorld(FindPeer(NM_ListenServer));
        Inspector->SelectMission(DeliveryPanel(Room->Host.Get()));
        TestEqual(TEXT("Completed mission shows the new restore point"), Inspector->CheckpointSummary(),
            FString(TEXT("Saved stage: Complete (v1)\nRestored this run: Complete (v1)")));
        TSet<FString> Deliveries;
        int32 Arrivals = 0, Interruptions = 0;
        FindPeer(NM_ListenServer)->GetSubsystem<UAngelIOTraceSubsystem>()->VisitEvents([&](const FJsonObject& E, const UObject*)
        {
            FString Id;
            if (!E.TryGetStringField(TEXT("operationId"), Id)) return;
            const FString Type = E.GetStringField(TEXT("type"));
            if (Type == TEXT("call") && E.GetStringField(TEXT("operation")) == TEXT("Lift.MoveTo") &&
                E.GetObjectField(TEXT("caller"))->GetStringField(TEXT("script")) == TEXT("Examples/RoomDelivery.as"))
                Deliveries.Add(Id);
            if (Type != TEXT("complete") || !Deliveries.Contains(Id)) return;
            if (E.GetStringField(TEXT("status")) == TEXT("Arrived")) ++Arrivals;
            FString Reason;
            if (E.TryGetStringField(TEXT("reason"), Reason) && Reason == TEXT("TargetUnloaded")) ++Interruptions;
        });
        TestEqual(TEXT("No replay after the committed checkpoint"), Deliveries.Num(), 2);
        TestEqual(TEXT("One interrupted attempt"), Interruptions, 1);
        TestEqual(TEXT("Exactly one successful delivery"), Arrivals, 1);
        GEditor->RequestLateJoin();
        return true;
    });
    Steps(TEXT("load the completed room on a late client"), [this, Room]
    {
        auto* Late = FindPeer(NM_Client, Room->Client->GetWorld());
        if (!Late || !Late->GetFirstPlayerController() || !Late->GetFirstPlayerController()->GetNetConnection()) return false;
        Room->Late = LoadLiftRoom(Late);
        TestTrue(TEXT("Created late client's room"), Room->Late.IsValid());
        return true;
    });
    Steps(TEXT("late client sees the restored final stage before another room instance loads"), [this, Room]
    {
        if (!TripOutput(DeliveryPanel(Room->Late.Get()), TEXT("Stage"), 2)) return false;
        Room->CompletedRoom = Room->Host;
        Room->Host = LoadLiftRoom(FindPeer(NM_ListenServer), false, TEXT("AngelIOSecondDeliveryRoom"));
        Room->Client = LoadLiftRoom(Room->Client->GetWorld(), false, TEXT("AngelIOSecondDeliveryRoom"));
        TestTrue(TEXT("Created a distinct instance of the same map"), Room->Host.IsValid() && Room->Client.IsValid());
        return true;
    });
    ShowRoom();
    Steps(TEXT("identical actor and component names in a second room start fresh"), [this, Room, Inspector]
    {
        auto* Client = DeliveryPanel(Room->Client.Get());
        if (!Client || Client->GetWaitingSignal() != TEXT("Dispatch")) return false;
        TestTrue(TEXT("Room instance isolates progress"), TripOutput(Client, TEXT("Stage"), 0) &&
            TripOutput(DeliveryPanel(Room->CompletedRoom.Get()), TEXT("Stage"), 2));
        Inspector->SelectMission(DeliveryPanel(Room->Host.Get()));
        TestTrue(TEXT("Selecting a different room clears the checkpoint display"), Inspector->CheckpointSummary().IsEmpty());
        FString Path;
        if (TestTrue(TEXT("Export mixed checkpoint and operation history"),
            FindPeer(NM_ListenServer)->GetSubsystem<UAngelIOTraceSubsystem>()->Export(Path)))
            AddInfo(TEXT("Checkpoint trace: ") + Path);
        TestTrue(TEXT("Dispatch the independently bound second room"), DeliveryPanel(Room->Host.Get())->SendSignal(TEXT("Dispatch"), 0));
        return true;
    });
    Steps(TEXT("only the second room's chosen service lift makes the new delivery"), [this, Room]
    {
        const auto* Client = DeliveryPanel(Room->Client.Get());
        if (!Client || Client->GetWaitingSignal() != TEXT("Confirm")) return false;
        for (auto* Stream : {Room->Host.Get(), Room->Client.Get()})
        {
            auto* Service = CastChecked<AAngelIOExampleLift>(DeliveryPanel(Stream)->GetOwner());
            auto* Freight = CastChecked<AAngelIOExampleLift>(DeliveryPanel(Stream, 2)->GetOwner());
            if (!TripOutput(Service->Mission, TEXT("LastFloor"), 4) || !TripOutput(Freight->Mission, TEXT("LastFloor"), 2)) return false;
        }
        auto* OriginalService = CastChecked<AAngelIOExampleLift>(DeliveryPanel(Room->CompletedRoom.Get())->GetOwner());
        TestTrue(TEXT("Same named lift in the first room stayed at floor three"), TripOutput(OriginalService->Mission, TEXT("LastFloor"), 3));
        return true;
    });
    EndListenSession();
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FAngelIOCheckpointRestart, "AngelIO.Network.ListenServer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FAngelIOCheckpointRestart::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    Names.Add(TEXT("CheckpointRestart"));
    Commands.Add(TEXT("Delivered"));
    Names.Add(TEXT("MidRideRestart"));
    Commands.Add(TEXT("Dispatched"));
    Names.Add(TEXT("LandedBeforeDeliveredRestart"));
    Commands.Add(TEXT("LandedBeforeDelivered"));
}

bool FAngelIOCheckpointRestart::RunTest(const FString& InitialStage)
{
    if (!TestNull(TEXT("PIE must be idle"), GEditor->PlayWorld.Get())) return false;
    // The default run uses two PIE sessions. The optional GUID runs each half in a separate OS process.
    FString Token;
    const bool bWriteOnly = FParse::Value(FCommandLine::Get(), TEXT("AngelIORestartTestWrite="), Token);
    const bool bReadOnly = FParse::Value(FCommandLine::Get(), TEXT("AngelIORestartTestRead="), Token);
    FGuid TestId = FGuid::NewGuid();
    if ((bWriteOnly || bReadOnly) && !TestTrue(TEXT("Pass one restart phase with a test GUID"),
        bWriteOnly != bReadOnly && FGuid::Parse(Token, TestId))) return false;
    const bool bMidRide = InitialStage == TEXT("Dispatched");
    const bool bLandedBeforeDelivered = InitialStage == TEXT("LandedBeforeDelivered");
    const TCHAR* SlotPrefix = bLandedBeforeDelivered ? TEXT("LandedTest-") : bMidRide ? TEXT("MidRideTest-") : TEXT("RestartTest-");
    const FString Slot = SlotPrefix + TestId.ToString();
    const FString File = FPaths::ProjectSavedDir() / TEXT("AngelIO/Checkpoints/PIE") / (TEXT("slot-") + Slot + TEXT(".sqlite3"));
    if (!TestEqual(TEXT("Only the read phase starts with an existing test slot"), FPaths::FileExists(File), bReadOnly)) return false;
    const auto Steps = ListenSteps(this);
    struct FSession
    {
        TWeakObjectPtr<ULevelStreamingDynamic> Host, Client;
        FGuid CheckpointId;
        FVector FloorFourLocation;
        TWeakObjectPtr<ULevelStreamingDynamic> LateRoom;
        TStrongObjectPtr<UAngelIOTestSignalObserver> Motion{NewObject<UAngelIOTestSignalObserver>()};
        TStrongObjectPtr<UAngelIOTestSignalObserver> ClientMotion{NewObject<UAngelIOTestSignalObserver>()};
    };
    const auto Session = MakeShared<FSession>();
    auto Start = [&]
    {
        Steps(TEXT("start a fresh listen server"), [Slot]
        {
            StartListenSession(TEXT("/Engine/Maps/Entry"), Slot);
            return true;
        });
        Steps(TEXT("load delivery room on connected peers"), [this, Session, Slot]
        {
            UWorld* Host = FindPeer(NM_ListenServer);
            UWorld* Client = FindPeer(NM_Client);
            if (!Host || !Client || !Client->GetFirstPlayerController() || !Client->GetFirstPlayerController()->GetNetConnection()) return false;
            if (!TestEqual(TEXT("Authority started in the isolated slot"), FString(Host->URL.GetOption(TEXT("AngelIOCheckpointSlot="), TEXT("missing"))), Slot)) return true;
            Session->Host = LoadLiftRoom(Host, false);
            Session->Client = LoadLiftRoom(Client, false);
            TestTrue(TEXT("Both peers created their streamed room"), Session->Host.IsValid() && Session->Client.IsValid());
            return true;
        });
        Steps(TEXT("start the placed delivery script when the room becomes visible"), [Session, bMidRide, bLandedBeforeDelivered]
        {
            auto* Panel = DeliveryPanel(Session->Host.Get());
            auto* FreightAddress = DeliveryPanel(Session->Host.Get(), 2);
            if (!Panel || !FreightAddress || !Session->Client->GetLoadedLevel()) return false;
            auto* Freight = CastChecked<AAngelIOExampleLift>(FreightAddress->GetOwner());
            if (bLandedBeforeDelivered)
            {
                auto* ClientAddress = DeliveryPanel(Session->Client.Get(), 2);
                if (!ClientAddress) return false;
                Session->Motion->GameplayEvents = 0;
                Session->ClientMotion->Presentations = 0;
                Observe(Freight->Mission, Session->Motion.Get());
                Observe(CastChecked<AAngelIOExampleLift>(ClientAddress->GetOwner())->Mission, Session->ClientMotion.Get());
            }
            if (bMidRide)
            {
                // Leave enough network frames to observe both peers strictly between landings.
                Freight->GetWorld()->GetWorldSettings()->SetTimeDilation(0.35f);
                Session->Motion->Cab = Freight;
                Session->Motion->DepartureForFloorFour.Reset();
                Session->Motion->GameplayEvents = 0;
                Freight->Mission->OnSignal.AddDynamic(Session->Motion.Get(), &UAngelIOTestSignalObserver::OnGameplay);
            }
            FAngelIOValue StartFloor;
            StartFloor.Integer = 3;
            Freight->Mission->InitialInputs.Add(TEXT("StartingFloor"), StartFloor);
            // The root's serialized placement is available before its world transform is registered.
            const FVector Placement = Freight->GetRootComponent()->GetRelativeLocation();
            Session->FloorFourLocation = Session->Host->LevelTransform.TransformPosition(Placement) + FVector(0, 0, 200);
            Panel->Script = TEXT("Examples/RoomDelivery.as");
            Panel->bStartOnBeginPlay = true;
            Session->Host->SetShouldBeVisible(true);
            Session->Client->SetShouldBeVisible(true);
            return true;
        });
    };
    auto BothAtFloorFour = [this, Session](int32 Trips)
    {
        for (ENetMode Peer : {NM_ListenServer, NM_Client})
        {
            auto* World = FindPeer(Peer);
            if (!AtFloor(World, 2, 4, Trips)) return false;
            const FVector Cab = FindLift(World, 2)->GetActorLocation();
            if (Peer == NM_ListenServer)
                TestTrue(*FString::Printf(TEXT("Host cab at %s matches floor four at %s"), *Cab.ToString(),
                    *Session->FloorFourLocation.ToString()), Cab.Equals(Session->FloorFourLocation, 1.0));
            else if (!Cab.Equals(Session->FloorFourLocation, 1.0)) return false;
        }
        return true;
    };
    const auto Inspector = SNew(SAngelIOMissionInspector);
    auto CheckLanding = [this, Inspector](bool bRestored)
    {
        auto* Host = FindPeer(NM_ListenServer);
        const auto* Freight = FindLift(Host, 2);
        FString Id;
        for (UWorld* World : {Host, FindPeer(NM_Client)})
        {
            Inspector->SelectWorld(World);
            Inspector->SelectMission(FindLift(World, 2)->Mission);
            if (!TestTrue(TEXT("Selected freight mission resolves to the server"), Inspector->AuthorityMission.Get() == Freight->Mission) ||
                !TestEqual(TEXT("One landing record for the selected lift"), Inspector->Inspection.PersistedIntegers.Num(), 1)) return;
            Id = Inspector->Inspection.PersistedIntegers.CreateConstIterator().Key().ToString(EGuidFormats::DigitsWithHyphensLower);
            TestEqual(TEXT("Inspector shows committed and restored floors"), Inspector->PersistedStateSummary(),
                FString::Printf(TEXT("Saved floor: 4\nRestored floor: %s\nState ID: %s"), bRestored ? TEXT("4") : TEXT("None this activation"), *Id));
            Inspector->SelectMission(FindLift(World, 3)->Address);
            TestTrue(TEXT("Selecting the delivery panel clears the lift state"), Inspector->PersistedStateSummary().IsEmpty());
        }
        TArray<FString> Changes;
        Host->GetSubsystem<UAngelIOTraceSubsystem>()->VisitEvents([&](const FJsonObject& Event, const UObject* Subject)
        {
            FString StateId;
            if (!Event.TryGetStringField(TEXT("stateId"), StateId) || StateId != Id) return;
            TestTrue(TEXT("Landing belongs to the freight executor"), Subject == Freight->Mission &&
                Event.GetStringField(TEXT("target")) == TEXT("FreightLift") && Event.GetStringField(TEXT("field")) == TEXT("floor"));
            Changes.Add(FString::Printf(TEXT("%s %d"), *Event.GetStringField(TEXT("type")), Event.GetIntegerField(TEXT("value"))));
        });
        TestEqual(TEXT("Only committed landings and applied restores enter the journal"), FString::Join(Changes, TEXT(", ")),
            FString(bRestored ? TEXT("stateRestored 4") : TEXT("stateSaved 2, stateSaved 4")));
    };
    auto CheckHistory = [this](int32 ExpectedTrips, const TCHAR* ExpectedEvent, const TCHAR* Stage = TEXT("Delivered"))
    {
        const auto* Trace = FindPeer(NM_ListenServer)->GetSubsystem<UAngelIOTraceSubsystem>();
        int32 Trips = 0, Checkpoints = 0;
        Trace->VisitEvents([&](const FJsonObject& Event, const UObject*)
        {
            const TSharedPtr<FJsonObject>* Caller;
            if (!Event.TryGetObjectField(TEXT("caller"), Caller)) return;
            FString Script;
            if (!(*Caller)->TryGetStringField(TEXT("script"), Script) || Script != TEXT("Examples/RoomDelivery.as")) return;
            FString Operation;
            if (Event.TryGetStringField(TEXT("operation"), Operation) && Operation == TEXT("Lift.MoveTo")) ++Trips;
            if (Event.GetStringField(TEXT("type")) == ExpectedEvent && Event.GetStringField(TEXT("stage")) == Stage) ++Checkpoints;
        });
        TestEqual(TEXT("Delivery MoveTo calls in this server session"), Trips, ExpectedTrips);
        TestEqual(*FString::Printf(TEXT("One %s event for %s"), ExpectedEvent, Stage), Checkpoints, 1);
    };
    if (!bReadOnly)
    {
        Start();
        Steps(TEXT("dispatch exactly one delivery"), [this, Session, bLandedBeforeDelivered]
        {
            auto* Client = DeliveryPanel(Session->Client.Get());
            if (!Client || Client->GetWaitingSignal() != TEXT("Dispatch")) return false;
            if (!AtFloor(FindPeer(NM_Client), 2, 2, 1) || !AtFloor(FindPeer(NM_Client), 3, 3, 1)) return false;
            auto* Panel = DeliveryPanel(Session->Host.Get());
            TestTrue(TEXT("Dispatch accepted"), Panel->SendSignal(TEXT("Dispatch"), 0));
            if (bLandedBeforeDelivered)
            {
                TestEqual(TEXT("Writer is awaiting the dispatched ride"), Panel->GetWaitingSignal(), FString(TEXT("LiftTrip.Wait")));
                // Let the executor land and replicate while the caller cannot consume the completed trip.
                Panel->SetComponentTickEnabled(false);
            }
            return true;
        });
        if (bMidRide)
        {
            Steps(TEXT("shut down with both cabs between floor two and floor four"), [this, Session, CheckHistory]
            {
                auto* Host = FindPeer(NM_ListenServer);
                auto* Panel = DeliveryPanel(Session->Host.Get());
                if (Panel->GetWaitingSignal() != TEXT("LiftTrip.Wait")) return false;
                const double StartZ = Session->FloorFourLocation.Z - 400;
                for (auto* World : {Host, FindPeer(NM_Client)})
                {
                    auto* Lift = FindLift(World, 2);
                    if (!Lift || !TripOutput(Lift->Mission, TEXT("LastFloor"), 2)) return false;
                    const double Z = Lift->GetActorLocation().Z;
                    const double Progress = (Z - StartZ) / (Session->FloorFourLocation.Z - StartZ);
                    if (Progress < 0.45 || Progress > 0.70) return false;
                }
                TestEqual(TEXT("No arrival before shutdown"), Session->Motion->GameplayEvents, 0);
                const auto Info = Panel->InspectMission();
                TestEqual(TEXT("Accepted dispatch already on disk"), Info.SavedStage, FString(TEXT("Dispatched")));
                CheckHistory(1, TEXT("checkpointSaved"), TEXT("Dispatched"));
                AddInfo(FString::Printf(TEXT("Stopping ride %s between floors: host Z=%.1f, client Z=%.1f, last landing Z=%.1f"),
                    *Info.Operation.ToString(), FindLift(Host, 2)->GetActorLocation().Z,
                    FindLift(FindPeer(NM_Client), 2)->GetActorLocation().Z, StartZ));
                GEditor->RequestEndPlayMap();
                return true;
            });
        }
        else if (bLandedBeforeDelivered)
        {
            Steps(TEXT("landing is durable while the delivery checkpoint is still Dispatched"), [this, Session, BothAtFloorFour, CheckHistory, CheckLanding]
            {
                if (!BothAtFloorFour(2) || !Session->ClientMotion->Presentations) return false;
                auto* Panel = DeliveryPanel(Session->Host.Get());
                TestEqual(TEXT("Only Dispatched reached the mission store"), Panel->InspectMission().SavedStage, FString(TEXT("Dispatched")));
                TestEqual(TEXT("Caller has not consumed the completed operation"), Panel->GetWaitingSignal(), FString(TEXT("LiftTrip.Wait")));
                TestEqual(TEXT("One original arrival on the server"), Session->Motion->GameplayEvents, 1);
                TestEqual(TEXT("One original arrival on the client"), Session->ClientMotion->Presentations, 1);
                CheckHistory(1, TEXT("checkpointSaved"), TEXT("Dispatched"));
                CheckLanding(false);
                return true;
            });
        }
        else Steps(TEXT("completed ride saved before shutting down the host"), [this, Session, File, CheckHistory, CheckLanding, BothAtFloorFour]
        {
            auto* Client = DeliveryPanel(Session->Client.Get());
            if (!Client || Client->GetWaitingSignal() != TEXT("Confirm") || !TripOutput(Client, TEXT("Stage"), 1) ||
                !BothAtFloorFour(2) || !AtFloor(FindPeer(NM_Client), 3, 3, 1)) return false;
            Session->CheckpointId = DeliveryPanel(Session->Host.Get())->InspectMission().CheckpointId;
            TestTrue(TEXT("Slot exists before teardown"), FPaths::FileExists(File) && Session->CheckpointId.IsValid());
            CheckHistory(1, TEXT("checkpointSaved"));
            CheckLanding(false);
            return true;
        });
        EndListenSession();
    }
    if (!bWriteOnly)
    {
        Start();
        if (bMidRide)
        {
            Steps(TEXT("mission retries from the saved landing without another dispatch"), [this, Session, CheckHistory, BothAtFloorFour]
            {
                auto* Host = FindPeer(NM_ListenServer);
                auto* Client = DeliveryPanel(Session->Client.Get());
                if (!Client || Client->GetWaitingSignal() != TEXT("Confirm") ||
                    !TripOutput(Client, TEXT("Stage"), 1) || !BothAtFloorFour(1)) return false;
                const FVector FloorTwo = Session->FloorFourLocation - FVector(0, 0, 400);
                if (!TestTrue(TEXT("Retry departed from the physical second floor"), Session->Motion->DepartureForFloorFour.IsSet() &&
                    Session->Motion->DepartureForFloorFour->Equals(FloorTwo, 1.0))) return true;
                TestEqual(TEXT("Exactly one arrival from the retried ride"), Session->Motion->GameplayEvents, 1);
                const auto Info = DeliveryPanel(Session->Host.Get())->InspectMission();
                TestEqual(TEXT("Mission resumed the accepted dispatch"), Info.RestoredStage, FString(TEXT("Dispatched")));
                TestEqual(TEXT("Successful retry durably advanced the mission"), Info.SavedStage, FString(TEXT("Delivered")));
                CheckHistory(1, TEXT("checkpointRestored"), TEXT("Dispatched"));
                CheckHistory(1, TEXT("checkpointSaved"), TEXT("Delivered"));
                AddInfo(FString::Printf(TEXT("Automatic retry: departure Z=%.1f, floor 4 host %s, client %s, one arrival"),
                    Session->Motion->DepartureForFloorFour->Z,
                    *FindLift(Host, 2)->GetActorLocation().ToString(), *FindLift(FindPeer(NM_Client), 2)->GetActorLocation().ToString()));
                return true;
            });
        }
        else if (bLandedBeforeDelivered)
        {
            Steps(TEXT("recover the committed landing and wait for Confirm on both peers"), [this, Session, BothAtFloorFour, CheckHistory, CheckLanding]
            {
                auto* Panel = DeliveryPanel(Session->Host.Get());
                auto* Client = DeliveryPanel(Session->Client.Get());
                if (!Panel || !Client || Client->GetWaitingSignal() != TEXT("Confirm") ||
                    !TripOutput(Client, TEXT("Stage"), 1) || !BothAtFloorFour(0)) return false;
                TestEqual(TEXT("Server waits for confirmation"), Panel->GetWaitingSignal(), FString(TEXT("Confirm")));
                const auto Info = Panel->InspectMission();
                TestEqual(TEXT("Recovered an outstanding dispatch"), Info.RestoredStage, FString(TEXT("Dispatched")));
                TestEqual(TEXT("Recovered arrival advances the on-disk stage"), Info.SavedStage, FString(TEXT("Delivered")));
                CheckHistory(0, TEXT("checkpointRestored"), TEXT("Dispatched"));
                CheckHistory(0, TEXT("checkpointSaved"), TEXT("Delivered"));
                CheckLanding(true);
                auto* Trace = FindPeer(NM_ListenServer)->GetSubsystem<UAngelIOTraceSubsystem>();
                TestEqual(TEXT("Recovery trace has no gaps"), Trace->GetDroppedEventCount(), int64(0));
                Trace->VisitEvents([this](const FJsonObject& Event, const UObject*)
                {
                    FString Operation, Status;
                    TestFalse(TEXT("No MoveTo in the new server trace"), Event.TryGetStringField(TEXT("operation"), Operation) && Operation == TEXT("Lift.MoveTo"));
                    TestFalse(TEXT("No Arrived in the new server trace"), Event.TryGetStringField(TEXT("status"), Status) && Status == TEXT("Arrived"));
                });
                TestEqual(TEXT("No repeat gameplay arrival"), Session->Motion->GameplayEvents, 0);
                TestEqual(TEXT("No repeat client arrival presentation"), Session->ClientMotion->Presentations, 0);
                AddInfo(FString::Printf(TEXT("Recovered Dispatched -> Delivered, waiting for Confirm; client floor 4 at %s, zero trips and zero arrivals"),
                    *FindLift(FindPeer(NM_Client), 2)->GetActorLocation().ToString()));
                GEditor->RequestLateJoin();
                return true;
            });
            Steps(TEXT("late client loads the recovered room"), [Session]
            {
                auto* Late = FindPeer(NM_Client, Session->Client->GetWorld());
                if (!Late || !Late->GetFirstPlayerController() || !Late->GetFirstPlayerController()->GetNetConnection()) return false;
                Session->LateRoom = LoadLiftRoom(Late);
                return Session->LateRoom.IsValid();
            });
            Steps(TEXT("late client sees the recovered cab and pending confirmation"), [Session]
            {
                auto* Panel = DeliveryPanel(Session->LateRoom.Get());
                if (!Panel || Panel->GetWaitingSignal() != TEXT("Confirm") || !TripOutput(Panel, TEXT("Stage"), 1)) return false;
                auto* Late = Panel->GetWorld();
                return AtFloor(Late, 2, 4, 0) && FindLift(Late, 2)->GetActorLocation().Equals(Session->FloorFourLocation, 1.0);
            });
        }
        else
        {
            Steps(TEXT("new server restores Delivered and accepts confirmation"), [this, Session]
            {
                auto* Client = DeliveryPanel(Session->Client.Get());
                if (!Client || Client->GetWaitingSignal() != TEXT("Confirm") || !TripOutput(Client, TEXT("Stage"), 1)) return false;
                auto* Panel = DeliveryPanel(Session->Host.Get());
                const auto Info = Panel->InspectMission();
                TestEqual(TEXT("Restored stage on replacement server"), Info.RestoredStage, FString(TEXT("Delivered")));
                if (Session->CheckpointId.IsValid()) TestEqual(TEXT("Checkpoint keeps its identity on disk"), Info.CheckpointId, Session->CheckpointId);
                TestTrue(TEXT("Recreated mission can finish"), Panel->SendSignal(TEXT("Confirm"), 0));
                return true;
            });
            Steps(TEXT("both peers see the restored fourth-floor cab without a startup or delivery ride"), [this, Session, CheckHistory, CheckLanding, BothAtFloorFour]
            {
                if (!TripOutput(DeliveryPanel(Session->Client.Get()), TEXT("Stage"), 2) || !BothAtFloorFour(0) ||
                    !AtFloor(FindPeer(NM_Client), 3, 3, 0)) return false;
                AddInfo(FString::Printf(TEXT("Restored floor 4: host %s, client %s, zero completed trips on both peers"),
                    *FindLift(FindPeer(NM_ListenServer), 2)->GetActorLocation().ToString(),
                    *FindLift(FindPeer(NM_Client), 2)->GetActorLocation().ToString()));
                CheckHistory(0, TEXT("checkpointRestored"));
                CheckLanding(true);
                return true;
            });
        }
        EndListenSession();
        ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, File]
        {
            if (!HasAnyErrors()) TestTrue(TEXT("Remove isolated test slot"), IFileManager::Get().Delete(*File, true));
            return true;
        }));
    }
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
            const FString Type = Event->GetStringField(TEXT("type"));
            TestEqual(TEXT("Ordered sequence"), Event->GetIntegerField(TEXT("sequence")), ++ExpectedSequence);
            FString Id;
            if (!Event->TryGetStringField(TEXT("operationId"), Id)) continue;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOSharedInspectorPIE, "AngelIO.Network.ListenServer.SharedRegistrationInspector",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOSharedInspectorPIE::RunTest(const FString&)
{
    if (!TestNull(TEXT("PIE must be idle"), GEditor->PlayWorld.Get())) return false;
    struct FPanels
    {
        TWeakObjectPtr<UAngelIOScriptComponent> Local, Remote, Replica;
        TWeakPtr<FAngelIORegisteredEngine> Engine;
    };
    const auto Panels = MakeShared<FPanels>();
    const auto Inspector = SNew(SAngelIOMissionInspector);
    auto Step = ListenSteps(this);
    StartListenSession();
    Step(TEXT("restart both authority panels on one registered engine"), [this, Panels]
    {
        auto* Host = FindPeer(NM_ListenServer);
        auto* Client = FindPeer(NM_Client);
        if (!AtFloor(Host, 2, 2, 1) || !AtFloor(Client, 2, 2, 1)) return false;
        auto* PC = Cast<AAngelIOPlayerController>(Client->GetFirstPlayerController());
        if (!PC || PC->TripPanel->GetWaitingSignal() != TEXT("CallLift")) return false;
        for (auto It = Host->GetPlayerControllerIterator(); It; ++It)
            if (auto* Player = Cast<AAngelIOPlayerController>(It->Get()))
                (Player->IsLocalController() ? Panels->Local : Panels->Remote) = Player->TripPanel.Get();
        if (!Panels->Local.IsValid() || !Panels->Remote.IsValid()) return false;
        Panels->Replica = PC->TripPanel.Get();
        {
            FAngelIOStartupProbe Probe(1);
            TGuardValue<FAngelIOStartupProbe*> Capture(FAngelIOStartupProbe::Current(), &Probe);
            {
                FAngelIOProgram Seed;
                if (!TestTrue(TEXT("Build the panels' script once"), Seed.CompileFile(Panels->Local->Script, Host))) return true;
            }
            if (!TestTrue(TEXT("Both panels started"), Panels->Local->StartScript() && Panels->Remote->StartScript()) ||
                !TestEqual(TEXT("Restarted panels reuse the world's registered API"), Probe.Registrations, 0) ||
                !TestEqual(TEXT("One source build"), Probe.Builds, 1) ||
                !TestEqual(TEXT("Both inspected panels loaded bytecode"), Probe.Loads, 2)) return true;
            Panels->Engine = FAngelIOStartupProbe::WorldEngine(Host);
        }
        PC->LiftTrip(4, 200);
        return true;
    });
    Step(TEXT("inspect simultaneous signal and trip waits"), [this, Inspector, Panels]
    {
        if (Panels->Replica->GetWaitingSignal() != TEXT("LiftTrip.Wait")) return false;
        Inspector->SelectWorld(FindPeer(NM_Client));
        Inspector->SelectMission(Panels->Replica);
        TestEqual(TEXT("Client selection shows the authoritative trip wait"), Inspector->Location, FString(TEXT("TypedLift.as:22")));
        TestTrue(TEXT("Remote panel waits for a particular ride"), Inspector->Inspection.Operation.IsValid());
        Inspector->SelectWorld(FindPeer(NM_ListenServer));
        Inspector->SelectMission(Panels->Local);
        TestEqual(TEXT("Local panel is still at the input wait"), Inspector->Location, FString(TEXT("TypedLift.as:11")));
        TestTrue(TEXT("Local panel handles an independent rejected call"), Panels->Local->SendSignal(TEXT("CallLift"), 7) &&
            TripOutput(Panels->Local.Get(), TEXT("Completed"), 1));
        TestTrue(TEXT("Remote panel's completion counter is still zero"), TripOutput(Panels->Remote.Get(), TEXT("Completed"), 0));
        Panels->Local->StopScript();
        TestTrue(TEXT("Outstanding native wait retains the engine"), Panels->Engine.IsValid());
        return true;
    });
    Step(TEXT("surviving module finishes its ride and replicates its floor"), [this, Inspector, Panels]
    {
        if (!TripOutput(Panels->Replica.Get(), TEXT("Completed"), 1) ||
            !TripOutput(Panels->Replica.Get(), TEXT("LastFloor"), 4)) return false;
        Inspector->SelectWorld(FindPeer(NM_Client));
        Inspector->SelectMission(Panels->Replica);
        TestEqual(TEXT("Surviving panel resumes at the correct line"), Inspector->Location, FString(TEXT("TypedLift.as:11")));
        TestEqual(TEXT("Surviving panel is ready for input"), Inspector->Inspection.Waiting, FString(TEXT("CallLift")));
        Panels->Remote->StopScript();
        TestTrue(TEXT("World retains the registered engine after both panels stop"), Panels->Engine.IsValid());
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
