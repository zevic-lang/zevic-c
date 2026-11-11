# Zevic 実装プラン

仕様書(「Zevic 言語仕様」)をもとに、**Hello World から、Zevic 自身で書いたコンパイラが自分をビルドできる(セルフホスティング)まで**を、順に実装する計画です。

- 名前は仮。公開前に確認する(→ フェーズ0)
- 最初のコンパイラ `zevic0` は **C(C11)** で書く。出力は **テキストの LLVM IR**。ビルドと結合は **clang**
- GC は最初はなし(`malloc`して放置)。段階⑤で Boehm GC に差し替える
- 各段階は「動くものが増える」単位で区切る。段階が終わるまで、次の機能に手を付けない

---

## 0. 全体の流れ

| フェーズ | 内容 | 完了の目安 | リリース |
| --- | --- | --- | --- |
| 0 | 準備(名前、リポジトリ、環境) | リポジトリがあり、CIが緑 | |
| 1 | 手書き LLVM IR の実験 | `.ll` から Hello World が動く | |
| ① | Hello World | `zevic hello.zev -o hello` が動く | v0.0.1 |
| ② | 計算と制御 | FizzBuzz、階乗が動く | v0.0.2 |
| ③ | データと最小のジェネリクス | `struct` `enum` `match`、ジェネリクスの最小版、標準ライブラリの `Option` `Result` `Array` `String` | v0.0.3 |
| ④ | クラスとC連携 | `class` `interface`、ジェネリクスの制約、標準ライブラリの `Map`、`extern` `export` | v0.1.0(公開) |
| ⑤ | 抽象化 | クロージャ、`defer`、`params`、Boehm GC | v0.2.0 |
| ⑥ | ツール | `zevpm`、`test`、`zevic.toml`、標準ライブラリ第2段(`Process` を含む) | v0.3.0 |
| ⑦ | セルフホスティング | Zevic製コンパイラ、`zevfmt`、ブートストラップ | v0.4.0 |
| 仕上げ | 1.0 に向けた整備 | 仕様の凍結、ドキュメント、配布 | v1.0.0 |

**進め方の原則**

1. 各段階の最初に、その段階で通すべき**サンプルとテスト**を先に書く(`examples/` と `tests/`)
2. 仕様と実装が食い違ったら、**先に仕様書を直す**。実装が先に進まない
3. 各段階の終わりに、タグを打ち、`CHANGELOG.md` に1行書く
4. 「速さ」と「エラーメッセージの親切さ」は、段階⑥までは後回しにする。**正しく動くこと**が先

---

## フェーズ0: 準備

### 0-1. 名前

- [x] 言語名は「Zevic」で確定(GitHub の Org は `zevic-lang`)

### 0-2. リポジトリ

- [ ] GitHub Organization(非公開)を作る
- [ ] `.github`、`docs`、`zevic`、`examples` の4つを作る
- [ ] `docs` に仕様書(`spec/zevic-spec.md`)と、`proposals/0000-template.md` を置く
- [ ] `examples` に、サンプル01〜10を置く(草案と明記)
- [ ] `LICENSE`(Apache-2.0)、`README.md` を置く

### 0-3. 開発環境

- [ ] clang 15 以上を入れる(`clang --version`)。Windows は Visual Studio Build Tools も
- [ ] `make`、`git`、C コンパイラ(clang 15 以上)を入れる
- [ ] まず1つの OS に絞る(いちばん慣れているもの)。他の OS は段階⑥で CI に足す
- [ ] Boehm GC のソースを取得し、ライセンスを確認する(同梱するのは段階⑤)

### 0-4. 最初の CI

- [ ] GitHub Actions で、Linux のビルドとテストが通る小さな CI を作る(まだ中身はほぼ空でよい)

**完了**: 非公開リポジトリが4つあり、CI が緑。名前は確定している。

---

## フェーズ1: 手書き LLVM IR の実験

コンパイラを書く前に、**コンパイラが出力すべき形**を手で書いて確かめる。これが以後の設計の見本になる。

### 1-1. 最小の `.ll`

