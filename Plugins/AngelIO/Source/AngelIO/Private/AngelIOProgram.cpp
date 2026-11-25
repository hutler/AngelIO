#include "AngelIOProgram.h"
#include "AngelIOBytecodeCache.h"
#include "AngelIOCheckpointSubsystem.h"
#include "AngelIOScriptAPI.h"
#include "AngelIOScriptComponent.h"
#include "AngelIOScriptWorldSubsystem.h"
#include "AngelIOTraceSubsystem.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Features/IModularFeatures.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include <string_view>
#include <algorithm>
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Tests/AngelIOStartupProbe.h"
#define ANGELIO_BENCHMARK_MARK(Field) FAngelIOStartupProbe::Mark(&FAngelIOStartupProbe::Field)
#else
#define ANGELIO_BENCHMARK_MARK(Field) do {} while (false)
#endif

THIRD_PARTY_INCLUDES_START
#include "angelscript.h"
#include "scriptstdstring.h"
THIRD_PARTY_INCLUDES_END

namespace
{
    struct FConditionWait final : FAngelIOOperation
    {
        FConditionWait(UWorld* World, const FString& Label, const FString& Target, TFunction<bool()> InReady)
            : FAngelIOOperation(World, Label, Target), IsReady(MoveTemp(InReady)) {}

        void Poll() { if (IsReady()) MarkComplete(TEXT("Ready")); }
        void Timeout() { MarkComplete(TEXT("TimedOut")); }
        void Detach() { MarkComplete(TEXT("Cancelled"), TEXT("WaiterDetached")); }
    private:
        TFunction<bool()> IsReady;
    };

    struct FExecutionScope
    {
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<UActorComponent> Component;
        FGuid InstanceId;
        FGuid CheckpointStepId;
        FGuid BranchId;
        TSharedPtr<FAngelIOOperation> Pending;
        TSharedPtr<FConditionWait> Condition;
        TSharedPtr<EAngelIOWaitOutcome> WaitOutcome;
        TOptional<double> Deadline;
        TSharedPtr<FJsonObject> WaitSite;
        FString Label;
#if WITH_EDITOR
        int32 WaitLine = 0;
#endif
    };
    constexpr asPWORD ExecutionKey = 0x41494F;

    FExecutionScope* GetWaitScope(TOptional<double> TimeoutSeconds)
    {
        auto* Context = asGetActiveContext();
        if (!Context) return nullptr;
        auto* Scope = static_cast<FExecutionScope*>(Context->GetUserData(ExecutionKey));
        if (!Scope || !Scope->World.IsValid())
        {
            Context->SetException("Waiting requires a world-backed mission in Run");
            return nullptr;
        }
        if (TimeoutSeconds.IsSet() && (!FMath::IsFinite(*TimeoutSeconds) || *TimeoutSeconds < 0))
        {
            Context->SetException("Wait timeout must be finite and non-negative");
            return nullptr;
        }
        return Scope;
    }

    struct FMissionFrame
    {
        asUINT Index = 0;
        const char* Section = nullptr;
        int32 Line = 0;

        explicit FMissionFrame(asIScriptContext& Context)
        {
            Line = Context.GetLineNumber(Index, nullptr, &Section);
            while (Section && FCStringAnsi::Strcmp(Section, "AngelIO API") == 0 && Index + 1 < Context.GetCallstackSize())
                Line = Context.GetLineNumber(++Index, nullptr, &Section);
        }
    };

    TSharedRef<FJsonObject> CaptureSite()
    {
        auto Site = MakeShared<FJsonObject>();
        auto* Context = asGetActiveContext();
        auto* Scope = Context ? static_cast<FExecutionScope*>(Context->GetUserData(ExecutionKey)) : nullptr;
        Site->SetStringField(TEXT("kind"), Scope ? TEXT("script") : TEXT("native"));
        if (!Scope) return Site;
        Site->SetStringField(TEXT("instanceId"), Scope->InstanceId.ToString(EGuidFormats::DigitsWithHyphensLower));
        if (Scope->BranchId.IsValid()) Site->SetStringField(TEXT("branchId"), Scope->BranchId.ToString(EGuidFormats::DigitsWithHyphensLower));
        const FMissionFrame Frame(*Context);
        Site->SetNumberField(TEXT("line"), Frame.Line);
        if (Frame.Section) Site->SetStringField(TEXT("script"), UTF8_TO_TCHAR(Frame.Section));
        if (auto* Function = Context->GetFunction(Frame.Index))
            Site->SetStringField(TEXT("function"), UTF8_TO_TCHAR(Function->GetDeclaration()));
        if (auto* Component = Scope->Component.Get())
        {
            Site->SetStringField(TEXT("component"), Component->GetPathName(Scope->World.Get()));
            if (auto* PC = Cast<APlayerController>(Component->GetOwner()))
                Site->SetStringField(TEXT("controller"), PC->IsLocalController() ? TEXT("local") : TEXT("remote"));
        }
        return Site;
    }
}

FAngelIOOperation::FAngelIOOperation(UWorld* World, const FString& Name, const FString& Target,
    TSharedPtr<FJsonObject> Arguments, const UObject* TraceTarget) : OperationId(FGuid::NewGuid())
{
    check(IsInGameThread());
#if WITH_EDITOR
    InspectionTarget = TraceTarget;
#endif
    if (!World) World = AngelIO::GetScriptWorld();
    if (!World || World->GetNetMode() == NM_Client) return;
    Trace = World->GetSubsystem<UAngelIOTraceSubsystem>();
    if (auto Event = AddEvent(TEXT("call")))
    {
        Event->SetStringField(TEXT("operation"), Name);
        Event->SetStringField(TEXT("target"), Target);
        if (Arguments)
        {
            TSharedPtr<FJsonObject> Snapshot = MakeShared<FJsonObject>();
            FJsonObject::Duplicate(Arguments, Snapshot);
            Event->SetObjectField(TEXT("arguments"), Snapshot);
        }
        Event->SetObjectField(TEXT("caller"), CaptureSite());
    }
}

FAngelIOOperation::~FAngelIOOperation()
{
#if WITH_EDITOR
    if (!bComplete)
        if (auto* Journal = Trace.Get()) Journal->ForgottenTargets += Journal->LiveTargets.Remove(OperationId);
#endif
}

TSharedPtr<FJsonObject> FAngelIOOperation::AddEvent(const TCHAR* Type) const
{
    if (auto* Journal = Trace.Get())
    {
        const UObject* Target = nullptr;
#if WITH_EDITOR
        Target = InspectionTarget.Get();
#endif
        return Journal->Append(OperationId, Type, Target);
    }
    return nullptr;
}

void FAngelIOOperation::MarkComplete(const FString& Status, const FString& Reason)
{
    check(IsInGameThread());
    if (bComplete) return;
    bComplete = true;
    if (auto Event = AddEvent(TEXT("complete")))
    {
        Event->SetStringField(TEXT("status"), Status);
        if (!Reason.IsEmpty()) Event->SetStringField(TEXT("reason"), Reason);
    }
}

UWorld* AngelIO::GetScriptWorld()
{
    auto* Context = asGetActiveContext();
    auto* Scope = Context ? static_cast<FExecutionScope*>(Context->GetUserData(ExecutionKey)) : nullptr;
    return Scope ? Scope->World.Get() : nullptr;
}

