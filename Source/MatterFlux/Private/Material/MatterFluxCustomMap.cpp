#include "Material/MatterFluxCustomMap.h"

#include "Material/MatterFluxMaterialSimulationRuntime.h"
#include "Material/MatterFluxMaterialWorld.h"

namespace
{
	MatterFlux::Material::FWorldSettings MakeCustomMapWorldSettings(
		const FMatterFluxCustomMapDefinition& Map)
	{
		constexpr int32 ChunkSize = 64;
		const int32 Width = Map.MaximumCellExclusive.X - Map.MinimumCell.X;
		const int32 Height = Map.MaximumCellExclusive.Y - Map.MinimumCell.Y;
		const int32 ActiveRadius = FMath::Clamp(
			FMath::DivideAndRoundUp(FMath::Max(Width, Height), ChunkSize) + 1,
			1,
			16);
		MatterFlux::Material::FWorldSettings Settings;
		Settings.ChunkSize = ChunkSize;
		Settings.ActiveChunkRadius = ActiveRadius;
		const int32 Diameter = ActiveRadius * 2 + 1;
		Settings.MaxActiveChunks = Diameter * Diameter;
		Settings.bUseSurfaceTopology = true;
		Settings.MinSurfaceCell = Map.MinimumCell;
		Settings.MaxSurfaceCellExclusive = Map.MaximumCellExclusive;
		Settings.bCullOutsideSurfaceBounds = true;
		return Settings;
	}

	FIntPoint GetCustomMapFocus(const FMatterFluxCustomMapDefinition& Map)
	{
		return FIntPoint(
			(Map.MinimumCell.X + Map.MaximumCellExclusive.X - 1) / 2,
			(Map.MinimumCell.Y + Map.MaximumCellExclusive.Y - 1) / 2);
	}
}

namespace MatterFlux::Material
{
	bool BuildCustomMap(
		const FName MapId,
		const FMatterFluxContentRegistry& Registry,
		const int32 Seed,
		FChunkedMaterialWorld& OutWorld,
		FCustomMapScene& OutScene,
		FString& OutError)
	{
		OutError.Reset();
		OutScene = FCustomMapScene();
		const FMatterFluxCustomMapDefinition* Map =
			Registry.CustomMaps.Find(MapId);
		if (!Map)
		{
			OutError = FString::Printf(
				TEXT("Custom map '%s' is not registered"),
				*MapId.ToString());
			return false;
		}

		const int32 Width = Map->MaximumCellExclusive.X - Map->MinimumCell.X;
		const int32 Height = Map->MaximumCellExclusive.Y - Map->MinimumCell.Y;
		const FWorldSettings Settings = MakeCustomMapWorldSettings(*Map);
		if (!OutWorld.Initialize(Settings, Registry, Seed, OutError))
		{
			return false;
		}
		OutWorld.SetSimulationFocus(GetCustomMapFocus(*Map));

		TArray<FSeedCell> SurfaceCells;
		SurfaceCells.Reserve(Width * Height);
		TMap<FIntPoint, int32> SurfaceIndices;
		SurfaceIndices.Reserve(Width * Height);
		for (int32 Y = Map->MinimumCell.Y; Y < Map->MaximumCellExclusive.Y; ++Y)
		{
			for (int32 X = Map->MinimumCell.X; X < Map->MaximumCellExclusive.X; ++X)
			{
				const FIntPoint Cell(X, Y);
				SurfaceIndices.Add(Cell, SurfaceCells.Add({ Cell, NAME_None, 0 }));
			}
		}
		const auto PaintCell = [&SurfaceCells, &SurfaceIndices](
			const FIntPoint Cell,
			const FName MaterialId)
		{
			if (const int32* Index = SurfaceIndices.Find(Cell))
			{
				SurfaceCells[*Index].MaterialId = MaterialId;
				return true;
			}
			return false;
		};

		for (const FMatterFluxCustomMapStampDefinition& Stamp : Map->Stamps)
		{
			if (Stamp.Shape == EMatterFluxCustomMapStampShape::Rectangle)
			{
				for (int32 Y = Stamp.MinimumCell.Y;
					Y <= Stamp.MaximumCellInclusive.Y;
					++Y)
				{
					for (int32 X = Stamp.MinimumCell.X;
						X <= Stamp.MaximumCellInclusive.X;
						++X)
					{
						if (!PaintCell(FIntPoint(X, Y), Stamp.MaterialId))
						{
							OutError = FString::Printf(
								TEXT("Custom map '%s' failed to stamp material '%s' at (%d,%d)"),
								*MapId.ToString(),
								*Stamp.MaterialId.ToString(),
								X,
								Y);
							return false;
						}
					}
				}
				continue;
			}

			const int32 RadiusSquared = Stamp.RadiusCells * Stamp.RadiusCells;
			for (int32 LocalY = -Stamp.RadiusCells;
				LocalY <= Stamp.RadiusCells;
				++LocalY)
			{
				for (int32 LocalX = -Stamp.RadiusCells;
					LocalX <= Stamp.RadiusCells;
					++LocalX)
				{
					if (LocalX * LocalX + LocalY * LocalY > RadiusSquared)
					{
						continue;
					}
					const FIntPoint Cell = Stamp.CenterCell
						+ FIntPoint(LocalX, LocalY);
					if (!PaintCell(Cell, Stamp.MaterialId))
					{
						OutError = FString::Printf(
							TEXT("Custom map '%s' failed to stamp circle material '%s' at (%d,%d)"),
							*MapId.ToString(),
							*Stamp.MaterialId.ToString(),
							Cell.X,
							Cell.Y);
						return false;
					}
				}
			}
		}
		if (!OutWorld.SeedSurface(SurfaceCells))
		{
			OutError = FString::Printf(
				TEXT("Custom map '%s' failed to seed its horizontal surface"),
				*MapId.ToString());
			return false;
		}

		OutScene.CellSizeCentimeters = Map->CellSizeCentimeters;
		OutScene.MaterialDepthCells = Map->MaterialDepthCells;
		OutScene.MinimumCell = Map->MinimumCell;
		OutScene.MaximumCellExclusive = Map->MaximumCellExclusive;
		for (const FMatterFluxCustomMapMarkerDefinition& Marker : Map->Markers)
		{
			OutScene.MarkerLocations.Add(
				Marker.Id,
				FVector(
					Marker.Cell.X * Map->CellSizeCentimeters,
					Marker.Cell.Y * Map->CellSizeCentimeters,
					0.0f));
		}
		OutScene.Boxes.Reserve(Map->SceneBoxes.Num());
		for (const FMatterFluxCustomMapSceneBoxDefinition& SourceBox
			: Map->SceneBoxes)
		{
			FCustomMapSceneBox& Box = OutScene.Boxes.Emplace_GetRef();
			Box.Id = SourceBox.Id;
			Box.MaterialId = SourceBox.MaterialId;
			Box.Center = SourceBox.CenterCells * Map->CellSizeCentimeters;
			Box.Size = SourceBox.SizeCells * Map->CellSizeCentimeters;
			Box.bCollision = SourceBox.bCollision;
		}
		OutScene.Cameras.Reserve(Map->Cameras.Num());
		for (const FMatterFluxCustomMapCameraDefinition& SourceCamera
			: Map->Cameras)
		{
			FCustomMapSceneCamera& Camera =
				OutScene.Cameras.Emplace_GetRef();
			Camera.Id = SourceCamera.Id;
			Camera.Location =
				SourceCamera.LocationCells * Map->CellSizeCentimeters;
			Camera.Target = SourceCamera.TargetCells * Map->CellSizeCentimeters;
			Camera.FieldOfViewDegrees = SourceCamera.FieldOfViewDegrees;
		}
		OutScene.PourContainers.Reserve(Map->PourContainers.Num());
		for (const FMatterFluxCustomMapPourContainerDefinition& SourceContainer
			: Map->PourContainers)
		{
			FCustomMapPourContainer& Container =
				OutScene.PourContainers.Emplace_GetRef();
			Container.Id = SourceContainer.Id;
			Container.ContainerMaterialId =
				SourceContainer.ContainerMaterialId;
			Container.LiquidMaterialId = SourceContainer.LiquidMaterialId;
			Container.Center =
				SourceContainer.CenterCells * Map->CellSizeCentimeters;
			Container.InteriorSizeCells = SourceContainer.InteriorSizeCells;
			Container.StartStep = SourceContainer.StartStep;
			Container.TiltDurationSteps = SourceContainer.TiltDurationSteps;
			Container.TiltDegrees = SourceContainer.TiltDegrees;
			Container.PourCellsPerStep = SourceContainer.PourCellsPerStep;
		}
		return true;
	}

