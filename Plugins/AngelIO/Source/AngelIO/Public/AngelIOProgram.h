#pragma once

#include "CoreMinimal.h"
#include "AngelIOProgram.generated.h"

class UWorld;
class UActorComponent;
class UAngelIOScriptWorldSubsystem;

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
#if WITH_EDITOR
    FString DefaultExpression;
#endif
};

struct FAngelIOEvent
{
    FString Name;
    int32 Value;
    FName Target = NAME_None;
    bool bNotifySelf = false;
};

enum class EAngelIOState { Empty, Compiled, Running, Waiting, Finished, Failed };

#if WITH_EDITOR
struct FAngelIOLiteralTarget
{
    FString Function;
    FString Name;
    UClass* OwnerClass;
    bool bRequireSingleActor;
    int32 Line;
    bool bAllowUnloadedGlobal = false;
};

struct FAngelIOInspectedInteger
{
    FString Field;
    int32 Saved = 0;
    TOptional<int32> Restored;
};

struct FAngelIOMissionInspection
{
    EAngelIOState State = EAngelIOState::Empty;
    FString Script;
    int32 Line = 0;
    FString Waiting;
    FGuid Instance;
    FGuid Operation;
    FGuid CheckpointId;
    FString SavedStage, RestoredStage;
    FString SourceRevision;
    int32 SavedVersion = 0, RestoredVersion = 0;
    TMap<FGuid, FAngelIOInspectedInteger> PersistedIntegers;
    TArray<FString> BranchWaits;
};
#endif

// Game-thread instance. Compile inspects declarations without running script code.
class ANGELIO_API FAngelIOProgram
{
public:
    FAngelIOProgram();
    ~FAngelIOProgram();
    FAngelIOProgram(const FAngelIOProgram&) = delete;
    FAngelIOProgram& operator=(const FAngelIOProgram&) = delete;

    // Supply the authority world to share its registered engine. Worldless compilation is isolated for tools.
    bool CompileFile(const FString& RelativeScript, UWorld* World = nullptr);
    bool Compile(const FString& Label, const FString& Source, UWorld* World = nullptr);
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
    bool ResolveRunningSource(FString& Path, int32& Line) const;
    const TArray<FAngelIOLiteralTarget>& LiteralTargets() const;
#endif

private:
    friend class UAngelIOScriptComponent;
    friend class UAngelIOScriptWorldSubsystem;
    void DetachComponent();
    void UnbindWorld();
    void EndWorld();
    UAngelIOScriptWorldSubsystem* WorldOwner = nullptr; // Unlinked on either owner's destruction.
    struct FImpl;
    TSharedPtr<FImpl> Impl;
};
