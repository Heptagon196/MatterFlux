#include "Game/MatterFluxPlayableLevel.h"
#include "MatterFluxLog.h"

namespace MatterFlux::PlayableLevel
{
	namespace
	{
		constexpr float TerrainOriginX =
			-(static_cast<float>(TerrainCellsX) - 1.0f) * TerrainCellSize * 0.5f;
		constexpr float TerrainOriginY =
			-(static_cast<float>(TerrainCellsY) - 1.0f) * TerrainCellSize * 0.5f;
		constexpr float DecorationFacingYaw = 45.0f;
		constexpr float GroundAttachmentEmbedFraction = 0.5f;

		FVector2D MakeTerrainNoiseOffset(const int32 Seed)
		{
			const uint32 SeedBits = static_cast<uint32>(Seed);
			return FVector2D(
				static_cast<float>(SeedBits & 0xffffu) / 1024.0f,
				static_cast<float>((SeedBits >> 16u) & 0xffffu) / 1024.0f);
		}

		float SampleFractalPerlin(
			const double X,
			const double Y,
			const FVector2D& SeedOffset)
		{
			const float BroadShape = FMath::PerlinNoise2D(FVector2D(
				X * 0.00048 + SeedOffset.X,
				Y * 0.00048 + SeedOffset.Y));
			const float Detail = FMath::PerlinNoise2D(FVector2D(
				X * 0.0017 + SeedOffset.X * 1.73,
				Y * 0.0017 + SeedOffset.Y * 1.41));
			return BroadShape * 0.74f + Detail * 0.26f;
		}

		float SampleTerrainHeight(
			const double WorldX,
			const double WorldY,
			const FVector2D& SeedOffset)
		{
			float Height = FMath::GridSnap(
				130.0f
					+ SampleFractalPerlin(WorldX, WorldY, SeedOffset)
						* 120.0f,
				TerrainCellSize);
			return FMath::Clamp(Height, 50.0f, 245.0f);
		}

		uint8 SelectTerrainColorBand(const float Height)
		{
			if (Height >= 168.0f)
			{
				return 2;
			}
			return Height >= 112.0f ? 1 : 0;
		}

		FVector EmbedGroundAttachment(
			const FVector& SurfaceLocation,
			const float DecorationCellSize)
		{
			// A mask's bottom side face is horizontal. Leaving it exactly on the
			// terrain surface makes the depth buffer alternate between the two
			// coplanar faces as the camera moves. Bury half of the first voxel so
			// the connection remains solid while the conflicting face stays below
			// the terrain. This changes only presentation placement; masks and
			// deterministic source identities remain unchanged.
			return SurfaceLocation - FVector(
				0.0f,
				0.0f,
				DecorationCellSize * GroundAttachmentEmbedFraction);
		}

		struct FGenerationContext
		{
			explicit FGenerationContext(
				const int32 InSeed,
				const FMatterFluxContentRegistry* InContent)
				: Seed(InSeed)
				, Content(InContent)
			{
				NoiseOffset = MakeTerrainNoiseOffset(Seed);
			}

			FRandomStream MakeRuleStream(const uint32 RuleSalt) const
			{
				uint32 Mixed = static_cast<uint32>(Seed) ^ RuleSalt;
				Mixed ^= Mixed >> 16u;
				Mixed *= 0x7feb352du;
				Mixed ^= Mixed >> 15u;
				Mixed *= 0x846ca68bu;
				Mixed ^= Mixed >> 16u;
				return FRandomStream(static_cast<int32>(Mixed));
			}

			int32 ToCellX(const float X) const
			{
				return FMath::Clamp(
					FMath::RoundToInt((X - TerrainOriginX) / TerrainCellSize),
					0,
					TerrainCellsX - 1);
			}

			int32 ToCellY(const float Y) const
			{
				return FMath::Clamp(
					FMath::RoundToInt((Y - TerrainOriginY) / TerrainCellSize),
					0,
					TerrainCellsY - 1);
			}

			float SurfaceAtCell(const int32 X, const int32 Y) const
			{
				return SurfaceHeights[Y * TerrainCellsX + X];
			}

			float SurfaceAt(const float X, const float Y) const
			{
				return SurfaceAtCell(ToCellX(X), ToCellY(Y));
			}

			float StreamXAt(const float Y) const
			{
				const int32 Row = ToCellY(Y);
				return TerrainOriginX
					+ static_cast<float>(StreamColumns[Row]) * TerrainCellSize;
			}

			bool IsNearStream(const float X, const float Y, const float Margin) const
			{
				return FMath::Abs(X - StreamXAt(Y)) < Margin;
			}

			int32 Seed;
			const FMatterFluxContentRegistry* Content = nullptr;
			FVector2D NoiseOffset = FVector2D::ZeroVector;
			TArray<float> SurfaceHeights;
			TArray<int32> StreamColumns;
			TMap<FName, int32> SourceOrdinals;
		};

