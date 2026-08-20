# Siec Multi-Station z Centralnym Logiem - Dokument Techniczny

## Cel

Celem jest dodanie pracy wielu stacji w topologii gwiazdy:

- jedna instancja centralna (server) jest zrodlem prawdy dla logu,
- stacje operatorskie (client) synchronizuja lokalne logi z centralnym,
- system ma byc odporny na restart klienta i chwilowa utrate lacznosci,
- numeracja seryjna (dla contestow wymagajacych seriali) ma byc spójna globalnie.

Dokument jest przygotowany pod aktualna architekture projektu i model danych SQLite.

## Inspiracja DXLog - decyzje projektowe

Wnioski z podejscia DXLog, ktore przenosimy:

- tryb client-server jako podstawa dla stabilnej synchronizacji,
- jedna instancja serwera na siec,
- mechanizm wymuszonej synchronizacji po restarcie stacji,
- centralny number server dla seryjnych numerow contestowych,
- ostroznosc wobec UDP i WAN (utrata pakietow), preferencja TCP dla synchronizacji logu.

Wnioski, ktorych nie kopiujemy 1:1:

- synchronizacja i replika beda oparte o dziennik operacji i idempotencje (silniejsza odpornosc na duplikaty i reconnect).

## Architektura docelowa

### Topologia

- Server (centralny log):
  - nasluch TCP,
  - walidacja i aplikacja operacji,
  - dystrybucja zmian do klientow,
  - serwis rezerwacji seriali.
- Client (stacja operatorska):
  - lokalny zapis QSO do SQLite,
  - tworzenie operacji w outbox,
  - wysylka operacji do serwera,
  - odbior i aplikacja operacji z serwera,
  - catch-up od ostatniego global_seq.

### Model synchronizacji

- Event sourcing light: replikuje sie operacje, nie cale tabele.
- Kazda operacja ma unikalne op_id (UUID) i jest idempotentna.
- Kolejnosc globalna jest nadawana przez serwer (global_seq).

## 1. Propozycja zmian schematu SQLite

Zmiany obejmuja dwie role bazy:

- local client db (na stacji),
- central db (na serwerze).

W obu rolach utrzymujemy ten sam glowny model QSO, ale dochodza tabele synchronizacji i metadane.

### 1.1 Zmiany w tabeli qso

Dodac kolumny:

- qso_uid TEXT NOT NULL DEFAULT ''
  - globalny identyfikator QSO, generowany na kliencie przy tworzeniu QSO,
  - format rekomendowany: UUID v4.
- station_id TEXT NOT NULL DEFAULT ''
  - identyfikator stacji, ktora utworzyla QSO (np. RUN1, MULT1).
- station_qso_seq INTEGER NOT NULL DEFAULT 0
  - lokalny, monotoniczny numer QSO dla danej stacji.
- updated_at_utc TEXT NOT NULL DEFAULT ''
  - znacznik czasu ostatniej modyfikacji rekordu (UTC, ISO8601).
- version INTEGER NOT NULL DEFAULT 1
  - wersja logiczna rekordu do kontroli konfliktow przy update.

Indeksy:

- CREATE UNIQUE INDEX IF NOT EXISTS idx_qso_qso_uid ON qso(qso_uid);
- CREATE INDEX IF NOT EXISTS idx_qso_station_seq ON qso(station_id, station_qso_seq);
- CREATE INDEX IF NOT EXISTS idx_qso_updated_at ON qso(updated_at_utc);

Uwagi:

- id pozostaje lokalnym kluczem technicznym SQLite.
- Wszystkie update po synchronizacji musza byc wykonywane po qso_uid, nie po id.

### 1.2 Nowe tabele synchronizacji

#### sync_station_identity

Przechowuje tozsamosc instancji klienckiej.

- station_id TEXT PRIMARY KEY
- station_uuid TEXT NOT NULL
- created_at_utc TEXT NOT NULL

#### sync_outbox

Niezatwierdzone lokalne operacje do wysylki.

