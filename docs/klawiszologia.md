# Klawiszologia i obsługa programu

Ten dokument opisuje skróty klawiszowe, działanie klawiszy funkcyjnych, wpisywanie komend oraz podstawowy workflow obsługi programu.

## Podział sterowania

Program ma trzy główne sposoby obsługi:

- klawisze funkcyjne `Fn` do wysyłania komunikatów CW
- skróty `Ctrl+Fn` i `Ctrl+K` do sterowania aplikacją Qt
- wpisywanie danych QSO albo komend tekstowych w polach wejściowych

## Klawisze funkcyjne `Fn` w CW

Zwykłe `F1...F10` są przeznaczone dla wiadomości CW. Treść jest pobierana z `cw_keys.ini`.

### Skąd biorą się wiadomości

- najpierw z pliku `cw_keys.ini` obok binarki Qt
- potem z `cw_keys.ini` w bieżącym katalogu roboczym
- na końcu z `../cw_keys.ini` względem katalogu aplikacji

### Sekcje pliku `cw_keys.ini`

- `[RUN]` dla pracy RUN
- `[S&P]` dla pracy Search and Pounce

### Obsługiwane znaczniki w wiadomościach CW

| Znacznik | Znaczenie |
| --- | --- |
| `{MYCALL}` | znak z `STATION_CALL` |
| `{RST}` | `599` dla CW albo `59` dla innych trybów |
| `{EXCH}` | aktualna nadawana wymiana contestowa |
| `{HISCALL}` / `{CALL}` | aktualna zawartość pola `Call` dla aktywnego radia |

### Domyślny przykład `cw_keys.ini`

| Klawisz | RUN | S&P |
| --- | --- | --- |
| `F1` | `CQ DE {MYCALL}` | `CQ DE {MYCALL}` |
| `F2` | `{RST} {EXCH}` | `{RST} {EXCH}` |
| `F3` | `{EXCH}` | `{EXCH}` |
| `F4` | `{MYCALL}` | `{MYCALL}` |
| `F5` | `AGN` | `AGN` |
| `F6` | `NR` | `NR` |
| `F7` | `{RST}` | `{RST}` |
| `F8` | `QRZ` | `QRZ` |
| `F9` | `{HISCALL}` | `{HISCALL}` |
| `F10` | `?` | `?` |

Jeśli dla danego klawisza nie ma definicji, program pokaże status `F<n>: brak definicji CW`.

## Skróty aplikacyjne Qt

### `Ctrl+Fn`

| Skrót | Działanie |
| --- | --- |
| `Ctrl+F1` | otwiera okno dokumentacji (README + pliki z `docs/`, filtr plików, render Markdown, Match case i podświetlanie wszystkich trafień) |
| `Ctrl+F2` | otwiera dialog tworzenia nowego logu z opcjonalnym wyborem presetu zawodów |
| `Ctrl+F3` | otwiera dialog wyboru istniejącego nazwanego logu |
| `Ctrl+F4` | uruchamia tryb wpisania nazwy pliku ADIF do eksportu |
| `Ctrl+F5` | pokazuje lub ukrywa okno DXCluster |
| `Ctrl+F6` | przelicza statystyki |
| `Ctrl+F7` | pobiera `wl_cty.dat` i blokuje klawiaturę na czas aktualizacji |
| `Ctrl+F8` | otwiera okno konfiguracji zawodów |
| `Ctrl+F9` | pokazuje albo ukrywa panel konfiguracji CAT/CW |
| `Ctrl+F10` | kończy program |
| `Ctrl+Up` | przechodzi do poprzedniego spotu w bandmapie i ustawia jego częstotliwość |
| `Ctrl+Down` | przechodzi do następnego spotu w bandmapie i ustawia jego częstotliwość |

### Inne skróty Qt

| Skrót | Działanie |
| --- | --- |
| `Ctrl+K` | otwiera okno ręcznego keyera CW |
| `Esc` | zatrzymuje nadawanie CW i jednocześnie czyści aktywne pola wejściowe |
| `Alt+W` | czyści pola `Call` i `Exchange` dla aktywnego radia |

## Menu i widoczność paneli

Aplikacja używa jednego menu głównego `Menu` z pogrupowanymi akcjami.

Najważniejsze pozycje:

- `Contest Config` - otwiera konfigurację zawodów (to samo co `Ctrl+F8`)
- `DX Cluster` - przełącza widoczność okna clustera
- `Show CAT/CW Config` - pokazuje albo ukrywa panel konfiguracji CAT/CW (to samo co `Ctrl+F9`)

## Znaczniki połączenia CAT i CW

Obok sekcji `Run/S&P` są stale widoczne dwa badge:

- `CAT ON`/`CAT OFF`
- `CW ON`/`CW OFF`

`CW` jest wyświetlany pod `CAT`, dzięki czemu status połączeń jest czytelny nawet przy ukrytym panelu konfiguracji CAT/CW.

## Bandmapa

W panelu DXCluster widoczna jest dodatkowa lista bandmapy:

- zawiera spoty z aktualnie ustawionego pasma
- jest sortowana rosnąco po częstotliwości
- automatycznie pomija duplikaty tego samego callsignu na bardzo bliskich częstotliwościach
- podwójne kliknięcie na spot ustawia częstotliwość tego spotu
- `Ctrl+Up` / `Ctrl+Down` przechodzi po liście i stroi radio do wybranego spotu

## Klawisze sterujące w głównym oknie

