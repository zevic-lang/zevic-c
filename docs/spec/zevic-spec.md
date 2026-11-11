# Zevic 言語仕様

Oct 27, 2026 · @荒木知也。

## 概要と設計方針

Zevicは、Cの資産をそのまま使える、小さくて書きやすいネイティブ言語です。本書は言語仕様で、設計の決定をすべて記します。

### 目標

- Cのライブラリとソースを、橋渡しなしで呼べる(`extern "C"`、`zevic app.zev b.c`)
- C#に近い読みやすさ(`class`、`interface`、`enum`と`match`、補間文字列)
- 覚えることが少ない。同じことを書く方法は、なるべく1つにする
- LLVM IRを出力し、`clang`で機械語にする。リンカとクロスコンパイルは`clang`に任せる
- 将来、Zevic自身でコンパイラを書く(セルフホスト)
- `zevic`(コンパイラ)、`zevpm`(パッケージ管理)、`zevfmt`(整形)で、ビルドから整形までそろえる

### 非目標

- OSやカーネルを書くための低レベル制御(`unsafe`、手動メモリ、`NoGC`モードは、将来の検討に退避した)
- 継承(`interface`と合成だけを使う)
- マクロ(可変長引数は`params`で足りる。必要になればセルフホストの後で検討する)
- 関数のオーバーロード、デフォルト引数、名前付き引数
- 演算子のオーバーロード(`==`と順序の比較を除く)

### 設計の原則

- 型は省略しない(変数、引数、フィールド、戻り値)。暗黙の型変換はしない
- 大文字始まりは公開、小文字始まりは非公開
- `this`は省略できない。`if` `while` `for` `match`の条件は括弧で囲み、`{}`も必須
- `if`と`match`は式で、値を返せる
- 失敗が普通の処理は`Result`と`?`、プログラムのバグは`Panic`で扱う
- 「ない」は`null`ではなく`Option`で表す
- メモリは自動で管理する(GC)

### 名前

- 言語名は「Zevic」(ゼヴィック)、拡張子は`.zev`。確定した名前
- 名前に由来するもの: コマンド名(`zevic` `zevpm` `zevfmt`)、標準ライブラリのルート(`Zevic.*`)、`zevic.toml`、環境変数(`ZEVIC_HOME`など)、GitHubのOrg(`zevic-lang`)

### 本書の読み方

- コード例では、固定幅の型名(`I32` `I64` `F64`など)を使う
- 「決定」と書いたものは確定で、「将来」「未決」と書いたものは確定していない。未決の一覧は「未決事項と将来の検討」の章にある
- 実装順の番号(①〜⑦)は「実装計画」の章を参照

## 字句構造

ソースはUTF-8のテキストで、トークンは識別子、予約語、リテラル、記号です。トークンは空白とコメントで区切ります。

### 文字コードと空白

- UTF-8。BOMは無視する。改行は`LF`と`CRLF`のどちらも読め、`zevfmt`が`LF`にそろえる
- 空白(スペース、タブ)はトークンの区切りで、インデントに意味はない

### 改行と文の区切り

改行は文の終わりです。`;`を使うと、1行に複数の文を書けます。次の場合は、改行しても文が続きます。

- 行末が二項演算子(`+ - * / % & | ^ << >> && || == != < > <= >=`)、`,`、`(` `[` `{`、`=`、`=>`、`->`、`.`のとき
- 次の行の先頭が`.`、`&&`、`||`のとき
- `( )`と`[ ]`の中(改行は常に無視する)

次の行が`-`や`(`で始まるときは、続きとは見なしません(前の行の次の行にある`-b`を、引き算と取り違えないため)。

```
let total: I64 = nums
    .Filter((x: I32) => x > 0)
    .Map((x: I32) => x.ToI64())
    .Sum()

if (a > 0 &&
    b > 0) { ... }

let a: I32 = 1; let b: I32 = 2
```

### コメント

```
// 行コメント
/* ブロックコメント /* 入れ子にできる */ */
/// ドキュメントコメント(次の宣言に付く。将来のzevdoc用)
```

コメントは字句解析で捨てず、構文木に残します(`zevfmt`がコメントを消さないため)。

### 識別子

- ASCIIの英数字と`_`。先頭は英字か`_`。大文字と小文字を区別する
- 公開の名前は`UpperCamelCase`、非公開の名前、ローカル変数、引数は`lowerCamelCase`
- `_`だけは、パターンの「何でもよい」を表す
- 予約語を識別子として使いたいときは、バッククォートで囲む(`` `type` ``)。Cのヘッダに出てくる`type`や`in`といった名前を扱うために必要

### 予約語(30個)

識別子には使えません。小文字ばかりで、大文字なのは`Self`だけです。

| 分類 | 語 |
| --- | --- |
| 宣言 | `module` `import` `as` `fn` `let` `mut` |
| 型 | `class` `struct` `enum` `interface` `static` `Self` |
| C連携 | `extern` `export` |
| 制御 | `if` `else` `while` `loop` `for` `in` `match` `return` `break` `continue` `defer` |
| 非同期 | `async` `await` |
| 値 | `true` `false` `this` |

`extern`と`export`の後ろの`"C"`は、キーワードではなく文字列リテラルです。

### 文脈キーワード

その位置でだけ意味を持ち、識別子にも使えます。

- `params`: 引数の宣言の先頭にあるとき
- `test`: モジュールの最上位で、文字列リテラルの直前にあるとき

### 将来のために予約する語

今は使えません。`macro` `unsafe` `type` `where` `const` `yield` `impl` `null`。

### 予約された名前

キーワードではありませんが、モジュールの最上位で同じ名前を定義するとエラーになります。

- プレリュード: `I8` `I16` `I32` `I64` `U8` `U16` `U32` `U64` `F32` `F64` `Bool` `Char` `String` `Unit` `Array` `Map` `Option` `Result` `Error` `ErrorKind` `StringBuilder` `Some` `None` `Ok` `Err` `Panic` `Same`
- 特別なメソッド名: `Init`(コンストラクタ)、`New`(コンパイラが生成する関数。自分で定義するとエラー)

### リテラル

**整数**

```
123    1_000_000    0xFF    0b1010    0o17
255_U8    10_I64         // 型を指定するときだけ、_ と型名を付ける
```

- `_`は数字の間にだけ書ける(先頭、末尾、連続はエラー)。接頭辞は小文字のみ
- `017`のように0から始まる8進数は、エラー
- 型は文脈で決まる(`let x: U8 = 200`)。文脈がなければ`I64`。範囲を超えたらコンパイルエラー
- 負の数は前置の`-`と整数の組。`let x: I8 = -128`のように、範囲の端まで書ける

**小数**

```
1.5    2.0    1e10    1.5e-3    1_000.5    1.5_F32
```

- `.`の両側に数字が必要(`1.`と`.5`はエラー)。指数があれば、小数点がなくても小数
- 型は文脈で決まり、なければ`F64`。`NaN`と無限大は`F64.NaN`と`F64.Infinity`
- 数字の後ろの`.`は、その次に数字が続くときだけ小数点。`1.ToString()`はメソッド呼び出し、`1..10`は範囲

**真偽、Unit**

```
true    false    ()          // () が Unit の値
```

**文字とバイト**

```
'a'    '\n'    '\u{1F600}'    'あ'
b'a'    b'\n'    b'\xFF'      // U8 のバイト(ASCIIと \xNN)
```

- `Char`はUnicodeの1文字(`'ab'`はエラー)
- `b'...'`は`U8`。ソースを走査するとき(`s.ByteAt(i) == b'a'`)やC連携に使う

**文字列**

```
"hello"    "tab\there"    "quote: \""    "x = ${x}"    "price: \${1}"
```

- 使える`\`: `\n` `\r` `\t` `\\` `\"` `\'` `\0` `\$` `\u{...}`と、`\xNN`(00〜7Fのみ。文字列は常に正しいUTF-8)
- 補間`${式}`の中に、文字列を書いてよい(`"a ${f("b")} c"`)。補間に書式指定は付けない(桁指定は`Printf`)
- 引用符の中で改行は書けない(閉じ忘れを早く見つけるため)

**複数行の文字列と生の文字列**

```
let text: String = """
    Hello,
      ${name}!
    """

r"C:\temp\new"          // エスケープも補間もなし
r"""
複数行の生の文字列
"""
```

- `"""`の直後は改行。閉じる`"""`の行のインデントが、全行から取り除かれる。最後の改行は含まない
- `"""`の中では、エスケープと補間が使える。`r`が付くと、どちらも無効

**コレクションとタプル**

```
[1, 2, 3]            // 配列(空は let a: Array<I32> = [])
{"a": 1, "b": 2}      // Map(空は Map.New())
(1, "x")              // タプル
```

- 配列リテラルの要素の型は、**左辺の型**で決める。`let a: Array<Shape> = [Shape.Circle(1.0), Shape.Empty]`は、要素を`Shape`として読む(`interface`型も同じ)
- 左辺の型がないときは、要素がすべて同じ型でなければエラー。空の`[]`は、左辺の型が必要
- `Map`のリテラルも同じで、キーと値の型は左辺の型で決める

### 記号

`. .. ..= , ; : => -> ? ! ~ & | ^ << >> + - * / % = == != < > <= >= && || ( ) [ ] { }`と、文字列の中だけの`${`。`@`はループのラベルにだけ使います([提案 0001](../proposals/0001-loop-labels.md))。`#` `::`は今は使いません。

## 型

Zevicの型は、固定幅の数値型、`Bool`、`Char`、`String`、`Unit`と、`Array` `Map` `Option` `Result`、タプル、ユーザー定義型(`struct` `class` `enum`)です。幅が決まらない`Int`や`Float`という名前の型はありません。

### 基本型

| 型 | 内容 |
| --- | --- |
| `I8` `I16` `I32` `I64` | 符号つき整数 |
| `U8` `U16` `U32` `U64` | 符号なし整数(`U8`がバイト) |
| `F32` `F64` | IEEE 754の浮動小数点 |
| `Bool` | `true` / `false` |
| `Char` | Unicodeのスカラー値(32bit) |
| `String` | UTF-8の不変の文字列 |
| `Unit` | 値は`()`のみ。「値がない」ことを表す |

- 整数の計算で範囲を超えたら`Panic`(ラップしない)。整数の0除算も`Panic`
- 型が書かれない整数リテラルは、文脈の型に合わせる。文脈がなければ`I64`、小数は`F64`
- 任意精度の整数(`Integer`)は、今はない

### 複合型

- **`Array<T>`**: 可変長の配列。参照型(GC管理)。要素が`struct`や数値のときは、要素を並べて持つ
- **`Map<K, V>`**: ハッシュ表。反復は挿入順。キーは`Hashable`を実装した型(最初の実装では`I32`などの整数、`Char`、`Bool`、`String`のみ)
- **`Option<T>`**: `Some(T)`か`None`。`null`の代わり
- **`Result<T, E>`**: `Ok(T)`か`Err(E)`。標準では`E`に`Error`を使う
- **タプル**: `(I32, String)`。要素の取り出しは分解だけ(`let (a, b): (I32, String) = t`)
- **関数型**: `fn(I32) -> I32`
- **ユニオン型**: `I32 | String`。名前のない、いくつかの型のどれか1つ(次の「ユニオン型」の節)
- **`Self`**: `interface`と型の定義の中で、その型自身を指す

`Option`と`Result`は、標準ライブラリの`enum`です(「型の定義」の章)。

### 型の書き方と推論

- 変数、引数、フィールド、戻り値の型は、必ず書く(戻り値が`Unit`のときだけ、`-> Unit`を省略できる)
- ジェネリクスの型引数は、左辺の型や引数の型から決まる(`let s: Stack<I32> = Stack.New()`)。決まらなければエラー
- 型が決まらない整数・小数リテラルは、他の制約が決まった後で、`I64`と`F64`になる
- ラムダの引数の型は書く。戻り値の型は、本体の式から決まる

### 型の変換

暗黙の変換はありません。`ToXxx`というメソッドで明示します。

| 変換 | 戻り値 |
| --- | --- |
| 常に収まる(`I32`→`I64`、`I32`→`F64`、`U8`→`I32`) | 値 |
| 収まらないことがある(`I64`→`I32`、`I32`→`U8`、`String`→`I32`) | `Result<T, Error>` |
| 範囲を切り詰める版(`ToI32Wrap()`など) | 値 |
| `F64`→整数(`ToI32()` `ToI64()` `Floor()` `Ceil()` `Round()`) | 値(小数部は切り捨て。`NaN`と範囲外は`Panic`) |
| すべての型の`ToString()` | `String` |

