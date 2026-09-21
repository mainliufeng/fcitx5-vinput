# 安装与配置：二级精修版（流式预览 + 离线重解码）

本文档对应 `feat/issue-1-two-pass-refinement` 分支新增的**二级精修**能力。
面向 Arch Linux + KDE Wayland + fcitx5，无独显（纯 CPU）环境实测通过。

> **一句话说明它多做了什么**：上游本地 ASR 只能二选一——流式（响应快、准确率一般）
> 或离线（准确率高、松手才出字）。本分支把它变成**接力**：说话时用流式模型边说边出，
> 松手后用另一个更准的离线模型对**同一段音频**重解码一遍，用它的结果定稿。

---

## 0. 效果示意

同一段录音，两遍解码的对比：

![二级精修对比](images/03-two-pass.png)

第二级只多花 **150–250 ms**（SenseVoice int8 解 5 秒音频约 147 ms；本机实测的
X-ASR 离线模型同量级），用户基本无感。

---

## 1. 架构

```
        ┌─────────────────────────── fcitx5 ───────────────────────────┐
        │  addon: fcitx5-vinput.so（上游，未改动）                      │
        │  热键 Alt_R → 开始/停止录音 → 通过 D-Bus 调 daemon            │
        │  收到 partial → 渲染成 preedit；收到 final → commitString 上屏 │
        └───────────────────────────────┬──────────────────────────────┘
                                        │ D-Bus  org.fcitx.Vinput
        ┌───────────────────────────────▼──────────────────────────────┐
        │  vinput-daemon                                                │
        │   PipeWire 采集 → VAD 静音裁剪 →                              │
        │                                                               │
        │   ┌── 本分支新增：refining_backend ──────────────────────┐    │
        │   │  pass 1  流式模型（X-ASR 960ms）→ 边说边出            │    │
        │   │  pass 2  离线模型（X-ASR 离线）→ 整段重解码 → 定稿    │    │
        │   └──────────────────────────────────────────────────────┘    │
        │   → 可选 LLM 场景后处理 → 回传 addon                          │
        └───────────────────────────────────────────────────────────────┘
```

二级精修**完全实现在 daemon 的 ASR 层**，不改 addon、不改 D-Bus 接口、不改提交语义。
不配置 `refine_model` 时行为与上游完全一致。

---

## 2. 安装

### 方式 A：只装上游版（简单，但没有二级精修）

```bash
# archlinuxcn 仓库
sudo pacman -S fcitx5-vinput

# 或 AUR
yay -S fcitx5-vinput-bin
```

装完直接跳到第 3 节配置中的「最小配置」。**这种方式没有 `vinput refine` 命令。**

### 方式 B：本分支（含二级精修）

#### 2.1 前置依赖

```bash
# 构建工具
sudo pacman -S --needed base-devel cmake ninja clang gettext

# 运行时依赖（sherpa-onnx 在 archlinuxcn）
sudo pacman -S --needed fcitx5 pipewire qt6-base nlohmann-json \
     libarchive openssl curl sherpa-onnx
```

> 本机实测：`nlohmann_json` 与 `CLI11` 即使不装也会由 CMake `FetchContent` 自动拉取，
> 但装了更快、更适合离线构建。

#### 2.2 取代码并构建

```bash
git clone https://github.com/mainliufeng/fcitx5-vinput.git
cd fcitx5-vinput
git checkout feat/issue-1-two-pass-refinement

cmake --preset release-clang-mold \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=gold \
  -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=gold \
  -DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=gold
cmake --build --preset release-clang-mold -j$(nproc)
sudo cmake --install build
```

> **关于链接器**：上游预设用 `mold`。若未安装 `mold` 也没有 `lld`，用上面的
> `-fuse-ld=gold` 覆盖即可（`ld.gold` 随 binutils 自带）。

安装产物：

| 路径 | 内容 |
|---|---|
| `/usr/lib/fcitx5/fcitx5-vinput.so` | fcitx5 addon |
| `/usr/bin/vinput-daemon`、`/usr/bin/vinput`、`/usr/bin/vinput-gui` | 守护进程 / CLI / 图形配置 |
| `/usr/share/systemd/user/vinput-daemon.service` | 用户级 systemd 服务 |
| `/usr/share/fcitx5/addon/vinput.conf` | addon 注册（`Category=Module`，**不是输入法**） |

#### 2.3 启动

```bash
systemctl --user enable --now vinput-daemon.service
```

**必须重启 fcitx5**，否则运行中的进程不会加载新 addon（这一步很容易漏，见第 6 节坑 1）：

