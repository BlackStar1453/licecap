# ScreenToGif 重复帧删除算法复现指南

## 概述

本文档详细说明了如何将 ScreenToGif 中的高效重复帧删除算法复现到 licecap 项目中。该算法采用像素级精确比较和并行处理，能够有效减少动画文件大小。

## 核心算法原理

### 1. 算法流程图

```
开始 -> 读取帧序列 -> 逐对比较相邻帧 -> 计算像素差异度 -> 判断是否重复 ->
根据模式删除帧 -> 可选：调整显示时间 -> 生成去重后的帧序列 -> 结束
```

### 2. 核心参数

- **相似度阈值** (默认: 90%): 两帧相似度达到该值即被认为是重复
- **删除模式**:
  - `First`: 删除第一帧，保留第二帧
  - `Last`: 删除第二帧，保留第一帧
- **延迟调整模式**:
  - `DontAdjust`: 不调整显示时间
  - `Average`: 取两帧显示时间的平均值
  - `Sum`: 累加两帧显示时间

## 详细实现步骤

### 步骤1: 使用 ace MCP 分析 licecap 项目结构

首先需要了解 licecap 项目的代码结构，特别是图像捕获和GIF生成相关的代码：

```bash
# 启动 ace MCP 进行项目分析
Task(subagent_type: "code-searcher",
     description: "分析 licecap 项目的截图和GIF生成相关代码结构",
     prompt: "请分析当前 licecap 项目的实现方案，重点关注：
     1. 图像捕获和保存的相关代码文件
     2. GIF编码和生成的核心逻辑
     3. 帧管理和存储的数据结构
     4. 用户界面中可能的重复帧删除集成点
     5. 评估重复帧删除算法的集成复杂度
     6. 任务复杂度判断(简单单个/复杂多个)")
```

### 步骤2: 核心数据结构定义

#### 2.1 帧信息结构

```csharp
// 基础帧信息接口
public interface IFrame
{
    int Index { get; set; }           // 帧索引
    string Path { get; set; }         // 图像文件路径
    int Delay { get; set; }           // 显示延迟（毫秒）
    int Width { get; set; }           // 图像宽度
    int Height { get; set; }          // 图像高度
}

// 具体实现（适配 licecap 的现有结构）
public class FrameInfo : IFrame
{
    public int Index { get; set; }
    public string Path { get; set; }
    public int Delay { get; set; }
    public int Width { get; set; }
    public int Height { get; set; }

    // licecap 特有的属性
    public DateTime CaptureTime { get; set; }
    public long FileSize { get; set; }
}
```

#### 2.2 枚举定义

```csharp
// 重复帧删除模式
public enum DuplicatesRemovalModes
{
    First = 0,  // 删除第一帧
    Last = 1    // 删除第二帧
}

// 延迟调整模式
public enum DuplicatesDelayModes
{
    DontAdjust = 0,  // 不调整
    Average = 1,     // 平均值
    Sum = 2         // 总和
}
```

### 步骤3: 核心图像比较算法

#### 3.1 主要比较方法

```csharp
/// <summary>
/// 计算两个帧之间的相似度（像素级比较）
/// </summary>
/// <param name="first">第一帧</param>
/// <param name="second">第二帧</param>
/// <returns>相似度百分比 (0-100)</returns>
public static decimal CalculateDifference(IFrame first, IFrame second)
{
    // 1. 加载图像文件
    using var image1 = LoadImage(first.Path);
    using var image2 = LoadImage(second.Path);

    // 2. 验证图像尺寸和格式
    if (image1.Width != image2.Width || image1.Height != image2.Height)
        return 0; // 尺寸不同，相似度为0

    // 3. 计算像素总数
    var pixelCount = image1.Width * image1.Height;

    // 4. 并行比较像素差异
    var changedPixelCount = CountDifferentPixels(image1, image2);

    // 5. 计算相似度百分比
    return ((decimal)(pixelCount - changedPixelCount) / pixelCount) * 100;
}
```

#### 3.2 像素级比较实现

