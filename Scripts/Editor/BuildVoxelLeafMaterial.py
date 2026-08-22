"""导入原创叶片像素纹理，并生成不透明的方块叶材质。"""

from pathlib import Path

import unreal


PROJECT_DIR = Path(unreal.Paths.project_dir())
SOURCE_TEXTURE = PROJECT_DIR / "SourceArt" / "T_LeafBlocksV3Mask.png"
TEXTURE_PATH = "/Game/MatterFlux/Materials/T_LeafBlocksV3"
BASE_MATERIAL_PATH = "/Game/MatterFlux/Materials/M_VoxelPalette"
LEAF_MATERIAL_PATH = "/Game/MatterFlux/Materials/M_VoxelLeaf"


def import_leaf_texture() -> unreal.Texture2D:
    """可重复地导入源图，并强制使用体素游戏需要的点采样。"""
    if unreal.EditorAssetLibrary.does_asset_exist(TEXTURE_PATH):
        unreal.EditorAssetLibrary.delete_asset(TEXTURE_PATH)

    task = unreal.AssetImportTask()
    task.filename = str(SOURCE_TEXTURE)
    task.destination_path = "/Game/MatterFlux/Materials"
    task.destination_name = "T_LeafBlocksV3"
    task.automated = True
    task.replace_existing = True
    task.save = False
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    texture = unreal.load_asset(TEXTURE_PATH)
    if not isinstance(texture, unreal.Texture2D):
        raise RuntimeError(f"叶片纹理导入失败：{SOURCE_TEXTURE}")
    texture.set_editor_property("filter", unreal.TextureFilter.TF_NEAREST)
    texture.set_editor_property(
        "mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    texture.set_editor_property("max_texture_size", 16)
    texture.set_editor_property("srgb", True)
    texture.set_editor_property("never_stream", True)
    unreal.EditorAssetLibrary.save_loaded_asset(texture)
    return texture


def build_leaf_material(texture: unreal.Texture2D) -> unreal.Material:
    """保留体素明暗面和像素色差，不使用会透出内部切片的透明孔。"""
    if unreal.EditorAssetLibrary.does_asset_exist(LEAF_MATERIAL_PATH):
        unreal.EditorAssetLibrary.delete_asset(LEAF_MATERIAL_PATH)
    material = unreal.EditorAssetLibrary.duplicate_asset(
        BASE_MATERIAL_PATH, LEAF_MATERIAL_PATH)
    if not isinstance(material, unreal.Material):
        raise RuntimeError("无法从 M_VoxelPalette 创建 M_VoxelLeaf")

    old_base = unreal.MaterialEditingLibrary.get_material_property_input_node(
        material, unreal.MaterialProperty.MP_BASE_COLOR)
    old_base_output = (
        unreal.MaterialEditingLibrary.get_material_property_input_node_output_name(
            material, unreal.MaterialProperty.MP_BASE_COLOR)
        if old_base else "")
    if old_base is None:
        raise RuntimeError("M_VoxelPalette 缺少 Base Color 输入")

    texture_node = unreal.MaterialEditingLibrary.create_material_expression(
        material,
        unreal.MaterialExpressionTextureSampleParameter2D,
        node_pos_x=-260,
        node_pos_y=120)
    texture_node.set_editor_property("parameter_name", "LeafTexture")
    texture_node.set_editor_property("texture", texture)

    tint_node = unreal.MaterialEditingLibrary.create_material_expression(
        material,
        unreal.MaterialExpressionMultiply,
        node_pos_x=60,
        node_pos_y=0)
    unreal.MaterialEditingLibrary.connect_material_expressions(
        old_base, old_base_output, tint_node, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(
        texture_node, "RGB", tint_node, "B")
    vertex_color = unreal.MaterialEditingLibrary.create_material_expression(
        material,
        unreal.MaterialExpressionVertexColor,
        node_pos_x=60,
        node_pos_y=180)
    ao_node = unreal.MaterialEditingLibrary.create_material_expression(
        material,
        unreal.MaterialExpressionMultiply,
        node_pos_x=300,
        node_pos_y=0)
    unreal.MaterialEditingLibrary.connect_material_expressions(
        tint_node, "", ao_node, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(
        vertex_color, "RGB", ao_node, "B")
    unreal.MaterialEditingLibrary.connect_material_property(
        ao_node, "", unreal.MaterialProperty.MP_BASE_COLOR)
    # MatterFlux 的树冠由闭合体素块构成。若每一层都使用同一张透明
    # 遮罩，孔洞会沿深度对齐，直接暴露内部树干和二维切片。这里采用
    # Minecraft Fast Leaves 的不透明路径，Alpha 仅保留在源图中供将来
    # 其它表现使用，不接入材质输出。
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    material.set_editor_property("two_sided", False)
    unreal.MaterialEditingLibrary.layout_material_expressions(material)
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    return material


if __name__ == "__main__":
    if not SOURCE_TEXTURE.is_file():
        raise FileNotFoundError(f"找不到叶片源纹理：{SOURCE_TEXTURE}")
    leaf_texture = import_leaf_texture()
    leaf_material = build_leaf_material(leaf_texture)
    unreal.log(
        f"MatterFlux 叶片材质已生成：{leaf_material.get_path_name()}")