		struct FSurfaceScatterRule
		{
			FName ContentId;
			FName LayerName;
			FName MaterialId;
			FLinearColor Color;
			int32 Count;
			uint32 SeedSalt;
			float StreamMargin;
			bool bEnableCollision = false;
		};

		const FMatterFluxMaterialDefinition* FindMaterial(
			const FGenerationContext& Context,
			const FName MaterialId)
		{
			return Context.Content
				? Context.Content->Materials.Find(MaterialId)
				: nullptr;
		}

		FLinearColor ResolveMaterialColor(
			const FGenerationContext& Context,
			const FName MaterialId,
			const FLinearColor& Fallback)
		{
			const FMatterFluxMaterialDefinition* Material =
				FindMaterial(Context, MaterialId);
			return Material ? Material->Color : Fallback;
		}

		const FMatterFluxDecoratorDefinition* FindDecorator(
			const FGenerationContext& Context,
			const FName DecoratorId,
			const FName ExpectedGenerator)
		{
			if (!Context.Content)
			{
				return nullptr;
			}
			const FMatterFluxDecoratorDefinition* Definition =
				Context.Content->Decorators.Find(DecoratorId);
			return Definition && Definition->GeneratorId == ExpectedGenerator
				? Definition
				: nullptr;
		}

		int32 ResolveDecoratorCount(
			const FGenerationContext& Context,
			const FMatterFluxDecoratorDefinition* Definition,
			const int32 Fallback,
			const uint32 SeedSalt)
		{
			if (!Definition)
			{
				return Fallback;
			}
			FRandomStream CountRandom =
				Context.MakeRuleStream(SeedSalt ^ 0x434f554eu);
			return CountRandom.RandRange(
				Definition->MinCount,
				Definition->MaxCount);
		}

		FLevelLayer& AddLayer(
			FLevelLayout& Layout,
			const FName Name,
			const ELayerPrimitive Primitive,
			const FLinearColor& Color,
			const bool bEnableCollision = false,
			const ELevelLayerRenderMode RenderMode =
				ELevelLayerRenderMode::Lit)
		{
			FLevelLayer& Layer = Layout.Layers.AddDefaulted_GetRef();
			Layer.Name = Name;
			Layer.Primitive = Primitive;
			Layer.RenderMode = RenderMode;
			Layer.Color = Color;
			Layer.bEnableCollision = bEnableCollision;
			return Layer;
		}

		FFragmentSourceMask MakeEmptyMask(
			const int32 Width,
			const int32 Height,
			const float CellSize,
			const int32 MinFragmentAreaPixels = 1)
		{
			FFragmentSourceMask Mask;
			Mask.Width = Width;
			Mask.Height = Height;
			Mask.CellSize = CellSize;
			Mask.MinFragmentAreaPixels = MinFragmentAreaPixels;
			Mask.MaxFragmentsPerBreak = 16;
			Mask.SolidMask.Init(0, Width * Height);
			return Mask;
		}

		void SetSolid(FFragmentSourceMask& Mask, const int32 X, const int32 Y)
		{
			if (X >= 0 && X < Mask.Width && Y >= 0 && Y < Mask.Height)
			{
				Mask.SolidMask[Y * Mask.Width + X] = 1;
			}
		}

		void AddFragmentSource(
			FGenerationContext& Context,
			FLevelLayout& Layout,
			const FName Name,
			const FName MaterialId,
			const FLinearColor& Color,
			const FTransform& Transform,
			FFragmentSourceMask&& Mask,
			const bool bEnableCollision = false,
			const FGuid& AggregateId = FGuid(),
			const bool bAggregateRoot = false)
		{
			const int32 Ordinal =
				Context.SourceOrdinals.FindOrAdd(Name)++;
			const FString Signature = FString::Printf(
				TEXT("PlayableForest|Seed=%d|Type=%s|Index=%d"),
				Context.Seed,
				*Name.ToString(),
				Ordinal);

			FLevelFragmentSource& Source =
				Layout.FragmentSources.AddDefaulted_GetRef();
			Source.Name = Name;
			Source.MaterialId = MaterialId;
			Source.SourceId = FGuid::NewDeterministicGuid(
				Signature,
				static_cast<uint64>(static_cast<uint32>(Context.Seed)));
			Source.AggregateId = AggregateId;
			Source.bAggregateRoot =
				AggregateId.IsValid() && bAggregateRoot;
			Source.Color = Color;
			Source.bEnableCollision = bEnableCollision;
			Source.Transform = Transform;
			Source.Mask = MoveTemp(Mask);
		}

