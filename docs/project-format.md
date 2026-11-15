# 工程格式 v1

工程为 UTF-8 JSON，文件名建议使用 `.plcsim.json`。完整可用文件见 [hmi-demo.plcsim.json](../examples/hmi-demo.plcsim.json)。

```json
{
  "format": "FluentPLC-Simulator",
  "version": 1,
  "name": "S7 测试台",
  "server": {"bindAddress": "127.0.0.1", "port": 1102, "memorySize": 512},
  "tags": [
    {
      "name": "设定值",
      "address": "DB1.DBD0",
      "dataType": "REAL",
      "initialValue": 25,
      "mode": "manual",
      "minimum": 0,
      "maximum": 100,
      "periodSeconds": 10,
      "source": ""
    }
  ]
}
```

`format` 与 `version` 用来识别文件，不兼容的值会被拒绝。加载先构造并校验候选模型，成功后才替换原工程；失败应保留当前模型。保存先校验，失败不覆盖原文件。

| 服务字段 | 含义 |
|---|---|
| `name` | 项目名称 |
| `server.bindAddress` | 要监听的本机 IPv4 地址 |
| `server.port` | TCP 监听端口；常用 `1102` |
| `server.memorySize` | 每个已注册区域/DB 的字节数，范围 1–65535 |
| `tags` | 命名变量和轻量规则列表 |

## 地址与类型

| 区域 | BOOL | BYTE / SINT | WORD / INT | DWORD / DINT / REAL |
|---|---|---|---|---|
| 数据块 | `DB1.DBX0.0` | `DB1.DBB2` | `DB1.DBW4` | `DB1.DBD8` |
| 标志位 | `M0.0` | `MB2` | `MW4` | `MD8` |
| 输入映像 | `I0.0` | `IB2` | `IW4` | `ID8` |
| 输出映像 | `Q0.0` | `QB2` | `QW4` | `QD8` |

BOOL 位号为 0–7；DB 编号为 1–65535。地址大小写及数字前导零会归一化。每个变量的完整存储区间必须位于对应内存内，地址宽度必须与 `dataType` 相符。

| 类型 | 存储 | 取值 |
|---|---|---|
| BOOL | 1 位 | true / false |
| BYTE / SINT | 1 字节 | 0–255 / −128–127 |
| WORD / INT | 2 字节 | 0–65535 / −32768–32767 |
| DWORD / DINT | 4 字节 | 0–4294967295 / −2147483648–2147483647 |
| REAL | 4 字节 | 有限 IEEE-754 单精度浮点值 |

数值按 Siemens 大端编码；不能通过切换 Modbus 寄存器顺序解释。解析器也接受实现中列出的整数别名，但交换工程建议使用上表的标准名。当前不支持 STRING、WSTRING、64 位类型、定时器/计数器区域、符号地址或优化 DB。

重叠按位区间检查：`DB1.DBX0.0` 与 `DB1.DBX0.1` 可以同时存在；`DB1.DBB0` 与其中任何一位不能同时定义为两个变量。禁止重叠的是项目变量定义，并不限制客户端读写一个涵盖多个变量的连续内存块。

## 规则

| `mode` | 行为 |
|---|---|
| `manual` | 不产生周期性写入，保留当前值 |
| `sine` | 以 minimum/maximum 的中点为中心、半差为振幅的正弦信号 |
| `ramp` | 从 minimum 向 maximum 递增，到周期边界回到 minimum |
| `toggle` | 初始 BOOL 值在每个周期的后半段翻转 |
| `follow` | 复制 `source` 地址对应的当前值 |

`sine/ramp/toggle` 的 `periodSeconds` 必须是有效正数。`minimum/maximum` 必须适合目标类型与规则。规则时钟传入的是累计运行秒数，同一 tick 的跟随关系按依赖顺序计算。

所有模式均要求有限的 `minimum/maximum`、`minimum <= maximum` 以及有限的正周期；即使 manual/follow 不使用周期，也应保留有效配置。服务端口范围为 1–65535。工程文件最多 8 MiB，已注册区域总内存最多 32 MiB；DB 最多 2048 个。

`source` 使用已定义变量的地址，不是显示名称；源与目标的规范化类型必须相同。跟随源缺失、自引用及多个变量形成的循环都会被拒绝。该规则只复制值，不包含延时、锁存、条件表达式或梯形图扫描语义。

`initialValue` 只在应用工程时写入。运行值变化不会自动改写该字段；保存/重新加载工程将恢复配置的初始状态。服务停止/重启保留内存，和重新加载工程的行为不同。
