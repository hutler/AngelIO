#include "AngelIOLiftTrip.h"
#include "Dom/JsonObject.h"

namespace
{
    TSharedRef<FJsonObject> TripArguments(int32 Floor, float Speed)
    {
        auto Args = MakeShared<FJsonObject>();
        Args->SetNumberField(TEXT("floor"), Floor);
        if (FMath::IsFinite(Speed)) Args->SetNumberField(TEXT("speed"), Speed);
        else Args->SetField(TEXT("speed"), MakeShared<FJsonValueNull>());
        return Args;
    }
}

FLiftTrip::FLiftTrip(UWorld* World, const FString& Target, int32 InFloor, float Speed) :
    FAngelIOOperation(World, TEXT("Lift.MoveTo"), Target, TripArguments(InFloor, Speed)), Floor(InFloor)
{}

void FLiftTrip::Finish(ELiftTripStatus InStatus, const FString& Reason)
{
    if (IsComplete()) return;
    Status = InStatus;
    const TCHAR* Name = Status == ELiftTripStatus::Arrived ? TEXT("Arrived") :
        Status == ELiftTripStatus::Rejected ? TEXT("Rejected") : TEXT("Cancelled");
    MarkComplete(Name, Reason);
}