| Klawisz | Działanie |
| --- | --- |
| `Space` | przechodzi do następnego pola wejścia |
| `Enter` | zatwierdza wpis QSO albo komendę |
| `Backspace` | kasuje znak w aktywnym polu |
| `Tab` | wstawia aktualnie zaznaczoną sugestię znaku do pola `Call` |
| `Up` | wybiera poprzednią sugestię znaku |
| `Down` | wybiera następną sugestię znaku |
| `Left` | w trybie `SO2V`/`SO2R` przełącza aktywne radio na `R1` |
| `Right` | w trybie `SO2V`/`SO2R` przełącza aktywne radio na `R2` |

## Workflow wpisywania QSO

### Tryb normalny

Widoczne są trzy pola:

- `Call`
- `RST`
- `Comments`

Typowy przebieg:

1. wpisz znak w `Call`
2. naciśnij `Space`
3. wpisz raport w `RST`
4. naciśnij `Space`
5. wpisz komentarz albo częstotliwość, zależnie od sposobu pracy
6. naciśnij `Enter`

### Tryb contestowy

Widoczne są dwa pola:

- `Call`
- `Exchange`

W tym trybie:

- etykieta pola `Exchange` pochodzi z `FIELD=...` z definicji zawodów
- raport nadawany jest ustawiany automatycznie jako `599` dla CW albo `59` dla innych emisji
- komentarze nie są wpisywane ręcznie
- nadawana wymiana (`TX ...`) pochodzi z `EXCHANGE_SENT`
- jeśli `EXCHANGE_SENT=#`, program zawsze generuje `1`, `2`, `3`... zgodnie z kolejnymi zapisanymi QSO

## Podpowiedzi znaków

Podpowiedzi działają tylko podczas edycji pola `Call`.

| Akcja | Efekt |
| --- | --- |
| wpisywanie znaku | odświeża listę dopasowań z historii |
| `Up` / `Down` | zmienia zaznaczenie w liście sugestii |
| `Tab` | wstawia zaznaczoną sugestię bez przechodzenia do kolejnego pola |
| `Space` | wstawia zaznaczoną sugestię i przechodzi do kolejnego pola |

## Ręczne ustawianie częstotliwości

Jeżeli w polu `Call` wpiszesz wyłącznie cyfry i naciśniesz `Enter`, program potraktuje to jako częstotliwość w kHz.

Przykład:

- wpisz `7020`
- naciśnij `Enter`
- program ustawi częstotliwość ręcznie, a przy aktywnym CAT spróbuje też ustawić radio

## Tryb eksportu ADIF

Po `Ctrl+F4` program przechodzi w specjalny tryb wpisania nazwy pliku ADIF.

W tym trybie:

- wpisujesz tylko nazwę pliku
- `Enter` uruchamia eksport
- `Esc` anuluje eksport

## Komendy tekstowe

Komendy można wpisywać w linii wejścia i zatwierdzać `Enter`.

| Komenda | Działanie |
| --- | --- |
| `export` | eksportuje `log.csv` i `log.adi` |
| `export mojlog.adi` | eksportuje `log.csv` i wskazany plik ADIF |
| `exportcab` | eksportuje Cabrillo do `log.cbr` |
| `exportcab mojlog.cbr` | eksportuje Cabrillo do wskazanego pliku |
| `invalid` | przełącza flagę INVALID dla ostatniego QSO |
| `newlog` lub `clear` | tworzy nowy pusty log |
| `newlog Nazwa zawodów` | tworzy i przełącza na nowy nazwany log |
| `prevlog`, `openprev`, `previous` | otwiera poprzedni log |
| `logs` | pokazuje listę nazwanych logów |
| `openlog 12` | otwiera nazwany log po ID |
| `openlog Summer Contest` | otwiera log po nazwie |
| `contest plik.conf` | ładuje definicję zawodów |
| `contest none` | wyłącza tryb zawodów |
| `technique` | pokazuje aktualną technikę operatorską |
| `technique SO1R` | ustawia `SO1R` |
| `technique SO2V` | ustawia `SO2V` |
| `technique SO2R` | ustawia `SO2R` |
| `quit` | kończy program |

## Tworzenie nowego logu w Qt

Po `Ctrl+F2` frontend Qt uruchamia dwa kroki:

1. pyta o nazwę nowego logu
2. pyta o preset zawodów

Jeżeli wybierzesz preset:

- frontend wykona `newlog <nazwa>`
- następnie wykona `contest <ścieżka do presetu>`

Jeżeli wybierzesz `None`:

- frontend wykona `newlog <nazwa>`
- następnie wykona `contest none`

## Co pokazuje dolny pasek funkcji

Dolny pasek ma dwie linie:

- pierwsza linia pokazuje bieżące wiadomości `Fn` rozwinięte dla aktualnego trybu `RUN` albo `S&P`
- druga linia pokazuje skróty `Ctrl+Fn` i `Ctrl+K`

## Uwagi praktyczne

- `Space` nie wpisuje spacji do komendy, tylko przechodzi do następnego pola. Dlatego w GUI komendy z argumentami z dialogów są wykonywane bezpośrednio, a nie przez symulację klawiszy.
- W trybie aktualizacji CTY po `Ctrl+F7` klawiatura jest blokowana do zakończenia pobierania.
- `PageUp` i `PageDown` są mapowane w warstwie Qt, ale obecnie nie mają odrębnej akcji użytkowej opisanej przez kontroler.