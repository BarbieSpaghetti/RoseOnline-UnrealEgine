#include "RoseZoneFactory.h"
#include "Misc/Paths.h"
#include "RoseImporter.h"
#include "RoseMapInfo.h"
#include "SRoseZoneBrowser.h"

URoseZoneFactory::URoseZoneFactory() {
  // We import ZON files, which technically spawn actors in the world rather
  // than creating a single asset. However, usually we might want to create a
  // DataAsset that holds references, or just run the import logic. For now,
  // let's claim we support it and use a placeholder UObject or run the logical
  // import.

  SupportedClass = URoseMapInfo::StaticClass();
  bCreateNew = false;
  bEditorImport = true;
  ImportPriority = DefaultImportPriority + 10;
  Formats.Add(TEXT("zon;ROSE Online Zone File"));
  Formats.Add(TEXT("stb;ROSE Online LIST_ZONE String Table"));
  UE_LOG(LogTemp, Warning,
         TEXT("[RoseZoneFactory] Constructor called/Registered"));
}

bool URoseZoneFactory::FactoryCanImport(const FString &Filename) {
  FString Ext = FPaths::GetExtension(Filename).ToLower();
  UE_LOG(LogTemp, Log,
         TEXT("[RoseZoneFactory] Checking import for: %s (Ext: %s)"), *Filename,
         *Ext);
  return Ext == TEXT("zon") || Ext == TEXT("stb");
}

UObject *URoseZoneFactory::FactoryCreateFile(UClass *InClass, UObject *InParent,
                                             FName InName, EObjectFlags Flags,
                                             const FString &Filename,
                                             const TCHAR *Parms,
                                             FFeedbackContext *Warn,
                                             bool &bOutOperationCanceled) {
  UE_LOG(LogTemp, Warning,
         TEXT("[RoseZoneFactory] FactoryCreateFile called for: %s"), *Filename);

  FString Ext = FPaths::GetExtension(Filename).ToLower();
  FString ZonPathToLoad = Filename;

  // Handle LIST_ZONE.STB selection
  if (Ext == TEXT("stb")) {
    FRoseSTB Stb;
    if (!Stb.Load(Filename)) {
      UE_LOG(LogTemp, Error, TEXT("Failed to load STB: %s"), *Filename);
      bOutOperationCanceled = true;
      return nullptr;
    }

    // Open Browser
    TSharedPtr<FZoneRow> Selected = SRoseZoneBrowser::PickZone(Stb);
    if (!Selected.IsValid()) {
      bOutOperationCanceled = true;
      return nullptr;
    }

    // Construct ZON path from selection
    // Root/3Ddata/STB/LIST_ZONE.STB -> Root/3Ddata/ + ZonPath from STB
    FString RoseRoot = FPaths::GetPath(FPaths::GetPath(Filename)); // Pop STB/
    FString ParentDir = FPaths::GetPath(Filename);
    FString GrandParent = FPaths::GetPath(
        ParentDir); // This should be 3Ddata if structure is standard

    FString RelPath = Selected->ZonPath;
    FPaths::NormalizeFilename(RelPath);
    if (RelPath.StartsWith(TEXT("3DDATA/"), ESearchCase::IgnoreCase)) {
      RelPath = RelPath.RightChop(7);
    }

    ZonPathToLoad = FPaths::Combine(GrandParent, RelPath);
    FPaths::NormalizeFilename(ZonPathToLoad);
    UE_LOG(LogTemp, Log, TEXT("Selected Zone: %s -> %s"), *Selected->Name,
           *ZonPathToLoad);
  }

  URoseImporter *Importer = NewObject<URoseImporter>();

  // Infer related files logic inside ImportZone handles the rest
  bool bResult = Importer->ImportZone(ZonPathToLoad);

  if (bResult) {
    // Create a dummy asset to return, so the Editor knows import succeeded.
    URoseMapInfo *NewAsset = NewObject<URoseMapInfo>(InParent, InName, Flags);
    NewAsset->OriginalZONPath = ZonPathToLoad;

    UE_LOG(
        LogTemp, Display,
        TEXT(
            "[RoseZoneFactory] Import Successful. Created RoseMapInfo asset."));

    return NewAsset;
  }

  bOutOperationCanceled = true;
  return nullptr;
}
