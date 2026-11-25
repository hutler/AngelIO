#include "AngelIOBytecodeCache.h"
#include "AngelIOBytecodeStream.h"
#include "AngelIOSettings.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogAngelIOBytecodeCache, Log, All);

FAngelIOBytecodeCache::FAngelIOBytecodeCache(UWorld& World)
{
    int32 KiB = FMath::Max(0, GetDefault<UAngelIOSettings>()->BytecodeCacheBudgetKiB);
    if (const TCHAR* Option = World.URL.GetOption(TEXT("AngelIOBytecodeCacheKiB="), nullptr))
    {
        TCHAR* End = nullptr;
        const int64 Parsed = FCString::Strtoi64(Option, &End, 10);
        if (End != Option && *End == 0 && Parsed >= 0 && Parsed <= MAX_int32) KiB = int32(Parsed);
        else UE_LOG(LogAngelIOBytecodeCache, Warning, TEXT("Invalid AngelIOBytecodeCacheKiB, using project budget of %d KiB"), KiB);
    }
    Stats.BudgetBytes = uint64(KiB) * 1024;
}

FAngelIOBytecodeCache::FNode* FAngelIOBytecodeCache::Find(const FString& Label) const
{
    for (auto* Node = Entries.GetHead(); Node; Node = Node->GetNextNode())
        if (Node->GetValue().Label.Equals(Label, ESearchCase::CaseSensitive)) return Node;
    return nullptr;
}

void FAngelIOBytecodeCache::Remove(FNode* Node)
{
    Stats.RetainedBytes -= Node->GetValue().Charge;
    --Stats.Entries;
    Entries.RemoveNode(Node);
}

FAngelIOBytecodeCache::ELoad FAngelIOBytecodeCache::TryLoad(asIScriptModule& Module, const FString& Label,
    const FBlake3Hash& Revision)
{
    auto* Node = Find(Label);
    if (!Node || Node->GetValue().Revision != Revision) return ELoad::Miss;
    FAngelIOBytecodeStream Stream(Node->GetValue().Bytes);
    if (Module.LoadByteCode(&Stream) < 0)
    {
        Remove(Node);
        return ELoad::Invalid;
    }
    if (Node != Entries.GetHead())
    {
        Entries.RemoveNode(Node, false);
        Entries.AddHead(Node);
    }
    ++Stats.Hits;
    return ELoad::Hit;
}

void FAngelIOBytecodeCache::Store(asIScriptModule& Module, const FString& Label, const FBlake3Hash& Revision)
{
    TArray<uint8> Bytes;
    FAngelIOBytecodeStream Stream(Bytes, int32(FMath::Min<uint64>(Stats.BudgetBytes, MAX_int32)));
    const int Result = Module.SaveByteCode(&Stream, /*stripDebugInfo=*/false);
    if (Stream.bLimitExceeded) { ++Stats.Oversized; return; }
    if (Result < 0 || Bytes.IsEmpty()) return;

    auto NewNode = MakeUnique<FNode>(FEntry{});
    auto& Entry = NewNode->GetValue();
    Entry.Label = Label;
    Entry.Revision = Revision;
    Bytes.Shrink();
    Entry.Bytes = MoveTemp(Bytes);
    // Charge the allocator's usable block size, including slack, for every owned allocation.
    auto Charge = [](void* Ptr, uint64 Requested)
    {
        return Ptr ? FMath::Max<uint64>(Requested, FMemory::GetAllocSize(Ptr)) : 0;
    };
    Entry.Charge = Charge(NewNode.Get(), sizeof(FNode)) + Charge(Entry.Label.GetCharArray().GetData(), Entry.Label.GetAllocatedSize()) +
        Charge(Entry.Bytes.GetData(), Entry.Bytes.GetAllocatedSize());
    if (Entry.Charge > Stats.BudgetBytes) { ++Stats.Oversized; return; }
    if (auto* Previous = Find(Label)) Remove(Previous);
    while (Entry.Charge > Stats.BudgetBytes - Stats.RetainedBytes)
    {
        Remove(Entries.GetTail());
        ++Stats.Evictions;
    }
    Stats.RetainedBytes += Entry.Charge;
    ++Stats.Entries;
    Entries.AddHead(NewNode.Release());
}
