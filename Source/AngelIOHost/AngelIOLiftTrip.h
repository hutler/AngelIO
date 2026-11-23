#pragma once

#include "AngelIOScriptAPI.h"

enum class ELiftTripStatus : int32 { Arrived, Rejected, Cancelled };

struct FLiftTrip : public FAngelIOOperation
{
    explicit FLiftTrip(int32 InFloor) : Floor(InFloor) {}
    void Finish(ELiftTripStatus InStatus)
    {
        if (IsComplete()) return;
        Status = InStatus;
        MarkComplete();
    }
    const int32 Floor;
    ELiftTripStatus Status = ELiftTripStatus::Rejected;
};
