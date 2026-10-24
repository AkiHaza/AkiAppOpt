# AppOpt

[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](LICENSE)

一个使用 C 语言编写的 Android 应用线程 CPU 亲和性优化工具，通过 **cpuset** 机制将指定应用的线程绑定到预设的 CPU 核心簇，实现精细化调度控制，兼顾性能与功耗。

---

## 特性

| 特性 | 说明 |
|------|------|
| **自定义规则** | 支持传统单行及块语法、线程通配符，按具体度与配置顺序选择命中规则 |
| **语义核心** | 自动检测 CPU 频率簇，支持 `e-core` / `p-core` / `hp-core` / `all-core` |
| **热加载** | 基于 inotify 实时监控配置文件变更，无需重启服务 |
| **低开销** | 包名哈希索引 + 规则候选分组 + 线程匹配提前结束，减少无关规则遍历与重复 I/O |
| **崩溃恢复** | 独立守护脚本监测主程序退出，按 5 / 10 / 30 / 60 秒退避重启 |
| **多架构** | 支持 arm64-v8a / armabi-v7a / x86_64 |
| **Magisk 模块** | 以 Magisk / KernelSU 模块形式分发，开机自启 |

---

## 编译

### 依赖

- Android NDK r27c 或更高版本

### 本地编译

```bash
# arm64-v8a
aarch64-linux-android21-clang -O2 -s -static \
  -o module/bin/arm64-v8a/AppOpt module/AppOpt.c

# armabi-v7a
armv7a-linux-androideabi21-clang -O2 -s -static \
  -o module/bin/armabi-v7a/AppOpt module/AppOpt.c

# x86_64
x86_64-linux-android21-clang -O2 -s -static \
  -o module/bin/x86_64/AppOpt module/AppOpt.c
```

以上命令只编译主程序。可刷入的 zip 还需要 `META-INF` 下的 Magisk 安装器；请使用仓库的 GitHub Actions 构建流程，它会生成安装器并打包。

