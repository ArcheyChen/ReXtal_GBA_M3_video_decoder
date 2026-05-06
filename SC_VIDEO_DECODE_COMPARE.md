# M3 GBA 解码器与 FilmPlay/SuperCard 视频解码实现对比

日期：2026-05-05

本文只记录静态对比结论，不包含代码修改建议的落地实现。对比对象：

- 我们当前仓库：`gba_decoder/source/gbm_decoder.c`、`gba_decoder/include/gbm_decoder.h`
- FilmPlay/SuperCard：`FilmPlay_fixed.gba`，Ghidra 中 ROM 地址 `0810xxxx` / `0812xxxx`，运行时复制到 IWRAM `0300xxxx`

## 结论摘要

行为层面：我们的视频解码树、bitstream 判定、copy/motion、delta、fill、最小块双颜色处理，与 FilmPlay/SuperCard 的视频解码器是同一套算法。没有看到“多做了边界检查导致语义不同”的问题。

性能层面：我们当前实现确实更慢，主要不是因为算法错误，而是实现形态差异：

1. 我们的 `gbm_decoder.c` 以 `-mthumb` 编译，FilmPlay 热路径是 ARM 代码。
2. FilmPlay 为每个块尺寸生成/保留专门函数，固定偏移 load/store 全展开；我们用 C 版通用 helper 处理 copy/fill/delta，并带 `rows/words` 参数和 `switch`。
3. FilmPlay 解码前会去掉 2 字节 `frame_len`，让 flag stream 从 `+4` 开始，32-bit 对齐；我们直接解完整帧记录，flag stream 在 `offset + 6`，所以 refill 用 `read_u32_unaligned()` 四个 byte load 拼 u32。
4. FilmPlay 把 bitstream 状态、flag 指针、payload 指针、palette 指针、dst/ref、codebook 基址长期放在寄存器里；我们用 `DecodeContext` 结构体，很多路径需要 load/store context 字段。
5. FilmPlay 的 copy/delta/fill 在各块函数里直接展开；我们在大量分支中调用或内联通用 helper，存在额外调用、参数传递、循环和分支。

因此，如果后续要追性能，最优先怀疑点是“Thumb + C 抽象 + 非对齐 flag refill”，而不是额外边界检查。

## FilmPlay 已识别视频函数

Ghidra 中已润色命名：

| FilmPlay ROM 地址 | 名称 | 对应尺寸/职责 |
|---|---|---|
| `08100FA8` | `movie_video_decode_frame_blocks_arm` | 设置 dst/ref 并进入 20x30 个 8x8 block 解码 |
| `08103A28` | `movie_bitstream_refill_arm` | 第一段 ARM bitstream refill |
| `08103A34` | `movie_video_decode_8x8_block_arm` | 8x8 block 顶层解码 |
| `0811FC28` | `movie_bitstream_refill_second_block_arm` | 第二段 ARM bitstream refill，反编译不完整但汇编可识别 |
| `0812143C` | `movie_video_decode_8x4_block_arm` | 8x4 block |
| `08120F04` | `movie_video_decode_4x8_block_arm` | 4x8 block |
| `081208E8` | `movie_video_decode_8x2_block_arm` | 8x2 block |
| `081205D0` | `movie_video_decode_2x8_block_arm` | 2x8 block |
| `08120BD0` | `movie_video_decode_4x4_block_arm` | 4x4 block |
| `08121F84` | `movie_video_decode_2x4_block_arm` | 2x4 block |
| `0812216C` | `movie_video_decode_4x2_block_arm` | 4x2 block |
| `081225A8` | `movie_video_decode_8x1_block_arm` | 8x1 block |
| `08122344` | `movie_video_decode_1x8_block_arm` | 1x8 block |
| `08121ACC` | `movie_video_decode_1x4_block_arm` | 1x4 block |
| `0812197C` | `movie_video_decode_2x2_block_arm` | 2x2 block |
| `08121C40` | `movie_video_decode_4x1_block_arm` | 4x1 block |
| `08121D90` | `movie_video_decode_1x2_block_arm` | 1x2 block |
| `08121E88` | `movie_video_decode_2x1_block_arm` | 2x1 leaf block |

