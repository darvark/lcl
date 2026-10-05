# LCL Logger

LCL Logger to aplikacja do prowadzenia logu zawodów krótkofalarskich. Aplikacja opiera się na współdzielonej logice kontrolera, z framework Qt jako warstwą interfejsu użytkownika.

Dodatkowa dokumentacja:

- [docs/architektura.md](docs/architektura.md)
- [docs/konfiguracja-i-zawody.md](docs/konfiguracja-i-zawody.md)
- [docs/klawiszologia.md](docs/klawiszologia.md)
- [docs/przykladowe-konfiguracje.md](docs/przykladowe-konfiguracje.md)
- [docs/sciaga-operatora.md](docs/sciaga-operatora.md)
- [docs/siec-centralny-log.md](docs/siec-centralny-log.md)
- [docs/siec-centralny-log-projekt.md](docs/siec-centralny-log-projekt.md) (archiwalna dokumentacja historyczna)
- [docs/self-signed-tls-operacja.md](docs/self-signed-tls-operacja.md) (token, pin TLS i backup)

## Status prac i stabilizacji

Aktualny stan repozytorium obejmuje obsługę VHF i runtime synchronizacji. Pełny
zestaw jednostkowy oraz zarejestrowane testy CTest przechodzą. Ostrzeżenia
OpenSSL 3 dotyczą użycia przestarzałego API RSA przy generowaniu self-signed
certyfikatu.

- wprowadzono obsługę VHF/UHF/SHF z lokalizacją i punktacją odległościową,
- dodano nowe typy exchange i multiplierów dla `SERIAL_GRID` / `GRID_PER_BAND`,
- wprowadzono testy regresyjne dla VHF,
- poprawiono archiwizację i przełączanie named logs, wejście wymiany z lokatorem oraz testy sugestii.

Sieciowe bramki CTest obejmują migracje, protokół, routing `shared_log_id`,
testy fault injection i test wieloprocesowy serwera z dwoma klientami.

## Status pracy sieciowej

