
# BonCasClient

BonCasClient v1.1.1 (http://www2.wazoku.net/2sen/dtvvup/source/BonCasClient(1.1.1).zip) に同梱されているソースコードのミラー。

Visual Studio 2019 対応と既存パッチに加え、複数の仮想リーダー、OS のカードリーダーとの併用、WinSCard 互換 API を追加している。GitHub Actions では Visual Studio 2022 の v143 ツールセットでビルドする。
以下のドキュメントは [BonCasClient.txt](BonCasClient.txt) を現状に合わせて一部改変し、Markdown 形式に書き換えたものである。  
オリジナルの [BonCasClient.txt](BonCasClient.txt) も参照されたい。

### 利用時の注意

BonCasClient のビット数 (32bit or 64bit) は、各種 WinSCard.dll 同様に TVTest などの BonCasClient を利用するソフトに合わせる必要がある。  
さもなければ、ライブラリの読み込みに失敗し、スクランブルの解除に失敗する。

ただし、BonCasClient と BonCasLink (BonCasServer, BonCasService) が異なるビット数になっている事は問題ない。  
BonCasClient と BonCasLink が TCP で通信しているためで、たとえば TVTest と BonCasClient が 64bit 、BonCasLink が 32bit の場合でも正常に読み込める。

----

## BonCasClient.dll

BonCasLink のクライアントインターフェースを実装した DLL。  
既存の以下9関数 CasLinkXXXX は SCardXXXX の別名でもエクスポートされる。
- CasLinkConnectA
- CasLinkConnectW
- CasLinkDisconnect
- CasLinkEstablishContext
- CasLinkFreeMemory
- CasLinkListReadersA
- CasLinkListReadersW
- CasLinkTransmit
- CasLinkReleaseContext

接続先を設定する1関数、

- BOOL WINAPI SetConfig(const DWORD dwIP, const WORD wPort)

が実装されている。`SetConfig` は従来の単一リーダー構成の接続先を変更する。

追加の WinSCard API は `SCardStatusA/W`、`SCardGetStatusChangeA/W`、`SCardBeginTransaction`、`SCardEndTransaction`、`SCardReconnect`、`SCardIsValidContext`、`SCardCancel`、`SCardGetAttrib`、`SCardControl`。PCI データ `g_rgSCardT0Pci`、`g_rgSCardT1Pci`、`g_rgSCardRawPci` もエクスポートする。
仮想リーダーは T=1、カード挿入済みとして扱い、ATR は空。仮想カードの属性取得・Control は未対応を返す。

## ダウンロード

[BonCasClient v1.1.1-patch](https://github.com/mizunoko/BonCasClient/releases/download/v1.1.1-patch/BonCasClient_v1.1.1-patch.zip)

## 設定

設定ファイルは DLL と同じフォルダの、DLL と同じベース名 + `.ini`。`BonCasClient.dll` なら `BonCasClient.ini`、`winscard.dll` にリネームした場合は `winscard.ini` を作成する。
書式及びデフォルト設定 ( ini がない場合) は以下の通りです。

    [Server]
    IP=127.0.0.1
    Port=6900

複数の仮想リーダーを使う場合は `[Readers]` に名前と接続先を追加します。ポートだけなら `[Server] IP` を利用し、`IP:ポート` なら個別サーバーへ接続します。行の順が一覧の順です。セクションが空、存在しない、または有効な行がない場合は従来の `BonCasLink Client Card Reader` 1台を使います。不正な行は無視し、ポートは 1〜65535、サーバーは IPv4 アドレスを指定します。同じ名前が複数ある場合は最初の有効な行を採用します。名前は大文字・小文字を区別します。

`[Readers]` セクションが存在する場合、未登録の名前や NULL の名前では仮想リーダーへ接続せず、未知のリーダーとして扱います。従来構成（セクション不存在）だけは、OS 側でも接続できない名前や NULL を唯一の仮想リーダーへ接続します。

    [Readers]
    BonCasLink B-CAS=6900
    BonCasLink A-CAS=6900
    BonCasLink C-CAS=6901
    Other Server=192.168.1.20:6900

カードリーダー選択に対応したデコーダーでは、たとえば `B25Decoder.ini` に次を設定して B-CAS 用リーダーを指定できます。

    [CardReader]
    Name=BonCasLink B-CAS

A-CAS 用デコーダーでは同じ設定項目を `Name=BonCasLink A-CAS` に変更し、各デコーダーからリーダーを選びます。名前選択への対応や設定ファイル名はデコーダーの実装に依存します。同じ IP・ポートの別名は同じサーバーへ接続するため、別のカードを選べるようにするには対応するサーバー・ポートを用意します。

この PC の OS カードリーダーも一覧に出す場合は次を追加します。省略時は併用しません。

    [Local]
    Enable=1

一覧は仮想リーダー、OS リーダーの順です。仮想名と同名の OS リーダーがある場合、仮想リーダーを優先します。OS の WinSCard は最初の API 呼び出し時に `GetSystemDirectory()` の `winscard.dll` をフルパスで読み込みます。このパスは設定で変更できません。読み込み・コンテキスト作成・OS リーダー列挙に失敗しても仮想リーダーは利用できます。

OS 標準の DLL に加えて、別の WinSCard 実装からもリーダー名を列挙する場合は、`[Local]` に DLL のフルパスを指定します。

    [Local]
    Enable=1
    Path=C:\SmartCard\winscard.dll

指定した DLL のリーダー名は OS リーダーの後に追加され、同名は重複表示しません。指定 DLL の読み込みやリーダー列挙に失敗した場合は、その DLL のリーダーだけ一覧から省かれます。

OS リーダーを使う場合は、デコーダーの `Name` に OS リーダーの実際の名前を指定します。OS に渡したカードへの送受信・状態取得・属性取得などは本物の WinSCard へ転送します。

## Visual C++ ランタイムが必要です

使用するツールセットに対応する Microsoft Visual C++ 再頒布可能パッケージ（CI ビルドは Visual Studio 2022 対応版）。

- 32bit (x86) 版 … https://aka.ms/vs/17/release/VC_redist.x86.exe
- 64bit (x64) 版 … https://aka.ms/vs/17/release/VC_redist.x64.exe

(使う DLL により必要なランタイムも x86、x64 で異なります。x64 の OS なら両方入れておいても問題ありません)  

また、Windows Update による更新があれば最新にしてください。  
(開発環境側も Windows Update により必要な環境が更新されている場合があります)

## ベースにしたもの

- Multi2Dec Ver.2.10 (http://2sen.dip.jp/dtv/)
- BonCasLink Ver.1.10 z1

## 履歴

- 2011/09/25 … 64bit用DLL追加
- 2010/11/11 … 実験品
