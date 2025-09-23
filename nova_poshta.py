"""Nova Poshta integration helpers for Bot.BSG.

This module encapsulates all interaction with the Nova Poshta API as well
as the local persistence that keeps per-user history, bookmarks, notes and
administrator assignments.  The data lives in ``data/nova_poshta.json`` so it
survives bot restarts and remains bot-global (not tied to projects).

The helpers are intentionally stateless from the caller perspective: every
operation reloads the cached JSON structure on demand and persists changes
immediately.  The structure roughly looks like this::

    {
        "users": {
            "<uid>": {
                "history": [
                    {"ttn": "...", "timestamp": "...", "status_payload": {...}}
                ],
                "bookmarks": {
                    "<ttn>": {"added_at": "...", "status_payload": {...}}
                },
                "notes": {
                    "<ttn>": [{"note_id": "...", "text": "...", "timestamp": "..."}]
                },
                "assigned": {
                    "<ttn>": {"assigned_at": "..."}
                }
            }
        },
        "assignments": {
            "<ttn>": {
                "ttn": "...",
                "assigned_to": <uid>,
                "assigned_by": <admin_uid>,
                "note": "...",
                "created_at": "...",
                "updated_at": "...",
                "status_payload": {...},
                "delivered_at": "..." | null,
                "delivery_note": "..."
            }
        }
    }

All timestamps are stored as ISO strings in UTC.
"""

from __future__ import annotations

import json
import os
import re
import secrets
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional, Tuple

import requests

API_URL = "https://api.novaposhta.ua/v2.0/json/"
NOVA_POSHTA_API_KEY = "2b7d39d126d56e60cfc61d00cd0b452c"

DATA_FILE = os.path.join("data", "nova_poshta.json")

_state_cache: Optional[Dict[str, Any]] = None


def _utcnow() -> str:
    return datetime.now(timezone.utc).isoformat()


def _ensure_storage() -> None:
    os.makedirs(os.path.dirname(DATA_FILE), exist_ok=True)


def _blank_state() -> Dict[str, Any]:
    return {"users": {}, "assignments": {}}


def _load_state() -> Dict[str, Any]:
    global _state_cache
    if _state_cache is not None:
        return _state_cache
    _ensure_storage()
    if not os.path.exists(DATA_FILE):
        _state_cache = _blank_state()
        _save_state()
        return _state_cache
    try:
        with open(DATA_FILE, "r", encoding="utf-8") as fh:
            payload = json.load(fh)
        if not isinstance(payload, dict):
            raise ValueError("Invalid payload")
        payload.setdefault("users", {})
        payload.setdefault("assignments", {})
        _state_cache = payload
    except Exception:
        _state_cache = _blank_state()
        _save_state()
    return _state_cache


def _save_state() -> None:
    if _state_cache is None:
        return
    _ensure_storage()
    tmp_file = f"{DATA_FILE}.tmp"
    with open(tmp_file, "w", encoding="utf-8") as fh:
        json.dump(_state_cache, fh, ensure_ascii=False, indent=2)
    os.replace(tmp_file, DATA_FILE)


def _user_bucket(uid: int) -> Dict[str, Any]:
    state = _load_state()
    user = state["users"].setdefault(str(uid), {})
    user.setdefault("history", [])
    user.setdefault("bookmarks", {})
    user.setdefault("notes", {})
    user.setdefault("assigned", {})
    return user


