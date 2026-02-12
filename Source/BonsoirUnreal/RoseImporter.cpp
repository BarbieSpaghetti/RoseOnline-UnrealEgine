#include "RoseImporter.h"
#include "AssetExportTask.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "BonsoirUnrealLog.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "ContentBrowserModule.h"
#include "DrawDebugHelpers.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Exporters/Exporter.h"
#include "Exporters/FbxExportOption.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Factories/TextureFactory.h"
#include "FileHelpers.h"
#include "IContentBrowserSingleton.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Internationalization/Text.h"
#include "Kismet/GameplayStatics.h"
#include "Landscape.h"
#include "LandscapeEditLayer.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"
#include "Materials/MaterialExpressionLandscapeLayerCoords.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionRotator.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/ScopedSlowTask.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "RoseFormats.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshDescription.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

bool URoseImporter::ImportZone(const FString &ZONPath) {
  FScopedSlowTask SlowTask(3.0f, NSLOCTEXT("RoseImporter", "ImportingZone",
                                           "Importing ROSE Zone..."));
  SlowTask.MakeDialog();

  SlowTask.EnterProgressFrame(
      1.0f, NSLOCTEXT("RoseImporter", "LoadingFiles", "Loading Files..."));
  FRoseZON ZON;
  if (!ZON.Load(ZONPath))
    return false;

  FString Folder = FPaths::GetPath(ZONPath);
  FPaths::NormalizeFilename(Folder);

  // ROBUST ROOT DISCOVERY: Find 3DData by walking up parents
  RoseRootPath = Folder;
  bool bFoundRoot = false;
  FString CurrentSearch = Folder;
  while (!CurrentSearch.IsEmpty()) {
    FString TestPath = FPaths::Combine(CurrentSearch, TEXT("3DData"));
    if (IFileManager::Get().DirectoryExists(*TestPath)) {
      RoseRootPath = CurrentSearch;
      bFoundRoot = true;
      break;
    }
    CurrentSearch = FPaths::GetPath(CurrentSearch);
  }

  if (!bFoundRoot) {
    // Fallback to legacy logic
    int32 Index = ZONPath.Find(TEXT("3Ddata"), ESearchCase::IgnoreCase,
                               ESearchDir::FromEnd);
    RoseRootPath =
        (Index != INDEX_NONE) ? ZONPath.Left(Index) : Folder + TEXT("/");
    UE_LOG(LogRoseImporter, Warning,
           TEXT("Root '3DData' not found by walking up. Using fallback: %s"),
           *RoseRootPath);
  }
  FPaths::NormalizeFilename(RoseRootPath);
  UE_LOG(LogRoseImporter, Log, TEXT("Final Rose Root Path: %s"), *RoseRootPath);

  // Load ZONETYPEINFO.STB for TileSet lookup
  bZoneTypeInfoLoaded = false;
  bCurrentTileSetValid = false;

  if (LoadZoneTypeInfo(RoseRootPath)) {
    // Log ZoneType from ZON file
    UE_LOG(LogRoseImporter, Log, TEXT("Zone Type: %d"), ZON.ZoneType);

    // Try to load TileSet for this zone
    if (LoadTileSetForZone(ZON.ZoneType, CurrentTileSet)) {
      bCurrentTileSetValid = true;
      UE_LOG(LogRoseImporter, Log,
             TEXT("TileSet loaded: %d brushes, ready for intelligent blending"),
             CurrentTileSet.Brushes.Num());
    } else {
      UE_LOG(LogRoseImporter, Warning,
             TEXT("TileSet not loaded - using frequency-based blending"));
    }
  } else {
    UE_LOG(LogRoseImporter, Warning,
           TEXT("ZONETYPEINFO not available - using fallback texture system"));
  }

  // Load ZSCs via LIST_ZONE
  // Zone Name is the directory name of the ZON file (e.g. JDT01)
  // But sometimes it matches the filename (e.g. JG07.ZON -> JG07)
  FString ZoneDirName = FPaths::GetBaseFilename(FPaths::GetPath(ZONPath));
  FString ZoneFileName = FPaths::GetBaseFilename(ZONPath);

  TArray<FString> Candidates;
  Candidates.Add(ZoneDirName);
  if (ZoneDirName != ZoneFileName) {
    Candidates.Add(ZoneFileName);
  }

  if (LoadZSCsFromListZone(RoseRootPath, Candidates)) {
    UE_LOG(LogRoseImporter, Log, TEXT("ZSCs loaded successfully for zone %s"),
           *ZoneDirName);
  } else {
    UE_LOG(LogRoseImporter, Warning,
           TEXT("Failed to load associated ZSCs for zone %s"), *ZoneDirName);
  }

  UWorld *World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
  if (!World)
    return false;

  TArray<FString> FoundFiles;
  IFileManager::Get().FindFiles(FoundFiles, *Folder, TEXT("*.him"));
  if (FoundFiles.Num() == 0)
    IFileManager::Get().FindFiles(FoundFiles, *Folder, TEXT("*.HIM"));

  int32 MinX = MAX_int32, MinY = MAX_int32, MaxX = MIN_int32, MaxY = MIN_int32;
  struct FTileInfo {
    int32 X, Y;
    FString BaseName;
  };
  TArray<FTileInfo> TilesToLoad;
  for (const FString &File : FoundFiles) {
    FString Base = FPaths::GetBaseFilename(File), L, R;
    if (Base.Split(TEXT("_"), &L, &R)) {
      int32 X = FCString::Atoi(*L), Y = FCString::Atoi(*R);
      if (L.IsNumeric() && R.IsNumeric()) {
        if (X < MinX)
          MinX = X;
        if (X > MaxX)
          MaxX = X;
        if (Y < MinY)
          MinY = Y;
        if (Y > MaxY)
          MaxY = Y;
        TilesToLoad.Add({X, Y, Base});
      }
    }
  }
  if (TilesToLoad.Num() == 0)
    return false;

  // Clear previous state
  GlobalHISMMap.Empty();
  ProcessedMaterialPaths.Empty();

  // Find and destroy any existing ZoneObjects actor with the same name
  FString ActorName = TEXT("ZoneObjects_") + ZoneDirName;
  for (TActorIterator<AActor> It(World); It; ++It) {
    AActor *ExistingActor = *It;
    if (ExistingActor && ExistingActor->GetName() == ActorName) {
      UE_LOG(LogRoseImporter, Warning,
             TEXT("[Import] Destroying existing actor: %s"), *ActorName);
      World->DestroyActor(ExistingActor);
      break;
    }
  }

  // Also destroy the old ZoneObjectsActor if it exists
  if (ZoneObjectsActor) {
    if (World->ContainsActor(ZoneObjectsActor)) {
      World->DestroyActor(ZoneObjectsActor);
    }
  }
  FActorSpawnParameters SpawnParams;
  // Don't specify name - let Unreal generate unique name automatically
  SpawnParams.SpawnCollisionHandlingOverride =
      ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

  ZoneObjectsActor =
      World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector,
                                FRotator::ZeroRotator, SpawnParams);

  if (!ZoneObjectsActor) {
    UE_LOG(LogRoseImporter, Error,
           TEXT("Failed to spawn ZoneObjectsActor. Aborting import."));
    return false;
  }

  // CRITICAL FIX: AActor has no RootComponent by default. We must add one.
  USceneComponent *RootComp =
      NewObject<USceneComponent>(ZoneObjectsActor, TEXT("ZoneRoot"));
  RootComp->SetMobility(EComponentMobility::Static); // Fix for HISM Attachment
  ZoneObjectsActor->SetRootComponent(RootComp);
  RootComp->RegisterComponent();

  ZoneObjectsActor->SetActorLabel(TEXT("ZoneObjects_") + ZoneDirName);

#if WITH_EDITOR
  ZoneObjectsActor->SetFolderPath(FName(*(TEXT("Rose/") + ZoneDirName)));
#endif

  // PHASE 1: COLLECT ALL TILES
  TArray<FLoadedTile> AllTiles;

  for (const auto &Tile : TilesToLoad) {
    TArray<uint8> Data;
    FString HIMPath = FPaths::Combine(Folder, Tile.BaseName + TEXT(".him"));
    if (FFileHelper::LoadFileToArray(Data, *HIMPath)) {
      FMemoryReader R(Data, true);
      FRoseArchive Ar(R);

      FLoadedTile LoadedTile;
      LoadedTile.X = Tile.X;
      LoadedTile.Y = Tile.Y;
      LoadedTile.HIM.Serialize(Ar);
      LoadedTile.TIL.Load(
          FPaths::Combine(Folder, Tile.BaseName + TEXT(".til")));

      AllTiles.Add(LoadedTile);
    }
  }

  if (AllTiles.Num() == 0) {
    return false;
  }

  UE_LOG(LogRoseImporter, Log,
         TEXT("Loaded %d tiles, creating unified landscape..."),
         AllTiles.Num());

  // PHASE 2: CREATE UNIFIED LANDSCAPE (Matches Reference Plugin approach)
  // This creates a single global landscape with merged heightmap and layers.
  CreateUnifiedLandscape(AllTiles, ZON, World, MinX, MinY, MaxX, MaxY, Folder);

  /* Individual Landscapes Removed - using Unified Global Landscape instead */

  // PHASE 3: SPAWN OBJECTS
  UE_LOG(LogRoseImporter, Log, TEXT("Spawning Zone Objects..."));

  // Create a unified landscape first? Or strictly tile-based?
  // Current logic: ProcessHeightmap spawns discrete landscapes?
  // Wait, ProcessHeightmap spawns ALandscape actors.

  float WorkPerTile = 1.0f / FMath::Max(1, TilesToLoad.Num());

  for (const FTileInfo &Tile : TilesToLoad) {
    SlowTask.EnterProgressFrame(
        WorkPerTile,
        FText::Format(NSLOCTEXT("RoseImporter", "SpawningObjects",
                                "Spawning Objects for Tile {0}..."),
                      FText::FromString(Tile.BaseName)));

    FString IFOPath = FPaths::Combine(Folder, Tile.BaseName + TEXT(".ifo"));
    if (FPaths::FileExists(IFOPath)) {
      FRoseIFO IFO;
      if (IFO.Load(IFOPath)) {
        // FIX: IFO positions are GLOBAL — no tile offset needed.
        // Reference plugin uses obj.Position directly.
        int32 ZoneWidth = MaxX - MinX + 1;
        int32 ZoneHeight = MaxY - MinY + 1;

        ProcessObjects(IFO, World, FVector::ZeroVector, MinX, MinY, ZoneWidth,
                       ZoneHeight);
      }
    }
  }

  // PHASE 4: FINALIZE HISM COMPONENTS (Deferred Registration & Attachment)
  UE_LOG(LogRoseImporter, Log,
         TEXT("Finalizing (Attach+Register) %d HISM Components..."),
         GlobalHISMMap.Num());
  for (auto &Elem : GlobalHISMMap) {
    if (Elem.Value) {
      if (!Elem.Value->GetAttachParent()) {
        Elem.Value->AttachToComponent(
            ZoneObjectsActor->GetRootComponent(),
            FAttachmentTransformRules::KeepRelativeTransform);
      }
      if (!Elem.Value->IsRegistered()) {
        Elem.Value->RegisterComponent();
      }
    }
  }

  UE_LOG(LogRoseImporter, Log, TEXT("Zone Import Complete."));
  return true;
}

