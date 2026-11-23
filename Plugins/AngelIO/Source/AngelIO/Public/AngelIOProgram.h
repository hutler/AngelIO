#pragma once

#include "CoreMinimal.h"
#include "AngelIOProgram.generated.h"

class UWorld;
class UActorComponent;

UENUM(BlueprintType)
enum class EAngelIOType : uint8 { Integer, Float, Boolean };

USTRUCT(BlueprintType)
struct ANGELIO_API FAngelIOValue
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AngelIO")
    EAngelIOType Type = EAngelIOType::Integer;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AngelIO", meta=(EditCondition="Type == EAngelIOType::Integer", EditConditionHides))
    int32 Integer = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AngelIO", meta=(EditCondition="Type == EAngelIOType::Float", EditConditionHides))
    float Float = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AngelIO", meta=(EditCondition="Type == EAngelIOType::Boolean", EditConditionHides))
    bool Boolean = false;
};

struct FAngelIODiagnostic
{
    FString Severity;
    FString Message;
    int32 Line = 0;
    int32 Column = 0;
};

struct FAngelIOPort
{
    FString Name;
    EAngelIOType Type;
    bool bInput;
    int32 Index;
};

struct FAngelIOEvent
{
    FString Name;
    int32 Value;
    FName Target = NAME_None;
};

enum class EAngelIOState { Empty, Compiled, Running, Waiting, Finished, Failed };

#if WITH_EDITOR
struct FAngelIOMissionInspection
{
    EAngelIOState State = EAngelIOState::Empty;
    FString Script;
    int32 Line = 0;
    FString Waiting;
    FGuid Instance;
    FGuid Operation;
};
#endif

// Game-thread instance. Compile inspects declarations without running script code.
class ANGELIO_API FAngelIOProgram
{
public:
    FAngelIOProgram();
    ~FAngelIOProgram();

    bool CompileFile(const FString& RelativeScript);
    bool Compile(const FString& Label, const FString& Source);
    bool Start(const TMap<FString, FAngelIOValue>& Inputs, UWorld* World = nullptr, UActorComponent* Component = nullptr);
    bool SetInput(const FString& Name, const FAngelIOValue& Value);
    bool GetOutput(const FString& Name, FAngelIOValue& Value) const;
    bool Signal(const FString& Name, int32 Value);
    bool IsWaitingOnOperation() const;
    bool ResumeCompletedOperation();
    const TArray<FString>& ExternalDeclarations() const;

    const TArray<FAngelIOPort>& Ports() const;
    const TMap<int32, FString>& Signals(bool bIncoming) const;
    const TArray<FAngelIODiagnostic>& Diagnostics() const;
    TArray<FAngelIOEvent> TakeEvents();
    EAngelIOState State() const;
    FString WaitingFor() const;
#if WITH_EDITOR
    FAngelIOMissionInspection InspectMission() const;
#endif

private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};
