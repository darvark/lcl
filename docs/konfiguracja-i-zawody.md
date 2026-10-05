# Konfiguracja i definicje zawodów

Ten dokument opisuje pola obsługiwane w `logger.conf` oraz klucze dozwolone w plikach definicji zawodów w katalogu `contest_defs/`.

## Format pliku `logger.conf`

- Format: `KLUCZ=WARTOŚĆ`
- Puste linie są ignorowane.
- Linie zaczynające się od `#` są ignorowane.
- Parser nie rozróżnia sekcji: liczy się tylko para `klucz=wartość`.

## Gdzie są bazy logów

- Każdy nowy log jest zapisywany jako osobny plik SQLite.
- Domyślny katalog baz logów: `$HOME/.config/contest-logger/logs/`.
- `newlog Nazwa` tworzy plik `Nazwa.db`.
- `newlog` bez nazwy tworzy plik `log_YYYYMMDD_HHMMSS.db`.
- `LOGGER_DB_PATH` może wymusić własną ścieżkę pojedynczej bazy (tryb ręczny).
- jeśli istnieje stary `logger.db`, a brak jeszcze `logs/Default Log.db`, aplikacja automatycznie skopiuje starą bazę do nowej lokalizacji (bez usuwania źródła).

## Pola konfiguracyjne `logger.conf`

### Lokalizacja i identyfikacja stacji

| Klucz | Typ / wartości | Domyślna wartość | Opis |
| --- | --- | --- | --- |
| `LAT` | liczba zmiennoprzecinkowa | `0.0` | Szerokość geograficzna stacji. |
| `LON` | liczba zmiennoprzecinkowa | `0.0` | Długość geograficzna stacji. |
| `LOCATOR` | tekst | pusty | Lokator Maidenhead stacji. |
| `STATION_CALL` | tekst | `N0CALL` | Znak stacji używany m.in. w eksporcie Cabrillo. |
| `STATION_EXCHANGE` | tekst | pusty | Własna wymiana stacji w zawodach, które jej wymagają; w WAG ustaw DOK albo `NM`, jeśli stacja pracuje z Niemiec. |
| `OPERATOR_CALL` | tekst | taki sam jak `STATION_CALL` | Znak operatora logującego łączność; jeśli nie ustawiony, domyślnie przyjmuje wartość znaku stacji. |
| `OPERATOR_NAME` | tekst | pusty | Nazwa operatora do metadanych. |

Uwaga praktyczna:

- `STATION_CALL` pozostaje niezmienione i jest używany tam, gdzie chodzi o tożsamość stacji (np. export Cabrillo i podstawowe dane stacji).
- W profilu WAG stacja wysyła `STATION_EXCHANGE` jako DOK/`NM`; gdy pole jest puste, niemiecki znak jest wykrywany z CTY i logger zablokuje zapis QSO, prosząc o konfigurację wymiany. Stacje poza Niemcami nadają kolejny numer seryjny. Wpisywana wymiana odebrana jest tekstowa, więc obsługuje zarówno numery, jak i alfanumeryczne DOK.
- `OPERATOR_CALL` jest używany w działaniach operatora, np. przy logowaniu, CW, QTC i UI.
- W głównym oknie można zmienić aktywnego operatora szybkim skrótem `Ctrl+O`.
- Pasek statusu pokazuje bieżący znak operatora w formacie `OP: <znak>`.

### DXCluster

| Klucz | Typ / wartości | Domyślna wartość | Opis |
| --- | --- | --- | --- |
| `DXC_HOST` | tekst | `telnet.reversebeacon.net` | Host serwera DXCluster. |
| `DXC_PORT` | liczba całkowita | `7000` | Port TCP DXCluster. |
| `DXC_CALL` | tekst | `N0CALL` | Znak używany do logowania do DXCluster. |

### CAT dla radia 1

