# 🌹 Bonsoir ROSE Importer — Unreal Engine Plugin

Import **ROSE Online** game assets (maps, terrain, objects, textures) directly into Unreal Engine 5.7.

---

## Prerequisites

| Requirement | Version |
|---|---|
| **Unreal Engine** | 5.7+ |
| **Visual Studio** | 2022 or later (with C++ game development workload) |
| **ROSE Online Client** | A valid ROSE Online client with `3DData/` folder containing game assets |

### Required ROSE Online Files

The plugin reads the following file formats from the ROSE Online client:

| Format | Description |
|---|---|
| `.ZON` | Zone definition (entry point for import) |
| `.ZSC` | Zone Scene Container (object/building definitions, meshes, materials) |
| `.ZMS` | Zone Mesh (3D mesh vertex/triangle data) |
| `.IFO` | Information File Object (object placement per tile) |
| `.HIM` | Heightmap data |
| `.TIL` | Tile texture assignments |
| `.DDS` | Texture files (DirectDraw Surface) |
| `.STB` | String Table Binary (zone lookup tables) |

---

## Installation

1. Copy the `BonsoirUnreal` folder into your Unreal Engine project's `Plugins/` directory:
   ```
   YourProject/
   └── Plugins/
       └── BonsoirUnreal/
           ├── BonsoirUnreal.uplugin
           ├── Source/
           ├── Content/
           └── Config/
   ```
2. Open your project in Unreal Engine — the plugin will compile automatically.
3. Verify the plugin is enabled: **Edit → Plugins → search "Bonsoir"**.

---

## Usage

### Importing a Zone

1. In the **Content Browser**, click the **Import** button (or drag & drop).
2. Navigate to your ROSE Online client folder and select a `.ZON` file.
   - Example: `3DData/MAPS/JUNON/JDT01/JDT01.ZON`
3. The plugin will automatically:
   - Discover the ROSE root path (parent of `3DData/`)
   - Load the associated ZSC files via `LIST_ZONE.STB`
   - Import terrain heightmaps and create a unified landscape
   - Apply landscape layer textures from tile data
   - Import all decoration and building objects with correct transforms
   - Create and assign materials with textures from DDS files

### What Gets Imported

| Asset Type | How It Appears in UE |
|---|---|
| **Terrain** | `ALandscapeProxy` with blended texture layers |
| **Decorations** | HISM (Hierarchical Instanced Static Mesh) components |
| **Buildings** | HISM components |
| **Textures** | `UTexture2D` assets in `/Game/Rose/Imported/Textures/` |
| **Materials** | `UMaterialInstanceConstant` in `/Game/Rose/Imported/Materials/` |
| **Meshes** | `UStaticMesh` in `/Game/Rose/Imported/Meshes/` |

### Output Structure

All imported assets are organized under `/Game/Rose/`:
```
Content/
└── Rose/
    ├── Imported/
    │   ├── Meshes/        — Static meshes from ZMS files
    │   ├── Materials/     — Material instances with textures
    │   └── Textures/      — DDS textures converted to UTexture2D
    └── Materials/         — Master materials (Opaque, Alpha, TwoSided)
```

### Re-importing

To re-import a zone:
1. Delete the previously imported meshes in `/Game/Rose/Imported/Meshes/`
2. Import the `.ZON` file again — existing landscape and objects will be replaced automatically

---

## Features

- ✅ **Full zone import** — terrain + all objects in one step
- ✅ **Automatic ZSC discovery** — finds decoration and building ZSCs via `LIST_ZONE.STB`
- ✅ **Correct coordinate system** — handles ROSE→UE coordinate conversion (Y-flip, scale)
- ✅ **Multi-part objects** — supports objects with multiple mesh parts and correct transform composition
- ✅ **Material handling** — opaque, alpha-blended, two-sided, and alpha-tested materials
- ✅ **HISM instancing** — identical meshes share instances for optimal performance
- ✅ **Unified landscape** — merges all tiles into a single seamless landscape
- ✅ **Landscape layers** — texture blending based on tile data

---

## Changelog

### Version 1.2 (Current)
- **Terrain Blending:** Implemented Height Blending for smoother texture transitions using the alpha channel (e.g., paths over grass).
- **Layer Limit:** Increased landscape layer limit from 24 to 64 per component to prevent missing textures on complex tiles.
- **Texture Logic:** Fixed logic for secondary (overlay) textures to ensure they are correctly counted and assigned.
- **Visual Improvements:** Adjusted default UV scale to 0.25 for more realistic texture density.
- **Stability:** Fixed compilation errors and removed experimental rotation logic.

### Version 1.1
- **Animation Support:** Added support for animated objects (Type 6) and `.IFO` flags.
- **Collision Fixes:** Automatically disabled collision for "grass" objects to prevent player obstruction.
- **Usability:** Added drag-and-drop support for `.STB` zone lists.
- **Tangent Fixes:** Enabled "Recompute Tangents" to fix rendering artifacts on some meshes.

### Version 1.0
- **Initial Release:** Complete import pipeline for `.ZON` files.
- **Features:** Unified Landscape creation, HISM instancing for performance, automatic ZSC discovery, and material generation.

---

## Credits

- **Author:** BarbieSpaghetti
- **Version:** 1.2
- **Engine:** Unreal Engine 5.7
- **License:** Private

---

*Built with ❤️ for the ROSE Online community*