- `String`から数値への変換が失敗すると、`ErrorKind.InvalidFormat`(書式が違う)か`ErrorKind.OutOfRange`(範囲外)の`Error`を返す
- 整数型どうしの演算は、同じ型どうしだけ(`I32 + I64`はエラー)
- C型への変換は「C/C++連携」の章

### ユニオン型

`A | B | C`は、メンバーのうちどれか1つの値を持つ型です(Kotlinの`Result`風のエラー処理や、「値か失敗」を軽く書くための型)。`enum`と違って宣言せず、メンバーはそのままの型です。

```
fn Parse(s: String) -> I32 | ParseError {
    if (s == "1") { return 1 }          // メンバーの値は、そのまま渡せる
    ParseError.New("cannot parse ${s}")
}

let r: I32 | ParseError = Parse("x")
if (r is ParseError) {
    Console.Println(r.Msg)              // ここでは r は ParseError
} else {
    Console.Println("${r + 1}")         // ここでは r は I32
}
match (r) {
    n: I32 => Console.Println("ok ${n}"),
    e: ParseError => Console.Println(e.Msg),
}
```

- **同じ型**: メンバーの並びと入れ子は関係ない。`(A | B) | C`は`A | B | C`、`B | A`は`A | B`と同じ型。同じ型を2回書いても1つ。型の名前は、メンバーを名前順に並べて`|`で結ぶ(エラーメッセージもこの順)。メンバーが1つになる場合はエラー。`Never`はメンバーにできない
- **暗黙の変換**(言語で唯一の暗黙の変換): メンバーの値は、そのメンバーを含むユニオン型に入る。ユニオン型の値は、メンバーがすべて入っている、より大きなユニオン型に入る(`I32 | String`→`I32 | String | Bool`)。逆(ユニオンからメンバー)は、`is`や`match`で絞り込む
- 数値リテラルは、期待されるユニオンに整数(小数)のメンバーが1つだけあれば、その型になる。2つ以上あるときは`I64`(`F64`)になるので、それがメンバーでなければエラー
- **`is`と`!is`**: `x is T`は、ユニオン型の値`x`が`T`かどうか(`Bool`)。`T`はメンバーでなければエラー。優先順位は比較(`<` など)と同じで、連ねて書けない
- **絞り込み(スマートキャスト)**: `mut`でないローカル変数(引数を含む)`x`は、`x is T`が決まった範囲で、`T`(または残りのメンバーのユニオン)として使える。範囲は次のとおり
  - `if (x is T) { ... } else { ... }`の、それぞれの枝(`!is`なら逆)
  - `&&`の右側(`x is T && x.F`)と、`||`の右側(`x !is T || x.F`)。`!`や`&&` `||`で結んだ条件も、決まる範囲で使われる
  - `if (x is T) { return ... }`のように、`else`のない`if`の本体が`return` `break` `continue`で終わるとき、その後ろのブロックの残り
  - `mut`の変数、フィールド、式は絞り込まれない(途中で変わりうるため)。いったん`let`で受ける
- **`match`**: パターンに`名前: 型`(または`_: 型`)を書くと、その型のメンバーに一致し、名前に束縛する。型パターンはユニオン型の値に対してだけ使える。すべてのメンバーを(または`_`で)網羅していなければコンパイルエラー
- **`if`と`match`の分岐**: 期待されるユニオン型があれば(`let`の型、戻り値の型、引数)、各分岐の値がユニオンに変換される。期待がなければ、分岐の型は同じでなければならない
- 比較(`==`)と`${}`への埋め込みは、ユニオン型の値にはできない(`is`や`match`で絞り込んでから使う)
- ジェネリクスで`T | Error`のように使える。型引数は、ほかの引数や戻り値から決まらなければ書く(`Pass<I32>(5)`)
- 実装: `enum`と同じ形(タグと、最大のメンバーが入る大きさの領域)で持つ。値型で、コピーされる

### 等価性と順序

`==`は、その型が決める値としての等しさです。

| 型 | `==`の意味 |
| --- | --- |
| 整数、`Bool`、`Char` | 値の比較 |
| `F32` `F64` | IEEEに従う(`NaN == NaN`は偽) |
| `String` | 中身の比較 |
| `struct` `enum` タプル `Option` `Result` | 自動で、フィールドやバリアントごとの比較(中身が`==`できるとき) |
| `Array<T>` `Map<K, V>` | 要素ごとの比較(要素が`==`できるとき) |
| `class` | 既定は参照の比較。`Equatable`を実装していれば`Equals`を呼ぶ |

- `!=`は`==`の否定。型が違うものどうしの`==`はエラー
- 参照が同じかは、プレリュードの関数`Same(a, b)`で調べる
- `< > <= >=`は、整数・`Char`・`String`(バイト順)・`F32` `F64`と、`Comparable`を実装した型で使える。比較は結合しない(`a < b < c`はエラー)
- `struct`と`enum`は、`Equatable`と`Hashable`を自動で持つ(中身が持てるとき)。`Comparable`は手で書く
- 整数・`Char`・`Bool`・`String`は`Equatable` `Hashable` `Comparable`を実装済み。`F32` `F64`は`Equatable`と`Comparable`だけ(`NaN`は最後に並べる)で、`Hashable`ではない

## 変数・式・演算子

変数は`let`(不変)と`mut`(可変)の2つで宣言し、型と初期値を必ず書きます。

### 変数

```
let x: I32 = 10      // 不変
mut y: F64 = 2.5     // 可変
```

- 型と初期値は必須。初期値のない宣言はない
- `let`の変数は、再代入できず、**中身の変更もできない**。`Array`や`Map`への`Push`・`Set`、`class`の`mut fn`の呼び出し、フィールドへの代入は、エラーになる
- `mut`の配列の要素のフィールドには、代入できる(`xs[i].Count += 1`)。`class`の要素はその場で変わり、`struct`の要素は取り出して変更し、配列に書き戻される。そのため`struct`の要素では、配列と添字は単純な式(変数・フィールド・数)に限る。`let`の配列では、エラー(提案0004)
- 変更するなら`mut`で宣言する

```
mut words: Array<String> = ["a"]
words.Push("b")             // OK

let ws: Array<String> = ["a"]
ws.Push("b")                // エラー: let の中身は変えられない
```

- 関数の引数は`let`と同じで、変更できない。変更したいなら`mut`を付ける(`fn Add(mut a: Array<I32>)`)。`mut`の引数には、`mut`の変数だけを渡せる
- `struct`などの値型はコピーされるので、この規則は参照型(`class` `Array` `Map` `StringBuilder`)に効く
- モジュールの最上位では、`let`(定数)だけ書ける。`mut`は書けない(グローバルな可変状態を禁止する)
- 同じスコープで、同じ名前を2回宣言するとエラー。内側のスコープで外側と同じ名前を宣言できるか(シャドーイング)は、未決

### ブロックと式

- `{ ... }`はブロックで、最後の式が値になる。値がなければ`Unit`
- `if`と`match`は式で、値を返せる。全分岐が同じ型で、`else`のない`if`は`Unit`
- 代入は式ではなく文。`a = b = c`や`if (a = b)`は書けない
- `return` `break` `continue`と`Panic`は、どの式の位置にも書ける(`None => return`のように使う)

### 演算子の優先順位

高いものから並べます。

| 順位 | 演算子 | 結合 |
| --- | --- | --- |
| 1 | `.`(メンバー)、`( )`(呼び出し)、`[ ]`(添字)、`?` | 左から |
| 2 | 前置の`-` `!` `~` `await` | 右から |
| 3 | `*` `/` `%` | 左から |
| 4 | `+` `-` | 左から |
| 5 | `<<` `>>` | 左から |
| 6 | `&` | 左から |
| 7 | `^` | 左から |
| 8 | ビット和(縦線1本) | 左から |
| 9 | `==` `!=` `<` `>` `<=` `>=` | 結合なし |
| 10 | `&&` | 左から |
| 11 | 論理和(縦線2本) | 左から |
| 12 | `..` `..=` | 結合なし |
| 13 | `=>`(ラムダ) | 本体は右へできるだけ長く |

- ビット演算(`& | ^ ~ << >>`)は、整数型だけで使える。Cと違い、比較より強く結びつく(`a & b == c`は`(a & b) == c`)
- 後置(`.` `?` `[]`)は前置の`-` `!`より強い。`-x.Abs()`は`-(x.Abs())`で、負数に呼ぶときは`(-5).Abs()`と書く
- 比較と範囲は結合しない。`a < b < c`、`a == b == c`、`1..2..3`はエラー。`0..n - 1`は`0..(n - 1)`
- `&&`と`||`は短絡評価。`!`は`Bool`だけに使える
- `+`は、同じ型の数値どうし、または`String`どうし

### 整数と小数の演算

- 整数の範囲超過は`Panic`(ラップしない)。ラップさせたいときは、`WrapAdd(y)`のような専用のメソッドを使う(「標準ライブラリ第1段」の章)
- 整数の`/`は、0に向かって切り捨てる。`%`の符号は左辺に従う(Cと同じ)。0除算は`Panic`
- シフト量は`I32`。負数や型の幅以上は`Panic`
- 小数はIEEE 754に従う。`1.0 / 0.0`は無限大で、`Panic`にはしない
- 三項演算子、`++`、`--`はない(`if`が式で、`x += 1`を使う)

### 代入

```
x = 1
a[i] = 2
obj.Name = "x"
x += 1          // += -= *= /= %= &= |= ^= <<= >>= が使える
```

左辺は、`mut`の変数、`mut`の変数のフィールド、`mut`の配列の要素です。

## 制御構文

制御構文は`if` `while` `loop` `for` `match`で、条件は必ず括弧で囲み、本体の`{}`は省略できません。`if`と`match`は式で、値を返せます。

### if

```
if (x > 0) {
    Zevic.Console.Println("正")
} else if (x == 0) {
    Zevic.Console.Println("ゼロ")
} else {０
    Zevic.Console.Println("負")
}

let label: String = if (x > 0) { "正" } else { "非正" }
```

- 条件は`Bool`だけ(`if (1)`はエラー)
- 式として使うとき、全分岐は同じ型で、`else`が必要。`else`のない`if`の型は`Unit`

### while と loop

```
while (i < 10) {
    i += 1
}

loop {
    if (done) { break }
}
```

- `loop`は無限ループで、`break`で抜ける。`break`は値を持たない
- `break`と`continue`は、一番内側のループに効く
- ループには`名前@`でラベルを付けられる。`break@名前`と`continue@名前`は、そのラベルのループに効く(Kotlinと同じ形)

```
outer@ for (row in rows) {
    for (x in row) {
        if (x < 0) { continue@outer }
        if (x == target) { break@outer }
    }
}
```

  - ラベルを付けられるのは`while` `loop` `for`だけ。`名前@`はループと同じ行に、`@`を名前にくっつけて書く
  - `break@名前`は、`break`と`@`と名前の間に空白を入れない
  - ラベルが指すのは、同じ関数の中で、その位置を囲んでいるループ。ラムダは越えられない
  - 存在しないラベル、囲んでいるループと同じ名前のラベルは、エラー
  - 抜けるとき、間にある`defer`は内側から順に実行する

### for

```
for (i in 0..10) { ... }          // 0〜9
for (i in 0..=10) { ... }         // 0〜10
for (x in arr) { ... }
for (i, x in arr) { ... }         // 配列は (番号, 要素)
for (k, v in map) { ... }         // Map は (キー, 値)。挿入順
for ((a, b) in pairs) { ... }     // 要素がタプルのとき
```

- C式の`for (init; cond; step)`はない
- ループ変数は`let`と同じで、変更できない
- 2変数の形は、自作の型でも(番号, 要素)。`Map`だけが(キー, 値)の例外。要素がタプルなら、括弧つきの`((a, b) in obj)`で分解する
- `0..n`の型は、`n`の型に合わせる(`n: I32`なら`I32`)。どちらも定数なら`I64`
- `for (x in obj)`は、次のように展開される(`Iterable`の規則は「型の定義」の章)

```
mut it = obj.Iter()
loop {
    match (it.Next()) {
        Some(x) => { /* 本体 */ },
        None => break,
    }
}
```

### match

