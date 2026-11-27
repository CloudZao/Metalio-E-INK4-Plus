// FreeType 模块注册清单，按 FT_USE_MODULE 协议多次包含，不设置头文件保护。
// 阅读与字体转换使用普通栅格化；保留 TrueType/OpenType、CFF、提示与位图渲染，不注册未使用的 SDF 渲染器。
FT_USE_MODULE(FT_Module_Class, autofit_module_class)
FT_USE_MODULE(FT_Driver_ClassRec, tt_driver_class)
FT_USE_MODULE(FT_Driver_ClassRec, cff_driver_class)
FT_USE_MODULE(FT_Module_Class, psaux_module_class)
FT_USE_MODULE(FT_Module_Class, psnames_module_class)
FT_USE_MODULE(FT_Module_Class, pshinter_module_class)
FT_USE_MODULE(FT_Module_Class, sfnt_module_class)
FT_USE_MODULE(FT_Renderer_Class, ft_smooth_renderer_class)
FT_USE_MODULE(FT_Renderer_Class, ft_raster1_renderer_class)
FT_USE_MODULE(FT_Renderer_Class, ft_svg_renderer_class)