FGuid AngelIO::GetCheckpointStepId()
{
    auto* Context = asGetActiveContext();
    auto* Scope = Context ? static_cast<FExecutionScope*>(Context->GetUserData(ExecutionKey)) : nullptr;
    return Scope ? Scope->CheckpointStepId : FGuid();
}

UAngelIOScriptComponent* AngelIO::GetScriptMission()
{
    auto* Context = asGetActiveContext();
    auto* Scope = Context ? static_cast<FExecutionScope*>(Context->GetUserData(ExecutionKey)) : nullptr;
    return Scope ? Cast<UAngelIOScriptComponent>(Scope->Component.Get()) : nullptr;
}

void AngelIO::AwaitOperation(TSharedRef<FAngelIOOperation> Operation, const FString& Label)
{
    AwaitOperation(Operation, Label, {});
}

TSharedRef<const EAngelIOWaitOutcome> AngelIO::AwaitOperation(TSharedRef<FAngelIOOperation> Operation,
    const FString& Label, TOptional<double> TimeoutSeconds)
{
    auto Outcome = MakeShared<EAngelIOWaitOutcome>(EAngelIOWaitOutcome::Detached);
    auto* Scope = GetWaitScope(TimeoutSeconds);
    if (!Scope) return Outcome;
    const bool bPoll = TimeoutSeconds.IsSet() && *TimeoutSeconds == 0;
    *Outcome = Operation->IsComplete() ? EAngelIOWaitOutcome::Completed :
        bPoll ? EAngelIOWaitOutcome::TimedOut : EAngelIOWaitOutcome::Pending;
    const bool bPending = *Outcome == EAngelIOWaitOutcome::Pending;
    if (auto Event = Operation->AddEvent(TEXT("wait")))
    {
        auto Site = CaptureSite();
        Event->SetObjectField(TEXT("waiter"), Site);
        Event->SetBoolField(TEXT("suspended"), bPending);
        if (TimeoutSeconds.IsSet()) Event->SetNumberField(TEXT("timeoutSeconds"), *TimeoutSeconds);
        if (*Outcome == EAngelIOWaitOutcome::TimedOut) Event->SetStringField(TEXT("outcome"), TEXT("TimedOut"));
        if (bPending) Scope->WaitSite = Site;
    }
    if (!bPending) return Outcome;
    Scope->Pending = Operation;
    Scope->WaitOutcome = Outcome;
    if (TimeoutSeconds.IsSet()) Scope->Deadline = Scope->World->GetTimeSeconds() + *TimeoutSeconds;
    Scope->Label = Label.IsEmpty() ? TEXT("Operation") : Label;
    auto* Context = asGetActiveContext();
#if WITH_EDITOR
    // After the SDK returns from this native call, its program counter may point at the next statement.
    Scope->WaitLine = FMissionFrame(*Context).Line;
#endif
    Context->Suspend();
    return Outcome;
}

void AngelIO::AwaitCondition(const FString& Label, const FString& Target, TOptional<double> TimeoutSeconds,
    TFunction<bool()> IsReady)
{
    auto* Scope = GetWaitScope(TimeoutSeconds);
    if (!Scope) return;
    auto Condition = MakeShared<FConditionWait>(Scope->World.Get(), Label, Target, MoveTemp(IsReady));
    Condition->Poll();
    const auto Outcome = AwaitOperation(Condition, Label, TimeoutSeconds);
    if (*Outcome == EAngelIOWaitOutcome::Pending) Scope->Condition = Condition;
    else if (*Outcome == EAngelIOWaitOutcome::TimedOut) Condition->Timeout();
    else if (*Outcome == EAngelIOWaitOutcome::Detached) Condition->Detach();
}

struct FAngelIOModuleBindings
{
    TArray<FAngelIOScriptWrapper> Wrappers;
#if WITH_EDITOR
    TArray<FAngelIOMapLookup> MapLookups{{TEXT("SendTo"), nullptr, false}};
#endif
};

struct FAngelIORegisteredEngine
{
    asIScriptEngine* Engine;
    FAngelIOBytecodeCache Cache;
    FAngelIOModuleBindings Bindings;
    TArray<FString> Declarations;
    uint64 NextModule = 0;

    FAngelIORegisteredEngine(asIScriptEngine* InEngine, UWorld& World) : Engine(InEngine), Cache(World) {}
    ~FAngelIORegisteredEngine() { Engine->ShutDownAndRelease(); }
};

void UAngelIOScriptWorldSubsystem::Deinitialize()
{
    bClosing = true;
    while (!Programs.IsEmpty()) (*Programs.CreateConstIterator())->EndWorld();
    RegisteredEngine.Reset();
    Super::Deinitialize();
}

TOptional<FAngelIOBytecodeCacheStats> AngelIO::GetBytecodeCacheStats(UWorld* World)
{
    auto* Owner = World ? World->GetSubsystem<UAngelIOScriptWorldSubsystem>() : nullptr;
    if (!Owner || !Owner->RegisteredEngine || Owner->bClosing) return {};
    return Owner->RegisteredEngine->Cache.Stats;
}

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
TWeakPtr<FAngelIORegisteredEngine> FAngelIOStartupProbe::WorldEngine(UWorld* World)
{
    auto* Owner = World ? World->GetSubsystem<UAngelIOScriptWorldSubsystem>() : nullptr;
    return Owner ? Owner->RegisteredEngine : nullptr;
}

int32 FAngelIOStartupProbe::WorldModuleCount(UWorld* World)
{
    auto Registered = WorldEngine(World).Pin();
    return Registered ? int32(Registered->Engine->GetModuleCount()) : 0;
}
#endif

struct FAngelIOProgram::FImpl
{
    asIScriptEngine* Engine = nullptr;
    TSharedPtr<FAngelIORegisteredEngine> SharedEngine;
    bool PrepareWorldModule(UAngelIOScriptWorldSubsystem& Owner, FAngelIOModuleBindings& Bindings);
    bool CreateWorldModule();
    bool RegisterAPI(FAngelIOModuleBindings& Bindings);
    asIScriptModule* Module = nullptr;
    asIScriptContext* Context = nullptr;
    TArray<FAngelIODiagnostic> Messages;
    TArray<FAngelIOPort> PublicPorts;
    TMap<int32, FString> Inputs, Outputs;
    TArray<FAngelIOEvent> Events;
    FExecutionScope Scope;
    FString ScriptSection;
    int32 RestoredVersion = 0;
#if WITH_EDITOR
    FString RestoredStage;
    TArray<FAngelIOLiteralTarget> LiteralTargets;