```
; hello.ll
@.str = private constant [14 x i8] c"Hello, World!\00"
declare i32 @puts(ptr)
define i32 @main() {
  %r = call i32 @puts(ptr @.str)
  ret i32 0
}
```

- [ ] `clang hello.ll -o hello` で動く(clang 15 以上、opaque pointer の `ptr`)
- [ ] Windows でも動くか確認する(`.exe` とリンク)

### 1-2. ランタイムを C で書いて、`.ll` から呼ぶ

- [ ] `runtime/zevic_rt.c`: `zevic_alloc`、`zevic_panic`、`zevic_print_str`
- [ ] `.ll` から `declare` して呼び、`clang a.ll zevic_rt.c -o a` で結合する

### 1-3. 型の表現を試す

仕様書の「生成コードの規則」の表を、手書きで確かめる。

- [ ] `String` = `{ ptr, i64 }`(末尾に NUL)
- [ ] `I32` の足し算とオーバーフロー検査(`llvm.sadd.with.overflow.i32`)→ 起きたら `zevic_panic`
- [ ] `struct` = LLVM の構造体。`class` = GC ヒープ上の構造体への `ptr`
- [ ] `enum` = タグ + 最大のバリアントの領域
- [ ] `interface` の値 = `{ ptr, ptr }`(オブジェクトと関数表)
- [ ] 関数値(クロージャ) = `{ ptr 関数, ptr 環境 }`

**成果物**: `docs/design/ir-notes.md`(上の確認結果)。**完了**: すべて手書きで動く。

---

## 段階①: Hello World

**目標**: 次のプログラムをビルドして動かす。

```
module Main

import Zevic.Console

fn Main() {
    Console.Println("Hello, World!")
}
```

### やること

| 部品 | 内容 |
| --- | --- |
| `lexer.c` | UTF-8、識別子、予約語、整数・文字列リテラル、コメント、改行の扱い |
| `parser.c` | `module` `import` `fn`、呼び出し式、文字列リテラル。再帰下降パーサ |
| `ast.c` | AST の定義とダンプ(`--dump-ast`) |
| `resolve.c` | `module` / `import` の解決。`Zevic.` の省略を含む。標準ライブラリの探索 |
| `check.c`(骨組み) | 型の表現、関数の宣言と呼び出しの検査 |
| `emit_ll.c` | 関数、文字列定数、呼び出しを `.ll` に出す |
| `driver.c` | `clang` を探して呼ぶ。`-o` `--emit-ll` |
| `runtime/` | `zevic_alloc`(`malloc`)、`zevic_panic`、`Console.Print`/`Println` |
| `main` の生成 | C の `main(int, char**)` を生成し、`Main` を呼んで終了コードを返す |

### エラーメッセージ

- ファイル名、行、列、問題の行、原因の一文。最初から入れる(後から足すより安い)

### テスト

- [ ] ゴールデンテスト: `tests/NN-name/input.zev` と `expected_output.txt` と `expected_exit_code`
- [ ] `tests/run.py` が全テストを実行して、差分を表示する
- [ ] 異常系: `module` が無い、`Main` が無い、複数ある、未定義の名前、構文エラー

**完了**: `zevic hello.zev -o hello` が動く。CI(Linux)で通る。**v0.0.1**。

---

## 段階②: 計算と制御

**目標**: FizzBuzz、階乗、素数判定のような、数値と制御だけのプログラム。

### 言語機能

- [ ] 整数型 `I8`〜`I64` `U8`〜`U64`、`F32` `F64`、`Bool`、`Char`、`Unit`
- [ ] リテラル(`1_000`、`0xFF`、`0b1010`、`1e3`、`255_U8`、文字、エスケープ)
- [ ] `let` / `mut`、代入、`+=` など
- [ ] 演算子と優先順位(仕様書の表どおり)、比較、論理、ビット演算
- [ ] **オーバーフロー検査**(加減乗、シフト、キャスト)、0除算、`Panic`(終了コード101)
- [ ] `if` / `else`(式)、`while`、`loop`、`break`、`continue`、`for (i in 0..n)`
- [ ] 関数(引数、戻り値、`return`、再帰)
- [ ] `Panic` `Assert`
- [ ] 文字列補間 `"${x}"`(基本型だけ)、`Console.Printf`(型・個数のコンパイル時検査)
- [ ] 型変換(`ToI64` `ToF64`、`Result` を返す狭い方向は段階③で)

