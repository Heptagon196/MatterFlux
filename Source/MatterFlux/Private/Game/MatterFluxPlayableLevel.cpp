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

			bool IsFlatFootprint(
				const float X,
				const float Y,
				const float HalfExtent) const
			{
				const int32 MinX = ToCellX(X - HalfExtent);
				const int32 MaxX = ToCellX(X + HalfExtent);
				const int32 MinY = ToCellY(Y - HalfExtent);
				const int32 MaxY = ToCellY(Y + HalfExtent);
				const float ReferenceHeight = SurfaceAtCell(MinX, MinY);
				for (int32 CellY = MinY; CellY <= MaxY; ++CellY)
				{
					for (int32 CellX = MinX; CellX <= MaxX; ++CellX)
					{
						if (!FMath::IsNearlyEqual(
							SurfaceAtCell(CellX, CellY),
							ReferenceHeight))
						{
							return false;
						}
					}
				}
				return true;
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

			bool IsInsideLake(
				const float X,
				const float Y,
				const float Margin = 0.0f) const
			{
				const float RadiusX = FMath::Max(LakeRadius.X + Margin, 1.0f);
				const float RadiusY = FMath::Max(LakeRadius.Y + Margin, 1.0f);
				const float NormalizedX = (X - LakeCenter.X) / RadiusX;
				const float NormalizedY = (Y - LakeCenter.Y) / RadiusY;
				return NormalizedX * NormalizedX
					+ NormalizedY * NormalizedY <= 1.0f;
			}

			int32 Seed;
			const FMatterFluxContentRegistry* Content = nullptr;
			FVector2D NoiseOffset = FVector2D::ZeroVector;
			TArray<float> SurfaceHeights;
			TArray<int32> StreamColumns;
			FVector2D LakeCenter = FVector2D::ZeroVector;
			FVector2D LakeRadius = FVector2D(360.0f, 260.0f);
			float LakeSurfaceZ = 100.0f;
			FVector2D HouseCenter = FVector2D::ZeroVector;
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
			const auto ScaleBackdropColor = [](
				const FLinearColor& Color,
				const float Scale)
			{
				return FLinearColor(
					FMath::Clamp(Color.R * Scale, 0.0f, 1.0f),
					FMath::Clamp(Color.G * Scale, 0.0f, 1.0f),
					FMath::Clamp(Color.B * Scale, 0.0f, 1.0f),
					1.0f);
			};
			Layout.Terrain.Seed = Context.Seed;
			Layout.Terrain.bInfinite = true;
			Layout.Terrain.Width = TerrainCellsX;
			Layout.Terrain.Height = TerrainCellsY;
			Layout.Terrain.CellSize = TerrainCellSize;
			Layout.Terrain.BottomZ = -110.0f;
			Layout.Terrain.FirstCellCenter =
				FVector2D(TerrainOriginX, TerrainOriginY);

			// The real terrain is streamed at full resolution around players only.
			// A deterministic coarse underlay fills distant holes in an isometric
			// view without extending simulation, collision, replication, or the
			// terrain-chunk working set.
			FLevelLayer& Backdrop = AddLayer(
				Layout,
				TEXT("Backdrop"),
				ELayerPrimitive::Cube,
				ScaleBackdropColor(GrassColor, 0.82f),
				false,
				ELevelLayerRenderMode::VoxelUnlit);
			constexpr float BackdropCellSize = 400.0f;
			constexpr int32 BackdropCellsX = 32;
			constexpr int32 BackdropCellsY = 24;
			constexpr float BackdropOverlapScale = 1.01f;
			constexpr float BackdropClearance = TerrainCellSize;
			Backdrop.Instances.Reserve(BackdropCellsX * BackdropCellsY);
			for (int32 Y = 0; Y < BackdropCellsY; ++Y)
			{
				for (int32 X = 0; X < BackdropCellsX; ++X)
				{
					const float WorldX =
						(static_cast<float>(X) + 0.5f
							- static_cast<float>(BackdropCellsX) * 0.5f)
						* BackdropCellSize;
					const float WorldY =
						(static_cast<float>(Y) + 0.5f
							- static_cast<float>(BackdropCellsY) * 0.5f)
						* BackdropCellSize;
					// 远景块只是一层无碰撞伪装，绝不能穿过真实流式地形。
					// 旧代码只采样 400 cm 方块中心；块内的柏林噪声低谷会
					// 露出这张粗平面，斜视时就成为明显的深绿色三角形。
					// 按真实 8 cm 地形晶格扫描整个渲染覆盖区（含 1% 接缝
					// 重叠），以最低地形高度为上限，再下沉一个格消除共面。
					const float RenderedHalfSize =
						BackdropCellSize * BackdropOverlapScale * 0.5f;
					const int64 MinimumCellX = FMath::FloorToInt64(
						(WorldX - RenderedHalfSize - TerrainOriginX)
						/ TerrainCellSize);
					const int64 MaximumCellX = FMath::CeilToInt64(
						(WorldX + RenderedHalfSize - TerrainOriginX)
						/ TerrainCellSize);
					const int64 MinimumCellY = FMath::FloorToInt64(
						(WorldY - RenderedHalfSize - TerrainOriginY)
						/ TerrainCellSize);
					const int64 MaximumCellY = FMath::CeilToInt64(
						(WorldY + RenderedHalfSize - TerrainOriginY)
						/ TerrainCellSize);
					float MinimumTerrainHeight =
						TNumericLimits<float>::Max();
					for (int64 CellY = MinimumCellY;
						CellY <= MaximumCellY;
						++CellY)
					{
						for (int64 CellX = MinimumCellX;
							CellX <= MaximumCellX;
							++CellX)
						{
							MinimumTerrainHeight = FMath::Min(
								MinimumTerrainHeight,
								SampleTerrainHeight(
									TerrainOriginX
										+ static_cast<double>(CellX)
											* TerrainCellSize,
									TerrainOriginY
										+ static_cast<double>(CellY)
											* TerrainCellSize,
									Context.NoiseOffset));
						}
					}
					const float TopZ = MinimumTerrainHeight
						- BackdropClearance;
					constexpr float BackdropThickness = 24.0f;
					Backdrop.Instances.Emplace(
						FRotator::ZeroRotator,
						FVector(
							WorldX,
							WorldY,
							TopZ - BackdropThickness * 0.5f),
						FVector(
							BackdropCellSize * BackdropOverlapScale / 100.0f,
							BackdropCellSize * BackdropOverlapScale / 100.0f,
							BackdropThickness / 100.0f));
				}
			}

			// The coarse field is intentionally finite. A single still-cheaper
			// floor below the lowest real terrain hides its remote edge, so no
			// camera angle can reveal the renderer's black clear color.
			FLevelLayer& HorizonFloor = AddLayer(
				Layout,
				TEXT("HorizonFloor"),
				ELayerPrimitive::Cube,
				ScaleBackdropColor(GrassColor, 0.68f),
				false,
				ELevelLayerRenderMode::VoxelUnlit);
			constexpr float HorizonTopZ = 24.0f;
			constexpr float HorizonBottomZ = -320.0f;
			HorizonFloor.Instances.Emplace(
				FRotator::ZeroRotator,
				FVector(
					0.0f,
					0.0f,
					(HorizonTopZ + HorizonBottomZ) * 0.5f),
				FVector(
					400.0f,
					400.0f,
					(HorizonTopZ - HorizonBottomZ) / 100.0f));

			FLevelLayer& Soil = AddLayer(
				Layout,
				TEXT("Soil"),
				ELayerPrimitive::Cube,
				ResolveMaterialColor(
					Context,
					TEXT("soil"),
					FLinearColor(0.16f, 0.045f, 0.012f)),
				true,
				ELevelLayerRenderMode::CollisionOnly);
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

			// 固定候选区附近生成一个 seed 可重复的大湖。先从原始柏林
			// 地形取得岸线最低点，再用连续权重压出碗形湖床；边缘权重为
			// 0，因此不会产生一圈突然断开的悬崖。
			FRandomStream LakeRandom = Context.MakeRuleStream(0x4c414b45u);
			Context.LakeCenter = FVector2D(
				-120.0f + LakeRandom.FRandRange(-90.0f, 90.0f),
				140.0f + LakeRandom.FRandRange(-70.0f, 70.0f));
			Context.LakeRadius = FVector2D(
				LakeRandom.FRandRange(350.0f, 410.0f),
				LakeRandom.FRandRange(250.0f, 300.0f));
			float MinimumRimHeight = TNumericLimits<float>::Max();
			for (int32 Y = 0; Y < TerrainCellsY; ++Y)
			{
				for (int32 X = 0; X < TerrainCellsX; ++X)
				{
					const float WorldX = TerrainOriginX
						+ static_cast<float>(X) * TerrainCellSize;
					const float WorldY = TerrainOriginY
						+ static_cast<float>(Y) * TerrainCellSize;
					const float NX = (WorldX - Context.LakeCenter.X)
						/ Context.LakeRadius.X;
					const float NY = (WorldY - Context.LakeCenter.Y)
						/ Context.LakeRadius.Y;
					const float Radius = FMath::Sqrt(NX * NX + NY * NY);
					if (Radius >= 0.92f && Radius <= 1.08f)
					{
						MinimumRimHeight = FMath::Min(
							MinimumRimHeight,
							Context.SurfaceAtCell(X, Y));
					}
				}
			}
			Context.LakeSurfaceZ = FMath::GridSnap(
				MinimumRimHeight - TerrainCellSize,
				TerrainCellSize);
			for (int32 Y = 0; Y < TerrainCellsY; ++Y)
			{
				for (int32 X = 0; X < TerrainCellsX; ++X)
				{
					const float WorldX = TerrainOriginX
						+ static_cast<float>(X) * TerrainCellSize;
					const float WorldY = TerrainOriginY
						+ static_cast<float>(Y) * TerrainCellSize;
					const float NX = (WorldX - Context.LakeCenter.X)
						/ Context.LakeRadius.X;
					const float NY = (WorldY - Context.LakeCenter.Y)
						/ Context.LakeRadius.Y;
					const float Radius = FMath::Sqrt(NX * NX + NY * NY);
					if (Radius >= 1.0f)
					{
						continue;
					}
					const float Interior = FMath::Clamp(
						(1.0f - Radius) / 0.72f,
						0.0f,
						1.0f);
					const float SmoothInterior = Interior * Interior
						* (3.0f - 2.0f * Interior);
					const float TargetDepth = FMath::Lerp(
						8.0f,
						128.0f,
						SmoothInterior);
					const float TargetBed = Context.LakeSurfaceZ - TargetDepth;
					const int32 Index = Y * TerrainCellsX + X;
					const float BlendWeight = FMath::Clamp(
						(1.0f - Radius) / 0.82f,
						0.0f,
						1.0f);
					Context.SurfaceHeights[Index] = FMath::GridSnap(
						FMath::Lerp(
							Context.SurfaceHeights[Index],
							FMath::Min(Context.SurfaceHeights[Index], TargetBed),
							BlendWeight),
						TerrainCellSize);
				}
			}
			Layout.Terrain.Heights = Context.SurfaceHeights;
			for (int32 Index = 0; Index < Context.SurfaceHeights.Num(); ++Index)
			{
				Layout.Terrain.ColorBands[Index] =
					SelectTerrainColorBand(Context.SurfaceHeights[Index]);
			}

			// Backdrop 最初按未雕刻地形放置。湖床降低后再收紧与湖相交
			// 的粗块上表面，保证远景代理永远在真实流式地形之下。
			for (FTransform& Instance : Backdrop.Instances)
			{
				const FVector Scale = Instance.GetScale3D();
				const FVector Location = Instance.GetLocation();
				const float HalfX = Scale.X * 50.0f;
				const float HalfY = Scale.Y * 50.0f;
				if (FMath::Abs(Location.X - Context.LakeCenter.X)
						> HalfX + Context.LakeRadius.X
					|| FMath::Abs(Location.Y - Context.LakeCenter.Y)
						> HalfY + Context.LakeRadius.Y)
				{
					continue;
				}
				const int32 MinX = Context.ToCellX(Location.X - HalfX);
				const int32 MaxX = Context.ToCellX(Location.X + HalfX);
				const int32 MinY = Context.ToCellY(Location.Y - HalfY);
				const int32 MaxY = Context.ToCellY(Location.Y + HalfY);
				float MinimumHeight = TNumericLimits<float>::Max();
				for (int32 CellY = MinY; CellY <= MaxY; ++CellY)
				{
					for (int32 CellX = MinX; CellX <= MaxX; ++CellX)
					{
						MinimumHeight = FMath::Min(
							MinimumHeight,
							Context.SurfaceAtCell(CellX, CellY));
					}
				}
				const float CurrentTop = Location.Z + Scale.Z * 50.0f;
				const float SafeTop = FMath::Min(
					CurrentTop,
					MinimumHeight - TerrainCellSize);
				Instance.SetLocation(FVector(
					Location.X,
					Location.Y,
					SafeTop - Scale.Z * 50.0f));
			}

			// 溪流形状由 seed 决定。按固定候选顺序挑选第一块远离溪流
			// 的地面，使所有联机端可独立得到同一个房屋预留区。
			const FVector2D HouseCandidates[] = {
				FVector2D(650.0f, -720.0f),
				FVector2D(920.0f, 260.0f),
				FVector2D(-120.0f, 920.0f),
				FVector2D(-1050.0f, 180.0f)
			};
			FVector2D HouseCenter = HouseCandidates[0];
			for (const FVector2D Candidate : HouseCandidates)
			{
				if (!Context.IsNearStream(
						Candidate.X, Candidate.Y, 760.0f)
					&& !Context.IsInsideLake(
						Candidate.X, Candidate.Y, 760.0f))
				{
					HouseCenter = Candidate;
					break;
				}
			}
			Context.HouseCenter = HouseCenter;
			Layout.HouseLocation = FVector(HouseCenter, 0.0f);
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
				ELevelLayerRenderMode::Liquid);
			Stream.MaterialId = TEXT("water");
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

		void GenerateLake(FGenerationContext& Context, FLevelLayout& Layout)
		{
			FLevelLayer& Lake = AddLayer(
				Layout,
				TEXT("Lake"),
				ELayerPrimitive::Cube,
				ResolveMaterialColor(
					Context,
					TEXT("water"),
					FLinearColor(0.06f, 0.34f, 0.72f, 0.82f)),
				false,
				ELevelLayerRenderMode::Liquid);
			Lake.MaterialId = TEXT("water");
			for (int32 Y = 0; Y < TerrainCellsY; ++Y)
			{
				for (int32 X = 0; X < TerrainCellsX; ++X)
				{
					const float WorldX = TerrainOriginX
						+ static_cast<float>(X) * TerrainCellSize;
					const float WorldY = TerrainOriginY
						+ static_cast<float>(Y) * TerrainCellSize;
					if (!Context.IsInsideLake(WorldX, WorldY)
						|| Context.LakeSurfaceZ
							- Context.SurfaceAtCell(X, Y) < 4.0f)
					{
						continue;
					}
					constexpr float SurfaceThickness = 8.0f;
					Lake.Instances.Emplace(
						FRotator::ZeroRotator,
						FVector(
							WorldX,
							WorldY,
							Context.LakeSurfaceZ
								- SurfaceThickness * 0.5f),
						FVector(
							TerrainCellSize * 1.004f / 100.0f,
							TerrainCellSize * 1.004f / 100.0f,
							SurfaceThickness / 100.0f));
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
				const bool bClearOfHouse =
					FMath::Abs(X - Context.HouseCenter.X) > 850.0f
					|| FMath::Abs(Y - Context.HouseCenter.Y) > 710.0f;
				if (bClearOfDefaultSpawn
					&& bClearOfHouse
					&& !Context.IsInsideLake(X, Y, 90.0f)
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
				FVector Location = FVector::ZeroVector;
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
			FRandomStream Random = Context.MakeRuleStream(0x54524545u);
			const int32 Count = ResolveDecoratorCount(
				Context,
				Definition,
				42,
				0x54524545u);
			// 所有树部件共用一个方块尺寸。除了让轮廓接近 Minecraft，
			// 也让区块代理只需少量材质/尺寸分组。
			constexpr float BlockSize = 18.0f;
			// 七格树冠直径为 126。斜视投影会把前后距离压缩，因此留出
			// 另一整个树冠的间隔，避免近处树干叠到远处叶面上，视觉上
			// 被误读为“树干穿叶”。
			constexpr float MinimumTreeSpacing = BlockSize * 14.0f;
			TArray<FVector2D> PlacedTreeLocations;
			PlacedTreeLocations.Reserve(Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				FVector Location = FVector::ZeroVector;
				bool bFoundFlatTreeLocation = false;
				// 树干是方柱，不能像圆形装饰那样插进阶梯地形。
				// 只接受整个一格树干占地都处于同一高度的平台；否则地形
				// 会斜切侧面，在根部留下用户能看到的三角形棕色尖角。
				for (int32 Attempt = 0;
					Attempt < 16 && !bFoundFlatTreeLocation;
					++Attempt)
				{
					if (!FindScatterLocation(
						Context,
						Random,
						310.0f,
						true,
						Location))
					{
						break;
					}
					const FVector2D Candidate(Location.X, Location.Y);
					const bool bClearOfOtherTrees =
						!PlacedTreeLocations.ContainsByPredicate(
							[Candidate](const FVector2D Existing)
							{
								return FVector2D::Distance(Candidate, Existing)
									< MinimumTreeSpacing;
							});
					bFoundFlatTreeLocation = bClearOfOtherTrees
						&& Context.IsFlatFootprint(
						Location.X,
						Location.Y,
						// 2x2 树干旋转 45 度后的世界轴外接半径。
						BlockSize * FMath::Sqrt(2.0f));
				}
				if (!bFoundFlatTreeLocation)
				{
					continue;
				}
				PlacedTreeLocations.Emplace(Location.X, Location.Y);
				// 2x2 的方块截面仍保持 Minecraft 比例，同时避免斜视时每个
				// 可见面只有树冠的 1/7 宽、被误读成两张薄片。
				constexpr int32 TrunkWidthBlocks = 2;
				constexpr int32 TrunkDepthSlices = 2;
				FVector TreeSurfaceLocation = Location;
				TreeSurfaceLocation.Z = Context.SurfaceAt(Location.X, Location.Y);
				// 树根下沉一个完整体素。斜俯视时，如果第一格完整露在地面上，
				// 方柱的近角与两条底边会投影成向内收的 V，看起来像两张
				// 薄片张开。地下根体素使地面切过连续侧面，底面永远不会
				// 进入视野；树、枝、叶继续共享同一个整数体素格。
				const FVector GroundedLocation = TreeSurfaceLocation
					- FVector(0.0f, 0.0f, BlockSize);
				// 2.5D 镜头沿世界对角线观察。若树仍按世界轴摆放，镜头正对
				// 立方体近角，树冠底边会投影成向内收的 V。整棵树统一旋转
				// 45 度，让一个方形正面朝向镜头；逻辑 mask 与碰撞也使用
				// 同一朝向，而不是只在材质或顶点阶段伪造透视。
				const FQuat TreeRotation = FRotator(
					0.0f,
					DecorationFacingYaw,
					0.0f).Quaternion();
				// 偶数宽树干的格心位于半整数坐标；奇数宽树冠和单格枝条
				// 整体偏移半格，确保所有部件仍落在同一三维体素晶格。
				const FVector HalfCellLatticeOffset =
					TreeRotation.RotateVector(FVector(
						BlockSize * 0.5f,
						BlockSize * 0.5f,
						0.0f));
				// 普通林木保留足够的可砍高度，但不再使用远高于叶冠的细长
				// 电线杆比例。地下根格不计入玩家看到的树干高度。
				const int32 TrunkBlocks = Random.RandRange(10, 13);
				const FGuid TreeAggregateId =
					FGuid::NewDeterministicGuid(
						FString::Printf(
							TEXT("PlayableForest|Seed=%d|TreeAggregate=%d"),
							Context.Seed,
							Index),
						static_cast<uint64>(
							static_cast<uint32>(Context.Seed)));

				for (int32 DepthSlice = 0;
					DepthSlice < TrunkDepthSlices;
					++DepthSlice)
				{
					FFragmentSourceMask TrunkMask =
						MakeEmptyMask(
							TrunkWidthBlocks + 2,
							TrunkBlocks,
							BlockSize,
							2);
					TrunkMask.GeometryStyle =
						EFragmentSourceGeometryStyle::VoxelBlocks;
					for (int32 Y = 0; Y < TrunkBlocks; ++Y)
					{
						for (int32 X = 1; X <= TrunkWidthBlocks; ++X)
						{
							SetSolid(TrunkMask, X, Y);
						}
					}
					const float DepthOffset =
						(static_cast<float>(DepthSlice) - 0.5f) * BlockSize;
					AddFragmentSource(
						Context,
						Layout,
						TEXT("TreeTrunk"),
						Definition
							? Definition->MaterialId
							: FName(TEXT("wood")),
						TrunkColor,
						FTransform(
							TreeRotation,
							GroundedLocation
								+ TreeRotation.RotateVector(FVector(
									0.0f,
									DepthOffset,
									0.0f))
								+ FVector(
									0.0f,
									0.0f,
									static_cast<float>(TrunkMask.Height)
										* BlockSize * 0.5f),
							FVector::OneVector),
						MoveTemp(TrunkMask),
						Definition ? Definition->bEnableCollision : true,
						TreeAggregateId,
						DepthSlice == 0);
				}

				// 两根短枝只沿世界方格的四个主方向生长，并埋在树冠最厚的
				// 中层。最多两格长，外面至少保留一层叶体素，避免正面出现
				// 棕色 L 形木板。逻辑上仍是独立 mask，砍倒后会和树干、
				// 树冠一起进入动态 aggregate。
				constexpr int32 BranchCount = 2;
				const int32 FirstBranchDirection = Random.RandRange(0, 3);
				for (int32 BranchIndex = 0;
					BranchIndex < BranchCount;
					++BranchIndex)
				{
					const int32 DirectionIndex =
						(FirstBranchDirection + BranchIndex) % 4;
					const float BranchYaw =
						static_cast<float>(DirectionIndex) * 90.0f;
					const FVector LocalBranchDirection(
						FMath::Cos(FMath::DegreesToRadians(BranchYaw)),
						FMath::Sin(FMath::DegreesToRadians(BranchYaw)),
						0.0f);
					const FVector BranchDirection =
						TreeRotation.RotateVector(LocalBranchDirection);
					// 枝条保留随机方向，但只占一格：两格枝条在削角后的
					// 树冠外表偶尔会露成棕色方块，看起来像贴在叶面上。
					constexpr int32 BranchBlocks = 1;
					const int32 AttachmentBlock =
						TrunkBlocks - 3 + BranchIndex;
					const FVector BranchStart = GroundedLocation
						+ FVector(
							0.0f,
							0.0f,
							(static_cast<float>(AttachmentBlock) + 0.5f)
								* BlockSize);
					// mask 左右各有一格 padding，所以实体格的局部中心
					// 关于 0 对称。要让第一格从主干相邻的整数格开始，
					// 整个 mask 中心必须位于 (格数 + 1) / 2，而不是旧的
					// 格数 / 2；旧公式会把每根枝条错开半个体素。
					const FVector BranchCenter = BranchStart
						+ HalfCellLatticeOffset
						+ BranchDirection
							* (static_cast<float>(BranchBlocks + 1)
								* BlockSize * 0.5f);
					FFragmentSourceMask BranchMask =
						MakeEmptyMask(BranchBlocks + 2, 3, BlockSize, 2);
					BranchMask.SupportMode = EFragmentSupportMode::None;
					BranchMask.GeometryStyle =
						EFragmentSourceGeometryStyle::VoxelBlocks;
					for (int32 X = 1; X <= BranchBlocks; ++X)
					{
						SetSolid(BranchMask, X, 1);
					}
					AddFragmentSource(
						Context,
						Layout,
						TEXT("TreeBranch"),
						Definition
							? Definition->MaterialId
							: FName(TEXT("wood")),
						TrunkColor * 0.92f,
						FTransform(
							FRotator(
								0.0f,
								DecorationFacingYaw + BranchYaw,
								0.0f),
							BranchCenter),
						MoveTemp(BranchMask),
						false,
						TreeAggregateId);
				}

				// 方块树冠不是一个被贪心网格合并后的 7x7x7 实心盒子。
				// 使用五层、五格宽的离散叶簇：下三层是缺角方块，中上层
				// 收成 3x3 与十字形。每个 Y 切片仍是独立 MatterFlux mask，
				// 因而可切割、燃烧并随整棵树脱离；渲染层只合并相邻外表面。
				constexpr int32 CanopyDepthSlices = 5;
				constexpr int32 CanopyHeightBlocks = 5;
				constexpr int32 MaximumCanopyRadius = 2;
				const float CanopyBottomBlock =
					static_cast<float>(TrunkBlocks - 4);
				for (int32 SliceIndex = 0;
					SliceIndex < CanopyDepthSlices;
					++SliceIndex)
				{
					const int32 DepthBlock = SliceIndex - CanopyDepthSlices / 2;
					constexpr int32 MaskPadding = 1;
					FFragmentSourceMask LeafMask =
						MakeEmptyMask(
							MaximumCanopyRadius * 2 + 1 + MaskPadding * 2,
							CanopyHeightBlocks + MaskPadding * 2,
							BlockSize,
							3);
					LeafMask.SupportMode = EFragmentSupportMode::None;
					LeafMask.GeometryStyle =
						EFragmentSourceGeometryStyle::VoxelBlocks;
					for (int32 HeightBlock = 0;
						HeightBlock < CanopyHeightBlocks;
						++HeightBlock)
					{
						for (int32 HorizontalBlock = -MaximumCanopyRadius;
							HorizontalBlock <= MaximumCanopyRadius;
							++HorizontalBlock)
						{
							const int32 AbsX = FMath::Abs(HorizontalBlock);
							const int32 AbsY = FMath::Abs(DepthBlock);
							bool bLeafCell = false;
							if (HeightBlock <= 2)
							{
								// 5x5 的主体削去四角，轮廓仍由完整方块组成，
								// 但不会再次退化成一整个塑料立方体。
								bLeafCell = AbsX <= 2 && AbsY <= 2
									&& !(AbsX == 2 && AbsY == 2);
								if (bLeafCell && HeightBlock == 2)
								{
									// 每棵树在上部外沿留下一个确定性缺口，
									// 增加随机感而不产生悬空叶块。
									const int32 Notch =
										(Index * 17 + Context.Seed) & 3;
									bLeafCell = !(
										(Notch == 0 && HorizontalBlock == 0 && DepthBlock == -2)
										|| (Notch == 1 && HorizontalBlock == 2 && DepthBlock == 0)
										|| (Notch == 2 && HorizontalBlock == 0 && DepthBlock == 2)
										|| (Notch == 3 && HorizontalBlock == -2 && DepthBlock == 0));
								}
							}
							else if (HeightBlock == 3)
							{
								bLeafCell = AbsX <= 1 && AbsY <= 1;
							}
							else
							{
								// 最上层沿格轴形成 +，避免平顶大盒子，也不
								// 使用斜面、圆球或薄片冒充体素。
								bLeafCell = AbsX + AbsY <= 1;
							}
							if (bLeafCell)
							{
								SetSolid(
									LeafMask,
									HorizontalBlock + MaximumCanopyRadius
										+ MaskPadding,
									HeightBlock + MaskPadding);
							}
						}
					}
					AddFragmentSource(
						Context,
						Layout,
						TEXT("TreeLeaves"),
						TEXT("leaf"),
						CanopyColor,
						FTransform(
							TreeRotation,
							GroundedLocation
								+ HalfCellLatticeOffset
								+ TreeRotation.RotateVector(FVector(
								0.0f,
								static_cast<float>(DepthBlock) * BlockSize,
								(CanopyBottomBlock
									+ CanopyHeightBlocks * 0.5f) * BlockSize)),
							FVector::OneVector),
						MoveTemp(LeafMask),
						false,
						TreeAggregateId);
				}
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
			&GenerateLake,
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
