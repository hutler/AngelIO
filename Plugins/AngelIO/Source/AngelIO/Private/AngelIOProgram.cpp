#include "AngelIOProgram.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include <string_view>

THIRD_PARTY_INCLUDES_START
#include "angelscript.h"
#include "scriptstdstring.h"
THIRD_PARTY_INCLUDES_END

struct FAngelIOProgram::FImpl
{
    asIScriptEngine* Engine = nullptr;
    asIScriptModule* Module = nullptr;
    asIScriptContext* Context = nullptr;
    TArray<FAngelIODiagnostic> Messages;
    TArray<FAngelIOPort> PublicPorts;
    TMap<int32, FString> Inputs, Outputs;
    TArray<FAngelIOEvent> Events;
    EAngelIOState Status = EAngelIOState::Empty;
    FString Waiting;
    int32 Payload = 0;
    int32 Steps = 0;
    int32 BudgetLine = 0;
    bool bInRun = false;

    ~FImpl()
    {
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
        const int Code = Context->Execute();
        bInRun = false;
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
    if (!S.Engine) return S.Fail(TEXT("Could not create AngelScript engine"));
    RegisterStdString(S.Engine);
    if (S.Engine->SetMessageCallback(asFUNCTION(FImpl::CompilerMessage), &S, asCALL_CDECL) < 0 ||
        S.Engine->SetEngineProperty(asEP_INIT_GLOBAL_VARS_AFTER_BUILD, false) < 0 ||
        S.Engine->SetDefaultNamespace("AngelIOInternal") < 0 ||
        S.Engine->RegisterGlobalFunction("void WaitFor(const ?&in)", asFUNCTION(FImpl::Wait), asCALL_GENERIC) < 0 ||
        S.Engine->RegisterGlobalFunction("void Emit(const ?&in, int)", asFUNCTION(FImpl::Emit), asCALL_GENERIC) < 0 ||
        S.Engine->SetDefaultNamespace("") < 0 ||
        S.Engine->RegisterGlobalFunction("void SendTo(const string &in target, const string &in input, int value)", asFUNCTION(FImpl::SendTo), asCALL_GENERIC) < 0 ||
        S.Engine->RegisterGlobalFunction("int SignalValue()", asFUNCTION(FImpl::SignalValue), asCALL_GENERIC) < 0)
        return S.Fail(TEXT("Could not register AngelIO script API"));
    S.Module = S.Engine->GetModule("level", asGM_ALWAYS_CREATE);
    FTCHARToUTF8 SourceBytes(*Source);
    S.AddSignalBindings({SourceBytes.Get(), static_cast<size_t>(SourceBytes.Length())});
    if (S.Module->AddScriptSection(TCHAR_TO_UTF8(*Label), SourceBytes.Get(), SourceBytes.Length()) < 0 ||
        S.Module->Build() < 0 || !S.Inspect())
    {
        S.Status = EAngelIOState::Failed;
        return false;
    }
    S.Status = EAngelIOState::Compiled;
    return true;
}

bool FAngelIOProgram::Start(const TMap<FString, FAngelIOValue>& Inputs)
{
    auto& S = *Impl;
    if (S.Status != EAngelIOState::Compiled) return false;
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
    if (S.Status != EAngelIOState::Waiting || !S.Waiting.Equals(Name, ESearchCase::CaseSensitive)) return false;
    S.Waiting.Reset();
    S.Payload = Value;
    return S.Execute();
}

const TArray<FAngelIOPort>& FAngelIOProgram::Ports() const { return Impl->PublicPorts; }
const TMap<int32, FString>& FAngelIOProgram::Signals(bool bIncoming) const { return bIncoming ? Impl->Inputs : Impl->Outputs; }
const TArray<FAngelIODiagnostic>& FAngelIOProgram::Diagnostics() const { return Impl->Messages; }
TArray<FAngelIOEvent> FAngelIOProgram::TakeEvents() { return MoveTemp(Impl->Events); }
EAngelIOState FAngelIOProgram::State() const { return Impl->Status; }
FString FAngelIOProgram::WaitingFor() const { return Impl->Waiting; }