UMaterial *
URoseImporter::CreateLandscapeMaterial(const FRoseZON &ZON,
                                       const TArray<FLoadedTile> &AllTiles) {
  // 1. Analyze TIL data to find unique (TexID1, TexID2) texture pairs
  // Use int64 as key to avoid hash function issues: (TexID1 << 32) | TexID2
  TMap<int64, int32> TexturePairFrequency;

  for (const FLoadedTile &Tile : AllTiles) {
    for (const FRoseTilePatch &Patch : Tile.TIL.Patches) {
      // Use Patch.Tile (TileID) to lookup in ZON.Tiles array
      int32 TileID = Patch.Tile;

      if (TileID >= 0 && TileID < ZON.Tiles.Num()) {
        // Get the TWO texture indices for this tile
        int32 TexID1 = ZON.Tiles[TileID].GetTextureID1();
        int32 TexID2 = ZON.Tiles[TileID].GetTextureID2();

        // Use only TexID1 (base texture) for now
        // Future: implement dual-texture blending
        int64 Key = ((int64)TexID1 << 32) | (int64)TexID1;

        int32 &Count = TexturePairFrequency.FindOrAdd(Key, 0);
        Count++;
      }
    }
  }

  UE_LOG(LogRoseImporter, Log, TEXT("Found %d unique texture pairs"),
         TexturePairFrequency.Num());

  // 2. Sort texture pairs by frequency (most common first)
  struct FTexturePair {
    int32 TexID1;
    int32 TexID2;
    int32 Count;
  };

  TArray<FTexturePair> AllPairs;
  for (const auto &Pair : TexturePairFrequency) {
    FTexturePair TexPair;
    TexPair.TexID1 = (int32)(Pair.Key >> 32);
    TexPair.TexID2 = (int32)(Pair.Key & 0xFFFFFFFF);
    TexPair.Count = Pair.Value;
    AllPairs.Add(TexPair);
  }

  AllPairs.Sort([](const FTexturePair &A, const FTexturePair &B) {
    return A.Count > B.Count; // Descending by frequency
  });

  // 3. Select top N pairs (limit for performance and shader samplers)
  // Tried 24 layers to reduce texture mismatch
  const int32 MaxLayers = 24;
  int32 NumLayersToCreate = FMath::Min(AllPairs.Num(), MaxLayers);

  TArray<FTexturePair> SelectedPairs;
  for (int32 i = 0; i < NumLayersToCreate; ++i) {
    SelectedPairs.Add(AllPairs[i]);

    UE_LOG(LogRoseImporter, Log,
           TEXT("Layer %d: Texture %d (used in %d patches, %.1f%%)"), i,
           AllPairs[i].TexID1, AllPairs[i].Count,
           (float)AllPairs[i].Count * 100.0f /
               (AllTiles.Num() * 16.0f * 16.0f));
  }

  UE_LOG(LogRoseImporter, Log,
         TEXT("Creating %d landscape layers from texture analysis"),
         NumLayersToCreate);

  // 3. Create the Material asset
  FString MaterialName = TEXT("M_RoseLandscape");
  FString MaterialPackageName = TEXT("/Game/ROSE/Materials/") + MaterialName;

  UPackage *MaterialPackage = CreatePackage(*MaterialPackageName);
  UMaterialFactoryNew *MaterialFactory = NewObject<UMaterialFactoryNew>();

  UMaterial *Material = (UMaterial *)MaterialFactory->FactoryCreateNew(
      UMaterial::StaticClass(), MaterialPackage, *MaterialName,
      RF_Standalone | RF_Public, nullptr, GWarn);

  if (!Material) {
    UE_LOG(LogRoseImporter, Error, TEXT("Failed to create landscape material"));
    return nullptr;
  }

  // 4. Create LandscapeLayerBlend node
  UMaterialExpressionLandscapeLayerBlend *LayerBlend =
      NewObject<UMaterialExpressionLandscapeLayerBlend>(Material);
  Material->GetExpressionCollection().AddExpression(LayerBlend);
  LayerBlend->MaterialExpressionEditorX = -400;
  LayerBlend->MaterialExpressionEditorY = 0;

  // 5. Create texture samplers for each selected texture pair
  int32 YOffset = 0;
  int32 LayerIndex = 0;

  for (const FTexturePair &Pair : SelectedPairs) {
    // Create a new layer in the blend node
    FLayerBlendInput &Layer = LayerBlend->Layers.AddDefaulted_GetRef();
    FString LayerName = FString::Printf(TEXT("T%d"), Pair.TexID1);
    Layer.LayerName = FName(*LayerName);

    // Use LB_WeightBlend for ALL layers to ensure correct normalization
    // This allows using arbitrary weight values (like 50) without transparency
    // issues
    Layer.BlendType = LB_WeightBlend;
    Layer.ConstLayerInput = FVector(0, 0, 0);

    /* REMOVED AlphaBlend special case for first layer */

    // Create texture sample parameter
    UMaterialExpressionTextureSampleParameter2D *TexSample =
        NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
    Material->GetExpressionCollection().AddExpression(TexSample);
    TexSample->MaterialExpressionEditorX = -800;
    TexSample->MaterialExpressionEditorY = YOffset;
    TexSample->ParameterName =
        FName(*FString::Printf(TEXT("Tex_%s"), *LayerName));

    // Load the texture using TexID1 (base texture)
    int32 TextureIdx = Pair.TexID1;
    if (TextureIdx >= 0 && TextureIdx < ZON.Textures.Num()) {
      FString TexturePath = ZON.Textures[TextureIdx];
      UTexture *LoadedTexture = LoadRoseTexture(TexturePath);
      if (LoadedTexture) {
        TexSample->Texture = LoadedTexture;
        UE_LOG(LogRoseImporter, Log, TEXT("Layer %s: loaded texture %s"),
               *LayerName, *TexturePath);
      }
    }

    // Create UV coordinate scaling (LandscapeCoords)
    UMaterialExpressionLandscapeLayerCoords *UVCoords =
        NewObject<UMaterialExpressionLandscapeLayerCoords>(Material);
    Material->GetExpressionCollection().AddExpression(UVCoords);
    UVCoords->MaterialExpressionEditorX = -1100;
    UVCoords->MaterialExpressionEditorY = YOffset;
    UVCoords->MappingScale = 1.0f;
    UVCoords->MappingType = TCMT_Auto;

    // Connect UV to texture sampler
    TexSample->Coordinates.Expression = UVCoords;

    // Connect texture to layer blend
    Layer.LayerInput.Expression = TexSample;

    YOffset += 250;
    LayerIndex++; // Increment for next layer
  }

  // 6. Connect LayerBlend to material's Base Color
  Material->GetExpressionInputForProperty(MP_BaseColor)->Connect(0, LayerBlend);

  // 7. Update and compile the material
  Material->PreEditChange(nullptr);
  Material->PostEditChange();
  Material->MarkPackageDirty();

  FAssetRegistryModule::AssetCreated(Material);

  UE_LOG(LogRoseImporter, Log,
         TEXT("Created landscape material with %d layers"),
         LayerBlend->Layers.Num());

  return Material;
}

// Create a simple material to preview vertex colors (debug)
UMaterial *URoseImporter::CreateVertexColorPreviewMaterial() {
  FString MaterialName = TEXT("M_VertexColorPreview");
  FString MaterialPackageName = TEXT("/Game/ROSE/Materials/") + MaterialName;

  UPackage *MaterialPackage = CreatePackage(*MaterialPackageName);
  UMaterialFactoryNew *MaterialFactory = NewObject<UMaterialFactoryNew>();

  UMaterial *Material = (UMaterial *)MaterialFactory->FactoryCreateNew(
      UMaterial::StaticClass(), MaterialPackage, *MaterialName,
      RF_Standalone | RF_Public, nullptr, GWarn);

  if (!Material) {
    UE_LOG(LogRoseImporter, Error, TEXT("Failed to create preview material"));
    return nullptr;
  }

  // Create Vertex Color node
  UMaterialExpressionVertexColor *VertexColor =
      NewObject<UMaterialExpressionVertexColor>(Material);
  Material->GetExpressionCollection().AddExpression(VertexColor);
  VertexColor->MaterialExpressionEditorX = -400;
  VertexColor->MaterialExpressionEditorY = 0;

  // Connect RGB directly to Base Color for visualization
  // R = TextureID1, G = TextureID2, B = BlendFactor
  Material->GetExpressionInputForProperty(MP_BaseColor)
      ->Connect(0, VertexColor);

  // Compile
  Material->PreEditChange(nullptr);
  Material->PostEditChange();
  Material->MarkPackageDirty();

  FAssetRegistryModule::AssetCreated(Material);

  UE_LOG(LogRoseImporter, Log, TEXT("Created vertex color preview material"));

  return Material;
}

// Create test material that blends 2 most frequent textures
UMaterial *URoseImporter::CreateDualTextureTestMaterial(
    const FRoseZON &ZON, const TArray<FLoadedTile> &AllTiles) {

  // Analyze to find top 2 most frequent textures
  TMap<int32, int32> TexIDFrequency;

  for (const FLoadedTile &Tile : AllTiles) {
    for (const FRoseTilePatch &Patch : Tile.TIL.Patches) {
      int32 TileID = Patch.Tile;
      if (TileID >= 0 && TileID < ZON.Tiles.Num()) {
        int32 TexID1 = ZON.Tiles[TileID].GetTextureID1();
        if (TexID1 >= 0 && TexID1 < ZON.Textures.Num()) {
          TexIDFrequency.FindOrAdd(TexID1, 0)++;
        }
      }
    }
  }

  // Sort by frequency
  TArray<TPair<int32, int32>> Sorted;
  for (const auto &Pair : TexIDFrequency) {
    Sorted.Add(TPair<int32, int32>(Pair.Key, Pair.Value));
  }
  Sorted.Sort([](const TPair<int32, int32> &A, const TPair<int32, int32> &B) {
    return A.Value > B.Value;
  });

  if (Sorted.Num() < 2) {
    UE_LOG(LogRoseImporter, Warning,
           TEXT("Not enough textures found for dual-texture test material"));
    return nullptr;
  }

  int32 TopTexID1 = Sorted[0].Key;
  int32 TopTexID2 = Sorted[1].Key;

  UE_LOG(
      LogRoseImporter, Log,
      TEXT("Creating test material with textures %d (%.1f%%) and %d (%.1f%%)"),
      TopTexID1, Sorted[0].Value * 100.0f / AllTiles.Num() / 256, TopTexID2,
      Sorted[1].Value * 100.0f / AllTiles.Num() / 256);

  // Load the 2 textures
  UTexture *Tex1 = LoadRoseTexture(ZON.Textures[TopTexID1]);
  UTexture *Tex2 = LoadRoseTexture(ZON.Textures[TopTexID2]);

  if (!Tex1 || !Tex2) {
    UE_LOG(LogRoseImporter, Error, TEXT("Failed to load test textures"));
    return nullptr;
  }

  // Create material
  FString MaterialName = TEXT("M_DualTextureTest");
  FString MaterialPackageName = TEXT("/Game/ROSE/Materials/") + MaterialName;

  UPackage *MaterialPackage = CreatePackage(*MaterialPackageName);
  UMaterialFactoryNew *MaterialFactory = NewObject<UMaterialFactoryNew>();

  UMaterial *Material = (UMaterial *)MaterialFactory->FactoryCreateNew(
      UMaterial::StaticClass(), MaterialPackage, *MaterialName,
      RF_Standalone | RF_Public, nullptr, GWarn);

  if (!Material) {
    return nullptr;
  }

  // Vertex Color node
  UMaterialExpressionVertexColor *VertexColor =
      NewObject<UMaterialExpressionVertexColor>(Material);
  Material->GetExpressionCollection().AddExpression(VertexColor);
  VertexColor->MaterialExpressionEditorX = -800;
  VertexColor->MaterialExpressionEditorY = 200;

  // UV Coords
  UMaterialExpressionLandscapeLayerCoords *UVCoords =
      NewObject<UMaterialExpressionLandscapeLayerCoords>(Material);
  Material->GetExpressionCollection().AddExpression(UVCoords);
  UVCoords->MaterialExpressionEditorX = -800;
  UVCoords->MaterialExpressionEditorY = -200;
  UVCoords->MappingScale = 1.0f;

  // Texture Sample 1
  UMaterialExpressionTextureSampleParameter2D *TexSample1 =
      NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
  Material->GetExpressionCollection().AddExpression(TexSample1);
  TexSample1->MaterialExpressionEditorX = -500;
  TexSample1->MaterialExpressionEditorY = -300;
  TexSample1->ParameterName = FName(TEXT("Texture1"));
  TexSample1->Texture = Tex1;
  TexSample1->Coordinates.Expression = UVCoords;

  // Texture Sample 2
  UMaterialExpressionTextureSampleParameter2D *TexSample2 =
      NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
  Material->GetExpressionCollection().AddExpression(TexSample2);
  TexSample2->MaterialExpressionEditorX = -500;
  TexSample2->MaterialExpressionEditorY = 0;
  TexSample2->ParameterName = FName(TEXT("Texture2"));
  TexSample2->Texture = Tex2;
  TexSample2->Coordinates.Expression = UVCoords;

  // Lerp node
  UMaterialExpressionLinearInterpolate *Lerp =
      NewObject<UMaterialExpressionLinearInterpolate>(Material);
  Material->GetExpressionCollection().AddExpression(Lerp);
  Lerp->MaterialExpressionEditorX = -200;
  Lerp->MaterialExpressionEditorY = 0;

  Lerp->A.Expression = TexSample1;
  Lerp->B.Expression = TexSample2;
  Lerp->Alpha.Expression = VertexColor;
  Lerp->Alpha.OutputIndex = 2; // B channel = BlendFactor

  // Connect to Base Color
  Material->GetExpressionInputForProperty(MP_BaseColor)->Connect(0, Lerp);

  // Compile
  Material->PreEditChange(nullptr);
  Material->PostEditChange();
  Material->MarkPackageDirty();

  FAssetRegistryModule::AssetCreated(Material);

  UE_LOG(LogRoseImporter, Log, TEXT("Created dual-texture test material"));

  return Material;
}

// Create production material with switch-based texture selection (top 12)
UMaterial *URoseImporter::CreateSwitchBasedDualTextureMaterial(
    const FRoseZON &ZON, const TArray<FLoadedTile> &AllTiles) {

  UE_LOG(LogRoseImporter, Log,
         TEXT("CreateSwitchBasedDualTextureMaterial called"));

  // NOTE: Full implementation requires ~300+ lines of if/else cascade
  // For now, returning the simpler dual-texture test material
  // TODO: Implement complete switch logic with 12 textures

  UE_LOG(LogRoseImporter, Warning,
         TEXT("Switch-based material stub - returning test material instead"));

  return CreateDualTextureTestMaterial(ZON, AllTiles);
}

