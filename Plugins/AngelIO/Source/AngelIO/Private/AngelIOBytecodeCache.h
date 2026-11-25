#pragma once

#include "AngelIOBytecodeCacheStats.h"
#include "Containers/List.h"
#include "Hash/Blake3.h"

class asIScriptModule;

// One registered engine owns the bindings, generated wrappers and cache for its lifetime.
class FAngelIOBytecodeCache
{
public:
    explicit FAngelIOBytecodeCache(UWorld& World);
    enum class ELoad { Miss, Hit, Invalid };
    ELoad TryLoad(asIScriptModule& Module, const FString& Label, const FBlake3Hash& Revision);
    void Store(asIScriptModule& Module, const FString& Label, const FBlake3Hash& Revision);
    FAngelIOBytecodeCacheStats Stats;

private:
    struct FEntry
    {
        FString Label;
        FBlake3Hash Revision;
        TArray<uint8> Bytes;
        uint64 Charge = 0;
    };
    using FList = TDoubleLinkedList<FEntry>;
    using FNode = FList::TDoubleLinkedListNode;
    FNode* Find(const FString& Label) const;
    void Remove(FNode* Node);
    FList Entries; // Most recently used at the head. Removing a node releases all its storage.
};
