#!/bin/bash

# GitHub Actions 构建状态检查脚本
# 使用方法: ./check_build_status.sh

echo "🔍 检查 LICEcap GitHub Actions 构建状态"
echo "=========================================="

REPO="BlackStar1453/licecap"
REPO_URL="https://github.com/BlackStar1453/licecap"

# 检查最新提交
echo ""
echo "📋 最新提交信息:"
git log --oneline -1

echo ""
echo "🌐 远程仓库状态:"
echo "上游仓库: $REPO_URL"

# 检查是否有GitHub CLI
if command -v gh &> /dev/null; then
    echo ""
    echo "🔄 正在检查 GitHub Actions 状态..."

    # 尝试获取工作流
    WORKFLOWS=$(gh api repos/$REPO/actions/workflows 2>/dev/null || echo '{"total_count":0}')
    TOTAL=$(echo $WORKFLOWS | jq -r '.total_count' 2>/dev/null || echo "0")

    if [ "$TOTAL" -gt 0 ]; then
        echo "✅ 找到 $TOTAL 个工作流"

        # 获取最新运行
        echo ""
        echo "📊 最近的运行:"
        gh run list --repo $REPO --limit 5 --json status,conclusion,headBranch,createdAt,displayTitle 2>/dev/null | \
        jq -r '.[] | "\(.status | ascii_upcase) | \(.conclusion // "RUNNING") | \(.displayTitle) | \(.createdAt)"' 2>/dev/null || \
        echo "  无法获取运行详情"
    else
        echo "⏳ GitHub Actions 还未同步，请稍后再试"
    fi
else
    echo ""
    echo "❌ GitHub CLI 未安装"
    echo "安装方法: brew install gh"
fi

echo ""
echo "🔗 手动检查方法:"
echo "1. 访问: $REPO_URL"
echo "2. 点击 'Actions' 标签"
echo "3. 查找 'Build LICEcap' 工作流"
echo "4. 监控构建进度"

echo ""
echo "⏱️ 预计构建时间: 15-20分钟"
echo "📦 构建完成后可在 Actions 页面下载构建产物"

# 如果有jq，检查最新提交是否在主分支
if command -v jq &> /dev/null; then
    echo ""
    echo "📈 提交状态:"
    LATEST_COMMIT=$(git rev-parse HEAD)
    echo "最新提交: $LATEST_COMMIT"
fi

echo ""
echo "🎯 默认配置已更新:"
echo "   similarity_threshold: 0.90 (90%)"
echo "   这是更实用的默认值，避免丢失必要内容"