| Klucz | Typ / wartości | Domyślna wartość | Opis |
| --- | --- | --- | --- |
| `CAT_MODEL` | liczba całkowita | `2` | Identyfikator modelu Hamlib. |
| `CAT_DEVICE` | tekst | `/dev/ttyUSB0` | Port urządzenia CAT. |
| `CAT_BAUD` | liczba całkowita | `9600` | Prędkość portu szeregowego. GUI podpowiada `1200`, `2400`, `4800`, `9600`, `19200`, `38400`, `57600`, `115200`. |
| `CAT_DATA_BITS` | liczba całkowita | `8` | Liczba bitów danych. GUI używa `5`, `6`, `7`, `8`. |
| `CAT_STOP_BITS` | liczba całkowita | `1` | Liczba bitów stopu. GUI używa `1` lub `2`. |
| `CAT_PARITY` | tekst | `None` | Parzystość. GUI używa `None`, `Even`, `Odd`. |
| `CAT_HANDSHAKE` | tekst | `None` | Handshake. GUI używa `None`, `RTSCTS`, `XONXOFF`. |
| `CAT_MODE_FROM_RIG` | `0` lub `1` | `0` | Gdy `1`, tryb pracy jest pobierany z radia zamiast z częstotliwości lub definicji zawodów. |

### CAT dla radia 2

Pola `CAT2_*` działają tak samo jak `CAT_*`, ale dotyczą drugiego radia.

| Klucz | Typ / wartości | Domyślna wartość |
| --- | --- | --- |
| `CAT2_MODEL` | liczba całkowita | `2` |
| `CAT2_DEVICE` | tekst | `/dev/ttyUSB1` |
| `CAT2_BAUD` | liczba całkowita | `9600` |
| `CAT2_DATA_BITS` | liczba całkowita | `8` |
| `CAT2_STOP_BITS` | liczba całkowita | `1` |
| `CAT2_PARITY` | tekst | `None` |
| `CAT2_HANDSHAKE` | tekst | `None` |

### Zawody

| Klucz | Typ / wartości | Domyślna wartość | Opis |
| --- | --- | --- | --- |
| `CONTEST_DEF_FILE` | ścieżka lub nazwa pliku | `contest.conf` | Definicja zawodów. Może wskazywać plik lokalny albo preset z `contest_defs/`. |
| `CONTEST_TECHNIQUE` | `SO1R`, `SO2V`, `SO2R` | `SO1R` | Technika operatorska. |

### CW keyer

| Klucz | Typ / wartości | Domyślna wartość | Opis |
| --- | --- | --- | --- |
| `CW_DEVICE` | tekst | `/dev/ttyUSB2` | Port klucza CW. |
| `CW_KEYER_LINE` | `DTR` lub `RTS` | `DTR` | Linia sterująca używana przez keyer. |
| `CW_WPM` | liczba całkowita | `20` | Tempo nadawania. Parser ogranicza zakres do `1..60`. Wartość jest natychmiast stosowana po zmianie w głównym interfejsie. |
| `CW_ESM` | `0` lub `1` | `0` | Włącza Enter Sends Message (ESM) dla CW. Działa wyłącznie pod `Enter` i opiera decyzje na RUN/S&P, aktywnym polu (`Call`/`Exchange`) oraz tym, czy pola `Call` i `Exchange` są puste. |

### Centralny log i synchronizacja