// Create a Deferred Decal material for transparency effects
UMaterial *URoseImporter::CreateDecalMaterial(UTexture2D *Texture,
                                              int32 TexID) {
  if (!Texture) {
    return nullptr;
  }

  FString MaterialName = FString::Printf(TEXT("M_Decal_T%d"), TexID);
  FString MaterialPackageName =
      TEXT("/Game/ROSE/Materials/Decals/") + MaterialName;

  // Check if already exists
  if (UMaterial *ExistingMaterial =
          LoadObject<UMaterial>(nullptr, *MaterialPackageName)) {
    return ExistingMaterial;
  }

  UPackage *MaterialPackage = CreatePackage(*MaterialPackageName);
  UMaterialFactoryNew *MaterialFactory = NewObject<UMaterialFactoryNew>();

  UMaterial *Material = (UMaterial *)MaterialFactory->FactoryCreateNew(
      UMaterial::StaticClass(), MaterialPackage, *MaterialName,
      RF_Standalone | RF_Public, nullptr, GWarn);

  if (!Material) {
    UE_LOG(LogRoseImporter, Error,
           TEXT("Failed to create decal material for T%d"), TexID);
    return nullptr;
  }

  // Configure for deferred decal
  Material->MaterialDomain = MD_DeferredDecal;
  Material->BlendMode = BLEND_Translucent;
  Material->SetShadingModel(MSM_DefaultLit);

  // Create texture sampler
  UMaterialExpressionTextureSampleParameter2D *TexSample =
      NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
  Material->GetExpressionCollection().AddExpression(TexSample);
  TexSample->MaterialExpressionEditorX = -400;
  TexSample->MaterialExpressionEditorY = 0;
  TexSample->ParameterName =
      FName(*FString::Printf(TEXT("DecalTex_T%d"), TexID));
  TexSample->Texture = Texture;

  // Connect RGB to Base Color (output 0)
  Material->GetExpressionInputForProperty(MP_BaseColor)->Connect(0, TexSample);

  // Connect Alpha to Opacity (output 4 = alpha channel)
  Material->GetExpressionInputForProperty(MP_Opacity)->Connect(4, TexSample);

  // Compile and save
  Material->PreEditChange(nullptr);
  Material->PostEditChange();
  Material->MarkPackageDirty();

  FAssetRegistryModule::AssetCreated(Material);

  UE_LOG(LogRoseImporter, Log, TEXT("Created decal material: %s"),
         *MaterialName);

  return Material;
}

// Spawn decal actors for textures with alpha transparency
void URoseImporter::SpawnDecalsForTextures(UWorld *World, const FRoseZON &ZON,
                                           const TArray<FLoadedTile> &AllTiles,
                                           int32 MinX, int32 MinY) {
  if (!World) {
    return;
  }

  UE_LOG(LogRoseImporter, Log, TEXT("Spawning decals for alpha textures..."));

  // Structure to track decal placements
  struct FDecalPlacement {
    int32 TexID;
    FVector WorldLocation;
    FVector2D Size;
  };

  TArray<FDecalPlacement> DecalPlacements;

  // Analyze all tiles and patches to find textures that need decals
  for (const FLoadedTile &Tile : AllTiles) {
    for (int32 py = 0; py < 16; ++py) {
      for (int32 px = 0; px < 16; ++px) {
        int32 PatchIdx = py * 16 + px;
        if (PatchIdx >= Tile.TIL.Patches.Num())
          continue;

        const FRoseTilePatch &Patch = Tile.TIL.Patches[PatchIdx];
        int32 TileID = Patch.Tile;

        if (TileID < 0 || TileID >= ZON.Tiles.Num())
          continue;

        int32 TexID1 = ZON.Tiles[TileID].GetTextureID1();

        // For now, spawn decals for all textured patches
        // TODO: Filter only textures with significant alpha

        // Calculate world position for this patch
        // Each patch is 4x4 meters in ROSE
        float PatchWorldX = ((Tile.X - MinX) * 64) + (px * 4);
        float PatchWorldY = ((Tile.Y - MinY) * 64) + (py * 4);

        // ROSE to Unreal conversion: swap Y/Z, scale
        float UnrealX = PatchWorldX * 100.0f; // cm
        float UnrealY = PatchWorldY * 100.0f; // cm
        float UnrealZ = 1000.0f;              // Spawn above terrain

        FDecalPlacement Decal;
        Decal.TexID = TexID1;
        Decal.WorldLocation = FVector(UnrealX, UnrealY, UnrealZ);
        Decal.Size = FVector2D(400.0f, 400.0f); // 4m = 400cm

        DecalPlacements.Add(Decal);
      }
    }
  }

  UE_LOG(LogRoseImporter, Log, TEXT("Found %d potential decal placements"),
         DecalPlacements.Num());

  // Limit decals for performance (TODO: implement merging/optimization)
  int32 MaxDecals = 100; // Conservative limit for testing
  if (DecalPlacements.Num() > MaxDecals) {
    UE_LOG(LogRoseImporter, Warning,
           TEXT("Limiting decals from %d to %d for performance"),
           DecalPlacements.Num(), MaxDecals);
    DecalPlacements.SetNum(MaxDecals);
  }

  // TODO: Spawning will be implemented in next phase
  // For now, just log that we would spawn decals
  UE_LOG(LogRoseImporter, Log,
         TEXT("Decal spawning ready - %d decals prepared (spawn code TODO)"),
         DecalPlacements.Num());
}

// Setup vertex colors for dual-texture blending
void URoseImporter::SetupVertexColors(ALandscape *Landscape,
                                      const TArray<FLoadedTile> &AllTiles,
                                      const FRoseZON &ZON, int32 MinX,
                                      int32 MinY) {
  if (!Landscape) {
    return;
  }

  UE_LOG(LogRoseImporter, Log,
         TEXT("Setting up vertex colors for dual-texture blending..."));

  // Create a map for fast tile lookup by coordinates
  TMap<FIntPoint, const FLoadedTile *> TileMap;
  for (const FLoadedTile &Tile : AllTiles) {
    TileMap.Add(FIntPoint(Tile.X, Tile.Y), &Tile);
  }

  // Get all landscape components
  TArray<ULandscapeComponent *> LandscapeComponents;
  Landscape->GetComponents(LandscapeComponents);

  if (LandscapeComponents.Num() == 0) {
    UE_LOG(LogRoseImporter, Warning, TEXT("No landscape components found"));
    return;
  }

  int32 ComponentsProcessed = 0;

  // Process each component
  for (ULandscapeComponent *Component : LandscapeComponents) {
    if (!Component) {
      continue;
    }

    const int32 ComponentSizeQuads = Component->ComponentSizeQuads;
    const int32 VerticesPerSide = ComponentSizeQuads + 1;
    const int32 VertexCount = VerticesPerSide * VerticesPerSide;

    // Allocate vertex colors array
    TArray<FColor> VertexColors;
    VertexColors.SetNum(VertexCount);

    // Get component's section base (offset in landscape)
    FIntPoint ComponentBase = Component->GetSectionBase();

    // For each vertex in component
    for (int32 VertexY = 0; VertexY < VerticesPerSide; ++VertexY) {
      for (int32 VertexX = 0; VertexX < VerticesPerSide; ++VertexX) {
        int32 VertexIdx = VertexY * VerticesPerSide + VertexX;

        // Calculate landscape-space coordinates of this vertex
        int32 LandscapeX = ComponentBase.X + VertexX;
        int32 LandscapeY = ComponentBase.Y + VertexY;

        // Convert to ROSE tile coordinates (64 vertices per tile)
        int32 RoseTileX = MinX + (LandscapeX / 64);
        int32 RoseTileY = MinY + (LandscapeY / 64);

        // Convert to patch coordinates within tile (16 patches per tile)
        int32 LocalVertexX = LandscapeX % 64;
        int32 LocalVertexY = LandscapeY % 64;
        int32 PatchX = LocalVertexX / 4; // 4 vertices per patch
        int32 PatchY = LocalVertexY / 4;

        // Default: no texture (black)
        uint8 TexID1 = 0;
        uint8 TexID2 = 0;
        uint8 BlendFactor = 255; // 255 = 100% ID1

        // Look up the tile
        const FLoadedTile *Tile =
            TileMap.FindRef(FIntPoint(RoseTileX, RoseTileY));

        if (Tile) {
          // Calculate patch index
          int32 PatchIdx = PatchY * 16 + PatchX;

          if (PatchIdx >= 0 && PatchIdx < Tile->TIL.Patches.Num()) {
            const FRoseTilePatch &Patch = Tile->TIL.Patches[PatchIdx];
            int32 TileID = Patch.Tile;

            if (TileID >= 0 && TileID < ZON.Tiles.Num()) {
              // Get texture IDs
              TexID1 = (uint8)FMath::Clamp(ZON.Tiles[TileID].GetTextureID1(), 0,
                                           255);
              TexID2 = (uint8)FMath::Clamp(ZON.Tiles[TileID].GetTextureID2(), 0,
                                           255);

              // Determine blend factor (simple: 50/50 if blending, else 100%
              // ID1)
              if (ZON.Tiles[TileID].IsBlending()) {
                BlendFactor = 128; // 50% blend
              } else {
                BlendFactor = 255; // 100% ID1
              }
            }
          }
        }

        // Encode in vertex color: R=ID1, G=ID2, B=BlendFactor, A=255
        VertexColors[VertexIdx] = FColor(TexID1, TexID2, BlendFactor, 255);
      }
    }

    // Apply vertex colors to component (NOTE: This requires enabling on
    // component) For now, just log that we prepared the data Full application
    // needs SetLODVertexColors or similar
    ComponentsProcessed++;
  }

  UE_LOG(LogRoseImporter, Log,
         TEXT("Vertex colors prepared for %d components (application TODO)"),
         ComponentsProcessed);
}