    // Display the initializer without executing game-module code in the editor.
    void ReadPortDefaults(std::string_view Source)
    {
        FAngelIOPort* Port = nullptr;
        const char* Start = nullptr;
        std::string_view Previous;
        int32 Braces = 0, Parens = 0;
        for (auto& P : PublicPorts)
            P.DefaultExpression = P.Type == EAngelIOType::Boolean ? TEXT("false") : TEXT("0");
        while (!Source.empty())
        {
            asUINT Size = 0;
            const auto Kind = Engine->ParseToken(Source.data(), Source.size(), &Size);
            if (!Size) break;
            const auto Token = Source.substr(0, Size);
            Source.remove_prefix(Size);
            if (Kind == asTC_COMMENT || Kind == asTC_WHITESPACE) continue;
            if (!Braces && !Parens)
            {
                if (Port && (Token == ";" || Token == ","))
                {
                    if (Start)
                    {
                        FUTF8ToTCHAR Text(Start, int32(Token.data() - Start));
                        Port->DefaultExpression = FString(Text.Length(), Text.Get()).TrimStartAndEnd();
                    }
                    Port = nullptr;
                    Start = nullptr;
                }
                else if (Port && Token == "=" && !Start) Start = Source.data();
                else if (!Port && (Previous == "int" || Previous == "bool" || Previous == "float" || Previous == ","))
                {
                    const bool Input = Token.starts_with("in_");
                    if (Input || Token.starts_with("out_"))
                    {
                        const auto Name = Token.substr(Input ? 3 : 4);
                        FUTF8ToTCHAR Text(Name.data(), int32(Name.size()));
                        const FString Field(Text.Length(), Text.Get());
                        Port = PublicPorts.FindByPredicate([&](const auto& P){ return P.bInput == Input && P.Name == Field; });
                    }
                }
            }
            if (Token == "{") ++Braces;
            if (Token == "}") --Braces;
            if (Token == "(") ++Parens;
            if (Token == ")") --Parens;
            Previous = Token;
        }
    }

    // The SDK lexer excludes comments and quoted source text from call matching.
    // Only direct literal arguments are knowable before running a mission.
    void ReadMapTargets(std::string_view Source, TArray<FAngelIOMapLookup>& MapLookups)
    {
        // A mission may define an overload or a method that hides a provider's
        // unqualified name. Leave that name to runtime resolution in this file.
        auto ExcludeShadow = [&](const asIScriptFunction* Function)
        {
            const FString Name(UTF8_TO_TCHAR(Function->GetName()));
            MapLookups.RemoveAll([&](const FAngelIOMapLookup& Rule)
                { return Rule.Function.Equals(Name, ESearchCase::CaseSensitive); });
        };
        for (asUINT I = 0; I < Module->GetFunctionCount(); ++I)
        {
            const auto* Fn = Module->GetFunctionByIndex(I);
            const char* Section = nullptr;
            Fn->GetDeclaredAt(&Section, nullptr, nullptr);
            if (!Section || FCStringAnsi::Strcmp(Section, "AngelIO API") != 0) ExcludeShadow(Fn);
        }
        for (asUINT I = 0; I < Module->GetObjectTypeCount(); ++I)
        {
            const auto* Type = Module->GetObjectTypeByIndex(I);
            for (asUINT M = 0; M < Type->GetMethodCount(); ++M) ExcludeShadow(Type->GetMethodByIndex(M));
        }
        if (MapLookups.IsEmpty()) return;
        struct FToken { std::string_view Text; int32 Line = 1; };
        FToken Before, Function, Open, Argument;
        int32 Line = 1;
        while (!Source.empty())
        {
            asUINT Length = 0;
            const auto Kind = Engine->ParseToken(Source.data(), Source.size(), &Length);
            if (!Length) break;
            const auto Token = Source.substr(0, Length);
            Source.remove_prefix(Length);
            const int32 TokenLine = Line;
            Line += static_cast<int32>(std::count(Token.begin(), Token.end(), '\n'));
            if (Kind == asTC_COMMENT || Kind == asTC_WHITESPACE) continue;
            const auto Literal = Argument.Text;
            if ((Token == "," || Token == ")") && Open.Text == "(" && Before.Text != "." && Before.Text != "::" &&
                Literal.size() >= 2 && Literal.front() == '"' && Literal.back() == '"' &&
                Literal.substr(1, Literal.size() - 2).find_first_of("\\\"\r\n") == std::string_view::npos)
            {
                for (const auto& Lookup : MapLookups)
                {
                    FTCHARToUTF8 Name(*Lookup.Function);
                    if (Function.Text != std::string_view(Name.Get(), Name.Length())) continue;
                    const FUTF8ToTCHAR Target(Literal.data() + 1, static_cast<int32>(Literal.size() - 2));
                    LiteralTargets.Add({Lookup.Function, FString(Target.Length(), Target.Get()),
                        Lookup.OwnerClass, Lookup.bRequireSingleActor, Function.Line});
                }
            }
            Before = Function;
            Function = Open;
            Open = Argument;
            Argument = {Token, TokenLine};
        }
    }
#endif
    TArray<FString> ExternalAPI;
    EAngelIOState Status = EAngelIOState::Empty;
    FString Waiting;
    int32 Payload = 0;
    int32 Steps = 0;
    int32 BudgetLine = 0;
    bool bInRun = false;
    FImpl* Root = nullptr;
    int32 ChildCount = 0;
    TArray<TSharedPtr<FImpl>> Children;
    TSet<FString> EndSignals;
    FString BranchName;

    TArray<FAngelIOEvent>& EventQueue() { return Root ? Root->Events : Events; }

    void DetachOwner()
    {
        Scope.Component.Reset();
        for (const auto& Child : Children) Child->DetachOwner();
    }

    void AbortActiveContexts()
    {
        if (Context && Context->GetState() == asEXECUTION_ACTIVE) Context->Abort();
        for (const auto& Child : Children) Child->AbortActiveContexts();
    }

    ~FImpl()
    {
        Children.Reset();
        if (Scope.WaitOutcome) *Scope.WaitOutcome = EAngelIOWaitOutcome::Detached;
        if (Scope.Pending && Scope.WaitSite)
            if (auto Event = Scope.Pending->AddEvent(TEXT("detach")))
                Event->SetObjectField(TEXT("waiter"), Scope.WaitSite);
        if (Scope.Condition) Scope.Condition->Detach();
        if (Context) Context->Release();
        if (Root) --Root->ChildCount;
        else if (SharedEngine)
        {
            if (Module) Module->Discard();
        }
        else if (Engine) Engine->ShutDownAndRelease();
    }

    bool Fail(FString Message, int32 Line = 0, int32 Column = 0)
    {
        Messages.Add({TEXT("error"), MoveTemp(Message), Line, Column});
        Status = EAngelIOState::Failed;
        Waiting.Reset();
        Events.Reset();
        Children.Reset();
        return false;
    }

    static void CompilerMessage(const asSMessageInfo* Info, void* User)
    {
        auto& Self = *static_cast<FImpl*>(User);
        const TCHAR* Severity = Info->type == asMSGTYPE_ERROR ? TEXT("error") :
            Info->type == asMSGTYPE_WARNING ? TEXT("warning") : TEXT("info");
        Self.Messages.Add({Severity, UTF8_TO_TCHAR(Info->message), Info->row, Info->col});
    }

    static void Line(asIScriptContext* Ctx, void* User)
    {
        auto& Self = *static_cast<FImpl*>(User);
        if (++Self.Steps > 100000)
        {
            Self.BudgetLine = Ctx->GetLineNumber();
            Ctx->Abort();
        }
    }

