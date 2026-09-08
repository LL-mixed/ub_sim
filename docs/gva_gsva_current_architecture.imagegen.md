# GVA / GSVA 架构插画生成记录

日期：2026-09-08。用于维护图像来源和再次生成所需提示。

## 交付与边界

- 逻辑依据：[SVG 原图](gva_gsva_current_architecture.svg)。
- 选定交付：[空间设备插画 PNG](gva_gsva_current_architecture.generated.png)。
- 选定版本：四张生成结果中的第二张，由用户明确选择。
- 原始文件名：`exec-7eb091bc-98c7-4c9d-8745-9cea88a15629.png`。
- 方式：内置 image_gen；先生成，再针对连接与文字做定点修订。
- 原生尺寸：1536 × 1024；未做人工插值放大。
- 模型：工具不支持显式模型选择，未返回模型版本，无法核验为 GPT-image2。
- 参考图：SVG 的光栅渲染；仅用于表达架构逻辑，布局重新设计。
- 选定图展示架构职责，完整生命周期与 reader 顺序继续由 SVG 表达。
- 图中的设备、线缆和 checkpoint 是视觉符号，不要求新增硬件或服务进程。

核对结果：GVA/GSVA 位于左侧；OBMM 与 UB-SSD 同级并列；无两种 backing 之间的
依赖连接；直接数据路径分别连接两种资源；控制路径以虚线区分。
第二版保留地址数字和设备小字，其中存在生成文字瑕疵，不能作为地址布局或 ABI
依据；精确逻辑以 SVG 和正文为准。选定图未表达吞吐或硬件性能。

## 原始提示与修订

下列英文为实际提交给工具的提示。修订按顺序作用于前一张输出。
当前交付采用前两个提示产生的第二张图；第三、第四张仅保留提示历史，未采用。
重做时以 SVG 的最新逻辑为准，不把设备形象当作硬件规格。

### Initial generation

```text
Use case: scientific-educational.
Create a completely redesigned premium architectural illustration for an engineering document titled "GVA / GSVA". This is a spatial, isometric, physically tangible 3D technical illustration, with server chassis, memory modules and SSD drawers, NOT a flat flowchart, NOT a restyled copy of the reference's rectangles and NOT a dashboard. Use the attached SVG raster ONLY as the source of architecture semantics. Discard its layout, text-heavy boxes and lower panels. High-definition landscape composition, largest native resolution available, crisp readable restrained English labels, bright neutral studio background, elegant shadows, precise metal and PCB materials, teal memory accents and amber storage accents with neutral silver equipment. Do not make a blue/purple sci-fi city.

COMPOSITION:
Far left: a small group of generic client compute nodes, under prominent "GVA / GSVA" title and label "Client access". Near these nodes two clean conceptual address ribbons labeled "GVA" and "GSVA", the latter showing the same aligned address slice across multiple nodes. These ribbons are software address semantics, NOT hardware chips. Small caption "Shared addresses, checked lifetimes".
Upper center: an airy translucent logical control layer labeled "Memory Service", with compact subtitle "Identity · Placement · Version · Lifecycle". Immediately adjacent beneath it a separate smaller logical element labeled "Address manager" with subtitle "Aperture · Segment · Mapping". Do not portray this as an additional physical data-forwarding machine.
Right: TWO DISTINCT, EQUALLY PROMINENT, SIDE-BY-SIDE isometric hardware assemblies on the SAME LEVEL: an open memory appliance showing DIMMs, labeled "OBMM memory pool"; and an SSD shelf showing individual drives, labeled "UB-SSD". Below the pair: "Parallel backing resources". Neither sits on top of the other. There is absolutely NO edge, arrow or pipe between the two resources.
Foreground center: direct data access fabric from client/provider endpoints, splitting independently to both backing resources, clearly labeled "Direct data plane". This path bypasses the Memory Service control layer. Show a subtle key/token/epoch checkpoint along this path, a functional gate NOT an extra mandatory hardware device. Distinct dashed thin control lines connect client resolution to service and service placement/configuration to each backing. Place a tiny legend "Dashed: control" and "Solid: data". Keep all lines readable and unambiguous, no crossings through labels.
Small annotations: near memory "Export / import / mapping"; near storage "Versioned block I/O"; near the control layer "Descriptors only; no payload proxy". At bottom a short footnote "Logical architecture illustration; device shapes are illustrative."

INVARIANTS:
GVA/GSVA remain on the LEFT as access semantics. OBMM memory pool and UB-SSD are peer resources, never a stack or dependency. Control-plane placement/identity separate from direct provider payload access. Do not imply SSD block IDs are virtual addresses, do not imply every backend supports CPU load/store, do not suggest URMA implements OBMM mapping. No invented performance numbers, no claims of production-complete manager/failover. Preserve architecture truth while radically improving spatial presentation. Avoid paragraphs, flat rounded boxes, lifecycle panels, crowded labels, decorative glowing orbs, logos and watermarks. Request GPT-image2 if the tool supports model selection; otherwise return the available image without inventing model metadata.
```

### Data connections and address-label correction

```text
Edit this architecture illustration with very limited technical corrections. Keep the existing composition, perspective, devices, colors, readable typography and all other content intact. 1. The foreground direct-data cable branches after the key checkpoint. Currently one branch correctly plugs into UB-SSD and the other ends loose in the white floor near the lower center-right. Route that loose branch up and plug it visibly into a front connector on the OBMM memory pool chassis. Both peer resources MUST each have an independently connected solid data path from the checkpoint; no loose cable ends and no cable between the two backends. 2. In the GSVA address grid the bottom row has 0x2000 twice. Correct the cell immediately RIGHT of the highlighted 0x2000 column to 0x3000. Each row should read 0x0000, 0x1000, 0x2000, 0x3000, 0x4000, ... . 3. Add two subtle thin dashed control connectors branching from the right edge of the Memory Service translucent panel, ending independently at the OBMM memory pool and UB-SSD chassis tops. These control connectors should not touch the solid foreground data cables and should avoid all text. No connector between OBMM and SSD. Do not add any other blocks or text. Render a sharp native high-definition image.
```

### Address-label simplification

```text
Final precise cleanup of the attached isometric GVA / GSVA illustration. Keep the entire composition, devices, all control/data connectors and all other typography unchanged. ONLY TWO changes: (1) REMOVE ALL hexadecimal address labels (0x0000, 0x1000, 0x2000, 0x3000, 0x4000 and ellipses) from BOTH the GVA ribbon and the GSVA grid at lower left. Keep the glass cells, their outlines and the aligned highlighted teal column. The cells must be EMPTY, with no replacement text or numbers. The GVA and GSVA labels outside the ribbons remain. (2) Replace the small garbled text directly underneath the memory chassis with exactly 'Shared memory'. Maintain clean readable letters and sharp high-definition rendering. Do not add anything else.
```

### Final caption cleanup

```text
Precise image cleanup, ONE tiny area only. Remove the single slanted caption under the central-right OBMM memory pool chassis, the line that currently looks like 'Export / imeort / mapping'. Its pixels are approximately x=895..1095, y=610..665 on the 1536x1024 image. Fill ONLY those caption letter pixels with the continuous light background, preserving the nearby cables, chassis and shadows. Do NOT replace the removed caption with any new text. Do NOT touch the 'Shared memory' caption beneath the left address grid. Preserve all other pixels and labels as closely as possible. No other edits.
```