```bash
fcitx5 -r -d
# 或
pkill -x fcitx5; sleep 2; fcitx5 -d --replace
```

#### 2.4 验证安装

![安装与状态检查](images/02-install-status.png)

---

## 3. 配置

### 3.1 下载模型

```bash
vinput model list --available      # 看可下载列表
vinput model add onnx-xasr-zh-en-960ms-punct-stream   # pass 1：流式（127.7 MB）
vinput model add onnx-xasr-zh-en-punct-int8-off       # pass 2：离线双语（130.1 MB）
```

### 3.2 设置两个模型

```bash
vinput model use onnx-xasr-zh-en-960ms-punct-stream   # 主模型 = 流式（first pass）
vinput refine set onnx-xasr-zh-en-punct-int8-off      # 精修模型 = 离线（second pass）
```

> `refine set` 接受 `model list` 显示的短 ID，内部会解析成完整的 `model.<源>.<名>`；
> 模型管理器只认完整 ID，所以不要把短 ID 直接写进 `config.json`。

配置写入 `~/.config/vinput/config.json`：

```json
{
  "asr": {
    "active_provider": "sherpa-onnx",
    "providers": [
      {
        "id": "sherpa-onnx",
        "type": "local",
        "model": "model.sherpa-onnx.x-asr-960ms-streaming-zipformer-transducer-zh-en-punct-int8",
        "refine_model": "model.sherpa-onnx.x-asr-zipformer-transducer-zh-en-punct-int8",
        "timeout_ms": 15000
      }
    ]
  }
}
```

**不想要二级精修**就清空：

```bash
vinput refine clear      # 退回纯流式（更快，但松手后不再重解码）
```

### 3.3 选精修模型的硬性要求：**必须是双语离线模型**

这一条是踩过坑才明白的，请务必注意：

| 精修模型 | 结果 |
|---|---|
| **X-ASR 离线（zh-en，双语 BPE）** | ✅ 中文、英文、中英混说、标点、大小写全对 |
| ❌ SenseVoice（中文倾向） | ❌ **会破坏英文词**：`repo`→`RAIPPLE`、`prompt`→`PROMT`、`push`→`布置`、`流式`→`流逝`，且英文全变大写 |
| ❌ 另一个流式模型 | 代码会拒绝（`refine_model` 只接受 `sherpa-offline` 后端），并降级为纯流式 |

原因：SenseVoice 是中文/多语训练但**英文词汇量弱**，它会把流式已经认对的英文词"纠正"成
形状相似的其他 token。**二级精修的前提是第二级至少不能比第一级差。**

### 3.4 热词（提升英文术语 / 专有名词）

```bash
cat > ~/.config/vinput/hotwords.txt <<'EOF'
skill:5.0
repo:5.0
API:5.0
prompt:5.0
EOF
vinput hotword set ~/.config/vinput/hotwords.txt
```

一行一个词，可选 `:权重`（默认 4，范围 1–10）。
**热词只对流式那一级生效**（会强制切到 `modified_beam_search` 解码），
离线精修级不受影响。

> ⚠️ 热词是"偏置"不是"替换"：声学证据很强时压不过去（实测「子鸡→自己」加词也纠不回来）。
> 它擅长补**术语召回**，不擅长纠正**同音错字**。

### 3.5 音频与 VAD

```bash
vinput device list                 # 列可用输入设备
vinput device use default          # 或指定设备名
```

`config.json` 中相关项：

```json
"global": { "capture_device": "default", "duck_output_while_recording": false },
"asr": {
  "normalize_audio": true,     // 峰值归一化，麦克风音量低时很有用
  "input_gain": 1.0,           // 持续增益；实测录音峰值仅 0.06 时仍能识别
  "vad": {
    "enabled": true,           // 强烈建议开启：去首尾静音，也避免静音幻觉
    "threshold": 0.45,
    "min_speech_duration": 0.15,
    "min_silence_duration": 0.5,
    "speech_pad_ms": 300
  }
}
```

### 3.6 图形界面

![Vinput 配置界面](images/01-gui-control.png)

```bash
vinput-gui
```

> ⚠️ **当前 GUI 还没有"二级精修"这一项**——`refine_model` 只能用 CLI 配
> （`vinput refine set/get/clear`）。GUI 保存配置时不会覆盖它，但界面上看不到。
> 补 GUI 界面是后续工作。

---

## 4. 使用

vinput 是 fcitx5 的 **Module**，不是可选输入法——**不需要切换到它**。

