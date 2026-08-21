# Ściąga operatora

Krótka ściąga do codziennej pracy z programem.

## Start

- sprawdź `logger.conf`
- upewnij się, że `wl_cty.dat` jest dostępny
- jeśli `MASTER.SCP` nie istnieje, pobierz go przez `Menu → Update SCP (Check Partial)`
- jeśli pracujesz w zawodach, wybierz preset przez `Ctrl+F2` albo komendę `contest ...`

## Najważniejsze skróty

| Klawisz | Co robi |
| --- | --- |
| `F1..F10` | wysyłają wiadomości CW z `cw_keys.ini` |
| `Ctrl+F1` | okno dokumentacji (README + docs) |
| `Ctrl+F2` | nowy log + wybór zawodów |
| `Ctrl+F3` | otwarcie nazwanego logu |
| `Ctrl+F4` | eksport ADIF/CSV |
| `Ctrl+F5` | pokaż/ukryj DXCluster |
| `Ctrl+F6` | przelicz statystyki |
| `Ctrl+F7` | aktualizacja `wl_cty.dat` |
| `Menu → Update SCP` | aktualizacja bazy Check Partial (`MASTER.SCP`) |
| `Ctrl+F8` | konfiguracja zawodów |
| `Ctrl+Shift+F8` | konfiguracja stacji |
| `Ctrl+Shift+F9` | konfiguracja sieci |
| `Ctrl+F9` | pokaż/ukryj panel CAT/CW |
| `Ctrl+Up / Ctrl+Down` | poprzedni/następny spot w bandmapie + strojenie |
| `Ctrl+F10` | wyjście |
| `Ctrl+K` | ręczny keyer CW |

## Nawigacja po polach

| Klawisz | Co robi |
| --- | --- |
| `Space` | następne pole |
| `Enter` | zatwierdzenie wpisu |
| `Backspace` | kasowanie znaku |
| `Tab` | wstawienie aktualnej sugestii znaku |
| `Up` / `Down` | wybór sugestii znaku |
| `Left` / `Right` | zmiana aktywnego radia w `SO2V` i `SO2R` |
| `Esc` | czyszczenie aktywnych pól i zatrzymanie CW |

## Podpowiedzi i Check Partial

- panel **Suggestions / SCP** pojawia się w prawym górnym rogu tabeli logów
- **ciemnożółte** wpisy — znaki z historii wywoławczej
- **ciemnozielone** wpisy — znaki z bazy SCP (aktywuje się po wpisaniu ≥ 2 znaków)
- `Tab` / `Space` wstawia podświetloną pozycję z historii

## Normalny QSO workflow

1. wpisz `Call`
2. `Space`
3. wpisz `RST`
4. `Space`
5. wpisz `Comments`
6. `Enter`

## Contest workflow

1. załaduj zawody
2. wpisz `Call`
3. `Space`
4. wpisz odebraną wymianę
5. `Enter`

Ważne:

- raport nadawany ustawia się automatycznie
- `TX Exchange` jest wyliczane z definicji zawodów
- `EXCHANGE_SENT=#` zawsze daje `1`, `2`, `3`...

## Ręczne ustawienie częstotliwości

1. w polu `Call` wpisz samą liczbę, np. `7020`
2. naciśnij `Enter`

## Najczęstsze komendy tekstowe

| Komenda | Zastosowanie |
| --- | --- |
| `export` | szybki eksport CSV + ADIF |
| `export mojlog.adi` | eksport z własną nazwą ADIF |
| `exportcab` | szybki eksport Cabrillo |
| `invalid` | oznacz ostatnie QSO jako INVALID |
| `logs` | pokaż zapisane logi |
| `openlog 12` | otwórz log po ID |
| `contest contest_defs/cq_wpx_cw.conf` | załaduj preset zawodów |
| `contest none` | wyłącz zawody |
| `technique SO1R` | ustaw SO1R |
| `technique SO2V` | ustaw SO2V |
| `technique SO2R` | ustaw SO2R |

## Konfiguracja sieci

Okno `Menu -> Network Config` pozwala ustawić podstawowe parametry pracy sieciowej:

- `NET_ENABLED`: czy sync ma być aktywny po starcie,
- `NET_STATION_ID`: nazwa / identyfikator stacji,
- `NET_ROLE`: `client` albo `server`,
- `NET_SERVER_HOST`: adres serwera centralnego tylko dla trybu `client`,
- `NET_SERVER_PORT`: port TCP serwera; w trybie `server` to port nasłuchu aplikacji,
- `NET_TLS`: włączenie warstwy TLS,
- `NET_SHARED_KEY`: wspólny klucz autoryzacji aplikacyjnej,
- `NET_TLS_PEER_FINGERPRINT`: fingerprint certyfikatu serwera dla pinningu klienta.

Ważne:

- po zapisaniu okna konfiguracji sync jest restartowany automatycznie,
- jeśli `NET_ENABLED=0`, ustawienia są tylko zapisywane, ale sync pozostaje wyłączony,
- przy pierwszym połączeniu TLS fingerprint serwera może zostać zapisany automatycznie do `logger.conf`.

## Sekcje NET_* w logger.conf

Pola sieciowe są zapisywane w osobnych sekcjach:

- `# Network Basic`
	- `NET_ENABLED`
	- `NET_ROLE`
	- `NET_STATION_ID`
	- `NET_SERVER_HOST`
	- `NET_SERVER_PORT`
- `# Network Security`
	- `NET_AUTH_TOKEN`
	- `NET_SHARED_KEY`
	- `NET_TLS_CERT_FILE`
	- `NET_TLS_KEY_FILE`
	- `NET_TLS_PEER_FINGERPRINT`
	- `NET_TLS`
- `# Network Runtime`
	- `NET_SYNC_INTERVAL_MS`
	- `NET_HEARTBEAT_SEC`
	- `NET_RETRY_MIN_MS`
	- `NET_RETRY_MAX_MS`
	- `NET_RATE_LIMIT_WINDOW_SEC`
	- `NET_RATE_LIMIT_BURST`
	- `NET_MAX_FRAME_BYTES`

## Co sprawdzić, gdy coś nie działa

- brak CTY: sprawdź `wl_cty.dat`
- brak CAT: sprawdź `CAT_*` albo `CAT2_*`
- zła wymiana contestowa: sprawdź `CONTEST_DEF_FILE`, `EXCHANGE_SENT`, `CONTEST_TX_EXCHANGE`
- brak CW: sprawdź `CW_DEVICE`, `CW_KEYER_LINE`, `cw_keys.ini`
- brak sync: sprawdź `NET_ENABLED`, `NET_ROLE`, `NET_SERVER_HOST`, `NET_SERVER_PORT`
- błąd TLS/pinning: sprawdź `NET_TLS`, `NET_SHARED_KEY`, `NET_TLS_PEER_FINGERPRINT`

## UI pod ręką

- wszystkie akcje są zebrane w jednym menu `Menu`
- `Menu -> Show CAT/CW Config` (lub `Ctrl+F9`) pokazuje lub ukrywa panel konfiguracji CAT/CW
- bandmapa (lista spotów z bieżącego pasma) jest pod tabelą DXCluster, podwójne kliknięcie ustawia QRG
- obok `Run/S&P` stale widać lampki `CAT ON/OFF` i `CW ON/OFF` (CW pod CAT)

## Gdzie szukać pełnych opisów

- [konfiguracja-i-zawody.md](konfiguracja-i-zawody.md)
- [klawiszologia.md](klawiszologia.md)
- [przykladowe-konfiguracje.md](przykladowe-konfiguracje.md)