		void GenerateTerrain(FGenerationContext& Context, FLevelLayout& Layout)
		{
			const float MapSizeX = static_cast<float>(TerrainCellsX) * TerrainCellSize;
			const float MapSizeY = static_cast<float>(TerrainCellsY) * TerrainCellSize;
			const FLinearColor GrassColor = ResolveMaterialColor(
				Context,
				TEXT("grassland"),
				FLinearColor(0.018f, 0.18f, 0.035f));
			Layout.Terrain.Seed = Context.Seed;
			Layout.Terrain.bInfinite = true;
			Layout.Terrain.Width = TerrainCellsX;
			Layout.Terrain.Height = TerrainCellsY;
			Layout.Terrain.CellSize = TerrainCellSize;
			Layout.Terrain.BottomZ = -110.0f;
			Layout.Terrain.FirstCellCenter =
				FVector2D(TerrainOriginX, TerrainOriginY);

			// The playable terrain is finite, but the isometric camera can see
			// beyond an edge when a player explores near it. A single non-colliding
			// voxel slab replaces the black void with a dark forest floor without
			// extending simulation, streaming, navigation, or collision bounds.
			FLevelLayer& Backdrop = AddLayer(
				Layout,
				TEXT("Backdrop"),
				ELayerPrimitive::Cube,
				GrassColor * 0.55f,
				false,
				ELevelLayerRenderMode::VoxelLit);
			Backdrop.Instances.Emplace(
				FRotator::ZeroRotator,
				FVector(0.0f, 0.0f, -130.0f),
				FVector(
					MapSizeX * 2.0f / 100.0f,
					MapSizeY * 2.0f / 100.0f,
					0.4f));

			FLevelLayer& Soil = AddLayer(
				Layout,
				TEXT("Soil"),
				ELayerPrimitive::Cube,
				ResolveMaterialColor(
					Context,
					TEXT("soil"),
					FLinearColor(0.16f, 0.045f, 0.012f)),
				true);
			Soil.Instances.Emplace(
				FRotator::ZeroRotator,
				FVector(0.0f, 0.0f, -55.0f),
				FVector(MapSizeX / 100.0f, MapSizeY / 100.0f, 1.1f));

			Context.SurfaceHeights.Reserve(TerrainCellsX * TerrainCellsY);

			for (int32 Y = 0; Y < TerrainCellsY; ++Y)
			{
				for (int32 X = 0; X < TerrainCellsX; ++X)
				{
					const float WorldX = TerrainOriginX + static_cast<float>(X) * TerrainCellSize;
					const float WorldY = TerrainOriginY + static_cast<float>(Y) * TerrainCellSize;
					const float Height = SampleTerrainHeight(
						WorldX,
						WorldY,
						Context.NoiseOffset);
					Context.SurfaceHeights.Add(Height);
				}
			}
			Layout.Terrain.Heights = Context.SurfaceHeights;
			Layout.Terrain.ColorBands.Init(
				0,
				Context.SurfaceHeights.Num());

			const FLinearColor LowlandColor =
				GrassColor * FLinearColor(0.78f, 0.90f, 0.86f);
			const FLinearColor MidlandColor =
				GrassColor * FLinearColor(0.92f, 1.00f, 0.90f);
			const FLinearColor HighlandColor =
				GrassColor * FLinearColor(1.05f, 1.08f, 0.84f);
			Layout.Terrain.BandColors = {
				LowlandColor,
				MidlandColor,
				HighlandColor
			};
			for (int32 Index = 0; Index < Context.SurfaceHeights.Num(); ++Index)
			{
				Layout.Terrain.ColorBands[Index] =
					SelectTerrainColorBand(Context.SurfaceHeights[Index]);
			}

			Context.StreamColumns.Reserve(TerrainCellsY);
			int32 PreviousColumn = TerrainCellsX / 2;
			for (int32 Y = 0; Y < TerrainCellsY; ++Y)
			{
				const float WorldY = TerrainOriginY + static_cast<float>(Y) * TerrainCellSize;
				const float StreamNoise = FMath::PerlinNoise2D(FVector2D(
					Context.NoiseOffset.X + 31.0f,
					WorldY * 0.0009f + Context.NoiseOffset.Y));
				const int32 DesiredColumn = FMath::Clamp(
					FMath::RoundToInt(
						static_cast<float>(TerrainCellsX) * 0.5f
							+ StreamNoise * static_cast<float>(TerrainCellsX) * 0.24f),
					3,
					TerrainCellsX - 3);
				const int32 Column = Y == 0
					? DesiredColumn
					: FMath::Clamp(DesiredColumn, PreviousColumn - 1, PreviousColumn + 1);
				Context.StreamColumns.Add(Column);
				PreviousColumn = Column;
			}
		}