| Klucz | Typ / wartości | Domyślna wartość | Opis |
| --- | --- | --- | --- |
| `NET_ENABLED` | `0` lub `1` | `0` | Uruchamia serwer lub okresowego workera klienta. |
| `NET_ROLE` | `client`, `server` | `client` | Rola tej instancji. |
| `NET_STATION_ID` | tekst | generowany lokalnie | Stabilna tożsamość stacji; nie kopiuj jej między klientami. |
| `NET_SHARED_LOG_ID` | `sl-` + 32 małe znaki hex | pusty | Wymagany dla klienta. Serwer tworzy ID w aktywnej bazie i pokazuje je w `net status`; sparuj klienta poleceniem `netsync pair <ID>` przy wyłączonej sieci. |
| `NET_SERVER_HOST` | nazwa lub adres IP | `127.0.0.1` | Adres serwera dla klienta. |
| `NET_SERVER_PORT` | `1..65535` | `9230` | Port TCP serwera. |
| `NET_AUTH_TOKEN` | sekret 32+ znaków | pusty | Wymagany sekret serwera i klienta; serwer nie wystartuje bez silnego tokenu. `NET_SHARED_KEY` jest aliasem zgodności. |
| `NET_TLS` | `0` lub `1` | `0` (sieć domyślnie wyłączona) | Ustaw `1` przed uruchomieniem synchronizacji. Klient wymaga wcześniej skonfigurowanego fingerprintu. |
| `NET_ALLOW_INSECURE_LAN` | `0` lub `1` | `0` | Jawny wyjątek dla zaufanej, odizolowanej sieci przy wyłączonym TLS. Token jest wtedy przesyłany jawnie; nie używaj w Internecie ani sieci współdzielonej. |
| `NET_TLS_CERT_FILE` | ścieżka PEM | `logger_net_cert.pem` | Certyfikat serwera; przy pierwszym starcie może zostać wygenerowany self-signed. |
| `NET_TLS_KEY_FILE` | ścieżka PEM | `logger_net_key.pem` | Prywatny klucz serwera; ogranicz prawa pliku do operatora. |
| `NET_TLS_PEER_FINGERPRINT` | SHA-256 z dwukropkami | pusty | Obowiązkowy pin klienta przy `NET_TLS=1`; nie jest akceptowany automatycznie przy pierwszym połączeniu. |
| `NET_SYNC_INTERVAL_MS` | `100..60000` | `1000` | Odstęp okresowego pollingu klienta. Sesje TCP są krótkie; catch-up może pobrać wiele stron. |
| `NET_HEARTBEAT_SEC` | `1..300` | `5` | Timeout/heartbeat sesji. |
| `NET_RETRY_MIN_MS` / `NET_RETRY_MAX_MS` | liczba milisekund | `1000` / `30000` | Zakres retry z backoffem. |
| `NET_MAX_FRAME_BYTES` | `1024..65536` | `65536` | Limit ramki protokołu. |

### LiveScore

| Klucz | Typ / wartości | Domyślna wartość | Opis |
| --- | --- | --- | --- |
| `LIVE_UPLOAD_ENABLED` | `0` lub `1` | `0` | Wysyła aktualizację po zapisaniu QSO. |
| `LIVE_UPLOAD_HOST` | nazwa lub adres IP | pusty | Host serwera LiveScore. |
| `LIVE_UPLOAD_PORT` | `1..65535` | `9871` | Port UDP serwera LiveScore. |
| `LIVE_UPLOAD_TOKEN` | tekst | pusty | Token przekazywany w każdym pakiecie aktualizacji. |

Aktualizacja jest wysyłana jako pojedynczy datagram UDP z JSON-em typu
`qso_score_update`; zawiera ostatnie QSO i bieżące statystyki. UDP nie gwarantuje
dostarczenia pakietu, a pakiet ani token nie są szyfrowane. Używaj integracji
wyłącznie w zaufanej sieci. Ustawienia LiveScore są dostępne w formularzu
konfiguracji sieciowej, w sekcji LiveScore.

Token musi mieć co najmniej 32 znaki, co najmniej trzy klasy znaków i być
identyczny na serwerze oraz klientach. Wygeneruj go bez spacji/nowej linii, np.:

```bash
openssl rand -base64 48 | tr -d '\n'
```

Plik bieżącego użytkownika znajduje się w `~/.config/contest-logger/logger.conf`
i ma prawa `0600`. Ustawienia synchronizacji oraz LiveScore edytuje się w
formularzu konfiguracji sieciowej. W polu komend aplikacji dostępne jest
sterowanie `net on|off|status`, `net role client|server`,
`netsync pair <shared_log_id>` i `netsync catchup`. Po zmianie konfiguracji
zatrzymaj i uruchom sieć ponownie.

Przykład serwera:

```ini
NET_ENABLED=1
NET_ROLE=server
NET_SERVER_PORT=9230
NET_AUTH_TOKEN=<wspólny-silny-sekret>
NET_TLS=1
NET_TLS_CERT_FILE=/etc/contest-logger/server-cert.pem
NET_TLS_KEY_FILE=/etc/contest-logger/server-key.pem
```

Przykład klienta:

```ini
NET_ENABLED=1
NET_ROLE=client
NET_SERVER_HOST=192.0.2.10
NET_SERVER_PORT=9230
NET_AUTH_TOKEN=<ten-sam-wspólny-sekret>
NET_SHARED_LOG_ID=sl-0123456789abcdef0123456789abcdef
NET_TLS=1
NET_TLS_PEER_FINGERPRINT=<SHA-256-fingerprint-serwera>
```

