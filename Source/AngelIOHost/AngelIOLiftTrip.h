#pragma once

#include "AngelIOScriptAPI.h"

enum class ELiftTripStatus : int32 { Arrived, Rejected, Cancelled };

struct FLiftTrip : public FAngelIOOperation
{
    FLiftTrip(UWorld* World, const FString& Target, int32 InFloor, float Speed);
    void Finish(ELiftTripStatus InStatus, const FString& Reason = {});
    const int32 Floor;
    ELiftTripStatus Status = ELiftTripStatus::Rejected;
};