```
let kind: String = match (n) {
    0 => "ゼロ",
    1..=9 => "一桁",
    10 | 20 | 30 => "きりのよい数",
    n if (n > 100) => "大",
    _ => "それ以外",
}
```

- 網羅していなければコンパイルエラー。`_`で、残りをまとめられる。到達しないパターンには警告を出す
- 分岐の値は、全部同じ型
- **パターン**として書けるもの:
  - リテラル(`0`、`"a"`、`'x'`、`true`)と範囲(`1..=9`)
  - `_`(何でもよい)と、変数への束縛(`n`)
  - タプル(`(a, b)`)
  - `enum`のバリアント(位置引数の`Shape.Circle(r)`、名前付きの`Shape.Named { Size: s, .. }`、`..`は残りを無視)
  - 入れ子(`Ok(Some(v))`)
  - `|`で並べた複数の候補(`0 | 1`)
  - ユニオン型のメンバー(`n: I32`、`_: Error`。「ユニオン型」の節)
  - ガード(`n if (n > 100)`)。ガードの条件も括弧で囲む
- 全体への別名(`all @ Shape.Circle(_)`)は、将来の検討
- 修飾なしで書けるバリアントは`Some` `None` `Ok` `Err`だけ。自作の`enum`は、`Shape.Circle`のように型名で修飾する

### return、break、continue、Panic

```
let line: String = match (Zevic.Console.Prompt("年齢: ")) {
    Some(l) => l,
    None => return,
}
```

- `return` `break` `continue`と`Panic`は、どの式の位置にも書ける(利用者には`Never`という型は見えない)
- `return`は、関数から抜ける。`Unit`を返す関数では、値なしの`return`と書く
- 関数の本体の最後の式が、戻り値になる(`return`は省略できる)

### defer

```
fn Load(path: String) -> Result<String, Error> {
    let f: Zevic.Fs.File = Zevic.Fs.Open(path, "r").Context("opening ${path}")?
    defer f.Close()
    f.ReadAll()
}
```

- `defer 文`か`defer { ... }`で、そのブロックを抜けるときに実行する(関数単位ではなく、ブロック単位)
- 複数あれば、後に書いたものから順に実行する
- 通常の終了、`return`、`break`、`continue`、`?`による早期リターンのどれでも実行する。`Panic`と`Zevic.Env.Exit`では実行しない
- 戻り値の式は、先に評価してから`defer`が走る(上の例では、`ReadAll()`の結果が決まった後に`Close()`)
- `defer`の中の変数は、実行時点の値を見る(引数を`defer`の時点で固定しない)
- `defer`の中に`return` `break` `continue` `?`は書けない。`Result`を返す呼び出しの結果は、捨てられる
- 実装は、コンパイラがブロックの出口ごとに、`defer`したコードをIRに複製して挿入する

### 非同期(async、await)

```
import Zevic.Async

let a: Async.Task<I64> = async { slow(3) }     // 別のスレッドですぐに走り始める
let b: Async.Task<I64> = async { slow(4) }
let total: I64 = await a + await b              // 終わりを待って、結果を取り出す
```

- `async { ... }`は式で、型は`Async.Task<T>`(`T`はブロックの値の型)。`Async.Spawn(() => { ... })`と同じ
- `await e`は前置の演算子で、`!`と同じ強さ。`e.Await()`と同じ。`Task`は何回`await`してもよく、同じ値が返る
- 関数に`async`は付けない。非同期の関数は、`Task<T>`を返す普通の関数にする
- ブロックの中の`return` `break` `?`は、ラムダと同じで、ブロックを抜けるだけ
- スレッドの間でメモリは共有される。安全に渡すには、`Async.Channel<T>`(`Send` `Recv` `Close`)と`Async.Mutex`(`Lock` `Unlock`)を使う
- `async`ブロックの中で、外側の関数の`mut`の局所変数を使うと、エラー(競合するため)。ただしクラスの値は、参照として共有できる
- `Task`の中の`Panic`は、プログラム全体を止める。`main`が終わると、走っているタスクを待たずに終わる

### 乱数(Zevic.Random)

```
import Zevic.Random

mut rng: Random.Rng = Random.Rng.New(42)       // 同じ種なら、どの環境でも同じ数列
let dice: I64 = rng.Int(1, 7)                    // 1以上7未満
let p: F64 = rng.Float()                         // 0.0以上1.0未満
Random.Shuffle(rng, xs)                          // 並べ替え
let one: Option<I64> = Random.Pick(rng, xs)      // 1つ選ぶ(空ならNone)
```

- 種を決めない生成器は`Random.Rng.Seeded()`(時計から作る)。メソッドは`NextU64` `Int(low, high)` `Float` `Bool`
- アルゴリズムはSplitMix64。**暗号には使えない**。生成器はスレッドの間で共有せず、タスクごとに作る。詳しくは`docs/proposals/0005-random.md`

### JSON(Zevic.Json)

```
import Zevic.Json

let v: Json.Value = Json.Parse("{\"name\": \"zevic\", \"tags\": [1, 2]}")?
let name: String = v.Get("name").UnwrapOr(Json.Value.Null).AsString().UnwrapOr("?")
Console.Println(Json.Stringify(v))      // {"name":"zevic","tags":[1,2]}
Console.Println(Json.Pretty(v))         // 字下げあり
```

- `Json.Value`は`Null` `Bool(Bool)` `Number(F64)` `Str(String)` `List(Array<Value>)` `Object(Map<String, Value>)`の列挙型。取り出しは`AsBool` `AsF64` `AsI64` `AsString` `AsArray` `AsObject`(種類が違えば`None`)、`Get(キー)`と`At(添字)`
- `Parse`のエラーは`ErrorKind.Parse`で、行と桁を含む。厳密にRFC 8259(末尾のカンマやコメントはエラー)。入れ子は512段まで
- オブジェクトはキーの順番を保つ。数は`F64`(2^53までの整数は正確)。詳しくは`docs/proposals/0006-json.md`

### HTTP(Zevic.Http)

```
import Zevic.Http

let r: Http.Response = Http.Get("http://127.0.0.1:8080/hello")?     // Post(url, body)、Send(request) もある
Console.Println("${r.Status} ${r.Body}")                              // 404や500は Err ではなく Status で見る

let server: Http.Server = Http.Listen("127.0.0.1", 0)?                // ポート0: 空いているポートを選ぶ
let task: Async.Task<Result<Unit, Error>> = async {
    server.Serve((req: Http.Request) => Http.Response.Text(200, "hello ${req.Path()}"))
}
server.Close()                                                        // Serve を終わらせる
```

- `http://`だけ。`https://`は`Err`(TLSは未対応)。リダイレクトは追わない。接続は1回ごとに閉じる
- 本文は`Content-Length`、`chunked`、接続が閉じるまで、に対応。上限16MiB、待ち時間30秒
- `Serve`は、接続ごとに`async`のタスクを1つ作る。詳しくは`docs/proposals/0003-http.md`

## 関数とラムダ

関数は`fn`で宣言し、引数と戻り値の型を必ず書きます。同じ名前の関数は、1つのモジュールに1つだけです(オーバーロードなし)。

### 関数の宣言

```
fn Add(a: I32, b: I32) -> I32 {
    a + b
}

fn Greet(name: String) {                 // 戻り値が Unit のときだけ、-> Unit を省略できる
    Zevic.Console.Println("Hello, ${name}")
}

fn Main() -> Unit { ... }                // 書いてもよい
```

- 本体の最後の式が、戻り値になる。途中で抜けるときは`return 値`(`Unit`なら`return`だけ)
- 引数は変更できない。変更するなら`mut`を付ける(`fn Fill(mut a: Array<I32>)`)
- 関数の中に、関数を宣言する構文はない(ラムダを使う)
- 再帰は使える。宣言の順番は関係なく、モジュールの中のどこからでも呼べる

### 可変長の引数(params)

最後の引数1つだけを`params`にでき、呼ぶ側は値を並べて書き、受ける側は配列として受け取ります。

```
fn Sum(params nums: Array<I32>) -> I32 {
    nums.Reduce(0, (acc: I32, x: I32) => acc + x)
}

Sum(1, 2, 3)          // nums は [1, 2, 3]
Sum()                 // nums は []
```

`params`は文脈キーワードで、引数宣言の先頭にあるときだけ意味を持ちます。

### オーバーロード、デフォルト引数、名前付き引数

3つとも、ありません。代わりに次の書き方を使います。

- 複数の作り方が欲しい: 静的な関数を別の名前で用意する(`Point.Origin()`、`Point.FromPolar(r, theta)`)
- 数の型ごとに同じ関数が欲しい: 型のメソッドにする(`x.Abs()`、`a.Min(b)`)
- 引数を省略したい: 別の名前の関数にするか、`params`か`Option`を使う

### 関数を値として使う

関数は値で、変数に入れたり、引数に渡したりできます。型は`fn(引数の型) -> 戻り値の型`です。

```
fn Show(x: I32) -> Unit { Zevic.Console.Println("${x}") }

let f: fn(I32) -> Unit = Show
nums.Each(Show)
```

### ラムダとクロージャ

```
let inc: fn(I32) -> I32 = (x: I32) => x + 1

nums.Each((x: I32) => Zevic.Console.Println("${x}"))

nums.Each((x: I32) => {              // 複数行は {} で書く。最後の式が値
    let y: I32 = x * 2
    Zevic.Console.Println("${y}")
})
```

- 引数の型は書く。戻り値の型は、本体の式から決まる
- ラムダの中の`return` `break` `?`は、ラムダを抜けるだけで、外側の関数は抜けない。途中で外側を抜けたいときは、`for`を使う
- 外側の`mut`の変数を捕捉でき、変更も外側に見える(共有する)。捕捉した変数はGCが管理する
- 本体は`=>`の右へ、できるだけ長く続く

### ジェネリクス関数

```
fn First<T>(a: Array<T>) -> Option<T> {
    a.Get(0)
}

fn Max<T: Comparable>(a: T, b: T) -> T {
    if (a.Compare(b) >= 0) { a } else { b }
}
```

- 型引数は、引数の型や左辺の型から決まる。`First<I32>(a)`と明示してもよい
- 型引数の制約は`<T: Hashable>`と書く。`where`は用意しない。制約が入るのは実装順⑤(`interface`と同時)で、それまでは、組み込み型だけが対象
- 実装は、使われた型ごとに別の関数を作る(単相化)

### 関数の名前

- 公開の関数は`UpperCamelCase`、非公開は`lowerCamelCase`
- 他のモジュールの関数は、`import`したパス(または別名)で修飾して呼ぶ(`Zevic.Console.Println`、`U.Add`)

## 型の定義

独自の型は`struct`(値型)、`class`(参照型)、`enum`(データ付きの選択肢)で定義し、共通の約束を`interface`で表します。継承はなく、コードの共有は`interface`のデフォルト実装と、フィールドに持つ合成で行います。

### class と struct

```
class Person {
    name: String                  // 小文字 = 非公開(モジュール内)
    Age: I32                      // 大文字 = 公開
    Tags: Array<String> = []      // フィールドの初期値

    fn Init(name: String, age: I32) {
        this.name = name
        this.Age = age
    }

    fn Name() -> String { this.name }
    mut fn Birthday() { this.Age += 1 }
    static fn Anonymous() -> Person { Person.New("?", 0) }
}

mut p: Person = Person.New("Taro", 20)
p.Birthday()
```

|  | `class` | `struct` |
| --- | --- | --- |
| 種類 | 参照型(GCで管理) | 値型(代入でコピー) |
| 生成 | `Type.New(...)` | `Type.New(...)` |
| `interface`の実装 | できる | できる(`interface`型の値にすると、ヒープに置かれる) |
| `==`の既定 | 参照の比較 | フィールドごとの比較 |

**規則**

- フィールドは`名前: 型`と書き、`= 式`で初期値を付けられる。大文字始まりは公開、小文字始まりは非公開(同じモジュールの中でだけ見える)
- メソッドは`fn`。**`this`は省略できない**(`this.name`)
- \*\*`mut fn`\*\*は、`this`のフィールドを変更できる。`mut`の変数からだけ呼べる。普通の`fn`の中では、`this`の中身を変更できない(深く不変)
- **`static fn`**と**`static`フィールド**は、`Person.Anonymous()`のように型名で呼ぶ。`static`フィールドは定数で、代入できない(可変のグローバルな状態は持てない)
- クラスの外から公開フィールドへ代入するには、変数が`mut`であること
- 継承はない。別の型の機能を使いたいときは、その型をフィールドに持ち、必要なメソッドを転送して書く

