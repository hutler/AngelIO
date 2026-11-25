#pragma once

#include "CoreMinimal.h"
THIRD_PARTY_INCLUDES_START
#include "angelscript.h"
THIRD_PARTY_INCLUDES_END

class FAngelIOBytecodeStream final : public asIBinaryStream
{
public:
    explicit FAngelIOBytecodeStream(TArray<uint8>& InBytes, int32 InLimit = MAX_int32)
        : Bytes(InBytes), Limit(InLimit) {}

    int Read(void* Destination, asUINT Size) override
    {
        if (Size > static_cast<asUINT>(Bytes.Num() - Offset)) return -1;
        if (Size) FMemory::Memcpy(Destination, Bytes.GetData() + Offset, Size);
        Offset += Size;
        return 0;
    }

    int Write(const void* Source, asUINT Size) override
    {
        if (bLimitExceeded || Size > static_cast<asUINT>(Limit - Bytes.Num()))
        {
            bLimitExceeded = true;
            return -1;
        }
        Bytes.Append(static_cast<const uint8*>(Source), Size);
        return 0;
    }

    bool bLimitExceeded = false;

private:
    TArray<uint8>& Bytes;
    const int32 Limit;
    int32 Offset = 0;
};
