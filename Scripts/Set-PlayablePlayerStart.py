import unreal

level_path = "/Game/Default"
if not unreal.EditorLoadingAndSavingUtils.load_map(level_path):
    raise RuntimeError(f"Could not load {level_path}")

player_starts = unreal.GameplayStatics.get_all_actors_of_class(
    unreal.EditorLevelLibrary.get_editor_world(),
    unreal.PlayerStart,
)
if not player_starts:
    raise RuntimeError("Default map has no PlayerStart")

player_start = player_starts[0]
player_start.modify()
player_start.set_actor_location(unreal.Vector(0.0, 0.0, 500.0), False, False)
player_start.set_actor_rotation(unreal.Rotator(0.0, 0.0, 0.0), False)

source_class = unreal.load_class(None, "/Script/MatterFlux.Fragment2DSourceActor")
sources = unreal.GameplayStatics.get_all_actors_of_class(
    unreal.EditorLevelLibrary.get_editor_world(),
    source_class,
)
if not sources:
    raise RuntimeError("Default map has no Fragment2DSourceActor")

source = sources[0]
source.modify()
source.set_actor_location(unreal.Vector(1100.0, 700.0, 320.0), False, False)
source.set_actor_scale3d(unreal.Vector(0.40, 0.40, 0.40))

if not unreal.EditorAssetLibrary.save_asset(level_path, False):
    raise RuntimeError("Could not save the updated Default map")

unreal.log(
    f"Updated {player_start.get_name()} to {player_start.get_actor_location()} "
    f"and {source.get_name()} to {source.get_actor_location()}"
)
