#include "BonsoirUnrealCommands.h"

#define LOCTEXT_NAMESPACE "FBonsoirUnrealModule"

void FBonsoirUnrealCommands::RegisterCommands() {
  UI_COMMAND(ImportZoneAction, "Import ROSE Zone",
             "Import a ROSE Online .ZON file", EUserInterfaceActionType::Button,
             FInputChord());
  UI_COMMAND(ImportCharacterAction, "Import Default Character",
             "Import Default Avatar (Scott)", EUserInterfaceActionType::Button,
             FInputChord());
}

#undef LOCTEXT_NAMESPACE
