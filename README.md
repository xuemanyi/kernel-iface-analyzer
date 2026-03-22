
---

# 📘 Kernel Interface Analyzer

## 1. 项目简介

该工具用于分析 Linux 内核子系统的函数接口关系，基于 **Clang LibTooling AST** 提取：

* 函数定义 / 声明
* 调用关系（call graph）
* EXPORT_SYMBOL 接口
* internal / external interface 分类

工具链由两部分组成：

### 🔧 C++ AST 工具

* `tu_indexer`
* `function_extractor`
* `call_extractor`

### 🐍 Python Pipeline

* `run_pipeline.py`
* `export_scanner.py`
* `merge.py`
* `classify.py`

---

## 2. 环境要求

### ✅ 必须

* Linux
* `cmake`, `make`
* `g++` 或 `clang++`
* `python3`
* LLVM + Clang **开发库（非常关键）**
* Linux kernel 源码
* `compile_commands.json`

### ⚠️ LLVM / Clang（关键）

必须安装 **完整开发环境**，否则会出现：

```text
libclangBasic.a not found
```

### 推荐安装方式（已验证可修复问题）

```bash
sudo apt update
sudo apt install llvm-18-dev clang-18 libclang-18-dev clang-tools-18
```

安装后应存在：

```bash
/usr/lib/llvm-18/lib/libclangBasic.a
```

---

### 👍 建议

* `ninja`
* `bear`（生成 compile_commands.json）
* `jq`

---

## 3. LLVM / Clang 配置

必须安装 **开发包**，并确保存在：

```
/usr/lib/llvm-18/lib/cmake/llvm
/usr/lib/llvm-18/lib/cmake/clang
```

或：

```
/usr/lib/llvm-16/lib/cmake/llvm
/usr/lib/llvm-16/lib/cmake/clang
```

---

## 4. 目录结构（推荐）

```
/home/user/
├── linux-6.1.y/
│   └── output-clang/
│       └── compile_commands.json
└── kernel-interface-analyzer/
    ├── src/
    ├── include/
    ├── scripts/
    └── build/
```

---

## 5. 必备输入

### ① 内核源码

```
/home/user/linux-6.1.y
```

### ② 编译数据库

```
/home/user/linux-6.1.y/output-clang/compile_commands.json
```

⚠️ 必须满足：

* 路径真实存在
* 与当前源码匹配

---

## 6. 编译工具

```bash
cd kernel-interface-analyzer
mkdir -p build
cd build

cmake .. \
  -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm \
  -DClang_DIR=/usr/lib/llvm-18/lib/cmake/clang

make -j$(nproc)
```

生成：

```
build/
├── tu_indexer
├── function_extractor
└── call_extractor
```

---

## 7. 运行 Pipeline

```bash
python3 scripts/run_pipeline.py \
  --source-root /home/gs/linux-6.1.y \
  --build-dir /home/gs/linux-6.1.y/output-clang \
  --tool-build-dir /home/gs/kernel-iface-analyzer/build \
  --subsystem init \
  --out-dir /home/gs/kernel-iface-analyzer/analysis-init
```

---

## 8. 参数说明

| 参数                 | 说明                           |
| ------------------ | ---------------------------- |
| `--source-root`    | 内核源码目录                       |
| `--build-dir`      | compile_commands.json 所在目录   |
| `--tool-build-dir` | C++ 工具路径                     |
| `--subsystem`      | 子系统（init / fs / mm / kernel） |
| `--out-dir`        | 输出目录                         |

---

## 9. 输出结果说明

输出目录示例：

```
analysis-init/
```

### 核心文件

| 文件                         | 作用                |
| -------------------------- | ----------------- |
| `tu_list.json`             | 命中的 TU 列表         |
| `functions_raw/`           | 原始函数信息            |
| `calls_raw/`               | 原始调用关系            |
| `exports.json`             | EXPORT_SYMBOL     |
| `merged_call_graph.json`   | 合并后的调用图（核心）       |
| `internal_by_file.json`    | internal 接口       |
| `external_interfaces.json` | ⭐ external 接口（重点） |
| `unresolved_calls.json`    | 未解析调用             |
| `stats.json`               | 统计信息              |
| `failed_tus.json`          | 失败 TU             |

---
