# TLS i bezpieczeństwo Central Log

## Cel

Dokument opisuje aktywny model uwierzytelniania i TLS. Synchronizacja zawsze
wymaga silnego tokenu aplikacyjnego; przy `NET_TLS=1` klient dodatkowo wymaga
ręcznie skonfigurowanego fingerprintu serwera SHA-256.

- serwer utrzymuje lokalny self-signed certyfikat i klucz,
- klient porównuje certyfikat z jawnym pinem; brak pinu blokuje połączenie,
- `NET_AUTH_TOKEN` (lub `NET_SHARED_KEY`) jest wymagany i nie może być pusty.
- serwer odrzuca sekret krótszy niż 32 znaki lub o zbyt małej różnorodności.

## Pliki i ustawienia

W `logger.conf` obsluzone sa pola:

- `NET_TLS=1`
- `NET_TLS_CERT_FILE=logger_net_cert.pem`
- `NET_TLS_KEY_FILE=logger_net_key.pem`
- `NET_TLS_PEER_FINGERPRINT=`
- `NET_AUTH_TOKEN=<silny sekret współdzielony z klientami>`
- `NET_RATE_LIMIT_WINDOW_SEC=1`
- `NET_RATE_LIMIT_BURST=32`
- `NET_MAX_FRAME_BYTES=65536`

Znaczenie:

- `NET_TLS_CERT_FILE`: sciezka PEM certyfikatu serwera.
- `NET_TLS_KEY_FILE`: sciezka PEM klucza prywatnego serwera.
- `NET_TLS_PEER_FINGERPRINT`: pin SHA-256 certyfikatu serwera po stronie klienta.

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

## Kolejne polaczenia klienta

Przy nastepnych polaczeniach:

- klient porownuje otrzymany fingerprint z `NET_TLS_PEER_FINGERPRINT`,
- mismatch kończy handshake błędem i sesja sync nie startuje.

## Rotacja certyfikatu

Jesli certyfikat serwera musi zostac zrotowany:

1. zatrzymac serwer,
2. podmienic lub usunac `NET_TLS_CERT_FILE` i `NET_TLS_KEY_FILE`,
3. uruchomic serwer,
4. odczytac nowy fingerprint,
5. zaktualizowac `NET_TLS_PEER_FINGERPRINT` na klientach.

## Token i ograniczenia

- brak mTLS,
- brak CRL/OCSP,
- brak zewnetrznego CA,
- fingerprint pinning wymaga bezpiecznego kanału dystrybucji pinu,
- bez TLS token jest przesyłany jawnie; plain TCP stosuj wyłącznie w zaufanej, izolowanej sieci.

Wygeneruj token bez spacji i nowej linii, np.:

```bash
openssl rand -base64 48 | tr -d '\n'
```

Zapisz identyczną wartość w `NET_AUTH_TOKEN` na serwerze i klientach. Pliki
konfiguracyjne z tokenem oraz prywatny klucz serwera chroń uprawnieniami
ograniczonymi do operatora usługi (np. `chmod 600`).

## Rekomendacja operacyjna

Dla deploymentu klubowego/LAN:

- trzymac pliki cert/key poza katalogami tymczasowymi,
- zbackupowac PEM-y razem z konfiguracja serwera,
- zweryfikować fingerprint poza połączeniem aplikacji i skonfigurować go na klientach przed startem,
- utrzymywać ten sam silny token po obu stronach i rotować go koordynując serwer oraz klientów,
- nie udostępniać portu serwera do niezaufanej sieci bez TLS.

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