```csharp
/// <summary>
/// 统计两个图像中不同像素的数量
/// </summary>
private static int CountDifferentPixels(Bitmap image1, Bitmap image2)
{
    // 转换为统一格式 (32位ARGB)
    var format = PixelFormat.Format32bppArgb;
    var width = image1.Width;
    var height = image1.Height;

    var buffer1 = new int[width];
    var buffer2 = new int[width];
    var differentCount = 0;

    // 逐行扫描比较
    for (int y = 0; y < height; y++)
    {
        // 读取每行的像素数据
        ReadScanLine(image1, format, y, buffer1);
        ReadScanLine(image2, format, y, buffer2);

        // 并行比较该行的像素
        var rowDifferences = ParallelEnumerable.Range(0, width)
            .Count(x => buffer1[x] != buffer2[x]);

        differentCount += rowDifferences;
    }

    return differentCount;
}

/// <summary>
/// 读取图像的指定扫描线
/// </summary>
private static void ReadScanLine(Bitmap image, PixelFormat format, int y, int[] buffer)
{
    // 使用Bitmap.LockBits和Marshal.Copy实现高性能像素读取
    var rect = new Rectangle(0, y, image.Width, 1);
    var bitmapData = image.LockBits(rect, ImageLockMode.ReadOnly, format);

    try
    {
        Marshal.Copy(bitmapData.Scan0, buffer, 0, image.Width);
    }
    finally
    {
        image.UnlockBits(bitmapData);
    }
}
```

#### 3.3 优化版本（使用 unsafe 代码提升性能）

```csharp
/// <summary>
/// 高性能像素比较（unsafe版本）
/// </summary>
private static unsafe int CountDifferentPixelsUnsafe(Bitmap image1, Bitmap image2)
{
    var width = image1.Width;
    var height = image1.Height;
    var differentCount = 0;

    var rect = new Rectangle(0, 0, width, height);
    var data1 = image1.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
    var data2 = image2.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);

    try
    {
        var ptr1 = (int*)data1.Scan0;
        var ptr2 = (int*)data2.Scan0;
        var totalPixels = width * height;

        // 并行处理像素块
        Parallel.For(0, height, y =>
        {
            var rowPtr1 = ptr1 + y * width;
            var rowPtr2 = ptr2 + y * width;
            var localDiff = 0;

            for (int x = 0; x < width; x++)
            {
                if (rowPtr1[x] != rowPtr2[x])
                    localDiff++;
            }

            Interlocked.Add(ref differentCount, localDiff);
        });
    }
    finally
    {
        image1.UnlockBits(data1);
        image2.UnlockBits(data2);
    }

    return differentCount;
}
```

### 步骤4: 重复帧删除主算法

```csharp
/// <summary>
/// 删除重复帧的核心算法
/// </summary>
/// <param name="frames">帧序列</param>
/// <param name="similarity">相似度阈值 (0-100)</param>
/// <param name="removalMode">删除模式</param>
/// <param name="delayMode">延迟调整模式</param>
/// <param name="progressCallback">进度回调</param>
/// <returns>处理后的帧序列</returns>
public static List<IFrame> RemoveDuplicates(
    List<IFrame> frames,
    decimal similarity,
    DuplicatesRemovalModes removalMode,
    DuplicatesDelayModes delayMode,
    Action<int, int> progressCallback = null)
{
    if (frames == null || frames.Count <= 1)
        return frames;

    var removeList = new List<int>();
    var alterList = new List<int>();

    // 1. 并行检测相似帧对
    var similarFramePairs = DetectSimilarFrames(frames, similarity, progressCallback);

    // 2. 根据删除模式构建删除和修改列表
    foreach (var (firstIndex, secondIndex) in similarFramePairs)
    {
        switch (removalMode)
        {
            case DuplicatesRemovalModes.First:
                removeList.Add(firstIndex);
                alterList.Add(secondIndex);
                break;
            case DuplicatesRemovalModes.Last:
                alterList.Add(firstIndex);
                removeList.Add(secondIndex);
                break;
        }
    }

    if (removeList.Count == 0)
        return frames; // 没有需要删除的帧

    // 3. 排序确保正确处理
    removeList.Sort();
    alterList.Sort();

    // 4. 调整延迟时间（如果需要）
    if (delayMode != DuplicatesDelayModes.DontAdjust)
    {
        AdjustFrameDelays(frames, removeList, alterList, delayMode, progressCallback);
    }

    // 5. 删除重复帧
    var resultFrames = new List<IFrame>(frames);
    for (int i = removeList.Count - 1; i >= 0; i--)
    {
        var index = removeList[i];

        // 删除物理文件
        if (File.Exists(resultFrames[index].Path))
            File.Delete(resultFrames[index].Path);

        // 从列表中移除
        resultFrames.RemoveAt(index);
    }

    // 6. 重新分配索引
    for (int i = 0; i < resultFrames.Count; i++)
    {
        resultFrames[i].Index = i;
    }

    return resultFrames;
}
```