Serwer nie przyjmuje ID z klienta i nie przypisuje ponownie ID istniejącej
bazie. Klient bez parowania nie wystartuje; brak lub różnica ID w dowolnym
envelope kończy sesję przed apply, pull/catch-up, rezerwacją lub commit.
`logbook_id` w operacji jest lokalny: serwer zapisuje APPEND do swojego
aktywnego logbooka, a klient stosuje catch-up w swoim aktywnym logbooku. Jedna
instancja serwera obsługuje jeden aktywny wspólny log; dla różnych logów
uruchamiaj osobne instancje serwera.

Zmiana lub czyszczenie aktywnego logu jest blokowane, gdy `NET_ENABLED=1`.
Bezpieczny workflow zmiany:

1. `net off` na serwerze i klientach.
2. Otwórz lub utwórz docelowy log.
3. Na serwerze wykonaj `net on`; nowa, nieparowana baza dostanie własny ID.
4. Odczytaj `shared=` z `net status` na serwerze.
5. Na każdym kliencie wykonaj `netsync pair <shared_log_id>`, potem `net on`.

Nie kopiuj ID z poprzedniego serwerowego pliku do nowego logu. Klient odrzuci
start, jeśli jego lokalna baza ma inne, już zapisane pairing.

Przy `NET_TLS=1` kompilacja wymaga OpenSSL (`libssl-dev` na Debian/Ubuntu,
`openssl-devel` na Fedora, `openssl` na Arch). Odczytaj fingerprint serwera
poza połączeniem aplikacji poleceniem `openssl x509 -in server-cert.pem
-noout -fingerprint -sha256` i skonfiguruj go na klientach przed startem.

Sterowanie runtime w polu komend aplikacji: `net on|off|status`,
`net role client|server`, `netsync catchup`, `netserver start|stop`. Pasek statusu pokazuje online/offline,
zaległe i błędne operacje. Dla spójnego backupu zatrzymaj aplikacje i zachowaj
serwerową bazę SQLite, bazy klientów (outbox/cursor), `logger.conf` oraz
serwerowy certyfikat i klucz. Szczegóły pinowania i odtwarzania opisuje
[procedura TLS](self-signed-tls-operacja.md).

## Przykład `logger.conf`

```ini
LAT=52.000000
LON=21.000000
LOCATOR=JO92DF

DXC_HOST=telnet.reversebeacon.net
DXC_PORT=7000
DXC_CALL=SP6AA

CAT_MODEL=1042
CAT_DEVICE=/dev/ttyUSB0
CAT_BAUD=38400
CAT_DATA_BITS=8
CAT_STOP_BITS=1
CAT_PARITY=None
CAT_HANDSHAKE=None
CAT_MODE_FROM_RIG=1

CAT2_MODEL=2
CAT2_DEVICE=/dev/ttyUSB1
CAT2_BAUD=9600
CAT2_DATA_BITS=8
CAT2_STOP_BITS=1
CAT2_PARITY=None
CAT2_HANDSHAKE=None

STATION_CALL=SP6MI
OPERATOR_CALL=SP6MI
OPERATOR_NAME=
# W GUI: Ctrl+O zmienia aktywnego operatora, a pasek statusu pokazuje OP: SP6MI
CONTEST_DEF_FILE=contest_defs/cq_wpx_cw.conf
CONTEST_TECHNIQUE=SO2V

LIVE_UPLOAD_ENABLED=0
LIVE_UPLOAD_HOST=
LIVE_UPLOAD_PORT=9871
LIVE_UPLOAD_TOKEN=

CW_DEVICE=/dev/ttyUSB1
CW_KEYER_LINE=DTR
CW_WPM=20
CW_ESM=1
```

## Format definicji zawodów

- Format: `KLUCZ=WARTOŚĆ`
- Puste linie i komentarze `#...` są ignorowane.
- Klucze są zamieniane na wielkie litery przed interpretacją.
- Nieznane klucze są ignorowane.

## Dozwolone pola w definicji zawodów

### Pola identyfikacyjne i Cabrillo

