#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FToolBarBuilder;
class FMenuBuilder;

class FBonsoirUnrealModule : public IModuleInterface {
public:
  /** IModuleInterface implementation */
  virtual void StartupModule() override;
  virtual void ShutdownModule() override;

private:
  void RegisterMenus();
  void OnImportZoneClicked();

private:
  TSharedPtr<class FUICommandList> PluginCommands;
};