```
class Labeled {
    Inner: Circle
    Label: String

    fn Init(inner: Circle, label: String) {
        this.Inner = inner
        this.Label = label
    }
    fn Area() -> F64 { this.Inner.Area() }     // 転送は手で書く
}
```

### 生成(Init と New)

- コンストラクタは`Init`。生成は`Type.New(...)`で、コンパイラが作る静的関数
- `New`は、領域を確保し、フィールドの初期値を入れ、`Init(...)`を呼び、参照(または値)を返す。`New`は必ず成功する
- `Init`は直接呼べない(`p.Init(...)`はエラー)。戻り値はない。`Init`は暗黙に`mut fn`
- `Init`を抜けた時点で、すべてのフィールドが初期化されていること(初期値のあるフィールドは、省略してよい)
- `Init`を書かないクラスにも`New`は作られる。そのときは、**すべてのフィールドに初期値が必要**
- `Init`は1つだけ(オーバーロードなし)。複数の作り方は、静的関数にする(`Point.Origin()`)
- 失敗しうる生成(ファイルを開く、解析する)は`New`にせず、`Result`を返す静的関数にする
- `struct`にも`New`がある。`let p: Point = Point.New(1, 2)`
- Cの構造体と同じ配置にしたい`extern struct`は「C/C++連携」の章

### enum

```
enum Shape {
    Circle(F64),
    Rect(F64, F64),
    Named { Name: String, Size: F64 },
    Empty,

    fn Area() -> F64 {
        match (this) {
            Shape.Circle(r) => 3.14 * r * r,
            Shape.Rect(w, h) => w * h,
            Shape.Named { Size: s, .. } => s * s,
            Shape.Empty => 0.0,
        }
    }
}

let s: Shape = Shape.Circle(2.0)
```

- バリアントは3種類: データなし(`Empty`)、位置引数(`Circle(F64)`)、名前付き(`Named { ... }`)
- 名前付きバリアントの値は、パターンと同じ形で作る: `Shape.Named { Name: "x", Size: 3.0 }`(全フィールドが必要で、順序は自由。式は宣言の順に評価する)。位置引数のバリアントを`{ }`で、名前付きのバリアントを`( )`で作るとエラー(2026-10-31に追記)
- 値型。`==`と`Hashable`は、中身が持てるときに自動で付く
- バリアントは`Shape.Circle`のように型名で修飾する。`Some` `None` `Ok` `Err`だけは、修飾なしで書ける
- メソッドと`interface`の実装を書ける。継承はなし
- `enum`が自分自身を持つ(再帰する)ときは、`class`か`Array`のフィールドを通して持つ
- `Option`と`Result`は、標準ライブラリがこの仕組みで定義している

```
enum Option<T> { Some(T), None }
enum Result<T, E> { Ok(T), Err(E) }
```

### interface

```
interface Shape {
    fn Area() -> F64
    fn Describe() -> String {            // デフォルト実装
        "area = ${this.Area()}"
    }
}

class Circle : Shape {
    r: F64
    fn Init(r: F64) { this.r = r }
    fn Area() -> F64 { 3.14 * this.r * this.r }
}

let s: Shape = Circle.New(2.0)
```

- 実装する`interface`は、`class Dog : Hashable, Printable`のように、`:`の後にカンマで並べる(`struct`も同じ)
- `interface`にはデフォルト実装を書ける。2つの`interface`のデフォルトが衝突したら、型の側で書き直すエラー
- `interface`は別の`interface`を拡張できる(`interface Hashable : Equatable`)。要求を足すだけで、状態は持たない
- `interface`型の値は、(オブジェクトへの参照, 関数表)の組で持ち、呼び出しは動的に解決される
- `Self`を使う`interface`(`Equals(other: Self)`)は、**ジェネリクスの制約専用**で、値の型としては使えない
- 実装するメソッドの戻り値は、`interface`の宣言より具体的な型にしてよい(`Iter()`が具体的な`ListIter<T>`を返す、など)
- ジェネリクスなメソッドは、`interface`の関数表に入れられない。デフォルト実装としては書ける(使われた型ごとに作られる)

### 標準の interface

```
interface Equatable { fn Equals(other: Self) -> Bool }
interface Hashable : Equatable { fn Hash() -> U64 }
interface Comparable : Equatable { fn Compare(other: Self) -> I32 }   // 負 / 0 / 正

interface Iterator<T> { mut fn Next() -> Option<T> }
interface Iterable<T> {
    fn Iter() -> Iterator<T>
    // 以下はデフォルト実装(Iter() だけ書けば使える)
    fn ToArray() -> Array<T>
    fn Each(f: fn(T) -> Unit)
    fn EachIndexed(f: fn(I64, T) -> Unit)
    fn Map<U>(f: fn(T) -> U) -> Array<U>
    fn Filter(f: fn(T) -> Bool) -> Array<T>
    fn Reduce<U>(init: U, f: fn(U, T) -> U) -> U
    fn Find(f: fn(T) -> Bool) -> Option<T>
    fn Any(f: fn(T) -> Bool) -> Bool
    fn All(f: fn(T) -> Bool) -> Bool
    fn Count(f: fn(T) -> Bool) -> I64
}
```

- `Map`と`Filter`は配列を返す(遅延評価にはしない)
- `Array` `Map` 範囲(`0..n`)は、`Iterable`を実装済み
- すべての型に`ToString()`がある。プリミティブは組み込み、`struct`と`enum`と`class`は、型名とフィールドを出す既定のものが作られる。自分で`fn ToString() -> String`を書けば、そちらが使われる(`"${x}"`は`x.ToString()`を呼ぶ)

### ジェネリクス

```
class Stack<T> {
    items: Array<T> = []
    mut fn Push(x: T) { this.items.Push(x) }
    mut fn Pop() -> Option<T> { this.items.Pop() }
}

class Cache<K: Hashable, V> { ... }
fn Largest<T: Comparable + Hashable>(a: Array<T>) -> Option<T> { ... }
```

- `struct` `class` `enum` `interface`と関数に、型引数を付けられる
- 制約は`<K: Hashable>`と書き、複数なら`+`でつなぐ。`where`は用意しない
- 型引数は、使われた型ごとに別のコードを作る(単相化)
- 制約が入るのは、実装順⑤(`interface`と同時)。それまで`Map`のキーは組み込み型だけ

### 破棄

デストラクタとファイナライザは、利用者には見せません。ファイルなどの後始末は、`defer`で行います。

## モジュールとimport

コードは**モジュール**に分けます。1ファイルは必ず1つの`module`宣言で始まります(省略はできません)。

```
module Main

import Zevic.Console
import MyApp.Util.Math as M

fn Main(args: Array<String>) {
    Console.Println("sum = ${M.Add(1, 2)}")        // Zevic.Console.Println とも書ける
}
```

### module

- 名前は`.`区切りのPascalCase(`MyApp.Util.Math`)。すべての要素が大文字始まり
- 同じ名前の`module`を宣言した複数のファイルは、1つのモジュールになる
- モジュール名は、パッケージ内のディレクトリ構造と一致させる(`src/Util/Math.zev` → `MyApp.Util.Math`)。実行ファイルの`src/Main.zev`だけは`module Main`
- 標準ライブラリは`Zevic.`で始まる(`Zevic.Console` `Zevic.Fs`)。`Zevic`は予約で、利用者のパッケージ名には使えない

### import

- `import`はフルパスで書く。ワイルドカード(`import X.*`)と、名前を一部だけ取り込む書き方はない
- `as`を付けないときは、使うときもフルパスで書く(`MyApp.Util.Math.Add(1, 2)`)
- **標準ライブラリは、先頭の`Zevic`を省略できる**。`import Zevic.Console`のあと、`Console.Println("hi")`とも、`Zevic.Console.Println("hi")`とも書ける(どちらでも同じ)。省略した名前が他の`import`や自分の名前と衝突するときはエラーになるので、フルパスか`as`を使う
- 短く書きたいときは`as`を使う(`import Zevic.Console as C` なら `C.Println("hi")`)
- `as`を付けたら、元の名前は使えなくなる(`C.Println`だけ)
- 別名の衝突はエラー。同じモジュールを2回`import`するのもエラー
- `import`はファイルの先頭の`module`の直後にまとめる
- **循環`import`は禁止**。依存の向きは一方向でなければならない(コンパイラは循環の経路を表示する)

### 公開範囲

| 名前の始まり | 見える範囲 |
| --- | --- |
| 大文字 | 公開(他のモジュールから使える) |
| 小文字 | 非公開(同じモジュールのファイルからだけ使える) |

- 関数・型・フィールド・メソッド・`static`フィールドのすべてに適用される
- 型が公開でも、小文字のフィールドは外から見えない
- `private`などのキーワードはない

### プレリュード

`import`なしで使える名前は次のものだけです。

- 型: `I8` `I16` `I32` `I64` `U8` `U16` `U32` `U64` `F32` `F64` `Bool` `Char` `String` `Unit` `Array` `Map` `Option` `Result` `Error`
- バリアント: `Some` `None` `Ok` `Err`
- 組み込み: `Panic` `Assert`
- 標準の`interface`: `Equatable` `Hashable` `Comparable` `Iterable` `Iterator`

これ以外(`Console`、`Math`、`Fs`など)は、`import`が必要です。

### エントリポイント

- 実行ファイル(`kind = "bin"`)は、`module Main`の公開関数`Main`から始まる。**ただ1つ**なければならない
- 許す形は、`fn Main()`と`fn Main(args: Array<String>)`の2つだけ
- `args`はコマンドライン引数。プログラム名は含まない(`args[0]`が最初の引数)
- 終了コードは`Env.Exit(code)`で返す(`import Zevic.Env`が必要)。`Main`が`Result`を返す形は、今は入れない
- ライブラリ(`kind = "lib"`)には`module Main`を置かない

### 探索の規則

1. `zevic.toml`のあるディレクトリがパッケージの根
2. 自分のパッケージのモジュールは、`src/`の下のディレクトリ構造から見つける
3. 依存パッケージ(`zevpm get`で取得)は、`zevic.toml`の`[deps]`にある名前で見つける。依存のパッケージ名が、モジュール名の先頭の要素になる
4. 標準ライブラリは、コンパイラと同梱の`Zevic`パッケージ(`packages/Zevic/`。`zevic.toml`と`src/`を持つ普通のzevpmパッケージ)。`zevic = ">=…"`の判定は、このパッケージのバージョンに対して行う

```
myapp/
  zevic.toml
  src/
    Main.zev          // module Main
    Util/Math.zev     // module MyApp.Util.Math
```

## エラー処理とPanic

エラーは2種類に分けます。**起こりうる失敗**は`Result`で返し、**プログラムのバグ**は`Panic`で止めます。例外はありません。

| 種類 | 例 | 扱い |
| --- | --- | --- |
| 起こりうる失敗 | ファイルがない、数字でない文字列、通信の切断 | `Result<T, Error>`を返す |
| バグ | 配列の範囲外、`Unwrap`の失敗、整数のオーバーフロー、0での除算 | `Panic`(プログラムを止める) |

### Result と ?

```
import Zevic.Fs

fn ReadPort(path: String) -> Result<U16, Error> {
    let text: String = Fs.ReadAll(path)?           // Err ならその場で関数から返す
    let n: I32 = text.Trim().ToI32()?
    if (n < 0 || n > 65535) {
        return Err(Error.New(ErrorKind.Invalid, "port out of range: ${n}"))
    }
    Ok(n.ToU16())
}
```

- `式?`は、`Ok(x)`なら`x`を取り出し、`Err(e)`ならその`e`で関数から`return`する
- `?`が使えるのは、戻り値が`Result<_, Error>`の関数の中だけ。エラー型は`Error`に固定なので、変換は要らない
- `Option`にも`?`は使えない。`OkOr`で`Result`にしてから使う
- 成功して値がないときは`Ok(())`を返す
- `Result`は無視できない。返り値を使わない呼び出しは警告(明示的に捨てるなら`let _: ... =`)

### Error

```
class Error {
    Kind: ErrorKind
    Message: String
    Cause: Option<Error>
}

enum ErrorKind { NotFound, Invalid, Permission, Io, Parse, Timeout, Other }
```

