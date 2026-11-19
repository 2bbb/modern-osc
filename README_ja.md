# modern-osc

[English](README.md)

独立したパケットコーデック、送信側アドレスパターンによる配送、任意のIPv4 UDPアダプターを備えたC++17のOSCライブラリです。

## OSC対応

- MessageとBundleを符号化・復号します。入れ子のBundleと、OSC 1.0に記載された全引数タグに対応します。
- OSC 1.0のアドレスパターンと、OSC 1.1論文の階層ワイルドカード `//` に対応します。受信側Dispatcherが登録済みの具体アドレスに照合します。
- BundleのTimetagを保持し、明示的な指定でワイヤ順に即時配送します。アプリがBundleの要素を走査して配送時刻を決めることもできます。
- `s` と `S` の受信は既定でバイトを保持し、送信は既定でASCIIを検証します。厳格な受信と互換送信は別々に設定できます。

仕様資料。 [OSC 1.0](https://opensoundcontrol.stanford.edu/spec-1_0.html) · [OSC 1.1論文](https://opensoundcontrol.stanford.edu/files/2009-NIME-OSC-1.1.pdf)

## ビルドと組み込み

C++17コンパイラとCMake 3.16以上が必要です。コアは標準ライブラリを使います。任意のUDPアダプターはOSのソケットを使い、WindowsではWinsockをリンクします。

```sh
cmake -S . -B build -DMODERN_OSC_BUILD_TESTS=ON -DMODERN_OSC_BUILD_EXAMPLES=ON
cmake --build build --config Release
(cd build && ctest -C Release --output-on-failure)
cmake --install build --config Release --prefix "$PWD/install"
```

チェックアウトしたソースは `add_subdirectory` で追加できます。インストール済みのパッケージを使う場合は `CMAKE_PREFIX_PATH` にインストール先を指定します。

```cmake
find_package(modern_osc 0.1 CONFIG REQUIRED COMPONENTS core)
target_link_libraries(my_app PRIVATE modern_osc::core)
```

同梱のIPv4ユニキャスト用アダプターを使う場合は `udp` コンポーネントを指定し、`modern_osc::udp` をリンクします。独自の通信実装を使う場合は、`add_subdirectory` や `FetchContent_MakeAvailable` の前に `MODERN_OSC_BUILD_UDP=OFF` を設定できます。

## 送受信

```cpp
#include <modern_osc/dispatch.hpp>

using namespace modern_osc;

auto make_gain_packet() {
    Packet packet{Message{"/deck/1/gain", {{Float32::from_value(0.5f)}}}};
    return encode(packet);
}

template<class Peer>
auto on_datagram(Dispatcher<Peer>& dispatcher, ByteView bytes, const Peer& sender)
    -> Result<DispatchReport>
{
    auto packet = decode(bytes);
    if (!packet) return packet.error();
    return dispatcher.dispatch_packet(*packet, sender, BundlePolicy::immediate);
}
```

送信時は `encode` または `encode_into` の出力を通信側の送信関数に渡します。受信コールバックからは完全なUDPデータグラム1個と通信側の送信元を渡します。`dispatcher.add("/deck/1/gain", handler)` で登録するハンドラは `const MessageView&` と `const Peer&` を受け取ります。 [同梱UDPの返信サンプル](examples/echo.cpp)

ビューは受信バッファを参照します。同期配送が完了するまでバッファを上書きせず、後で使う値はコピーします。一つのDispatcherへの登録と配送は直列化します。照合予算を超えた場合のハンドラ呼び出しは0件です。ハンドラが例外を投げると、残りの配送を打ち切り例外を伝播します。

照合予算の既定値は1パケットあたり100万ステップです。具体アドレスは索引で引き、パターンでは調べた登録アドレスと照合状態を計上します。多数の宛先へ一斉配送する場合は `DispatchLimits` で調整できます。

UDPの切り詰めを通知しないAPIでは、データグラム全体が収まる経路を用意します。通常のIPv4 UDPのペイロードなら65,507バイトです。 [UDP利用ガイドライン](https://www.rfc-editor.org/rfc/rfc8085.html)

## 開発

`MODERN_OSC_BUILD_FUZZER=ON` にすると、ClangのAddressSanitizer・UndefinedBehaviorSanitizer付きで、シード入力を再現可能に変異させる実行ファイルをビルドできます。対応範囲、設計判断、既存ライブラリとの比較、検証範囲は [PLAN.md](PLAN.md) にあります。

## ライセンス

[MITライセンス](LICENSE)で配布します。
