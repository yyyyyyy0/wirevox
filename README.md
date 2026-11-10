# VOXRACK

マイク入力に VST3 プラグイン（ボイスチェンジャー、EQ、ノイズ除去など）を直列に掛け、Loopback などの仮想デバイスへ出力する小さなホストです。通話アプリ側ではその仮想デバイスをマイクとして選びます。

物理マイク → VST3 を順に通す → 仮想デバイス（Loopback の `Discord out` など）

## ビルド

必要なもの: macOS、Command Line Tools、CMake 3.22 以上。JUCE は git submodule（`JUCE/`、タグ 9.0.5）で、ビルド中にネットワークは使いません。

```sh
git submodule update --init   # 初回のみ。JUCE を取得済みなら不要
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

成果物: `build/MicFxHost_artefacts/Release/VOXRACK.app`

```sh
open build/MicFxHost_artefacts/Release/VOXRACK.app
```

Command Line Tools の入れ方によっては、clang が標準 C++ ヘッダ（`<algorithm>` など）を見つけられないことがあります。`CMakeLists.txt` が検出して、SDK 内の libc++ ヘッダを使うように自動で切り替えます。

## 使い方

1. 初回起動ではデバイスを開きません。マイクがスピーカーに直結してハウリングするのを避けるためです。左のパネルで出力（Loopback の仮想デバイス）と入力（マイク）を選びます。
2. 入力チャンネルは、マイクがつながっているチャンネルだけにチェックを入れます。1 チャンネルなら左右の両方に同じ音を送ります。2 チャンネル以上なら 1 番目を L、2 番目を R にします。
3. 「プラグインを追加...」から `.vst3` を選ぶか、`.vst3` をウィンドウへドロップします。追加はこのファイルだけをスキャンします。
4. 上下ボタンで順番を変え、バイパスで一時的に外し、編集でプラグイン自身の画面を開きます。
5. 右下に、サンプルレート、バッファ、入出力デバイスの遅延、プラグイン遅延、合計（ms）、xrun 数が出ます。

遅延を小さくするには、バッファサイズを小さくします。入力と出力でサンプルレートが違う場合は、左のパネルでそろえてください（JUCE が内部でドリフト補正付きの合成デバイスを作ります）。

バイパス用のパラメータを持つプラグイン（VST3 の多く）は、バイパス中も遅延が残ります（切り替えで音が途切れないようにするためです）。遅延ごと無くしたいときは削除してください。

表示される入力・出力デバイスの遅延には、バッファ 1 回分が含まれています。

プロダクト名は `CMakeLists.txt` の `MFX_PRODUCT_NAME` ひとつで決まります（`.app` の名前、ウィンドウタイトル、設定フォルダ、バンドル ID `local.nil.voxrack`）。バンドル ID が変わったため、旧 mic-fx-host から移ると、マイクの許可を macOS がもう一度確認します。

マイクへのアクセスは初回に macOS が確認します。許可しないと入力は無音になります。

設定（デバイス、チェーン、各プラグインの状態、バイパス）は `~/Library/Application Support/VOXRACK/settings.xml` に保存し、次回起動時に復元します。旧名 mic-fx-host の `~/Library/Application Support/mic-fx-host/settings.xml` があって新しいファイルがまだ無いときは、起動時に一度だけ写しを作って引き継ぎます（旧フォルダには触りません）。`--settings <file>` を付けると、そのファイルを設定として使い、引き継ぎもしません（見た目の確認など、実際の設定に触れたくないとき用）。読み込めないプラグインは赤字の行で残り、状態は失われません。

設定ファイルが壊れていて読めないときは、上書きする前に同じフォルダの `settings.unreadable.xml` へ写しを残します。プラグインの読み込み中にアプリごと落ちた場合、次の起動ではチェーンを読み込まず、元の設定を `settings.crashed.xml` に残します。

## 動作確認

```sh
APP=build/MicFxHost_artefacts/Release/VOXRACK.app/Contents/MacOS/VOXRACK

"$APP" --list-devices                          # 入出力デバイス名を表示して終了
"$APP" --selftest "/Library/Audio/Plug-Ins/VST3/TDR Nova.vst3"
"$APP" --selftest-settings                     # 旧フォルダからの設定の引き継ぎを一時フォルダで確認
(cd build && ctest)                            # selftest と設定の引き継ぎの確認を実行
```

`--selftest` はウィンドウもオーディオデバイスも使わず、GUI と同じ `Chain` とグラフでプラグインを読み込み、48 kHz・64 サンプルのブロックで 3 秒処理します。プラグイン名、報告された遅延、出力のピーク、無音でないか、バイパス時にモノ入力が左右に届くか、チェーンを XML にして復元したときに同じになるかを確認します。成功で終了コード 0、読み込み失敗などで 0 以外です。

`--selftest-settings` は一時フォルダの中だけで、旧設定が新しい場所へ写されること、旧ファイルが残ること、新しいファイルが既にあれば上書きしないことを確認します。

ctest に載せるプラグインは `-DMFX_TEST_PLUGINS="パス1;パス2"` で変えられます。

## 範囲外

メニューバー常駐、プラグインの一括スキャン、プリセット UI、公証、AU ホスティングは入れていません（VST3 のみ）。