    static void SignalCall(asIScriptGeneric* Call, bool bWait)
    {
        asIScriptContext* Ctx = asGetActiveContext();
        auto* Owner = static_cast<FImpl*>(Ctx->GetUserData());
        if (!Owner)
        {
            Ctx->SetException("Signals require an active mission context");
            return;
        }
        auto& Self = *Owner;
        const char* Expected = bWait ? "InputSignal" : "OutputSignal";
        const int EnumType = Self.Module->GetTypeIdByDecl(Expected);
        if (!Self.bInRun || EnumType < 0 || Call->GetArgTypeId(0) != EnumType)
        {
            Ctx->SetException(bWait ? "WaitFor requires an InputSignal in Run" : "Emit requires an OutputSignal in Run");
            return;
        }
        const int32 Id = *static_cast<const int32*>(Call->GetArgAddress(0));
        const FString* Name = (bWait ? Self.Inputs : Self.Outputs).Find(Id);
        if (!Name)
        {
            Ctx->SetException("Unknown signal value");
            return;
        }
        if (bWait)
        {
            Self.Waiting = *Name;
#if WITH_EDITOR
            Self.Scope.WaitLine = FMissionFrame(*Ctx).Line;
#endif
            Ctx->Suspend();
        }
        else
        {
            Self.EventQueue().Add({*Name, static_cast<int32>(Call->GetArgDWord(1))});
        }
    }

    static void Wait(asIScriptGeneric* Call) { SignalCall(Call, true); }
    static void SendTo(asIScriptGeneric* Call)
    {
        auto* Context = asGetActiveContext();
        auto* Self = static_cast<FImpl*>(Context->GetUserData());
        const auto& Target = *static_cast<const std::string*>(Call->GetArgObject(0));
        const auto& Input = *static_cast<const std::string*>(Call->GetArgObject(1));
        if (!Self || !Self->bInRun || Target.empty() || Target.size() >= NAME_SIZE || Input.empty())
        {
            Context->SetException("SendTo requires a target name and input signal in Run");
            return;
        }
        const FName TargetName(UTF8_TO_TCHAR(Target.c_str()));
        if (TargetName.IsNone())
        {
            Context->SetException("None is not an addressable target");
            return;
        }
        Self->EventQueue().Add({UTF8_TO_TCHAR(Input.c_str()), static_cast<int32>(Call->GetArgDWord(2)), TargetName});
    }
    static void Emit(asIScriptGeneric* Call) { SignalCall(Call, false); }
    static void BranchSignal(asIScriptGeneric* Call)
    {
        auto* Ctx = asGetActiveContext();
        auto* Self = static_cast<FImpl*>(Ctx->GetUserData());
        const bool bEnd = Call->GetArgCount() == 1;
        if (!Self || !Self->bInRun || (bEnd && !Self->Root) ||
            Call->GetArgTypeId(0) != Self->Module->GetTypeIdByDecl("InputSignal"))
        {
            Ctx->SetException("Notify requires a mission input; EndOn requires a child branch and a mission input");
            return;
        }
        const auto* Name = Self->Inputs.Find(*static_cast<const int32*>(Call->GetArgAddress(0)));
        if (!Name) { Ctx->SetException("Unknown input signal"); return; }
        if (bEnd) Self->EndSignals.Add(*Name);
        else Self->EventQueue().Add({*Name, static_cast<int32>(Call->GetArgDWord(1)), NAME_None, true});
    }

    static void WaitSeconds(asIScriptGeneric* Call)
    {
        const float Seconds = Call->GetArgFloat(0);
        auto* Scope = GetWaitScope(Seconds);
        if (!Scope || Seconds == 0) return;
        TWeakObjectPtr<UWorld> World = Scope->World;
        const double End = World->GetTimeSeconds() + Seconds;
        AngelIO::AwaitCondition(TEXT("WaitSeconds"), {}, {}, [World, End]{ return World.IsValid() && World->GetTimeSeconds() >= End; });
    }

    static void Fork(asIScriptGeneric* Call)
    {
        auto* Ctx = asGetActiveContext();
        auto* Self = static_cast<FImpl*>(Ctx->GetUserData());
        auto* Fn = static_cast<asIScriptFunction*>(Call->GetArgObject(0));
        if (!Self || !Self->bInRun || !Fn || Fn->GetModule() != Self->Module || Fn->GetObjectType())
        {
            Ctx->SetException("Fork requires a mission function with signature void f()");
            return;
        }
        auto* Root = Self->Root ? Self->Root : Self;
        if (Root->ChildCount >= 64) { Ctx->SetException("Mission branch limit reached"); return; }
        TSharedPtr<FImpl> Child = MakeShared<FImpl>();
        Child->Root = Root;
        ++Root->ChildCount;
        Child->Engine = Self->Engine;
        Child->Module = Self->Module;
        Child->Inputs = Self->Inputs;
        Child->Outputs = Self->Outputs;
        Child->ScriptSection = Self->ScriptSection;
        Child->Scope.Component = Self->Scope.Component;
        Child->Scope.World = Self->Scope.World;
        Child->Scope.InstanceId = Self->Scope.InstanceId;
        Child->Scope.BranchId = FGuid::NewGuid();
        Child->Scope.CheckpointStepId = Self->Scope.CheckpointStepId;
        Child->BranchName = UTF8_TO_TCHAR(Fn->GetName());
        Child->Context = Self->Engine->CreateContext();
        if (!Child->Context || Child->Context->Prepare(Fn) < 0) { Ctx->SetException("Cannot prepare child branch"); return; }
        Child->Context->SetUserData(Child.Get());
        Child->Context->SetLineCallback(asFUNCTION(Line), Child.Get(), asCALL_CDECL);
        Self->Children.Add(Child);
        if (!Child->Execute()) Ctx->SetException(TCHAR_TO_UTF8(*Child->Messages.Last().Message));
        if (Child->Status != EAngelIOState::Waiting) Self->Children.RemoveSingle(Child);
    }
    static void SignalValue(asIScriptGeneric* Call)
    {
        const auto* Self = static_cast<FImpl*>(asGetActiveContext()->GetUserData());
        if (Self)
            Call->SetReturnDWord(Self->Payload);
        else
            asGetActiveContext()->SetException("SignalValue requires an active mission context");
    }