	bool BuildPlayableCustomMap(
		const FName MapId,
		const FMatterFluxContentRegistry& Registry,
		const int32 Seed,
		const float StepSeconds,
		const int32 MaxStepsPerAdvance,
		FSimulationRuntime& OutRuntime,
		FCustomMapScene& OutScene,
		FString& OutError)
	{
		FChunkedMaterialWorld StagingWorld;
		if (!BuildCustomMap(
			MapId,
			Registry,
			Seed,
			StagingWorld,
			OutScene,
			OutError))
		{
			return false;
		}
		const FMatterFluxCustomMapDefinition* Map =
			Registry.CustomMaps.Find(MapId);
		if (!Map)
		{
			OutError = TEXT("Custom map disappeared during runtime adoption");
			return false;
		}
		TArray<uint8> InitialState;
		if (!StagingWorld.ExportActiveState(0, InitialState, OutError))
		{
			return false;
		}

		FRuntimeSettings RuntimeSettings;
		RuntimeSettings.World = MakeCustomMapWorldSettings(*Map);
		RuntimeSettings.StepSeconds = StepSeconds;
		RuntimeSettings.MaxStepsPerAdvance = MaxStepsPerAdvance;
		const FIntPoint InitialFocus = GetCustomMapFocus(*Map);
		const TArray<FIntPoint> InitialFocuses = { InitialFocus };
		if (!OutRuntime.Initialize(
			RuntimeSettings,
			Registry,
			Seed,
			InitialFocuses,
			OutError))
		{
			return false;
		}
		int32 LogicalStep = 0;
		FIntPoint ImportedFocus = FIntPoint::ZeroValue;
		if (!OutRuntime.ImportActiveState(
			InitialState,
			LogicalStep,
			ImportedFocus,
			OutError))
		{
			OutRuntime.Reset();
			return false;
		}
		return true;
	}
}
