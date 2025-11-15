# GitHub Actions 构建监控指南

## 🚀 构建已触发！

✅ **已成功推送到上游仓库**，GitHub Actions工作流已自动触发

## 📋 构建详情

- **仓库**: https://github.com/BlackStar1453/licecap
- **分支**: main
- **最新提交**: `Adjust default similarity threshold to 90% for practical use`
- **提交哈希**: `158a9c6f`

## 🔍 如何监控构建状态

### 方法1: 直接访问GitHub Actions

1. **打开仓库**: https://github.com/BlackStar1453/licecap
2. **点击"Actions"标签**: https://github.com/BlackStar1453/licecap/actions
3. **查看最新工作流**: 寻找"Build LICEcap"工作流
4. **监控进度**:
   - 🔄 **进行中**: 黄色圆点表示正在构建
   - ✅ **成功**: 绿色勾号表示构建成功
   - ❌ **失败**: 红色叉号表示构建失败

### 方法2: 检查构建作业

工作流包含以下作业：

| 作业 | 平台 | 预期时间 | 状态 |
|------|------|----------|------|
| `build-macos` | macOS | 5-10分钟 | ⏳ |
| `build-windows` | Windows | 3-8分钟 | ⏳ |
| `build-info` | 总结 | 1分钟 | ⏳ |

### 方法3: 命令行检查（GitHub CLI）

如果你安装了GitHub CLI：

```bash
gh run list --repo BlackStar1453/licecap
gh run view --repo BlackStar1453/licecap
```

## 📦 构建产物

成功构建后，你将获得以下文件：

### macOS
- `licecap-macos` artifact
- `licecap.app.zip` - 主应用程序
- `licecap<version>.dmg` - 安装包（如果DMG生成成功）

### Windows
- `licecap-windows` artifact
- `Release_Win32/` 文件夹包含:
  - `licecap.exe` - 主程序
  - 依赖DLL文件

## 🆘 常见问题排查

### 如果构建失败：

1. **查看日志**: 点击失败的作业查看详细错误信息
2. **常见问题**:
   - macOS: PHP依赖问题（已解决）
   - Windows: MSBuild配置问题
   - 依赖缺失: WDL库路径问题

### 手动触发构建：

如果自动构建没有启动：

1. 访问 https://github.com/BlackStar1453/licecap/actions
2. 点击"Build LICEcap"工作流
3. 点击"Run workflow"按钮
4. 选择分支并触发

## ⏱️ 预期时间线

| 阶段 | 时间 | 说明 |
|------|------|------|
| ✅ 代码推送 | 0分钟 | 已完成 |
| 🔄 构建启动 | 1-2分钟 | GitHub Actions启动 |
| 🔄 macOS构建 | 5-10分钟 | 编译和打包 |
| 🔄 Windows构建 | 3-8分钟 | MSBuild编译 |
| 🔄 构建完成 | 15-20分钟 | 生成artifacts |
| ✅ 可下载 | 20分钟后 | 从Actions页面下载 |

## 🎯 下一步

构建完成后：

1. **下载对应平台的构建产物**
2. **测试重复帧删除功能**:
   ```ini
   dup_remove_enable=1
   dup_similarity=0.90  # 现在是90%默认值
   ```
3. **验证文件大小减少效果**
4. **如需调整**: 可以继续优化配置参数

---

**当前状态**: 🔄 GitHub Actions正在构建中...

访问 https://github.com/BlackStar1453/licecap/actions 查看实时状态！