    static void AccessCheckpoint(asIScriptGeneric* Call, bool bSave)
    {
        auto* Context = asGetActiveContext();
        auto* Self = static_cast<FImpl*>(Context->GetUserData());
        // AngelScript owns the return slot even when a native binding raises an exception.
        std::string* Result = bSave ? nullptr : new (Call->GetAddressOfReturnLocation()) std::string();
        const auto* Component = Self ? Self->Scope.Component.Get() : nullptr;
        auto* World = Self ? Self->Scope.World.Get() : nullptr;
        const auto* Owner = Component ? Component->GetOwner() : nullptr;
        if (!Self || !Self->bInRun || !World || World->GetNetMode() == NM_Client || !IsValid(Owner) ||
            !Owner->HasAuthority() || !Owner->GetLevel() || Component->GetWorld() != World)
        {
            Context->SetException("Checkpoints require an authoritative mission component in Run");
            return;
        }
        const int32 Version = static_cast<int32>(Call->GetArgDWord(bSave ? 1 : 0));
        if (Version < 1)
        {
            Context->SetException("Checkpoint version must be positive");
            return;
        }
        auto* Store = World->GetSubsystem<UAngelIOCheckpointSubsystem>();
        if (!Store)
        {
            Context->SetException("Checkpoint store is unavailable in this world");
            return;
        }
        UAngelIOCheckpointSubsystem::FCheckpoint Record;
        FString Error;
        if (bSave)
        {
            const auto& Stage = *static_cast<const std::string*>(Call->GetArgObject(0));
            if (Stage.empty() || Stage.find('\0') != std::string::npos)
            {
                Context->SetException("Checkpoint stage must be non-empty and contain no null characters");
                return;
            }
            Record.Version = Version;
            Record.Stage = FUtf8String(reinterpret_cast<const UTF8CHAR*>(Stage.c_str()));
            Record.SavedBy = Self->Scope.InstanceId;
            if (!Store->Save(*Component, Self->ScriptSection, Record, Error))
            {
                Context->SetException(TCHAR_TO_UTF8(*Error));
                return;
            }
        }
        else
        {
            if (!Store->Load(*Component, Self->ScriptSection, Record, Error))
            {
                Context->SetException(TCHAR_TO_UTF8(*Error));
                return;
            }
            if (Record.Version != Version)
            {
                Self->Scope.CheckpointStepId.Invalidate();
                return;
            }
            Result->assign(reinterpret_cast<const char*>(*Record.Stage), Record.Stage.Len());
        }
        Self->Scope.CheckpointStepId = FGuid::NewDeterministicGuid(FString::Printf(TEXT("%s/%d/%s"),
            *Record.SavedBy.ToString(), Version, *FString(Record.Stage)));
        if (!bSave)
        {
            // Only the first matching read from an earlier run establishes this run's restore point.
            if (Self->RestoredVersion || Record.SavedBy == Self->Scope.InstanceId) return;
            Self->RestoredVersion = Version;
#if WITH_EDITOR
            Self->RestoredStage = FString(Record.Stage);
#endif
        }
        if (auto* Trace = World->GetSubsystem<UAngelIOTraceSubsystem>())
            Trace->RecordCheckpoint(bSave, Record.Id, FString(Record.Stage), Version, Record.SavedBy, CaptureSite());
    }
    static void LoadCheckpoint(asIScriptGeneric* Call) { AccessCheckpoint(Call, false); }
    static void SaveCheckpoint(asIScriptGeneric* Call) { AccessCheckpoint(Call, true); }

    // Give the public functions the enum types declared by this mission. Use the
    // SDK tokenizer so comments, strings and enums inside namespaces stay local.
    void AddSignalBindings(std::string_view Source)
    {
        bool bAfterEnum = false;
        int32 Depth = 0;
        while (!Source.empty())
        {
            asUINT Length = 0;
            const auto Kind = Engine->ParseToken(Source.data(), Source.size(), &Length);
            if (!Length) break;
            const auto Token = Source.substr(0, Length);
            Source.remove_prefix(Length);
            if (Kind == asTC_COMMENT || Kind == asTC_WHITESPACE) continue;
            if (bAfterEnum && Token == "InputSignal")
                Module->AddScriptSection("AngelIO API", "void WaitFor(InputSignal s) { AngelIOInternal::WaitFor(s); }\n"
                    "void EndOn(InputSignal s) { AngelIOInternal::EndOn(s); }\n"
                    "void Notify(InputSignal s, int value = 0) { AngelIOInternal::Notify(s, value); }");
            if (bAfterEnum && Token == "OutputSignal")
                Module->AddScriptSection("AngelIO API", "void Emit(OutputSignal s, int v) { AngelIOInternal::Emit(s, v); }");
            if (Token == "{") ++Depth;
            if (Token == "}") --Depth;
            bAfterEnum = Depth == 0 && Token == "enum";
        }
    }

    bool ReadSignals(const char* EnumName, TMap<int32, FString>& Names)
    {
        asITypeInfo* Info = Module->GetTypeInfoByName(EnumName);
        if (!Info) return true;
        if (!(Info->GetFlags() & asOBJ_ENUM) || Info->GetSize() != sizeof(int32))
            return Fail(FString::Printf(TEXT("%s must be a 32-bit enum"), UTF8_TO_TCHAR(EnumName)));
        for (asUINT I = 0; I < Info->GetEnumValueCount(); ++I)
        {
            asINT64 Id;
            const FString Name = UTF8_TO_TCHAR(Info->GetEnumValueByIndex(I, &Id));
            if (Id < MIN_int32 || Id > MAX_int32 || Names.Contains(static_cast<int32>(Id)))
                return Fail(FString::Printf(TEXT("Invalid or duplicate signal ID for %s"), *Name));
            Names.Add(static_cast<int32>(Id), Name);
        }
        return true;
    }

    bool Inspect()
    {
        if (!Module->GetFunctionByDecl("void Run()"))
            return Fail(TEXT("Missing entry point void Run()"));
        if (Module->GetImportedFunctionCount())
            return Fail(TEXT("Imported script functions require a module binding"));
        for (asUINT I = 0; I < Module->GetGlobalVarCount(); ++I)
        {
            const char* RawName;
            const char* Namespace;
            int TypeId;
            bool bConst;
            Module->GetGlobalVar(I, &RawName, &Namespace, &TypeId, &bConst);
            if (*Namespace) continue;
            const FString Name = UTF8_TO_TCHAR(RawName);
            const bool bInput = Name.StartsWith(TEXT("in_"), ESearchCase::CaseSensitive);
            if (!bInput && !Name.StartsWith(TEXT("out_"), ESearchCase::CaseSensitive)) continue;
            FString PublicName = Name.Mid(bInput ? 3 : 4);
            EAngelIOType Type;
            switch (TypeId)
            {
                case asTYPEID_INT32: Type = EAngelIOType::Integer; break;
                case asTYPEID_FLOAT: Type = EAngelIOType::Float; break;
                case asTYPEID_BOOL: Type = EAngelIOType::Boolean; break;
                default: return Fail(FString::Printf(TEXT("Unsupported public port type for %s"), *Name));
            }
            if (PublicName.IsEmpty() || (bInput && bConst))
                return Fail(FString::Printf(TEXT("Invalid public port %s"), *Name));
            PublicPorts.Add({MoveTemp(PublicName), Type, bInput, static_cast<int32>(I)});
        }
        return ReadSignals("InputSignal", Inputs) && ReadSignals("OutputSignal", Outputs);
    }

    bool Execute()
    {
        Status = EAngelIOState::Running;
        Steps = 0;
        bInRun = true;
        Context->SetUserData(&Scope, ExecutionKey);
        const int Code = Context->Execute();
        Context->SetUserData(nullptr, ExecutionKey);
        bInRun = false;
        if (Scope.Pending) Waiting = Scope.Label;
        if (Code == asEXECUTION_FINISHED)
        {
            Status = EAngelIOState::Finished;
            Children.Reset();
        }
        else if (Code == asEXECUTION_SUSPENDED && !Waiting.IsEmpty())
            Status = EAngelIOState::Waiting;
        else
        {
            if (Code == asEXECUTION_ABORTED)
                return Fail(TEXT("Execution budget exceeded before the next wait"), BudgetLine);
            int Column = 0;
            const int Row = Context->GetExceptionLineNumber(&Column);
            const char* Message = Context->GetExceptionString();
            return Fail(Message ? UTF8_TO_TCHAR(Message) : TEXT("Execution failed"), Row, Column);
        }
        return true;
    }

    const FAngelIOPort* FindPort(const FString& Name, bool bInput) const
    {
        return PublicPorts.FindByPredicate([&](const FAngelIOPort& P)
        {
            return P.bInput == bInput && P.Name.Equals(Name, ESearchCase::CaseSensitive);
        });
    }

