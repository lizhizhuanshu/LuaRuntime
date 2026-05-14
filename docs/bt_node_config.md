# 行为树节点 JSON 配置参考

## 顶层结构

JSON 必须包含 `"root"` 字段，所有节点必须包含 `"type"` 字段。

```json
{
  "root": {
    "type": "节点类型",
    "name": "可选名称",
    "children": [...],
    "decorators": [...]
  }
}
```

---

## 1. 复合节点 (Composite)

有子节点，通过 `"children"` 数组配置。

### Selector - 选择节点

依次执行子节点，任一成功即返回成功（OR 逻辑）。

```json
{
  "root": {
    "type": "Selector",
    "name": "可选名称",
    "children": [...]
  }
}
```

**行为：** 从左到右依次 tick 子节点，第一个返回 Success 的子节点即返回 Success；全部 Failure 才返回 Failure。Running 时记住当前位置，下次从该子节点继续。

### Sequence - 顺序节点

依次执行子节点，全部成功才返回成功（AND 逻辑）。

```json
{
  "root": {
    "type": "Sequence",
    "name": "可选名称",
    "children": [...]
  }
}
```

**行为：** 从左到右依次 tick 子节点，第一个返回 Failure 的子节点即返回 Failure；全部 Success 才返回 Success。Running 时记住当前位置。

### Parallel - 并行节点

同时执行所有子节点，通过策略控制成功/失败判定。

```json
{
  "root": {
    "type": "Parallel",
    "name": "可选名称",
    "success_policy": "RequireAll",
    "failure_policy": "RequireOne",
    "children": [...]
  }
}
```

| 字段 | 可选值 | 默认值 | 说明 |
|------|--------|--------|------|
| `success_policy` | `"RequireAll"` / `"RequireOne"` | `"RequireAll"` | 全部成功才算成功 / 任一成功即成功 |
| `failure_policy` | `"RequireAll"` / `"RequireOne"` | `"RequireOne"` | 全部失败才算失败 / 任一失败即失败 |

---

## 2. 叶子节点 (Leaf)

无子节点，执行具体逻辑。

### Script - 脚本节点

执行 Lua 脚本文件。

```json
{
  "type": "Script",
  "path": "scripts/attack.lua",
  "name": "可选名称（默认为 path）"
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `path` | **是** | Lua 脚本文件路径 |
| `name` | 否 | 节点名称，默认等于 path |

Script 节点支持 Lua 协程（yield），可在脚本内使用 `coroutine.yield()` 等待。

---

## 3. 装饰器 (Decorators)

附加在任意节点上，通过 `"decorators"` 数组配置。装饰器在节点 tick 前评估条件。

```json
{
  "type": "Script",
  "path": "a.lua",
  "decorators": [
    { "type": "BlackboardCondition", ... },
    { "type": "Inverter" }
  ]
}
```

### BlackboardCondition - 黑板条件

根据黑板数据决定节点是否可执行。

```json
{
  "type": "BlackboardCondition",
  "key": "hp",
  "operator": "greater_than",
  "value": 50,
  "abort": "Self"
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `key` | **是** | 黑板键名 |
| `operator` | 否 | 比较运算符，默认 `"is_set"` |
| `value` | 否 | 期望值（部分运算符需要） |
| `abort` | 否 | 中断模式，默认 `"None"` |

**operator 可选值：**

| 值 | 说明 | 需要 value |
|---|------|-----------|
| `"is_set"` | 键存在即通过 | 否 |
| `"is_not_set"` | 键不存在即通过 | 否 |
| `"equals"` | 值相等 | 是 |
| `"not_equals"` | 值不相等 | 是 |
| `"greater_than"` | 值大于 | 是 |
| `"less_than"` | 值小于 | 是 |

**value 支持类型：** `bool`、`int`（int64）、`double`、`string`。类型不匹配时条件为 false。

**abort 可选值（UE4/5 风格观察者中止）：**

| 值 | 说明 |
|---|------|
| `"None"` | 不中断（默认） |
| `"Self"` | 条件变化时中断自身正在执行的子树 |
| `"LowerPriority"` | 条件变化时中断右侧低优先级节点 |
| `"Both"` | 同时中断自身和低优先级 |

### Inverter - 取反装饰器

反转被装饰节点的成功/失败结果。

```json
{"type": "Inverter", "abort": "None"}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `abort` | 否 | 中断模式，默认 `"None"` |

### ForceSuccess - 强制成功装饰器

无论被装饰节点结果如何，始终返回成功。

```json
{"type": "ForceSuccess"}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `abort` | 否 | 中断模式，默认 `"None"` |

---

## 4. 完整示例

```json
{
  "root": {
    "type": "Selector",
    "name": "ai_root",
    "decorators": [
      {
        "type": "BlackboardCondition",
        "key": "alive",
        "operator": "is_set",
        "abort": "Self"
      }
    ],
    "children": [
      {
        "type": "Sequence",
        "name": "combat",
        "decorators": [
          {
            "type": "BlackboardCondition",
            "key": "has_target",
            "operator": "is_set"
          }
        ],
        "children": [
          {"type": "Script", "path": "scripts/aim.lua", "name": "aim"},
          {"type": "Script", "path": "scripts/attack.lua", "name": "attack"}
        ]
      },
      {
        "type": "Sequence",
        "name": "patrol",
        "children": [
          {"type": "Script", "path": "scripts/find_point.lua"},
          {"type": "Script", "path": "scripts/move_to.lua"}
        ]
      },
      {
        "type": "Script",
        "path": "scripts/idle.lua",
        "decorators": [
          {"type": "ForceSuccess"}
        ]
      }
    ]
  }
}
```

---

## 5. 节点类型汇总

| 类型 | 分类 | 必填字段 | 子节点 | 说明 |
|------|------|---------|--------|------|
| `Selector` | 复合 | `type` | `children` | OR 逻辑，任一成功即成功 |
| `Sequence` | 复合 | `type` | `children` | AND 逻辑，全部成功才成功 |
| `Parallel` | 复合 | `type` | `children` | 并行执行，策略控制结果 |
| `Script` | 叶子 | `type`, `path` | 无 | 执行 Lua 脚本 |
| `BlackboardCondition` | 装饰器 | `type`, `key` | N/A | 黑板条件判断 |
| `Inverter` | 装饰器 | `type` | N/A | 反转结果 |
| `ForceSuccess` | 装饰器 | `type` | N/A | 强制成功 |
