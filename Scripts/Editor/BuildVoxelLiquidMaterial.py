"""生成由 Lua 参数驱动、按水深增加不透明度的透明液体材质。"""

import unreal


MATERIAL_PATH = "/Game/MatterFlux/Materials/M_VoxelLiquid"
MATERIAL_DIRECTORY = "/Game/MatterFlux/Materials"


def expression(material, expression_class, x, y):
    """创建表达式并固定编辑器位置，方便初学者打开材质图阅读。"""
    return unreal.MaterialEditingLibrary.create_material_expression(
        material, expression_class, node_pos_x=x, node_pos_y=y)


def scalar(material, name, default, x, y):
    node = expression(
        material, unreal.MaterialExpressionScalarParameter, x, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", default)
    return node


def build_material():
    if unreal.EditorAssetLibrary.does_asset_exist(MATERIAL_PATH):
        unreal.EditorAssetLibrary.delete_asset(MATERIAL_PATH)

    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        "M_VoxelLiquid",
        MATERIAL_DIRECTORY,
        unreal.Material,
        unreal.MaterialFactoryNew())
    if not isinstance(material, unreal.Material):
        raise RuntimeError("无法创建 M_VoxelLiquid")

    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("two_sided", False)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    material.set_editor_property("used_with_instanced_static_meshes", True)
    # Surface Translucency Volume 在斜视水面上保留受光。不同 UE 小版本可能不暴露该 Python 属性，
    # 因此仅对这一编辑器便利属性使用兼容回退。
    try:
        material.set_editor_property(
            "translucency_lighting_mode",
            unreal.TranslucencyLightingMode.TLM_SURFACE)
    except Exception as error:
        unreal.log_warning(
            f"未设置 translucency_lighting_mode，使用引擎默认值：{error}")

    color = expression(
        material, unreal.MaterialExpressionVectorParameter, -720, -220)
    color.set_editor_property("parameter_name", "Color")
    color.set_editor_property(
        "default_value", unreal.LinearColor(0.12, 0.46, 0.78, 1.0))
    shallow = scalar(material, "ShallowOpacity", 0.16, -720, 40)
    deep = scalar(material, "DeepOpacity", 0.88, -720, 120)
    opacity_depth = scalar(material, "OpacityDepth", 160.0, -720, 260)
    roughness = scalar(material, "Roughness", 0.24, -120, -80)

    # DepthFade 返回从水面像素到后方不透明几何的归一化距离。
    # 该值驱动浅/深不透明度插值，因此湖岸能看清湖床，深水中心逐渐遮蔽。
    depth_fade = expression(
        material, unreal.MaterialExpressionDepthFade, -420, 180)
    depth_fade.set_editor_property("opacity_default", 1.0)
    unreal.MaterialEditingLibrary.connect_material_expressions(
        opacity_depth, "", depth_fade, "FadeDistance")

    opacity = expression(
        material, unreal.MaterialExpressionLinearInterpolate, -120, 100)
    unreal.MaterialEditingLibrary.connect_material_expressions(
        shallow, "", opacity, "A")
    unreal.MaterialEditingLibrary.connect_material_expressions(
        deep, "", opacity, "B")
    unreal.MaterialEditingLibrary.connect_material_expressions(
        depth_fade, "", opacity, "Alpha")

    unreal.MaterialEditingLibrary.connect_material_property(
        color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    unreal.MaterialEditingLibrary.connect_material_property(
        roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)
    unreal.MaterialEditingLibrary.connect_material_property(
        opacity, "", unreal.MaterialProperty.MP_OPACITY)
    unreal.MaterialEditingLibrary.layout_material_expressions(material)
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    return material


if __name__ == "__main__":
    result = build_material()
    unreal.log(f"MatterFlux 液体材质已生成：{result.get_path_name()}")
