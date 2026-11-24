#include "AngelIOProgram.h"
#include "AngelIOCheckpointSubsystem.h"
#include "AngelIOScriptAPI.h"
#include "AngelIOTraceSubsystem.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Features/IModularFeatures.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include <string_view>
#include <algorithm>

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
        TSharedPtr<FAngelIOOperation> Pending;
        TSharedPtr<FConditionWait> Condition;
        TSharedPtr<EAngelIOWaitOutcome> WaitOutcome;
        TOptional<double> Deadline;
        TSharedPtr<FJsonObject> WaitSite;
        FString Label;
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

    TSharedRef<FJsonObject> CaptureSite()
    {
        auto Site = MakeShared<FJsonObject>();
        auto* Context = asGetActiveContext();
        auto* Scope = Context ? static_cast<FExecutionScope*>(Context->GetUserData(ExecutionKey)) : nullptr;
        Site->SetStringField(TEXT("kind"), Scope ? TEXT("script") : TEXT("native"));
        if (!Scope) return Site;
        Site->SetStringField(TEXT("instanceId"), Scope->InstanceId.ToString(EGuidFormats::DigitsWithHyphensLower));
        const char* Section = nullptr;
        asUINT Frame = 0;
        int Line = Context->GetLineNumber(Frame, nullptr, &Section);
        while (Section && FCStringAnsi::Strcmp(Section, "AngelIO API") == 0 && Frame + 1 < Context->GetCallstackSize())
            Line = Context->GetLineNumber(++Frame, nullptr, &Section);
        Site->SetNumberField(TEXT("line"), Line);
        if (Section) Site->SetStringField(TEXT("script"), UTF8_TO_TCHAR(Section));
        if (auto* Function = Context->GetFunction(Frame))
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
    asGetActiveContext()->Suspend();
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

struct FAngelIOProgram::FImpl
{
    asIScriptEngine* Engine = nullptr;
    asIScriptModule* Module = nullptr;
    asIScriptContext* Context = nullptr;
    TArray<FAngelIODiagnostic> Messages;
    TArray<FAngelIOPort> PublicPorts;
    TMap<int32, FString> Inputs, Outputs;
    TArray<FAngelIOEvent> Events;
    FExecutionScope Scope;
    FString ScriptSection;
#if WITH_EDITOR
    TArray<FAngelIOLiteralTarget> LiteralTargets;

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
        for (asUINT I = 0; I < Module->GetFunctionCount(); ++I) ExcludeShadow(Module->GetFunctionByIndex(I));
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

    ~FImpl()
    {
        if (Scope.WaitOutcome) *Scope.WaitOutcome = EAngelIOWaitOutcome::Detached;
        if (Scope.Pending && Scope.WaitSite)
            if (auto Event = Scope.Pending->AddEvent(TEXT("detach")))
                Event->SetObjectField(TEXT("waiter"), Scope.WaitSite);
        if (Scope.Condition) Scope.Condition->Detach();
        if (Context) Context->Release();
        if (Engine) Engine->ShutDownAndRelease();
    }

    bool Fail(FString Message, int32 Line = 0, int32 Column = 0)
    {
        Messages.Add({TEXT("error"), MoveTemp(Message), Line, Column});
        Status = EAngelIOState::Failed;
        Waiting.Reset();
        Events.Reset();
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
            Ctx->Suspend();
        }
        else
        {
            Self.Events.Add({*Name, static_cast<int32>(Call->GetArgDWord(1))});
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
        Self->Events.Add({UTF8_TO_TCHAR(Input.c_str()), static_cast<int32>(Call->GetArgDWord(2)), TargetName});
    }
    static void Emit(asIScriptGeneric* Call) { SignalCall(Call, false); }
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
        const auto* Self = static_cast<FImpl*>(Context->GetUserData());
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
        if (bSave)
        {
            const auto& Stage = *static_cast<const std::string*>(Call->GetArgObject(0));
            if (Stage.empty() || Stage.find('\0') != std::string::npos)
            {
                Context->SetException("Checkpoint stage must be non-empty and contain no null characters");
                return;
            }
            Store->Save(*Component, Self->ScriptSection, Version, FUtf8String(reinterpret_cast<const UTF8CHAR*>(Stage.c_str())));
        }
        else
        {
            const FUtf8String Stage = Store->Load(*Component, Self->ScriptSection, Version);
            Result->assign(reinterpret_cast<const char*>(*Stage), Stage.Len());
        }
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
                Module->AddScriptSection("AngelIO API", "void WaitFor(InputSignal s) { AngelIOInternal::WaitFor(s); }");
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
            Status = EAngelIOState::Finished;
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
};

FAngelIOProgram::FAngelIOProgram() : Impl(MakeUnique<FImpl>()) {}
FAngelIOProgram::~FAngelIOProgram() = default;

void FAngelIOProgram::DetachComponent()
{
    Impl->Scope.Component.Reset();
}

bool FAngelIOProgram::CompileFile(const FString& RelativeScript)
{
    const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("AngelIO"));
    FString Path = FPaths::ConvertRelativePathToFull(Root, RelativeScript);
    FString Source;
    if (!FPaths::IsRelative(RelativeScript) || !FPaths::CollapseRelativeDirectories(Path) ||
        !FPaths::IsUnderDirectory(Path, Root) || !FPaths::GetExtension(Path).Equals(TEXT("as"), ESearchCase::IgnoreCase) ||
        !FFileHelper::LoadFileToString(Source, *Path))
    {
        Impl = MakeUnique<FImpl>();
        return Impl->Fail(TEXT("Cannot read .as under Content/AngelIO: ") + RelativeScript);
    }
    return Compile(RelativeScript, Source);
}