运行时映射已用 dump 校验：

```text
03000000-03003DFF ~= ROM 08100200-08103FFF
03003E00-03007BFF ~= ROM 08120200-08123FFF
```

核心视频函数区域与 ROM 基本一致，可以直接分析 ROM 函数。

## 我们当前视频函数清单

来源：`gba_decoder/source/gbm_decoder.c`

| 我们的函数 | 行号 | 说明 |
|---|---:|---|
| `gbm_set_version` | 59 | 根据 GBM version 设置 frame header XOR key |
| `read_u32_unaligned` | 107 | 非对齐 u32 读取，byte 拼接 |
| `read_u16_unaligned` | 113 | 非对齐 u16 读取，byte 拼接 |
| `obfuscation_mix` / `frame_field_key` | 117 / 126 | 网页/多 bank 元数据额外混淆用，不属于 FilmPlay 热路径 |
| `next_bit` | 131 | 读取 1 bit |
| `next_2bits` | 145 | 读取 2 bit |
| `read_palette_color` | 175 | 读取 palette/delta 颜色 |
| `read_code` | 185 | 读取 motion/codebook byte |
| `copy_u32_block` | 196 | 通用 u32 copy helper |
| `fill_u32_block` | 272 | 通用 u32 fill helper |
| `delta_u32_block` | 310 | 通用 u32 delta helper |
| `copy_u16_block` | 329 | 通用 u16 copy helper |
| `fill_u16_block` | 342 | 通用 u16 fill helper |
| `delta_u16_block` | 353 | 通用 u16 delta helper |
| `decode_block_8x8` | 391 | 8x8 block |
| `decode_block_8x4` | 424 | 8x4 block |
| `decode_block_4x8` | 461 | 4x8 block |
| `decode_block_2x8` | 499 | 2x8 block |
| `decode_block_1x8` | 537 | 1x8 block |
| `decode_block_4x4` | 570 | 4x4 block |
| `decode_block_8x2` | 608 | 8x2 block |
| `decode_block_2x4` | 645 | 2x4 block |
| `decode_block_4x2` | 683 | 4x2 block |
| `decode_block_8x1` | 721 | 8x1 block |
| `decode_block_1x4` | 753 | 1x4 block |
| `decode_block_2x2` | 786 | 2x2 block |
| `decode_block_4x1` | 824 | 4x1 block |
| `decode_block_1x2` | 855 | 1x2 block |
| `decode_block_2x1` | 891 | 2x1 block |
| `gbm_decode_frame_internal` | 931 | 帧头解析、初始化 context、20x30 block 循环 |
| `gbm_decode_frame` | 979 | 普通入口 |
| `gbm_decode_frame_obfuscated` | 983 | 额外混淆入口 |

## 逐项对比

### 帧入口：`gbm_decode_frame_internal` vs `movie_video_decode_frame_blocks_arm`

行为：基本一致，都是 20 行 x 30 列个 8x8 block，当前帧/参考帧双缓冲。

差异：

- FilmPlay 的视频读帧层 `FUN_08100FD0` 会把当前帧记录的 2 字节长度去掉，复制成从 `02025800` 开始的解码输入。也就是说解码器看到的数据起点是 `bit_enc`，不是 `frame_len`。
- 我们的 `gbm_decode_frame_internal` 直接从完整记录读：

```c
frame_len     = read_u16_unaligned(data + offset);
bit_enc       = read_u16_unaligned(data + offset + 2);
palette_bytes = read_u16_unaligned(data + offset + 4);
flag_ptr      = data + offset + 6;
```

- FilmPlay 解码输入是：

```text
02025800 + 0: bit_enc
02025800 + 2: palette_bytes
02025800 + 4: flag stream
```

性能影响：

- FilmPlay flag stream 从 `+4` 开始，32-bit 对齐，可以直接 `ldr r6, [r3], #4`。
- 我们 flag stream 从完整帧记录的 `+6` 开始，通常非 4 字节对齐，必须 byte 拼接 `u32`。
- 这是热路径 refill 的明确性能差异。

