#pragma once

#include "BonsoirUnrealStyle.h"
#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"

class FBonsoirUnrealCommands : public TCommands<FBonsoirUnrealCommands> {
public:
  FBonsoirUnrealCommands()
      : TCommands<FBonsoirUnrealCommands>(
            TEXT("BonsoirUnreal"),
            NSLOCTEXT("Contexts", "BonsoirUnreal", "Bonsoir ROSE Importer"),
            NAME_None, FBonsoirUnrealStyle::GetStyleSetName()) {}

  // TCommands<> interface
  virtual void RegisterCommands() override;

public:
  TSharedPtr<FUICommandInfo> ImportZoneAction;
  TSharedPtr<FUICommandInfo> ImportCharacterAction;
};