| Klucz | Typ / wartości | Domyślna wartość | Opis |
| --- | --- | --- | --- |
| `NAME` | tekst | `GENERAL` | Nazwa zawodów w UI i logice programu. |
| `CABRILLO_NAME` | tekst | `GENERAL` | Nazwa zawodów w eksporcie Cabrillo. |
| `CABRILLO-CONTEST` | tekst | alias `CABRILLO_NAME` | Alternatywna nazwa tego samego pola. |
| `MODE` | tekst | `MIXED` | Tryb zawodów, np. `CW`, `SSB`, `RTTY`, `MIXED`. Wartość jest zamieniana na wielkie litery. |

### Kategorie Cabrillo

| Klucz | Typ / wartości | Domyślna wartość |
| --- | --- | --- |
| `CATEGORY_OPERATOR` | tekst | `SINGLE-OP` |
| `CATEGORY_BAND` | tekst | `ALL` |
| `CATEGORY_POWER` | tekst | `LOW` |
| `CATEGORY_OVERLAY` | tekst | pusty |
| `STATION_LOCATION` | tekst | `DX` |
| `OPERATORS` | tekst | pusty |

### Wymiana i pola wejściowe

| Klucz | Typ / wartości | Domyślna wartość | Opis |
| --- | --- | --- | --- |
| `EXCHANGE_SENT` | tekst | `#` | Szablon nadawanej wymiany. `#` zawsze daje rosnący numer `1`, `2`, `3`... |
| `FIELD` | `NAZWA,ETYKIETA,required?` | brak | Jedno pole odbieranej wymiany. Powtarzaj klucz, aby zdefiniować do 16 pól. |

Okno konfiguracji zawodów udostępnia osobne ustawienia dla pierwszego pola
odbieranego (`FIELD`: typ i etykieta) oraz szablonu wymiany nadawanej
(`EXCHANGE_SENT`). Zmiany zapisują się do pliku definicji. Pozostałe pola
`FIELD` pozostają zachowane i można je edytować bezpośrednio w pliku.

Szablony wymiany nadawanej:

- `EXCHANGE_SENT=#` generuje kolejny numer, zaczynając od `1`.
- `EXCHANGE_SENT=SERIAL_GRID` (także `#LOCATOR`) łączy numer z własnym lokatorem, np. `12JO92DF`.
- `EXCHANGE_SENT=# LOCATOR` (także `# GRID`, `SERIAL LOCATOR`) rozdziela numer i lokator spacją, np. `12 JO92DF`.
- inne wartości, np. `EXCHANGE_SENT=ITU`, są wysyłane dosłownie.
- nadawana wymiana pochodzi z definicji zawodów; `logger.conf` nie ma osobnego nadpisania.

Reguły `FIELD`:

- pierwszy element to techniczna nazwa pola, np. `SERIAL`, `ITU_ZONE`, `CQZONE`
- drugi element to etykieta widoczna w UI, np. `Serial Number`
- trzeci element jest opcjonalny i oznacza wymagalność
- jako trzeci element parser rozumie: `required`, `1`, `yes`
- nazwy zawierające `SERIAL`, `NR`, `NUMBER`, `NUM` lub `ZONE` wymuszają wartość numeryczną
- inne nazwy, np. `EXCHANGE`, przyjmują wymianę jako tekst i nie sprawdzają jej formatu

Przykłady:

```ini
FIELD=SERIAL,Serial Number,required
FIELD=ITU_ZONE,ITU Zone,required
FIELD=NAME,Operator Name
```

### Punktacja i mnożniki