    bool NeedsTick() const
    {
        return Status == EAngelIOState::Waiting && (Scope.Pending.IsValid() ||
            Children.ContainsByPredicate([](const auto& Child){ return Child->NeedsTick(); }));
    }
    bool ResumeOperation();
    bool Advance(const FString* Signal, int32 Value);
};

FAngelIOProgram::FAngelIOProgram() : Impl(MakeShared<FImpl>()) {}
FAngelIOProgram::~FAngelIOProgram() { UnbindWorld(); }

void FAngelIOProgram::UnbindWorld()
{
    if (WorldOwner) WorldOwner->Programs.Remove(this);
    WorldOwner = nullptr;
}

void FAngelIOProgram::EndWorld()
{
    UnbindWorld();
    Impl->AbortActiveContexts();
    Impl->DetachOwner();
    Impl = MakeShared<FImpl>();
    Impl->Fail(TEXT("Mission world ended"));
}

void FAngelIOProgram::DetachComponent()
{
    Impl->DetachOwner();
}

bool FAngelIOProgram::CompileFile(const FString& RelativeScript, UWorld* World)
{
    ANGELIO_BENCHMARK_MARK(FileBegin);
    const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("AngelIO"));
    FString Path = FPaths::ConvertRelativePathToFull(Root, RelativeScript);
    FString Source;
    if (!FPaths::IsRelative(RelativeScript) || !FPaths::CollapseRelativeDirectories(Path) ||
        !FPaths::IsUnderDirectory(Path, Root) || !FPaths::GetExtension(Path).Equals(TEXT("as"), ESearchCase::IgnoreCase) ||
        !FFileHelper::LoadFileToString(Source, *Path))
    {
        UnbindWorld();
        Impl = MakeShared<FImpl>();
        return Impl->Fail(TEXT("Cannot read .as under Content/AngelIO: ") + RelativeScript);
    }
    return Compile(RelativeScript, Source, World);
}

bool FAngelIOProgram::FImpl::RegisterAPI(FAngelIOModuleBindings& Bindings)
{
    Engine = asCreateScriptEngine();
    ANGELIO_BENCHMARK_MARK(EngineCreated);
    if (!Engine) return Fail(TEXT("Could not create AngelScript engine"));
    RegisterStdString(Engine);
    if (Engine->SetMessageCallback(asFUNCTION(FImpl::CompilerMessage), this, asCALL_CDECL) < 0 ||
        Engine->SetEngineProperty(asEP_INIT_GLOBAL_VARS_AFTER_BUILD, false) < 0 ||
        Engine->SetDefaultNamespace("AngelIOInternal") < 0 ||
        Engine->RegisterGlobalFunction("void WaitFor(const ?&in)", asFUNCTION(FImpl::Wait), asCALL_GENERIC) < 0 ||
        Engine->RegisterGlobalFunction("void Emit(const ?&in, int)", asFUNCTION(FImpl::Emit), asCALL_GENERIC) < 0 ||
        Engine->RegisterGlobalFunction("void EndOn(const ?&in)", asFUNCTION(FImpl::BranchSignal), asCALL_GENERIC) < 0 ||
        Engine->RegisterGlobalFunction("void Notify(const ?&in, int)", asFUNCTION(FImpl::BranchSignal), asCALL_GENERIC) < 0 ||
        Engine->SetDefaultNamespace("") < 0 ||
        Engine->RegisterFuncdef("void MissionBranch()") < 0 ||
        Engine->RegisterGlobalFunction("void Fork(MissionBranch@+ entry)", asFUNCTION(FImpl::Fork), asCALL_GENERIC) < 0 ||
        Engine->RegisterGlobalFunction("void WaitSeconds(float seconds)", asFUNCTION(FImpl::WaitSeconds), asCALL_GENERIC) < 0 ||
        Engine->RegisterGlobalFunction("void SendTo(const string &in target, const string &in input, int value)", asFUNCTION(FImpl::SendTo), asCALL_GENERIC) < 0 ||
        Engine->RegisterGlobalFunction("string LoadCheckpoint(int version = 1)", asFUNCTION(FImpl::LoadCheckpoint), asCALL_GENERIC) < 0 ||
        Engine->RegisterGlobalFunction("void SaveCheckpoint(const string &in stage, int version = 1)", asFUNCTION(FImpl::SaveCheckpoint), asCALL_GENERIC) < 0 ||
        Engine->RegisterGlobalFunction("int SignalValue()", asFUNCTION(FImpl::SignalValue), asCALL_GENERIC) < 0)
        return Fail(TEXT("Could not register AngelIO script API"));
    ANGELIO_BENCHMARK_MARK(CoreRegistered);
    auto& SDK = *Engine;
    const auto FirstFunction = SDK.GetGlobalFunctionCount();
    const auto FirstType = SDK.GetObjectTypeCount();
    const auto FirstEnum = SDK.GetEnumCount();
    for (auto* Provider : IModularFeatures::Get().GetModularFeatureImplementations<IAngelIOScriptAPI>(
        IAngelIOScriptAPI::FeatureName()))
    {
        if (!Provider->RegisterBindings(SDK) || SDK.SetDefaultNamespace("") < 0)
            return Fail(TEXT("Could not register external AngelIO script bindings"));
        Provider->GetScriptWrappers(Bindings.Wrappers);
#if WITH_EDITOR
        Provider->GetMapLookups(Bindings.MapLookups);
#endif
    }
    for (asUINT I = FirstFunction; I < SDK.GetGlobalFunctionCount(); ++I)
    {
        const auto* Function = SDK.GetGlobalFunctionByIndex(I);
        if (FCStringAnsi::Strcmp(Function->GetNamespace(), "AngelIOInternal") != 0)
            ExternalAPI.Add(UTF8_TO_TCHAR(Function->GetDeclaration(true, true, true)));
    }
    for (asUINT I = FirstType; I < SDK.GetObjectTypeCount(); ++I)
    {
        auto* Type = SDK.GetObjectTypeByIndex(I);
        ExternalAPI.Add(UTF8_TO_TCHAR(SDK.GetTypeDeclaration(Type->GetTypeId(), true)));
        for (asUINT M = 0; M < Type->GetMethodCount(); ++M)
            ExternalAPI.Add(UTF8_TO_TCHAR(Type->GetMethodByIndex(M)->GetDeclaration(true, true, true)));
    }
    for (asUINT I = FirstEnum; I < SDK.GetEnumCount(); ++I)
    {
        auto* Type = SDK.GetEnumByIndex(I);
        TArray<FString> Names;
        for (asUINT N = 0; N < Type->GetEnumValueCount(); ++N)
        {
            asINT64 Value;
            Names.Add(UTF8_TO_TCHAR(Type->GetEnumValueByIndex(N, &Value)));
        }
        ExternalAPI.Add(FString::Printf(TEXT("enum %s { %s }"),
            UTF8_TO_TCHAR(SDK.GetTypeDeclaration(Type->GetTypeId(), true)), *FString::Join(Names, TEXT(", "))));
    }
    ANGELIO_BENCHMARK_MARK(ProvidersInspected);
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
    if (auto* Probe = FAngelIOStartupProbe::Current()) ++Probe->Registrations;
#endif
    return true;
}

