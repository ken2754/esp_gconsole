# esp_gconsole

**ES3C28P（ESP32-S3 / 8MB PSRAM / 2.8インチIPS液晶＋静電容量タッチ）** をベースにした、子ども向けのオリジナル携帯ゲームコンソールプロジェクトです。

- **ターゲットジャンル**：横スクロールアクション（マリオ風）／リズムゲーム（太鼓・リズム天国風）
- **ハードウェア特徴**：オンボードの液晶・タッチ・オーディオコーデック（ES8311）・スピーカーアンプ（FM8002E）・LiPo充電回路（TP4054）を最大限に活かし、**外付け部品は最小限（タクトスイッチ×6・8Ω 1Wスピーカー・LiPoバッテリー）** で構成します。
- **開発フレームワーク**：ESP-IDF v5.x + LovyanGFX + `espressif/es8311`

---

## ドキュメント

- [ハードウェア仕様・配線ガイド (docs/HARDWARE.md)](docs/HARDWARE.md)
- [ソフトウェア・ゲームエンジン設計 (docs/ARCHITECTURE.md)](docs/ARCHITECTURE.md)

---

## リポジトリ構成

```text
esp_gconsole/
├── README.md
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── docs/
│   ├── HARDWARE.md        # ES3C28Pピン配置・6ボタン並列結線・バッテリー注意点
│   └── ARCHITECTURE.md    # 描画・入力抽象化・I2S基準クロック・ゲーム追加方法
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml  # LovyanGFX, espressif/es8311
    ├── board_pins.h       # ES3C28P 固定ピン定義
    ├── main.cpp           # 初期化・60fpsメインループ
    ├── hal/
    │   ├── display.hpp/.cpp  # LovyanGFX (ILI9341V 320x240 + FT6336Gタッチ)
    │   ├── input.hpp/.cpp    # 4ピン6ボタンデバウンス・操作モード抽象化
    │   └── audio.hpp/.cpp    # ES8311 + I2S DMAミキサ（トーン・サンプル基準時計）
    └── game/
        ├── game_app.hpp      # ゲーム共通インターフェース
        └── hw_test_game.hpp/.cpp # ハード動作確認用テストアプリ
```

---

## ビルド・書き込み手順（ESP-IDF v5.x）

> [!NOTE]
> ESP-IDF v5.0以上（v5.2〜v5.4推奨）の環境がセットアップされている前提です。

```bash
# 1. ESP-IDF環境を読み込む（パスは環境に合わせて変更）
. $HOME/esp/esp-idf/export.sh

# 2. ターゲット設定（初回のみ）
idf.py set-target esp32s3

# 3. ビルド（初回は idf_component.yml から LovyanGFX と es8311 を自動取得します）
idf.py build

# 4. 書き込み＆シリアルモニタ
idf.py -p /dev/ttyACM0 flash monitor
```

### ビルド時のトラブルシュートメモ
- **I2Cドライバ競合エラーが出る場合**：ESP-IDF v5.2以降で `CONFLICT! driver_ng is not allowed to be used with this old driver` が出た場合は、`idf.py menuconfig` の `Component config -> Driver configurations -> I2C configuration` で `Skip legacy I2C driver conflict check`（`CONFIG_I2C_SKIP_LEGACY_CONFLICT_CHECK`）を確認するか、コンポーネントバージョンを合わせてください。
- **画面の色が反転している／向きが逆の場合**：[main/board_pins.h](main/board_pins.h) の `LCD_INVERT_COLORS` や `LCD_DEFAULT_ROTATION` を調整してください。