| Klucz | Typ / wartości | Domyślna wartość | Opis |
| --- | --- | --- | --- |
| `POINTS_PER_QSO` | liczba całkowita `> 0` | `1` | Bazowa liczba punktów za QSO. |
| `POINTS_CW` | liczba całkowita `>= 0` | `0` | Punkty dodatkowe/specyficzne dla CW. |
| `POINTS_PHONE` | liczba całkowita `>= 0` | `0` | Punkty dla emisji telefonicznych. |
| `POINTS_DIGI` | liczba całkowita `>= 0` | `0` | Punkty dla emisji cyfrowych. |
| `POINTS_NEW_DXCC` | liczba całkowita `>= 0` | `0` | Punkty za nowy DXCC. |
| `POINTS_SAME_DXCC` | liczba całkowita `>= 0` | `0` | Punkty za kolejny QSO z tym samym DXCC. |
| `POINTS_NEW_BAND_DXCC` | liczba całkowita `>= 0` | `0` | Punkty za nowy DXCC na paśmie. |
| `POINTS_SAME_BAND_DXCC` | liczba całkowita `>= 0` | `0` | Punkty za ten sam DXCC na tym samym paśmie. |
| `MULTIPLIER` | `NONE`, `DXCC`, `DXCC_PER_BAND`, `ZONE_PER_BAND`, `ZONE`, `PREFIX`, `PREFIX_PER_BAND` | `DXCC` | Typ mnożnika (`BAND_DXCC`/`BAND-DXCC` i `MODE_DXCC`/`MODE-DXCC` nadal działają jako aliasy kompatybilności). |
| `BONUS_POINTS` | liczba całkowita `>= 0` | `0` | Dodatkowe punkty bonusowe. |
| `QTC_SENDER` | `NONE`, `EU`, `DX`, `BOTH` | `NONE` | Określa, która strona może wysyłać QTC (wymiana QTC w zawodach WAE). `NONE` wyłącza obsługę QTC. |
| `POINTS_PER_QTC` | liczba całkowita `>= 0` | `0` | Punkty za każdy rekord QSO zawarty w paczce QTC. Wymaga `QTC_SENDER` ≠ `NONE`. |

## Przykład definicji zawodów

```ini
NAME=CQ-WPX-CW
CABRILLO_NAME=CQ-WPX-CW
MODE=CW
CATEGORY_OPERATOR=SINGLE-OP
CATEGORY_BAND=ALL
CATEGORY_POWER=LOW
EXCHANGE_SENT=#
POINTS_PER_QSO=1
MULTIPLIER=NONE
FIELD=SERIAL,Serial Number,required
```

### WAG: różne formaty wymiany

Stacja spoza Niemiec może nadawać numer kolejny, a odbierać DOK lub numer
seryjny. Użyj tekstowego typu `EXCHANGE`, aby logger nie wymuszał cyfr:

```ini
EXCHANGE_SENT=#
FIELD=EXCHANGE,Rcv Exch,required
```

W tym ustawieniu można wpisać np. `DOK12`, `NM` albo numer. Program nie
sprawdza jeszcze, który format powinien nadejść od danego znaku. Mnożnik DOK
w WAG również nie jest zaimplementowany.

## Presety dostarczane z programem

W katalogu `contest_defs/` znajdują się gotowe definicje wyprowadzone z oficjalnych plików DXLog.net.
Każdy plik zawiera komentarze opisujące zasady punktowania i ewentualne ograniczenia implementacji.

### Duże zawody międzynarodowe

| Plik | Zawody | Cabrillo | Multiplikator | Uwagi |
|------|--------|----------|--------------|-------|
| `cq_ww_cw.conf` | CQ World Wide DX CW | `CQ-WW-CW` | DXCC + CQ zone/band | 0/1/2/3 pkt wg kontynentu |
| `cq_ww_ssb.conf` | CQ World Wide DX SSB | `CQ-WW-SSB` | DXCC + CQ zone/band | j.w. |
| `cq_wpx_cw.conf` | CQ WPX CW | `CQ-WPX-CW` | PREFIX | 1–6 pkt wg kontynentu i pasma |
| `cq_wpx_ssb.conf` | CQ WPX SSB | `CQ-WPX-SSB` | PREFIX | j.w. |
| `iaru_hf_championship.conf` | IARU HF Championship | `IARU-HF` | ZONE_PER_BAND (ITU) | 1/3/5 pkt; stacje HQ nie zaimplementowane |
| `wae_cw.conf` | WAE DX CW | `DARC-WAEDC-CW` | DXCC/band | EU↔DX=1 pkt; QTC=1 pkt/rekord |
| `wae_ssb.conf` | WAE DX SSB | `DARC-WAEDC-SSB` | DXCC/band | j.w. |
| `sac_cw.conf` | Scandinavian Activity CW | `SAC-CW` | DXCC/band | Scand→EU=2, →DX=3; EU→Scand=1 |
| `sac_ssb.conf` | Scandinavian Activity SSB | `SAC-SSB` | DXCC/band | j.w. |
| `arrl_dx_cw.conf` | ARRL DX CW | `ARRL-DX-CW` | DXCC/band | K/VE↔DX = 3 pkt |
| `arrl_dx_ssb.conf` | ARRL DX SSB | `ARRL-DX-SSB` | DXCC/band | j.w. |
| `oceania_dx_cw.conf` | Oceania DX CW | `OCEANIA-DX-CW` | PREFIX/band | 160m=20, 80m=10, 40m=5, 20m=1, 15m=2, 10m=3 pkt |
| `oceania_dx_ssb.conf` | Oceania DX SSB | `OCEANIA-DX-SSB` | PREFIX/band | j.w. |
| `rdxc_cw.conf` | Russian DX CW | `RDXC` | DXCC/band | Uproszczone; oblast nie zaimplementowany |
| `rdxc_ssb.conf` | Russian DX SSB | `RDXC` | DXCC/band | j.w. |
| `holyland.conf` | Holyland DX | `HOLYLAND-DX` | DXCC/band | 4X obszary nie zaimplementowane |
| `wag.conf` | Worked All Germany | `WAG` | DXCC/band | DOK i numer odbierane jako tekst; mnożnik DOK niezaimplementowany |

