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

#include "RoseFormats.h"
#include "SRoseZoneBrowser.h"

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

    const FString FileTypes =
        TEXT("All Supported Files|*.zon;*.stb|ROSE Zone Files "
             "(*.zon)|*.zon|ROSE Zone List (*.stb)|*.stb");
    const FString DefaultPath = FPaths::ProjectContentDir();

    if (DesktopPlatform->OpenFileDialog(ParentWindowHandle,
                                        TEXT("Select ROSE Zone File to Import"),
                                        DefaultPath, TEXT(""), FileTypes,
                                        EFileDialogFlags::None, OutFiles)) {
      if (OutFiles.Num() > 0) {
        FString FilePath = OutFiles[0];
        FString Ext = FPaths::GetExtension(FilePath).ToLower();
        FString ZonePathToImport = FilePath;

        // Handle STB
        if (Ext == TEXT("stb")) {
          FRoseSTB Stb;
          if (Stb.Load(FilePath)) {
            TSharedPtr<FZoneRow> Selected = SRoseZoneBrowser::PickZone(Stb);
            if (Selected.IsValid()) {
              // Resolve Path logic (same as Factory)
              FString ParentDir = FPaths::GetPath(FilePath);
              FString GrandParent = FPaths::GetPath(ParentDir);

              FString RelPath = Selected->ZonPath;
              FPaths::NormalizeFilename(RelPath);
              if (RelPath.StartsWith(TEXT("3DDATA/"),
                                     ESearchCase::IgnoreCase)) {
                RelPath = RelPath.RightChop(7); // Remove "3DDATA/"
              }

              ZonePathToImport = FPaths::Combine(GrandParent, RelPath);
              FPaths::NormalizeFilename(ZonePathToImport);
            } else {
              return; // Canceled
            }
          } else {
            FMessageDialog::Open(EAppMsgType::Ok,
                                 FText::FromString("Failed to load STB file."));
            return;
          }
        }

        // Import
        URoseImporter *Importer = NewObject<URoseImporter>();
        if (Importer) {
          bool bSuccess = Importer->ImportZone(ZonePathToImport);

          if (bSuccess) {
            FMessageDialog::Open(
                EAppMsgType::Ok,
                FText::Format(
                    LOCTEXT("ImportSuccess", "Successfully imported zone: {0}"),
                    FText::FromString(
                        FPaths::GetBaseFilename(ZonePathToImport))));
          } else {
            UE_LOG(LogTemp, Error,
                   TEXT("[BonsoirUnreal] Failed to import zone from path: %s"),
                   *ZonePathToImport);
            FMessageDialog::Open(
                EAppMsgType::Ok,
                FText::Format(LOCTEXT("ImportFailed",
                                      "Failed to import zone:\n{0}\nCheck "
                                      "Output Log for details."),
                              FText::FromString(ZonePathToImport)));
          }
        }
      }
    }
  }
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBonsoirUnrealModule, BonsoirUnreal)
