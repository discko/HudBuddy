# 中文字体

来源：Noto CJK 官方仓库 `notofonts/noto-cjk`，`Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf`。

许可：SIL Open Font License 1.1，完整文本见 `OFL.txt`。固件中的派生点阵字体由 `scripts/build-fonts.py` 生成，保留同一许可。

生成命令：`python3 scripts/build-fonts.py`（需要 Pillow）。输出为 `firmware/hud/HudFonts.cpp` 和 `HudFonts.h`。每种字号包含 28,447 个码位，覆盖 ASCII、Latin-1（含 ·、×）、常用标点、假名、CJK 扩展 A、CJK 统一汉字和全角字符。字号为 24 / 28 px，1bpp；未包含 Emoji 与补充平面汉字。

字体直接存储在 Flash 中，运行时按码位二分查找，不把整套字库载入内存。