### Zawody krajowe

| Plik | Zawody | Cabrillo | Multiplikator | Uwagi |
|------|--------|----------|--------------|-------|
| `sp_dx.conf` | SP DX Contest | `SP-DX` | SPDX (voivodeships) | DX→SP=3, SP→EU=1, SP→DX=3 pkt |

### Ograniczenia implementacji

Niektóre zaawansowane funkcje z definicji DXLog nie są jeszcze obsługiwane:

- **CUSTOM_MULT_LIST** (oblast RDXC, DOK WAG, area Holyland, section ARRL DX) – zastąpiony przez DXCC_PER_BAND
- **Multiplikator PFX_AREA** (WAE dla K/VE/VK/etc.) – używany jest DXCC_PER_BAND
- **HQ stations** w IARU HF – wliczane jako ZONE multiplikator
- **Punktacja zależna od pasma** (Oceania) – zaimplementowana dla CW i SSB
- **Warunkowe formaty Cabrillo** (DL vs DX w WAG) – uproszczone

## Ważne uwagi praktyczne

- Definicja zawodów może być ładowana z `logger.conf`, z komendy `contest <plik>` albo z dialogu tworzenia nowego logu w Qt.
- Preset typu `contest_defs/cq_wpx_cw.conf` jest rozwiązywany także po uruchomieniu programu z katalogu `build/`.
- Jeśli `CONTEST_DEF_FILE` albo komenda `contest` wskazuje nieistniejący plik, tryb zawodów nie zostanie aktywowany.
- Gdy logbook jest otwierany ponownie, program automatycznie przywraca zapisany `contest_definition_path` i wczytuje zgodną definicję zawodów dla tego logu.
- Jeśli `EXCHANGE_SENT=#`, numer nadawany rośnie razem z kolejnymi zapisanymi QSO i jest zapisywany do logu oraz używany w eksporcie Cabrillo.

### Konfiguracja zawodów z poziomu UI Qt

- Okno konfiguracji zawodów otworzysz przez `Ctrl+F8` albo `Menu -> Contest Config`.
- Formularz edytuje typ i etykietę pierwszego pola odbieranego oraz szablon wymiany nadawanej. Dodatkowe pola odbierane edytuje się w pliku definicji.
- Po zatwierdzeniu formularza aplikacja zapisuje plik definicji zawodów i aktualizuje `logger.conf`.
- Przeładowanie zawodów odbywa się po zamknięciu okna dialogowego, aby zminimalizować chwilowe zacięcia UI przy większych logach.

### Import definicji DXLog z poziomu komendy

- `contest import <dxlog_file> [output_conf]` importuje surowy plik DXLog, zapisuje znormalizowaną definicję i od razu ją ładuje.
- `contest import-only <dxlog_file> [output_conf]` importuje i zapisuje plik, ale nie uruchamia auto-load.
- `contest import-only` nie zmienia aktywnej ścieżki `CONTEST_DEF_FILE`.
- Po imporcie, jeśli źródło zawiera reguły spoza wspieranego podzbioru, w linii informacji pojawia się raport ostrzeżeń o pominiętych kluczach DXLog.