### Bitstream：`next_bit` / `next_2bits` vs `movie_bitstream_refill_*`

行为：一致，都是 32-bit shift register + sentinel/refill。

FilmPlay 汇编特征：

```asm
adds r6, r6, r6
bleq refill
bcs  ...
```

它直接利用 ARM carry flag 做 bit 判定，refill 近似：

```asm
ldr  r6, [r3], #4
adcs r6, r6, r6
```

我们的实现：

```c
if (ctx->state == (1u << 31)) {
    u32 word = read_u32_unaligned(ctx->flag_ptr);
    ctx->flag_ptr += 4;
    ...
}
```

性能影响：

- FilmPlay 的 bit 判定基本是寄存器 shift + 条件跳转。
- 我们的 C 实现会读写 `ctx->state` 和 `ctx->flag_ptr`，并且 refill 用 byte 拼接。
- `next_2bits` 减少了一些调用次数，但仍比 FilmPlay 的 ARM carry 写法重。

### Codebook offsets

行为：一致。

我们 `CODEBOOK_OFFSETS` 表与 FilmPlay 用法匹配：code byte 作为索引，取有符号 offset，加到当前块位置的参考帧地址。

性能差异：

- FilmPlay 常驻寄存器保存 codebook 表基址。
- 我们通过 C 全局数组访问，编译器通常需要 literal load/寄存器调度，具体开销取决于当前函数是否已内联和寄存器压力。

### Copy helpers：`copy_u32_block` / `copy_u16_block`

行为：一致，copy same-position 分支在调用层当作 no-op，copy-with-codebook 从 ref + offset 拷贝到 dst。

实现差异：

- FilmPlay 没有通用 `copy(rows, words)` helper；每个块函数直接展开固定 load/store。
- 例如 `movie_video_decode_8x8_block_arm` 的 copy path 直接复制 8 行 x 4 words。
- `movie_video_decode_4x2_block_arm` 直接复制 2 行 x 2 words。
- `movie_video_decode_2x1_block_arm` 直接一个 u32 load/store。

我们：

- `copy_u32_block(ctx, dst_off, ref_off, rows, words)` 带 `rows/words` 参数。
- 内部有 `switch (words)`、行循环、对齐分支。
- 对 ref_off 半字不对齐的情况有 fallback，用两个 `u16` 拼 `u32`。

性能影响：

- 我们多了函数调用、参数传递、`switch`、循环终止判断。
- 对齐 fallback 是必要兼容，但 FilmPlay 在专门路径里也有对非 4 字节源地址的处理；区别是 FilmPlay 的处理内嵌在固定尺寸函数里，没有通用 helper 层。

### Fill helpers：`fill_u32_block` / `fill_u16_block`

行为：一致。

差异：

- FilmPlay 在每个块函数中把颜色扩展成 `u32` 后直接写固定偏移。
- 我们通用 helper 按 `rows/words` 循环写。
- 对 leaf `2x1` / `1x2` 的双颜色 case，我们也有直接写 dst 的路径，和 FilmPlay leaf 行为一致。

性能影响：

- 大块 fill：FilmPlay 明显更展开。
- 小块 fill：我们已经有部分直接写，差距小一些。

### Delta helpers：`delta_u32_block` / `delta_u16_block`

行为：一致，都是参考像素加 signed 16-bit delta。

我们对 u32 路径使用：

```c
(val & 0x7FFF7FFF) + delta32
```

FilmPlay 对 u32 也在寄存器里使用双半字 delta，避免两个像素之间进位污染。反编译中表现为 `CONCAT22(delta, delta)` 加到载入的 u32 值。

差异：

- FilmPlay 每个块尺寸直接展开固定行偏移。
- 我们通用 helper 循环处理。
- 我们每次 helper 内计算 `delta32`，FilmPlay 通常把 palette/delta 指针和颜色保持在寄存器路径中。

性能影响：同 copy/fill，主要来自通用 helper 和 Thumb。

## 块解码函数对应表