- op_id TEXT PRIMARY KEY
- station_id TEXT NOT NULL
- station_seq INTEGER NOT NULL
- op_type TEXT NOT NULL
- qso_uid TEXT
- payload_json TEXT NOT NULL
- created_at_utc TEXT NOT NULL
- send_attempts INTEGER NOT NULL DEFAULT 0
- next_retry_at_utc TEXT
- state TEXT NOT NULL DEFAULT 'pending'  -- pending|acked|failed
- last_error TEXT NOT NULL DEFAULT ''

Indeksy:

- CREATE UNIQUE INDEX IF NOT EXISTS idx_outbox_station_seq ON sync_outbox(station_id, station_seq);
- CREATE INDEX IF NOT EXISTS idx_outbox_state_retry ON sync_outbox(state, next_retry_at_utc);

#### sync_inbox

Historia odebranych operacji z serwera (idempotencja + audyt).

- op_id TEXT PRIMARY KEY
- global_seq INTEGER NOT NULL
- source_station_id TEXT NOT NULL
- op_type TEXT NOT NULL
- qso_uid TEXT
- payload_json TEXT NOT NULL
- applied_at_utc TEXT NOT NULL

Indeksy:

- CREATE UNIQUE INDEX IF NOT EXISTS idx_inbox_global_seq ON sync_inbox(global_seq);

#### sync_state

Kursor synchronizacji i metadane sesji.

- key TEXT PRIMARY KEY
- value TEXT NOT NULL

Klucze minimalne:

- last_global_seq_applied
- last_acked_station_seq
- server_instance_id
- protocol_version

#### serial_reservations

Lokalny cache rezerwacji numerow seryjnych (zrodlo prawdy jest na serwerze).

- reservation_id TEXT PRIMARY KEY
- contest_id TEXT NOT NULL
- station_id TEXT NOT NULL
- serial_nr INTEGER NOT NULL
- reserved_at_utc TEXT NOT NULL
- expires_at_utc TEXT NOT NULL
- state TEXT NOT NULL  -- reserved|consumed|released|expired
- qso_uid TEXT NOT NULL DEFAULT ''

### 1.3 Tabele serwerowe (tylko centralny server)

#### server_op_log

Globalny dziennik operacji.

- global_seq INTEGER PRIMARY KEY AUTOINCREMENT
- op_id TEXT NOT NULL UNIQUE
- source_station_id TEXT NOT NULL
- station_seq INTEGER NOT NULL
- op_type TEXT NOT NULL
- qso_uid TEXT
- payload_json TEXT NOT NULL
- received_at_utc TEXT NOT NULL
- applied_at_utc TEXT NOT NULL

Indeksy:

- CREATE UNIQUE INDEX IF NOT EXISTS idx_server_station_seq ON server_op_log(source_station_id, station_seq);
- CREATE INDEX IF NOT EXISTS idx_server_qso_uid ON server_op_log(qso_uid);

#### server_serial_counters

Centralny stan numeracji contestowej.

- contest_id TEXT PRIMARY KEY
- next_serial INTEGER NOT NULL
- updated_at_utc TEXT NOT NULL

### 1.4 Migracja schematu (wersjonowanie)

Dodac wersjonowanie schematu:

- app_meta: schema_version

Plan migracji:

1. schema_version 1 -> 2: kolumny qso_uid/station_id/station_qso_seq/updated_at_utc/version.
2. Uzupełnienie danych historycznych:
   - qso_uid dla starych rekordow generowane jednorazowo,
   - station_id='LEGACY', station_qso_seq=id, updated_at_utc=data+utc,
   - version=1.
3. Utworzenie tabel sync_*.
4. schema_version=2.

Wymaganie bezpieczenstwa:

- migracja w transakcji BEGIN IMMEDIATE ... COMMIT,
- rollback na kazdym bledzie.

## 2. Definicja protokolu komunikatow

## 2.1 Transport

- TCP jako transport bazowy.
- Opcjonalnie TLS (zalecane poza izolowana siecia LAN).
- Kodowanie: UTF-8.
- Framing: length-prefixed JSON.
  - 4 bajty uint32 big-endian = dlugosc payload,
  - payload = JSON.

