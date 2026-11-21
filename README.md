# ClipBank

> 检测项目的“备菜间”：把任意视频变成规格统一、可复现、可寻址的片段与清单。

Passerby 的独立副项目，只通过文件与命令行衔接。当前 **v0.1.0**。

```text
任意视频 ──▶ clipbank ──▶ 1080p30 CFR 片段 + manifest.json ──▶ 检测项目
```

## 快速开始

环境：Linux、C++20、CMake ≥ 3.23、Ninja、系统 `ffmpeg` / `ffprobe`（需要 `libx264`）。

```bash
cmake --preset release && cmake --build build/release -j

./build/release/clipbank init --library ./library
./build/release/clipbank add --library ./library /path/to/videos
./build/release/clipbank ls --library ./library
./build/release/clipbank verify --all --library ./library
./build/release/clipbank manifest --library ./library -o manifest.json
```

`--library` 默认取 `$CLIPBANK_LIBRARY`，否则是当前目录下的 `library/`。

## 命令

| 命令 | 作用 |
|---|---|
| `init` | 创建库目录与 SQLite schema（幂等） |
| `add <path…>` | 递归扫描；probe → normalize → segment → thumb → hash → meta；`--dry-run` 只探测 |
| `ls` | 列出 runs；`--state` / `--profile` 过滤，`--json` 机读 |
| `manifest` | 输出 schema v1；`-o` 写文件，`--run` 限定单个 run |
| `verify` | 校验规格、时长、帧数、关键帧、hash、`meta.json`；失败退出 6 |
| `retry <run_id>` | 从最后完成阶段续跑；`--all-failed` 重试所有非 done 状态 |

常用：`add --dry-run`、`add --no-hash`（verify 需 `--no-hash`）、`add --force-reencode`、
`verify --run <id> --json`、`retry --all-failed`。

## profile `detect`

```json
{"width":1920,"height":1080,"fps_num":30,"fps_den":1,"codec":"h264",
 "crf":20,"preset":"veryfast","pix_fmt":"yuv420p","segment_seconds":60,"audio":false}
```

等比缩放 + 黑边到 1920×1080；CFR 30fps；无音轨/字幕；清元数据；`+faststart`；
每 60 秒强制关键帧，切片 `-c copy`。

## 目录布局

```text
library/
├── clipbank.db                 # SQLite（WAL），只是索引
├── <run_id>/
│   ├── normalized.mp4
│   ├── clips/0000.mp4 …
│   ├── thumb.jpg
│   ├── meta.json
│   └── run.log
├── live/
└── .tmp/<run_id>/
```

## 与检测项目 CLI 的契约

```bash
./passerby <Model> <Source> [--no-show] [--dump <CSV>]
```

检测项目只有两个位置参数，不解析 manifest。ClipBank 直接把 clip 路径传进去：

```bash
./passerby models/yolov8n.onnx \
  library/<run_id>/clips/0000.mp4 --no-show --dump <clip_id>.csv
```

CSV：`id,x,y,w,h,score`；`id` 是 0 基帧号；空帧写 `id,0,0,0,0,0.000`
（`score=0` 表示无检测）；绝对时间：

```text
abs_ms = clip.start_ms + frame_idx * 1000 / 30
```

检测项目实测退出码：0 成功；1 参数；4 帧循环异常；**255** 源/模型打开失败
（`run()` 返回 -1）。ClipBank 只依赖 0 表示成功。

## ClipBank 退出码

| 码 | 含义 |
|---|---|
| 0 | 成功 |
| 2 | 用法 / 配置（未初始化库、未知 run、toolchain 变化需 `--force-reencode`） |
| 3 | 依赖缺失（ffmpeg / ffprobe / libx264） |
| 4 | 输入 / 解码 |
| 5 | 内部 / SQLite |
| 6 | `verify` 失败 |

## Manifest schema v1

`clipbank manifest -o manifest.json` 只导出状态为 `done` 的 run；`--run <id>` 可限定单个。

- 顶层：`schema`、`generated_at`、`profile`、`profile_fp`、`profile_params`、
  `toolchain`、`library`、`sources[]`、`clips[]`
- `sources[]`：`source_id`、`path`、`sha256`、`width`、`height`、`fps`、`duration_ms`、
  `vfr`、`vfr_ratio`、`hdr`、`rotation`
- `clips[]`：`clip_id`、`run_id`、`source_id`、`path`、`rel_path`、`start_ms`、`end_ms`、
  `frames`、`fps`、`width`、`height`、`sha256`
- `path` 是绝对路径，`rel_path` 相对 `library`；`--no-hash` 时 `sha256` 为 `null`
- 第一段 `start_ms=0`，后一段 `start_ms` 等于前一段 `end_ms`；最后一段可短

ID 规则：

```text
source_id = sha256(file)[:12]
profile_fp = sha256(profile_json)[:8]
run_id     = sha256(source_id + "|" + profile_fp)[:12]
clip_id    = run_id + "-" + 3 位 index        # 例 c43b2f4e4c83-000
```

## 行为

- 同内容 + 同 profile 重复 `add` 跳过。
- normalize / segment / thumb / meta 都是 tmp + rename；任一阶段被杀，`add` 或
  `retry` 从最后完成阶段继续。
- ffmpeg 版本 + `libx264` 参与 `toolchain_fp`；升级后已有派生文件需 `--force-reencode`。
- 单进程串行；只有 `detect` profile；不做 HDR tonemap。

## 构建与测试

```bash
cmake --preset debug   && cmake --build build/debug -j   && ctest --test-dir build/debug --output-on-failure
cmake --preset release && cmake --build build/release -j && ctest --test-dir build/release --output-on-failure
cmake --preset asan    && cmake --build build/asan -j    && ctest --test-dir build/asan --output-on-failure
```

集成测试用 lavfi 合成视频，不提交大媒体。

## 依赖

Vendored 在 `third_party/`：nlohmann/json 3.11.3（MIT）、CLI11 2.6.1（BSD-3）、
doctest 2.4.12（MIT）、SQLite 3.50.4 amalgamation（public domain）；许可证在
`third_party/licenses/`。SHA-256 为内置实现；ffmpeg/ffprobe 是系统二进制，
通过 `fork/execvp + pipe` 调用，不链 libav；缺 `libx264` 退出 3。

## License

MIT，见 [LICENSE](LICENSE)。