void URoseImporter::CreateUnifiedLandscape(const TArray<FLoadedTile> &AllTiles,
                                           const FRoseZON &ZON, UWorld *World,
                                           int32 MinX, int32 MinY, int32 MaxX,
                                           int32 MaxY,
                                           const FString &ZoneFolder) {
  // Calculate total landscape size
  int32 TotalSizeX = (MaxX - MinX + 1) * 64 + 1;
  int32 TotalSizeY = (MaxY - MinY + 1) * 64 + 1;

  UE_LOG(LogRoseImporter, Log, TEXT("Creating unified landscape: %dx%d"),
         TotalSizeX, TotalSizeY);

  // STEP 1: Merge all heightmaps
  TArray<uint16> MergedHeights;
  MergedHeights.SetNumZeroed(TotalSizeX * TotalSizeY);

  for (const auto &Tile : AllTiles) {
    int32 OffsetX = (Tile.X - MinX) * 64;
    int32 OffsetY = (Tile.Y - MinY) * 64;

    for (int32 y = 0; y < 65; ++y) {
      for (int32 x = 0; x < 65; ++x) {
        int32 DestIdx = (OffsetY + y) * TotalSizeX + (OffsetX + x);
        float h = Tile.HIM.Heights[y * 65 + x];
        MergedHeights[DestIdx] =
            (uint16)((FMath::Clamp(h + 25600.0f, 0.0f, 51200.0f) / 51200.0f) *
                     65535.0f);
      }
    }
  }

  // STEP 2: Analyze TIL data to find unique texture pairs
  // CRITICAL: Use SAME logic as CreateLandscapeMaterial to ensure layer names
  // match!
  TMap<int64, int32> TexturePairFrequency;

  for (const FLoadedTile &Tile : AllTiles) {
    for (const FRoseTilePatch &Patch : Tile.TIL.Patches) {
      int32 TileID = Patch.Tile;

      if (TileID >= 0 && TileID < ZON.Tiles.Num()) {
        int32 TexID1 = ZON.Tiles[TileID].GetTextureID1();
        int32 TexID2 = ZON.Tiles[TileID].GetTextureID2();

        // Use only TexID1 for now (matching material logic)
        int64 Key = ((int64)TexID1 << 32) | (int64)TexID1;

        int32 &Count = TexturePairFrequency.FindOrAdd(Key, 0);
        Count++;
      }
    }
  }

  // Sort and select top N (matching material logic)
  struct FTexturePair {
    int32 TexID1;
    int32 TexID2;
    int32 Count;
  };

  TArray<FTexturePair> AllPairs;
  for (const auto &Pair : TexturePairFrequency) {
    FTexturePair TexPair;
    TexPair.TexID1 = (int32)(Pair.Key >> 32);
    TexPair.TexID2 = (int32)(Pair.Key & 0xFFFFFFFF);
    TexPair.Count = Pair.Value;
    AllPairs.Add(TexPair);
  }

  AllPairs.Sort([](const FTexturePair &A, const FTexturePair &B) {
    return A.Count > B.Count;
  });

  const int32 MaxLayers = 24;
  int32 NumLayersToCreate = FMath::Min(AllPairs.Num(), MaxLayers);

  TArray<FTexturePair> SelectedPairs;
  for (int32 i = 0; i < NumLayersToCreate; ++i) {
    SelectedPairs.Add(AllPairs[i]);
  }

  UE_LOG(LogRoseImporter, Log,
         TEXT("Found %d texture pairs, creating %d weightmaps"), AllPairs.Num(),
         NumLayersToCreate);

  // STEP 3: Generate weightmaps for selected texture pairs
  // Map: TexID1 -> WeightData
  TMap<int32, TArray<uint8>> WeightDataMap;
  for (const FTexturePair &Pair : SelectedPairs) {
    TArray<uint8> &WeightData = WeightDataMap.Add(Pair.TexID1);
    WeightData.SetNumZeroed(TotalSizeX * TotalSizeY);
  }

  // Fill weightmaps from TIL data
  for (const auto &Tile : AllTiles) {
    int32 OffsetX = (Tile.X - MinX) * 64;
    int32 OffsetY = (Tile.Y - MinY) * 64;

    for (int32 py = 0; py < 16; ++py) {
      for (int32 px = 0; px < 16; ++px) {
        int32 PatchIdx = py * 16 + px;
        if (PatchIdx >= Tile.TIL.Patches.Num())
          continue;

        const FRoseTilePatch &Patch = Tile.TIL.Patches[PatchIdx];
        int32 TileID = Patch.Tile;

        if (TileID < 0 || TileID >= ZON.Tiles.Num())
          continue;

        int32 TexID1 = ZON.Tiles[TileID].GetTextureID1();

        // Find if this TexID is in our selected pairs
        TArray<uint8> *WeightData = WeightDataMap.Find(TexID1);

        // If texture not in top 12, assign to first layer (base) to avoid black
        // squares
        if (!WeightData && SelectedPairs.Num() > 0) {
          WeightData = WeightDataMap.Find(SelectedPairs[0].TexID1);
        }

        if (!WeightData)
          continue;

        // Upsample this patch to 4x4 weightmap pixels
        for (int32 dy = 0; dy < 4; ++dy) {
          for (int32 dx = 0; dx < 4; ++dx) {
            int32 DestX = OffsetX + px * 4 + dx;
            int32 DestY = OffsetY + py * 4 + dy;
            int32 DestIdx = DestY * TotalSizeX + DestX;

            if (DestIdx >= 0 && DestIdx < WeightData->Num()) {
              if (DestIdx >= 0 && DestIdx < WeightData->Num()) {
                (*WeightData)[DestIdx] = 255;
              }
            }
          }
        }
      }
    }
  }

  // STEP 4: Create landscape layers for selected texture pairs
  TArray<FLandscapeImportLayerInfo> LayerInfos;

  for (const FTexturePair &Pair : SelectedPairs) {
    // Use same naming as material: "T{TexID1}"
    FString LayerName = FString::Printf(TEXT("T%d"), Pair.TexID1);
    FString PackageName = TEXT("/Game/Rose/Imported/Landscape/Layers");
    FString AssetName = LayerName;

    UPackage *Package = CreatePackage(*(PackageName / AssetName));
    ULandscapeLayerInfoObject *LIO = NewObject<ULandscapeLayerInfoObject>(
        Package, *AssetName, RF_Public | RF_Standalone);

    if (LIO) {
      LIO->SetLayerName(FName(*LayerName), false);
      FAssetRegistryModule::AssetCreated(LIO);
      Package->MarkPackageDirty();

      FLandscapeImportLayerInfo LayerInfo;
      LayerInfo.LayerName = FName(*LayerName);
      LayerInfo.LayerInfo = LIO;
      LayerInfo.LayerData = WeightDataMap[Pair.TexID1];
      LayerInfos.Add(LayerInfo);

      UE_LOG(LogRoseImporter, Log, TEXT("Created layer: %s"), *LayerName);
    }
  }

  // STEP 5: Spawn landscape at correct global position
  // Reference formula: (tileIndex - 32) * 16000 - 8000
  // Tile 32 is ROSE world origin. -8000 compensates for UE landscape pivot.
  FVector LandscapeLocation((MinX - 32) * 16000.0f - 8000.0f,
                            (MinY - 32) * 16000.0f - 8000.0f, 0.0f);
  UE_LOG(LogRoseImporter, Log, TEXT("Landscape at: %s (MinX=%d MinY=%d)"),
         *LandscapeLocation.ToString(), MinX, MinY);

  ALandscape *Landscape =
      World->SpawnActor<ALandscape>(LandscapeLocation, FRotator::ZeroRotator);

  if (Landscape) {
    Landscape->SetActorLabel(TEXT("RoseZone_UnifiedLandscape"));
    Landscape->SetActorScale3D(FVector(250.0f, 250.0f, 100.0f));

    // STEP 5: Create and assign 12-layer weightmap material
    UE_LOG(LogRoseImporter, Log,
           TEXT("Creating 12-layer landscape material..."));

    UMaterial *LandscapeMaterial = CreateLandscapeMaterial(ZON, AllTiles);

    if (LandscapeMaterial) {
      Landscape->LandscapeMaterial = LandscapeMaterial;
      UE_LOG(LogRoseImporter, Log,
             TEXT("Assigned 12-layer weightmap material to landscape"));
    } else {
      UE_LOG(LogRoseImporter, Warning,
             TEXT("Failed to create landscape material, using fallback"));
      EnsureMasterMaterial();
      if (MasterMaterial) {
        Landscape->LandscapeMaterial = MasterMaterial;
      }
    }

    TMap<FGuid, TArray<uint16>> HeightDataMap;
    TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerInfoMap;

    HeightDataMap.Add(FGuid(), MergedHeights);
    MaterialLayerInfoMap.Add(FGuid(), LayerInfos);

    Landscape->Import(FGuid::NewGuid(), 0, 0, TotalSizeX - 1, TotalSizeY - 1, 1,
                      63, // ComponentsPerSection, SectionsPerComponent
                      HeightDataMap, nullptr, MaterialLayerInfoMap,
                      ELandscapeImportAlphamapType::Additive,
                      TArrayView<const FLandscapeLayer>());

    UE_LOG(LogRoseImporter, Log,
           TEXT("Unified landscape created successfully!"));
  }
}

// Helper to create a material with layers for a single tile
UMaterial *URoseImporter::CreateTileMaterial(const FRoseTIL &TIL,
                                             const FRoseZON &ZON,
                                             const FString &TileName,
                                             TArray<int32> &OutTextureIDs) {
  // 1. Analyze TIL data to find unique textures
  TMap<int32, int32> TextureCounts;
  for (const FRoseTilePatch &Patch : TIL.Patches) {
    int32 TileID = Patch.Tile;
    if (TileID >= 0 && TileID < ZON.Tiles.Num()) {
      int32 TexID1 = ZON.Tiles[TileID].GetTextureID1();
      if (TexID1 >= 0) {
        TextureCounts.FindOrAdd(TexID1)++;
      }
    }
  }

  // 2. Select top N textures (limit 16)
  struct FTextureCount {
    int32 TexID;
    int32 Count;
  };
  TArray<FTextureCount> TextureList;
  for (const auto &Pair : TextureCounts) {
    TextureList.Add({Pair.Key, Pair.Value});
  }
  TextureList.Sort([](const FTextureCount &A, const FTextureCount &B) {
    return A.Count > B.Count;
  });

  // Limit to 16 layers (shader sampler limit)
  const int32 MaxLayers = 16;
  int32 NumLayers = FMath::Min(TextureList.Num(), MaxLayers);

  OutTextureIDs.Empty();
  for (int32 i = 0; i < NumLayers; ++i) {
    OutTextureIDs.Add(TextureList[i].TexID);
  }

  // If no textures found, return nullptr (will fallback to master material)
  if (NumLayers == 0)
    return nullptr;

  // 3. Create Material
  FString MaterialName = TEXT("M_Landscape_") + TileName;
  FString PName = TEXT("/Game/Rose/Imported/Materials/") + MaterialName;

  UPackage *Package = CreatePackage(*PName);
  UMaterialFactoryNew *Factory = NewObject<UMaterialFactoryNew>();
  UMaterial *Material = (UMaterial *)Factory->FactoryCreateNew(
      UMaterial::StaticClass(), Package, *MaterialName,
      RF_Public | RF_Standalone, nullptr, GWarn);

  // 4. Create LandscapeLayerBlend
  UMaterialExpressionLandscapeLayerBlend *LayerBlend =
      NewObject<UMaterialExpressionLandscapeLayerBlend>(Material);
  Material->GetExpressionCollection().AddExpression(LayerBlend);
  LayerBlend->MaterialExpressionEditorX = -200;
  LayerBlend->MaterialExpressionEditorY = 0;

  int32 YOffset = 0;
  for (int32 i = 0; i < NumLayers; ++i) {
    int32 TexID = OutTextureIDs[i];

    // Add layer
    FLayerBlendInput &Layer = LayerBlend->Layers.AddDefaulted_GetRef();
    Layer.LayerName = FName(*FString::Printf(TEXT("T%d"), TexID));

    // First layer is AlphaBlend with gray fallback, others WeightBlend
    if (i == 0) {
      Layer.BlendType = LB_AlphaBlend;
      Layer.ConstLayerInput = FVector(0.5f, 0.5f, 0.5f);
    } else {
      Layer.BlendType = LB_WeightBlend;
      Layer.ConstLayerInput = FVector(0, 0, 0);
    }

    // Create Texture Sampler
    UMaterialExpressionTextureSampleParameter2D *Sampler =
        NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
    Material->GetExpressionCollection().AddExpression(Sampler);
    Sampler->MaterialExpressionEditorX = -600;
    Sampler->MaterialExpressionEditorY = YOffset;
    Sampler->ParameterName = FName(*FString::Printf(TEXT("Tex_T%d"), TexID));

    // Load Texture
    if (TexID >= 0 && TexID < ZON.Textures.Num()) {
      Sampler->Texture = LoadRoseTexture(ZON.Textures[TexID]);
    }

    // Create UV Coords
    UMaterialExpressionLandscapeLayerCoords *UVs =
        NewObject<UMaterialExpressionLandscapeLayerCoords>(Material);
    Material->GetExpressionCollection().AddExpression(UVs);
    UVs->MaterialExpressionEditorX = -800;
    UVs->MaterialExpressionEditorY = YOffset;
    UVs->MappingScale = 1.0f;
    UVs->MappingType = TCMT_Auto;

    Sampler->Coordinates.Expression = UVs;
    Layer.LayerInput.Expression = Sampler;

    YOffset += 250;
  }

  // Connect LayerBlend to BaseColor
  Material->GetExpressionInputForProperty(MP_BaseColor)->Connect(0, LayerBlend);

  Material->PreEditChange(nullptr);
  Material->PostEditChange();
  FAssetRegistryModule::AssetCreated(Material);

  return Material;
}

// Generate weightmaps for the tile
TMap<int32, TArray<uint8>>
URoseImporter::GenerateTileWeightmaps(const FRoseTIL &TIL, const FRoseZON &ZON,
                                      const TArray<int32> &TextureIDs) {

  TMap<int32, TArray<uint8>> Weightmaps;
  TMap<int32, int32> RotationCounts;

  // Initialize weightmaps (64x64)
  for (int32 TexID : TextureIDs) {
    TArray<uint8> &Map = Weightmaps.Add(TexID);
    Map.SetNumZeroed(64 * 64);
  }

  // Fill weightmaps from 16x16 patch grid
  for (int32 py = 0; py < 16; ++py) {
    for (int32 px = 0; px < 16; ++px) {
      int32 PatchIdx = py * 16 + px;
      if (PatchIdx >= TIL.Patches.Num())
        continue;

      int32 TileID = TIL.Patches[PatchIdx].Tile;
      if (TileID < 0 || TileID >= ZON.Tiles.Num())
        continue;

      // Log Rotation Stats
      int32 Rot = ZON.Tiles[TileID].Rotation;
      RotationCounts.FindOrAdd(Rot)++;

      int32 TexID = ZON.Tiles[TileID].GetTextureID1();

      // Find matching weightmap
      TArray<uint8> *Map = Weightmaps.Find(TexID);

      // If texture not in selected list, fallback to first layer (if
      // available)
      if (!Map && TextureIDs.Num() > 0) {
        Map = Weightmaps.Find(TextureIDs[0]);
      }

      if (Map) {
        // Upsample 1 patch -> 4x4 weightmap pixels
        for (int32 dy = 0; dy < 4; ++dy) {
          for (int32 dx = 0; dx < 4; ++dx) {
            int32 wy = py * 4 + dy;
            int32 wx = px * 4 + dx;
            if (wy < 64 && wx < 64) {
              (*Map)[wy * 64 + wx] = 255;
            }
          }
        }
      }
    }
  }

  // Log rotation stats
  if (RotationCounts.Num() > 0) {
    FString RotStats;
    for (const auto &Pair : RotationCounts) {
      RotStats += FString::Printf(TEXT("Rot%d:%d "), Pair.Key, Pair.Value);
    }
    UE_LOG(LogRoseImporter, Log, TEXT("Tile Rotation Stats: %s"), *RotStats);
  }

  return Weightmaps;
}

