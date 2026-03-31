# OpenClaw 配置说明

## 服务器信息

| 项目 | 值 |
|------|----|
| 服务器 IP | 8.133.3.7 |
| SSH 用户 | root |
| 运行用户 | admin |
| 运行方式 | 进程（非 Docker） |
| 网关端口 | 14072 |

---

## 目录结构

```
/home/admin/.openclaw/
├── openclaw.json          # 主配置文件（模型、渠道、插件等）
├── workspace/
│   └── SOUL.md            # ★ 角色设定文件（最常编辑）
├── agents/
│   └── main/sessions/     # 对话历史
├── memory/                # 持久记忆（自动管理）
├── extensions/            # 插件目录（qqbot、wechat、wecom 等）
└── qqbot/                 # QQ Bot 专属数据
```

---

## 角色配置：SOUL.md

SOUL.md 是 OpenClaw 的核心角色文件，每次会话启动时通过 `boot-md` hook 自动注入为系统提示。

**文件路径：**
```
/home/admin/.openclaw/workspace/SOUL.md
```

**本项目对应的本地源文件：**
```
openclaw-data/child_profile.md
```

### 当前角色设定

```markdown
## 角色设定

**你的名字是「书童」**，不是其他名字。你是叶欣羽的专属 AI 学习伙伴。

**关于欣羽：**
- 姓名：叶欣羽，10岁，小学二年级
- 主要科目：语文、数学、英语

**说话规则（必须遵守）：**
1. 始终自称「书童」，不使用其他名字
2. 用「同学」或「欣羽」称呼她
3. 每次回答严格不超过3句话
4. 语言简单活泼，多鼓励，不批评

**禁止内容：** 暴力、恐怖、成人内容、政治话题

**每日定时提醒：**
- 18:00 → 提醒完成作业
- 20:30 → 提醒准备睡觉
```

### 修改角色的步骤

1. 编辑本地文件 `openclaw-data/child_profile.md`
2. SSH 登录服务器，更新 SOUL.md：

```bash
ssh root@8.133.3.7
# 编辑角色文件
nano /home/admin/.openclaw/workspace/SOUL.md
```

> **无需重启服务**，下一次对话会话启动时自动生效。

---

## 主配置文件：openclaw.json

路径：`/home/admin/.openclaw/openclaw.json`

### 已配置的模型

| 别名 | 提供商 | 模型 ID |
|------|--------|---------|
| minimax-m2.5 | MiniMax | MiniMax-M2.5 |
| minimax-m2.7 | MiniMax | MiniMax-M2.7 |
| qwen3-max | DashScope | qwen3-max-2026-01-23 |
| qwen3.5-plus | DashScope | qwen3.5-plus |

**默认对话模型：** `minimax/MiniMax-M2.5`

### 修改默认模型

编辑 `openclaw.json` 中的以下字段：

```json
"agents": {
  "defaults": {
    "model": {
      "primary": "minimax/MiniMax-M2.5"   ← 改这里
    }
  }
}
```

---

## 已启用的渠道（插件）

| 插件 | 渠道 | 状态 |
|------|------|------|
| qqbot | QQ 机器人 | ✅ 已启用 |
| wechat | 微信 | ✅ 已启用 |
| wecom | 企业微信 | ✅ 已启用 |
| dingtalk | 钉钉 | ✅ 已启用 |
| dashscope-cfg | DashScope 配置 | ✅ 已启用 |

---

## API 接口

OpenClaw 网关暴露兼容 OpenAI 格式的 HTTP 接口，可用于调试和测试。

### 认证

所有请求需携带 Bearer Token：

```
Authorization: Bearer 7af105c86a3d295a6d2a6923f0c136681a7e58d204df52da35dad1baf9d54de8
```

### Chat Completions

```bash
curl -X POST http://8.133.3.7:14072/v1/chat/completions \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer 7af105c86a3d295a6d2a6923f0c136681a7e58d204df52da35dad1baf9d54de8" \
  -d '{
    "model": "minimax/MiniMax-M2.5",
    "messages": [
      {"role": "system", "content": "你是书童，叶欣羽的学习伙伴。"},
      {"role": "user", "content": "你好，你是谁？"}
    ],
    "max_tokens": 200
  }'
```

> **注意：** 直接调用 `/v1/chat/completions` 是透传接口，不会自动加载 SOUL.md。
> 如需测试角色，请在请求中手动添加 `system` 消息。
> 通过 QQ / 微信等渠道发起的对话会自动加载 SOUL.md。

---

## 服务管理

### 查看运行状态

```bash
ps aux | grep openclaw
```

### 查看端口监听

```bash
ss -tlnp | grep 14072
```

### 重启服务

```bash
# 以 admin 用户重启（openclaw 配置了 reload.mode = restart）
su - admin
cd /opt/openclaw
node dist/index.js gateway --bind lan --port 14072 &
```

### 查看日志

```bash
ls /home/admin/.openclaw/logs/
```

---

## Web 管理界面

OpenClaw 提供 Web UI（需要浏览器访问）：

```
http://8.133.3.7:14072/2b8b52db
```

> 认证 Token 同上。

---

## 常见问题

### Q：修改 SOUL.md 后没有生效？

SOUL.md 在**每次新会话启动**时加载，已建立的会话不会刷新。
解决方式：在 QQ 或微信中发送 `/reset` 或重新开启对话窗口。

### Q：如何添加新的 AI 模型？

编辑 `/home/admin/.openclaw/openclaw.json`，在 `models.providers` 下添加新的提供商配置，格式参考已有的 `dashscope` 或 `minimax` 节点。

### Q：如何修改孩子信息（姓名、年级等）？

1. 修改本地 `openclaw-data/child_profile.md`
2. 将内容同步到服务器 `/home/admin/.openclaw/workspace/SOUL.md`
3. 无需重启，下次对话自动生效