### 検査

- [ ] 暗黙変換の禁止、型の不一致、`let` への再代入、未初期化、戻り値の型、到達不能
- [ ] すべての分岐が値を返すか(式としての `if`)

### 実装上の要点

- 型検査と IR 生成を分ける(`check.c` が型付き AST を作り、`emit_ll.c` はそれだけを見る)
- 中間表現は、型付き AST のまま使う。独自の IR は、必要になるまで作らない

**完了**: FizzBuzz、階乗、素数のテストが通る。**v0.0.2**。

---

## 段階③: データ

**目標**: データ構造を扱うプログラム(簡単な電卓、単語カウント前半)。

### 言語機能

- [ ] **ジェネリクスの最小版**(型引数、推論、**単相化**。制約はまだない)。標準ライブラリの `Option` `Result` `Array` を書くために、この段階で入れる
- [ ] タプル、タプルの分解 `let (a, b): ...`
- [ ] `struct`(`Init` `New`、`this`、メソッド、`==`、値のコピー)
- [ ] `enum`(3種のバリアント、メソッド)、`match`(網羅性検査、ガード、範囲、タプル・構造体パターン)
- [ ] `Option` `Result`: **標準ライブラリの `enum`**(`Zevic.Core`)。`?`、`Context` `MapErr` `OkOr`、`Error` と `ErrorKind`
- [ ] `String`: UTF-8 検証、`Len` `CharCount`、`+`、比較、`ToI32` など
- [ ] `Array<T>`: **標準ライブラリの型**(`Zevic.Array`)。リテラル、添字、範囲外 Panic、`Push` `Pop` など主要メソッド
- [ ] `for (x in arr)`、`for (i, x in arr)`
- [ ] `mut fn` と `let` の深い不変性の検査(`let` の配列に `Push` できない)
- [ ] 公開・非公開(大文字・小文字)の検査

### 標準ライブラリの土台(最初から標準ライブラリに置く)

`Array` と `Map` は、最初から**標準ライブラリの型**として書く(コンパイラの組み込みにしない)。ただし Zevic にはポインタの読み書きがないので、土台として、**標準ライブラリ専用のモジュール `Zevic.Intrinsics`** をコンパイラが提供する。

- [ ] `Intrinsics.Buf<T>`: 長さ固定の領域。`New(cap)` `Get(i)` `Set(i, v)` `Copy(from, to, n)` を、コンパイラが IR に直接展開する(一般の利用者は `import` できない)
- [ ] `Zevic.Array<T>` は、`Buf<T>` を持つ普通の `class` として Zevic で書く(伸長は倍々、範囲検査は `Array` 側)
- [ ] `String` は、非ジェネリックなので、ランタイムの C(`zevic_str_*`)を `extern "C"` で呼ぶ薄い型として書く
- [ ] 組み込みとして特別扱いするのは、リテラル `[1, 2, 3]` の**構文**(`Array.New` と `Push` の呼び出しに展開)と、`for` の展開だけ

### 実装上の要点

- `enum` の `match` は、決定木ではなく、**単純な `switch` + 順番の照合**でよい(最適化は後)
- `Result` の `?` は、早期 `return` に展開する(`defer` が入る段階⑤で、展開の順序を再確認)

**完了**: `examples/03`、`06` の一部と、`Array<T>` を使うサンプルが動く。**v0.0.3**。

---

## 段階④: クラスとC連携

**目標**: 外部の C ライブラリを呼ぶ、オブジェクト指向のプログラム。**ここで公開する(v0.1.0)**。

### 言語機能

