#include "Material/MatterFluxCombustion.h"

namespace MatterFlux::Combustion
{
	namespace
	{
		constexpr int64 MaximumCombustionCellCount = 1024ll * 1024ll;

		uint32 MixBits(uint32 Value)
		{
			Value ^= Value >> 16u;
			Value *= 0x7feb352du;
			Value ^= Value >> 15u;
			Value *= 0x846ca68bu;
			Value ^= Value >> 16u;
			return Value;
		}

		bool IsPermille(const int32 Value)
		{
			return Value >= 0 && Value <= 1000;
		}

		bool IsValidCombustionMask(
			const FFragmentSourceMask& SourceMask)
		{
			if (SourceMask.Width <= 0
				|| SourceMask.Height <= 0)
			{
				return false;
			}
			const int64 CellCount =
				static_cast<int64>(SourceMask.Width)
				* static_cast<int64>(SourceMask.Height);
			if (CellCount <= 0
				|| CellCount > MaximumCombustionCellCount
				|| static_cast<int64>(SourceMask.SolidMask.Num())
					!= CellCount)
			{
				return false;
			}

			bool bHasFuel = false;
			for (const uint8 Cell : SourceMask.SolidMask)
			{
				if (Cell > 1)
				{
					return false;
				}
				bHasFuel |= Cell != 0;
			}
			return bHasFuel;
		}

		bool IsValidCombustionRule(
			const FMatterFluxCombustionDefinition& Rule)
		{
			return !Rule.Id.IsNone()
				&& !Rule.FuelMaterial.IsNone()
				&& !Rule.FlameMaterial.IsNone()
				&& !Rule.SmokeMaterial.IsNone()
				&& !Rule.ResidueMaterial.IsNone()
				&& IsPermille(Rule.IgnitionChancePermille)
				&& IsPermille(Rule.SpreadChancePermille)
				&& IsPermille(Rule.SmokeChancePermille)
				&& Rule.BurnDurationSteps >= 1
				&& Rule.BurnDurationSteps <= 255;
		}
	}

	bool FMaskCombustion::Initialize(
		const FFragmentSourceMask& SourceMask,
		const FMatterFluxCombustionDefinition& InRule,
		const int32 InSeed)
	{
		bInitialized = false;
		FuelMask.Reset();
		ResidueMask.Reset();
		BurningMask.Reset();
		ActiveBurningIndices.Reset();
		PendingIgnitionEpochs.Reset();
		Width = 0;
		Height = 0;
		Tick = 0;
		PendingIgnitionEpoch = 0;

		if (!IsValidCombustionMask(SourceMask)
			|| !IsValidCombustionRule(InRule))
		{
			return false;
		}

		Rule = InRule;
		Width = SourceMask.Width;
		Height = SourceMask.Height;
		Seed = InSeed;
		FuelMask = SourceMask.SolidMask;
		ResidueMask.Init(0, FuelMask.Num());
		BurningMask.Init(0, FuelMask.Num());
		PendingIgnitionEpochs.Init(0, FuelMask.Num());
		bInitialized = true;
		return true;
	}

	bool FMaskCombustion::CaptureState(
		FStateSnapshot& OutState) const
	{
		if (!bInitialized)
		{
			return false;
		}
		OutState.RuleId = Rule.Id;
		OutState.Width = Width;
		OutState.Height = Height;
		OutState.Seed = Seed;
		OutState.Tick = Tick;
		OutState.FuelMask = FuelMask;
		OutState.ResidueMask = ResidueMask;
		OutState.BurningMask = BurningMask;
		return true;
	}