| メソッド | 内容 |
| --- | --- |
| `Error.New(kind, message)` | 作る |
| `e.Kind()` / `e.Message()` / `e.Cause()` | 読む |
| `r.Context(msg)` | `Err`のとき、新しい`Error`で包み、元を`Cause`に残す |
| `r.MapErr(f)` | `Err`の中身を変換する |
| `o.OkOr(e)` | `Option`を`Result`にする(`None`なら`Err(e)`) |
| `r.Unwrap()` / `r.UnwrapOr(d)` | `Err`ならPanic / 既定値 |

- `ErrorKind`は分類だけで、細かい情報はメッセージに書く
- `Error`は自作の型を入れられない。分類が足りないときは`Other`にして、メッセージで区別する
- `ToString()`は、`kind: message`と、原因があれば`caused by: ...`を続けて出す

### Panic

```
Panic("unreachable state")        // どの型の式の位置にも書ける
Assert(x > 0, "x must be positive")
```

- `Panic(msg)`は、メッセージ・ファイル名・行番号を標準エラー出力に書いて、**終了コード101**で終了する。回復はできない
- 戻り値の型は`Never`(式として、どの型にもなれる)
- `Assert(cond, msg)`は、`cond`が`false`のときに`Panic`する。リリースビルドでも消えない
- Panicが起きる組み込みの操作:
  - `a[i]`の範囲外、`m[k]`でキーがない
  - `Unwrap()`が`None` / `Err`に当たる
  - 整数の演算がオーバーフロー、`0`での除算・剰余、シフト量が幅以上
  - `ToXxx()`のうち、範囲外で`Result`を返さない種類(型の章の変換表を参照)
- オーバーフローしてほしい演算には、`WrapAdd` `WrapSub` `WrapMul`(折り返し)を使う
- `defer`は、Panicの前には**実行されない**(プログラムが即座に止まる)。後始末が要る処理は、OSが片付ける前提で書く
- C側から呼ばれた`export "C"`関数の中でPanicしたときも、そのままプロセスを終了する(C側へは戻らない)

### 終了コード

| 終了コード | 意味 |
| --- | --- |
| 0 | `Main`が普通に戻った |
| 101 | `Panic` |
| 任意 | `Env.Exit(code)` |

### テストとの関係

`test`ブロックの中の`Panic`は、そのテストを失敗にして、次のテストへ進む(テストの章を参照)。

## メモリ管理

ZevicはGC(ガベージコレクション)でメモリを管理します。利用者は解放を書きません。低レベルのメモリ制御は、この言語の目標ではありません。

### 方針

- 実装は、同梱(vendored)した**Boehm GC**を使う。保守的GCなので、コンパイラがスタックマップを作らなくてよい
- `class`、`Array`、`Map`、`String`、クロージャの環境は、すべてGCヒープに置く。`struct`と`enum`は値で、持ち主(変数、フィールド、配列の要素)の中に置く
- 確保は、ランタイムの関数に集約する。コンパイラが生成するコードは、これ以外で確保しない

| 関数 | 用途 |
| --- | --- |
| `zevic_alloc(size)` | ポインタを含みうる領域(`class`、`Array<class>`など) |
| `zevic_alloc_atomic(size)` | ポインタを含まない領域(`String`の中身、`Array<I32>`など)。GCが中身を調べない |

- 解放を呼ぶ手段はない。`Drop`や`Dispose`もない
- ファイルやソケットのような**OSの資源**は、`defer`で閉じる(GCが回収するタイミングには頼らない)

```
import Zevic.Fs

let f: Fs.File = Fs.Open(path)?
defer f.Close()
```

### 実装の段階

1. 最初のコンパイラ(`zevic0`)は、GCなしで動かす。`zevic_alloc`は`malloc`を呼ぶだけで、解放しない
2. 動くようになってから、`zevic_alloc`の中身をBoehm GCに差し替える。呼び出し側は変えない
3. ランタイムの初期化(`GC_init`)は、コンパイラが作る`main`の最初で呼ぶ

### スレッドとGC

- スレッド(`Zevic.Async`)は、Boehm GCのスレッド対応ビルドを使う。スレッドの作成は、ランタイムのラッパーを通す
- ランタイムの状態(エラー番号、プロセス起動の作業用の値、テストの`longjmp`先)は、スレッドごとに持つ

### 設定

- GCの調整(ヒープの上限、回収の頻度)は、環境変数で行う(`ZEVIC_GC_...`)。ソースコードからは触れない
- `NoGC`モードや`unsafe`は、今は仕様に入れない(「未決事項と将来の検討」を参照)

### Cとの境界

GCが管理するメモリをCに渡すときの規則は、「C/C++連携」の章の「GCとの境界」にまとめています。要点は次のとおりです。

- GCメモリのアドレスをCに渡して、呼び出しの間だけ使うのは安全(呼び出し中は、スタック上のポインタが領域を生かす)
- **Cが呼び出しの後まで保持する**ときは、GCが回収しないように`Handle`に入れる
- `malloc`した領域は、GCの管理外。`free`はCの側で行う

## C/C++連携

ZevicはC言語の関数を呼び、Zevicの関数をCに公開できます。C++は、`extern "C"`で包んだ関数を通して使います。ヘッダファイルは不要で、`extern`の宣言を書きます。

### 取り込み(extern)

```
import Zevic.Console

extern "C" {
    fn puts(s: CString) -> CInt
    fn abs(n: CInt) -> CInt
    fn getenv(name: CString) -> CString
}

fn Main() {
    let r: CInt = puts("hello".ToC())
    Console.Println("puts returned ${r.ToI32()}")
}
```

- `extern "C" { ... }`のブロックに、Cの関数を`fn`で宣言する。本体は書かない
- 宣言は**そのモジュールの中だけ**で使える。他のモジュールへ再公開はしない。公開したいときは、Zevicの関数で包む
- 名前はCの名前そのまま(マングルしない)。Cの名前を変えたいときは`fn Foo() -> CInt = "c_name"`の形で別名を付ける
- 呼び出しは、普通の関数と同じ。ただし引数と戻り値は**C型だけ**

### 公開(export)

```
import Zevic.Console

export "C" fn Greet(name: CString) -> CVoid {
    Console.Println("hello, ${String.FromC(name)}")
}
```

- `export "C"`を付けた関数は、Cから呼べる名前(マングルなし)で出る
- 引数と戻り値は、C型だけ
- ふつうの関数は、`zevic_<Module>_<Name>`の形に名前が変わり、Cからは直接呼ばない
- `Main`は`export`できない
- `export`した関数の中でPanicすると、プロセスが終了する(Cには戻らない)
- 同じ名前を2か所で`export`するとエラー

### C型

Cのすべての基本型に対応する型があります。名前は`C`で始まります。

| Zevicの型 | Cの型 | 備考 |
| --- | --- | --- |
| `CChar` / `CSChar` / `CUChar` | `char` / `signed char` / `unsigned char` | `CChar`は処理系の`char` |
| `CShort` / `CUShort` | `short` / `unsigned short` |  |
| `CInt` / `CUInt` | `int` / `unsigned int` |  |
| `CLong` / `CULong` | `long` / `unsigned long` | WindowsとLinuxで幅が違う |
| `CLongLong` / `CULongLong` | `long long` / `unsigned long long` |  |
| `CSizeT` / `CSSizeT` / `CPtrDiff` | `size_t` / `ssize_t` / `ptrdiff_t` |  |
| `CIntPtr` / `CUIntPtr` | `intptr_t` / `uintptr_t` |  |
| `CFloat` / `CDouble` | `float` / `double` |  |
| `CBool` | `_Bool` |  |
| `CString` | `const char*` | NUL終端 |
| `CVoid` | `void` | 戻り値だけに書ける |
| `*T` | `T*` | ポインタ |

- Cの型の幅は、そのプラットフォームのCコンパイラに合わせてコンパイラが決める
- `CChar`の符号は処理系によって違う。符号が必要なら`CSChar` `CUChar`を使う

### 変換(暗黙変換なし)

Zevicの型とC型は**自動で変換されません**。明示的に変換します。

| 変換 | 書き方 |
| --- | --- |
| `I32` → `CInt` | `x.ToCInt()` |
| `CInt` → `I32` | `c.ToI32()` |
| `I64` → `CLongLong` | `x.ToCLongLong()` |
| `CLongLong` → `I64` | `c.ToI64()` |
| `CLong` → `I64` | `c.ToI64()` |
| `CSizeT` → `U64` | `c.ToU64()`(幅が合わない環境では`Result`を返す版を使う) |
| `F64` → `CDouble` | `x.ToCDouble()` |
| `CDouble` → `F64` | `c.ToF64()` |
| `String` → `CString` | `s.ToC()` |
| `CString` → `String` | `String.FromC(c)` |

- `.ToC()`で作った`CString`は、**呼び出しの間だけ**有効。C側が保持するなら、`NewString`でGCの外に複製する
- `String.FromC(c)`は、`c`の中身を**コピー**して、UTF-8として確かめてから`String`にする。UTF-8でないときはPanicではなく、`String.FromCLossy(c)`(不正なバイトを`U+FFFD`にする)を使う
- 範囲外になる変換(`CLong` → `CInt`など)は、`Result`を返す`ToCInt()`と、折り返す`ToCIntWrap()`を用意する

### ポインタ

```
extern "C" {
    fn memcpy(dst: *CVoid, src: *CVoid, n: CSizeT) -> *CVoid
    fn qsort(base: *CVoid, n: CSizeT, size: CSizeT, cmp: fn(*CVoid, *CVoid) -> CInt) -> CVoid
}
```

- ポインタは`*T`と書く。`*CVoid`が`void*`
- ポインタは、**Cの関数に渡して、受け取ることだけ**ができる。`*p`で中身を読み書きする構文は、今は入れない
- ポインタの比較(`==`)と、`Null`との比較はできる。`Null`は、ポインタ型だけに使える定数
- `Array<T>`の先頭のアドレスは`a.Ptr()`で得る。この値は**呼び出しの間だけ**有効(GCが動かさないので位置は変わらないが、配列が伸びたときは古くなる)
- コールバックには、Zevicの関数(クロージャを捕捉しないもの)を`fn`型として渡せる。捕捉するラムダは渡せない

### 可変長引数

`printf`のような可変長引数のC関数を取り込むには、`...`を使います。

```
extern "C" {
    fn printf(fmt: CString, ...) -> CInt
}

printf("%d %s\n".ToC(), 42.ToCInt(), "x".ToC())
```

- `...`の部分に渡せるのは、C型だけ
- Zevicの`Printf`は別物で、書式の型と個数をコンパイル時に検査する(標準ライブラリの章)

### extern struct

Cの構造体と同じ並び(レイアウト)の型を宣言できます。

```
extern "C" {
    struct Point { X: CInt, Y: CInt }
    fn Move(p: *Point, dx: CInt) -> CVoid
}

mut p: Point = Point.New(1, 2)
Move(&p, 10.ToCInt())
```

- 並びとパディングは、Cコンパイラと同じ規則
- フィールドは、C型か、別の`extern struct`だけ
- `extern struct`は`struct`と同じ値型。`&p`でポインタを取れる(`mut`の変数だけ)
- `union`や、ビットフィールドは扱わない

### GCとの境界

GCは、**C側に渡したポインタが、Cのメモリの中に保存されたこと**を知りません。GCの管理下にあるメモリをCに保持させるときは、次の規則に従います。

| 場面 | 規則 |
| --- | --- |
| 呼び出しの間だけ使う | そのまま渡してよい |
| C側が呼び出しの後も保持する | `Handle`に入れて、Handleの番号(`CULong`)を渡す。使い終わったら`h.Release()` |
| C側が文字列を保持する | `NewString(s)`で、GCの外(`malloc`)に複製する。Cの側で`free`する |
| Cの`malloc`したメモリを受け取る | `CString`のまま扱い、`String.FromC`でコピーして、元は`free`する |

```
let h: Handle<Counter> = Handle.New(counter)    // GCが回収しないように登録
RegisterCallback(h.Id())                        // Cには番号を渡す
...
h.Release()                                     // 不要になったら外す
```

- `Handle<T>`は、`class`の参照を、番号で出し入れできる表に登録する。表にある間は、GCが回収しない
- C側から戻ってきた番号は`Handle.Get<T>(id)`で、`Option<T>`に戻す

### zevic.h

- `export "C"`した関数があるパッケージは、ビルド時に\*\*`zevic.h`\*\*を生成する(`build/include/<package>.h`)
- Cからは、そのヘッダを`#include`して使える
- ヘッダの生成が要らなければ、`zevic.toml`で`header = false`にする

