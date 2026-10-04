# Centralny Log - Backlog Implementacyjny

Aktualizacja: 2026-10-04
Status: runtime, trwałość, token/TLS pinning, routing `shared_log_id` i test wieloprocesowy są aktywne.

## Cel

Lista pozostałych funkcjonalnosci i ograniczeń pracy wielostanowiskowej z centralnym logiem.

## Kontrakt danych v1

Ponizsze decyzje obowiazuja dla aktywnego runtime v1.

- **Topologia:** jedna instancja serwera obsluguje jeden aktywny wspolny log.
  Wiele wspolnych logow wymaga osobnych instancji/baz serwera; obsluga wielu
  logow w jednej instancji jest poza zakresem v1.
- **Tozsamosc logu:** `shared_log_id` ma format `sl-` plus 32 male znaki
  szesnastkowe. Jest przechowywany w `sync_identity` konkretnego pliku SQLite,
  przenoszony przez envelope protocol v2 i porownywany z HELLO_ACK oraz aktywna
  baza przed APPEND/PULL/CATCHUP/rezerwacja/commit. Klient musi jawnie przyjac
  ID serwera. Lokalne `logbook_id` nie jest globalne: serwer mapuje operacje do
  swojego aktywnego ID, a klient mapuje catch-up do swojego.
- **Zrodlo prawdy:** serwer jest autorytatywny dla zaakceptowanych operacji i
  `global_seq`. Klient zapisuje lokalnie i trwale kolejkuje operacje; stan
  `pending` nie oznacza, ze wpis zostal zatwierdzony centralnie.
- **Praca offline:** klient moze kolejkowac QSO. Numery contestowe sa pobierane
  z wyprzedzeniem do trwalej lokalnej puli i mozna je wykorzystac podczas
  krotkiej awarii sieci. Bez dostepnej rezerwacji aplikacja blokuje zapis z
  numerem zamiast podstawic lokalny licznik.
- **Idempotencja i konflikty:** powtorzony `op_id` jest potwierdzany jako ta
  sama operacja; para `station_id + station_seq` jest unikalna. Rozne QSO z tym
  samym znakiem/pasmem/trybem pozostaja w logu, a centralna logika oznacza
  duplikat jako invalid z zerowa punktacja. Aktualizacja QSO na nieaktualnej
  wersji jest odrzucana i pokazywana operatorowi; brak cichego last-write-wins.
- **Migracja:** migracja SQLite dodaje `shared_log_id` jako puste pole i nie
  przypisuje ID automatycznie starym logom. Operator wybiera kanoniczny log,
  robi kopie baz, tworzy ID na serwerze i jawnie paruje klientow. Import historii
  zachowuje wszystkie QSO; rekordy bez `qso_uid` dostaja unikalne ID migracyjne.
  Nie scala sie wpisow tylko na podstawie call/time. Centralny licznik seriali
  startuje powyzej najwyzszego istniejacego numeru dla wspolnego logu.

## Rzeczywisty stan builda

Moduly sieciowe sa uruchamiane z konfiguracji aplikacji. Klient ma worker poza
UI, serwer ma kontrolowany start/stop, a pasek statusu pokazuje online/offline,
pending i failed. `ctest --test-dir build --output-on-failure` uruchamia test
migracji, `network_tests` oraz `multiprocess_sync_tests` z dwoma klientami.
Pelny `unit_tests` nadal ma stare
niepowiazane bledy testow F3/#LOCATOR, sugestii, konfiguracji i logbookow.

Pozostale ograniczenia:

- Jedna instancja serwera routuje do jednego aktywnego pliku SQLite; inne
  wspolne logi wymagaja osobnych instancji.
- QSO zapisane lokalnie na serwerze pozostaje w jego outboxie i nie jest jeszcze
  automatycznie publikowane do `log_ops`; potrzebny jest publisher oraz backfill
  istniejacych QSO. Test wieloprocesowy seeduje wpis serwera bezposrednio do
  dziennika, aby sprawdzic mapowanie logbook IDs.

## Priorytet P0 (krytyczne dla spojnosci danych)

- [x] Ustalic kontrakt v1 dla pojedynczego logu serwera, autorytetu, offline,
  konfliktow oraz migracji.
  - Szczegoly: ta sekcja.

- [x] Dodac `shared_log_id` do `sync_identity` z migracja starych baz i API
  jawnego tworzenia/parowania.
  - Test: legacy schema dostaje puste ID, zachowuje station_id, a ustawione ID
    nie moze zostac podmienione.

- [x] Przenosic `shared_log_id` w HELLO/HELLO_ACK i kazdym envelope; serwer ma odrzucac
  brak lub niezgodnosc ID przed apply/pull/rezerwacja.
  - Dotyczy: `net_protocol.*`, `net_sync.*`, `net_server.*`.

- [x] Zapisac QSO i wpis outbox atomowo oraz apply zdalny razem z `log_ops`.
  Bledy kolejki lub dziennika nie moga zostawiac polowicznie zatwierdzonych danych.

- [x] Dodac twarde wykrywanie konfliktu station_seq dla tej samej station_id przy innym op_id.
  - Oczekiwane zachowanie: serwer odrzuca konflikt i zwraca czytelny blad.
  - Dotyczy: src/net_server.c, src/db.c

