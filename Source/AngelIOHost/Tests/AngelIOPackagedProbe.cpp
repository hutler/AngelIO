// Opt-in driver for Tools/TestPackagedDelivery.py in the demo game.
#include "../AngelIOPlayerController.h"
#include "../AngelIOExampleLift.h"
#include "../AngelIORoomTerminal.h"
#include "../AngelIORoomDoor.h"
#include "AngelIOScriptComponent.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Pawn.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
struct FDeliveryProbe
{
    bool bHost = false;
    bool bRestore = false;
    bool bStartedTravel = false;
    int32 PeerPort = 0;
    int32 Room = 0;
    double LastUse = -1;
    FString LastObservation;

    void Report(const FString& Text)
    {
        if (LastObservation == Text) return;
        const FString File = FPaths::ProjectSavedDir() / TEXT("AngelIODeliveryProbe.txt");
        if (FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
            LastObservation = Text;
    }

    void Tick(UWorld* World, ELevelTick, float)
    {
        if (!World->IsGameWorld() || Room >= 2) return;
        // Shipping uses the configured game map at startup. Enter the demo through normal travel on both builds.
        if (!bStartedTravel && World->GetNetMode() == NM_Standalone && World->HasBegunPlay())
        {
            auto* Local = World->GetFirstPlayerController();
            if (!Local) return;
            bStartedTravel = true;
            if (bHost)
                UGameplayStatics::OpenLevel(World, TEXT("/Game/Demo/TwoDeliveryRooms"), true, TEXT("listen?AngelIOCheckpointSlot=Packaging"));
            else
                Local->ClientTravel(FString::Printf(TEXT("127.0.0.1:%d"), PeerPort), TRAVEL_Absolute);
            return;
        }
        if (bHost || World->GetNetMode() != NM_Client) return;
        auto* Player = Cast<AAngelIORoomPlayerController>(World->GetFirstPlayerController());
        APawn* Pawn = Player ? Player->GetPawn() : nullptr;
        if (!Pawn) { Report(TEXT("Waiting for the room pawn")); return; }
        TArray<AAngelIORoomTerminal*> Terminals;
        for (TActorIterator<AAngelIORoomTerminal> It(World); It; ++It) Terminals.Add(*It);
        if (Terminals.Num() != 2) { Report(FString::Printf(TEXT("Terminals %d"), Terminals.Num())); return; }
        Terminals.Sort([](const auto& A, const auto& B){ return A.GetActorLocation().X < B.GetActorLocation().X; });
        auto* Terminal = Terminals[Room];
        AAngelIOExampleLift* Lift = nullptr;
        AAngelIORoomDoor* Door = nullptr;
        for (TActorIterator<AAngelIOExampleLift> It(World); It; ++It)
            if (It->GetLevel() == Terminal->GetLevel()) Lift = *It;
        for (TActorIterator<AAngelIORoomDoor> It(World); It; ++It)
            if (It->GetLevel() == Terminal->GetLevel()) Door = *It;
        FAngelIOValue Floor, Trips, Credit;
        if (!Door || !Lift || !Lift->Mission->GetOutput(TEXT("LastFloor"), Floor) ||
            !Lift->Mission->GetOutput(TEXT("TotalTrips"), Trips) || !Terminal->Mission->GetOutput(TEXT("Credits"), Credit))
        { Report(TEXT("Waiting for room replicas and public ports")); return; }
        const FString Wait = Terminal->Mission->GetWaitingSignal();
        Report(FString::Printf(TEXT("Room %d wait=%s floor=%d trips=%d credits=%d"), Room, *Wait, Floor.Integer, Trips.Integer, Credit.Integer));
        if (!Terminal->IsInReach(Pawn))
        {
            Pawn->AddMovementInput((Terminal->GetActorLocation() - Pawn->GetActorLocation()).GetSafeNormal2D());
            return;
        }
        const bool AtLanding = Floor.Integer == 4 && Trips.Integer == (bRestore ? 0 : 1) && Door->IsOpen() &&
            FMath::IsNearlyEqual(Lift->GetActorLocation().Z, 610.0, 1.0);
        if (AtLanding && (bRestore ? Credit.Integer == 1 : Wait == TEXT("Confirm")))
        {
            if (++Room == 2) Report(TEXT("PASS"));
            return;
        }
        const bool Use = bRestore ? (AtLanding && Wait == TEXT("Confirm")) : Wait == TEXT("Dispatch");
        if (Use && World->GetTimeSeconds() - LastUse > 0.35)
        {
            Player->UseTerminal();
            LastUse = World->GetTimeSeconds();
        }
    }
};
}

FDelegateHandle StartAngelIOPackagedProbe()
{
    FString Phase;
    if (!FParse::Value(FCommandLine::Get(), TEXT("AngelIODeliveryProbe="), Phase)) return {};
    if (Phase == TEXT("landings"))
        return FWorldDelegates::OnWorldTickStart.AddLambda([](UWorld* World, ELevelTick, float)
        {
            if (World->GetNetMode() != NM_Standalone || !World->IsGameWorld()) return;
            TSet<int32> Floors;
            for (TActorIterator<AAngelIOExampleLift> It(World); It; ++It)
            {
                FAngelIOValue Floor, Trips;
                if (It->Mission->GetOutput(TEXT("LastFloor"), Floor) && It->Mission->GetOutput(TEXT("TotalTrips"), Trips) &&
                    Trips.Integer == 1 && Floor.Integer == It->RequestedFloor) Floors.Add(Floor.Integer);
            }
            if (Floors.Contains(2) && Floors.Contains(3))
                FPlatformMisc::RequestExit(false, TEXT("AngelIO packaged landings complete"));
        });
    if (Phase != TEXT("host") && Phase != TEXT("dispatch") && Phase != TEXT("restore")) return {};
    auto Probe = MakeShared<FDeliveryProbe>();
    Probe->bHost = Phase == TEXT("host");
    Probe->bRestore = Phase == TEXT("restore");
    if (!Probe->bHost && (!FParse::Value(FCommandLine::Get(), TEXT("AngelIODeliveryPeerPort="), Probe->PeerPort) ||
        Probe->PeerPort <= 0 || Probe->PeerPort > 65535)) return {};
    return FWorldDelegates::OnWorldTickStart.AddLambda([Probe](UWorld* World, ELevelTick Type, float Delta)
        { Probe->Tick(World, Type, Delta); });
}