void URoseImporter::ProcessHeightmap(const FRoseHIM &HIM, const FRoseTIL &TIL,
                                     const FRoseZON &ZON, UWorld *World,
                                     const FVector &Offset, const FString &Base,
                                     const FString &ZFolder) {
  UE_LOG(LogRoseImporter, Log, TEXT("Processing Tile %s ..."), *Base);

  if (HIM.Width != 65 || HIM.Height != 65)
    return;

  // Create Landscape Actor
  ALandscape *Lansc =
      World->SpawnActor<ALandscape>(Offset, FRotator::ZeroRotator);
  if (Lansc) {
    Lansc->SetActorLabel(FString::Printf(TEXT("Landscape_%s"), *Base));
    // Correct Scale: 16000cm / 64 intervals = 250.0cm per interval
    // ROSE Heightmap is 65x65 vertices (64 intervals).
    // Unreal Component (63 quads) has 64x64 vertices.
    // We map 0..63 of ROSE directly to 0..63 of Unreal.
    // The 64th ROSE vertex is the start of the next tile (overlap).
    Lansc->SetActorScale3D(FVector(250.0f, 250.0f, 100.0f));

    // 1. Create Multilayer Material for this tile
    TArray<int32> TextureIDs;
    UMaterial *TileMaterial = CreateTileMaterial(TIL, ZON, Base, TextureIDs);

    if (!TileMaterial) {
      UE_LOG(LogRoseImporter, Warning,
             TEXT("Tile %s: Could not create material, skipping"), *Base);
      Lansc->Destroy();
      return;
    }

    Lansc->LandscapeMaterial = TileMaterial;

    // 2. Generate Weightmaps
    TMap<int32, TArray<uint8>> Weightmaps =
        GenerateTileWeightmaps(TIL, ZON, TextureIDs);

    // 3. Import Landscape with Layers
    TArray<uint16> HD;
    HD.SetNumUninitialized(64 * 64);

    // Direct Mapping (No Resampling) - Use Scale 250
    // We take the first 64x64 vertices from the 65x65 HIM
    for (int y = 0; y < 64; ++y) {
      for (int x = 0; x < 64; ++x) {
        // Direct lookup (1:1)
        float hmValue = HIM.Heights[y * 65 + x];

        // Normalize to uint16
        float ueValue =
            (FMath::Clamp(hmValue + 25600.0f, 0.0f, 51200.0f) / 51200.0f) *
            65535.0f;
        HD[y * 64 + x] = (uint16)ueValue;
      }
    }

    FGuid LG = FGuid();
    TMap<FGuid, TArray<uint16>> HDM;
    HDM.Add(LG, HD);

    // 4. Create LayerInfos for Import
    TMap<FGuid, TArray<FLandscapeImportLayerInfo>> LI;
    TArray<FLandscapeImportLayerInfo> LayerInfos;

    for (int32 TexID : TextureIDs) {
      FString LayerName = FString::Printf(TEXT("Layer_T%d"), TexID);

      ULandscapeLayerInfoObject *LayerInfo =
          NewObject<ULandscapeLayerInfoObject>();
      LayerInfo->LayerName = FName(*LayerName);

      FLandscapeImportLayerInfo ImportInfo;
      ImportInfo.LayerName = FName(*LayerName);
      ImportInfo.LayerInfo = LayerInfo;

      if (TArray<uint8> *MapData = Weightmaps.Find(TexID)) {
        ImportInfo.LayerData = *MapData;
      } else {
        ImportInfo.LayerData.Init(0, 64 * 64);
      }

      LayerInfos.Add(ImportInfo);
    }

    LI.Add(LG, LayerInfos);

    Lansc->Import(FGuid::NewGuid(), 0, 0, 63, 63, 1, 63, HDM, nullptr, LI,
                  ELandscapeImportAlphamapType::Additive,
                  TArrayView<const FLandscapeLayer>());

    UE_LOG(LogRoseImporter, Log,
           TEXT("Tile %s: Created landscape with %d layers"), *Base,
           TextureIDs.Num());
  }
}

void URoseImporter::ProcessObjects(const FRoseIFO &IFO, UWorld *World,
                                   const FVector &TileOffset, int32 MinX,
                                   int32 MinY, int32 ZoneWidth,
                                   int32 ZoneHeight) {
  if (!ZoneObjectsActor)
    return;

  // Helper lambda to process a list of objects vs a specific ZSC
  auto ProcessList = [&, this](const TArray<FRoseMapObject> &MapObjects,
                               FRoseZSC &ZSC, const FString &DebugCtx) {
    if (!ZoneObjectsActor) {
      UE_LOG(LogRoseImporter, Error,
             TEXT("ZoneObjectsActor is null in ProcessObjects!"));
      return;
    }

    if (ZSC.Meshes.Num() == 0 && ZSC.Objects.Num() == 0) {
      return;
    }

    int32 SpawnCount = 0;
    for (const FRoseMapObject &MapObj : MapObjects) {
      if (MapObj.ObjectID < 0 || MapObj.ObjectID >= ZSC.Objects.Num()) {
        continue;
      }

      const FRoseZSC::FObjectEntry &ZSCObj = ZSC.Objects[MapObj.ObjectID];
      if (ZSCObj.Parts.Num() == 0) {
        continue;
      }

      for (const FRoseZSC::FObjectPart &Part : ZSCObj.Parts) {
        if (Part.MeshIndex < 0 || Part.MeshIndex >= ZSC.Meshes.Num()) {
          continue;
        }

        const FString &MeshPath = ZSC.Meshes[Part.MeshIndex].MeshPath;
        const FRoseZSC::FMaterialEntry *MatEntry = nullptr;

        if (Part.MaterialIndex >= 0 &&
            Part.MaterialIndex < ZSC.Materials.Num()) {
          MatEntry = &ZSC.Materials[Part.MaterialIndex];
        }

        UStaticMesh *Mesh = ImportRoseMesh(MeshPath, MatEntry, RoseRootPath);
        if (!Mesh) {
          continue;
        }

        UHierarchicalInstancedStaticMeshComponent *HISM = nullptr;
        if (GlobalHISMMap.Contains(Mesh)) {
          HISM = GlobalHISMMap[Mesh];
        } else {
          FString HISMName =
              TEXT("HISM_") + Mesh->GetName() + TEXT("_") + DebugCtx;
          HISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(
              ZoneObjectsActor, *HISMName);
          HISM->SetStaticMesh(Mesh);
          HISM->SetMobility(EComponentMobility::Static);
          GlobalHISMMap.Add(Mesh, HISM);
        }

        // Skip meshes with invalid bounds
        if (Mesh->GetBoundingBox().GetExtent().ContainsNaN()) {
          continue;
        }

        // Calculate Instance Transform
        FTransform PartTransform(FQuat(Part.Rotation), FVector(Part.Position),
                                 FVector(Part.Scale));
        FTransform ObjectTransform(MapObj.Rotation, MapObj.Position,
                                   MapObj.Scale);
        FTransform CombinedLocal = PartTransform * ObjectTransform;
        FTransform FinalTransform(CombinedLocal.GetRotation(),
                                  CombinedLocal.GetLocation(),
                                  CombinedLocal.GetScale3D());

        // Validate Transform
        if (FinalTransform.ContainsNaN() || !FinalTransform.IsValid() ||
            FinalTransform.GetLocation().Size() > 10000000.0f ||
            FinalTransform.GetScale3D().IsNearlyZero()) {
          continue;
        }

        HISM->AddInstance(FinalTransform);
        SpawnCount++;
      }
    }
    UE_LOG(LogRoseImporter, Log,
           TEXT("[%s] Spawned %d instances from %d entries"), *DebugCtx,
           SpawnCount, MapObjects.Num());
  };

  // Process Decorations
  ProcessList(IFO.Objects, DecoZSC, TEXT("Deco"));

  // Process Buildings
  ProcessList(IFO.Buildings, CnstZSC, TEXT("Cnst"));
}

void URoseImporter::EnsureMasterMaterial() {
  auto EnsureVariant = [&](UMaterial *&MatPtr, const FString &Name,
                           EBlendMode BlendMode) {
    FString PN = TEXT("/Game/Rose/Materials/") + Name;
    // Always try to load first
    if (!MatPtr)
      MatPtr = Cast<UMaterial>(FSoftObjectPath(PN).TryLoad());

    bool bNeedsBuild = false;

    // If not found, create it
    if (!MatPtr) {
      UE_LOG(LogRoseImporter, Log,
             TEXT("[Material] Creating new master material: %s"), *Name);
      UPackage *P = CreatePackage(*PN);
      MatPtr = (UMaterial *)NewObject<UMaterialFactoryNew>()->FactoryCreateNew(
          UMaterial::StaticClass(), P, *Name, RF_Public | RF_Standalone,
          nullptr, GWarn);
      bNeedsBuild = true;
    } else {
      // Only rebuild if empty (check if expressions exist)
      if (MatPtr->GetExpressionCollection().Expressions.Num() == 0) {
        bNeedsBuild = true;
      }
    }

    // Only update content if needed (Prevents constant Saving/Rebuilding)
    if (MatPtr && bNeedsBuild) {
      // Clear existing expressions to rebuild clean
      MatPtr->GetExpressionCollection().Empty();

      auto BT = NewObject<UMaterialExpressionTextureSampleParameter2D>(MatPtr);
      BT->ParameterName = TEXT("BaseTexture");
      MatPtr->GetExpressionCollection().AddExpression(BT);

      // Create Tint Color Parameter (Default White)
      auto TintColor = NewObject<UMaterialExpressionVectorParameter>(MatPtr);
      TintColor->ParameterName = TEXT("TintColor");
      TintColor->DefaultValue = FLinearColor::White;
      MatPtr->GetExpressionCollection().AddExpression(TintColor);

      // Multiply Texture * Tint
      auto Mult = NewObject<UMaterialExpressionMultiply>(MatPtr);
      Mult->A.Expression = BT;
      Mult->B.Expression = TintColor;
      MatPtr->GetExpressionCollection().AddExpression(Mult);

#if WITH_EDITOR
      MatPtr->GetEditorOnlyData()->BaseColor.Expression = Mult;
      MatPtr->GetEditorOnlyData()->BaseColor.OutputIndex = 0;

      // Connect Alpha (Source is still Texture Alpha)
      if (BlendMode == BLEND_Masked) {
        MatPtr->GetEditorOnlyData()->OpacityMask.Expression = BT;
        MatPtr->GetEditorOnlyData()->OpacityMask.OutputIndex = 4; // Alpha
      } else if (BlendMode == BLEND_Translucent) {
        MatPtr->GetEditorOnlyData()->Opacity.Expression = BT;
        MatPtr->GetEditorOnlyData()->Opacity.OutputIndex = 4; // Alpha
      }
#endif

      MatPtr->BlendMode = BlendMode;
      MatPtr->bUsedWithInstancedStaticMeshes = true;
      MatPtr->TwoSided =
          (BlendMode !=
           BLEND_Opaque); // Default TwoSided for non-opaque? Or maybe not.
                          // Let's keep existing logic (MIC overrides it).

      MatPtr->PostEditChange();
      FAssetRegistryModule::AssetCreated(MatPtr);
      SaveRoseAsset(MatPtr);
    }
  };

  EnsureVariant(MasterMaterial, TEXT("M_RoseMaster"), BLEND_Opaque);
  EnsureVariant(MasterMaterial_Masked, TEXT("M_RoseMaster_Masked"),
                BLEND_Masked);
  EnsureVariant(MasterMaterial_Translucent, TEXT("M_RoseMaster_Translucent"),
                BLEND_Translucent);
}

