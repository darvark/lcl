# Obsługa QTC – WAE DX Contest

## Czym jest QTC?

QTC to technika wymiany łączności stosowana w zawodach **WAE DX Contest** (Worked All Europe,
organizowanych przez DARC). Stacja europejska (EU) może przesłać stacji DX paczkę zawierającą
dane o wcześniej przeprowadzonych QSO. Każde takie QSO w paczce liczy się jako dodatkowy punkt
zarówno dla nadawcy, jak i odbiorcy.

- Jedna paczka QTC: maks. **10** rekordów QSO.
- Format rekordu: `HHMM CALL NR_SERYJNY` (czas UTC, znak wywoławczy, odebrany numer seryjny).
- Jedno QSO może być zawarte **tylko w jednej** paczce.
- Stacje EU wysyłają QTC do stacji DX.

### Punktacja QSO (WAE)

| Kierunek | Punkty |
|----------|--------|
| EU ↔ DX  | 1 punkt za QSO |
| EU ↔ EU  | 0 punktów |
| DX ↔ DX  | 0 punktów |

### Multiplikatory

| Multiplikator | Zakres |
|---------------|--------|
| MULT1 – DXCC per band | podstawowy, wszystkie pasma |
| MULT2 – Prefix area per band | tylko dla dużych DXCC: K, VE, VK, ZL, ZS, JA, BY, PY, UA9 |

> **Uwaga:** Multiplikator MULT2 (Prefix Area) nie jest jeszcze w pełni zaimplementowany.
> Aktualnie używany jest `DXCC_PER_BAND` jako najlepsza dostępna aproksymacja.

---

## Konfiguracja zawodów z QTC

Definicja zawodów musi zawierać pola:

```ini
QTC_SENDER=EU          # kto wysyła QTC: EU | DX | BOTH | NONE (domyślne)
POINTS_PER_QTC=1       # punkty za każdy rekord QTC
```

Gotowe pliki konfiguracyjne dla WAE (wyprowadzone z oficjalnej definicji DXLog):

- [contest_defs/wae_cw.conf](../contest_defs/wae_cw.conf) – WAE DX Contest CW (`DARC-WAEDC-CW`)
- [contest_defs/wae_ssb.conf](../contest_defs/wae_ssb.conf) – WAE DX Contest SSB (`DARC-WAEDC-SSB`)

Załadowanie zawodów:

```
contest contest_defs/wae_cw.conf
```

---

## Okno QTC – Ctrl+L

Skrót **Ctrl+L** otwiera okno wymiany QTC. Okno jest dostępne wyłącznie po załadowaniu definicji zawodów z `QTC_SENDER` ≠ `NONE`.

### Elementy okna

| Element | Opis |
|---------|------|
| **Receiver** | Znak stacji, z którą wymieniamy QTC |
| **Bundle** | Numer paczki (auto-inkrementowany) |
| **Sending / Receiving** | Kierunek: czy wysyłamy, czy odbieramy QTC |
| **Tabela rekordów** | 10 wierszy: Data, Czas, Call, Wymiana |
| **Pre-fill from Log** | Automatyczne wypełnienie z ostatnich QSO (nie wysłanych wcześniej) |
| **Save QTC Bundle** | Zapisuje paczkę do bazy danych i aktualizuje statystyki |

### Wysyłanie przez CW Keyer

Przyciski sekcji **CW Send**:

| Przycisk | Akcja |
|----------|-------|
| **Send Preamble** | Wysyła zapowiedź paczki wg szablonu `PREAMBLE` z `cw_keys.ini` |
| **Send Record** | Wysyła zaznaczony wiersz wg szablonu `RECORD` |
| **Send QSL** | Wysyła potwierdzenie (szablon `CONFIRM`) |
| **Send QRV** | Wysyła żądanie QTC do DX (szablon `REQUEST`) |

### Procedura wysyłania QTC (EU → DX)

1. Wpisz znak DX w polu **Receiver**.
2. Kliknij **Pre-fill from Log** – tabela wypełni się maks. 10 ostatnimi QSO.
3. Kliknij **Send Preamble** → keyer wyśle np. `QTC 1/10`.
4. Zaznacz wiersz 1, kliknij **Send Record** → keyer wyśle np. `1430 DK5AI 42`.
5. Po potwierdzeniu od DX kliknij **Send QSL**.
6. Powtarzaj kroki 4–5 dla każdego rekordu.
7. Po wysłaniu wszystkich rekordów kliknij **Save QTC Bundle**.

### Procedura przyjmowania QTC (DX przyjmuje)

1. Ustaw **Sending/Receiving** na „Receiving".
2. Wpisz znak EU w polu **Receiver**.
3. Ręcznie wpisuj dane każdego rekordu w tabeli podczas odbioru.
4. Po zakończeniu kliknij **Save QTC Bundle**.

---

## Szablony CW dla QTC (`cw_keys.ini`)

Sekcja `[QTC]` w pliku `cw_keys.ini`:

```ini
[QTC]
PREAMBLE = QTC {QTC_NR}/{QTC_COUNT}
RECORD   = {QTC_TIME} {QTC_CALL} {QTC_EXCH}
CONFIRM  = QSL
REQUEST  = QRV
```

### Dostępne placeholdery

| Placeholder | Wartość |
|-------------|---------|
| `{QTC_NR}` | Numer paczki QTC |
| `{QTC_COUNT}` | Liczba rekordów w paczce |
| `{QTC_TIME}` | Czas QSO (HHMM) z bieżącego rekordu |
| `{QTC_CALL}` | Znak wywoławczy z bieżącego rekordu |
| `{QTC_EXCH}` | Wymiana (nr seryjny) z bieżącego rekordu |
| `{MYCALL}` | Własny znak stacji |
| `{HISCALL}` / `{CALL}` | Znak stacji drugiej |

---

## Eksport Cabrillo z QTC

Polecenie `exportcab` automatycznie dołącza linie `QTC:` gdy zawody mają włączone QTC:

```
exportcab log.cbr
```

Format linii QTC w Cabrillo 3.0:

```
QTC: freq mode yyyy-mm-dd hhmm sender_call NR/TOT receiver_call
     yyyy-mm-dd hhmm call exch
```

---

## Statystyki QTC

W panelu statystyk (`Ctrl+F6`) pojawia się dodatkowa informacja:

```
QTC: N rekordów, M punktów
```

Punktacja końcowa:
`(punkty QSO + punkty QTC) × multiplikatory + punkty bonusowe`

---

## Ograniczenia i reguły

- To samo QSO (identyczny znak + data + czas) nie może być w dwóch różnych paczkach.
- Maksymalnie 10 rekordów w jednej paczce.
- QTC jest dostępne tylko gdy załadowano definicję zawodów z `QTC_SENDER` ≠ `NONE`.
- Pole `QTC_SENDER=EU` oznacza, że wysyłać QTC mogą tylko stacje europejskie. Kontynentalność własnej stacji jest sprawdzana na podstawie CTY.