bool FAngelIOProgram::FImpl::PrepareWorldModule(UAngelIOScriptWorldSubsystem& Owner, FAngelIOModuleBindings& Bindings)
{
    if (!Owner.RegisteredEngine)
    {
        if (Owner.bRegistering) return Fail(TEXT("Recursive world API registration"));
        TGuardValue<bool> Guard(Owner.bRegistering, true);
        if (!RegisterAPI(Bindings)) return false;
        if (Owner.bClosing) return Fail(TEXT("Mission world ended during API registration"));
        SharedEngine = MakeShared<FAngelIORegisteredEngine>(Engine, *Owner.GetWorld());
        SharedEngine->Bindings = Bindings;
        SharedEngine->Declarations = ExternalAPI;
        Owner.RegisteredEngine = SharedEngine;
    }
    else
    {
        SharedEngine = Owner.RegisteredEngine;
        Engine = SharedEngine->Engine;
        ANGELIO_BENCHMARK_MARK(EngineCreated);
        ANGELIO_BENCHMARK_MARK(CoreRegistered);
        if (Engine->SetMessageCallback(asFUNCTION(FImpl::CompilerMessage), this, asCALL_CDECL) < 0)
            return Fail(TEXT("Could not route shared engine diagnostics"));
        ExternalAPI = SharedEngine->Declarations;
        Bindings = SharedEngine->Bindings; // Map validation may remove shadowed names from this copy.
        ANGELIO_BENCHMARK_MARK(ProvidersInspected);
    }
    return CreateWorldModule();
}

bool FAngelIOProgram::FImpl::CreateWorldModule()
{
    const FString Name = FString::Printf(TEXT("mission_%llu"), ++SharedEngine->NextModule);
    Module = Engine->GetModule(TCHAR_TO_UTF8(*Name), asGM_ALWAYS_CREATE);
    return Module || Fail(TEXT("Could not create shared engine mission module"));
}

bool FAngelIOProgram::Compile(const FString& Label, const FString& Source, UWorld* World)
{
    ANGELIO_BENCHMARK_MARK(CompileBegin);
    UnbindWorld();
    Impl = MakeShared<FImpl>();
    const auto Compiling = Impl; // A native provider may trigger teardown.
    auto& S = *Compiling;
    S.ScriptSection = Label;
    FAngelIOModuleBindings Bindings;
    // The SDK callback is engine-wide. Keep the per-mission destination only for this compile.
    ON_SCOPE_EXIT { if (S.SharedEngine) S.Engine->ClearMessageCallback(); };
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
    if (auto* Probe = FAngelIOStartupProbe::Current(); Probe && Probe->bForcePrivateEngine) World = nullptr;
#endif
    if (World)
    {
        auto* Owner = World->GetSubsystem<UAngelIOScriptWorldSubsystem>();
        if (!Owner || Owner->bClosing || World->bIsTearingDown || World->GetNetMode() == NM_Client)
            return S.Fail(TEXT("Mission compilation requires a live authority game world"));
        WorldOwner = Owner;
        Owner->Programs.Add(this);
        if (!S.PrepareWorldModule(*Owner, Bindings)) return false;
    }
    else
    {
        if (!S.RegisterAPI(Bindings)) return false;
        S.Module = S.Engine->GetModule("level", asGM_ALWAYS_CREATE);
    }
    FString WrapperSource;
    for (const auto& Wrapper : Bindings.Wrappers)
    {
        WrapperSource += Wrapper.Declaration + TEXT(" { ") + Wrapper.Body + TEXT(" }\n");
        S.ExternalAPI.Add(Wrapper.Declaration);
    }
    FTCHARToUTF8 SourceBytes(*Source);
    auto* Cache = S.SharedEngine ? &S.SharedEngine->Cache : nullptr;
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
    // The historical paired benchmarks explicitly select their own source/bytecode path.
    if (FAngelIOStartupProbe::Current()) Cache = nullptr;
#endif
    const bool bCacheEnabled = Cache && Cache->Stats.BudgetBytes > 0;
    FBlake3Hash Revision;
    bool bLoaded = false;
    if (bCacheEnabled)
    {
        // Native declarations and wrapper bodies are fixed for this registered engine.
        Revision = FBlake3::HashBuffer(SourceBytes.Get(), SourceBytes.Length());
        const int32 PriorMessages = S.Messages.Num();
        const auto Load = Cache->TryLoad(*S.Module, Label, Revision);
        bLoaded = Load == FAngelIOBytecodeCache::ELoad::Hit;
        if (Load == FAngelIOBytecodeCache::ELoad::Invalid)
        {
            S.Module->Discard();
            if (!S.CreateWorldModule()) return false;
            S.Messages.SetNum(PriorMessages);
        }
    }
    if (!bLoaded)
    {
        if (Cache) ++Cache->Stats.SourceBuilds;
        FTCHARToUTF8 WrapperBytes(*WrapperSource);
        if (WrapperBytes.Length() && S.Module->AddScriptSection("AngelIO API", WrapperBytes.Get(), WrapperBytes.Length()) < 0)
            return S.Fail(TEXT("Could not register external script wrappers"));
        S.AddSignalBindings({SourceBytes.Get(), static_cast<size_t>(SourceBytes.Length())});
        if (S.Module->AddScriptSection(TCHAR_TO_UTF8(*Label), SourceBytes.Get(), SourceBytes.Length()) < 0)
            return S.Fail(TEXT("Could not add mission source"));
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
        const int Build = FAngelIOStartupProbe::Build(*S.Module, World, Label, Source, WrapperSource);
#else
        const int Build = S.Module->Build();
#endif
        if (Build < 0) { S.Status = EAngelIOState::Failed; return false; }
    }
    if (!S.Inspect())
    {
        S.Status = EAngelIOState::Failed;
        return false;
    }
    // Preserve source diagnostics on subsequent compiles of scripts that produced warnings.
    if (bCacheEnabled && !bLoaded && S.Messages.IsEmpty())
    {
        Cache->Store(*S.Module, Label, Revision);
        S.Messages.Reset(); // An optional serialization failure is not a source diagnostic.
    }
    S.Status = EAngelIOState::Compiled;
#if WITH_EDITOR
    S.ReadMapTargets({SourceBytes.Get(), static_cast<size_t>(SourceBytes.Length())}, Bindings.MapLookups);
    S.ReadPortDefaults({SourceBytes.Get(), static_cast<size_t>(SourceBytes.Length())});
#endif
    ANGELIO_BENCHMARK_MARK(CompileEnd);
    return Compiling == Impl;
}

bool FAngelIOProgram::Start(const TMap<FString, FAngelIOValue>& Inputs, UWorld* World, UActorComponent* Component)
{
    const auto Running = Impl;
    auto& S = *Running;
    if (S.Status != EAngelIOState::Compiled) return false;
    if (WorldOwner)
    {
        if (!World) World = WorldOwner->GetWorld();
        if (WorldOwner->GetWorld() != World || (Component && Component->GetWorld() != World))
            return S.Fail(TEXT("Mission compiled for a different world"));
    }
    if (World && World->GetNetMode() == NM_Client) return S.Fail(TEXT("Mission requires authority"));
    S.Scope.World = World;
    S.Scope.Component = Component;
    S.Scope.InstanceId = FGuid::NewGuid();
    S.Context = S.Engine->CreateContext();
    if (!S.Context) return S.Fail(TEXT("Could not create execution context"));
    S.Context->SetUserData(&S);
    if (S.Context->SetLineCallback(asFUNCTION(FImpl::Line), &S, asCALL_CDECL) < 0 ||
        S.Module->ResetGlobalVars(S.Context) < 0)
        return S.Fail(TEXT("Failed to initialize script globals"));
    if (Running != Impl) return false;
    S.Status = EAngelIOState::Running;
    for (const auto& Entry : Inputs)
    {
        if (!SetInput(Entry.Key, Entry.Value))
            return S.Fail(TEXT("Unknown input or incorrect input type: ") + Entry.Key);
    }
    if (S.Context->Prepare(S.Module->GetFunctionByDecl("void Run()")) < 0)
        return S.Fail(TEXT("Could not prepare Run"));
    const bool bStarted = S.Execute();
    ANGELIO_BENCHMARK_MARK(StartEnd);
    return bStarted && Running == Impl;
}

