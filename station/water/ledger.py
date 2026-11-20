"""The water point's ledger (M9): one SQLite file on the Pi, data/water.db.

It is the record. Every dispense and every water event is written here first, so nothing is lost while the
broker or the dashboard is down: events not yet published are sent again later, marked "buffered"
(contract rule 5). The daily quotas are counted from it, so a restart does not give a household its quota
again.

    dispenses   one row per session: card, day, litres, why the valve closed
    events      every water_event message, and whether it reached the broker
    filters     each cartridge's tag: capacity, litres so far, when it was fitted and used up
    quotas      cards with their own daily quota (the others have the default)
    settings    the filter fitted now

The admin command line (python -m station.water quota, filter, report) works on the same file while the
water point runs: SQLite takes care of the two processes.
"""

import json
import sqlite3
import threading
from pathlib import Path

SCHEMA = """
CREATE TABLE IF NOT EXISTS dispenses (
    id INTEGER PRIMARY KEY, day TEXT NOT NULL, uid TEXT NOT NULL, card TEXT NOT NULL,
    started_ms INTEGER NOT NULL, ended_ms INTEGER NOT NULL, litres REAL NOT NULL, end_reason TEXT NOT NULL,
    filter_uid TEXT);
CREATE INDEX IF NOT EXISTS dispenses_by_day ON dispenses (day, uid);
CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY, ts INTEGER NOT NULL, day TEXT NOT NULL, type TEXT NOT NULL, message TEXT NOT NULL,
    published INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS events_unpublished ON events (published);
CREATE TABLE IF NOT EXISTS filters (
    uid TEXT PRIMARY KEY, label TEXT NOT NULL, capacity_l REAL NOT NULL, litres REAL NOT NULL DEFAULT 0,
    added_ms INTEGER NOT NULL, fitted_ms INTEGER, expired_ms INTEGER);
CREATE TABLE IF NOT EXISTS quotas (uid TEXT PRIMARY KEY, litres_per_day REAL NOT NULL);
CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
"""
FILTER_FIELDS = ("uid", "label", "capacity_l", "litres", "added_ms", "fitted_ms", "expired_ms")