- [ ] `class`(参照型、フィールドの初期値、`Init`、`New`、`static`)
- [ ] `interface`(メソッド、デフォルト実装、実装の検査、動的ディスパッチの関数表)
- [ ] 標準 `interface`: `Equatable` `Hashable` `Comparable` `Iterable` `Iterator`
- [ ] `for` で自作の `Iterable` を回す
- [ ] **ジェネリクスの制約**(`<K: Hashable>`、`+` でつなぐ)と、制約を満たすかの検査。組み込み型(整数、`String` など)には、`Hashable` `Equatable` `Comparable` をコンパイラが用意する
- [ ] `Map<K, V>`: **標準ライブラリの型**(`Zevic.Map`)として Zevic で書く(ハッシュ表。挿入順を保つ。`Intrinsics.Buf` の上)。`Set<T>` も同じ
- [ ] **C 連携**: `extern "C"`、`export "C"`、C型の表、変換メソッド、ポインタ、可変長引数 `...`、`extern struct`
- [ ] `Handle<T>`、`NewString`、`Free`(`Zevic.C`)
- [ ] `native/` と、`.c` `.cpp` `.o` `.a` などの結合(拡張子の表)
- [ ] `export "C"` があるときの `zevic.h` の生成

### ツール

- [ ] `zevic` のオプション一式(`-l` `-L` `-I` `-D` `--cc` `--cxx`)
- [ ] clang の探索順、見つからないときの案内、バージョンの検査

### 公開の準備

- [x] 名前は「Zevic」で確定している
- [ ] `README.md`(特徴、ビルド方法、Hello World)、`CONTRIBUTING.md`、行動規範、連絡先の記入
- [ ] `examples` のうち、動くものは動くと明記。動かないものは「予定」と書く
- [ ] リポジトリと Organization を**公開**する

**完了**: C ライブラリ(例: SQLite、zlib)を呼ぶサンプルが動く。**v0.1.0 を公開**。

---

## 段階⑤: 抽象化

**目標**: 標準ライブラリを Zevic 自身で書けるだけの表現力と、GC。

### 言語機能

- [ ] **単相化の強化**(同じ具体型の組を1回だけ生成、名前の組み立て、生成量の測定)。ジェネリクス本体は段階③・④で入っている
- [ ] **クロージャ**(捕捉、共有、環境の確保)、関数値、ラムダ、高階メソッド(`Map` `Filter` `Reduce` ...)
- [ ] `defer`(`return` `?` `break` の経路すべてで実行。Panic では実行されない)
- [ ] `params`(可変長引数)
- [ ] 配列・`Map` リテラルの、左辺の型による決定

### GC

- [ ] Boehm GC のソースを `runtime/third_party/bgc/` に同梱し、ビルドに加える
- [ ] `zevic_alloc` / `zevic_alloc_atomic` を GC へ差し替える(呼び出し側は変えない)
- [ ] `main` の最初で `GC_init`
- [ ] 負荷テスト(大量の確保、長時間のループ)で、メモリが増え続けないか確認する
- [ ] C との境界の規則(`Handle`、呼び出しの間だけ有効なポインタ)のテスト

### 実装上の要点

- `Array` `Map` は段階③・④から標準ライブラリにあるので、GC の差し替え(`zevic_alloc` の中身を変えるだけ)が、既存のテストを壊さないことを確かめる
- 単相化は**名前の衝突**と**コード量の増加**が問題になる。生成する関数名にハッシュを入れる。同じ具体型の組は1回だけ生成する
- 再帰する型(`enum` が自分を持つ)は `class` か `Array` を通す規則を、検査で強制する

**完了**: クロージャを使う `Map` `Filter` `Reduce`、`Cache<K, V>` が動く。GC 付きで長時間動く。**v0.2.0**。

---

## 段階⑥: ツール

**目標**: 普段の開発が `zevpm` で完結する。標準ライブラリ第2段まで。

### `zevpm`

- [x] `new` `init` `build` `run` `test` `clean`
- [x] `zevic.toml` の読み込み(`[package]` `[deps]` `[native]`)
- [x] `build/` への出力、`kind = "lib"` のときの静的ライブラリと `include/<name>.h`
- [x] `get` / `remove` / `update`(`git clone --depth 1`、`~/.zevpm` のキャッシュ、`zevic.lock`)
- [x] 依存の依存の取得、`native/` の自動ビルド
- [x] `zevpm` 自身の実装: 最初は Python か C で書き、セルフホスティング(段階⑦)で Zevic に置き換える

### テスト機能