		void GenerateStream(FGenerationContext& Context, FLevelLayout& Layout)
		{
			FLevelLayer& Stream = AddLayer(
				Layout,
				TEXT("Stream"),
				ELayerPrimitive::Cube,
				ResolveMaterialColor(
					Context,
					TEXT("water"),
					FLinearColor(0.015f, 0.24f, 0.78f)),
				false,
				ELevelLayerRenderMode::VoxelLit);
			Stream.Instances.Reserve(TerrainCellsY * 6);
			for (int32 Y = 0; Y < TerrainCellsY; ++Y)
			{
				const float WorldY = TerrainOriginY + static_cast<float>(Y) * TerrainCellSize;
				const int32 CenterColumn = Context.StreamColumns[Y];
				for (int32 Offset = -2; Offset <= 1; ++Offset)
				{
					const int32 X = CenterColumn + Offset;
					const float WorldX = TerrainOriginX + static_cast<float>(X) * TerrainCellSize;
					Stream.Instances.Emplace(
						FRotator::ZeroRotator,
						FVector(
							WorldX,
							WorldY,
							Context.SurfaceAtCell(X, Y) + 5.0f),
						FVector(
							TerrainCellSize * 1.004f / 100.0f,
							TerrainCellSize * 1.004f / 100.0f,
							0.07f));
				}
			}
		}

		bool FindScatterLocation(
			const FGenerationContext& Context,
			FRandomStream& Random,
			const float StreamMargin,
			const bool bReserveCameraCorridor,
			FVector& OutLocation)
		{
			const float HalfX =
				(static_cast<float>(TerrainCellsX) - 1.5f) * TerrainCellSize * 0.5f;
			const float HalfY =
				(static_cast<float>(TerrainCellsY) - 1.5f) * TerrainCellSize * 0.5f;
			for (int32 Attempt = 0; Attempt < 12; ++Attempt)
			{
				const float X = Random.FRandRange(-HalfX, HalfX);
				const float Y = Random.FRandRange(-HalfY, HalfY);
				const FVector2D SpawnDelta =
					FVector2D(X + 700.0f, Y + 500.0f);
				const FVector2D CameraGroundDirection =
					FVector2D(-1.0f, 1.0f).GetSafeNormal();
				const float DistanceTowardCamera =
					FVector2D::DotProduct(
						SpawnDelta,
						CameraGroundDirection);
				const float CameraCorridorDistance =
					FMath::Abs(
						SpawnDelta.X * CameraGroundDirection.Y
						- SpawnDelta.Y * CameraGroundDirection.X);
				const bool bInsideCameraCorridor =
					DistanceTowardCamera > 0.0f
					&& DistanceTowardCamera < 900.0f
					&& CameraCorridorDistance < 260.0f;
				const bool bClearOfDefaultSpawn =
					SpawnDelta.Size()
						>= (bReserveCameraCorridor ? 280.0f : 120.0f)
					&& (!bReserveCameraCorridor
						|| !bInsideCameraCorridor);
				if (bClearOfDefaultSpawn
					&& (StreamMargin <= 0.0f
						|| !Context.IsNearStream(X, Y, StreamMargin)))
				{
					OutLocation = FVector(X, Y, Context.SurfaceAt(X, Y));
					return true;
				}
			}
			return false;
		}

		void GenerateRocks(FGenerationContext& Context, FLevelLayout& Layout)
		{
			const FMatterFluxDecoratorDefinition* Definition = FindDecorator(
				Context,
				TEXT("forest.rock"),
				TEXT("rock"));
			const FLinearColor RockColor = ResolveMaterialColor(
				Context,
				Definition ? Definition->MaterialId : FName(TEXT("stone")),
				FLinearColor(0.30f, 0.34f, 0.40f));
			FRandomStream Random = Context.MakeRuleStream(0x524f434bu);
			const int32 Count = ResolveDecoratorCount(
				Context,
				Definition,
				22,
				0x524f434bu);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				FVector Location;
				if (!FindScatterLocation(
					Context,
					Random,
					250.0f,
					true,
					Location))
				{
					continue;
				}
				constexpr float CellSize = 10.0f;
				const FVector GroundedLocation =
					EmbedGroundAttachment(Location, CellSize);
				const int32 RockWidth = Random.RandRange(6, 10);
				const int32 RockHeight = Random.RandRange(4, 7);
				FFragmentSourceMask RockMask =
					MakeEmptyMask(RockWidth + 2, RockHeight + 1, CellSize, 2);
				const float Radius = static_cast<float>(RockWidth) * 0.5f;
				for (int32 X = 0; X < RockWidth; ++X)
				{
					const float NormalizedDistance =
						FMath::Abs(static_cast<float>(X) + 0.5f - Radius) / Radius;
					const int32 ColumnHeight = FMath::Clamp(
						FMath::RoundToInt(
							static_cast<float>(RockHeight)
							* (1.0f - 0.55f * NormalizedDistance)),
						1,
						RockHeight);
					for (int32 Y = 0; Y < ColumnHeight; ++Y)
					{
						SetSolid(RockMask, X + 1, Y);
					}
				}
				AddFragmentSource(
					Context,
					Layout,
					TEXT("RockCluster"),
					Definition
						? Definition->MaterialId
						: FName(TEXT("stone")),
					RockColor,
					FTransform(
						FRotator(0.0f, DecorationFacingYaw, 0.0f),
						GroundedLocation + FVector(
							0.0f,
							0.0f,
							static_cast<float>(RockMask.Height) * CellSize * 0.5f),
						FVector(1.0f, 2.0f, 1.0f)),
					MoveTemp(RockMask),
					Definition ? Definition->bEnableCollision : false);
			}
		}