### 步骤5: 相似帧检测算法

```csharp
/// <summary>
/// 检测相似帧对
/// </summary>
private static List<(int, int)> DetectSimilarFrames(
    List<IFrame> frames,
    decimal similarity,
    Action<int, int> progressCallback)
{
    var similarPairs = new List<(int, int)>();

    // 并行比较相邻帧
    var comparisons = ParallelEnumerable.Range(0, frames.Count - 1)
        .Select(i => new
        {
            FirstIndex = i,
            SecondIndex = i + 1,
            Similarity = CalculateDifference(frames[i], frames[i + 1])
        })
        .Where(x => x.Similarity >= similarity)
        .ToList();

    foreach (var comparison in comparisons)
    {
        similarPairs.Add((comparison.FirstIndex, comparison.SecondIndex));

        // 更新进度
        progressCallback?.Invoke(comparison.SecondIndex + 1, frames.Count - 1);
    }

    return similarPairs;
}
```

### 步骤6: 延迟调整算法

```csharp
/// <summary>
/// 调整帧延迟时间
/// </summary>
private static void AdjustFrameDelays(
    List<IFrame> frames,
    List<int> removeList,
    List<int> alterList,
    DuplicatesDelayModes delayMode,
    Action<int, int> progressCallback)
{
    var mode = removeList.Count > 0 && removeList[0] > alterList[0] ? 1 : -1;

    for (int i = 0; i < alterList.Count; i++)
    {
        var alterIndex = alterList[i];
        var removeIndex = alterIndex + mode;

        if (removeIndex >= 0 && removeIndex < frames.Count)
        {
            var alterFrame = frames[alterIndex];
            var removeFrame = frames[removeIndex];

            switch (delayMode)
            {
                case DuplicatesDelayModes.Sum:
                    alterFrame.Delay += removeFrame.Delay;
                    break;
                case DuplicatesDelayModes.Average:
                    alterFrame.Delay = (alterFrame.Delay + removeFrame.Delay) / 2;
                    break;
            }
        }

        // 更新进度
        progressCallback?.Invoke(i + 1, alterList.Count);
    }
}
```

### 步骤7: 集成到 licecap 项目

#### 7.1 使用 ace MCP 搜索 licecap 集成点

```bash
# 搜索 licecap 的图像处理相关代码
Task(subagent_type: "code-searcher",
     description: "搜索 licecap 的图像处理和GIF编码代码",
     prompt: "请搜索 licecap 项目中的以下相关代码：
     1. 图像文件保存和加载的实现
     2. 帧序列管理的数据结构和方法
     3. GIF编码的入口点和参数
     4. 用户界面中可能添加重复帧删除功能的位置
     5. 现有的图像操作方法（如果有的话）
     6. 找到录制过程中帧生成的具体位置")
```

#### 7.2 集成到 licecap 的录制流程