- [x] `test "名前" { ... }` ブロックと `_test.zev`
- [x] `Zevic.Test`(`Equal` `NotEqual` `True` `False` `Some` `None` `Ok` `Err` `Panics` `Fail`)
- [x] ランナー(`setjmp` / `longjmp` で、テスト内の Panic をそのテストの失敗にする)
- [x] 結果の表示、終了コード、名前での絞り込み

### 標準ライブラリ第2段

- [x] `Zevic.Fs`: `ReadAll` `WriteAll` `Open` `Exists` `List` `Remove`(すべて `Result`、`errno` → `ErrorKind`)
- [x] `Zevic.Text`: `Split` `Join` `Trim` `Replace` `StartsWith` `EndsWith` ほか
- [x] `Zevic.Math`: 数学関数、定数
- [x] `Zevic.Time`: `Now` `Duration` `Sleep`
- [x] `Zevic.Env`: `Get` `Set` `ProgramName` `Exit` `RawArgs`
- [x] `Zevic.Process`: 外部コマンドの起動(`Run(cmd, args)` で終了コードと出力を `Result` で返す)。セルフホスティング(段階⑦)で `clang` と `git` を呼ぶために必要なので、第3段から**第2段へ前倒し**した
- [x] OS の違い(Windows / POSIX)は同梱の `.c` で吸収

### 3つの OS

- [ ] CI を Linux / macOS / Windows の3つで通す(Linux のみ確認済み)
- [ ] パスの区切り、改行コード、`.exe` の扱い、clang の探索(仕様書の順)(Windows / macOS は未確認)

**完了**: `zevpm new` → `zevpm run` → `zevpm test` が3つの OS で動く。サンプル01〜10がすべて動く。**v0.3.0**。

---

## 段階⑦: セルフホスティング

**目標**: Zevic で書いたコンパイラ(`zevic1`)が、自分自身をビルドできる。

### 7-1. 前提の確認

- [ ] 段階①〜⑥の機能で、コンパイラが書けるか(必要な標準ライブラリ: 文字列、配列、`Map`、ファイル、プロセス起動 `Zevic.Process`)
- [ ] 足りないものは、先に標準ライブラリへ足す(`Zevic.Process` は段階⑥で入っている)
- [ ] 仕様に足りない機能が見つかったら、**提案(`proposals/`)を書いてから**入れる

### 7-2. 書き直す順序

`zevic0`(C)の構成をそのまま、同じ順で Zevic に移す。

1. `lexer` → 2. `ast` → 3. `parser` → 4. `resolve` → 5. `check` → 6. `mono` → 7. `lower` → 8. `emit_ll` → 9. `driver`

- 各部を移すたびに、`zevic0` で**Zevic 版をコンパイルして**、C 版と**同じ出力(AST ダンプ、`.ll`)**になることを確認する(差分テスト)
- 最後に、全体を結合する

### 7-3. ブートストラップ

```
zevic0 (C製) ─コンパイル→ zevic1 (Zevic製のコンパイラ)
zevic1       ─コンパイル→ zevic2
zevic2       ─コンパイル→ zevic3
```

- [x] `zevic1` と `zevic2` が出力する `.ll` が**完全に一致**する(一致したらセルフホスティング成功)
- [x] 最終的な `.ll` を `bootstrap/zevic.ll` として**コミットする**。C がなくても、`clang bootstrap/zevic.ll runtime/*.c` だけでコンパイラを作れる
- [x] CI で、毎回ブートストラップを検査する

### 7-4. 残りのツール

- [x] `zevfmt`(`tools/zevfmt/`。コメントと改行を保つ。冪等、AST不変をテスト。構文チェックは `zevic --dump-ast` でコンパイラのパーサを使い回す)
- [x] `zevpm` を Zevic で書き直す(`tools/zevpm/`。C 版 `zevpm/` は凍結版として残し、同じ結合テストを両方に通す)
- [ ] `zevic0`(C版)は残すが、**新機能は Zevic 版にだけ入れる**。C版は、ブートストラップのための凍結版にする

### 7-5. 言語と標準ライブラリの追加(提案が先)