		void GenerateTrees(FGenerationContext& Context, FLevelLayout& Layout)
		{
			const FMatterFluxDecoratorDefinition* Definition = FindDecorator(
				Context,
				TEXT("forest.tree"),
				TEXT("tree"));
			const FLinearColor TrunkColor = ResolveMaterialColor(
				Context,
				Definition ? Definition->MaterialId : FName(TEXT("wood")),
				FLinearColor(0.38f, 0.18f, 0.05f));
			const FLinearColor CanopyColor = ResolveMaterialColor(
				Context,
				TEXT("leaf"),
				FLinearColor(0.07f, 0.42f, 0.10f));
			const auto ScaleRgb = [](const FLinearColor& Color, const float Scale)
			{
				return FLinearColor(
					FMath::Clamp(Color.R * Scale, 0.0f, 1.0f),
					FMath::Clamp(Color.G * Scale, 0.0f, 1.0f),
					FMath::Clamp(Color.B * Scale, 0.0f, 1.0f),
					Color.A);
			};
			const FLinearColor CanopyBackColor = ScaleRgb(CanopyColor, 0.82f);
			const FLinearColor CanopyFrontColor = ScaleRgb(CanopyColor, 1.12f);
			const FQuat FacingRotation =
				FRotator(0.0f, DecorationFacingYaw - 12.0f, 0.0f).Quaternion();
			const FVector DepthAxis =
				FacingRotation.RotateVector(FVector::YAxisVector);
			const FVector CrownRightAxis =
				FacingRotation.RotateVector(FVector::XAxisVector);
			FRandomStream Random = Context.MakeRuleStream(0x54524545u);
			const int32 Count = ResolveDecoratorCount(
				Context,
				Definition,
				42,
				0x54524545u);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				FVector Location;
				if (!FindScatterLocation(
					Context,
					Random,
					310.0f,
					true,
					Location))
				{
					continue;
				}
				constexpr float CellSize = 12.0f;
				const FVector TrunkDepthOffset = -DepthAxis * 36.0f;
				FVector TreeSurfaceLocation = Location;
				// The trunk is deliberately behind the crown in view depth. Sample
				// terrain at that actual XY instead of the scatter anchor; otherwise
				// a trunk crossing an eight-centimetre terrain step can still leave
				// its bottom face coplanar with the higher cell.
				TreeSurfaceLocation.Z = Context.SurfaceAt(
					Location.X + TrunkDepthOffset.X,
					Location.Y + TrunkDepthOffset.Y);
				const FVector GroundedLocation =
					EmbedGroundAttachment(TreeSurfaceLocation, CellSize);
				const int32 TrunkCells = Random.RandRange(13, 20);
				const float TrunkHeight = static_cast<float>(TrunkCells) * CellSize;
				const FGuid TreeAggregateId =
					FGuid::NewDeterministicGuid(
						FString::Printf(
							TEXT("PlayableForest|Seed=%d|TreeAggregate=%d"),
							Context.Seed,
							Index),
						static_cast<uint64>(
							static_cast<uint32>(Context.Seed)));

				FFragmentSourceMask TrunkMask =
					MakeEmptyMask(7, TrunkCells + 1, CellSize, 2);
				for (int32 Y = 0; Y < TrunkCells; ++Y)
				{
					const int32 HalfWidth =
						Y < TrunkCells / 3 && (Index & 1) == 0 ? 2 : 1;
					for (int32 X = 3 - HalfWidth; X <= 3 + HalfWidth; ++X)
					{
						SetSolid(TrunkMask, X, Y);
					}
				}
				AddFragmentSource(
					Context,
					Layout,
					TEXT("TreeTrunk"),
					Definition
						? Definition->MaterialId
						: FName(TEXT("wood")),
					TrunkColor,
					FTransform(
						FacingRotation,
						GroundedLocation + FVector(
							0.0f,
							0.0f,
							static_cast<float>(TrunkMask.Height) * CellSize * 0.5f)
							+ TrunkDepthOffset,
						FVector(1.0f, 1.5f, 1.0f)),
					MoveTemp(TrunkMask),
					Definition ? Definition->bEnableCollision : true,
					TreeAggregateId,
					true);

				const FVector CanopyCenter =
					GroundedLocation
					+ FVector(0.0f, 0.0f, TrunkHeight + 30.0f);
				FFragmentSourceMask CanopyBackMask =
					MakeEmptyMask(19, 13, CellSize, 4);
				for (int32 Y = 0; Y < 12; ++Y)
				{
					const int32 DistanceFromMiddle = FMath::Abs(Y - 5);
					const int32 HalfWidth = FMath::Clamp(
						8 - DistanceFromMiddle,
						4,
						8);
					for (int32 X = 9 - HalfWidth; X <= 9 + HalfWidth; ++X)
					{
						SetSolid(CanopyBackMask, X, Y);
					}
				}
				AddFragmentSource(
					Context,
					Layout,
					TEXT("TreeCanopyBack"),
					TEXT("leaf"),
					CanopyBackColor,
					FTransform(
						FacingRotation,
						CanopyCenter
							- DepthAxis * 12.0f
							- CrownRightAxis * 12.0f
							+ FVector(0.0f, 0.0f, 8.0f),
						FVector(1.0f, 1.5f, 1.0f)),
					MoveTemp(CanopyBackMask),
					false,
					TreeAggregateId);

				FFragmentSourceMask CanopyMask =
					MakeEmptyMask(17, 13, CellSize, 4);
				for (int32 Y = 0; Y < 12; ++Y)
				{
					const int32 DistanceFromMiddle = FMath::Abs(Y - 5);
					const int32 HalfWidth = FMath::Clamp(7 - DistanceFromMiddle, 3, 7);
					const int32 WindOffset =
						(Index % 3 == 0 && Y > 6) ? 1 : 0;
					for (int32 X = 8 - HalfWidth + WindOffset;
						X <= 8 + HalfWidth + WindOffset;
						++X)
					{
						SetSolid(CanopyMask, X, Y);
					}
				}
				AddFragmentSource(
					Context,
					Layout,
					TEXT("TreeCanopy"),
					TEXT("leaf"),
					CanopyColor,
					FTransform(
						FacingRotation,
						CanopyCenter + DepthAxis * 10.0f,
						FVector(1.0f, 1.5f, 1.0f)),
					MoveTemp(CanopyMask),
					false,
					TreeAggregateId);

				FFragmentSourceMask CanopyFrontMask =
					MakeEmptyMask(11, 9, CellSize, 2);
				for (int32 Y = 0; Y < 8; ++Y)
				{
					const int32 DistanceFromMiddle = FMath::Abs(Y - 3);
					const int32 HalfWidth = FMath::Clamp(
						4 - DistanceFromMiddle,
						2,
						4);
					const int32 WindOffset =
						(Index % 3 == 1 && Y > 4) ? 1 : 0;
					for (int32 X = 5 - HalfWidth + WindOffset;
						X <= 5 + HalfWidth + WindOffset;
						++X)
					{
						SetSolid(CanopyFrontMask, X, Y);
					}
				}
				AddFragmentSource(
					Context,
					Layout,
					TEXT("TreeCanopyFront"),
					TEXT("leaf"),
					CanopyFrontColor,
					FTransform(
						FacingRotation,
						CanopyCenter
							+ DepthAxis * 32.0f
							+ CrownRightAxis * 14.0f
							- FVector(0.0f, 0.0f, 7.0f),
						FVector(1.0f, 1.5f, 1.0f)),
					MoveTemp(CanopyFrontMask),
					false,
					TreeAggregateId);
			}
		}

		void GenerateVoxelGroundCover(
			FGenerationContext& Context,
			FLevelLayout& Layout,
			const FSurfaceScatterRule& Rule)
		{
			FRandomStream Random = Context.MakeRuleStream(Rule.SeedSalt);
			const bool bGrass = Rule.LayerName == TEXT("GrassCluster");
			const int32 ElementsPerCluster = bGrass ? 5 : 3;
			const int32 ClusterCount =
				FMath::Max(1, FMath::DivideAndRoundUp(Rule.Count, ElementsPerCluster));
			for (int32 Index = 0; Index < ClusterCount; ++Index)
			{
				FVector Location;
				bool bFoundLocation = false;
				if (Index < 2)
				{
					const float Angle =
						Random.FRandRange(0.0f, 2.0f * PI);
					const float Radius = 250.0f
						+ static_cast<float>(Index) * 110.0f;
					const float CandidateX =
						-700.0f + FMath::Cos(Angle) * Radius;
					const float CandidateY =
						-500.0f + FMath::Sin(Angle) * Radius;
					if (!Context.IsNearStream(
						CandidateX,
						CandidateY,
						Rule.StreamMargin))
					{
						Location = FVector(
							CandidateX,
							CandidateY,
							Context.SurfaceAt(
								CandidateX,
								CandidateY));
						bFoundLocation = true;
					}
				}
				if (!bFoundLocation)
				{
					bFoundLocation = FindScatterLocation(
						Context,
						Random,
						Rule.StreamMargin,
						false,
						Location);
				}
				if (!bFoundLocation)
				{
					continue;
				}
				constexpr float CellSize = 6.0f;
				const FVector GroundedLocation =
					EmbedGroundAttachment(Location, CellSize);
				FFragmentSourceMask Mask = MakeEmptyMask(
					bGrass ? 17 : 13,
					bGrass ? 10 : 12,
					CellSize,
					1);
				const int32 BaseStart = bGrass ? 1 : 2;
				const int32 BaseEnd = bGrass ? 15 : 10;
				for (int32 X = BaseStart; X <= BaseEnd; ++X)
				{
					SetSolid(Mask, X, 0);
				}

				for (int32 Element = 0; Element < ElementsPerCluster; ++Element)
				{
					const int32 CenterX = bGrass ? 2 + Element * 3 : 3 + Element * 3;
					const int32 Height = bGrass
						? Random.RandRange(4, 9)
						: Random.RandRange(6, 9);
					for (int32 Y = 1; Y <= Height; ++Y)
					{
						const int32 Bend =
							bGrass && Y > Height / 2
								? ((Element & 1) == 0 ? -1 : 1)
								: 0;
						SetSolid(Mask, CenterX + Bend, Y);
					}
					if (!bGrass)
					{
						SetSolid(Mask, CenterX - 1, Height);
						SetSolid(Mask, CenterX + 1, Height);
						SetSolid(Mask, CenterX, Height - 1);
						SetSolid(Mask, CenterX, Height + 1);
					}
				}

				AddFragmentSource(
					Context,
					Layout,
					Rule.LayerName,
					Rule.MaterialId,
					Rule.Color,
					FTransform(
						FRotator(0.0f, DecorationFacingYaw, 0.0f),
						GroundedLocation + FVector(
							0.0f,
							0.0f,
							static_cast<float>(Mask.Height) * CellSize * 0.5f),
						FVector(1.0f, 1.5f, 1.0f)),
					MoveTemp(Mask),
					Rule.bEnableCollision);
			}
		}

		void GenerateGroundCover(FGenerationContext& Context, FLevelLayout& Layout)
		{
			static const FSurfaceScatterRule Rules[] = {
				{
					TEXT("forest.grass"),
					TEXT("GrassCluster"),
					TEXT("grass"),
					FLinearColor(0.15f, 0.70f, 0.035f),
					120,
					0x47524153u,
					210.0f
				},
				{
					TEXT("forest.flower.pink"),
					TEXT("PinkFlowerCluster"),
					TEXT("flower_pink"),
					FLinearColor(1.0f, 0.025f, 0.36f),
					42,
					0x50494e4bu,
					190.0f
				},
				{
					TEXT("forest.flower.gold"),
					TEXT("YellowFlowerCluster"),
					TEXT("flower_gold"),
					FLinearColor(1.0f, 0.55f, 0.015f),
					42,
					0x59454c4cu,
					190.0f
				},
				{
					TEXT("forest.flower.blue"),
					TEXT("PurpleFlowerCluster"),
					TEXT("flower_blue"),
					FLinearColor(0.42f, 0.025f, 1.0f),
					34,
					0x50555250u,
					190.0f
				}
			};

			for (const FSurfaceScatterRule& Rule : Rules)
			{
				FSurfaceScatterRule ResolvedRule = Rule;
				if (!Rule.ContentId.IsNone())
				{
					const FMatterFluxDecoratorDefinition* Definition =
						FindDecorator(
							Context,
							Rule.ContentId,
							TEXT("surface_scatter"));
					if (Definition)
					{
						ResolvedRule.Count = ResolveDecoratorCount(
							Context,
							Definition,
							Rule.Count,
							Rule.SeedSalt);
						ResolvedRule.Color = ResolveMaterialColor(
							Context,
							Definition->MaterialId,
							Rule.Color);
						ResolvedRule.MaterialId =
							Definition->MaterialId;
						ResolvedRule.bEnableCollision =
							Definition->bEnableCollision;
					}
				}
				GenerateVoxelGroundCover(Context, Layout, ResolvedRule);
			}

		}

		using FRuleGenerator = void(*)(FGenerationContext&, FLevelLayout&);
		constexpr FRuleGenerator RegisteredRules[] = {
			&GenerateStream,
			&GenerateRocks,
			&GenerateTrees,
			&GenerateGroundCover
		};
	}

	bool FLevelTerrain::TrySampleWorldCell(
		const int64 WorldCellX,
		const int64 WorldCellY,
		float& OutHeight,
		uint8& OutColorBand) const
	{
		if (!IsValid())
		{
			return false;
		}
		const double FirstCellX = FirstCellCenter.X / CellSize;
		const double FirstCellY = FirstCellCenter.Y / CellSize;
		if (!FMath::IsFinite(FirstCellX)
			|| !FMath::IsFinite(FirstCellY)
			|| FirstCellX < static_cast<double>(MIN_int32)
			|| FirstCellX > static_cast<double>(MAX_int32)
			|| FirstCellY < static_cast<double>(MIN_int32)
			|| FirstCellY > static_cast<double>(MAX_int32))
		{
			return false;
		}
		const int64 FirstWorldCellX = FMath::FloorToInt64(FirstCellX);
		const int64 FirstWorldCellY = FMath::FloorToInt64(FirstCellY);
		const int64 LocalX = WorldCellX - FirstWorldCellX;
		const int64 LocalY = WorldCellY - FirstWorldCellY;
		if (LocalX >= 0 && LocalX < Width
			&& LocalY >= 0 && LocalY < Height)
		{
			const int32 Index = ToIndex(
				static_cast<int32>(LocalX),
				static_cast<int32>(LocalY));
			OutHeight = Heights[Index];
			OutColorBand = ColorBands[Index];
			return true;
		}
		if (!bInfinite || Seed == 0)
		{
			return false;
		}

		const double WorldX = FirstCellCenter.X
			+ static_cast<double>(WorldCellX - FirstWorldCellX)
				* CellSize;
		const double WorldY = FirstCellCenter.Y
			+ static_cast<double>(WorldCellY - FirstWorldCellY)
				* CellSize;
		if (!FMath::IsFinite(WorldX) || !FMath::IsFinite(WorldY))
		{
			return false;
		}
		OutHeight = SampleTerrainHeight(
			WorldX,
			WorldY,
			MakeTerrainNoiseOffset(Seed));
		OutColorBand = SelectTerrainColorBand(OutHeight);
		return true;
	}

	const FLevelLayer* FLevelLayout::FindLayer(const FName LayerName) const
	{
		return Layers.FindByPredicate(
			[LayerName](const FLevelLayer& Layer)
			{
				return Layer.Name == LayerName;
			});
	}

	bool BuildLevelLayout(
		const int32 Seed,
		FLevelLayout& OutLayout,
		const FMatterFluxContentRegistry* Content)
	{
		OutLayout = FLevelLayout();
		FGenerationContext Context(Seed, Content);
		GenerateTerrain(Context, OutLayout);
		for (const FRuleGenerator Rule : RegisteredRules)
		{
			Rule(Context, OutLayout);
		}

		TSet<FName> LayerNames;
		for (const FLevelLayer& Layer : OutLayout.Layers)
		{
			if (Layer.Name.IsNone()
				|| Layer.Instances.IsEmpty()
				|| LayerNames.Contains(Layer.Name))
			{
				UE_LOG(
					LogTemp,
					Error,
					TEXT("Playable level validation failed for layer '%s': instances=%d duplicate=%s"),
					*Layer.Name.ToString(),
					Layer.Instances.Num(),
					LayerNames.Contains(Layer.Name) ? TEXT("true") : TEXT("false"));
				return false;
			}
			LayerNames.Add(Layer.Name);
			for (const FTransform& Transform : Layer.Instances)
			{
				if (!Transform.IsValid())
				{
					UE_LOG(
						LogTemp,
						Error,
						TEXT("Playable level layer '%s' contains an invalid transform"),
						*Layer.Name.ToString());
					return false;
				}
			}
		}

		TSet<FGuid> SourceIds;
		TMap<FGuid, int32> AggregateMemberCounts;
		TMap<FGuid, int32> AggregateRootCounts;
		for (const FLevelFragmentSource& Source : OutLayout.FragmentSources)
		{
			if (Source.Name.IsNone()
				|| Source.MaterialId.IsNone()
				|| !Source.SourceId.IsValid()
				|| SourceIds.Contains(Source.SourceId)
				|| !Source.Transform.IsValid()
				|| !Source.Mask.IsValid())
			{
				UE_LOG(
					LogTemp,
					Error,
					TEXT("Playable level validation failed for fragment source '%s'."),
					*Source.Name.ToString());
				return false;
			}
			if (Source.bAggregateRoot && !Source.AggregateId.IsValid())
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Playable level source '%s' is an aggregate root without an aggregate id"),
					*Source.Name.ToString());
				return false;
			}
			if (Source.AggregateId.IsValid())
			{
				AggregateMemberCounts.FindOrAdd(Source.AggregateId)++;
				AggregateRootCounts.FindOrAdd(Source.AggregateId) +=
					Source.bAggregateRoot ? 1 : 0;
			}
			SourceIds.Add(Source.SourceId);
		}
		for (const TPair<FGuid, int32>& Pair : AggregateMemberCounts)
		{
			if (Pair.Value < 2
				|| AggregateRootCounts.FindRef(Pair.Key) != 1)
			{
				UE_LOG(
					LogMatterFlux,
					Error,
					TEXT("Playable level aggregate '%s' must contain at least two members and exactly one root"),
					*Pair.Key.ToString(EGuidFormats::Digits));
				return false;
			}
		}
		return OutLayout.Terrain.IsValid()
			&& OutLayout.FindLayer(TEXT("Soil"))
			&& OutLayout.FindLayer(TEXT("Stream"))
			&& OutLayout.FragmentSources.Num() >= 32;
	}
}