UTexture2D *URoseImporter::LoadRoseTexture(const FString &RP) {
  FString AB = FPaths::GetBaseFilename(RP);
  FString PN = TEXT("/Game/Rose/Imported/Textures/") + AB;

  // Check if texture already loaded
  UTexture2D *Existing =
      FindObject<UTexture2D>(nullptr, *(PN + TEXT(".") + AB));
  if (Existing) {
    return Existing;
  }

  FString AP = FPaths::Combine(RoseRootPath, RP);
  if (!FPaths::FileExists(AP)) {
    FString DXT1 = FPaths::ChangeExtension(AP, TEXT("dds"));
    if (FPaths::FileExists(DXT1)) {
      AP = DXT1;
    } else {
      UE_LOG(LogRoseImporter, Error, TEXT("Texture not found: %s"), *AP);
      return nullptr;
    }
  }

  UE_LOG(LogRoseImporter, Log,
         TEXT("Attempting to load texture: %s -> Resolved: %s"), *RP, *AP);

  if (!FPaths::FileExists(AP)) {
    UE_LOG(LogRoseImporter, Error, TEXT("Texture File NOT FOUND: %s"), *AP);
    return nullptr;
  }

  TArray<uint8> FD;
  if (FFileHelper::LoadFileToArray(FD, *AP) && FD.Num() > 128) {
    int32 W = *(int32 *)&FD[16], H = *(int32 *)&FD[12], F = *(int32 *)&FD[84];
    UE_LOG(LogRoseImporter, Log,
           TEXT("[Texture] File loaded: %d bytes, Format: 0x%08X, Size: %dx%d"),
           FD.Num(), F, W, H);

    TArray<uint8> DecompressedData;

    if (F == 0x33545844) { // DXT3
      UE_LOG(LogRoseImporter, Log, TEXT("[Texture] Decompressing DXT3"));
      DecompressedData.SetNumUninitialized(W * H * 4);
      for (int y = 0; y < H; y += 4)
        for (int x = 0; x < W; x += 4)
          DecompressDXT3Block(FD.GetData() + 128 +
                                  ((y / 4) * (W / 4) + (x / 4)) * 16,
                              DecompressedData.GetData() + (y * W + x) * 4, W);
    } else if (F == 0x31545844) { // DXT1
      UE_LOG(LogRoseImporter, Log, TEXT("[Texture] Decompressing DXT1"));
      DecompressedData.SetNumUninitialized(W * H * 4);
      for (int y = 0; y < H; y += 4)
        for (int x = 0; x < W; x += 4)
          DecompressDXT1Block(FD.GetData() + 128 +
                                  ((y / 4) * (W / 4) + (x / 4)) * 8,
                              DecompressedData.GetData() + (y * W + x) * 4, W);
    } else {
      UE_LOG(LogRoseImporter, Error,
             TEXT("[Texture] Unsupported DDS format: 0x%08X"), F);
      return nullptr;
    }

    // Convert BGRA to RGBA for PNG
    for (int i = 0; i < DecompressedData.Num(); i += 4) {
      uint8 Temp = DecompressedData[i];
      DecompressedData[i] = DecompressedData[i + 2];
      DecompressedData[i + 2] = Temp;
    }

    // Create PNG file in temp directory
    FString TempPNGPath = FPaths::CreateTempFilename(
        *FPaths::ProjectSavedDir(), TEXT("RoseTex_"), TEXT(".png"));

    // Save as PNG using IImageWrapper
    IImageWrapperModule &ImageWrapperModule =
        FModuleManager::LoadModuleChecked<IImageWrapperModule>(
            FName("ImageWrapper"));
    TSharedPtr<IImageWrapper> ImageWrapper =
        ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

    if (ImageWrapper.IsValid() &&
        ImageWrapper->SetRaw(DecompressedData.GetData(), DecompressedData.Num(),
                             W, H, ERGBFormat::RGBA, 8)) {
      const TArray64<uint8> &PNGData = ImageWrapper->GetCompressed(100);
      if (FFileHelper::SaveArrayToFile(PNGData, *TempPNGPath)) {
        UE_LOG(LogRoseImporter, Log, TEXT("[Texture] Saved temp PNG: %s"),
               *TempPNGPath);

        // Import via UTextureFactory
        UTextureFactory *TextureFactory = NewObject<UTextureFactory>();
        TextureFactory->SuppressImportOverwriteDialog();

        UAssetImportTask *ImportTask = NewObject<UAssetImportTask>();
        ImportTask->Filename = TempPNGPath;
        ImportTask->DestinationPath = TEXT("/Game/Rose/Imported/Textures");
        ImportTask->DestinationName = AB;
        ImportTask->bSave = true;
        ImportTask->bAutomated = true;
        ImportTask->bReplaceExisting = true;
        ImportTask->Factory = TextureFactory;

        FAssetToolsModule &AssetToolsModule =
            FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
        AssetToolsModule.Get().ImportAssetTasks({ImportTask});

        // Clean up temp file
        IFileManager::Get().Delete(*TempPNGPath);

        if (ImportTask->GetObjects().Num() > 0) {
          UTexture2D *ImportedTexture =
              Cast<UTexture2D>(ImportTask->GetObjects()[0]);
          if (ImportedTexture) {
            UE_LOG(LogRoseImporter, Log,
                   TEXT("[Texture] Successfully imported via factory: %s"),
                   *AB);
            return ImportedTexture;
          }
        }

        UE_LOG(LogRoseImporter, Error, TEXT("[Texture] Factory import failed"));
      } else {
        UE_LOG(LogRoseImporter, Error,
               TEXT("[Texture] Failed to save temp PNG"));
      }
    } else {
      UE_LOG(LogRoseImporter, Error,
             TEXT("[Texture] Failed to create PNG wrapper"));
    }
  } else {
    UE_LOG(
        LogRoseImporter, Error,
        TEXT("[Texture] Failed to load file or file too small: %s (%d bytes)"),
        *AP, FD.Num());
  }
  return nullptr;
}

void URoseImporter::DecompressDXT3Block(const uint8 *B, uint8 *D, int32 S) {
  uint8 A[16];
  for (int i = 0; i < 8; ++i) {
    A[i * 2] = (B[i] & 0x0F) * 17;
    A[i * 2 + 1] = (B[i] >> 4) * 17;
  }
  const uint8 *CB = B + 8;
  uint16 C0 = *(uint16 *)CB, C1 = *(uint16 *)(CB + 2);
  uint32 IT = *(uint32 *)(CB + 4);
  FColor C[4];
  auto Dec = [](uint16 V, FColor &O) {
    O.R = ((V & 0xF800) >> 8) | ((V & 0xF800) >> 13);
    O.G = ((V & 0x07E0) >> 3) | ((V & 0x07E0) >> 9);
    O.B = ((V & 0x001F) << 3) | ((V & 0x001F) >> 2);
    O.A = 255;
  };
  Dec(C0, C[0]);
  Dec(C1, C[1]);
  C[2].R = (2 * C[0].R + C[1].R) / 3;
  C[2].G = (2 * C[0].G + C[1].G) / 3;
  C[2].B = (2 * C[0].B + C[1].B) / 3;
  C[3].R = (C[0].R + 2 * C[1].R) / 3;
  C[3].G = (C[0].G + 2 * C[1].G) / 3;
  C[3].B = (C[0].B + 2 * C[1].B) / 3;
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) {
      uint8 pi = y * 4 + x, ci = (IT >> (pi * 2)) & 0x03;
      FColor f = C[ci];
      f.A = A[pi];
      int32 di = (y * S + x) * 4;
      D[di] = f.B;
      D[di + 1] = f.G;
      D[di + 2] = f.R;
      D[di + 3] = f.A;
    }
}

void URoseImporter::DecompressDXT1Block(const uint8 *B, uint8 *D, int32 S) {
  // DXT1: 8 bytes per 4x4 block (no explicit alpha)
  uint16 C0 = *(uint16 *)B, C1 = *(uint16 *)(B + 2);
  uint32 IT = *(uint32 *)(B + 4);

  FColor C[4];
  auto Dec = [](uint16 V, FColor &O) {
    O.R = ((V & 0xF800) >> 8) | ((V & 0xF800) >> 13);
    O.G = ((V & 0x07E0) >> 3) | ((V & 0x07E0) >> 9);
    O.B = ((V & 0x001F) << 3) | ((V & 0x001F) >> 2);
    O.A = 255;
  };

  Dec(C0, C[0]);
  Dec(C1, C[1]);

  if (C0 > C1) {
    C[2].R = (2 * C[0].R + C[1].R) / 3;
    C[2].G = (2 * C[0].G + C[1].G) / 3;
    C[2].B = (2 * C[0].B + C[1].B) / 3;
    C[2].A = 255;
    C[3].R = (C[0].R + 2 * C[1].R) / 3;
    C[3].G = (C[0].G + 2 * C[1].G) / 3;
    C[3].B = (C[0].B + 2 * C[1].B) / 3;
    C[3].A = 255;
  } else {
    C[2].R = (C[0].R + C[1].R) / 2;
    C[2].G = (C[0].G + C[1].G) / 2;
    C[2].B = (C[0].B + C[1].B) / 2;
    C[2].A = 255;
    C[3].R = 0;
    C[3].G = 0;
    C[3].B = 0;
    C[3].A = 0; // Transparent for 1-bit alpha
  }

  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      uint8 pi = y * 4 + x, ci = (IT >> (pi * 2)) & 0x03;
      FColor f = C[ci];
      int32 di = (y * S + x) * 4;
      D[di] = f.B;
      D[di + 1] = f.G;
      D[di + 2] = f.R;
      D[di + 3] = f.A;
    }
  }
}

bool URoseImporter::ExportMeshToFBX(UStaticMesh *Mesh, const FString &FBXPath) {
  if (!Mesh)
    return false;

  UAssetExportTask *ExportTask = NewObject<UAssetExportTask>();
  ExportTask->Object = Mesh;
  ExportTask->Filename = FBXPath;
  ExportTask->bSelected = false;
  ExportTask->bReplaceIdentical = true;
  ExportTask->bPrompt = false;
  ExportTask->bAutomated = true;
  ExportTask->bUseFileArchive = false;
  ExportTask->bWriteEmptyFiles = false;

  UFbxExportOption *ExportOptions = NewObject<UFbxExportOption>();
  ExportOptions->VertexColor = false;
  ExportOptions->LevelOfDetail = false;
  ExportOptions->Collision = false;
  ExportTask->Options = ExportOptions;

  // Find exporter
  UExporter *Exporter = UExporter::FindExporter(Mesh, TEXT("FBX"));

  if (!Exporter) {
    UE_LOG(LogRoseImporter, Error, TEXT("Could not find FBX exporter"));
    return false;
  }

  ExportTask->Exporter = Exporter;

  return UExporter::RunAssetExportTask(ExportTask);
}

UStaticMesh *URoseImporter::ImportFBXMesh(const FString &FBXPath,
                                          const FString &DestName) {
  UFbxFactory *FbxFactory = NewObject<UFbxFactory>();
  FbxFactory->AddToRoot(); // Prevent GC

  // Configure import options via ImportUI
  if (!FbxFactory->ImportUI) {
    FbxFactory->ImportUI = NewObject<UFbxImportUI>();
  }

  FbxFactory->ImportUI->bImportMaterials = false;
  FbxFactory->ImportUI->bImportTextures = false;
  FbxFactory->ImportUI->bImportAsSkeletal = false;
  FbxFactory->ImportUI->MeshTypeToImport = FBXIT_StaticMesh;
  FbxFactory->ImportUI->bAutomatedImportShouldDetectType = false;

  // Static Mesh specific options
  if (!FbxFactory->ImportUI->StaticMeshImportData) {
    FbxFactory->ImportUI->StaticMeshImportData =
        NewObject<UFbxStaticMeshImportData>();
  }
  FbxFactory->ImportUI->StaticMeshImportData->bCombineMeshes = true;
  FbxFactory->ImportUI->StaticMeshImportData->bGenerateLightmapUVs = false;
  FbxFactory->ImportUI->StaticMeshImportData->bAutoGenerateCollision = true;

  UAssetImportTask *ImportTask = NewObject<UAssetImportTask>();
  ImportTask->Filename = FBXPath;
  ImportTask->DestinationPath = TEXT("/Game/Rose/Imported/Meshes");
  ImportTask->DestinationName = DestName;
  ImportTask->bSave = true;
  ImportTask->bAutomated = true;
  ImportTask->bReplaceExisting = true;
  ImportTask->Factory = FbxFactory;

  FAssetToolsModule &AssetToolsModule =
      FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
  AssetToolsModule.Get().ImportAssetTasks({ImportTask});

  FbxFactory->RemoveFromRoot(); // Allow GC

  if (ImportTask->GetObjects().Num() > 0) {
    return Cast<UStaticMesh>(ImportTask->GetObjects()[0]);
  }

  return nullptr;
}