- [x] ループのラベルと `break@ラベル` `continue@ラベル`(提案 0001)
- [x] 非同期(`async` `await`、`Zevic.Async`)(提案 0002)
- [x] HTTP(`Zevic.Http`)(提案 0003。HTTPSは未対応。Windows(Winsock)は未検証)

**完了**: `zevic2` の `.ll` が一致し、CI が通る。**v0.4.0**。

---

## 仕上げ(v1.0 に向けて)

### 言語

- [ ] 仕様書の凍結。未決事項のうち、必要なものを決める(並行処理、`Integer` など)
- [ ] 後方互換の方針を書く(1.x では、既存のコードを壊す変更をしない)
- [ ] 標準ライブラリ第3段(`Net` `Thread` `Async` `Json` `Random`。`Process` は段階⑥へ移した)。並行処理は、GC との相性(Boehm のスレッド対応)を確認してから

### ツールと品質

- [ ] エラーメッセージの改善(候補の提示、複数エラーの表示)
- [ ] コンパイル時間と生成コードの測定(ベンチマークを `tests/bench/` に)
- [ ] `-g` のデバッグ情報(DWARF)。デバッガで追えること
- [ ] ファズ(字句・構文解析に、ランダムな入力を食わせて、クラッシュしないこと)
- [ ] 言語サーバ(LSP)とエディタ拡張は、1.0 のあとに検討

### 配布

- [ ] リリースの作り方: 3つの OS の実行ファイル(`zevic` `zevpm` `zevfmt`)と、`runtime/`、標準ライブラリを1つにまとめる
- [ ] インストーラ(スクリプト)。`winget` `brew` などのパッケージの検討
- [ ] 公式サイトとチュートリアル(`docs/tutorial/`)、言語ツアー
- [ ] 提案(`proposals/`)の運用が回っているか確認

---

## リポジトリ構成(最終形)

```
zevic/
  zevic0/                 C製コンパイラ(凍結版)
  compiler/               Zevic製コンパイラ(セルフホスト版)
  runtime/
    zevic_rt.c  ...       ランタイム
    third_party/bgc/      Boehm GC
  packages/Zevic/         標準ライブラリ(Zevic.*)。zevic.toml + src/
  tools/
    zevpm/  zevfmt/
  bootstrap/zevic.ll      コミットしておく .ll
  tests/
    golden/               ゴールデンテスト
    bench/                ベンチマーク
    run.py
  Makefile
  LICENSE  README.md  CHANGELOG.md

docs/
  spec/zevic-spec.md      仕様書
  proposals/              言語の変更提案
  design/                 設計メモ(ir-notes.md など)
  tutorial/

examples/                 サンプル(01〜10、apps/)
.github/                  Organization共通のファイル
```

---

## リスクと対策

| リスク | 対策 |
| --- | --- |
| LLVM IR の仕様変更(clang の版差) | clang 15 以上に固定。CI で複数の版を試す。ポインタは常に `ptr` |
| 単相化でコードが肥大化する | 同じ具体型の組は1回だけ生成。最適化(`-O1`)に頼る。測定して判断 |
| Boehm GC と C の境界でのバグ | 境界の規則を仕様に書いた。`Handle` を使う。境界専用のテストを作る |
| Windows 固有の問題(パス、MSVC のライブラリ) | 段階①から Windows で動かす。CI に早めに入れる |
| `Array` `Map` を標準ライブラリに置くため、ジェネリクスが段階③から要る | 段階③は制約なしの最小版だけ入れる。制約は段階④(`Map` と同時) |
| 機能が増えて終わらない | 仕様書の「将来の検討」は、v1.0 まで入れない。各段階の範囲を守る |
| 一人で続けられなくなる | 小さく区切って、毎段階でタグを打つ。サンプルとテストで進捗が見えるようにする |
| セルフホスティングで仕様の穴が見つかる | 見つかったら提案を書いて入れる。コンパイラ自体が最大のテストになる |

---

## すぐに始めること(最初の1週間)

1. 非公開リポジトリ4つの作成、`.github` と仕様書の配置(0-2)
2. clang の確認(0-3)
3. 手書きの `hello.ll` を動かす(1-1)
4. `lexer.c` の着手(段階①の最初)
