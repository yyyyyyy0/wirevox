# mic-fx-host

マイク入力に VST3 プラグイン（ボイスチェンジャー、EQ、ノイズ除去など）を直列に掛け、Loopback などの仮想デバイスへ出力する小さなホストです。通話アプリ側ではその仮想デバイスをマイクとして選びます。

物理マイク → VST3 を順に通す → 仮想デバイス（Loopback の `Discord out` など）

## ビルド

必要なもの: macOS、Command Line Tools、CMake 3.22 以上。JUCE は git submodule（`JUCE/`、タグ 9.0.5）で、ビルド中にネットワークは使いません。

```sh
git submodule update --init   # 初回のみ。JUCE を取得済みなら不要
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

成果物: `build/MicFxHost_artefacts/Release/mic-fx-host.app`

```sh
open build/MicFxHost_artefacts/Release/mic-fx-host.app
```

Command Line Tools の入れ方によっては、clang が標準 C++ ヘッダ（`<algorithm>` など）を見つけられないことがあります。`CMakeLists.txt` が検出して、SDK 内の libc++ ヘッダを使うように自動で切り替えます。

## 使い方

1. 初回起動ではデバイスを開きません。マイクがスピーカーに直結してハウリングするのを避けるためです。左のパネルで出力（Loopback の仮想デバイス）と入力（マイク）を選びます。
2. 入力チャンネルは、マイクがつながっているチャンネルだけにチェックを入れます。1 チャンネルなら左右の両方に同じ音を送ります。2 チャンネル以上なら 1 番目を L、2 番目を R にします。
3. 「プラグインを追加...」から `.vst3` を選ぶか、`.vst3` をウィンドウへドロップします。追加はこのファイルだけをスキャンします。
4. 上下ボタンで順番を変え、バイパスで一時的に外し、編集でプラグイン自身の画面を開きます。
5. 右下に、サンプルレート、バッファ、入出力デバイスの遅延、プラグイン遅延、合計（ms）、xrun 数が出ます。

遅延を小さくするには、バッファサイズを小さくします。入力と出力でサンプルレートが違う場合は、左のパネルでそろえてください（JUCE が内部でドリフト補正付きの合成デバイスを作ります）。

バイパス中のプラグインも遅延は残ります（切り替えで音が途切れないようにするためです）。遅延ごと無くしたいときは削除してください。

マイクへのアクセスは初回に macOS が確認します。許可しないと入力は無音になります。

設定（デバイス、チェーン、各プラグインの状態、バイパス）は `~/Library/Application Support/mic-fx-host/settings.xml` に保存し、次回起動時に復元します。読み込めないプラグインは赤字の行で残り、状態は失われません。

## 動作確認

```sh
APP=build/MicFxHost_artefacts/Release/mic-fx-host.app/Contents/MacOS/mic-fx-host

"$APP" --list-devices                          # 入出力デバイス名を表示して終了
"$APP" --selftest "/Library/Audio/Plug-Ins/VST3/TDR Nova.vst3"
(cd build && ctest)                            # 既定のテスト用プラグインが存在すれば selftest を実行
```

`--selftest` はウィンドウもオーディオデバイスも使わず、GUI と同じ `Chain` とグラフでプラグインを読み込み、48 kHz・64 サンプルのブロックで 3 秒処理します。プラグイン名、報告された遅延、出力のピーク、無音でないか、バイパス時にモノ入力が左右に届くか、チェーンを XML にして復元したときに同じになるかを確認します。成功で終了コード 0、読み込み失敗などで 0 以外です。

ctest に載せるプラグインは `-DMFX_TEST_PLUGINS="パス1;パス2"` で変えられます。

## 範囲外

メニューバー常駐、プラグインの一括スキャン、プリセット UI、公証、AU ホスティングは入れていません（VST3 のみ）。