### ビルドとリンク

```
zevic hello.zev native/hello.c -o hello
```

- コマンドラインに並べたファイルは、**拡張子**で扱いを決める(下の表)
- パッケージでは、`native/`の下のファイルが、自動でビルドに加わる
- `zevic.toml`の`[native]`に、リンクするライブラリと、検索のパスを書く

```
[native]
libs = ["m", "sqlite3"]
lib_dirs = ["/opt/lib"]
include_dirs = ["native/include"]
defines = ["FOO=1"]
```

### 拡張子と言語

| 拡張子 | 扱い |
| --- | --- |
| `.zev` | Zevicのソース |
| `.c` | C(`clang`でコンパイル) |
| `.cpp` `.cc` `.cxx` | C++(`clang++`でコンパイル) |
| `.m` `.mm` | Objective-C / C++(macOSのみ) |
| `.s` `.S` | アセンブリ(`clang`) |
| `.o` `.obj` `.a` `.lib` `.so` `.dylib` `.dll` | そのままリンク |

- C++のファイルは、`extern "C"`で包んだ関数だけを、Zevicから呼べる(C++の名前のマングルや、クラス・テンプレートは、直接は扱わない)
- C++を含むときは、リンクに`clang++`を使い、C++の標準ライブラリもリンクする
- 言語ごとの外部コンパイラは、`CC` `CXX`の環境変数、または`--cc` `--cxx`で指定できる
- 外部の言語の追加は、拡張子の表に1行足すだけで増やせる作りにする

### native/ ディレクトリ

```
myapp/
  zevic.toml
  src/
    Main.zev
    Sqlite.zev          // extern の宣言とラッパー
  native/
    wrapper.c
    wrapper.cpp
    include/wrapper.h
```

- `native/`にあるC/C++のファイルは、`zevpm build`のときに、それぞれ自動でコンパイルされる
- `native/`の下のサブディレクトリも、再帰的に探す
- ヘッダは、`native/include/`に置く(`include_dirs`の既定値)
- 同じ名前の`.c`と`.cpp`は、同じ`.o`になるので、名前は重ならないようにする

### 書き方の方針

- 外部の関数は、`extern`の宣言を**1つのモジュールに集めて**、Zevic側のラッパー関数で、`Result`やZevicの型にして公開する
- 他のモジュールには、C型を見せない(利用者は、C型を意識しなくて済む)
- `Zevic.C`のモジュールに、C型の変換と`Handle` `NewString` `Free`などの道具がある

## 標準ライブラリ 第1段

最初に実装するのは Core、Array、Map、Console、C の5つです。名前は `Zevic.Core` のように `Zevic.` で始まります。Core とプレリュードの型は `import` なしで使えます。

### Core

```
Panic(msg: String) -> Never
Assert(cond: Bool, msg: String)

// 整数(I8〜I64, U8〜U64)
x.ToString() -> String
x.ToF64() -> F64                 // 失敗しない変換
x.ToI64() -> I64                 // 広い型へは値を返す
x.ToI32() -> Result<I32, Error>  // 狭い型へは範囲外で Err
x.ToI32Wrap() -> I32             // 範囲外は折り返す
x.WrapAdd(y) / WrapSub(y) / WrapMul(y)   // 折り返す演算
x.Abs() -> 同じ型  x.Min(y)  x.Max(y)  x.Clamp(lo, hi)

// 浮動小数点(F32, F64)
x.ToI32() -> Result<I32, Error>  // 小数部は切り捨て。NaN と範囲外は Err
x.IsNaN() -> Bool  x.IsInf() -> Bool

// String(UTF-8、不変、位置はバイト単位)
s.Len() -> I64                   // バイト数
s.CharCount() -> I64             // 文字数
s.IsEmpty() -> Bool
s.ToI32() -> Result<I32, Error>  // ToI64, ToF64 なども同様
s + t,  s == t,  s < t

// Bool / Char
x.ToString() -> String

// Option<T>
o.IsSome() / o.IsNone() -> Bool
o.Unwrap() -> T                  // None なら Panic
o.UnwrapOr(d: T) -> T
o.OkOr(e: Error) -> Result<T, Error>

// Result<T, Error>
r.IsOk() / r.IsErr() -> Bool
r.Unwrap() -> T                  // Err なら Panic
r.UnwrapOr(d: T) -> T
r.Context(msg: String) -> Result<T, Error>
r.MapErr(f: fn(Error) -> Error) -> Result<T, Error>

// Error
Error.New(kind: ErrorKind, message: String) -> Error
e.Kind() / e.Message() / e.Cause()
```

- `Min` `Max` `Clamp` は `Comparable` な型すべてで使える
- 変換の規則は「型」の章を参照: 失敗しない変換は値を返し、失敗しうる変換は `Result` を返す
- `Wrap*` 系の名前は、WrapAdd・WrapSub・WrapMulで確定。オーバーフローしてもPanicしないときだけ使う

### Array

```
[1, 2, 3]                          // リテラル
Array<T>.New() -> Array<T>
a.Len() -> I64
a.IsEmpty() -> Bool
a[i] / a[i] = x                    // 範囲外は Panic
a.Get(i: I64) -> Option<T>
a.First() / a.Last() -> Option<T>

// 変更(mut の変数からだけ呼べる)
mut fn Push(x: T)
mut fn Pop() -> Option<T>
mut fn Insert(i: I64, x: T)        // 範囲外は Panic
mut fn RemoveAt(i: I64) -> T       // 範囲外は Panic
mut fn Clear()
mut fn Reverse()
mut fn Sort()                      // T: Comparable
mut fn Extend(other: Array<T>)

// 新しい配列を返す
a.Slice(from: I64, to: I64) -> Array<T>
a.Concat(other: Array<T>) -> Array<T>
a.Map<U>(f: fn(T) -> U) -> Array<U>
a.Filter(f: fn(T) -> Bool) -> Array<T>

// 検索・集計(Iterable の既定実装)
a.Contains(x: T) -> Bool           // T: Equatable
a.IndexOf(x: T) -> Option<I64>     // T: Equatable
a.Find / Any / All / Count / Reduce / Each / EachIndexed
a.Join(sep: String) -> String      // Array<String> のとき

a.Ptr() -> *T                      // C 連携用。呼び出しの間だけ有効
```

- 長さと位置は `I64`
- `Sort` は安定ソート。比較は `Compare`
- 範囲 `0..n` と `0..=n` は `Iterable<I64>` で、`for` に使える

### Map

```
Map<K, V>.New() -> Map<K, V>       // K: Hashable
m.Len() -> I64
m.Get(k: K) -> Option<V>
m[k]                               // なければ Panic
m.Contains(k: K) -> Bool
m.Keys() / m.Values() -> Array
mut fn Set(k: K, v: V)
mut fn Remove(k: K) -> Option<V>
mut fn Clear()
for ((k, v) in m) { ... }          // 挿入順
```

- 反復の順は、**挿入した順**
- 反復中にその `Map` を変更するのは Panic
- `Set<T>` も用意する(`Add` `Contains` `Remove` `Len`)

### Console

```
Print(s: String)
Println(s: String)
Printf(fmt: String, ...)           // %d %u %x %f %s %c %% と、幅・精度
Eprint / Eprintln / Eprintf        // 標準エラー出力

ReadLine() -> Option<String>       // 改行を除く。EOF は None
Read() -> Option<Char>             // 1文字。EOF は None
ReadAll() -> String
Prompt(msg: String) -> Option<String>   // 表示してから ReadLine
```

- 出力系は失敗しても何も起きない(`Unit` を返す)。入力の失敗は EOF として `None`
- `Printf` の書式文字列がリテラルなら、型と個数を**コンパイル時に検査**する。変数なら実行時に検査し、不一致は Panic
- `%d` は整数のどの幅でも受ける(型から決まる)
- `Text.Format(fmt, ...) -> String` は、`Printf` と同じ書式(`%d %u %o %x %X %f %e %g %s %c %%`、フラグ・幅・精度は C と同じ)で、出力せずに `String` を返す。書式はリテラルだけで、型と個数はコンパイル時に検査する
- 文字列補間 `"${x}"` は `x.ToString()` を呼ぶ

### C

```
型: CChar CInt CLong CSizeT CFloat CDouble CString CVoid ... (C/C++連携の表を参照)
x.ToCInt() / c.ToI32()             // 変換
s.ToC() -> CString
String.FromC(c: CString) -> String
String.FromCLossy(c: CString) -> String
NewString(s: String) -> CString    // malloc で複製(C 側で free)
Free(p: *CVoid)
Handle<T>.New(x: T) -> Handle<T>   // h.Id() / h.Release() / Handle.Get<T>(id)
```

## 標準ライブラリ 第2段・第3段

第1段が動いてから、第2段、第3段の順に実装します。ここでは各モジュールの役割と、主なAPIの方針を書きます。関数の細かい一覧は、実装の直前に決めます。

### 第2段

| モジュール | 役割 | 主なAPI(方針) |
| --- | --- | --- |
| `Zevic.Fs` | ファイルとディレクトリ | `ReadAll(path)` `WriteAll(path, s)` `Open(path, mode)` `Exists(path)` `List(dir)` `Remove(path)`。すべて`Result`を返す。`File`は`Close()`を`defer`で呼ぶ |
| `Zevic.Text` | 文字列の操作 | `Split` `Join` `Trim` `Replace` `StartsWith` `EndsWith` `Contains` `ToUpper` `ToLower` `Lines`。位置はバイト単位 |
| `Zevic.Math` | 数学 | `Sqrt` `Sin` `Cos` `Tan` `Pow` `Log` `Exp` `Floor` `Ceil` `Round`、定数`Pi` `E` |
| `Zevic.Time` | 時間 | `Now()` `Duration` `Sleep(d)`、経過時間の計測 |
| `Zevic.Env` | 環境 | `Get(name)` `Set(name, v)` `ProgramName()` `Exit(code)` `RawArgs()` |
| `Zevic.Process` | 外部プロセスの起動 | `Run(cmd, args)`で終了コードと出力を`Result`で返す。セルフホスティングで`clang`や`git`を呼ぶため、第2段に入れる |

- `Fs`は`extern "C"`でCランタイムを呼ぶ薄いラッパーとして作り、`errno`を`ErrorKind`に変換して`Result`にする
- OSの違い(WindowsとPOSIX)は、同梱する`.c`で吸収する
- `Env.Exit`は`Never`を返す

### 第3段

| モジュール | 役割 |
| --- | --- |
| `Zevic.Net` | TCPとUDP、HTTPクライアント |
| `Zevic.Thread` | スレッド、ミューテックス、チャネル |
| `Zevic.Async` | 非同期処理 |
| `Zevic.Json` | JSONの読み書き |
| `Zevic.Random` | 乱数 |
| `Zevic.Path` `Zevic.Bytes` `Zevic.Chars` | パス操作、バイト列(`Array<U8>`)、文字(`Char`)の分類と変換(v0.4.1) |
| `Zevic.Encoding` `Zevic.Digest` | 16進・Base64・URLエンコード、CRC-32・Adler-32・SHA-256・HMAC-SHA-256(v0.4.1) |
| `Zevic.DateTime` `Zevic.Log` `Zevic.Cli` | 日時(固定オフセット)、ログ、コマンドライン引数の解析(v0.4.1) |
| `Zevic.Regex` `Zevic.Csv` `Zevic.Toml` | 正規表現(Pike VM)、CSV(RFC 4180)、TOML 1.0(v0.4.1) |
| `Zevic.Deflate` `Zevic.Zlib` `Zevic.Gzip` `Zevic.Zip` `Zevic.Tar` | 圧縮と展開、zip・tarアーカイブの読み書き(zip-slip対策つき)(v0.4.1) |
| `Zevic.Test` | テスト用のアサーション(テストの章を参照)。実際の実装は実装順⑥で、第2段より先に入る |

- 並行処理(`Thread` / `Async`)のモデルは未決。GCとの相性(Boehmのスレッド対応)を見て決める

### 命名の規則

- 型は大文字の名詞、操作は動詞
- 変換は`ToXxx`。失敗しうる変換や生成(`Open` `Parse`)は、`Result`を返す
- ブールを返すのは`Is`や`Has`で始める
- 変更するメソッドは`mut fn`
- 文字列はUTF-8固定。パスもUTF-8として扱う

## zevic(コンパイラ)

`zevic`はコンパイラのコマンドです。ZevicのソースをLLVM IRに変換し、`clang`でコンパイルとリンクをして、実行ファイルを作ります。

