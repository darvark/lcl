# Przykładowe konfiguracje SO1R, SO2V i SO2R

Ten dokument zawiera gotowe przykłady `logger.conf` dla trzech typowych sposobów pracy:

- `SO1R` - jedno radio
- `SO2V` - dwa VFO / dwa radia logiczne obsługiwane jak dwa stanowiska wejściowe
- `SO2R` - dwa niezależne radia CAT

Wartości typu porty urządzeń, model Hamlib, znak i host DXCluster trzeba dopasować do własnej stacji.

## Wspólne założenia

- `EXCHANGE_SENT=#` w definicji zawodów zawsze daje inkrementację `1`, `2`, `3`...
- `CONTEST_TX_EXCHANGE` ustawiaj tylko dla zawodów ze statyczną wymianą nadawaną
- `CONTEST_DEF_FILE` może wskazywać preset z `contest_defs/`

## Przykład 1: SO1R

Najprostsza konfiguracja dla jednego radia i jednego portu CAT.

```ini
LAT=52.000000
LON=21.000000
LOCATOR=JO92DF

DXC_HOST=telnet.reversebeacon.net
DXC_PORT=7000
DXC_CALL=SP6ABC

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

STATION_CALL=SP6ABC
OPERATOR_NAME=Jan Operator
CONTEST_DEF_FILE=contest_defs/cq_wpx_cw.conf
CONTEST_TX_EXCHANGE=
CONTEST_TECHNIQUE=SO1R

CW_DEVICE=/dev/ttyUSB2
CW_KEYER_LINE=DTR
CW_WPM=26
```

Kiedy użyć:

- jedno radio
- standardowa praca contestowa bez drugiego toru wejściowego
- najprostsza konfiguracja do codziennego logowania

## Przykład 2: SO2V

Konfiguracja dla jednego operatora, który chce pracować z dwoma torami wejściowymi w aplikacji i szybko przełączać aktywne radio/VFO.

```ini
LAT=52.000000
LON=21.000000
LOCATOR=JO92DF

DXC_HOST=telnet.reversebeacon.net
DXC_PORT=7000
DXC_CALL=SP6ABC

CAT_MODEL=1042
CAT_DEVICE=/dev/ttyUSB0
CAT_BAUD=38400
CAT_DATA_BITS=8
CAT_STOP_BITS=1
CAT_PARITY=None
CAT_HANDSHAKE=None
CAT_MODE_FROM_RIG=1

CAT2_MODEL=1042
CAT2_DEVICE=/dev/ttyUSB0
CAT2_BAUD=38400
CAT2_DATA_BITS=8
CAT2_STOP_BITS=1
CAT2_PARITY=None
CAT2_HANDSHAKE=None

STATION_CALL=SP6ABC
OPERATOR_NAME=Jan Operator
CONTEST_DEF_FILE=contest_defs/cq_wpx_cw.conf
CONTEST_TX_EXCHANGE=
CONTEST_TECHNIQUE=SO2V

CW_DEVICE=/dev/ttyUSB2
CW_KEYER_LINE=DTR
CW_WPM=28
```

Uwagi:

- aplikacja pokaże dwa pola wejściowe `R1` i `R2`
- strzałki `Left` i `Right` przełączają aktywne radio logiczne
- w praktyce oba profile CAT mogą wskazywać to samo radio, jeśli chcesz pracować w logice zbliżonej do dwóch VFO

## Przykład 3: SO2R

Konfiguracja dla dwóch niezależnych radii z osobnymi portami CAT.

```ini
LAT=52.000000
LON=21.000000
LOCATOR=JO92DF

DXC_HOST=telnet.reversebeacon.net
DXC_PORT=7000
DXC_CALL=SP6ABC

CAT_MODEL=1042
CAT_DEVICE=/dev/ttyUSB0
CAT_BAUD=38400
CAT_DATA_BITS=8
CAT_STOP_BITS=1
CAT_PARITY=None
CAT_HANDSHAKE=None
CAT_MODE_FROM_RIG=1

CAT2_MODEL=1234
CAT2_DEVICE=/dev/ttyUSB1
CAT2_BAUD=19200
CAT2_DATA_BITS=8
CAT2_STOP_BITS=1
CAT2_PARITY=None
CAT2_HANDSHAKE=None

STATION_CALL=SP6ABC
OPERATOR_NAME=Jan Operator
CONTEST_DEF_FILE=contest_defs/cq_wpx_cw.conf
CONTEST_TX_EXCHANGE=
CONTEST_TECHNIQUE=SO2R

CW_DEVICE=/dev/ttyUSB2
CW_KEYER_LINE=RTS
CW_WPM=30
```

Uwagi:

- `CAT_*` dotyczy radia 1
- `CAT2_*` dotyczy radia 2
- oba radia mają osobne częstotliwości, stany RUN/S&P i osobne pola wejściowe

## Przykład zawodów ze statyczną wymianą nadawaną

Jeżeli zawody mają stałą wymianę, użyj statycznego `EXCHANGE_SENT` w definicji zawodów i opcjonalnie `CONTEST_TX_EXCHANGE` w `logger.conf`.

Przykład:

```ini
CONTEST_DEF_FILE=contest_defs/iaru_hf_championship.conf
CONTEST_TX_EXCHANGE=28
```

W tym scenariuszu:

- jeśli preset ma `EXCHANGE_SENT=ITU`, program nada `28`
- jeśli preset ma `EXCHANGE_SENT=#`, `CONTEST_TX_EXCHANGE` zostanie zignorowane

## Szybki wybór scenariusza

| Tryb | Kiedy wybrać |
| --- | --- |
| `SO1R` | jedno radio, najprostsza obsługa |
| `SO2V` | dwa niezależne pola wejściowe, ale jeden fizyczny tor radiowy lub jedna logika operatorska |
| `SO2R` | dwa niezależne radia CAT i pełna praca dwuradiowa |