#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "AngelIOTraceSubsystem.generated.h"

class FJsonObject;

// One bounded operation history per authoritative game world.
UCLASS()
class ANGELIO_API UAngelIOTraceSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Deinitialize() override;
    bool Export(FString& OutFile) const;

private:
    friend class FAngelIOOperation;
    TSharedRef<FJsonObject> Append(const FGuid& OperationId, const TCHAR* EventType);
    static constexpr int32 Capacity = 4096;
    TArray<TSharedPtr<FJsonObject>> Events;
    FGuid SessionId;
    FString NetMode;
    int64 Total = 0;
};
