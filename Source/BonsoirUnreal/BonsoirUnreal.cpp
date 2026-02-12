#include "BonsoirUnreal.h"
#include "BonsoirUnrealCommands.h"
#include "BonsoirUnrealStyle.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "Framework/Commands/Commands.h"
#include "IDesktopPlatform.h"
#include "Misc/MessageDialog.h"
#include "RoseImporter.h"
#include "ToolMenus.h"


#define LOCTEXT_NAMESPACE "FBonsoirUnrealModule"

void FBonsoirUnrealModule::StartupModule() {
  // Initialize style and commands
  FBonsoirUnrealStyle::Initialize();
  FBonsoirUnrealStyle::ReloadTextures();
  FBonsoirUnrealCommands::Register();

  // Create command list
  PluginCommands = MakeShareable(new FUICommandList);

  // Map the import action
  PluginCommands->MapAction(
      FBonsoirUnrealCommands::Get().ImportZoneAction,
      FExecuteAction::CreateRaw(this,
                                &FBonsoirUnrealModule::OnImportZoneClicked),
      FCanExecuteAction());

  // Register menus
  UToolMenus::RegisterStartupCallback(
      FSimpleMulticastDelegate::FDelegate::CreateRaw(
          this, &FBonsoirUnrealModule::RegisterMenus));
}

void FBonsoirUnrealModule::ShutdownModule() {
  UToolMenus::UnRegisterStartupCallback(this);
  UToolMenus::UnregisterOwner(this);

  FBonsoirUnrealStyle::Shutdown();
  FBonsoirUnrealCommands::Unregister();
}

void FBonsoirUnrealModule::RegisterMenus() {
  // Owner will be used for cleanup in call to UToolMenus::UnregisterOwner
  FToolMenuOwnerScoped OwnerScoped(this);

  // Extend the level editor toolbar
  {
    UToolMenu *ToolbarMenu = UToolMenus::Get()->ExtendMenu(
        "LevelEditor.LevelEditorToolBar.PlayToolBar");
    {
      FToolMenuSection &Section = ToolbarMenu->FindOrAddSection("PluginTools");
      {
        FToolMenuEntry &Entry =
            Section.AddEntry(FToolMenuEntry::InitToolBarButton(
                FBonsoirUnrealCommands::Get().ImportZoneAction));
        Entry.SetCommandList(PluginCommands);
      }
    }
  }
}

void FBonsoirUnrealModule::OnImportZoneClicked() {
  // Open file picker dialog
  IDesktopPlatform *DesktopPlatform = FDesktopPlatformModule::Get();
  if (DesktopPlatform) {
    TArray<FString> OutFiles;
    const void *ParentWindowHandle =
        FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);

    const FString FileTypes = TEXT("ROSE Zone Files (*.zon)|*.zon");
    const FString DefaultPath = FPaths::ProjectContentDir();

    if (DesktopPlatform->OpenFileDialog(ParentWindowHandle,
                                        TEXT("Select ROSE Zone File to Import"),
                                        DefaultPath, TEXT(""), FileTypes,
                                        EFileDialogFlags::None, OutFiles)) {
      if (OutFiles.Num() > 0) {
        // Import the selected zone file
        FString ZonePath = OutFiles[0];

        URoseImporter *Importer = NewObject<URoseImporter>();
        if (Importer) {
          bool bSuccess = Importer->ImportZone(ZonePath);

          if (bSuccess) {
            FMessageDialog::Open(
                EAppMsgType::Ok,
                FText::Format(
                    LOCTEXT("ImportSuccess", "Successfully imported zone: {0}"),
                    FText::FromString(FPaths::GetBaseFilename(ZonePath))));
          } else {
            FMessageDialog::Open(
                EAppMsgType::Ok,
                FText::Format(
                    LOCTEXT("ImportFailed", "Failed to import zone: {0}"),
                    FText::FromString(FPaths::GetBaseFilename(ZonePath))));
          }
        }
      }
    }
  }
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBonsoirUnrealModule, BonsoirUnreal)
