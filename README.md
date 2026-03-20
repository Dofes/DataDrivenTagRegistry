# DataDrivenTagRegistry

一个用于 **LeviLamina** 的数据驱动标签注册模组。

它会在世界初始化完成后，扫描行为包中的 `data_driven_tags/*.json`，并将定义应用到：

- `item` 标签（物品）
- `block` 标签（方块）

## 功能

- 支持通过 JSON 声明标签成员
- 支持标签引用（`#other_tag`）
- 支持跨文件合并同名标签（同 `type` + 同 `identifier`）
- 自动去重成员
- 检测循环引用并记录警告

## JSON 格式

文件路径：`data_driven_tags/*.json`

```json
{
	"type": "item",
	"identifier": "my_tag",
	"items": [
		"minecraft:stone",
		"dirt",
		"#another_tag"
	]
}
```

字段说明：

- `type`: 当前支持 `item` / `block`
- `identifier`: 标签名（不带 `#`）
- `items`: 成员数组
	- 普通字符串：成员 ID
	- 以 `#` 开头：引用其他标签

说明：

- 成员若不带命名空间，会尝试补 `minecraft:` 进行兜底查找。
- 引用未定义标签、找不到的物品/方块、不支持的 `type` 都会输出警告日志。

## 构建

在仓库根目录执行：

1. `xmake f -y -p windows -a x64 -m release`
2. `xmake`

构建产物在 `bin/` 目录下。

## 兼容性

- LeviLamina 依赖版本见 `tooth.json`
- 默认目标平台为 `server`（可通过 `xmake.lua` 配置）

## 贡献

欢迎提 Issue / PR。

## License

MIT License © Dofes