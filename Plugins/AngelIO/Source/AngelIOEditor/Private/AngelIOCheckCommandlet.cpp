#include "AngelIOCheckCommandlet.h"
#include "AngelIOProgram.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"

UAngelIOCheckCommandlet::UAngelIOCheckCommandlet()
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = false;
}

int32 UAngelIOCheckCommandlet::Main(const FString& Params)
{
    FString Script, Report;
    const bool bApiOnly = FParse::Param(*Params, TEXT("ApiOnly"));
    if ((!bApiOnly && !FParse::Value(*Params, TEXT("Script="), Script)) || !FParse::Value(*Params, TEXT("Report="), Report))
    {
        UE_LOG(LogTemp, Error, TEXT("Expected -Script=<relative .as> -Report=<output.json>"));
        return 2;
    }
    FAngelIOProgram Program;
    const bool bValid = bApiOnly ? Program.Compile(TEXT("API"), TEXT("void Run() {}")) : Program.CompileFile(Script);
    auto Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("script"), Script);
    Root->SetBoolField(TEXT("valid"), bValid);
    TArray<TSharedPtr<FJsonValue>> Declarations;
    for (const auto& Decl : Program.ExternalDeclarations())
        Declarations.Add(MakeShared<FJsonValueString>(Decl));
    Root->SetArrayField(TEXT("externalDeclarations"), Declarations);
    TArray<TSharedPtr<FJsonValue>> Ports, Diagnostics;
    for (const auto& P : Program.Ports())
    {
        auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("name"), P.Name);
        Json->SetStringField(TEXT("direction"), P.bInput ? TEXT("in") : TEXT("out"));
        Json->SetStringField(TEXT("type"), P.Type == EAngelIOType::Integer ? TEXT("int") :
            P.Type == EAngelIOType::Float ? TEXT("float") : TEXT("bool"));
        Ports.Add(MakeShared<FJsonValueObject>(Json));
    }
    Root->SetArrayField(TEXT("ports"), Ports);
    for (bool bIncoming : {true, false})
    {
        TArray<TSharedPtr<FJsonValue>> Names;
        for (const auto& Pair : Program.Signals(bIncoming))
            Names.Add(MakeShared<FJsonValueString>(Pair.Value));
        Root->SetArrayField(bIncoming ? TEXT("inputSignals") : TEXT("outputSignals"), Names);
    }
    for (const auto& D : Program.Diagnostics())
    {
        auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("severity"), D.Severity);
        Json->SetStringField(TEXT("message"), D.Message);
        Json->SetNumberField(TEXT("line"), D.Line);
        Json->SetNumberField(TEXT("column"), D.Column);
        Diagnostics.Add(MakeShared<FJsonValueObject>(Json));
    }
    Root->SetArrayField(TEXT("diagnostics"), Diagnostics);
    FString Text;
    auto Writer = TJsonWriterFactory<>::Create(&Text);
    if (!FJsonSerializer::Serialize(Root, Writer) ||
        !FFileHelper::SaveStringToFile(Text, *Report, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        return 2;
    return bValid ? 0 : 1;
}
