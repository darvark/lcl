# TLS i bezpieczeństwo Central Log

## Cel

Dokument opisuje aktywny model uwierzytelniania i TLS. Synchronizacja zawsze
wymaga silnego tokenu aplikacyjnego oraz TLS. Plain TCP jest zablokowany, chyba
że operator jawnie zezwoli na niego dla zaufanej, odizolowanej sieci.

- serwer utrzymuje lokalny self-signed certyfikat i klucz,
- klient porównuje certyfikat z jawnym pinem; brak pinu blokuje połączenie,
- `NET_AUTH_TOKEN` (lub `NET_SHARED_KEY`) jest wymagany i nie może być pusty.
- serwer odrzuca sekret krótszy niż 32 znaki lub o zbyt małej różnorodności.

## Pliki i ustawienia

W `logger.conf` obsluzone sa pola:

- `NET_TLS=1`
- `NET_ALLOW_INSECURE_LAN=0`
- `NET_TLS_CERT_FILE=logger_net_cert.pem`
- `NET_TLS_KEY_FILE=logger_net_key.pem`
- `NET_TLS_PEER_FINGERPRINT=`
- `NET_TLS_REQUIRE_CLIENT_CERT=0`
- `NET_TLS_CLIENT_CA_FILE=`
- `NET_TLS_CLIENT_CERT_FILE=`
- `NET_TLS_CLIENT_KEY_FILE=`
- `NET_AUTH_TOKEN=<silny sekret współdzielony z klientami>`
- `NET_RATE_LIMIT_WINDOW_SEC=1`
- `NET_RATE_LIMIT_BURST=32`
- `NET_MAX_FRAME_BYTES=65536`

Znaczenie:

- `NET_TLS_CERT_FILE`: sciezka PEM certyfikatu serwera.
- `NET_TLS_KEY_FILE`: sciezka PEM klucza prywatnego serwera.
- `NET_TLS_PEER_FINGERPRINT`: pin SHA-256 certyfikatu serwera po stronie klienta.
- `NET_TLS_REQUIRE_CLIENT_CERT=1`: opcjonalne mTLS; serwer wymaga certyfikatu klienta zaufanego przez `NET_TLS_CLIENT_CA_FILE`.
- `NET_TLS_CLIENT_CA_FILE`: ścieżka do PEM CA weryfikującego certyfikaty klientów (serwer).
- `NET_TLS_CLIENT_CERT_FILE` i `NET_TLS_CLIENT_KEY_FILE`: para certyfikatu i klucza tożsamości klienta mTLS.
- `NET_ALLOW_INSECURE_LAN=1`: jawny wyjątek dla zaufanej, odizolowanej sieci;
	nigdy nie ustawiaj go dla Internetu, Wi-Fi gościnnego ani sieci współdzielonej.

Plik konfiguracji jest prywatny dla konta uruchamiającego aplikację:
`~/.config/contest-logger/logger.conf`, z prawami `0600`. Aplikacja naprawi
szersze prawa pliku należącego do bieżącego użytkownika; odmówi odczytu pliku
innego właściciela lub dowiązania symbolicznego. `config_save` tworzy plik z
prawami `0600`. Pakiety nie zawierają wspólnego `logger.conf`, który mógłby
ujawnić token wszystkim użytkownikom hosta. Generator konfiguracji również
ustawia `umask 077`.

Uruchomienie sieci wymaga `NET_TLS=1` i pinu fingerprintu po stronie klienta.
Jeżeli TLS jest niedostępny, ustaw `NET_ALLOW_INSECURE_LAN=1` wyłącznie po
świadomej ocenie, że cała sieć i hosty są zaufane.

## Parowanie wspólnego logu

Serwer przypisuje `shared_log_id` do aktywnego pliku SQLite przy pierwszym
starcie i pokazuje go w `net status` jako `shared=sl-...`. Na każdym kliencie,
przy zatrzymanej synchronizacji i otwartym docelowym logbooku, wykonaj:

```text
netsync pair sl-<32 małe znaki szesnastkowe>
```

Polecenie zapisuje pairing w `logger.conf` i lokalnym `sync_identity`. Istniejący
różny ID nie jest nadpisywany; wybierz właściwy log lub wykonaj świadomą
migrację zamiast scalać bazy. HELLO oraz każda ramka operacyjna przenoszą ID, a
serwer sprawdza je przed apply, pull/catch-up, rezerwacją i commit. Klient
odrzuca odpowiedź z innym ID przed apply lub ACK.