| 我们的函数 | FilmPlay 对应 | 行为一致性 | 主要实现差异 |
|---|---|---|---|
| `decode_block_8x8` | `08103A34 movie_video_decode_8x8_block_arm` | 一致 | FilmPlay 8x8 copy/delta/fill path 大量展开；我们顶层部分由 `gbm_decode_frame_internal` 内联，但 copy/delta/fill 仍走 helper |
| `decode_block_8x4` | `0812143C movie_video_decode_8x4_block_arm` | 一致 | FilmPlay 固定 8x4；我们函数内调用 helper 并递归子块 |
| `decode_block_4x8` | `08120F04 movie_video_decode_4x8_block_arm` | 一致 | 同上 |
| `decode_block_8x2` | `081208E8 movie_video_decode_8x2_block_arm` | 一致 | FilmPlay 复制 2 行 x 4 words 全展开 |
| `decode_block_2x8` | `081205D0 movie_video_decode_2x8_block_arm` | 一致 | FilmPlay 复制 8 行 x 1 word 全展开 |
| `decode_block_4x4` | `08120BD0 movie_video_decode_4x4_block_arm` | 一致 | 我们该函数通常 inline；FilmPlay 独立 ARM 专门函数 |
| `decode_block_2x4` | `08121F84 movie_video_decode_2x4_block_arm` | 一致 | FilmPlay 固定行偏移展开 |
| `decode_block_4x2` | `0812216C movie_video_decode_4x2_block_arm` | 一致 | FilmPlay 固定行偏移展开 |
| `decode_block_8x1` | `081225A8 movie_video_decode_8x1_block_arm` | 一致 | FilmPlay 单行 4 words 展开 |
| `decode_block_1x8` | `08122344 movie_video_decode_1x8_block_arm` | 一致 | FilmPlay 8 行 halfword 固定偏移展开 |
| `decode_block_1x4` | `08121ACC movie_video_decode_1x4_block_arm` | 一致 | FilmPlay 4 行 halfword 固定偏移展开 |
| `decode_block_2x2` | `0812197C movie_video_decode_2x2_block_arm` | 一致 | FilmPlay 2 行 u32 固定偏移展开 |
| `decode_block_4x1` | `08121C40 movie_video_decode_4x1_block_arm` | 一致 | FilmPlay 单行 2 words 展开 |
| `decode_block_1x2` | `08121D90 movie_video_decode_1x2_block_arm` | 一致 | Leaf 双颜色 case 一致；FilmPlay 直接写两个 halfword 行偏移 |
| `decode_block_2x1` | `08121E88 movie_video_decode_2x1_block_arm` | 一致 | Leaf 双颜色 case 一致；FilmPlay 一个 u32 或两个 halfword 直接写 |

## 是否存在“多余边界检查”

热路径里没有看到我们做了明显的范围检查或防御性边界检查。具体看：

- block decoder 没有检查块坐标是否超出 240x160。
- copy/delta/fill helper 没有检查 ref offset 是否越界。
- payload/palette/flag 指针没有逐次检查 frame end。

因此，当前性能差异不是“边界检查太多”，而是：

- C context abstraction；
- Thumb 指令集；
- 通用 helper；
- 非对齐 flag refill；
- 函数调用/递归结构；
- 以及编译器无法像 FilmPlay 那样把固定块尺寸全展开。

## 目前编译产物观察

当前 `gba_decoder/build/gbm_decoder.o` 中 `.iwram` 反汇编显示：

- 使用 Thumb 指令。
- `copy_u32_block` 是独立函数，大小约 `0x134`。
- `decode_block_8x4`、`decode_block_4x8`、`decode_block_2x4`、`decode_block_4x2` 等仍是独立 Thumb 函数。
- `gbm_decode_frame_internal` 大小约 `0x39c`，并内联了一部分 8x8 顶层逻辑，但遇到子块和通用操作仍会跳转/调用。

部分符号大小：

```text
copy_u32_block             0x0134
decode_block_1x8           0x017c
decode_block_8x1           0x0180
decode_block_8x2           0x01e4
decode_block_2x8           0x0210
gbm_decode_frame_internal  0x039c
decode_block_1x4           0x043a
decode_block_4x1           0x0484
decode_block_8x4           0x0624
decode_block_4x8           0x06b8
decode_block_2x4           0x1212
decode_block_4x2           0x12c4
```