### 使い方

```
zevic hello.zev -o hello                 # ビルド
zevic hello.zev native/a.c -o hello      # Cも一緣に
zevic hello.zev --emit-ll -o hello.ll    # LLVM IRだけ出す
zevic --version
```

| オプション | 意味 |
| --- | --- |
| `-o <file>` | 出力ファイル |
| `--emit-ll` | LLVM IR(テキスト)で止める |
| `-O0` `-O1` `-O2` | 最適化レベル(既定は`-O1`) |
| `-g` | デバッグ情報 |
| `-l<name>` `-L<dir>` `-I<dir>` `-D<k=v>` | リンクとCのオプション |
| `--cc <path>` `--cxx <path>` | 外部コンパイラを指定 |
| `--test` | テスト用の実行ファイルを作る |

### コンパイルの流れ

1. **解析**: 字句解析と構文解析でASTを作る
2. **名前解決**: `module`と`import`を辞って名前を結びつける。循環`import`を検出
3. **型検査**: 型を確かめ、ジェネリクスを推論し、`match`の網羅性、`let`の不変性を検査する
4. **単相化**: ジェネリクスを、使われた型ごとに展開する
5. **下ろし**: クロージャを関数と環境に変換し、`defer`と`?`を展開する
6. **出力**: プログラム全体を**1つの`.ll`**(テキストのLLVM IR)にする
7. **ビルド**: `clang`が`.ll`とC/C++のファイルをコンパイルし、ランタイムとリンクする

- プログラム全体を一度にコンパイルする(全体コンパイル)。モジュールごとの分割コンパイルや、インクリメンタルビルドは今はしない
- LLVM IRはテキストとして直接出力する。LLVMのC++ APIやライブラリには依存しない
- ZevicのソースからCを生成する方式はとらない

### 外部ツール(clang)

- **clang 15以上**が必要(テキストのLLVM IRを読めること、ポインタの型が不透明であることが前提)
- `clang`は、次の順で探す
  1. `--cc`で指定したパス
  2. 環境変数`ZEVIC_CLANG`
  3. `PATH`上の`clang`
  4. よくあるインストール先(Windowsの`C:\Program Files\LLVM\bin`、macOSのHomebrew、`/usr/lib/llvm-*/bin`)
- 見つからないときや、バージョンが古いときは、インストール方法(`winget install LLVM.LLVM`、`apt install clang`、`brew install llvm`)を表示して終了する
- WindowsではMSVCのライブラリが必要なので、Visual Studio Build Toolsがあることを前提にする

### ランタイム

- ランタイム(`zevic_alloc`、文字列、`Array`、`Map`、Panic、`Printf`の本体)は、**Cで書く**。コンパイラに同梱して、ビルドのたびに一緒にコンパイルする
- Boehm GCのソースも、同梱する(利用者がGCを入れる必要はない)
- 生成コードが呼ぶランタイム関数の名前は、すべて`zevic_`で始める

### 生成コードの規則

| Zevic | LLVM IR |
| --- | --- |
| `I8`〜`I64` / `U8`〜`U64` | `i8`〜`i64`(符号は命令で区別する) |
| `F32` / `F64` | `float` / `double` |
| `Bool` | `i1`(メモリ上は`i8`) |
| `String` | `{ ptr, i64 }`(バイトを指すポインタと長さ)。中身は末尾にNULを付けて置く |
| `class` | GCヒープ上の構造体への`ptr` |
| `struct` / `enum` | 値として持つタグ付きの構造体 |
| `interface`型 | `{ ptr, ptr }`(オブジェクトと関数表) |
| `fn`型(関数値) | `{ ptr, ptr }`(関数と環境) |
| `Unit` | サイズ0の型(戻り値では`void`) |

- 整数の演算は、オーバーフローを検査する命令(`llvm.sadd.with.overflow`など)で作り、起きたら`Panic`を呼ぶ
- 配列とマップの範囲検査も、生成コードに入れる
- 名前は、`zevic_<Module>_<Name>`の形にする(ピリオドは`_`に変える)。`export "C"`と`extern`はそのまま

### エラー表示

- エラーは、ファイル名・行・列と、問題の行を示して、原因を一文で説明する
- 1回の実行で、報告できるエラーはまとめて報告する(最初の1つで止めない)
- 終了コード: 成功`0`、コンパイルエラー`1`、外部ツールの失敗`2`

## zevpm

`zevpm`はパッケージとビルドの管理コマンドです。普段の開発は`zevpm`を使い、単一ファイルを試すときだけ`zevic`を直接使います。

### コマンド

| コマンド | 内容 |
| --- | --- |
| `zevpm new <name>` | 新しいパッケージを作る(`--lib`でライブラリ) |
| `zevpm init` | 今のディレクトリをパッケージにする |
| `zevpm build` | ビルドする(`--release`で最適化) |
| `zevpm run [-- args]` | ビルドして実行する |
| `zevpm test` | テストを実行する |
| `zevpm fmt` | フォーマットする(`zevfmt`を呼ぶ) |
| `zevpm clean` | `build/`を消す |
| `zevpm get <repo>` | 依存を追加して取得する |
| `zevpm remove <name>` | 依存を外す |
| `zevpm update [name]` | 依存を最新にする |

### ディレクトリ構造

`zevpm new myapp`で、次の構造ができます。

```
myapp/
  zevic.toml          パッケージの設定
  zevic.lock          依存の確定バージョン(コミットする)
  src/
    Main.zev          module Main
  native/             C/C++(必要なときだけ)
  tests/              (任意)統合テスト
  build/              生成物(コミットしない)
  .gitignore
```

- `--lib`のときは、`src/Main.zev`の代わりに`src/<Name>.zev`ができる
- `build/`の中に、`.ll`、`.o`、実行ファイル、`include/`ができる
- `.gitignore`には`build/`を書く

### zevic.toml

```
[package]
name = "MyApp"
version = "0.1.0"
kind = "bin"            # "bin" または "lib"
zevic = ">=0.1"

[deps]
JsonPlus = { git = "https://github.com/someone/jsonplus", tag = "v1.2.0" }
LocalLib = { path = "../locallib" }

[native]
libs = ["m"]
```

- `name`はPascalCase。`module`名の先頭の要素になる
- `version`は`主.副.修正`
- `[deps]`の依存は、`git`(`tag` `branch` `rev`のどれか1つ)か、`path`で指定する

### zevpm get

```
zevpm get github.com/someone/jsonplus@v1.2.0
zevpm get someone/jsonplus            # GitHubの省略形
```

- 基本は**GitHubのリポジトリ**から取得する。`git clone --depth 1`で取ってくる
- 取得したものは、`~/.zevpm/`にキャッシュする。同じバージョンを何度も取らない
- `zevic.toml`の`[deps]`に行を足し、`zevic.lock`にコミットのハッシュを記録する
- タグを指定しないときは、デフォルトブランチの最新のコミットを使う。タグを使うことを勧める
- レジストリは用意しない。バージョンの解決(複数の依存が同じパッケージの違うバージョンを求めるとき)もしない。対立したらエラーにして、利用者が選ぶ
- 依存の依存(推移的な依存)も、同じ仕組みで取得する
- 依存の`native/`も、自動でビルドに加わる
- このコマンドは`git`が必要

### 出力

- `zevpm build`は、`zevic`を呼ぶラッパーです。`src/`のすべての`.zev`と、`native/`のファイル、依存を集めて渡す
- 実行ファイルは`build/<name>`(Windowsでは`.exe`)
- `kind = "lib"`のときは、`build/`に静的ライブラリと`include/<name>.h`を作る

## zevfmt

`zevfmt`はフォーマッタです。**設定ファイルもオプションもなく**、書き方は1つに決まります(`gofmt`と同じ考え方)。

```
zevfmt file.zev          # 整形した結果を標準出力に
zevfmt -w file.zev       # ファイルを書き換える
zevfmt -check src/       # 整形が必要なファイルを一覧し、あれば終了コード 1
```

### 規則

| 項目 | 規則 |
| --- | --- |
| インデント | スペース4つ |
| 行の長さ | 折り返すことはしない(長い行はそのまま) |
| 波括弧 | 開き括弧は同じ行(`if (x) {`)。`else`は`}`と同じ行 |
| 演算子 | 前後に空白1つ(`a + b`)。単項演算子は空白なし |
| カンマ | 後ろに空白1つ。複数行のリストの最後にもカンマ |
| 空行 | 連続する空行は1つにする。関数どうしの間は1行空ける |
| `import` | アルファベット順に並べる |
| 文末 | `;`を付けない。行末の空白を消し、ファイルは改行で終わる |
| コメント | 内容は変えない。位置だけ揃える |

- ソースの意味は変えない。整形してもしなくても、コンパイル結果は同じ
- 構文エラーのあるファイルは整形せず、エラーを表示する
- 整形は**冒等**。一度整形した結果をもう一度整形しても、変わらない
- `zevpm fmt`は、パッケージ内の`.zev`をすべて整形する
- 実装は、コンパイラのパーサを使い回す(コメントを保つため、パーサはコメントを残す)
- 作るのは実装順⑦。それまでは、この規則を手で守る

## テスト

テストは言語の機能として用意します。`test`ブロックをソースに書き、`zevpm test`で実行します。

```
module MyApp.Util.Math
import Zevic.Test

fn Add(a: I32, b: I32) -> I32 { a + b }

test "Add adds two numbers" {
    Test.Equal(Add(1, 2), 3)
}

test "Add overflows" {
    Test.Panics(() => Add(2147483647, 1))
}
```

### 書き方

- `test "名前" { ... }`を、ファイルのトップレベルに書く。名前は文字列リテラル
- 同じモジュールの中に書いてよい。変数や関数は、非公開(小文字)のものも見える
- より大きなテストは、`_test.zev`で終わるファイル(`Math_test.zev`)に分けてよい。同じモジュールとして扱う
- `test`ブロックと`_test.zev`は、**通常のビルドでは無視**され、実行ファイルに入らない
- `tests/`ディレクトリのファイルは、公開のものだけを使う統合テスト

### Zevic.Test

| 関数 | 内容 |
| --- | --- |
| `Test.Equal(actual, expected)` | 等しいこと(`T: Equatable`)。違うときは両方の値を表示 |
| `Test.NotEqual(a, b)` | 等しくないこと |
| `Test.True(cond)` / `Test.False(cond)` | 真偽 |
| `Test.Some(o)` / `Test.None(o)` | `Option`の状態 |
| `Test.Ok(r)` / `Test.Err(r)` | `Result`の状態 |
| `Test.Panics(f)` | `f`がPanicすること |
| `Test.Fail(msg)` | すぐに失敗させる |

- すべてのアサーションは、失敗したときにファイル名と行を表示して、そのテストを打ち切る

### 実行

```
zevpm test                    # すべて
 zevpm test Add               # 名前に "Add" を含むテストだけ
```

- テスト用の実行ファイルを作り、テストを順に呼ぶランナーが`main`に入る
- 各テストは`setjmp`で包む。**テストの中ではPanicがプロセスを終わらせず**、`longjmp`でランナーに戻って、そのテストだけを失敗にする
- テストの中でヒープに確保したものは、次のテストに持ち越すことがある(リークは気にしない)
- `defer`は、Panicで中断されたテストでは実行されない
- 結果の表示:

```
running 3 tests
  ok    Add adds two numbers
  ok    Add overflows
  FAIL  Parse rejects empty input
        Math_test.zev:12: expected 1, got 0

2 passed, 1 failed
```

- 失敗があれば終了コード`1`、なければ`0`
- テストの並列実行と、ベンチマークは、今は入れない

## 実装計画

最初のコンパイラ`zevic0`を**Cで書き**、小さな部分から順に大きくします。将来は、ZevicでZevicのコンパイラを書き(セルフホスティング)、Cのコンパイラを不要にします。

### 段階

| 段階 | 目標 | できるようになること |
| --- | --- | --- |
| ① | Hello World | `module` / `fn` / 文字列 / `Console.Println`。パーサ、型検査の骨組み、LLVM IR出力、`clang`の呼び出し |
| ② | 計算と制御 | 整数と浮動小数点、`let` `mut`、`if` `while` `for` `loop`、関数、オーバーフロー検査、Panic |
| ③ | データ | `struct` `enum` `match`、タプル、`Option` `Result` `?`、`Array`、`String` |
| ④ | クラスとC | `class` `Init` `New`、`interface`、`extern "C"` `export "C"`、Cファイルのリンク、`Map` |
| ⑤ | 抽象化 | ジェネリクス(制約と単相化)、クロージャ、`defer`、`Iterable`、Boehm GC への差し替え |
| ⑥ | ツール | `zevpm`(`new` `build` `run` `test` `get`)、`test`ブロック、`zevic.toml`、第2段の標準ライブラリ |
| ⑦ | 自己ホスティング | `zevfmt`、Zevicで書いたコンパイラ、ブートストラップ |

