#pragma once

#include "UObject/Object.h"
#include "AngelIOTestSignalObserver.generated.h"

// Listens through the same reflected delegate interface as a Blueprint.
UCLASS(Transient)
class UAngelIOTestSignalObserver : public UObject
{
    GENERATED_BODY()
public:
    int32 Presentations = 0;
    int32 GameplayEvents = 0;

    UFUNCTION()
    void OnEffect(FString Name, int32 Floor)
    {
        if (Name == TEXT("Arrived") && Floor == 4) ++Presentations;
    }

    UFUNCTION()
    void OnGameplay(FString Name, int32 Floor)
    {
        if (Name == TEXT("Arrived") && Floor == 4) ++GameplayEvents;
    }
};
