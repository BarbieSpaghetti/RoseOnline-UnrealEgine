#pragma once

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "CoreMinimal.h"
#include "RoseFormats.h"
#include "RoseImporter.generated.h"

// DECLARE_LOG_CATEGORY_EXTERN(LogRoseImporter, Log, All);

/**
 * Loaded Tile Data
 */
struct FLoadedTile {
  int32 X = 0;
  int32 Y = 0;
  FRoseHIM HIM;
  FRoseTIL TIL;
};

class ALandscape;

/**
 * ROSE Online Zone Importer
 */
UCLASS()
class BONSOIRUNREAL_API URoseImporter : public UObject {
  GENERATED_BODY()

public:
  // Import a .ZON file and create the zone in the world
  bool ImportZone(const FString &ZONPath);

  UMaterial *CreateVertexColorPreviewMaterial();
  void CreateVertexColorVisualization(UStaticMeshComponent *MeshComp);

  UMaterial *CreateDualTextureTestMaterial(const FRoseZON &ZON,
                                           const TArray<FLoadedTile> &AllTiles);
  UMaterial *
  CreateSwitchBasedDualTextureMaterial(const FRoseZON &ZON,
                                       const TArray<FLoadedTile> &AllTiles);

  UMaterial *CreateDecalMaterial(UTexture2D *Texture, int32 TexID);

  void SpawnDecalsForTextures(UWorld *World, const FRoseZON &ZON,
                              const TArray<FLoadedTile> &AllTiles, int32 MinX,
                              int32 MinY);

  void SetupVertexColors(ALandscape *Landscape,
                         const TArray<FLoadedTile> &AllTiles,
                         const FRoseZON &ZON, int32 MinX, int32 MinY);

private:
  // Root path to ROSE Online data
  FString RoseRootPath;

  // Master Material Reference
  UPROPERTY()
  UMaterial *MasterMaterial = nullptr;
  UPROPERTY()
  UMaterial *MasterMaterial_Masked = nullptr;
  UPROPERTY()
  UMaterial *MasterMaterial_Translucent = nullptr;

  // Zone Type Info
  bool bZoneTypeInfoLoaded = false;
  FRoseSTB ZoneTypeInfoSTB;

  // Current TileSet
  bool bCurrentTileSetValid = false;
  FRoseTileSet CurrentTileSet;

  // Loaded ZSC Data
  FRoseZSC DecoZSC;
  FRoseZSC CnstZSC;

  // HISM Management
  TMap<UStaticMesh *, UHierarchicalInstancedStaticMeshComponent *>
      GlobalHISMMap;
  AActor *ZoneObjectsActor = nullptr;

  // Helper Functions
  bool LoadZoneTypeInfo(const FString &RootPath);
  FString GetTileSetPath(int32 ZoneType) const;
  bool LoadTileSetForZone(int32 ZoneType, FRoseTileSet &OutTileSet);
  const FRoseTileBrush *FindBrushForTile(int32 TileID) const;

  bool LoadZSCsFromListZone(const FString &RootPath,
                            const TArray<FString> &Candidates);

  void EnsureMasterMaterial();
  UTexture2D *LoadRoseTexture(const FString &RelPath);

  // Helper for DXT decompression
  void DecompressDXT3Block(const uint8 *B, uint8 *D, int32 S);
  void DecompressDXT1Block(const uint8 *B, uint8 *D, int32 S);
  UTexture2D *CreateTextureAssetDXT(UObject *Outer, FName Name, int32 W,
                                    int32 H, EPixelFormat F,
                                    const TArray<uint8> &D);

  UStaticMesh *ImportRoseMesh(const FString &RelPath,
                              const FRoseZSC::FMaterialEntry *M = nullptr,
                              const FString &RootPath = TEXT(""));

  void UpdateMeshMaterial(UStaticMesh *Mesh, const FRoseZSC::FMaterialEntry *M);

  // FBX Helpers
  bool ExportMeshToFBX(UStaticMesh *Mesh, const FString &FBXPath);
  UStaticMesh *ImportFBXMesh(const FString &FBXPath, const FString &DestName);

  void CreateUnifiedLandscape(const TArray<FLoadedTile> &AllTiles,
                              const FRoseZON &ZON, UWorld *World, int32 MinX,
                              int32 MinY, int32 MaxX, int32 MaxY,
                              const FString &Folder);

  void ProcessHeightmap(const FRoseHIM &HIM, const FRoseTIL &TIL,
                        const FRoseZON &ZON, UWorld *World,
                        const FVector &Offset, const FString &Base,
                        const FString &ZFolder);

  UMaterial *CreateLandscapeMaterial(const FRoseZON &ZON,
                                     const TArray<FLoadedTile> &AllTiles);

  // Per-tile landscape layer helpers
  UMaterial *CreateTileMaterial(const FRoseTIL &TIL, const FRoseZON &ZON,
                                const FString &TileName,
                                TArray<int32> &OutTextureIDs);

  TMap<int32, TArray<uint8>>
  GenerateTileWeightmaps(const FRoseTIL &TIL, const FRoseZON &ZON,
                         const TArray<int32> &TextureIDs);

  void ProcessObjects(const FRoseIFO &IFO, UWorld *World,
                      const FVector &TileOffset, int32 MinX, int32 MinY,
                      int32 ZoneWidth, int32 ZoneHeight);

  UHierarchicalInstancedStaticMeshComponent *
  GetOrCreateHISM(UStaticMesh *Mesh, UMaterialInterface *Material);

  // Helper to save any asset to disk
  bool SaveRoseAsset(UObject *Asset);

  // Cache to avoid redundant material saves
  TSet<FString> ProcessedMaterialPaths;
};