```csharp
/// <summary>
/// licecap 录制器增强版本（包含重复帧删除）
/// </summary>
public class EnhancedLicecapRecorder
{
    private readonly List<IFrame> _frames = new List<IFrame>();
    private readonly DuplicateFrameRemovalSettings _settings;
    private bool _enableDuplicateRemoval;

    public EnhancedLicecapRecorder(DuplicateFrameRemovalSettings settings = null)
    {
        _settings = settings ?? new DuplicateFrameRemovalSettings();
        _enableDuplicateRemoval = _settings.EnableByDefault;
    }

    /// <summary>
    /// 捕获帧（licecap 原有逻辑的增强版本）
    /// </summary>
    public void CaptureFrame(Bitmap frame, int delay)
    {
        var tempPath = GetTempFilePath();

        // 保存帧到临时文件
        frame.Save(tempPath, ImageFormat.Png);

        var frameInfo = new FrameInfo
        {
            Index = _frames.Count,
            Path = tempPath,
            Delay = delay,
            Width = frame.Width,
            Height = frame.Height,
            CaptureTime = DateTime.Now
        };

        _frames.Add(frameInfo);
    }

    /// <summary>
    /// 生成GIF（包含重复帧删除）
    /// </summary>
    public void GenerateGif(string outputPath)
    {
        List<IFrame> framesToEncode = _frames;

        // 如果启用了重复帧删除
        if (_enableDuplicateRemoval)
        {
            Console.WriteLine($"开始处理 {_frames.Count} 帧的重复帧删除...");

            framesToEncode = DuplicateFrameRemover.RemoveDuplicates(
                _frames,
                _settings.SimilarityThreshold,
                _settings.RemovalMode,
                _settings.DelayMode,
                (current, total) => Console.WriteLine($"去重进度: {current}/{total}")
            );

            Console.WriteLine($"重复帧删除完成，原始: {_frames.Count} 帧，去重后: {framesToEncode.Count} 帧");
        }

        // 使用 licecap 原有的 GIF 编码逻辑
        EncodeGifWithLicecap(framesToEncode, outputPath);

        // 清理临时文件
        CleanupTempFiles();
    }

    private void EncodeGifWithLicecap(List<IFrame> frames, string outputPath)
    {
        // 这里调用 licecap 原有的 GIF 编码逻辑
        // 可能需要适配帧数据结构
        // ...
    }

    private void CleanupTempFiles()
    {
        foreach (var frame in _frames)
        {
            if (File.Exists(frame.Path))
                File.Delete(frame.Path);
        }
    }
}
```

#### 7.3 设置类定义

```csharp
/// <summary>
/// 重复帧删除设置
/// </summary>
public class DuplicateFrameRemovalSettings
{
    public bool EnableByDefault { get; set; } = false;
    public decimal SimilarityThreshold { get; set; } = 90m;
    public DuplicatesRemovalModes RemovalMode { get; set; } = DuplicatesRemovalModes.First;
    public DuplicatesDelayModes DelayMode { get; set; } = DuplicatesDelayModes.Average;
    public bool ShowProgress { get; set; } = true;
}
```

### 步骤8: 性能优化建议

#### 8.1 内存管理优化（针对 licecap 的长时间录制）

```csharp
/// <summary>
/// licecap 长时间录制的内存优化版本
/// </summary>
public static class LicecapOptimizedDuplicateRemover
{
    /// <summary>
    /// 实时处理模式（边录制边去重）
    /// </summary>
    public static void ProcessFrameRealTime(
        List<IFrame> existingFrames,
        IFrame newFrame,
        decimal similarityThreshold,
        out bool shouldKeepFrame)
    {
        shouldKeepFrame = true;

        if (existingFrames.Count == 0)
        {
            shouldKeepFrame = true;
            return;
        }

        var lastFrame = existingFrames[existingFrames.Count - 1];

        using var image1 = LoadImage(lastFrame.Path);
        using var image2 = LoadImage(newFrame.Path);

        var similarity = CalculateDifference(image1, image2);

        if (similarity >= similarityThreshold)
        {
            // 如果相似，延长上一帧的延迟时间
            lastFrame.Delay += newFrame.Delay;
            shouldKeepFrame = false;

            // 删除新帧的临时文件
            if (File.Exists(newFrame.Path))
                File.Delete(newFrame.Path);
        }
    }
}
```

### 步骤9: licecap UI 集成

#### 9.1 添加设置界面

```csharp
// 在 licecap 的设置窗口中添加重复帧删除选项
public class LicecapSettingsDialog
{
    private CheckBox _enableDuplicateRemoval;
    private NumericUpDown _similarityThreshold;
    private ComboBox _removalMode;
    private ComboBox _delayMode;

    private void InitializeDuplicateRemovalControls()
    {
        // 相似度设置
        _similarityThreshold = new NumericUpDown
        {
            Minimum = 50,
            Maximum = 100,
            Value = 90,
            Increment = 1,
            DecimalPlaces = 0
        };

        // 删除模式
        _removalMode = new ComboBox();
        _removalMode.Items.AddRange(new object[]
        {
            "删除第一帧",
            "删除第二帧"
        });
        _removalMode.SelectedIndex = 0;

        // 延迟调整模式
        _delayMode = new ComboBox();
        _delayMode.Items.AddRange(new object[]
        {
            "不调整",
            "平均值",
            "总和"
        });
        _delayMode.SelectedIndex = 1;
    }

    public DuplicateFrameRemovalSettings GetDuplicateRemovalSettings()
    {
        return new DuplicateFrameRemovalSettings
        {
            EnableByDefault = _enableDuplicateRemoval.Checked,
            SimilarityThreshold = (decimal)_similarityThreshold.Value,
            RemovalMode = (DuplicatesRemovalModes)_removalMode.SelectedIndex,
            DelayMode = (DuplicatesDelayModes)_delayMode.SelectedIndex
        };
    }
}
```