	bool FMaskCombustion::RestoreState(
		const FStateSnapshot& State,
		const FMatterFluxCombustionDefinition& InRule,
		FString& OutError)
	{
		OutError.Reset();
		const int64 CellCount =
			static_cast<int64>(State.Width)
			* static_cast<int64>(State.Height);
		if (!IsValidCombustionRule(InRule)
			|| State.RuleId != InRule.Id
			|| State.Width <= 0
			|| State.Height <= 0
			|| CellCount <= 0
			|| CellCount > MaximumCombustionCellCount
			|| State.FuelMask.Num() != CellCount
			|| State.ResidueMask.Num() != CellCount
			|| State.BurningMask.Num() != CellCount)
		{
			OutError =
				TEXT("Combustion snapshot dimensions, rule, or array lengths are invalid");
			return false;
		}

		TArray<int32> RestoredActiveIndices;
		for (int32 Index = 0;
			Index < State.FuelMask.Num();
			++Index)
		{
			const uint8 Fuel = State.FuelMask[Index];
			const uint8 Residue = State.ResidueMask[Index];
			const uint8 Burning = State.BurningMask[Index];
			if (Fuel > 1
				|| Residue > 1
				|| (Fuel != 0 && Residue != 0)
				|| Burning > InRule.BurnDurationSteps
				|| (Burning != 0 && Fuel == 0))
			{
				OutError = FString::Printf(
					TEXT("Combustion snapshot cell %d is inconsistent"),
					Index);
				return false;
			}
			if (Burning != 0)
			{
				RestoredActiveIndices.Add(Index);
			}
		}

		Rule = InRule;
		Width = State.Width;
		Height = State.Height;
		Seed = State.Seed;
		Tick = State.Tick;
		FuelMask = State.FuelMask;
		ResidueMask = State.ResidueMask;
		BurningMask = State.BurningMask;
		ActiveBurningIndices =
			MoveTemp(RestoredActiveIndices);
		PendingIgnitionEpochs.Init(0, FuelMask.Num());
		PendingIgnitionEpoch = 0;
		bInitialized = true;
		return true;
	}

	bool FMaskCombustion::Ignite(
		const FIntPoint Cell,
		const FName IgnitionMaterial)
	{
		if (!bInitialized
			|| IgnitionMaterial != Rule.FlameMaterial
			|| !IsInside(Cell))
		{
			return false;
		}
		const int32 Index = ToIndex(Cell);
		if (FuelMask[Index] == 0
			|| BurningMask[Index] != 0
			|| !PassesChance(
				Cell,
				Rule.IgnitionChancePermille,
				0x49474e49u))
		{
			return false;
		}
		BurningMask[Index] =
			static_cast<uint8>(Rule.BurnDurationSteps);
		ActiveBurningIndices.Add(Index);
		return true;
	}

	bool FMaskCombustion::ConstrainFuelMask(
		const TArray<uint8>& AllowedFuelMask)
	{
		if (!bInitialized || AllowedFuelMask.Num() != FuelMask.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < FuelMask.Num(); ++Index)
		{
			if (AllowedFuelMask[Index] == 0)
			{
				FuelMask[Index] = 0;
				BurningMask[Index] = 0;
				ResidueMask[Index] = 0;
			}
		}
		ActiveBurningIndices.RemoveAll(
			[this](const int32 Index)
			{
				return !BurningMask.IsValidIndex(Index)
					|| BurningMask[Index] == 0;
			});
		return true;
	}

