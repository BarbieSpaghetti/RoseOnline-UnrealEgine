#include "RoseZoneFactory.h"
#include "Misc/Paths.h"
#include "RoseImporter.h"
#include "RoseMapInfo.h"

URoseZoneFactory::URoseZoneFactory() {
  // We import ZON files, which technically spawn actors in the world rather
  // than creating a single asset. However, usually we might want to create a
  // DataAsset that holds references, or just run the import logic. For now,
  // let's claim we support it and use a placeholder UObject or run the logical
  // import.

  SupportedClass = URoseMapInfo::StaticClass();
  bCreateNew = false;
  bEditorImport = true;
  Formats.Add(TEXT("zon;ROSE Online Zone File"));
  UE_LOG(LogTemp, Warning,
         TEXT("[RoseZoneFactory] Constructor called/Registered"));
}

bool URoseZoneFactory::FactoryCanImport(const FString &Filename) {
  return FPaths::GetExtension(Filename).ToLower() == TEXT("zon");
}

UObject *URoseZoneFactory::FactoryCreateFile(UClass *InClass, UObject *InParent,
                                             FName InName, EObjectFlags Flags,
                                             const FString &Filename,
                                             const TCHAR *Parms,
                                             FFeedbackContext *Warn,
                                             bool &bOutOperationCanceled) {
  UE_LOG(LogTemp, Warning,
         TEXT("[RoseZoneFactory] FactoryCreateFile called for: %s"), *Filename);
  URoseImporter *Importer = NewObject<URoseImporter>();

  // Infer related files
  // ZON file: zoneNAME.zon
  // HIM file: zoneNAME.him (usually same folder)
  // IFO file: zoneNAME.ifo (usually same folder)

  bool bResult = Importer->ImportZone(Filename);

  if (bResult) {
    // Create a dummy asset to return, so the Editor knows import succeeded.
    URoseMapInfo *NewAsset = NewObject<URoseMapInfo>(InParent, InName, Flags);
    NewAsset->OriginalZONPath = Filename;

    UE_LOG(
        LogTemp, Display,
        TEXT(
            "[RoseZoneFactory] Import Successful. Created RoseMapInfo asset."));

    return NewAsset;
  }

  bOutOperationCanceled = true;
  return nullptr;
}