- [x] Dodac ograniczenie unikalnosci station_id + station_seq po stronie dziennika operacji.
  - Oczekiwane zachowanie: baza nie pozwala zapisac dwoch roznych operacji o tej samej parze (station_id, station_seq).
  - Dotyczy: src/db.c (indeksy/migracja)

- [x] Podlaczyc rezerwacje i COMMIT_SERIAL do aktywnej sciezki zapisu QSO.
  - Worker buforuje rezerwacje; QSO i trwałe zadanie commit zapisuja sie atomowo.
  - Licznik serwera inicjuje sie powyzej najwyzszego numeru z istniejacego logu.

## Priorytet P1 (sprawnosc synchronizacji i zgodnosc z dokumentem projektu)

- [x] Dodac komunikat OP_BROADCAST (server -> client) i obsluge push zmian do aktywnych klientow.
  - Oczekiwane zachowanie: klient otrzymuje zmiany bez czekania na kolejny poll.
  - Dotyczy: src/net_protocol.c, src/net_protocol.h, src/net_server.c, src/net_sync.c

- [x] Dodac obsluge CATCHUP_REQUEST / CATCHUP_BATCH jako jawne typy protokolu (lub zaktualizowac dokumentacje, jesli PULL_OPS zostaje docelowo).
  - Oczekiwane zachowanie: jednoznaczna warstwa catch-up i zgodnosc nazewnictwa protokolu.
  - Dotyczy: src/net_protocol.c, src/net_protocol.h, src/net_sync.c, src/net_server.c, docs/siec-centralny-log-projekt.md

- [x] Dodac walidacje protocol_version po obu stronach oraz zwracanie dedykowanego bledu ERROR_UNSUPPORTED_PROTOCOL.
  - Oczekiwane zachowanie: kontrolowane zerwanie sesji przy niezgodnej wersji protokolu.
  - Dotyczy: src/net_protocol.c, src/net_server.c, src/net_sync.c

- [x] Dodac jitter (+/-20%) do mechanizmu backoff.
  - Oczekiwane zachowanie: mniejsze ryzyko efektu thundering herd po reconnect.
  - Dotyczy: src/net_sync.c

## Priorytet P2 (UX operatora i komendy)

- [x] Dodac status po zapisie QSO z informacja o kolejce sync (np. SYNC:PENDING).
  - Oczekiwane zachowanie: operator widzi od razu, czy wpis czeka na wysylke.
  - Dotyczy: src/qso.c, src/app_controller_runtime.inc

- [x] Dodac komunikat offline z rozmiarem kolejki (np. NET OFFLINE - local queue: N).
  - Oczekiwane zachowanie: czytelna diagnostyka w czasie braku lacznosci.
  - Dotyczy: src/app_controller_runtime.inc

- [x] Dodac aliasy/komendy zgodne z dokumentem projektu:
  - netsync on|off
  - netsync status
  - netsync catchup
  - netserver start|stop
  - Dotyczy: src/app_controller_runtime.inc

## Priorytet P3 (hardening i WAN-ready)

- [x] Wymagac silnego tokenu serwera i jawnego pinu SHA-256 przy TLS klienckim.
  - Pusty token blokuje start serwera; brak fingerprintu blokuje klienta TLS.
  - Wybrano reczne pinning self-signed certyfikatu, bez TOFU; mTLS/PKI pozostaje opcjonalnym hardeningiem.

- [ ] Rozszerzyc rate limiting o centralne statystyki, telemetrie i mechanizm blokad/blacklist.
  - Dotyczy: src/net_server.c, src/db.c, docs

- [ ] Dodac dedykowany runner fault-injection/chaos (profil WAN) do CI.
  - Dotyczy: tests/, CMakeLists.txt, pipeline CI (poza repo lub w repo)

## Testy do dopisania/rozszerzenia

- [x] Test konfliktu station_seq (ten sam station_seq, rozne op_id).
- [x] Test protocol_version mismatch -> ERROR_UNSUPPORTED_PROTOCOL.
- [x] Test reserve + commit serial (pelny cykl), restart i cleanup po TTL.
- [ ] Test push OP_BROADCAST do 2 klientow bez aktywnego pull.
- [x] Test reconnect, outage, restart klienta i catch-up wiekszego niz strona.
- [x] Test komend operatorskich netsync/netserver.
- [x] Test procesowy: serwer + dwoch klientow, osobne SQLite, rownolegle QSO, powtorzony APPEND i wspolna numeracja.

## Sugerowana kolejnosc pracy

1. P0: utrzymywac osobna instancje serwera dla kazdego aktywnego pliku/bazy logu.
2. P1: metryki rate limitu, mTLS/PKI lub rotacja pinu dla deploymentu WAN.
3. P2: chaos CI dla dlugich awarii, race i uszkodzen procesu podczas SQLite commit.

## Uwagi organizacyjne

- Po kazdym zadaniu uruchamiac: build + unit_tests + regression_tests.
- Po wdrozeniach protokolu aktualizowac dokumentacje techniczna, aby uniknac rozjazdu kod vs docs.
- Dla zmian w DB dodawac migracje zgodne wstecznie i test migracyjny.