	FStepStats FMaskCombustion::Step()
	{
		FStepStats Stats;
		if (!bInitialized || !IsBurning())
		{
			return Stats;
		}
		++Tick;

		TArray<int32> BurningIndices = ActiveBurningIndices;
		BurningIndices.Sort();

		static const FIntPoint NeighborOffsets[] =
		{
			FIntPoint(0, 1),
			FIntPoint(-1, 0),
			FIntPoint(1, 0),
			FIntPoint(0, -1),
			FIntPoint(-1, 1),
			FIntPoint(1, 1),
			FIntPoint(-1, -1),
			FIntPoint(1, -1)
		};
		TArray<int32> CellsToIgnite;
		++PendingIgnitionEpoch;
		if (PendingIgnitionEpoch == 0)
		{
			PendingIgnitionEpochs.Init(0, FuelMask.Num());
			PendingIgnitionEpoch = 1;
		}
		for (const int32 BurningIndex : BurningIndices)
		{
			const FIntPoint Cell(
				BurningIndex % Width,
				BurningIndex / Width);
			if (PassesChance(
				Cell,
				Rule.SmokeChancePermille,
				0x534d4f4bu))
			{
				Stats.SmokeEmissionCells.Add(Cell);
			}

			for (const FIntPoint Offset : NeighborOffsets)
			{
				const FIntPoint Neighbor = Cell + Offset;
				if (!IsInside(Neighbor))
				{
					continue;
				}
				const int32 NeighborIndex = ToIndex(Neighbor);
				if (FuelMask[NeighborIndex] == 0
					|| BurningMask[NeighborIndex] != 0
					|| PendingIgnitionEpochs[NeighborIndex]
						== PendingIgnitionEpoch
					|| !PassesChance(
						Neighbor,
						Rule.SpreadChancePermille,
						0x53505244u
							^ static_cast<uint32>(ToIndex(Cell))))
				{
					continue;
				}
				CellsToIgnite.Add(NeighborIndex);
				PendingIgnitionEpochs[NeighborIndex] =
					PendingIgnitionEpoch;
			}
		}

		TArray<int32> NextActiveBurningIndices;
		NextActiveBurningIndices.Reserve(
			BurningIndices.Num() + CellsToIgnite.Num());
		Stats.ChangedCellIndices.Reserve(
			BurningIndices.Num() + CellsToIgnite.Num());
		for (const int32 Index : BurningIndices)
		{
			Stats.ChangedCellIndices.Add(Index);
			if (BurningMask[Index] > 0)
			{
				--BurningMask[Index];
			}
			if (BurningMask[Index] == 0 && FuelMask[Index] != 0)
			{
				FuelMask[Index] = 0;
				ResidueMask[Index] = 1;
				++Stats.ConsumedFuelCells;
			}
			else if (BurningMask[Index] != 0)
			{
				NextActiveBurningIndices.Add(Index);
			}
		}
		for (const int32 Index : CellsToIgnite)
		{
			if (FuelMask[Index] != 0 && BurningMask[Index] == 0)
			{
				BurningMask[Index] =
					static_cast<uint8>(Rule.BurnDurationSteps);
				NextActiveBurningIndices.Add(Index);
				++Stats.IgnitedCells;
				Stats.ChangedCellIndices.Add(Index);
			}
		}
		NextActiveBurningIndices.Sort();
		ActiveBurningIndices = MoveTemp(
			NextActiveBurningIndices);
		return Stats;
	}

	bool FMaskCombustion::IsBurning() const
	{
		return !ActiveBurningIndices.IsEmpty();
	}

	int32 FMaskCombustion::CountFuelCells() const
	{
		int32 Count = 0;
		for (const uint8 Value : FuelMask)
		{
			Count += Value != 0 ? 1 : 0;
		}
		return Count;
	}

	int32 FMaskCombustion::CountResidueCells() const
	{
		int32 Count = 0;
		for (const uint8 Value : ResidueMask)
		{
			Count += Value != 0 ? 1 : 0;
		}
		return Count;
	}

	bool FMaskCombustion::IsInside(const FIntPoint Cell) const
	{
		return Cell.X >= 0 && Cell.X < Width
			&& Cell.Y >= 0 && Cell.Y < Height;
	}

	int32 FMaskCombustion::ToIndex(const FIntPoint Cell) const
	{
		return Cell.Y * Width + Cell.X;
	}

	bool FMaskCombustion::PassesChance(
		const FIntPoint Cell,
		const int32 ChancePermille,
		const uint32 Salt) const
	{
		if (ChancePermille >= 1000)
		{
			return true;
		}
		if (ChancePermille <= 0)
		{
			return false;
		}
		const uint32 Hash = MixBits(
			static_cast<uint32>(Seed)
				^ static_cast<uint32>(Cell.X) * 0x9e3779b9u
				^ static_cast<uint32>(Cell.Y) * 0x85ebca6bu
				^ Tick * 0xc2b2ae35u
				^ Salt);
		return static_cast<int32>(Hash % 1000u) < ChancePermille;
	}
}
