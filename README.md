# Blender for HarmonyOS - App Frontend

HarmonyOS 端 Blender 移植项目的 ArkTS/DevEco Studio 前端工程。

本仓库是 [blender-harmonyos-meta](https://github.com/你的用户名/blender-harmonyos-meta) 的一部分,负责:

- 通过 XComponent 承载 Vulkan 渲染画面
- 通过 NAPI 桥接触屏、鼠标、键盘事件至 libblender.so
- 启动画面、资源文件拷贝、Python 环境初始化

## 依赖

本工程无法独立运行,需要配套:

1. `libblender.so` 及所有 `.so` 依赖放入 `entry/libs/arm64-v8a/`
2. Blender datafiles / scripts / Python lib 放入 `entry/src/main/resources/rawfile/`

详细编译步骤请参考 [Meta 仓库](https://github.com/panedioic/blender-harmonyos-meta) 的 BUILD 文档。

## 开发环境

- DevEco Studio 6.0+
- HarmonyOS API 20+
- 真机: 支持 Vulkan 的 HarmonyOS NEXT 设备

## 许可证

GPL-3.0-or-later