- 段階①が動くことを最優先にする。それまでは、仕様にある機能を増やさない
- 各段階の終わりに、その段階で動くサンプルとテストをリポジトリに入れる
- `zevic0`は、メモリを解放しない(第5段階まではGCなし)。コンパイラ自身は、動いてすぐ終わるので問題にならない

### zevic0 の構成(C)

```
zevic0/
  lexer.c   parser.c   ast.c
  resolve.c        # module / import
  check.c          # 型検査
  mono.c           # 単相化
  lower.c          # クロージャ、defer、?
  emit_ll.c        # LLVM IR(テキスト)
  driver.c         # clang の呼び出し
  runtime/         # zevic_alloc、文字列、Array、Map、Panic
```

### ブートストラップ

1. `zevic0`(Cで作ったもの)で、Zevicで書いたコンパイラ`zevic1`をコンパイルする
2. `zevic1`で、同じソースをコンパイルして`zevic2`を作る
3. `zevic1`と`zevic2`が出力する`.ll`が**同じ**になれば、セルフホスティング成功
4. この段階での最終的な`.ll`をリポジトリに**コミット**しておく。Cを使わなくても、クローンした直後に`clang`だけでコンパイラを作れる

### 方針

- 実装の言語はC(C11)。ビルドは`clang`と単純な`Makefile`
- 依存ライブラリは、Boehm GCの同梱だけ
- 最初は、エラーメッセージの欲張りをしない。ファイル・行・原因がわかればよい
- テストは、`.zev`と、期待する出力を並べたゴールデンテストを基本にする

## プロジェクト運営

言語を長く育てるために、GitHubのOrganizationで運営します。名前は仮に「Zevic」とし、公開前に確認を済ませます。

### GitHub Organization

- Organization名は`zevic-lang`(`zevic`は取得済みだった)
- 最初は**非公開**で作り、Hello WorldとREADMEが動く段階(v0.1.0)で公開する

| リポジトリ | 内容 |
| --- | --- |
| `zevic` | コンパイラ(`zevic0`、のちにセルフホスト版)とランタイム |
| `zevpm` | パッケージマネージャ(初期は`zevic`と同じリポジトリでもよい) |
| `packages/Zevic` | 標準ライブラリ(`Zevic.*`)。`zevic.toml`を持つパッケージ |
| `docs` | 言語仕様、チュートリアル、設計メモ |
| `examples` | サンプルコード |
| `.github` | Organization共通のIssue/PRテンプレート、行動規範 |

### ライセンス

- コンパイラ、ツール、標準ライブラリ: **Apache-2.0**(特許条項があり、同梱のBoehm GC(MIT系)とも併用できる)
- 仕様書とドキュメント: CC BY 4.0(または同じくApache-2.0)
- 生成された実行ファイルには、ランタイムのライセンス表示が必要になる場合があることを、READMEに書く

### ブランチとレビュー

- `main`は常にビルドとテストが通る状態にする。直接pushせず、PRを通す
- 言語仕様の変更は、`docs`にある提案(`proposals/NNNN-title.md`)で議論してから反映する
- コミットメッセージは英語で、1行目に要約を書く

### CI(継続的インテグレーション)

- GitHub Actionsで、Linux、macOS、Windowsの3環境でビルドとテストを行う
- 内容: `zevic0`のビルド、`tests/`のゴールデンテスト、サンプルのコンパイル、`zevfmt -check`
- セルフホスト後は、ブートストラップの`.ll`が一致することも確かめる

## 未決事項と将来の検討

### 未決事項

| 項目 | 状況 |
| --- | --- |
| 言語の名前 | 「Zevic」は仮。公開前の確認の結果で決める(「プロジェクト運営」を参照) |
| 並行処理 | `Thread`と`Async`のモデル。Boehm GCとの相性を見て決める |
| ジェネリクスの制約 | 実装順⑤で入れる。`where`は用意しない方針 |
| `Integer`(任意精度) | 入れるかどうか、リテラルの書き方 |
| `Main`が`Result`を返す形 | 今は許さない。必要になったら再検討 |
| ポインタの読み書き | `*p`の構文。今は渡すだけ |
| 文字列の補間の書式 | `"${x:5.2}"`のような書式指定を入れるか |

### 将来の検討(今回は入れない)

- `NoGC`モードと`unsafe`(OS開発向けの低レベル制御)
- `extern struct`の`union`とビットフィールド
- 言語としてのマクロ(関数の引数は固定長で、可変長は`params`と`Printf`だけ)
- 継承(入れない方針。合成と`interface`で足りるかを使いながら見る)
- インクリメンタルビルドと、モジュール単位のコンパイル
- パッケージレジストリと、バージョンの解決
- 言語サーバ(LSP)とエディタ連携
- C++のクラスやテンプレートの直接利用
- WebAssemblyなど、LLVMの他のターゲット
- `Never`型を、利用者が書けるようにするか

## 付録A 文法の要約(EBNF風)

記法: `A B`は連接、`A | B`は選択、`[A]`は省略可、`{A}`は0回以上の繰り返し。`'x'`は文字そのもの。改行が文の区切りで、`;`はありません。

```
file        = 'module' modname { import } { decl }
import      = 'import' modname [ 'as' Ident ]
modname     = Ident { '.' Ident }

decl        = fnDecl | structDecl | classDecl | enumDecl | ifaceDecl
            | externBlock | exportFn | testDecl | constDecl
fnDecl      = [ 'mut' ] [ 'static' ] 'fn' Ident [ tparams ] '(' [ params ] ')' [ '->' type ] block
params      = param { ',' param } ; param = [ 'mut' ] Ident ':' type | 'params' Ident ':' type
tparams     = '<' tparam { ',' tparam } '>' ; tparam = Ident [ ':' type { '+' type } ]

classDecl   = 'class' Ident [ tparams ] [ ':' typelist ] '{' { field | fnDecl } '}'
structDecl  = 'struct' Ident [ tparams ] [ ':' typelist ] '{' { field | fnDecl } '}'
field       = [ 'static' ] Ident ':' type [ '=' expr ]
enumDecl    = 'enum' Ident [ tparams ] [ ':' typelist ] '{' variant { ',' variant } [ ',' ] { fnDecl } '}'
variant     = Ident [ '(' typelist ')' | '{' field { ',' field } '}' ]
variantLit  = path '{' Ident ':' expr { ',' Ident ':' expr } [ ',' ] '}'      (名前付きバリアントの値。path は Shape.Named のように大文字始まりで終わる)
ifaceDecl   = 'interface' Ident [ tparams ] [ ':' typelist ] '{' { fnSig | fnDecl } '}'

externBlock = 'extern' '"C"' '{' { cfn | cstruct } '}'
cfn         = 'fn' Ident '(' [ cparams ] ')' [ '->' type ] ; cparams may end with ',' '...'
exportFn    = 'export' '"C"' fnDecl
testDecl    = 'test' String block

block       = '{' { stmt } [ expr ] '}'
stmt        = 'let' pat ':' type '=' expr | 'mut' pat ':' type '=' expr
            | expr [ assignOp expr ] | 'return' [ expr ] | jump
            | 'defer' expr | [ label ] loopStmt
loopStmt    = 'while' '(' expr ')' block | 'loop' block
            | 'for' '(' forPat 'in' expr ')' block
label       = ident '@'
jump        = ( 'break' | 'continue' ) [ '@' ident ]    (* '@'は直前・直後に空白なし *)
forPat      = pat | Ident ',' pat

expr        = ifExpr | matchExpr | asyncExpr | 'await' expr | binary | unary | postfix | lambda | literal | '(' expr ')'
asyncExpr   = 'async' block
ifExpr      = 'if' '(' expr ')' block [ 'else' ( ifExpr | block ) ]
matchExpr   = 'match' '(' expr ')' '{' arm { ',' arm } [ ',' ] '}'
arm         = pat [ 'if' '(' expr ')' ] '=>' expr
lambda      = '(' [ lparam { ',' lparam } ] ')' '=>' expr
postfix     = primary { '.' Ident | '(' [ args ] ')' | '[' expr ']' | '?' }

pat         = '_' | literal | Ident | range | tuplePat | variantPat | structPat
type        = Ident [ '<' typelist '>' ] | '(' typelist ')' | 'fn' '(' [ typelist ] ')' '->' type | '*' type
```

- 演算子の優先順位は、「変数・式・演算子」の章の表に従う
- 条件式の括弧と、`{}`は必須(`if (x) { ... }`)
- 予約語: `module import as fn let mut static struct class enum interface test extern export if else match while loop for in break continue return defer async await this params true false`

## 付録B 総合サンプルコード

仕様の主な機能を使った、1つのプログラムです(単語の出現回数を数えます)。引数は`count <ファイル>`または`top <ファイル> [個数]`です。

```
module Main

import Zevic.Console
import Zevic.Env
import Zevic.Fs
import Zevic.Test
import Zevic.Text

enum Command {
    Count(String),
    Top(String, I64),
}

struct Entry {
    Word: String
    Count: I64

    fn Init(word: String, count: I64) {
        this.Word = word
        this.Count = count
    }
}

class Counter {
    counts: Map<String, I64> = Map.New()

    mut fn Add(word: String) {
        let n: I64 = this.counts.Get(word).UnwrapOr(0)
        this.counts.Set(word, n + 1)
    }

    fn Distinct() -> I64 { this.counts.Len() }

    // The `limit` most frequent words, the most frequent first (equal counts keep the order of the words).
    fn Top(limit: I64) -> Array<Entry> {
        mut items: Array<Entry> = []
        for ((w, n) in this.counts) {
            items.Push(Entry.New(w, n))
        }
        items.SortBy((a: Entry, b: Entry) => b.Count.Compare(a.Count))
        let end: I64 = if (limit < items.Len()) { limit } else { items.Len() }
        items.Slice(0, end)
    }
}

fn Parse(args: Array<String>) -> Result<Command, Error> {
    if (args.Len() < 2) {
        return Err(Error.New(ErrorKind.Invalid, "usage: wc <count|top> <file> [n]"))
    }
    match (args[0]) {
        "count" => Ok(Command.Count(args[1])),
        "top" => {
            let n: I64 = if (args.Len() > 2) { args[2].ToI64()? } else { 10 }
            Ok(Command.Top(args[1], n))
        },
        other => Err(Error.New(ErrorKind.Invalid, "unknown command: ${other}")),
    }
}

fn Load(path: String) -> Result<Counter, Error> {
    let text: String = Fs.ReadAll(path).Context("cannot read ${path}")?
    mut counter: Counter = Counter.New()
    for (line in Text.Lines(Text.ToLower(text))) {
        for (word in Text.Split(line, " ")) {
            if (!word.IsEmpty()) { counter.Add(word) }
        }
    }
    Ok(counter)
}

fn Run(args: Array<String>) -> Result<Unit, Error> {
    match (Parse(args)?) {
        Command.Count(path) => {
            let c: Counter = Load(path)?
            Console.Printf("%d distinct words\n", c.Distinct())
        },
        Command.Top(path, n) => {
            let c: Counter = Load(path)?
            for (i, e in c.Top(n)) {
                Console.Printf("%2d. %-12s %d\n", i + 1, e.Word, e.Count)
            }
        },
    }
    Ok(())
}

fn Main(args: Array<String>) {
    match (Run(args)) {
        Ok(_) => {},
        Err(e) => {
            Console.Eprintln(e.ToString())
            Env.Exit(1)
        },
    }
}

test "Parse rejects an unknown command" {
    Test.Err(Parse(["zzz", "a.txt"]))
}
```

- このサンプルは`examples/apps/wordcount/src/Main.zev`と同じもので、コンパイラの実装でそのまま動く(`tests/examples_test.py`が一致を確かめる)
- 標準ライブラリの`Zevic`は省略できる(`Console.Println`と`Zevic.Console.Println`は同じ)
- 実装順①の最初の目標は、これではなく「Hello World」(`module Main` / `Console.Println`)