bool FAngelIOProgram::SetInput(const FString& Name, const FAngelIOValue& Value)
{
    const auto& S = *Impl;
    if (S.Status != EAngelIOState::Waiting && S.Status != EAngelIOState::Running) return false;
    const auto* Port = S.FindPort(Name, true);
    if (!Port || Port->Type != Value.Type) return false;
    void* Ptr = S.Module->GetAddressOfGlobalVar(Port->Index);
    switch (Value.Type)
    {
        case EAngelIOType::Integer: *static_cast<int32*>(Ptr) = Value.Integer; break;
        case EAngelIOType::Float: *static_cast<float*>(Ptr) = Value.Float; break;
        case EAngelIOType::Boolean: *static_cast<asBYTE*>(Ptr) = Value.Boolean; break;
    }
    return true;
}

bool FAngelIOProgram::GetOutput(const FString& Name, FAngelIOValue& Value) const
{
    const auto& S = *Impl;
    if (S.Status != EAngelIOState::Waiting && S.Status != EAngelIOState::Finished) return false;
    const auto* Port = S.FindPort(Name, false);
    if (!Port) return false;
    void* Ptr = S.Module->GetAddressOfGlobalVar(Port->Index);
    Value = {};
    Value.Type = Port->Type;
    switch (Value.Type)
    {
        case EAngelIOType::Integer: Value.Integer = *static_cast<int32*>(Ptr); break;
        case EAngelIOType::Float: Value.Float = *static_cast<float*>(Ptr); break;
        case EAngelIOType::Boolean: Value.Boolean = *static_cast<asBYTE*>(Ptr) != 0; break;
    }
    return true;
}

bool FAngelIOProgram::Signal(const FString& Name, int32 Value)
{
    const auto Running = Impl;
    return Running->Advance(&Name, Value) && Running == Impl;
}

bool FAngelIOProgram::IsWaitingOnOperation() const
{
    return Impl->NeedsTick();
}

bool FAngelIOProgram::ResumeCompletedOperation()
{
    const auto Running = Impl;
    return Running->Advance(nullptr, 0) && Running == Impl;
}

bool FAngelIOProgram::FImpl::Advance(const FString* Signal, int32 Value)
{
    if (Status != EAngelIOState::Waiting) return false;
    if (Signal && EndSignals.Contains(*Signal))
    {
        Status = EAngelIOState::Finished;
        Children.Reset();
        return true;
    }
    // Snapshot recipients before resuming their parent. New branches never receive the triggering signal twice.
    const int32 Existing = Children.Num();
    bool bChanged = false;
    if (Signal && !Scope.Pending && Waiting.Equals(*Signal, ESearchCase::CaseSensitive))
    {
        Waiting.Reset();
        Payload = Value;
        if (!Execute()) return false;
        bChanged = true;
    }
    else if (!Signal && Scope.Pending) bChanged = ResumeOperation();
    if (Status != EAngelIOState::Waiting) return bChanged;
    for (int32 I = 0; I < Existing; ++I)
    {
        const auto Child = Children[I];
        bChanged |= Child->Advance(Signal, Value);
        if (Child->Status == EAngelIOState::Failed)
            return Fail(Child->BranchName + TEXT(": ") + Child->Messages.Last().Message, Child->Messages.Last().Line);
    }
    if (bChanged) Children.RemoveAll([](const auto& Child){ return Child->Status == EAngelIOState::Finished; });
    return bChanged;
}

bool FAngelIOProgram::FImpl::ResumeOperation()
{
    if (Scope.Condition) Scope.Condition->Poll();
    const bool bComplete = Scope.Pending->IsComplete();
    if (!bComplete && (!Scope.Deadline.IsSet() || !Scope.World.IsValid() ||
        Scope.World->GetTimeSeconds() < *Scope.Deadline)) return false;
    *Scope.WaitOutcome = bComplete ? EAngelIOWaitOutcome::Completed : EAngelIOWaitOutcome::TimedOut;
    if (Scope.Condition && !bComplete) Scope.Condition->Timeout();
    if (auto Event = Scope.Pending->AddEvent(TEXT("resume")))
    {
        Event->SetObjectField(TEXT("waiter"), Scope.WaitSite);
        if (!bComplete) Event->SetStringField(TEXT("outcome"), TEXT("TimedOut"));
    }
    Scope.WaitOutcome.Reset();
    Scope.Deadline.Reset();
    Scope.WaitSite.Reset();
    Scope.Pending.Reset();
    Scope.Condition.Reset();
    Waiting.Reset();
    return Execute();
}

const TArray<FString>& FAngelIOProgram::ExternalDeclarations() const { return Impl->ExternalAPI; }
const TArray<FAngelIOPort>& FAngelIOProgram::Ports() const { return Impl->PublicPorts; }
const TMap<int32, FString>& FAngelIOProgram::Signals(bool bIncoming) const { return bIncoming ? Impl->Inputs : Impl->Outputs; }
const TArray<FAngelIODiagnostic>& FAngelIOProgram::Diagnostics() const { return Impl->Messages; }
TArray<FAngelIOEvent> FAngelIOProgram::TakeEvents() { return MoveTemp(Impl->Events); }
EAngelIOState FAngelIOProgram::State() const { return Impl->Status; }
FString FAngelIOProgram::WaitingFor() const { return Impl->Waiting; }

#if WITH_EDITOR
const TArray<FAngelIOLiteralTarget>& FAngelIOProgram::LiteralTargets() const { return Impl->LiteralTargets; }

FAngelIOMissionInspection FAngelIOProgram::InspectMission() const
{
    FAngelIOMissionInspection Info;
    Info.State = Impl->Status;
    Info.Script = Impl->ScriptSection;
    Info.Instance = Impl->Scope.InstanceId;
    Info.RestoredStage = Impl->RestoredStage;
    Info.RestoredVersion = Impl->RestoredVersion;
    auto InspectChildren = [&](auto&& Visit, const FImpl& Parent) -> void
    {
        for (const auto& Child : Parent.Children)
        {
            Info.BranchWaits.Add(FString::Printf(TEXT("%s · %s:%d · %s"), *Child->BranchName, *Child->ScriptSection, Child->Scope.WaitLine, *Child->Waiting));
            Visit(Visit, *Child);
        }
    };
    InspectChildren(InspectChildren, *Impl);
    if (Info.State != EAngelIOState::Waiting) return Info;
    Info.Waiting = Impl->Waiting;
    if (Impl->Scope.Pending) Info.Operation = Impl->Scope.Pending->GetOperationId();
    Info.Line = Impl->Scope.WaitLine;
    return Info;
}
#endif