bool FAngelIOProgram::Compile(const FString& Label, const FString& Source)
{
    Impl = MakeUnique<FImpl>();
    auto& S = *Impl;
    S.Engine = asCreateScriptEngine();
    S.ScriptSection = Label;
#if WITH_EDITOR
    TArray<FAngelIOMapLookup> MapLookups{{TEXT("SendTo"), nullptr, false}};
#endif
    if (!S.Engine) return S.Fail(TEXT("Could not create AngelScript engine"));
    RegisterStdString(S.Engine);
    if (S.Engine->SetMessageCallback(asFUNCTION(FImpl::CompilerMessage), &S, asCALL_CDECL) < 0 ||
        S.Engine->SetEngineProperty(asEP_INIT_GLOBAL_VARS_AFTER_BUILD, false) < 0 ||
        S.Engine->SetDefaultNamespace("AngelIOInternal") < 0 ||
        S.Engine->RegisterGlobalFunction("void WaitFor(const ?&in)", asFUNCTION(FImpl::Wait), asCALL_GENERIC) < 0 ||
        S.Engine->RegisterGlobalFunction("void Emit(const ?&in, int)", asFUNCTION(FImpl::Emit), asCALL_GENERIC) < 0 ||
        S.Engine->SetDefaultNamespace("") < 0 ||
        S.Engine->RegisterGlobalFunction("void SendTo(const string &in target, const string &in input, int value)", asFUNCTION(FImpl::SendTo), asCALL_GENERIC) < 0 ||
        S.Engine->RegisterGlobalFunction("string LoadCheckpoint(int version = 1)", asFUNCTION(FImpl::LoadCheckpoint), asCALL_GENERIC) < 0 ||
        S.Engine->RegisterGlobalFunction("void SaveCheckpoint(const string &in stage, int version = 1)", asFUNCTION(FImpl::SaveCheckpoint), asCALL_GENERIC) < 0 ||
        S.Engine->RegisterGlobalFunction("int SignalValue()", asFUNCTION(FImpl::SignalValue), asCALL_GENERIC) < 0)
        return S.Fail(TEXT("Could not register AngelIO script API"));
    auto& Engine = *S.Engine;
    const auto FirstFunction = Engine.GetGlobalFunctionCount();
    const auto FirstType = Engine.GetObjectTypeCount();
    const auto FirstEnum = Engine.GetEnumCount();
    TArray<FAngelIOScriptWrapper> Wrappers;
    for (auto* Provider : IModularFeatures::Get().GetModularFeatureImplementations<IAngelIOScriptAPI>(
        IAngelIOScriptAPI::FeatureName()))
    {
        if (!Provider->RegisterBindings(Engine) || Engine.SetDefaultNamespace("") < 0)
            return S.Fail(TEXT("Could not register external AngelIO script bindings"));
        Provider->GetScriptWrappers(Wrappers);
#if WITH_EDITOR
        Provider->GetMapLookups(MapLookups);
#endif
    }
    for (asUINT I = FirstFunction; I < Engine.GetGlobalFunctionCount(); ++I)
    {
        const auto* Function = Engine.GetGlobalFunctionByIndex(I);
        if (FCStringAnsi::Strcmp(Function->GetNamespace(), "AngelIOInternal") != 0)
            S.ExternalAPI.Add(UTF8_TO_TCHAR(Function->GetDeclaration(true, true, true)));
    }
    for (asUINT I = FirstType; I < Engine.GetObjectTypeCount(); ++I)
    {
        auto* Type = Engine.GetObjectTypeByIndex(I);
        S.ExternalAPI.Add(UTF8_TO_TCHAR(Engine.GetTypeDeclaration(Type->GetTypeId(), true)));
        for (asUINT M = 0; M < Type->GetMethodCount(); ++M)
            S.ExternalAPI.Add(UTF8_TO_TCHAR(Type->GetMethodByIndex(M)->GetDeclaration(true, true, true)));
    }
    for (asUINT I = FirstEnum; I < Engine.GetEnumCount(); ++I)
    {
        auto* Type = Engine.GetEnumByIndex(I);
        TArray<FString> Names;
        for (asUINT N = 0; N < Type->GetEnumValueCount(); ++N)
        {
            asINT64 Value;
            Names.Add(UTF8_TO_TCHAR(Type->GetEnumValueByIndex(N, &Value)));
        }
        S.ExternalAPI.Add(FString::Printf(TEXT("enum %s { %s }"),
            UTF8_TO_TCHAR(Engine.GetTypeDeclaration(Type->GetTypeId(), true)), *FString::Join(Names, TEXT(", "))));
    }
    S.Module = S.Engine->GetModule("level", asGM_ALWAYS_CREATE);
    FString WrapperSource;
    for (const auto& Wrapper : Wrappers)
    {
        WrapperSource += Wrapper.Declaration + TEXT(" { ") + Wrapper.Body + TEXT(" }\n");
        S.ExternalAPI.Add(Wrapper.Declaration);
    }
    FTCHARToUTF8 WrapperBytes(*WrapperSource);
    if (WrapperBytes.Length() && S.Module->AddScriptSection("AngelIO API", WrapperBytes.Get(), WrapperBytes.Length()) < 0)
        return S.Fail(TEXT("Could not register external script wrappers"));
    FTCHARToUTF8 SourceBytes(*Source);
    S.AddSignalBindings({SourceBytes.Get(), static_cast<size_t>(SourceBytes.Length())});
    if (S.Module->AddScriptSection(TCHAR_TO_UTF8(*Label), SourceBytes.Get(), SourceBytes.Length()) < 0 ||
        S.Module->Build() < 0 || !S.Inspect())
    {
        S.Status = EAngelIOState::Failed;
        return false;
    }
    S.Status = EAngelIOState::Compiled;
#if WITH_EDITOR
    S.ReadMapTargets({SourceBytes.Get(), static_cast<size_t>(SourceBytes.Length())}, MapLookups);
#endif
    return true;
}