> CI 构建由 GitHub Actions 自动完成，产物可从 Actions 下载；发布产物可从 [Releases](https://github.com/AkiHaza/AkiAppOpt/releases) 获取。

---

## 使用

### 命令行

```
AppOpt [选项]

选项:
  -c <路径>    指定配置文件路径（默认: ./applist.conf）
  -s <秒>      设置检查间隔，必须 ≥ 1（默认: 2）
  -b <名称>    指定 /dev/cpuset 下的目录名（默认: AkiAppOpt）
  -v           显示版本信息
  -h           显示帮助

示例:
  AppOpt -c /data/applist.conf -s 3
  AppOpt -b MyAppOpt
```

### 配置文件格式 (`applist.conf`)

```
# 以 # 或 // 开头的行为注释

# 进程级规则，可使用数字范围或语义核心
com.tencent.mm=e-core
com.example=all-core

# 传统线程规则仍然兼容
com.example{RenderThread}=hp-core
com.example{Worker-*}:100=0-3

# 块语法；首行也可写成 com.example=all-core {
com.example {
    RenderThread=hp-core
    Worker-*=e-core,4
    IOThread:100=0-3
}
```

**语法说明：**

| 语法元素 | 说明 |
|----------|------|
| `包名` | 从 `/proc/<pid>/cmdline` 提取的进程名，支持 `fnmatch` 通配符（`*` `?` `[...]`） |
| `{线程名}` | 可选，支持 `fnmatch` 通配符（`*` `?` `[...]`） |
| `:延迟单位` | 每单位为 100 ms；传统语法写在 `}` 后，块语法写在线程名后 |
| 数字范围 | 支持 `0-3`、`4,5,6`、`0-3,6-7` 等写法 |
| 语义核心 | `e-core` 为最低频率簇，`hp-core` 为最高频率簇，`p-core` 为中间频率簇，`all-core` 为全部可用核心 |

语义核心可与数字范围混合，例如 `e-core,4-5`。设备不存在对应频率层时，该语义核心为空；比如双簇 CPU 通常没有 `p-core`。行尾支持 `#` 或 `//` 注释。

延迟从应用被 AppOpt 识别时开始计算。不写延迟或使用 `:0` 时行为与原规则相同；配置热加载和进程缓存重扫不会重新计时。

显式规则按 `-s` 设置的间隔扫描新进程和新线程，默认 2 秒；不依赖系统总线程数增加。 只有进程级规则的应用仍扫描全部线程，但跳过线程名读取；有候选线程规则时才读取线程名并匹配。配置无效时会输出文件路径、行号及原因，最多显示前 20 条详细错误；超过长度限制的行整行跳过。

模块服务由后台守护脚本监测主程序，每次异常退出后按 5 / 10 / 30 / 60 秒退避重启，最长间隔为 60 秒；连续运行满 60 秒后重置退避。启动脚本交接后即返回，不阻塞 Root 管理器。检测到模块被禁用、移除或主程序缺失时停止恢复，并清理所管理的进程；其他手动实例已占用同一配置锁时退出守护。

`AppOpt.log` 保留本次启动的前 64 KiB 输出及启动、重试记录，`AppOpt.log.prev` 保留上一次启动的日志；单份日志不超过 64 KiB，超出部分持续丢弃以避免阻塞主程序。

服务使用内核文件锁避免并发启动，同一配置路径只允许一个主程序实例（重复启动返回 73）。PID 记录同时保存进程启动时间与本次开机 ID，停止旧实例前会重新校验身份及完整程序路径，避免 PID 复用误判。`.AppOpt.service.lock` 和 `applist.conf.lock` 为运行锁，运行时请勿删除。

**匹配与优先级：**

- 线程级规则优先于进程级规则
- 模式具体度依次为：精确名称、字符组 `[...]`、单字符 `?`、星号 `*`；同类模式中非通配字符越多越优先
- 多条线程规则命中时先比较线程模式具体度，再比较包名模式具体度
- 多条进程规则命中时选择包名模式最具体的一条；同等具体度保持配置文件中靠前规则优先
- `cpuset` 不可用时仍会通过 `sched_setaffinity()` 应用规则

---

## 模块结构

刷入后 Magisk 模块目录：

```
/data/adb/modules/AkiAppOpt/
├── AppOpt              ← 当前设备架构的可执行文件
├── service.sh          ← 开机启动脚本
├── module.prop         ← 模块元信息
├── customize.sh        ← 刷入时安装脚本
└── applist.conf        ← 默认配置文件
```

---

## 工作原理

```
┌─────────────┐     ┌──────────────┐     ┌─────────────────────┐
│ applist.conf │ ──▶ │ 规则解析器    │ ──▶ │ /dev/cpuset/AkiAppOpt/ │
└─────────────┘     └──────────────┘     │   ├── 4-6/           │
                                         │   ├── 7/             │
       ┌─────────────────────┐           │   └── 6-7/           │
       │  inotify 热加载      │◀──────────│   (cpuset 分组)      │
       └─────────────────────┘           └─────────┬───────────┘
                                                   │
┌──────────┐     ┌──────────────┐     ┌───────────▼───────────┐
│  /proc   │ ──▶ │ 进程/线程扫描 │ ──▶ │ sched_setaffinity()  │
│ 扫描     │     │ + PID 跟踪    │     │ + tasks 文件写入      │
└──────────┘     └──────────────┘     └───────────────────────┘
```

1. 服务启动时解析 `applist.conf`，在 `/dev/cpuset/AkiAppOpt/` 下创建对应 cpuset 分组
2. 周期性扫描 `/proc`，通过 PID 跟踪与增量扫描机制降低开销
3. 将匹配线程的 PID 写入对应 cpuset 的 `tasks` 文件，同时调用 `sched_setaffinity()` 设置 CPU 亲和性
4. inotify 事件驱动配置文件热加载，自动回退到轮询模式作为兼容方案

---

## 文档

详细使用说明与配置指南：[http://appopt.suto.top](http://appopt.suto.top)

---

## 许可证

[GNU General Public License v3.0](LICENSE)