Powod: bezpieczne parsowanie strumienia, brak problemu z separatorami nowej linii.

## 2.2 Wersjonowanie

Pole wymagane w kazdym komunikacie:

- protocol_version: 1

Uwagi implementacyjne (kompatybilnosc):

- Aktualna implementacja emituje oba pola: protocol_version oraz protocol_ver,
  aby pozostac zgodna z istniejacymi klientami.
- Walidacja po obu stronach akceptuje protocol_version albo protocol_ver,
  ale wartosc musi byc rowna 1.

W przypadku niezgodnosci wersji:

- server odpowiada ERROR_UNSUPPORTED_PROTOCOL,
- client zrywa sesje i pokazuje status operatorowi.

## 2.3 Envelope

Kazda wiadomosc ma envelope:

- msg_type: typ komunikatu,
- msg_id: UUID,
- sent_at_utc: ISO8601 UTC,
- protocol_version: int,
- body: obiekt zalezny od msg_type.

## 2.4 Typy komunikatow

### HELLO (client -> server)

Cel: rozpoczecie sesji i autoryzacja logiczna stacji.

body:

- station_id
- station_uuid
- app_version
- last_global_seq_applied
- last_station_seq_sent
- capabilities: ["sync_v1", "serial_reservation_v1"]

### HELLO_ACK (server -> client)

body:

- server_instance_id
- accepted_station_id
- session_id
- server_time_utc
- heartbeat_interval_sec
- catchup_from_global_seq

### OP_SUBMIT (client -> server)

body:

- op_id
- station_id
- station_seq
- op_type
- qso_uid
- payload

op_type minimalnie:

- qso_insert
- qso_update_contest_fields
- qso_set_invalid

### OP_ACK (server -> client)

body:

- op_id
- station_id
- station_seq
- accepted: true|false
- error_code (opcjonalnie)
- error_message (opcjonalnie)
- assigned_global_seq (jesli accepted=true)

### OP_BROADCAST (server -> client)

Operacja rozglaszana do wszystkich klientow (w tym opcjonalnie do nadawcy).

body:

- global_seq
- op_id
- source_station_id
- station_seq
- op_type
- qso_uid
- payload

### CATCHUP_REQUEST (client -> server)

body:

- from_global_seq_exclusive
- max_batch_size

Uwagi implementacyjne:

- W kodzie utrzymano rowniez kompatybilne PULL_OPS/PULL_OPS_RESP.
- CATCHUP_REQUEST/CATCHUP_BATCH sa jawnie obslugiwane jako rownowazna
  warstwa catch-up.

### CATCHUP_BATCH (server -> client)

body:

- from_global_seq_exclusive
- to_global_seq_inclusive
- ops: [OP_BROADCAST.body ...]
- has_more

### SERIAL_RESERVE_REQUEST (client -> server)

body:

- request_id
- contest_id
- station_id
- ttl_sec

### SERIAL_RESERVE_ACK (server -> client)

body:

- request_id
- reservation_id
- contest_id
- serial_nr
- expires_at_utc

### SERIAL_CONSUME (client -> server)

body:

- reservation_id
- qso_uid

### HEARTBEAT

Dwukierunkowo.

body:

- session_id
- last_global_seq

### ERROR

body:

- code
- message
- retriable
- related_msg_id

## 2.5 Reguly idempotencji i kolejnosci

- OP_SUBMIT jest idempotentne po op_id.
- Serwer musi odrzucic duplikat station_seq dla tej samej station_id jesli op_id inne.
- Klient aplikuje OP_BROADCAST tylko jesli global_seq > last_global_seq_applied.
- Klient ignoruje OP_BROADCAST z global_seq <= last_global_seq_applied.

## 2.6 Retry i backoff

- Retry tylko dla bledow retriable.
- Exponential backoff: 1s, 2s, 4s, 8s, ... max 60s.
- Losowy jitter +-20%.
- Po reconnect klient wykonuje CATCHUP_REQUEST od lokalnego kursora.

## 2.7 Minimalne kody bledow

