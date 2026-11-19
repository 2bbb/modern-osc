# modern-osc 設計計画

`modern-osc` はC++17のOSCライブラリ。初版の **v0.1はOSC 1.0の全記載型を扱うコーデック、OSCパターン配送、UDP送受信**に絞る。macOS・Linux・Windowsを対象とし、標準ライブラリだけで使えるコアに、必要に応じて標準添付のUDPソケット実装や利用側の通信実装を組み合わせる。アプリ用ルーティングはv0.2、スケジューラとストリームフレーミングはv0.3の予定。

## 1. 対応するOSC仕様

OSC 1.0仕様書を基本とし、1.1については公式サイトが案内するNIME 2009論文を参照する。1.1は基本のパケット符号化を引き継ぎ、`T F N I t` を必須型に加え、階層ワイルドカード `//` とストリーム用のdouble-END SLIPを示した。v0.1は `//` を含むアドレス照合を提供し、SLIPはv0.3で追加する。論文が言及する複素数・行列などの追加型は、公開資料からタグと符号化を確定できないため保留する。 [OSC 1.0仕様書](https://opensoundcontrol.stanford.edu/spec-1_0.html) · [OSC 1.1公式案内](https://opensoundcontrol.stanford.edu/spec-1_1.html) · [NIME 2009論文 §3–4](https://opensoundcontrol.stanford.edu/files/2009-NIME-OSC-1.1.pdf)

| 型タグ | 値と表現 |
| --- | --- |
| `i` / `h` | 符号付き32 / 64ビット整数 |
| `f` / `d` | IEEE 754の32 / 64ビット浮動小数点 |
| `s` / `S` | OSC-string。受信はバイト保持、送信は既定でASCII検証。`S` は `Symbol` として区別する |
| `b` | Blob。長さ付きの任意バイト列。空Blobも扱う |
| `t` | 64ビットのOSC Timetag。Bundleの時刻にも同じ型を使う |
| `c` / `r` / `m` | 32ビットで送るASCII文字 / RGBA / ポートID付きMIDIの4バイト |
| `T` / `F` / `N` / `I` | 真 / 偽 / Nil / Impulse。引数領域は0バイト。`I` は1.0でInfinitumと呼ばれる同じタグ |
| `[` / `]` | 型タグ上の配列境界。異種混在、空配列、入れ子を扱う |

トップレベルの `Packet` は `Message` または `Bundle`。Bundleは空でもよく、要素としてMessageとBundleを任意の順序で持てる。コーデックはバイト順、4バイト境界、ゼロパディング、要素長、型タグと引数の対応を検証する。配列とBundleの深さ、パケット長、要素数には設定可能な上限を設ける。

型タグ省略は明示した互換設定で受け入れ、アドレスの後にデータがなければ空引数として扱う。引数を判定できない型タグ省略Messageや未知タグのあるMessageはrawバイトを保持し、型付き配送から外す。Bundleの要素境界を検証できた場合は後続のMessageを引き続き処理できる。形式違反のある既知型や壊れた要素境界はパケット全体のエラーとし、復号の途中でハンドラを呼び出さない。

`s` と `S` は受信時に元のバイトを保持し、厳格受信を選んだ場合にASCIIを検証する。既定の送信はASCIIを検証し、明示した互換設定で非ASCIIバイトの送信を許可する。どの設定でもNUL終端とゼロパディングを検証し、埋め込みNULを含む文字列の構築はエラーとする。アドレス、型タグ、`c` はASCIIのまま。実アプリとの交換では互換受信が必要になった入力と厳格設定の挙動を記録する。 [OSC 1.0のOSC-string定義](https://opensoundcontrol.stanford.edu/spec-1_0.html)

## 2. OSCの照合とアプリのパス操作

| 対象 | 役割 | 入力と結果の例 |
| --- | --- | --- |
| v0.1 `Address` | 検証済みの具体アドレスをメソッドとして登録 | `/cue/12/start` を登録し、同じアドレスへのMessageを受ける |
| v0.1 `AddressPattern` | Messageのアドレス欄に送信側が指定したパターンを、受信側Dispatcherが登録済みの具体アドレスに照合 | `/cue/{12,15}/start` で該当する全メソッドに配送 |
| v0.2 パス操作 | 分解、結合、親パス取得、セグメント単位のprefix操作 | `/mixer/ch/12/gain` から `/mixer` を取り除いて `/ch/12/gain`。`/mixer2` は対象外 |
| v0.2 `Route` | **受信側**が登録したテンプレートで受信した具体アドレスを照合し、名前付きで値を取得 | `cue`、`capture("id")`、`start` を登録すると、`/cue/0012/start` の `id` に `0012` を保持 |

OSC 1.0の `?`、`*`、`[abc]`、`[a-z]`、`[!abc]`、`{foo,bar}` を実装する。`*` の範囲は1セグメント内。1.1の `//` は階層を横断し、たとえば `/rig//gain` は `/rig/a/gain` や `/rig/a/b/gain` を照合する。パターンは再利用できる状態機械にコンパイルする。照合で同じ状態と入力位置の組を再探索しない方式を選び、パターン長、状態数、1回の配送全体の照合量にも上限を設ける。1メッセージにつき、各登録メソッドへの配送は最大1回。 [OSC 1.0仕様書「OSC Message Dispatching and Pattern Matching」](https://opensoundcontrol.stanford.edu/spec-1_0.html) · [NIME 2009論文 §3.4](https://opensoundcontrol.stanford.edu/files/2009-NIME-OSC-1.1.pdf)

v0.2ではprefixの追加・除去・差し替え、動的なアドレス組み立て、受信側フィルターを追加する。キャプチャはセグメントを組み立てるAPIで指定し、合法なアドレス文字 `:` はリテラルとして保持する。キャプチャの数値変換は呼び出し側が明示する。比較は大文字小文字を区別し、`.`、`..`、`%2F` も表記のまま扱う。`Route` は受信した具体アドレスに適用する。送信側パターンの配送先はv0.1で定義した具体メソッドの登録集合。 [QLabのキューアドレス](https://qlab.app/docs/v5/scripting/osc-dictionary-v5/) · [TouchOSCのアドレス組み立て](https://hexler.net/touchosc/manual/editor-messages-osc) · [TouchDesignerの受信フィルター](https://docs.derivative.ca/OSC_In_DAT)

`/a//b` は `/a/b` を含む中間0階層以上に一致し、先頭 `//b` は `/b` を含む任意の深さに一致する。末尾 `//` はv0.1の定義として子孫メソッドに一致し、`//` 単体は登録済み全メソッドを対象にする。連続する3個以上の `/`、末尾の単独 `/`、空の具体セグメントや空の選択肢は構文エラーとする。`{foo,bar}` はセグメント中の文字列選択で、選択肢にパターン記号があれば構文エラーとする。仕様が例示しない境界はライブラリの規則として明記し、仕様対応表の期待結果に含める。

## 3. APIと実行モデル

- **パケットAPI。** `Message`・`Bundle`・`Argument` は値として所有できる。引数の型を `std::variant` ベースで表し、`Symbol`・`Timetag`・`Rgba`・`Midi` などは専用型にする。ワイヤ上の型はAPIで明示でき、受信時の数値変換も明示的に行う。
- **バッファAPI。** `decode` は入力を参照するパケットビュー、`to_owned` は保持できる値、`encoded_size` は必要な出力長、`encode_into` は呼び出し側バッファへの書き込み。容量不足や入力の形式違反は出力を書き始める前に返す。読み出しはバイト操作か `memcpy` とし、4バイト境界はパケット先頭からの相対位置で検証する。文字列・Blobも入力を参照する。同期配送が完了するまで利用側が受信バッファを保持し、後で使う値は所有型にするかバッファの所有権ごと移す。入力由来の失敗はエラーコードと位置を持つC++17の結果型で返す。メモリ確保や利用側コールバックの例外は通常のC++の契約に従う。v0.1はリアルタイムオーディオコールバック向けの無確保・待ち時間を保証しない。
- **配送。** `Dispatcher<Context>` の `dispatch(MessageView, const Context&)` は該当ハンドラを呼び出し元スレッドで同期的に実行する。利用側の型 `Context` を各ハンドラの第2引数に渡すので、UDPの送信元やフレームワーク独自の返信先を使える。Messageの引数ビューとContext参照をハンドラから持ち出す場合は必要な値を所有する。登録単位はハンドラIDで、同じ具体アドレスに複数登録でき、それぞれ最大1回呼ぶ。利用側が登録・解除・配送を直列化する。配送中の登録変更と再入配送は明示エラーにし、必要な変更は呼び出し側で配送終了後に行う。
- **Bundleの即時配送。** `dispatch_packet(PacketView, const Context&, BundlePolicy::immediate)` をv0.1で提供する。検証済みパケットを深さ優先のワイヤ順で走査し、パケット全体の照合と配送先一覧の作成を先に行う。照合や一覧の上限を超えた場合はハンドラを一度も呼ばずにエラーを返す。成功時に同じContextで一覧順に同期配送する。ハンドラの例外は呼び出し元に伝播し、その時点で残りの呼び出しを打ち切る。子Bundleも含めて時刻のビット列はビューに保持する。時刻による延期、親子時刻の妥当性判定、実行順の再編はv0.3のスケジューラが担う。
- **時刻。** v0.1の `Timetag` はNTP形式の64ビットを保持し、OSC 1.0のimmediateは値 `0x0000000000000001` とする。v0.3でNTP eraを解決する絶対時刻変換と、呼び出し側が時刻を供給するスケジューラを追加する。1.0の実行方針では親子Bundleの時刻、遅着、同じBundle直下の順序と連続配送を扱う。 [OSC 1.0時刻規則](https://opensoundcontrol.stanford.edu/spec-1_0.html) · [NIME 2009論文 §3.6](https://opensoundcontrol.stanford.edu/files/2009-NIME-OSC-1.1.pdf) · [NTPの日時表現 RFC 5905](https://www.rfc-editor.org/rfc/rfc5905.html#section-6)
- **ストリーム。** v0.3で32ビット長プレフィックスとdouble-END SLIPの増分フレーマーを追加する。フレーム上限と読み込み分割を扱い、方式は接続ごとに明示する。TCPソケット接続管理、DNS-SD、OSCQueryは後続候補。 [OSC 1.0のストリーム形式](https://opensoundcontrol.stanford.edu/spec-1_0.html) · [NIME 2009論文 §4](https://opensoundcontrol.stanford.edu/files/2009-NIME-OSC-1.1.pdf) · [SLIP RFC 1055](https://www.rfc-editor.org/rfc/rfc1055.html)

### v0.1のUDP差し替え境界

通信実装との境界は**完全なデータグラム1個分のバイト列**。`modern_osc::core` はデータグラムを `decode` に渡せば使え、エンコードした結果を任意のUDP実装に渡せる。送信元アドレス・ポート等は通信側が持ち、必要に応じてアプリの受信コンテキストでメッセージと一緒に扱う。標準ソケットもフレームワーク固有実装も同じ経路を通る。

| 組み込み方 | v0.1の契約 |
| --- | --- |
| **同梱ソケット** `modern_osc::udp` | POSIX / WinsockのRAIIラッパー。IPv4ユニキャストと数値IPを対象に `bind`、`send_to`、`receive_into` で非ブロッキングI/O、`wait_readable(timeout)` で有限の受信待機を提供する。待機は準備完了・タイムアウト・エラーを区別し、その後の受信は未着になり得る。受信時は送信元と受信長を返す。WindowsのUDP `WSAECONNRESET` は回復可能な相手到達エラーとして返し、ソケットを保持する。IPv6やマルチキャストは後続候補 |
| **既存の通信実装** | フレームワークのイベントループや受信コールバックから、完全な受信データグラムを `decode` に供給する。送信時は `encode_into` で得たバイト列をアプリの送信APIに渡す。継承や既存ループの差し替えは不要 |

受信バッファは呼び出し側が用意する。macOS・Linuxでは `recvmsg` の出力 `MSG_TRUNC`、Windowsでは `recvfrom` の `WSAEMSGSIZE` を使い、切り詰めや未着をOSCの復号に渡さない。外部アダプターは切り詰め通知がある受け口、または全経路で対象データグラムの最大長を収める受け口を使う。通常のIPv4 UDPなら65,507バイト以上でペイロードが収まる。先頭だけで有効に見えるOSCパケットもあるため、受信長から完全性を推測しない。非同期送信を使う側は送信完了までバッファを保持する。 [Apple `recvmsg`](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/recvmsg.2.html) · [Linux `recvmsg`](https://man7.org/linux/man-pages/man2/recvmsg.2.html) · [Winsock `recvfrom`](https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-recvfrom) · [RFC 8085](https://www.rfc-editor.org/rfc/rfc8085.html)

利用例は、同梱ソケットで `wait_readable` → `receive_into` → 送信元をContextにして同期配送する例と、外部UDP受信コールバックから同じ処理を呼ぶ例を作る。別スレッドで受信する例では所有型を利用側のキューに渡す。ソケットハンドルの公開とライブラリ内のスレッド管理は用途が具体化した時点で個別に検討する。 [JUCE `DatagramSocket`](https://docs.juce.com/master/classjuce_1_1DatagramSocket.html)

## 4. 既存ライブラリとの比較

公開APIとリリース情報に基づく比較。初版で相互運用の測定結果を追記する。

| ライブラリ | 公開資料で確認した範囲 |
| --- | --- |
| **oscpack** | C++のパケット生成・解析と最小限のUDP機能。アドレス空間の構築・ルーティングは対象外と明記。 [公式README](https://github.com/RossBencina/oscpack#readme) |
| **liblo** | C APIとC++ラッパー。double-END SLIPはオプションで利用でき、2026年4月に0.36をリリース。0.36の公開送受信型一覧に `r` と配列タグの記載はない。 [リリースノート](https://liblo.sourceforge.net/NEWS.html) · [型一覧](https://liblo.sourceforge.net/docs/lo__osc__types_8h.html) · [C++ラッパー](https://liblo.sourceforge.net/docs/group__liblocpp.html) |
| **JUCE OSC** | JUCEに統合されたC++ API。公開の引数型は `i f s b r`。 [OSCTypesリファレンス](https://docs.juce.com/master/classjuce_1_1OSCTypes.html) |
| **oscpp** | C++11のヘッダーオンリー。配列と割り当てを抑えたパケット処理を提供。追加の引数型、パターン照合、時刻配送には制約がある。 [公式README](https://github.com/kaoskorobase/oscpp#readme) |
| **libossia** | OSCアドレス空間をノードと属性付きパラメータのツリーとして扱い、OSCやOSCQueryで公開できる分散オブジェクトモデル。modern-oscはデータグラムを受け取るコーデックとメソッド配送に集中する。 [libossia公式概要](https://ossia.io/site-libossia/about.html) |
| **oscpkt** | 作者の文書の転載では、単一ヘッダーのC++パケット処理とパターン照合、別ヘッダーのUDPを説明。対応型タグの詳細は確認保留。 [作者文書の転載](https://github.com/bwagner/oscpkt#readme) |
| **modern-osc（計画）** | v0.1はC++17の独立コア、仕様に記載された全型、送信側のパターン配送、差し替え可能なUDP。後続版でキャプチャとパス操作、時刻・フレーミングを追加。 |

既存ライブラリのソースは参照せず、OSCの一次資料と関連RFCから仕様対応表と期待バイト列を作る。既存ライブラリは公開文書とバージョンを固定したブラックボックスの送受信で比較する。READMEには導入方法、対応範囲、利用例、検証済みの比較を載せる。

## 5. 実装順と初版の完了条件

| 段階 | 実装 | 完了の確認 |
| --- | --- | --- |
| v0.1 パケット | 仕様対応表、全型コーデック、入れ子Bundle、バッファAPI | 仕様から導いたバイト列と、厳格な既知型のパケットの再エンコードを照合する。NaNペイロード・負のゼロ、非整列入力、容量不足時の出力不変、構造エラーとopaqueの区別を確認する |
| v0.1 アドレスと配送 | 1.0パターン、1.1の `//`、型付きContext、Bundle即時配送 | 入れ子Bundleの順序、送信元の引き渡し、配送中の変更拒否、`*` と `//` を含む長い不一致の照合予算を確認する。後半のMessageで予算を超えたBundleで配送数0を確認する。コーデックとパターンはASan/UBSan付きで、境界・深い入れ子のシードからファズ検証する |
| v0.1 UDPと組み込み | 同梱ソケット、外部UDP実装の組み込み例、CMake | macOS・Linux・Windowsでループバック双方向通信、有限待機、返信先、空Bundleに見える切り詰めを確認する。Windowsでは閉じたポートへの送信後も別の正常なデータグラムを受信する。外部UDPでも同じコアを通す |
| v0.1 配布と相互運用 | CMakeのソース組み込みとインストール済みパッケージ、実アプリとの交換 | C++17利用側から `add_subdirectory` とインストール後の `find_package` を確認する。`FetchContent` は同じソース組み込みターゲットを使う例を載せる。SuperColliderでBundleと応答、PdまたはMaxでMessageの交換を確認し、利用アプリの型の差を実測で記録する |
| v0.2 アプリ用ルーティング | パス操作、キャプチャ、フィルター | `0012` の保持、prefixのセグメント境界、リテラル `:` を確認する |
| v0.3 時刻とフレーミング | Timetag変換、スケジューラ、2種のフレーマー | 模擬時刻でimmediate・遅着・順序・2036年折り返しを確認する。分割・結合したストリーム、フレーム上限、SLIPのエスケープを確認する |

## 6. 実装着手前の設計レビュー

公開名前空間は `modern_osc`。プラットフォーム差分は同梱UDPの実装ファイルに置き、コアも通常のCMakeライブラリとしてビルドする。Contextを使うDispatcherのテンプレート部分はヘッダーに置く。v0.xはAPIとABIの変更があり得る。公開ライセンスは [MIT](LICENSE)。

復号の初期上限案はパケット1 MiB、各配列・Bundleの深さ32、全要素・引数の合計16,384、アドレスパターン1,024バイト。照合の上限はコンパイル済み状態・遷移数と、1パケット中の評価ステップ数で制御する。既知型の往復検証では浮動小数点をビット列のまま比較する。ファズは対象をコーデックとパターンに絞り、再現用シードを保存する。ソースの公開前に同梱設定のビルドと利用例で上限値を検証する。

Claude Desktopは13:01 JSTに、配送先一覧を事前に確定し、予算超過時にBundleを部分適用しないことを条件に実装着手OKと判定。上記の即時配送の契約と完了条件で反映済み。実装と検証を終えた差分は同じ担当へレビューに出し、指摘が解消するまで繰り返す。

## 7. v0.1の検証記録

- macOSのApple ClangとLinuxのGCCで、C++17のコーデック・配送・実UDPの各検証を実行した。実装レビューで再現された512件、14,352バイトのBundleは、具体アドレスの索引追加後に既定値で512件配送できることを確認した。
- macOSでClangのASan/UBSan付きシード変異を30,000回実行した。インストール済み `find_package` とコアのみの `add_subdirectory` は利用側プロジェクトから実行した。
- SuperCollider 3.14.1で、`i`・`d`・UTF-8を含む2件のBundleと返信順を実測した。Pure Data 0.56.2で整数と文字列のMessageを双方向に交換した。
- Claude Desktopは13:19 JSTに、索引とWindows SDKのマクロ対策を再レビューして実装OKと判定した。Windows x64向けMinGWクロスビルドは成功。MSVCとWindows上のUDP実行は、[CI](.github/workflows/ci.yml) のWindowsジョブで確認する。
