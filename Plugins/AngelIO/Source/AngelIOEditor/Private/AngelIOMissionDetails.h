#pragma once
#include "IDetailCustomization.h"

class FAngelIOMissionDetails : public IDetailCustomization
{
public:
    virtual void CustomizeDetails(IDetailLayoutBuilder& Layout) override;
};