bool FAngelIOProgram::Start(const TMap<FString, FAngelIOValue>& Inputs, UWorld* World, UActorComponent* Component)
{
    auto& S = *Impl;
    if (S.Status != EAngelIOState::Compiled) return false;
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
    S.Status = EAngelIOState::Running;
    for (const auto& Entry : Inputs)
    {
        if (!SetInput(Entry.Key, Entry.Value))
            return S.Fail(TEXT("Unknown input or incorrect input type: ") + Entry.Key);
    }
    if (S.Context->Prepare(S.Module->GetFunctionByDecl("void Run()")) < 0)
        return S.Fail(TEXT("Could not prepare Run"));
    return S.Execute();
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
    auto& S = *Impl;
    if (S.Status != EAngelIOState::Waiting || S.Scope.Pending || !S.Waiting.Equals(Name, ESearchCase::CaseSensitive)) return false;
    S.Waiting.Reset();
    S.Payload = Value;
    return S.Execute();
}

bool FAngelIOProgram::IsWaitingOnOperation() const
{
    return Impl->Status == EAngelIOState::Waiting && Impl->Scope.Pending.IsValid();
}

bool FAngelIOProgram::ResumeCompletedOperation()
{
    if (!IsWaitingOnOperation()) return false;
    auto& Scope = Impl->Scope;
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
    Impl->Waiting.Reset();
    return Impl->Execute();
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
    if (Info.State != EAngelIOState::Waiting) return Info;
    Info.Waiting = Impl->Waiting;
    if (Impl->Scope.Pending) Info.Operation = Impl->Scope.Pending->GetOperationId();
    // WaitFor has a generated wrapper. Report the first frame in the mission file.
    for (asUINT Frame = 0; Frame < Impl->Context->GetCallstackSize(); ++Frame)
    {
        const char* Section = nullptr;
        const int Line = Impl->Context->GetLineNumber(Frame, nullptr, &Section);
        if (Line > 0 && Section && Info.Script.Equals(UTF8_TO_TCHAR(Section), ESearchCase::CaseSensitive))
        {
            Info.Line = Line;
            break;
        }
    }
    return Info;
}
#endif