def fetch_tracking(ttn: str) -> Tuple[bool, Optional[Dict[str, Any]], str]:
    """Fetch a tracking payload from the Nova Poshta API.

    Returns ``(success, payload, message)`` where ``payload`` is the first
    document dictionary on success, and ``message`` contains a human readable
    explanation for logging or user-facing errors.
    """

    number = (ttn or "").strip()
    if not number:
        return False, None, "Пустой номер ТТН"
    if not re.fullmatch(r"[0-9A-Za-z-]{5,40}", number):
        return False, None, "Некорректный формат ТТН"

    payload = {
        "apiKey": NOVA_POSHTA_API_KEY,
        "modelName": "TrackingDocument",
        "calledMethod": "getStatusDocuments",
        "methodProperties": {
            "Documents": [{"DocumentNumber": number}],
        },
    }
    try:
        response = requests.post(API_URL, json=payload, timeout=15)
        response.raise_for_status()
    except requests.RequestException as exc:
        return False, None, f"Ошибка запроса: {exc}"

    try:
        data = response.json()
    except ValueError:
        return False, None, "Не удалось разобрать ответ API"

    docs = data.get("data") or []
    if docs:
        doc = docs[0]
        if isinstance(doc, dict):
            doc = dict(doc)
            doc.setdefault("Number", number)
            doc.setdefault("DocumentNumber", number)
            return True, doc, ""

    errors = data.get("errors") or data.get("message") or data.get("error")
    if isinstance(errors, list):
        msg = "; ".join(str(e) for e in errors if e)
    else:
        msg = str(errors or "Номер не найден")
    if not msg:
        msg = "Номер не найден"
    return False, None, msg


def remember_search(uid: int, ttn: str, status_payload: Optional[Dict[str, Any]]) -> None:
    user = _user_bucket(uid)
    status_payload = status_payload or {}
    history = [entry for entry in user["history"] if entry.get("ttn") != ttn]
    history.insert(0, {
        "ttn": ttn,
        "timestamp": _utcnow(),
        "status_payload": status_payload,
    })
    user["history"] = history[:25]
    _save_state()


def get_history(uid: int) -> List[Dict[str, Any]]:
    user = _user_bucket(uid)
    return list(user["history"])


def get_cached_status(uid: int, ttn: str) -> Optional[Dict[str, Any]]:
    user = _user_bucket(uid)
    for entry in user["history"]:
        if entry.get("ttn") == ttn:
            return entry.get("status_payload")
    bookmark = user["bookmarks"].get(ttn)
    if isinstance(bookmark, dict):
        return bookmark.get("status_payload")
    assignment = _load_state()["assignments"].get(ttn)
    if isinstance(assignment, dict) and (
        assignment.get("assigned_to") == uid or assignment.get("assigned_by") == uid
    ):
        return assignment.get("status_payload")
    return None


def set_bookmark(uid: int, ttn: str, status_payload: Optional[Dict[str, Any]]) -> None:
    user = _user_bucket(uid)
    user["bookmarks"][ttn] = {
        "added_at": _utcnow(),
        "status_payload": status_payload or {},
    }
    _save_state()


def remove_bookmark(uid: int, ttn: str) -> None:
    user = _user_bucket(uid)
    user["bookmarks"].pop(ttn, None)
    _save_state()


def toggle_bookmark(uid: int, ttn: str, status_payload: Optional[Dict[str, Any]] = None) -> bool:
    user = _user_bucket(uid)
    if ttn in user["bookmarks"]:
        remove_bookmark(uid, ttn)
        return False
    if status_payload is None:
        status_payload = get_cached_status(uid, ttn) or {}
    set_bookmark(uid, ttn, status_payload)
    return True


def list_bookmarks(uid: int) -> List[Tuple[str, Dict[str, Any]]]:
    user = _user_bucket(uid)
    items = []
    for ttn, payload in user["bookmarks"].items():
        entry = dict(payload)
        entry["ttn"] = ttn
        items.append((ttn, entry))
    items.sort(key=lambda x: x[1].get("added_at", ""), reverse=True)
    return items


def has_bookmark(uid: int, ttn: str) -> bool:
    return ttn in _user_bucket(uid)["bookmarks"]


def add_note(uid: int, ttn: str, text: str) -> Dict[str, Any]:
    user = _user_bucket(uid)
    bucket = user["notes"].setdefault(ttn, [])
    note = {
        "note_id": secrets.token_hex(6),
        "ttn": ttn,
        "text": text,
        "timestamp": _utcnow(),
    }
    bucket.insert(0, note)
    user["notes"][ttn] = bucket[:20]
    _save_state()
    return note