Jedna instancja serwera obsługuje jeden aktywny plik SQLite. Dla różnych
wspólnych logów uruchom osobne instancje serwera i osobne porty.

## Zmiana aktywnego logu

Przełączanie/clear logu jest zablokowane, gdy synchronizacja jest włączona, aby
worker lub aktywna sesja nie kontynuowały pracy na innej bazie. Zatrzymaj
`net off` na serwerze i klientach, otwórz/utwórz docelowy log, uruchom serwer i
odczytaj jego `shared_log_id` przez `net status`. Następnie na każdym kliencie
otwórz właściwy log, wykonaj `netsync pair <shared_log_id>` i dopiero wtedy
`net on`. Istniejący różny pairing nie jest automatycznie nadpisywany.

## Pierwszy start serwera

Przy pierwszym uruchomieniu z `NET_TLS=1`:

1. serwer sprawdza istnienie `NET_TLS_CERT_FILE` i `NET_TLS_KEY_FILE`,
2. jezeli ich nie ma, generuje self-signed certyfikat RSA 2048,
3. zapisuje oba pliki w formacie PEM,
4. uzywa ich przy kolejnych startach.

To oznacza stabilny fingerprint, o ile pliki nie zostana usuniete lub podmienione.

## Konfiguracja pinu klienta

1. Uruchom serwer raz z `NET_TLS=1`, aby wygenerował certyfikat i klucz.
2. Przekaż fingerprint klientom zaufanym kanałem administracyjnym (np. osobiście lub przez zarządzany menedżer sekretów). Nie pobieraj pinu automatycznie z połączenia, które dopiero weryfikujesz.
3. Odczytaj SHA-256 fingerprint z certyfikatu serwera:

```bash
openssl x509 -in logger_net_cert.pem -noout -fingerprint -sha256
```

4. Wpisz wartość po `=` do `NET_TLS_PEER_FINGERPRINT` w każdym klienckim `logger.conf`, zachowując dwukropki.
5. Ustaw ten sam `NET_AUTH_TOKEN` na serwerze i klientach.

Klient nie wykona TLS handshake przez warstwę synchronizacji, jeśli pin jest
pusty. Zmiana certyfikatu bez aktualizacji pinu kończy połączenie błędem.

## Wzajemne uwierzytelnianie TLS

mTLS jest opcjonalne i nie zastępuje pinu serwera ani tokenu aplikacyjnego.
Certyfikat klienta musi być podpisany przez CA zaufane przez serwer:

- serwer: ustaw `NET_TLS=1`, `NET_TLS_REQUIRE_CLIENT_CERT=1` oraz
	`NET_TLS_CLIENT_CA_FILE=/ścieżka/ca-klientów.pem`,
- klient: ustaw `NET_TLS=1`, `NET_TLS_CLIENT_CERT_FILE=/ścieżka/klient.pem` i
	`NET_TLS_CLIENT_KEY_FILE=/ścieżka/klient.key`,
- skonfiguruj także dotychczasowy `NET_TLS_PEER_FINGERPRINT` i
	`NET_AUTH_TOKEN`.

Klucz klienta musi należeć do bieżącego użytkownika i mieć prawa `0600`.
Niepoprawny, brakujący lub niezaufany certyfikat zamyka handshake.

## Metryki i blokada IP

`net status` pokazuje aktywne sesje oraz liczniki żądań, błędów uwierzytelniania
i TLS, przekroczeń rate limitu oraz blokad. Trzy naruszenia limitu z tego samego
IPv4 w skonfigurowanym oknie blokują ten adres na 60 sekund. Liczniki i blokady
są przechowywane w pamięci procesu; restart serwera je czyści. Adresy klientów
nie są wypisywane.

## Kolejne polaczenia klienta

Przy nastepnych polaczeniach:

- klient porownuje otrzymany fingerprint z `NET_TLS_PEER_FINGERPRINT`,
- mismatch kończy handshake błędem i sesja sync nie startuje.

## Rotacja certyfikatu

Certyfikat self-signed wygasa po roku; zaplanuj rotację przed wygaśnięciem, a
także po podejrzeniu ujawnienia klucza. Klucz prywatny serwera ma prawa `0600`;
aplikacja je egzekwuje i nie uruchomi serwera z kluczem obcego właściciela,
dowiązaniem symbolicznym ani niepoprawnym/niekompletnym zestawem plików.