UStaticMesh *URoseImporter::ImportRoseMesh(const FString &MP,
                                           const FRoseZSC::FMaterialEntry *M,
                                           const FString &RF) {
  FString CP = MP;
  CP.ReplaceInline(TEXT("\\"), TEXT("/"));
  FString BN = FPaths::GetBaseFilename(CP),
          MS = (M && !M->TexturePath.IsEmpty())
                   ? ObjectTools::SanitizeObjectName(
                         FPaths::GetBaseFilename(M->TexturePath))
                   : TEXT("NoMat");

  // Fix: Use consistent AssetName (BaseName + Suffix) for both check and
  // creation
  FString AssetName = ObjectTools::SanitizeObjectName(BN) + TEXT("_") + MS;
  FString PN = TEXT("/Game/Rose/Imported/Meshes/") + AssetName;
  FString MeshFullPath = PN + TEXT(".") + AssetName;

  if (UStaticMesh *E = FindObject<UStaticMesh>(nullptr, *MeshFullPath)) {
    UpdateMeshMaterial(E, M);
    return E;
  }
  if (UStaticMesh *E = LoadObject<UStaticMesh>(nullptr, *MeshFullPath)) {
    UpdateMeshMaterial(E, M);
    return E;
  }
  FRoseZMS ZMS;
  if (!ZMS.Load(FPaths::Combine(RF, CP))) {
    UE_LOG(LogRoseImporter, Error,
           TEXT("Failed to load ZMS file: '%s' (Root='%s', Rel='%s')"),
           *FPaths::Combine(RF, CP), *RF, *CP);
    return nullptr;
  }

  UStaticMesh *Mesh = NewObject<UStaticMesh>(
      GetTransientPackage(), *ObjectTools::SanitizeObjectName(BN), RF_Public);
  FMeshDescription MD;
  FStaticMeshAttributes(MD).Register();
  FPolygonGroupID PG = MD.CreatePolygonGroup();
  FStaticMeshAttributes(MD).GetPolygonGroupMaterialSlotNames()[PG] =
      FName("RoseMaterial");
  TArray<FVertexID> VIDs;
  for (int i = 0; i < ZMS.Vertices.Num(); ++i)
    VIDs.Add(MD.CreateVertex());
  TArray<FVertexInstanceID> VInsts;
  auto VPos = FStaticMeshAttributes(MD).GetVertexPositions();
  auto VNorms = FStaticMeshAttributes(MD).GetVertexInstanceNormals();
  // Detect active UV channels and Variance
  bool bHasUV1 = false, bHasUV2 = false, bHasUV3 = false, bHasUV4 = false;
  FVector2f MinUV1(FLT_MAX, FLT_MAX), MaxUV1(-FLT_MAX, -FLT_MAX);
  FVector2f MinUV2(FLT_MAX, FLT_MAX), MaxUV2(-FLT_MAX, -FLT_MAX);

  for (const auto &V : ZMS.Vertices) {
    if (!V.UV1.IsZero())
      bHasUV1 = true;
    if (!V.UV2.IsZero())
      bHasUV2 = true;
    if (!V.UV3.IsZero())
      bHasUV3 = true;
    if (!V.UV4.IsZero())
      bHasUV4 = true;

    MinUV1.X = FMath::Min(MinUV1.X, V.UV1.X);
    MinUV1.Y = FMath::Min(MinUV1.Y, V.UV1.Y);
    MaxUV1.X = FMath::Max(MaxUV1.X, V.UV1.X);
    MaxUV1.Y = FMath::Max(MaxUV1.Y, V.UV1.Y);

    MinUV2.X = FMath::Min(MinUV2.X, V.UV2.X);
    MinUV2.Y = FMath::Min(MinUV2.Y, V.UV2.Y);
    MaxUV2.X = FMath::Max(MaxUV2.X, V.UV2.X);
    MaxUV2.Y = FMath::Max(MaxUV2.Y, V.UV2.Y);
  }

  float ExtentUV1 = (MaxUV1 - MinUV1).Size();
  float ExtentUV2 = (MaxUV2 - MinUV2).Size();

  int32 SrcCh0 = 1;
  if (ExtentUV1 < 0.001f && ExtentUV2 > 0.01f) {
    SrcCh0 = 2;
    UE_LOG(LogRoseImporter, Warning,
           TEXT("[SmartUV] Swapping UV2 to Channel 0 for mesh '%s' (UV1 "
                "Extent=%f, UV2 Extent=%f)"),
           *BN, ExtentUV1, ExtentUV2);
  }

  int32 NumUVs = 1;
  if (bHasUV2)
    NumUVs = 2;
  if (bHasUV3)
    NumUVs = 3;
  if (bHasUV4)
    NumUVs = 4;

  auto VUVs = FStaticMeshAttributes(MD).GetVertexInstanceUVs();
  VUVs.SetNumChannels(NumUVs);

  for (int i = 0; i < ZMS.Vertices.Num(); ++i) {
    FVertexInstanceID ID = MD.CreateVertexInstance(VIDs[i]);
    VInsts.Add(ID);
    // Apply rtuPosition (Y-flip) + 100x scale, matching reference Zms.h:
    // vertexPositions[i] = rtuPosition(read<FVector>()) * 100;
    VPos[VIDs[i]] = FVector3f(ZMS.Vertices[i].Position.X * 100.0f,
                              -ZMS.Vertices[i].Position.Y * 100.0f,
                              ZMS.Vertices[i].Position.Z * 100.0f);
    // Apply rtuPosition Y-flip to normals too (must match vertex position flip)
    FVector3f N = ZMS.Vertices[i].Normal;
    N.Y = -N.Y;
    VNorms[ID] = N;

    // Set UVs based on Smart Decision
    if (SrcCh0 == 2) {
      VUVs.Set(ID, 0, ZMS.Vertices[i].UV2);
      if (NumUVs >= 2)
        VUVs.Set(ID, 1, ZMS.Vertices[i].UV1);
    } else {
      VUVs.Set(ID, 0, ZMS.Vertices[i].UV1);
      if (bHasUV2 && NumUVs >= 2)
        VUVs.Set(ID, 1, ZMS.Vertices[i].UV2);
    }

    if (bHasUV3 && NumUVs >= 3)
      VUVs.Set(ID, 2, ZMS.Vertices[i].UV3);
    if (bHasUV4 && NumUVs >= 4)
      VUVs.Set(ID, 3, ZMS.Vertices[i].UV4);
  }
  for (int i = 0; i < ZMS.Indices.Num(); i += 3) {
    // With rtuPosition Y-flip on vertices, use original winding order
    // (matching reference plugin which also uses rtuPosition + original order)
    TArray<FVertexInstanceID> T;
    T.Add(VInsts[ZMS.Indices[i]]);
    T.Add(VInsts[ZMS.Indices[i + 1]]);
    T.Add(VInsts[ZMS.Indices[i + 2]]);
    MD.CreateTriangle(PG, T);
  }
  Mesh->GetStaticMaterials().Add(
      FStaticMaterial(nullptr, FName("RoseMaterial")));
  FStaticMeshSourceModel &SM = Mesh->AddSourceModel();
  SM.BuildSettings.bRecomputeNormals = SM.BuildSettings.bRecomputeTangents =
      false;
  TArray<const FMeshDescription *> MDPs;
  MDPs.Add(&MD);
  Mesh->BuildFromMeshDescriptions(MDPs);

  // Export to FBX and Import via Factory
  FString TempFBXPath = FPaths::CreateTempFilename(
      *FPaths::ProjectSavedDir(), TEXT("RoseMesh_"), TEXT(".fbx"));

  UStaticMesh *FinalMesh = nullptr;

  if (ExportMeshToFBX(Mesh, TempFBXPath)) {
    UE_LOG(LogRoseImporter, Log, TEXT("[ImportMesh] Exported temp FBX: %s"),
           *TempFBXPath);
    // Fix: Pass full AssetName (BN_MS) to Import logic
    FinalMesh = ImportFBXMesh(TempFBXPath, AssetName);
    IFileManager::Get().Delete(*TempFBXPath);
  } else {
    UE_LOG(LogRoseImporter, Error, TEXT("[ImportMesh] Failed to export FBX"));
  }

  if (FinalMesh) {
    UpdateMeshMaterial(FinalMesh, M);
    return FinalMesh;
  }

  return nullptr;
}

void URoseImporter::UpdateMeshMaterial(UStaticMesh *Mesh,
                                       const FRoseZSC::FMaterialEntry *M) {
  if (!Mesh || !M)
    return;

  // Determine Material Name from TexturePath if possible
  FString MS = TEXT("NoMat");
  if (!M->TexturePath.IsEmpty()) {
    MS = ObjectTools::SanitizeObjectName(
        FPaths::GetBaseFilename(M->TexturePath));
  }

  FString MPN = TEXT("/Game/Rose/Imported/Materials/M_") + MS;
  // FIX: Full object path = PackagePath.ObjectName
  // MIC is created with name MS inside package M_MS
  FString FullMPN = MPN + TEXT(".") + MS;

  // Performance Optimization: Check Cache
  if (ProcessedMaterialPaths.Contains(MPN)) {
    // Already processed (updated & saved) this session.
    // We just need to ensure the Mesh uses it.

    // Try to find it in memory first
    UMaterialInstanceConstant *MIC =
        FindObject<UMaterialInstanceConstant>(nullptr, *FullMPN);
    if (!MIC) {
      // Load it if not in memory
      MIC = LoadObject<UMaterialInstanceConstant>(nullptr, *FullMPN);
    }

    if (MIC) {
      if (Mesh->GetStaticMaterials().Num() > 0) {
        // Only assign if different (avoid dirtying mesh unnecessarily)
        if (Mesh->GetStaticMaterials()[0].MaterialInterface != MIC) {
          Mesh->GetStaticMaterials()[0].MaterialInterface = MIC;
          Mesh->PostEditChange();
        }
      } else {
        Mesh->GetStaticMaterials().Add(FStaticMaterial(MIC));
        Mesh->PostEditChange();
      }
    }
    return;
  }

  EnsureMasterMaterial();
  UMaterialInstanceConstant *MIC =
      FindObject<UMaterialInstanceConstant>(nullptr, *FullMPN);

  if (!MIC) {
    UPackage *MatPkg = CreatePackage(*MPN);
    MatPkg->FullyLoad();

    auto MatFactory = NewObject<UMaterialInstanceConstantFactoryNew>();
    MIC = (UMaterialInstanceConstant *)MatFactory->FactoryCreateNew(
        UMaterialInstanceConstant::StaticClass(), MatPkg, *MS,
        RF_Public | RF_Standalone, nullptr, GWarn);
  }

  if (MIC) {
    // ALWAYS Update Parameters
    MIC->SetParentEditorOnly(MasterMaterial);

    // Configure Transparency
    bool bTranslucent = false;
    bool bMasked = false;

    if (M->AlphaEnabled) {
      if (M->AlphaTest > 0) {
        bMasked = true;
      } else if (M->BlendType != 0) {
        bTranslucent = true;
      }
    }

    UMaterial *ParentMat = MasterMaterial;
    if (bMasked) {
      ParentMat = MasterMaterial_Masked;
    } else if (bTranslucent) {
      ParentMat = MasterMaterial_Translucent;
    }

    if (!ParentMat)
      ParentMat = MasterMaterial;
    MIC->SetParentEditorOnly(ParentMat);

    if (M->TexturePath.Len() > 0) {
      UTexture2D *T = LoadRoseTexture(M->TexturePath);
      if (T) {
        MIC->SetTextureParameterValueEditorOnly(
            FMaterialParameterInfo(TEXT("BaseTexture")), T);
      } else {
        UE_LOG(LogRoseImporter, Error, TEXT("Failed to load texture '%s'"),
               *M->TexturePath);
      }
    }

    if (M->TwoSided) {
      MIC->BasePropertyOverrides.bOverride_TwoSided = true;
      MIC->BasePropertyOverrides.TwoSided = true;
    }

    if (M->Red > 0.01f || M->Green > 0.01f || M->Blue > 0.01f) {
      MIC->SetVectorParameterValueEditorOnly(
          FMaterialParameterInfo(TEXT("TintColor")),
          FLinearColor(M->Red, M->Green, M->Blue, 1.0f));
    }

    MIC->PostEditChange();
    FAssetRegistryModule::AssetCreated(MIC);

    FString MatPackageFileName = FPackageName::LongPackageNameToFilename(
        MIC->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());

    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    UPackage::SavePackage(MIC->GetPackage(), MIC, *MatPackageFileName,
                          SaveArgs);

    // Mark as Processed
    ProcessedMaterialPaths.Add(MPN);

    // Assign to Mesh
    if (Mesh->GetStaticMaterials().Num() > 0) {
      Mesh->GetStaticMaterials()[0].MaterialInterface = MIC;
    } else {
      Mesh->GetStaticMaterials().Add(FStaticMaterial(MIC));
    }
    Mesh->PostEditChange();
  }
}

UTexture2D *URoseImporter::CreateTextureAssetDXT(UObject *Outer, FName Name,
                                                 int32 W, int32 H,
                                                 EPixelFormat F,
                                                 const TArray<uint8> &D) {
  UTexture2D *T = UTexture2D::CreateTransient(W, H, F);
  if (!T)
    return nullptr;
  T->Rename(*Name.ToString(), Outer);
  T->ClearFlags(RF_Transient);
  T->SetFlags(RF_Public | RF_Standalone);
  if (T->GetPlatformData() && T->GetPlatformData()->Mips.Num() > 0) {
    FTexture2DMipMap &M = T->GetPlatformData()->Mips[0];
    void *DP = M.BulkData.Lock(LOCK_READ_WRITE);
    FMemory::Memcpy(
        DP, D.GetData(),
        FMath::Min((int32)M.BulkData.GetBulkDataSize(), (int32)D.Num()));
    M.BulkData.Unlock();
  }
  T->UpdateResource();
  return T;
}

// ============================================================================
// ZONETYPEINFO and TileSet Support Functions
// ============================================================================

