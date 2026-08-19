# Konfiguracja i definicje zawodów

Ten dokument opisuje pola obsługiwane w `logger.conf` oraz klucze dozwolone w plikach definicji zawodów w katalogu `contest_defs/`.

## Format pliku `logger.conf`

- Format: `KLUCZ=WARTOŚĆ`
- Puste linie są ignorowane.
- Linie zaczynające się od `#` są ignorowane.
- Parser nie rozróżnia sekcji: liczy się tylko para `klucz=wartość`.

## Pola konfiguracyjne `logger.conf`

### Lokalizacja i identyfikacja stacji

| Klucz | Typ / wartości | Domyślna wartość | Opis |
| --- | --- | --- | --- |
| `LAT` | liczba zmiennoprzecinkowa | `0.0` | Szerokość geograficzna stacji. |
| `LON` | liczba zmiennoprzecinkowa | `0.0` | Długość geograficzna stacji. |
| `LOCATOR` | tekst | pusty | Lokator Maidenhead stacji. |
| `STATION_CALL` | tekst | `N0CALL` | Znak stacji używany m.in. w eksporcie Cabrillo. |
| `OPERATOR_NAME` | tekst | pusty | Nazwa operatora do metadanych. |

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
| `CONTEST_TX_EXCHANGE` | tekst | pusty | Nadpisuje nadawaną wymianę tylko wtedy, gdy `EXCHANGE_SENT` w definicji zawodów jest stałym tekstem. |
| `CONTEST_TECHNIQUE` | `SO1R`, `SO2V`, `SO2R` | `SO1R` | Technika operatorska. |

Ważna reguła dla wymiany nadawanej:

- `EXCHANGE_SENT=#` zawsze oznacza numer inkrementowany od `1` w górę.
- W takim trybie `CONTEST_TX_EXCHANGE` jest ignorowane.
- `CONTEST_TX_EXCHANGE` ma sens tylko dla statycznych szablonów typu `ITU`, `CQZONE`, `28`.

### CW keyer

| Klucz | Typ / wartości | Domyślna wartość | Opis |
| --- | --- | --- | --- |
| `CW_DEVICE` | tekst | `/dev/ttyUSB2` | Port klucza CW. |
| `CW_KEYER_LINE` | `DTR` lub `RTS` | `DTR` | Linia sterująca używana przez keyer. |
| `CW_WPM` | liczba całkowita | `20` | Tempo nadawania. Parser ogranicza zakres do `1..60`. Wartość jest natychmiast stosowana po zmianie w głównym interfejsie. |

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
OPERATOR_NAME=
CONTEST_DEF_FILE=contest_defs/cq_wpx_cw.conf
CONTEST_TX_EXCHANGE=
CONTEST_TECHNIQUE=SO2V

CW_DEVICE=/dev/ttyUSB1
CW_KEYER_LINE=DTR
CW_WPM=20
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
| `FIELD` | `NAZWA,ETIETA,required?` | brak | Definicja jednego pola odbieranej wymiany. Można zdefiniować maksymalnie 16 pól. |

Reguły `FIELD`:

- pierwszy element to techniczna nazwa pola, np. `SERIAL`, `ITU_ZONE`, `CQZONE`
- drugi element to etykieta widoczna w UI, np. `Serial Number`
- trzeci element jest opcjonalny i oznacza wymagalność
- jako trzeci element parser rozumie: `required`, `1`, `yes`

Przykłady:

```ini
FIELD=SERIAL,Serial Number,required
FIELD=ITU_ZONE,ITU Zone,required
FIELD=NAME,Operator Name
```

Uwaga praktyczna:

- walidacja numeryczna w UI jest automatycznie stosowana dla nazw pól zawierających m.in. `SERIAL`, `NR`, `NUMBER`, `NUM`, `ZONE`

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
| `wag.conf` | Worked All Germany | `WAG` | DXCC/band | DOK multiplier nie zaimplementowany |

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
- Po zatwierdzeniu formularza aplikacja zapisuje plik definicji zawodów i aktualizuje `logger.conf`.
- Przeładowanie zawodów odbywa się po zamknięciu okna dialogowego, aby zminimalizować chwilowe zacięcia UI przy większych logach.

### Import definicji DXLog z poziomu komendy

- `contest import <dxlog_file> [output_conf]` importuje surowy plik DXLog, zapisuje znormalizowaną definicję i od razu ją ładuje.
- `contest import-only <dxlog_file> [output_conf]` importuje i zapisuje plik, ale nie uruchamia auto-load.
- `contest import-only` nie zmienia aktywnej ścieżki `CONTEST_DEF_FILE`.
- Po imporcie, jeśli źródło zawiera reguły spoza wspieranego podzbioru, w linii informacji pojawia się raport ostrzeżeń o pominiętych kluczach DXLog.