Runtime klient-serwer jest aktywny: klient ma okresowego workera poza UI,
trwały outbox i catch-up, a serwer obsługuje centralne operacje oraz rezerwacje
seriali. TLS jest wymagany domyślnie; klient musi mieć wcześniej skonfigurowany
fingerprint SHA-256 i nie ma automatycznego TOFU. Serwer generuje
`shared_log_id`; klienta jawnie paruje się z ID widocznym w `net status` przez
`netsync pair <shared_log_id>` przy wyłączonej synchronizacji.
Opcjonalny mTLS pozwala serwerowi wymagać certyfikatu klienta podpisanego przez
skonfigurowane CA. `net status` pokazuje liczniki sesji, żądań, błędów
uwierzytelniania/TLS, rate limitu i blokad IP.
Pełne ustawienia sieciowe (`NET_*`) są w `logger.conf` bieżącego użytkownika
(`~/.config/contest-logger/logger.conf`, prawa `0600`) i w formularzu ustawień
sieciowych UI. Dostępne są też komendy sterujące:
`net on|off|status`, `net role client|server`, `netsync pair <shared_log_id>`.
Plain TCP wymaga
jawnego `NET_ALLOW_INSECURE_LAN=1` i może być użyty wyłącznie w zaufanej,
odizolowanej sieci. Zobacz [konfigurację synchronizacji](docs/konfiguracja-i-zawody.md#centralny-log-i-synchronizacja)
i [procedurę TLS](docs/self-signed-tls-operacja.md).

Ograniczenie v1: jedna instancja serwera obsługuje jeden aktywny plik SQLite i
jego `shared_log_id`; używaj osobnej instancji/portu dla każdego wspólnego logu.
Blacklist jest tymczasowy, tylko IPv4 i przechowywany w pamięci procesu. mTLS
opiera się na ręcznie dostarczonych certyfikatach; enrollment i revocation list
nie są obsługiwane.

## Testowanie i konfiguracja runtime

Projekt ma dwa obszary konfiguracji: plik lokalny `logger.conf` w bieżącym katalogu roboczym oraz katalog runtime w `$HOME/.config/contest-logger`. W praktyce aplikacja priorytetowo czyta plik z katalogu roboczego, a dopiero gdy go nie ma, korzysta z runtime. To pozwala uruchamiać testy izolowane w katalogach tymczasowych bez ładowania globalnych ustawień repozytorium.

Testy należy uruchamiać przez:

```bash
cmake -S . -B build
cmake --build build -j2
./build/unit_tests
ctest --test-dir build --output-on-failure
```

## Architektura

```mermaid
flowchart LR
	subgraph UI[Qt UI]
		Qt[qt_frontend.cpp\nLoggerQtWindow]
	end

	subgraph Controller[Kontroler]
		App[app_controller.c\nAppController]
		State[AppRenderState\npola wejściowe, stan radia, status, wymiana]
	end

	subgraph Core[Moduły domenowe]
		QSO[qso.c\ntworzenie i walidacja QSO]
		Contest[contest.c\nparser definicji zawodów]
		Stats[stats.c\nstatystyki zawodów i trybów]
		Suggestion[suggestion.c\npodpowiedzi historii znaków]
		Export[export.c\nCSV, ADIF, Cabrillo]
		Maidenhead[maidenhead.c\nkonwersja lokatora]
	end

	subgraph Services[Usługi runtime]
		CAT[cat.c\nczęstotliwość i tryb radia]
		DXC[dxcluster.c\nworker klastra i feed spotów]
		CTY[cty.c\nwyszukiwanie DXCC i stref]
		SCP[scp.c\nbaza Super Check Partial]
		Config[config.c\nładowanie i zapis logger.conf]
	end

	subgraph Persistence[Persystencja SQLite]
		DB[db.c\naktywny log + przełączanie plików DB\nhistoria znaków]
	end

	subgraph External[Dane zewnętrzne]
		Conf[logger.conf]
		ContestDefs[contest_defs/*.conf lub własny plik zawodów]
		DXLogRaw[surowa definicja DXLog]
		ImportedDef[znormalizowany plik .conf zawodów]
		CTYFile[wl_cty.dat]
		SQLite[(logs/*.db)]
		Cluster[(serwer DXCluster)]
		Rig[(Radio przez Hamlib)]
	end

	Qt -->|klawisze i komendy| App
	App -->|migawka renderowania| State
	Qt -->|renderuje| State
	App --> QSO
	App --> Contest
	App --> Stats
	App --> Suggestion
	App --> Export
	App --> CAT
	App --> DXC
	App --> CTY
	App --> SCP
	App --> Config
	App --> DB
	QSO --> CTY
	QSO --> DB
	Stats --> QSO
	Export --> DB
	Config --> Conf
	Contest --> ContestDefs
	Contest --> ImportedDef
	DXLogRaw --> Contest
	CTY --> CTYFile
	SCP --> SQLite
	DB --> SQLite
	DXC --> Cluster
	CAT --> Rig
```

Qt pozostaje cienką warstwą: tłumaczy wejście klawiatury na akcje kontrolera i rysuje aktualny `AppRenderState`.

`app_controller.c` to warstwa orkiestracji. Zarządza trybem contestowym, stanem dual-radio, integracją CAT, cyklem życia DXCluster, komendami eksportu, odświeżaniem CTY i operacjami na logach (każdy jako osobny plik SQLite), delegując przechowywanie danych i reguły domenowe do modułów głównych.

Definicje zawodów rozdzielają wymianę odbieraną (`FIELD`) i nadawaną (`EXCHANGE_SENT`). `contest.c` je wczytuje, `app_controller.c` stosuje podczas wpisywania QSO, `qso.c` zapisuje obie wartości, a `export.c` wykorzystuje je w eksporcie.


![lnx_logger](./src/lnx_logger.png "LNX Logger")

## Co robi aplikacja

- Rejestruje QSO z desktopowego interfejsu Qt
- Używa podzielonych pól wpisywania: `call`, `exch`
- Przełącza się w tryb contestowy z generowaną nadawaną wymianą po załadowaniu definicji zawodów
- Umożliwia ręczne ustawienie częstotliwości przez wpisanie samych cyfr w polu `call`
- Wyświetla informacje DXCC, strefę CQ i ITU podczas wpisywania znaku
- Pokazuje panel „Suggestions / SCP" w prawym górnym rogu tabeli logów, łączący dopasowania z historii znaków (ciemnożółty) i z bazy Super Check Partial (ciemnozielony) w jednej liście
- Pobiera i ładuje bazę Super Check Partial (`MASTER.SCP`) z supercheckpartial.com do wyszukiwania check partial w czasie rzeczywistym
- Pokazuje panel wiadomości CW między paskiem statusu a panelem DXCC — F1–F10 jako klikalne przyciski z rozwiniętym tekstem wiadomości dla aktualnego trybu RUN/S&P; kliknięcie wysyła wiadomość przez keyer
- Łączy się z serwerem DXCluster, pokazuje odebrane spoty w oknie klastra i czysto zatrzymuje workera klastra przy zamknięciu aplikacji
- Pokazuje listę bandmapy dla bieżącego pasma (spoty posortowane po częstotliwości) z szybkim "grab spot"
- Bandmapa automatycznie pomija bliskie duplikaty tego samego znaku na bardzo zbliżonych częstotliwościach (tolerancja 2kHz)
- Śledzi proste statystyki
- Przechowuje log QSO i historię znaków w SQLite
- Każdy nowy log tworzy jako osobny, niezależny plik SQLite
- Eksportuje dane logu do plików CSV i ADIF
- Eksportuje log contestowy do Cabrillo z użyciem definicji zawodów w stylu DXLog
- Importuje surowe definicje DXLog i normalizuje je do lokalnego formatu zawodów
- Obsługuje zasady wymiany QTC w stylu WAE z konfigurowalną stroną nadawczą i punktacją za QTC (wciąż w trakcie testowania)
- Obsługuje techniki operatorskie SO1R, SO2V i SO2R
- Automatycznie przywraca zapisaną definicję zawodów przy ponownym otwarciu archiwalnego lub nazwanego logbooka
- Utrwala ścieżkę definicji zawodów per logbook i odtwarza aktywny stan kontestu bez ręcznej re-selekcji
- Aktualizuje prędkość keyera CW natychmiast przez kontrolkę runtime w głównym oknie
- Używa jednego menu głównego z pogrupowanymi akcjami

## Funkcjonalności

- Scalony panel „Suggestions / SCP": podpowiedzi historii znaków (ciemnożółty) i dopasowania SCP (ciemnozielony) w jednej liście
- Pobieranie bazy SCP z supercheckpartial.com przez akcję menu „Update SCP (Check Partial)"
- Klikalne przyciski wiadomości CW (F1–F10) w dedykowanym panelu między paskiem statusu a DXCC; tekst pokazuje rozwiniętą wiadomość dla aktywnego trybu RUN/S&P
- Podzielone wpisywanie QSO z polami call/rst/comments i przechodzeniem przez Spację
- Tryb contestowy z konfigurowalnymi polami wymiany i inkrementalną lub statyczną nadawaną wymianą
- Obsługa częstotliwości z uwzględnieniem CAT (żywa częstotliwość radia przy połączeniu, ręczny fallback)
- Opcjonalne pobieranie trybu pracy z bieżącego trybu radia przez CAT
- Stan per-radio: fokus, RUN/S&P, kontroler SO2R
- Lokalne wyszukiwanie DXCC z bazy CTY
- Wyświetlanie statusu i spotów DXCluster z bezpieczną ścieżką zatrzymania
- Nawigacja po bandmapie klawiaturą (`Ctrl+Up`/`Ctrl+Down`) i strojenie na aktywację spotu
- Oznaczanie duplikatów jako `invalid` bez pomijania ich w eksporcie
- Eksport CSV/ADIF z komentarzami i własną nazwą pliku ADIF, w tym wszystkie kontakty, także duplikaty
- Eksport Cabrillo (`exportcab`) z metadanymi kategorii z definicji zawodów
- Parser definicji zawodów w stylu DXLog (`contest <plik>`) z deklaracjami pól
- Import surowych definicji DXLog do znormalizowanego lokalnego formatu
- Obsługa punktacji QTC dla zawodów WAE przez `QTC_SENDER` i `POINTS_PER_QTC`
- Podwójne profile CAT Hamlib dla SO2R (`CAT_*` + `CAT2_*`)
- Stałe odznaki połączenia CAT/CW obok kontrolek RUN/S&P (`CAT ON/OFF`, `CW ON/OFF`)
- Przełączanie widoczności panelu konfiguracji CAT/CW z menu (`Show CAT/CW Config`)
- Globalny skrót `Ctrl+F9` do pokazywania/ukrywania panelu CAT/CW
- Jednoklawiszowa aktualizacja bazy CTY z internetu
- Aktualizacja bazy SCP z supercheckpartial.com (`Menu → Update SCP (Check Partial)`)
- Logbook i historia znaków w SQLite z nadpisaniem ścieżki przez `LOGGER_DB_PATH`
- Akcja tworzenia nowego pustego logu
- Niezależne nazwane logi jako osobne pliki `logs/<nazwa>.db`, wybierane po ID z listy lub nazwie
- Tworzenie nowego logu z opcjonalnym wyborem presetu zawodów w UI Qt
- Dialog konfiguracji zawodów z zapisem do pliku i dedykowanym skrótem (`Ctrl+F8`)

## Wymagania

- Kompilator C (GCC lub Clang)
- CMake
- make
- obsługa wątków pthread
- curl lub wget (do pobierania baz CTY i SCP)

Opcjonalnie dla frontendu GUI:

- pakiet deweloperski Qt Widgets (Qt 5 lub Qt 6)
- pakiet deweloperski Hamlib (dla obsługi CAT)

Na systemach Debian/Ubuntu zainstaluj wymagane pakiety:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake
```

Transport TLS wymaga OpenSSL podczas budowania. Na Debian/Ubuntu doinstaluj
`libssl-dev`; bez niego aplikacja nadal się buduje, ale `NET_TLS=1` nie będzie
dostępne. Fedora używa `openssl-devel`, a Arch pakietu `openssl`.

### Skrypty instalacyjne (Ubuntu/Debian, Fedora, Arch Linux)

W repozytorium są gotowe skrypty instalujące zależności build + GUI:

```bash
chmod +x scripts/install_*.sh
```

Ubuntu/Debian:

```bash
./scripts/install_ubuntu_debian.sh
```

Fedora:

```bash
./scripts/install_fedora.sh
```

Arch Linux:

```bash
./scripts/install_arch.sh
```

### Skrypt tworzenia paczek

Jest dostępny wspólny skrypt do budowania paczek dla Debian/Ubuntu, Fedora i Arch Linux:

```bash
chmod +x scripts/build_packages.sh
./scripts/build_packages.sh <target>
```

Dostępne targety:

- `debian` (pakiet `.deb`)
- `fedora` (pakiet `.rpm`)
- `arch` (pakiet `.pkg.tar.zst`)
- `all` (buduje wszystkie formaty, które są dostępne na aktualnym systemie)

Przykłady:

```bash
./scripts/build_packages.sh debian
./scripts/build_packages.sh fedora
./scripts/build_packages.sh arch
./scripts/build_packages.sh all
```

Gotowe paczki są zapisywane w katalogu `dist/`.

Po instalacji pakietów zbuduj projekt standardowo:

```bash
cmake -S . -B build
cmake --build build
```

## Budowanie

Z katalogu głównego projektu:

```bash
cmake -S . -B build
cmake --build build
```

Pliki wykonywalne są tworzone w katalogu `build`:

- `logger` (GUI)

## Testy regresji

Projekt zawiera zestaw testów regresji w `tests/regression` weryfikujący podstawowe zachowanie poza UI:

- parsowanie konfiguracji i wartości domyślne
- ładowanie bazy CTY i wyszukiwanie znaków
- parsowanie QSO, wykrywanie pasma/trybu i przełączanie flagi invalid
- agregację statystyk
- zawartość eksportów CSV i ADIF
- konwersję lokatora Maidenhead

Uruchomienie testów:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Testy jednostkowe

Projekt zawiera też testy jednostkowe w `tests/unit` weryfikujące eksportowane funkcje modułów poza UI:

- `app_controller`: wspólny przepływ klawiszy/stanu niezależny od frontendu
- `config`: `config_load`
- `cty`: `cty_load`, `cty_lookup`
- `qso`: `qso_init`, `qso_add`, `qso_mark_invalid`, `detect_band`, `detect_mode`
- `stats`: `stats_update`
- `export`: `export_csv`, `export_adif`
- `maidenhead`: `locator_to_latlon`
- `dxcluster`: `dxcluster_set_status`

Renderowanie UI nie jest pokryte automatycznymi testami i powinno być weryfikowane ręcznie.

Uruchomienie wszystkich testów (regresja + jednostkowe):

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Uruchomienie

```bash
cd build
./logger
```

## Konfiguracja

Aplikacja odczytuje plik konfiguracyjny `logger.conf` z bieżącego katalogu roboczego.

Przykładowa konfiguracja:

```ini
LAT=21.104127
LON=37.300154
LOCATOR=AA00AA

DXC_HOST=dx.da0bcc.de
DXC_PORT=7300
DXC_CALL=AAXAAA
```

### Konfiguracja i definicje zawodów

Szczegółowa dokumentacja pól: [docs/konfiguracja-i-zawody.md](docs/konfiguracja-i-zawody.md).

Gotowe przykłady dla `SO1R`, `SO2V` i `SO2R`: [docs/przykladowe-konfiguracje.md](docs/przykladowe-konfiguracje.md).

Najważniejsze zasady:

- `CONTEST_DEF_FILE` może wskazywać lokalny plik lub preset z `contest_defs/`
- `FIELD` określa typ i etykietę wymiany odbieranej; `EXCHANGE_SENT` określa wymianę nadawaną
- `EXCHANGE_SENT=#` zawsze oznacza numerację inkrementalną `1`, `2`, `3`...
- nadawana wymiana jest wyłącznie generowana z definicji zawodów; nie ma już przeładowania z `logger.conf`

## Obsługa

Pełna dokumentacja skrótów klawiszowych i workflow: [docs/klawiszologia.md](docs/klawiszologia.md).

Najkrótsza wersja do codziennej pracy: [docs/sciaga-operatora.md](docs/sciaga-operatora.md).

Najważniejsze zasady operacyjne:

- `F1..F10` wysyłają wiadomości CW zdefiniowane w `cw_keys.ini`; klawisze są też wyświetlane jako klikalne przyciski w panelu między statusem a DXCC
- opcjonalny `CW_ESM=1` w `logger.conf` włącza Enter Sends Message dla CW pod klawiszem `Enter` (decyzje: RUN/S&P + aktywne pole + pusty/niepusty `Call` i `Exchange`, akcje przez istniejące makra F1-F10)
- `Ctrl+F2` tworzy nowy log i umożliwia przypisanie presetu zawodów
- `Ctrl+F8` otwiera dialog konfiguracji zawodów
- `Ctrl+F9` pokazuje lub ukrywa panel konfiguracji CAT/CW
- `Ctrl+Shift+E` włącza albo wyłącza CW ESM
- `Ctrl+Up` i `Ctrl+Down` przechodzą do poprzedniego/następnego spotu na bandmapie bieżącego pasma i stroją częstotliwość
- `Menu → Show CAT/CW Config` pokazuje lub ukrywa panel konfiguracji CAT/CW
- stan połączeń CAT i CW jest zawsze widoczny jako dwie odznaki obok kontrolek RUN/S&P
- `Spacja` przechodzi między widocznymi polami wejściowymi, nie wstawia spacji do wpisywanej linii
- w trybie contestowym wpisujesz `Call` i odebraną `Exchange`, a nadawana wymiana jest generowana z definicji zawodów
- panel „Suggestions / SCP" pokazuje podpowiedzi z historii (ciemnożółty) i dopasowania SCP (ciemnozielony); `Tab` lub `Spacja` wstawiają zaznaczoną pozycję z historii
- `contest import <plik_dxlog> [plik_wyjściowy]` importuje surową definicję DXLog do znormalizowanego lokalnego formatu (domyślny plik wyjściowy: `contest.conf`) i ładuje ją natychmiast
- `contest import-only <plik_dxlog> [plik_wyjściowy]` importuje i zapisuje znormalizowany plik bez ładowania i bez zmiany aktywnego `CONTEST_DEF_FILE`
- po imporcie szczegóły ostrzeżeń o ignorowanych regułach DXLog są wyświetlane w linii informacyjnej

## Pliki danych

Program oczekuje pliku bazy DXCC o nazwie `wl_cty.dat` w bieżącym katalogu roboczym lub w katalogu build. Po naciśnięciu `Ctrl+F7` plik `wl_cty.dat` jest pobierany i zastępowany w bieżącym katalogu roboczym.

Log QSO i historia znaków są przechowywane domyślnie jako osobne pliki SQLite w katalogu runtime: `$HOME/.config/contest-logger/logs/`.

- `newlog Nazwa` tworzy nową bazę `Nazwa.db` i przełącza aplikację na ten plik.
- `newlog` (bez nazwy) tworzy nowy plik z automatyczną nazwą w formacie `log_YYYYMMDD_HHMMSS.db`.
- `logs` pokazuje listę plików logów (ID z listy są numeracją widoku, używaną przez `openlog <id>`).

Migracja z wcześniejszych wersji:

- przy pierwszym uruchomieniu nowego mechanizmu, jeśli nie istnieje jeszcze `logs/Default Log.db`, a istnieje stare `logger.db`, program automatycznie kopiuje starą bazę do `logs/Default Log.db`.
- migracja jest bezpieczna: źródłowy `logger.db` nie jest usuwany.

Ustaw `LOGGER_DB_PATH`, jeśli chcesz ręcznie wymusić pojedynczy plik SQLite poza tym mechanizmem. Przy pierwszym uruchomieniu domyślnej bazy istniejące wpisy z `call_history.txt` są importowane do SQLite, jeśli baza jest pusta.

Baza Super Check Partial jest przechowywana w pliku `MASTER.SCP` w bieżącym katalogu roboczym. Pobierz lub zaktualizuj ją przez `Menu → Update SCP (Check Partial)` albo ręcznie z https://www.supercheckpartial.com/downloads/MASTER.SCP. Wyszukiwanie check partial aktywuje się po wpisaniu co najmniej 2 znaków w polu `call`.

Pliki `logger.conf` i `wl_cty.dat` pozostają plikami tekstowymi.

## Uwagi

- Aplikacja używa Qt Widgets, więc jest przeznaczona dla środowisk desktopowych.
- Łączność z DXCluster zależy od skonfigurowanego hosta, portu i dostępu do sieci.
- Zamknięcie aplikacji uruchamia wspólną ścieżkę wyłączania, która zatrzymuje wątek workera DXCluster przed zamknięciem bazy danych.