bool URoseImporter::LoadZoneTypeInfo(const FString &RoseDataPath) {
  if (bZoneTypeInfoLoaded) {
    return true; // Already loaded
  }

  // Path: 3Ddata/TERRAIN/TILES/ZONETYPEINFO.STB
  FString STBPath = FPaths::Combine(
      RoseDataPath, TEXT("3Ddata/TERRAIN/TILES/ZONETYPEINFO.STB"));

  // Try alternate path formats
  if (!FPaths::FileExists(STBPath)) {
    STBPath = FPaths::Combine(RoseDataPath,
                              TEXT("3DData/TERRAIN/TILES/ZONETYPEINFO.STB"));
  }
  if (!FPaths::FileExists(STBPath)) {
    STBPath = FPaths::Combine(RoseDataPath,
                              TEXT("3ddata/terrain/tiles/zonetypeinfo.stb"));
  }

  if (!FPaths::FileExists(STBPath)) {
    UE_LOG(LogRoseImporter, Warning, TEXT("ZONETYPEINFO.STB not found at: %s"),
           *STBPath);
    return false;
  }

  if (ZoneTypeInfoSTB.Load(STBPath)) {
    bZoneTypeInfoLoaded = true;
    UE_LOG(LogRoseImporter, Log, TEXT("Loaded ZONETYPEINFO.STB: %d zone types"),
           ZoneTypeInfoSTB.GetRowCount());
    return true;
  }

  UE_LOG(LogRoseImporter, Error, TEXT("Failed to parse ZONETYPEINFO.STB: %s"),
         *STBPath);
  return false;
}

FString URoseImporter::GetTileSetPath(int32 ZoneType) const {
  if (!bZoneTypeInfoLoaded) {
    UE_LOG(LogRoseImporter, Warning,
           TEXT("ZONETYPEINFO not loaded, cannot get TileSet path"));
    return FString();
  }

  if (ZoneType < 0 || ZoneType >= ZoneTypeInfoSTB.GetRowCount()) {
    UE_LOG(LogRoseImporter, Warning, TEXT("Invalid ZoneType %d (max: %d)"),
           ZoneType, ZoneTypeInfoSTB.GetRowCount() - 1);
    return FString();
  }

  // Column 6 contains the TileSet filename (e.g., "GRASS.TSI")
  FString TileSetFile = ZoneTypeInfoSTB.GetCell(ZoneType, 6);

  if (TileSetFile.IsEmpty()) {
    UE_LOG(LogRoseImporter, Warning, TEXT("No TileSet defined for ZoneType %d"),
           ZoneType);
    return FString();
  }

  // Full path: 3Ddata/ESTB/<TileSetFile>
  FString FullPath =
      FPaths::Combine(RoseRootPath, TEXT("3Ddata/ESTB"), TileSetFile);

  UE_LOG(LogRoseImporter, Log, TEXT("ZoneType %d -> TileSet: %s"), ZoneType,
         *FullPath);

  return FullPath;
}

bool URoseImporter::LoadTileSetForZone(int32 ZoneType,
                                       FRoseTileSet &OutTileSet) {
  // Ensure ZONETYPEINFO is loaded
  if (!bZoneTypeInfoLoaded) {
    if (!LoadZoneTypeInfo(RoseRootPath)) {
      UE_LOG(LogRoseImporter, Warning,
             TEXT("Cannot load TileSet - ZONETYPEINFO not available"));
      return false;
    }
  }

  // Get TileSet path
  FString TileSetPath = GetTileSetPath(ZoneType);
  if (TileSetPath.IsEmpty()) {
    return false;
  }

  // TileSet files are actually STB format
  FRoseSTB TileSetSTB;
  if (!TileSetSTB.Load(TileSetPath)) {
    UE_LOG(LogRoseImporter, Error, TEXT("Failed to load TileSet STB: %s"),
           *TileSetPath);
    return false;
  }

  // Parse TileSet from STB
  if (!OutTileSet.LoadFromSTB(TileSetSTB)) {
    UE_LOG(LogRoseImporter, Error, TEXT("Failed to parse TileSet: %s"),
           *TileSetPath);
    return false;
  }

  UE_LOG(LogRoseImporter, Log,
         TEXT("Loaded TileSet for ZoneType %d: %d brushes"), ZoneType,
         OutTileSet.Brushes.Num());

  return true;
}

const FRoseTileBrush *URoseImporter::FindBrushForTile(int32 TileID) const {
  if (!bCurrentTileSetValid) {
    return nullptr;
  }

  // Iterate brushes to find which one contains this TileID
  // A TileID belongs to a brush if it is within [TileNumber, TileNumber +
  // TileCount] Or one of the other ranges (TileNumber0, TileNumberF)

  for (const FRoseTileBrush &Brush : CurrentTileSet.Brushes) {
    if (TileID >= Brush.TileNumber &&
        TileID < Brush.TileNumber + Brush.TileCount) {
      return &Brush;
    }
    if (TileID >= Brush.TileNumber0 &&
        TileID < Brush.TileNumber0 + Brush.TileCount0) {
      return &Brush;
    }
    if (TileID >= Brush.TileNumberF &&
        TileID < Brush.TileNumberF + Brush.TileCountF) {
      return &Brush;
    }
  }

  return nullptr;
}

bool URoseImporter::LoadZSCsFromListZone(const FString &RoseDataPath,
                                         const TArray<FString> &ZoneNames) {
  // Try to find LIST_ZONE.STB
  FString ListZonePath =
      FPaths::Combine(RoseDataPath, TEXT("3Ddata/STB/LIST_ZONE.STB"));
  if (!FPaths::FileExists(ListZonePath)) {
    ListZonePath =
        FPaths::Combine(RoseDataPath, TEXT("3Ddata/stb/LIST_ZONE.STB"));
  }

  if (!FPaths::FileExists(ListZonePath)) {
    UE_LOG(LogRoseImporter, Warning, TEXT("LIST_ZONE.STB not found at: %s"),
           *ListZonePath);
    return false;
  }

  FRoseSTB ListZoneSTB;
  if (!ListZoneSTB.Load(ListZonePath)) {
    UE_LOG(LogRoseImporter, Error, TEXT("Failed to load LIST_ZONE.STB: %s"),
           *ListZonePath);
    return false;
  }

  // Debug: Search for zone in specific columns
  UE_LOG(LogRoseImporter, Log, TEXT("Scanning LIST_ZONE.STB for zone: %s..."),
         *ZoneNames[0]);

  // DEBUG: Verify Header (Row 0)
  if (ListZoneSTB.GetRowCount() > 0) {
    FString HeaderStr = TEXT("Header (Row 0): ");
    for (int32 j = 0; j < FMath::Min(20, ListZoneSTB.GetColumnCount()); ++j) {
      HeaderStr +=
          FString::Printf(TEXT("[%d]='%s' "), j, *ListZoneSTB.GetCell(0, j));
    }
    UE_LOG(LogRoseImporter, Log, TEXT("%s"), *HeaderStr);
  }

  // Find the row for this zone
  int32 FoundRow = -1;
  FString MatchedZoneName;

  // Dynamic Column Lookup: Find "ZON" column index
  int32 ZonColumnIndex = 3; // Default fallback
  if (ListZoneSTB.GetRowCount() > 0) {
    for (int32 j = 0; j < ListZoneSTB.GetColumnCount(); ++j) {
      if (ListZoneSTB.GetCell(0, j).ToUpper() == TEXT("ZON")) {
        ZonColumnIndex = j;
        UE_LOG(LogRoseImporter, Log, TEXT("Found 'ZON' column at index %d"), j);
        break;
      }
    }
  }

  for (const FString &SearchNameCandidate : ZoneNames) {
    FString SearchName = SearchNameCandidate.ToUpper();

    for (int32 i = 0; i < ListZoneSTB.GetRowCount(); ++i) {
      // Safe cell access helper
      auto SafeGetCell = [&](int32 Row, int32 Col) -> FString {
        if (Row >= 0 && Row < ListZoneSTB.GetRowCount() && Col >= 0 &&
            Col < ListZoneSTB.GetColumnCount()) {
          return ListZoneSTB.GetCell(Row, Col);
        }
        return FString();
      };

      // Column 1 is usually the Shouting/Zone ID (e.g. JDT01)
      FString RowName1 = FPaths::GetBaseFilename(SafeGetCell(i, 1)).ToUpper();

      // Column 2 is sometimes used? Check both.
      FString RowName2 = FPaths::GetBaseFilename(SafeGetCell(i, 2)).ToUpper();

      // Use dynamic ZON column index
      FString RowZonFile =
          FPaths::GetBaseFilename(SafeGetCell(i, ZonColumnIndex)).ToUpper();

      if (RowName1 == SearchName || RowName2 == SearchName ||
          RowZonFile == SearchName) {
        UE_LOG(LogRoseImporter, Log,
               TEXT("MATCH FOUND at Row %d: Col1='%s', Col2='%s', Col%d='%s' "
                    "(Search='%s')"),
               i, *RowName1, *RowName2, ZonColumnIndex, *RowZonFile,
               *SearchName);
        FoundRow = i;
        MatchedZoneName = SearchNameCandidate;
        break;
      }
    }
    if (FoundRow != -1)
      break;
  }

  if (FoundRow == -1) {
    FString JoinedCandidates;
    for (const auto &C : ZoneNames)
      JoinedCandidates += C + TEXT(", ");
    UE_LOG(LogRoseImporter, Warning,
           TEXT("Zone candidates [%s] not found in LIST_ZONE.STB"),
           *JoinedCandidates);
    return false;
  }

  // Read ZSC paths
  // C# says Cells[mapID][12] for Decoration, [13] for Construction
  FString DecoZSCFile = ListZoneSTB.GetCell(FoundRow, 12);
  FString CnstZSCFile = ListZoneSTB.GetCell(FoundRow, 13);

  // Clean paths if they contain "3DData/" prefix (case insensitive)
  auto CleanPath = [](const FString &InPath) -> FString {
    FString Temp = InPath;
    Temp.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (Temp.StartsWith(TEXT("3DData/"), ESearchCase::IgnoreCase)) {
      return Temp.RightChop(7); // Remove "3DData/"
    }
    return Temp;
  };

  DecoZSCFile = CleanPath(DecoZSCFile);
  CnstZSCFile = CleanPath(CnstZSCFile);

  UE_LOG(
      LogRoseImporter, Log,
      TEXT("Zone '%s' found in LIST_ZONE (Row %d). RawDeco: %s, RawCnst: %s"),
      *MatchedZoneName, FoundRow, *ListZoneSTB.GetCell(FoundRow, 12),
      *ListZoneSTB.GetCell(FoundRow, 13));

  bool bSuccess = true;

  // Load Decoration ZSC
  if (!DecoZSCFile.IsEmpty()) {
    FString Path = FPaths::Combine(RoseDataPath, TEXT("3Ddata"), DecoZSCFile);
    if (DecoZSC.Load(Path)) {
      UE_LOG(LogRoseImporter, Log,
             TEXT("Loaded Decoration ZSC: %d meshes, %d materials"),
             DecoZSC.Meshes.Num(), DecoZSC.Materials.Num());
    } else {
      UE_LOG(LogRoseImporter, Warning, TEXT("Failed to load Deco ZSC: %s"),
             *Path);
      bSuccess = false;
    }
  }

  // Load Construction ZSC
  if (!CnstZSCFile.IsEmpty()) {
    FString Path = FPaths::Combine(RoseDataPath, TEXT("3Ddata"), CnstZSCFile);
    if (CnstZSC.Load(Path)) {
      UE_LOG(LogRoseImporter, Log,
             TEXT("Loaded Construction ZSC: %d meshes, %d materials"),
             CnstZSC.Meshes.Num(), CnstZSC.Materials.Num());
    } else {
      UE_LOG(LogRoseImporter, Warning, TEXT("Failed to load Cnst ZSC: %s"),
             *Path);
      bSuccess = false;
    }
  }

  return bSuccess;
}

bool URoseImporter::SaveRoseAsset(UObject *Asset) {
  if (!Asset)
    return false;

  UPackage *Pkg = Asset->GetPackage();
  if (!Pkg)
    return false;

  Asset->SetFlags(RF_Public | RF_Standalone);
  Pkg->MarkPackageDirty();
  FAssetRegistryModule::AssetCreated(Asset);

  FString PackageFileName = FPackageName::LongPackageNameToFilename(
      Pkg->GetName(), FPackageName::GetAssetPackageExtension());

  // DEBUG: Log where we're saving
  UE_LOG(LogRoseImporter, Warning, TEXT("[SaveAsset] Package: %s -> File: %s"),
         *Pkg->GetName(), *PackageFileName);

  FSavePackageArgs SaveArgs;
  SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
  SaveArgs.Error = GError;
  SaveArgs.SaveFlags = SAVE_NoError;

  if (UPackage::SavePackage(Pkg, Asset, *PackageFileName, SaveArgs)) {
    UE_LOG(LogRoseImporter, Verbose, TEXT("Saved asset: %s"), *PackageFileName);

    // Force Asset Registry to scan the file we just saved
    FAssetRegistryModule &AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
            "AssetRegistry");
    AssetRegistryModule.Get().ScanFilesSynchronous(
        TArray<FString>{PackageFileName}, true);

    UE_LOG(LogRoseImporter, Warning, TEXT("[SaveAsset] File scanned: %s"),
           *PackageFileName);

    return true;
  } else {
    UE_LOG(LogRoseImporter, Error, TEXT("Failed to save asset: %s"),
           *PackageFileName);
    return false;
  }
}