这说明当前 C/Thumb 版体积并不小，并且部分小块函数反而因为通用路径和内联展开变得很大。

## 与 FilmPlay 布局相关的差异

FilmPlay 的视频输入 staging 与我们当前 GBFS/NOR 直读路径不同：

- FilmPlay 从 TF/SD 以 sector 读入 `02025800+`。
- 如果当前帧跨 sector，会把当前 sector 用 DMA3 归一到 `02025800`，再追加后续 sector。
- 在进入视频解码器前，它把 2 字节 frame length 去掉。

我们当前的 `gbm_decode_frame` 是对完整 GBM 帧记录直接解码，外部通过 frame index 找到 frame offset。

这不影响播放正确性，但影响解码器内部输入对齐和热路径 refill。

## 后续优化优先级，仅供之后实施时参考

本文不修改代码，只记录后续可能方向：

1. 为视频解码热路径提供 ARM 版本，而不是 Thumb。
2. 让解码输入与 FilmPlay 一样去掉 2 字节 frame length，使 flag stream 4 字节对齐，允许 refill 用 aligned `ldr`。
3. 把 `copy/fill/delta` 按块尺寸专门化，减少 `rows/words` 参数和 `switch`。
4. 优先专门化高频 leaf/小块：`2x1`、`1x2`、`2x2`、`4x1`、`1x4`。
5. 把 bitstream state 和常用指针长期放寄存器，减少 `DecodeContext` 读写。
6. 如果继续保留 C 版，可以至少把 ref 对齐分支前移到专门函数，避免通用 helper 每次分派。

## 音频解码函数盘点

本次目标重点是视频解码性能，但为了避免混淆，这里也列出 `gbs_audio.c` 中真正属于音频解码的函数及当前状态。

| 我们的音频函数 | 行号 | 当前对比状态 |
|---|---:|---|
| `decode_obfuscated_gbs_header` | 166 | 元数据处理，不是播放热路径；FilmPlay/SuperCard 文件版本不走我们额外混淆 |
| `decode_ima_4bit` | 242 | 4-bit IMA ADPCM 单 sample 解码；此前已按 GBA 实测修正过 IMA 公式问题 |
| `decode_adpcm_3bit` | 265 | 3-bit ADPCM 单 sample 解码；本轮未与 FilmPlay 逐指令比对 |
| `decode_adpcm_2bit` | 292 | 2-bit ADPCM 单 sample 解码；本轮未与 FilmPlay 逐指令比对 |
| `parse_block_header_mono` | 428 | 音频 block header 解析；与视频无关 |
| `parse_block_header_stereo` | 449 | stereo block header 解析；与视频无关 |
| `advance_to_next_block` | 463 | 音频 block 推进；我们当前是 block/ring cache 结构，FilmPlay 是 16 KiB 半缓冲结构 |
| `decode_buffer_stereo_4bit` | 487 | 音频 PCM buffer 解码；本轮未深挖 FilmPlay 对应 IRQ 细节 |
| `decode_buffer_mono_3bit` | 525 | 同上 |
| `decode_buffer_mono_4bit` | 616 | 同上 |
| `decode_buffer_mono_2bit` | 668 | 同上 |
| `decode_buffer` / `decode_buffer_preserve_bank` | 736 / 757 | 音频模式分发和 bank 保护；与视频解码热路径无关 |

已确认的 FilmPlay 音频结构：

- 主循环读 TF/SD，把 GBS 压缩流填到 `02030000-02033FFF` 的 16 KiB 双缓冲。
- IRQ 软件解码 ADPCM，并通过 direct sound FIFO/DMA 播 PCM。
- 读卡失败时关闭声音 DMA；如果主循环太慢导致 IRQ 追上未补好的半缓冲，可能出现爆音。

本轮没有把音频 ADPCM 每个 mode 逐条指令对齐到 FilmPlay。原因是用户提出的性能担忧集中在视频解码函数，且 FilmPlay 的音频热路径是 IRQ 流式解码，和视频块树解码不是同一套代码。若后续要追音频质量/爆音根因，应单开音频对比文档。