def list_notes(uid: int, ttn: Optional[str] = None) -> Dict[str, List[Dict[str, Any]]]:
    user = _user_bucket(uid)
    notes = user["notes"]
    if ttn is not None:
        return {ttn: list(notes.get(ttn, []))}
    return {key: list(value) for key, value in notes.items() if value}


def remove_notes(uid: int, ttn: str) -> bool:
    """Remove all notes saved for the given TTN.

    Returns ``True`` if any entries were removed."""

    user = _user_bucket(uid)
    removed = bool(user["notes"].pop(ttn, None))
    if removed:
        _save_state()
    return removed


def assign_parcel(admin_uid: int, target_uid: int, ttn: str,
                  status_payload: Optional[Dict[str, Any]], note: Optional[str] = None) -> Dict[str, Any]:
    state = _load_state()
    now = _utcnow()
    assignment = state["assignments"].get(ttn, {})
    assignment.update({
        "ttn": ttn,
        "assigned_to": target_uid,
        "assigned_by": admin_uid,
        "note": note or "",
        "created_at": assignment.get("created_at") or now,
        "updated_at": now,
        "status_payload": status_payload or assignment.get("status_payload") or {},
        "delivered_at": None,
        "delivery_note": "",
    })
    state["assignments"][ttn] = assignment
    user = _user_bucket(target_uid)
    user["assigned"][ttn] = {
        "assigned_at": now,
        "assigned_by": admin_uid,
    }
    _save_state()
    return assignment


def get_assignment(ttn: str) -> Optional[Dict[str, Any]]:
    state = _load_state()
    assignment = state["assignments"].get(ttn)
    if assignment:
        return dict(assignment)
    return None


def list_assignments(uid: int) -> List[Dict[str, Any]]:
    state = _load_state()
    result: List[Dict[str, Any]] = []
    for ttn, assignment in state["assignments"].items():
        if assignment.get("assigned_to") == uid:
            entry = dict(assignment)
            entry["ttn"] = ttn
            result.append(entry)
    result.sort(key=lambda x: x.get("created_at", ""), reverse=True)
    return result


def list_admin_assignments(admin_uid: int) -> List[Dict[str, Any]]:
    state = _load_state()
    result: List[Dict[str, Any]] = []
    for ttn, assignment in state["assignments"].items():
        if assignment.get("assigned_by") == admin_uid:
            entry = dict(assignment)
            entry["ttn"] = ttn
            result.append(entry)
    result.sort(key=lambda x: x.get("created_at", ""), reverse=True)
    return result


def mark_assignment_received(uid: int, ttn: str, delivery_note: str = "") -> Optional[Dict[str, Any]]:
    state = _load_state()
    assignment = state["assignments"].get(ttn)
    if not assignment or assignment.get("assigned_to") != uid:
        return None
    assignment["delivered_at"] = _utcnow()
    assignment["delivery_note"] = delivery_note or ""
    assignment["updated_at"] = assignment["delivered_at"]
    user = _user_bucket(uid)
    bucket = user["assigned"].setdefault(ttn, {})
    bucket["delivered_at"] = assignment["delivered_at"]
    _save_state()
    return dict(assignment)


def refresh_assignment_status(ttn: str, status_payload: Optional[Dict[str, Any]]) -> None:
    state = _load_state()
    assignment = state["assignments"].get(ttn)
    if not assignment:
        return
    assignment["status_payload"] = status_payload or {}
    assignment["updated_at"] = _utcnow()
    _save_state()


__all__ = [
    "fetch_tracking",
    "remember_search",
    "get_history",
    "get_cached_status",
    "toggle_bookmark",
    "list_bookmarks",
    "has_bookmark",
    "add_note",
    "list_notes",
    "remove_notes",
    "assign_parcel",
    "list_assignments",
    "list_admin_assignments",
    "mark_assignment_received",
    "get_assignment",
    "refresh_assignment_status",
]