- ERROR_UNSUPPORTED_PROTOCOL
- ERROR_INVALID_MESSAGE
- ERROR_AUTH_FAILED
- ERROR_CONFLICT_STATION_SEQ
- ERROR_UNKNOWN_OP_TYPE
- ERROR_APPLY_FAILED
- ERROR_SERIAL_UNAVAILABLE
- ERROR_RATE_LIMIT

## 3. Lista zmian funkcji w plikach source

Ponizej mapa zmian pod aktualny kod.

## 3.1 src/db.h i src/db.c

Nowe API:

- int db_get_or_create_station_identity(char *station_id, size_t size);
- int db_sync_enqueue_op(const SyncOp *op);
- int db_sync_mark_acked(const char *op_id, long long global_seq);
- int db_sync_get_pending_ops(SyncOp *out, int max_items, int *out_count);
- int db_sync_apply_remote_op(const SyncOp *op, long long global_seq);
- int db_sync_get_last_global_seq(long long *out_seq);
- int db_sync_set_last_global_seq(long long seq);
- int db_reserve_serial_local(...);

Modyfikacje istniejacych funkcji:

- db_init:
  - migracje schema_version=2,
  - tworzenie tabel sync_* i indeksow,
  - inicjalizacja station identity.
- db_insert_qso:
  - wymagane qso_uid/station_id/station_qso_seq/updated_at_utc/version,
  - zwrot qso_uid obok out_id (nowy parametr lub osobna funkcja).
- db_update_qso_invalid:
  - zamiana semantyki na set state (invalid=true/false),
  - update po qso_uid jako sciezka preferowana.
- db_update_qso_contest_fields:
  - update po qso_uid,
  - version = version + 1,
  - updated_at_utc = now.

## 3.2 src/qso.h i src/qso.c

Rozszerzenie QSO struct:

- char qso_uid[37];
- char station_id[32];
- int station_qso_seq;
- char updated_at_utc[32];
- int version;

Modyfikacje:

- qso_add_fields / qso_add_contest_fields:
  - generowanie qso_uid,
  - uzupelnianie station_id/station_qso_seq,
  - enqueue OP qso_insert po sukcesie db_insert_qso.
- qso_mark_invalid:
  - zamiast toggle-only: zapis jawnego stanu i enqueue OP qso_set_invalid.
- nowa funkcja pomocnicza:
  - qso_apply_remote_update(...): stosowanie zmian przychodzacych z serwera.

## 3.3 src/app_controller_runtime.inc i src/app_controller.c

Modyfikacje inicjalizacji i shutdown:

- app_controller_init:
  - start net_sync_start() po qso_init i config_load.
- app_controller_shutdown:
  - net_sync_stop() przed db_shutdown.

Modyfikacje workflow Enter/logowania:

- po lokalnym zapisaniu QSO pokazac stan synchronizacji (np. "QSO OK [SYNC:PENDING]").
- przy utracie polaczenia status operatora: "NET OFFLINE - local queue: N".

Nowe komendy tekstowe:

- netsync on|off
- netsync status
- netsync catchup
- netserver start|stop (tryb centralny)

## 3.4 Nowe pliki i moduly

Dodac:

- src/net_sync.h
- src/net_sync.c
  - worker klienta, outbox retry, catch-up, heartbeat.
- src/net_protocol.h
- src/net_protocol.c
  - serializacja/deserializacja komunikatow, framing.
- src/net_server.h
- src/net_server.c
  - serwer centralny, session manager, op_log, serial server.

Integracja build:

- CMakeLists.txt: dodac nowe moduly do target logger oraz testow.

## 3.5 src/config.h i src/config.c

Dodac konfiguracje:

- NET_ENABLED=0|1
- NET_MODE=client|server|off
- NET_SERVER_HOST=...
- NET_SERVER_PORT=9888
- NET_STATION_ID=RUN1
- NET_AUTH_TOKEN=... (opcjonalnie)
- NET_TLS=0|1
- NET_RETRY_MAX_SEC=60
- NET_HEARTBEAT_SEC=5
- SERIAL_SERVER_ENABLED=0|1 (dla trybu server)

## 3.6 src/export.c i src/stats.c

Zmiany minimalne:

- export.c:
  - brak zmian funkcjonalnych wymaganych dla MVP.
- stats.c:
  - brak zmian logicznych, ale testowo upewnic sie, ze odtwarzanie remote ops nie psuje agregacji.

## 4. Plan testow regresyjnych i sieciowych

Plan dzieli testy na 4 warstwy: unit, component, integration, resilience.

## 4.1 Testy jednostkowe (tests/unit)

Nowe testy:

1. net_protocol:
- encode/decode wszystkich msg_type,
- bledne payloady,
- niezgodna protocol_version,
- framing z przycietym payloadem.

2. db sync:
- enqueue outbox,
- mark ack,
- idempotent apply remote op,
- conflict station_seq.

3. qso sync hooks:
- qso_add_contest_fields tworzy qso_uid,
- qso_mark_invalid generuje qso_set_invalid (set, nie toggle).

## 4.2 Testy komponentowe (tests/regression)

1. Migracja schema_version:
- stara baza -> migracja -> weryfikacja nowych kolumn/tabel.

2. Catch-up replay:
- lokalna baza pusta,
- aplikacja batcha OP_BROADCAST,
- zgodnosc stanu qso_count i pol contestowych.

3. Reconnect queue flush:
- symulacja offline,
- dodanie N QSO,
- powrot online,
- wszystkie OP_SUBMIT ack i outbox puste.

## 4.3 Testy integracyjne multi-station

Scenariusze minimalne:

1. Dwie stacje + jeden serwer, ten sam contest:
- stacja A loguje QSO,
- stacja B widzi QSO po synchronizacji.

2. Rownoczesne logowanie:
- A i B loguja w tym samym czasie,
- oba QSO obecne centralnie i lokalnie.

3. Aktualizacja pola invalid:
- A oznacza QSO invalid,
- B dostaje update i ma ten sam stan.

4. Numeracja seryjna:
- A i B rezerwuja seriale naprzemiennie,
- brak duplikatow numerow.

5. Restart klienta:
- klient restart po czesciowym ack,
- catch-up doprowadza stan do zgodnosci.

## 4.4 Testy odpornosci (fault injection)

1. Utrata polaczenia TCP w trakcie wysylki.
2. Duplikacja OP_SUBMIT po timeout.
3. Opoznione OP_BROADCAST (reordering).
4. Serwer restart z zachowaniem server_op_log.
5. Uszkodzony payload jednej operacji (ERROR_INVALID_MESSAGE).

Kryteria akceptacji:

- brak utraty QSO,
- brak zdublowanych QSO po op_id/qso_uid,
- finalna zbieznosc stanu wszystkich stacji i serwera,
- brak regresji eksportu CSV/ADIF/Cabrillo.

## 4.5 Automatyzacja i CI

Minimalny zakres CI po wdrozeniu:

- build,
- tests/unit,
- tests/regression,
- nowy test integration-lite (1 serwer + 2 klienty w tym samym procesie lub przez fixture IPC).

## Etapy wdrozenia

Etap 1 (MVP LAN):

- qso_uid + outbox + OP_SUBMIT/OP_ACK + catch-up,
- bez TLS,
- bez edycji historycznych poza invalid.

Etap 2 (pelna synchronizacja):

- update contest fields,
- conflict handling z version,
- serial reservation server.

Etap 3 (stabilizacja):

- resilience tests,
- metryki i statusy operatorskie,
- hardening reconnect i timeout.

Etap 4 (WAN-ready):

- TLS,
- auth token,
- limity i ochrona przed naduzyciem.

## Definicja gotowosci (Definition of Done)

Funkcjonalne:

- min. 2 stacje moga prowadzic wspolny log przez serwer.
- restart klienta nie powoduje rozjazdu logow.
- seriale sa globalnie spójne.

Techniczne:

- 100% testow unit/regression przechodzi,
- integration i resilience scenariusze krytyczne przechodza,
- brak regresji eksportu i podstawowego workflow contestowego.

Operacyjne:

- operator ma czytelny status sync i kolejki,
- istnieje komenda recznego catch-up,
- dokumentacja konfiguracji sieciowej jest kompletna.