| 按键 | 行为 |
|---|---|
| **右 Alt 短按** | 开始录音；再短按一次停止 |
| **右 Alt 长按** | 按住说话，松开即停（push-to-talk） |
| **右 Control** | 选中文本后按住，用语音指令改写选中内容 |
| **右 Shift** | 打开命令面板（`/model`、`/asr`、`/scene`） |

按键可在 fcitx5 配置界面中自定义。

**使用时必须有一个聚焦的文本输入框**（addon 是通过当前输入上下文上屏的）。

---

## 5. 性能与资源

本机实测（Intel i9-12900H，14 核 20 线程，**无独显**，Arch + KDE Wayland）：

| 配置 | 常驻内存 | 空闲 CPU | 松手到定稿 |
|---|---|---|---|
| 纯流式（`refine clear`） | ~300 MB | ≈0 | 无二次解码 |
| **流式 + X-ASR 离线精修（推荐）** | ~590 MB | ≈0 | +150–250 ms |
| 流式 + SenseVoice 精修（不推荐，伤英文） | ~640 MB | ≈0 | +150–250 ms |

流式首字延迟约 1.0–1.6 s（由模型前瞻决定，非算力限制）。

---

## 6. 故障排查（都是实际踩过的坑）

### 坑 1：按热键完全没反应

**最常见原因：装完 addon 后没有重启 fcitx5。**

`fcitx5 -r -d` 有时不会真正替换旧进程。先确认运行中的进程启动时间**晚于** addon 安装时间：

```bash
ps -o pid,lstart -p $(pgrep -x fcitx5)     # 启动时间必须晚于 addon 安装时间
grep 'Loaded addon vinput' <(journalctl --user -b 2>/dev/null)   # 或看 fcitx5 日志
```

注意 `fcitx5-diagnose` 显示的 "Vinput" 是**读磁盘上的 conf 文件**，不代表运行中的进程加载了它。

彻底重启：

```bash
pkill -x fcitx5; sleep 2; fcitx5 -d --replace
```

### 坑 2：英文技术词识别成乱码 / 被"改坏"

精修模型选错了。见 §3.3——**不要用 SenseVoice 做精修**，换成双语的离线 X-ASR。

### 坑 3：模型设置后没生效

```bash
vinput refine get        # 确认存的是完整 ID
journalctl --user -u vinput-daemon | grep 'second pass' | tail -3
```

若看到 `second pass disabled ...`，说明精修模型未找到或不是 `sherpa-offline`，
daemon 已自动降级为纯流式（不会报错给用户）。

### 坑 4：诊断两遍解码到底跑没跑

```bash
# 打开调试日志
mkdir -p ~/.config/systemd/user/vinput-daemon.service.d
printf '[Service]\nEnvironment=VINPUT_DEBUG=1\n' \
  > ~/.config/systemd/user/vinput-daemon.service.d/debug.conf
systemctl --user daemon-reload && systemctl --user restart vinput-daemon

# 说一句话后
journalctl --user -u vinput-daemon | grep -E 'pass 1|pass 2' | tail -4
```

会看到：

```
vinput:   pass 1 (streaming): 我平时会 skill 这个功能…repo 里边…
vinput:   pass 2 (refined):   我平时会 skill 这个功能…repo 里面…
```

验证完记得关掉（日志会很啰嗦）：删除该 drop-in 并 `systemctl --user daemon-reload`。

### 坑 5：daemon 在跑但热键没反应

检查 D-Bus 名字是否被占用：

```bash
busctl --user status org.fcitx.Vinput
```

---

## 7. 卸载

```bash
systemctl --user disable --now vinput-daemon.service
sudo rm -rf /usr/lib/fcitx5/fcitx5-vinput.so /usr/lib/fcitx5-vinput \
            /usr/bin/vinput /usr/bin/vinput-daemon /usr/bin/vinput-gui \
            /usr/share/fcitx5/addon/vinput.conf \
            /usr/share/systemd/user/vinput-daemon.service \
            /usr/share/dbus-1/services/org.fcitx.Vinput.service
rm -rf ~/.local/share/vinput ~/.config/vinput ~/.cache/vinput
fcitx5 -r -d
```

---

## 8. 上游与分支

- 上游项目：[xifan2333/fcitx5-vinput](https://github.com/xifan2333/fcitx5-vinput)（GPL-3.0）
- 本分支的改动：`feat(asr)` 二级精修，见 issue
  [mainliufeng/fcitx5-vinput#1](https://github.com/mainliufeng/fcitx5-vinput/issues/1)
- 参考：字节跳动的豆包语音识别在火山引擎 API 中同样提供
  `enable_nonstream`「二遍识别（流式上屏 + nostream 精修）」——本分支把同一思路做到了本地。