#### 9.2 状态栏显示去重信息

```csharp
// 在录制状态栏中显示去重信息
public class LicecapStatusBar
{
    private Label _frameCountLabel;
    private Label _duplicateRemovalStatus;

    public void UpdateDuplicateRemovalStatus(int originalFrames, int optimizedFrames)
    {
        if (originalFrames != optimizedFrames)
        {
            var reduction = originalFrames - optimizedFrames;
            var reductionPercent = (reduction * 100.0) / originalFrames;

            _duplicateRemovalStatus.Text =
                $"去重: {reduction} 帧 (-{reductionPercent:F1}%)";
        }
        else
        {
            _duplicateRemovalStatus.Text = "去重: 关闭";
        }
    }
}
```

### 步骤10: 测试和验证

#### 10.1 licecap 特定测试

```csharp
[TestClass]
public class LicecapDuplicateRemovalTests
{
    [TestMethod]
    public void TestLicecapRecordingIntegration()
    {
        // 模拟 licecap 录制过程
        var recorder = new EnhancedLicecapRecorder();

        // 录制一些测试帧
        var testFrame = CreateTestBitmap(200, 200);
        recorder.CaptureFrame(testFrame, 100);
        recorder.CaptureFrame(testFrame, 100); // 重复帧
        recorder.CaptureFrame(testFrame, 100); // 重复帧
        recorder.CaptureFrame(CreateTestBitmap(200, 200), 100); // 不同帧

        // 生成 GIF
        var outputPath = Path.GetTempFileName() + ".gif";
        recorder.GenerateGif(outputPath);

        // 验证结果
        Assert.IsTrue(File.Exists(outputPath));

        // 清理
        File.Delete(outputPath);
    }

    [TestMethod]
    public void TestRealTimeProcessing()
    {
        var frames = new List<IFrame>();
        var testFrame = CreateTestBitmap(100, 100);

        // 第一帧
        var frame1 = CreateFrameInfo("frame1.png", 100);
        frames.Add(frame1);

        // 第二帧（相似）
        var frame2 = CreateFrameInfo("frame2.png", 100);
        LicecapOptimizedDuplicateRemover.ProcessFrameRealTime(
            frames, frame2, 90m, out bool shouldKeep2);

        // 第三帧（不同）
        var frame3 = CreateFrameInfo("frame3.png", 100);
        using var bitmap = CreateTestBitmap(100, 100);
        bitmap.SetPixel(0, 0, Color.Red); // 改变一个像素
        bitmap.Save(frame3.Path, ImageFormat.Png);
        LicecapOptimizedDuplicateRemover.ProcessFrameRealTime(
            frames, frame3, 90m, out bool shouldKeep3);

        Assert.IsFalse(shouldKeep2); // 应该被丢弃
        Assert.IsTrue(shouldKeep3);   // 应该被保留
    }
}
```

## licecap 项目集成总结

### 集成策略

1. **最小侵入式集成**：通过扩展现有的录制器类，不破坏原有功能
2. **可选功能**：用户可以选择启用或禁用重复帧删除
3. **实时处理**：支持录制时实时去重，减少内存使用
4. **后处理模式**：支持录制完成后批量去重

### 关键集成点

1. **帧捕获接口**：在 `CaptureFrame` 方法中集成去重逻辑
2. **GIF编码入口**：在 `GenerateGif` 方法前添加去重处理
3. **设置界面**：在 licecap 设置中添加去重选项
4. **状态显示**：在状态栏显示去重统计信息

### 性能考虑

1. **内存使用**：长时间录制时使用实时处理模式
2. **处理速度**：使用并行计算和 unsafe 代码优化
3. **用户体验**：提供进度反馈和取消选项

### 预期效果

- **文件大小减少**：根据内容不同，可减少 10-50% 的文件大小
- **录制效率**：实时模式下可减少存储需求
- **兼容性**：不影响现有的 GIF 格式和播放器兼容性

按照本指南，开发者可以在 licecap 项目中成功集成 ScreenToGif 的高效重复帧删除算法，提升录制效率和输出质量。