class Ledger:
    def __init__(self, path=":memory:"):
        if str(path) != ":memory:":
            Path(path).parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(str(path), timeout=10, check_same_thread=False, isolation_level=None)
        self.lock = threading.Lock()
        with self.lock:
            if str(path) != ":memory:":
                self.db.execute("PRAGMA journal_mode=WAL")  # the admin commands can read while it writes
            self.db.executescript(SCHEMA)

    def _all(self, sql, args=()):
        with self.lock:
            return self.db.execute(sql, args).fetchall()

    def _one(self, sql, args=()):
        rows = self._all(sql, args)
        return rows[0] if rows else None

    def _write(self, *statements):
        """[(sql, args)...] in one transaction: all or nothing."""
        with self.lock:
            self.db.execute("BEGIN IMMEDIATE")
            try:
                last = None
                for sql, args in statements:
                    last = self.db.execute(sql, args).lastrowid
                self.db.execute("COMMIT")
                return last
            except BaseException:
                self.db.execute("ROLLBACK")
                raise

    def close(self):
        with self.lock:
            self.db.close()

    # ------------------------------------------------------------------ dispenses and quotas

    def add_dispense(self, day, uid, card, started_ms, ended_ms, litres, end, filter_uid):
        """One session. The filter's litres go up in the same step."""
        statements = [("INSERT INTO dispenses (day, uid, card, started_ms, ended_ms, litres, end_reason, filter_uid) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                       (day, uid, card, int(started_ms), int(ended_ms), float(litres), end, filter_uid))]
        if filter_uid:
            statements.append(("UPDATE filters SET litres = litres + ? WHERE uid = ?", (float(litres), filter_uid)))
        self._write(*statements)

    def used_on(self, day, uid):
        return self._one("SELECT COALESCE(SUM(litres), 0) FROM dispenses WHERE day = ? AND uid = ?", (day, uid))[0]

    def total_on(self, day):
        return self._one("SELECT COALESCE(SUM(litres), 0) FROM dispenses WHERE day = ?", (day,))[0]

    def quota(self, uid):
        """The card's own daily quota in litres, or None: it has the default."""
        row = self._one("SELECT litres_per_day FROM quotas WHERE uid = ?", (uid,))
        return None if row is None else row[0]

    def set_quota(self, uid, litres):
        if litres is None:
            self._write(("DELETE FROM quotas WHERE uid = ?", (uid,)))
        else:
            self._write(("INSERT INTO quotas (uid, litres_per_day) VALUES (?, ?) "
                         "ON CONFLICT (uid) DO UPDATE SET litres_per_day = excluded.litres_per_day",
                         (uid, float(litres))))

    def quotas(self):
        return dict(self._all("SELECT uid, litres_per_day FROM quotas ORDER BY uid"))

    def report(self, day):
        """Per card on that day: [(uid, card, litres, sessions)], most water first."""
        return self._all("SELECT uid, MAX(card), SUM(litres), COUNT(*) FROM dispenses WHERE day = ? "
                         "GROUP BY uid ORDER BY SUM(litres) DESC", (day,))

    # ------------------------------------------------------------------ events

    def add_event(self, message, day):
        return self._write(("INSERT INTO events (ts, day, type, message) VALUES (?, ?, ?, ?)",
                            (int(message["ts"]), day, message["type"],
                             json.dumps(message, separators=(",", ":"), ensure_ascii=False))))

    def mark_published(self, event_id):
        self._write(("UPDATE events SET published = 1 WHERE id = ?", (event_id,)))

    def unpublished(self, limit=500):
        """[(id, message)], oldest first."""
        rows = self._all("SELECT id, message FROM events WHERE published = 0 ORDER BY id LIMIT ?", (limit,))
        return [(i, json.loads(m)) for i, m in rows]

    def events_on(self, day, etype=None):
        sql, args = "SELECT message FROM events WHERE day = ?", [day]
        if etype:
            sql, args = sql + " AND type = ?", args + [etype]
        return [json.loads(m) for (m,) in self._all(sql + " ORDER BY id", args)]

    # ------------------------------------------------------------------ filters

    def add_filter(self, uid, label, capacity_l, now_ms):
        """Register a cartridge tag, or change the label or capacity of one already registered."""
        self._write(("INSERT INTO filters (uid, label, capacity_l, added_ms) VALUES (?, ?, ?, ?) "
                     "ON CONFLICT (uid) DO UPDATE SET label = excluded.label, capacity_l = excluded.capacity_l",
                     (uid, label, float(capacity_l), int(now_ms))))

    def filter(self, uid):
        row = self._one(f"SELECT {', '.join(FILTER_FIELDS)} FROM filters WHERE uid = ?", (uid,))
        return None if row is None else dict(zip(FILTER_FIELDS, row))

    def filters(self):
        return [dict(zip(FILTER_FIELDS, r)) for r in self._all(f"SELECT {', '.join(FILTER_FIELDS)} FROM filters ORDER BY added_ms")]

    def fitted_filter(self):
        row = self._one("SELECT value FROM settings WHERE key = 'filter'")
        return None if row is None else self.filter(row[0])

    def fit_filter(self, uid, now_ms):
        self._write(("INSERT INTO settings (key, value) VALUES ('filter', ?) "
                     "ON CONFLICT (key) DO UPDATE SET value = excluded.value", (uid,)),
                    ("UPDATE filters SET fitted_ms = ? WHERE uid = ?", (int(now_ms), uid)))

    def set_filter_expired(self, uid, now_ms):
        self._write(("UPDATE filters SET expired_ms = ? WHERE uid = ? AND expired_ms IS NULL", (int(now_ms), uid)))

    def set_filter_litres(self, uid, litres):
        """For the admin (a cartridge with a known past) and the demo set-up."""
        self._write(("UPDATE filters SET litres = ? WHERE uid = ?", (float(litres), uid)))