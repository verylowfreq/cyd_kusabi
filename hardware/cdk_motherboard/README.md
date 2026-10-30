# cdk_motherboard（基板 v1）

CyberDeck のマザーボードの KiCad データ。Freenove ESP32-S3 開発ボードと
480x320 の SPI 液晶モジュール（microSD スロット付き）を載せる。

| ファイル | 中身 |
|---|---|
| `cdk_motherboard_1.kicad_pro` / `.kicad_sch` / `.kicad_pcb` | プロジェクト・回路図・基板 |
| `ProjectLibrary.kicad_sym` / `ProjectLibrary.pretty/` | このプロジェクトで使うシンボルとフットプリント |
| `jlcpcb/gerber/` / `jlcpcb/production_files/` | JLCPCB に出したガーバー |

## 既知の誤り

**microSD の SPI 4 本（GPIO39〜42）は、回路図・基板データの信号名が実物と違う。**
液晶モジュールの SD のパッドの並びが、フットプリントでは逆順になっている。
基板はそのまま使えて、ファームウェアのピン指定（`cdk_board.h`）が実物に合わせてある。

ほかに、**部品の名前が実物と違う**ところが 2 つある（配線は正しい）。

- RS-232C の IC（U3）: シンボルは `ADM232A`。実装は 3.3V 対応の **XD3232**
- 外部端子（CN2〜CN4）: フットプリントは JST PH。実装は **Grove**

詳細（SD の実物の対応表、プルアップ抵抗のずれ）は
[HARDWARE_DESIGN.md の「基板 v1 の既知の誤り」](../../HARDWARE_DESIGN.md#基板-v1-の既知の誤り)。
仕様の検討から変わったところ（電源・RS-232C の IC など）も同じ文書の「基板 v1」にある。
