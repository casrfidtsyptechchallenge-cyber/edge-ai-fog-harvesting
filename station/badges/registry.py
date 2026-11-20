"""The station's badge list: one JSON file, checked against the contract, sent to the nodes as SYNC_ALLOWLIST.

data/badges.json looks like this:

    {"version": 3,
     "badges": [{"uid": "04A1B2C3", "role": "operator", "label": "white card"},
                {"uid": "04112233445566", "role": "technician", "label": "blue key fob"}]}

- version goes up by one with every change. A node refuses a list older than the one it has, so an old copy
  of the file can never undo a newer list.
- role is a contract role: operator, technician, admin, household or unknown.
- label says which card it is (colour, number, sticker). Keep it anonymous: no personal names here.
"""

import json
import re
import time
import uuid
from pathlib import Path

import aeroion_contract as ac

DEFAULT_FILE = Path("data") / "badges.json"
UID_PATTERN = re.compile(ac.COMMON["$defs"]["uid"]["pattern"])
MAX_BADGES = ac.SCHEMAS["cmd"]["$defs"]["SYNC_ALLOWLIST"]["properties"]["entries"]["maxItems"]
MAX_LABEL = 64
DEFAULT_TTL_S = 300


class BadgeError(ValueError):
    """A badge, or a badge file, that breaks the rules. The message says why."""


def normalize_uid(text):
    """'04:a1:b2:c3', '04 A1 B2 C3' or '04a1b2c3' -> '04A1B2C3', the contract's form.

    Readers and phone apps print UIDs in many ways; the contract wants upper-case hex, 4 to 10 whole bytes.
    """
    uid = re.sub(r"[\s:\-]", "", str(text)).upper()
    if not UID_PATTERN.fullmatch(uid):
        raise BadgeError(f"{text!r} is not a UID: it needs 8 to 20 hex digits (4 to 10 bytes)")
    if len(uid) % 2:
        raise BadgeError(f"{text!r} has an odd number of hex digits: one is missing")
    return uid


def new_registry():
    return {"version": 0, "badges": []}


def check(registry):
    """Every problem in a badge list, as readable sentences. An empty list means it is fine."""
    if not isinstance(registry, dict):
        return ["the file must hold one JSON object with 'version' and 'badges'"]
    problems = []
    extra = set(registry) - {"version", "badges"}
    if extra:
        problems.append(f"unknown key(s): {', '.join(sorted(extra))}")
    version = registry.get("version")
    if not isinstance(version, int) or isinstance(version, bool) or version < 0:
        problems.append("version must be a whole number from 0")
    badges = registry.get("badges")
    if not isinstance(badges, list):
        return problems + ["badges must be a list"]
    if len(badges) > MAX_BADGES:
        problems.append(f"{len(badges)} badges: the contract allows at most {MAX_BADGES}")
    seen = set()
    for i, badge in enumerate(badges):
        where = f"badge {i}"
        if not isinstance(badge, dict):
            problems.append(f"{where}: must be an object with uid, role and label")
            continue
        uid = badge.get("uid")
        try:
            if normalize_uid(uid) != uid:
                problems.append(f"{where}: uid {uid!r} must be written {normalize_uid(uid)!r}")
        except BadgeError as e:
            problems.append(f"{where}: {e}")
        if uid in seen:
            problems.append(f"{where}: uid {uid} is listed twice")
        seen.add(uid)
        if badge.get("role") not in ac.ROLES:
            problems.append(f"{where}: role must be one of {', '.join(ac.ROLES)}")
        label = badge.get("label", "")
        if not isinstance(label, str) or len(label) > MAX_LABEL:
            problems.append(f"{where}: label must be text of at most {MAX_LABEL} characters")
        extra = set(badge) - {"uid", "role", "label"}
        if extra:
            problems.append(f"{where}: unknown key(s): {', '.join(sorted(extra))}")
    return problems


def load(path=DEFAULT_FILE):
    """Read and check the badge file. A file that does not exist yet is an empty list (version 0)."""
    path = Path(path)
    if not path.exists():
        return new_registry()
    try:
        registry = json.loads(path.read_text(encoding="utf-8"))
    except (ValueError, UnicodeDecodeError) as e:
        raise BadgeError(f"{path} is not valid JSON: {e}") from None
    problems = check(registry)
    if problems:
        raise BadgeError(f"{path}: " + "; ".join(problems))
    return registry


def save(registry, path=DEFAULT_FILE):
    """Check, then write the file in one step, so a crash never leaves half a file behind."""
    problems = check(registry)
    if problems:
        raise BadgeError("not saved: " + "; ".join(problems))
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(registry, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def find(registry, uid):
    """The badge with this UID, or None."""
    uid = normalize_uid(uid)
    return next((b for b in registry["badges"] if b["uid"] == uid), None)


def add(registry, uid, role, label=""):
    """Add a badge, or change the role or label of one already listed.

    Returns True if the list changed; its version then goes up by one.
    """
    uid = normalize_uid(uid)
    if role not in ac.ROLES:
        raise BadgeError(f"role must be one of {', '.join(ac.ROLES)}")
    label = "" if label is None else str(label)
    if len(label) > MAX_LABEL:
        raise BadgeError(f"label is longer than {MAX_LABEL} characters")
    badge = find(registry, uid)
    if badge is None:
        if len(registry["badges"]) >= MAX_BADGES:
            raise BadgeError(f"the list is full: the contract allows at most {MAX_BADGES} badges")
        registry["badges"].append({"uid": uid, "role": role, "label": label})
    elif badge["role"] == role and badge.get("label", "") == label:
        return False
    else:
        badge["role"] = role
        badge["label"] = label
    registry["version"] += 1
    return True


def remove(registry, uid):
    """Remove a badge. Returns True if it was on the list; the version then goes up by one."""
    badge = find(registry, uid)
    if badge is None:
        return False
    registry["badges"].remove(badge)
    registry["version"] += 1
    return True


def sync_params(registry):
    """The params of SYNC_ALLOWLIST: the version and every badge's uid and role (labels stay on the station)."""
    return {"version": registry["version"],
            "entries": [{"uid": b["uid"], "role": b["role"]} for b in registry["badges"]]}


def sync_command(registry, ttl_s=DEFAULT_TTL_S, now=None):
    """The SYNC_ALLOWLIST command for this list, checked against the contract before it leaves the station."""
    ts = int((time.time() if now is None else now) * 1000)
    cmd = {"schema_version": ac.SCHEMA_VERSION,
           "cmd_id": f"acl-{registry['version']}-{uuid.uuid4().hex[:8]}",
           "ts": ts, "ttl_s": ttl_s, "type": "SYNC_ALLOWLIST", "params": sync_params(registry)}
    errors = ac.validate("cmd", cmd)
    if errors:
        raise BadgeError("the command breaks the contract: " + "; ".join(errors))
    return cmd