Procedura rotacji:

1. Uzgodnij okno serwisowe i zatrzymaj serwer.
2. Zrób chronioną kopię starego certyfikatu i klucza. Usuń oba pliki z aktywnych
	ścieżek albo przygotuj nową parę PEM; nie zostawiaj tylko jednego starego pliku.
3. Uruchom serwer. Jeśli oba pliki nie istnieją, aplikacja utworzy nową parę.
4. Odczytaj nowy fingerprint i przekaż go klientom zaufanym kanałem.
5. Zaktualizuj `NET_TLS_PEER_FINGERPRINT` na wszystkich klientach i uruchom sync.
	Klienci ze starym pinem nie połączą się z nowym certyfikatem.

Nie kasuj jedynej kopii starego klucza przed zakończeniem aktualizacji pinów i
sprawdzeniem połączeń.

## Rotacja tokenu

Rotuj token co 180 dni oraz natychmiast po podejrzeniu jego ujawnienia. Protokół
nie obsługuje nakładających się tokenów, więc zmiana wymaga skoordynowanej
przerwy:

1. Zatrzymaj synchronizację na serwerze i wszystkich klientach.
2. Wygeneruj nowy token, np. `openssl rand -base64 48 | tr -d '\n'`.
3. Zaktualizuj `NET_AUTH_TOKEN` (i zgodny alias `NET_SHARED_KEY`, jeśli jest
	używany) na serwerze i wszystkich klientach. Nie przesyłaj go w e-mailu,
	zgłoszeniu ani logach.
4. Sprawdź prawa `0600` wszystkich plików `logger.conf`, uruchom serwer, a potem
	klientów. Nie wznawiaj klientów, zanim serwer nie używa nowego tokenu.
5. Zweryfikuj połączenie każdej stacji i bezpiecznie usuń stare kopie tokenu.

## Token i ograniczenia

- mTLS jest opcjonalne; certyfikaty klienta i CA muszą być dostarczone oraz rotowane ręcznie,
- brak CRL/OCSP,
- brak automatycznego enrollment i lifecycle zewnętrznego CA,
- fingerprint pinning wymaga bezpiecznego kanału dystrybucji pinu,
- metryki i blokady IP są tylko w pamięci procesu; blacklist dotyczy IPv4 i wygasa po 60 sekundach,
- bez TLS token jest przesyłany jawnie; wyjątek plain TCP jest możliwy wyłącznie
	przez `NET_ALLOW_INSECURE_LAN=1` w zaufanej, izolowanej sieci.

Wygeneruj token bez spacji i nowej linii, np.:

```bash
openssl rand -base64 48 | tr -d '\n'
```

Zapisz identyczną wartość w `NET_AUTH_TOKEN` na serwerze i klientach. Pliki
konfiguracyjne i prywatny klucz serwera chroń uprawnieniami ograniczonymi do
operatora usługi (`chmod 600`).

## Rekomendacja operacyjna

Dla deploymentu klubowego/LAN:

- trzymac pliki cert/key poza katalogami tymczasowymi,
- zbackupowac PEM-y razem z konfiguracja serwera,
- zweryfikować fingerprint poza połączeniem aplikacji i skonfigurować go na klientach przed startem,
- utrzymywać ten sam silny token po obu stronach i rotować go koordynując serwer oraz klientów,
- pozostawić `NET_ALLOW_INSECURE_LAN=0`; TLS jest wymagany domyślnie.

## Backup i odtworzenie

Zatrzymaj serwer i klientów przed zwykłym kopiowaniem plików. Zachowaj razem:

- serwerową bazę SQLite i katalog `logs/`,
- klientowskie bazy SQLite (zawierają lokalny outbox, cursor i pending commity),
- `logger.conf` oraz serwerowe pliki `NET_TLS_CERT_FILE` i `NET_TLS_KEY_FILE`.

Po odtworzeniu serwera przywróć jego certyfikat i klucz bez zmian, aby fingerprint
pozostał stały. Jeśli certyfikat jest rotowany, przekaż i skonfiguruj nowy
fingerprint na wszystkich klientach przed ich ponownym uruchomieniem. Nie
odtwarzaj serwera i klientów z niespójnych punktów czasu bez zachowania ich
outboxów.