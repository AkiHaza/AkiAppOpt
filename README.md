# AppOpt

[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](LICENSE)

一个使用 C 语言编写的 Android 应用线程 CPU 亲和性优化工具，通过 **cpuset** 机制将指定应用的线程绑定到预设的 CPU 核心簇，实现精细化调度控制，兼顾性能与功耗。

---

## 特性

| 特性 | 说明 |
|------|------|
| **自定义规则** | 支持传统单行及块语法、线程通配符，按具体度与配置顺序选择命中规则 |
| **语义核心** | 自动检测 CPU 性能簇，支持 `e-core` / `p-core` / `hp-core` / `all-core` |
| **热加载** | 基于 inotify 实时监控配置文件变更，无需重启服务 |
| **低开销** | 包名与应用族索引 + 规则候选分组 + 线程匹配提前结束，减少无关规则遍历与重复 I/O |
| **自动负载分配** | 未命中规则的 Android 应用按线程 CPU 负载分配到 e-core / p-core / hp-core，显式规则优先 |
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
  -o module/bin/arm64-v8a/AppOpt module/AppOpt.c module/load_balancer.c

# armabi-v7a
armv7a-linux-androideabi21-clang -O2 -s -static \
  -o module/bin/armabi-v7a/AppOpt module/AppOpt.c module/load_balancer.c

# x86_64
x86_64-linux-android21-clang -O2 -s -static \
  -o module/bin/x86_64/AppOpt module/AppOpt.c module/load_balancer.c
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
  -g <路径>    指定游戏包名列表（默认: ./gamelist.conf）
  -s <秒>      设置检查间隔，必须 ≥ 1（默认: 2）
  -b <名称>    指定 /dev/cpuset 下的目录名（默认: AkiAppOpt）
  -v           显示版本信息
  -h           显示帮助

示例:
  AppOpt -c /data/applist.conf -g /data/gamelist.conf -s 3
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

显式规则按 `-s` 设置的间隔扫描新进程和新线程，默认 2 秒；不依赖系统总线程数增加。配置无效时会输出文件路径、行号及原因，最多显示前 20 条详细错误；超过长度限制的行整行跳过。

模块服务在模块目录的 `AppOpt.log` 中保留本次启动的前 64 KiB 输出，并记录启动失败；下一次启动新实例时重置日志。

**匹配与优先级：**

- 线程级规则优先于进程级规则
- 模式具体度依次为：精确名称、字符组 `[...]`、单字符 `?`、星号 `*`；同类模式中非通配字符越多越优先
- 多条线程规则命中时先比较线程模式具体度，再比较包名模式具体度
- 多条进程规则命中时选择包名模式最具体的一条；同等具体度保持配置文件中靠前规则优先
- `cpuset` 不可用时仍会通过 `sched_setaffinity()` 应用规则

### 自动负载分配

未被 `applist.conf` 规则命中的 Android 应用会由独立的 `load_balancer` 模块每秒采样一次线程 CPU 时间增量。采样结果经过平滑后按进程内负载排序，避免短时尖峰导致线程频繁跨簇迁移：

- 普通应用：有负载的前两个线程优先使用 `p-core`；第三名负载不低于第二名一半时也使用 `p-core`；其余线程使用 `e-core + p-core`
- 游戏进程：有负载的第一名线程使用 `hp-core`，第二名使用 `p-core`，其余线程使用 `e-core + p-core`

游戏包名通过 `gamelist.conf` 配置，每行一个包名，支持 `fnmatch` 通配符以及以 `#`、`//` 开头的整行注释：

```text
com.example.game
com.tencent.*
```

精确主包名同时匹配它的 `:子进程`。文件每秒检查一次修改时间，保存后无需重启服务即可生效。

核心簇优先按内核导出的 `cpu_capacity` 分组，缺失时回退到 `cpuinfo_max_freq`。例如 2+3+2+1 四簇结构会映射为最低簇 `e-core`、两个中间簇 `p-core`、最高簇 `hp-core`；双簇设备没有独立 `p-core` 时，自动策略会将性能线程回退到大核。模块只通过 `sched_setaffinity()` 限定允许的核心簇，支持 EAS 的内核仍会结合 PELT 利用率、CPU 剩余容量和 Energy Model 在掩码内选择具体 CPU；Android cpuset 限制仍由内核强制执行。

`gamelist.conf` 未命中的进程仍会根据包名和线程名中的 `game`、`unity`、`unreal`、`ue4`、`ue5`、`cocos`、`godot` 等特征进行兼容识别。只要主包或任一 `:子进程` 存在显式规则，整个应用族都会跳过自动负载分配。

参考：[Linux EAS](https://docs.kernel.org/scheduler/sched-energy.html)、[Capacity Aware Scheduling](https://docs.kernel.org/scheduler/sched-capacity.html)。

---

## 模块结构

刷入后 Magisk 模块目录：

```
/data/adb/modules/AkiAppOpt/
├── AppOpt              ← 当前设备架构的可执行文件
├── service.sh          ← 开机启动脚本
├── module.prop         ← 模块元信息
├── customize.sh        ← 刷入时安装脚本
├── applist.conf        ← 线程规则配置文件
└── gamelist.conf       ← 游戏包名配置文件
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

1. 服务启动时解析 `applist.conf` 和 `gamelist.conf`，在 `/dev/cpuset/AkiAppOpt/` 下创建对应 cpuset 分组
2. 周期性扫描 `/proc`，显式规则应用于命中进程，未规定应用按游戏列表和线程负载自动分簇
3. 将匹配线程的 PID 写入对应 cpuset 的 `tasks` 文件，同时调用 `sched_setaffinity()` 设置 CPU 亲和性
4. inotify 事件驱动配置文件热加载，自动回退到轮询模式作为兼容方案

---

## 文档

详细使用说明与配置指南：[http://appopt.suto.top](http://appopt.suto.top)

---

## 许可证

[GNU General Public License v3.0](LICENSE)
