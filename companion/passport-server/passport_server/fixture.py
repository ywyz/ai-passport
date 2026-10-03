"""Deterministic D1 fixture pack builder (contract section 11).

Fictional Jiangsu test material, `fixture: true`, `TEST-001` ticket with
cross-midnight travel and intermediate boarding, guide route fixture with
stops stop-001..stop-003, and an attraction labeled with U+752A for glyph
testing. Text content is produced here; digest/glyph data computed by the
server at publication. No real administrative data, no real train.
"""
from __future__ import annotations

import json

PACK_ID = "fixture-js-trip-001"
PROVINCE_ID = "CN-JS"

TICKET = {
    "schema_version": 1,
    "fixture": True,
    "id": "fixture-ticket-001",
    "revision": 1,
    "itinerary_id": PACK_ID,
    "travel_date": "2026-10-03",
    "timezone": "Asia/Shanghai",
    "train_number": "TEST-001",
    "boarding_station": {"id": "fixture-station-b", "name": "\u6837\u672c\u7ad9 B"},
    "arrival_station": {"id": "fixture-station-d", "name": "\u6837\u672c\u7ad9 D"},
    "carriage": "02",
    "seat": "03A",
    "departure_at": "2026-10-03T23:58:00+08:00",
    "train_model": None,
    "missing_reason": {"train_model": "source_unavailable"},
    "review_state": "published",
    "provenance": {
        "source_type": "fixture",
        "source_id": "fixture-ticket-001",
        "fetched_at": None,
        "reviewed_fields": ["boarding_station", "arrival_station", "train_number",
                            "travel_date", "carriage", "seat", "departure_at"],
    },
    "stops": [
        {"station_id": "fixture-station-a", "name": "\u6837\u672c\u7ad9 A",
         "sequence": 0, "arrival_at": None,
         "departure_at": "2026-10-03T23:20:00+08:00", "time_kind": "scheduled"},
        {"station_id": "fixture-station-b", "name": "\u6837\u672c\u7ad9 B",
         "sequence": 1, "arrival_at": "2026-10-03T23:55:00+08:00",
         "departure_at": "2026-10-03T23:58:00+08:00", "time_kind": "scheduled"},
        {"station_id": "fixture-station-c", "name": "\u6837\u672c\u7ad9 C",
         "sequence": 2, "arrival_at": "2026-10-04T00:20:00+08:00",
         "departure_at": "2026-10-04T00:25:00+08:00", "time_kind": "scheduled"},
        {"station_id": "fixture-station-d", "name": "\u6837\u672c\u7ad9 D",
         "sequence": 3, "arrival_at": "2026-10-04T01:00:00+08:00",
         "departure_at": None, "time_kind": "scheduled"},
    ],
}

ITINERARY = {
    "schema_version": 1,
    "fixture": True,
    "id": PACK_ID,
    "revision": 1,
    "title": "\u6d4b\u8bd5\u7248\u82cf\u5dde\u884c\u7a0b",
    "timezone": "Asia/Shanghai",
    "start_date": "2026-10-03",
    "end_date": "2026-10-04",
    "entry_ids": ["fixture-ticket-001"],
}

PLACES = {
    "schema_version": 1,
    "fixture": True,
    "province_id": PROVINCE_ID,
    "places": [
        {"id": "place-cn-jiangsu", "province_id": PROVINCE_ID, "parent_id": None,
         "kind": "province", "name": "\u6c5f\u82cf", "initials": "JS"},
        {"id": "place-cn-jiangsu-suzhou", "province_id": PROVINCE_ID,
         "parent_id": "place-cn-jiangsu", "kind": "city",
         "name": "\u82cf\u5dde", "initials": "SZ"},
        {"id": "place-cn-jiangsu-suzhou-test", "province_id": PROVINCE_ID,
         "parent_id": "place-cn-jiangsu-suzhou", "kind": "county",
         "name": "\u6d4b\u8bd5\u533a", "initials": "CS"},
        {"id": "stop-001", "province_id": PROVINCE_ID,
         "parent_id": "place-cn-jiangsu-suzhou-test", "kind": "attraction",
         "name": "\u752a\u5c71\u6d4b\u8bd5\u666f\u70b9", "initials": "MSS"},
        {"id": "stop-002", "province_id": PROVINCE_ID,
         "parent_id": "place-cn-jiangsu-suzhou-test", "kind": "attraction",
         "name": "\u82cf\u5dde\u6837\u4f8b\u56ed", "initials": "SZLY"},
        {"id": "stop-003", "province_id": PROVINCE_ID,
         "parent_id": "place-cn-jiangsu-suzhou-test", "kind": "attraction",
         "name": "\u5047\u540d\u53e4\u9547", "initials": "JMGZ"},
    ],
}

GUIDE_ROUTE = {
    "schema_version": 1,
    "fixture": True,
    "id": "fixture-route-001",
    "revision": 1,
    "itinerary_id": PACK_ID,
    "review_state": "published",
    "ordered_stop_ids": ["stop-001", "stop-002", "stop-003"],
}

STOP_TEXTS = {
    "stop-001": ("\u752a\u5c71\u6d4b\u8bd5\u666f\u70b9\uff08\u6d4b\u8bd5\u6570\u636e\uff09\n"
                 "\u8fd9\u662f\u5b57\u5f62\u8986\u76d6\u6d4b\u8bd5\u6587\u672c\uff0c"
                 "\u5305\u542b\u5b57\u5f62 U+752A \u4e0e\u8d1f\u4f8b\u3002\n"
                 "\u5f00\u653e\u65f6\u95f4\uff1a\u6d4b\u8bd5 09:00-17:00\uff0c"
                 "\u6570\u636e\u4e3a\u865a\u6784\u3002\n"),
    "stop-002": ("\u82cf\u5dde\u6837\u4f8b\u56ed\uff08\u6d4b\u8bd5\u6570\u636e\uff09\n"
                 "\u4e8c\u53f7\u505c\u9760\u70b9\u6587\u672c\u3002\u6ce8\u610f"
                 "\u5047\u5b57 U+2A302 \u4e0d\u5728\u5b57\u5f62\u5e93\u4e2d\n"),
    "stop-003": ("\u5047\u540d\u53e4\u9547\uff08\u6d4b\u8bd5\u6570\u636e\uff09\n"
                 "\u4e09\u53f7\u505c\u9760\u70b9\u3002\u672a\u77e5\u5b57 U+3FFFF \u4e3a\u8d1f\u4f8b\u3002\n"),
}


def required_glyphs() -> list[int]:
    """Code points actually present in published fixture texts (stop-001)."""
    text = STOP_TEXTS["stop-001"] + ITINERARY["title"] + \
        PLACES["places"][0]["name"] + PLACES["places"][2]["name"]
    return sorted({cp for cp in map(ord, text) if cp > 0x7F})


def fixture_pack_payload() -> dict:
    files = [
        {"path": "data/itinerary.json", "media_type": "application/json",
         "text": json.dumps(ITINERARY, ensure_ascii=False, indent=1)},
        {"path": "data/ticket.json", "media_type": "application/json",
         "text": json.dumps(TICKET, ensure_ascii=False, indent=1)},
        {"path": "data/places-index.json", "media_type": "application/json",
         "text": json.dumps(PLACES, ensure_ascii=False, indent=1)},
        {"path": "data/guide-route.json", "media_type": "application/json",
         "text": json.dumps(GUIDE_ROUTE, ensure_ascii=False, indent=1)},
    ]
    for stop_id, text in STOP_TEXTS.items():
        files.append({"path": f"text/{stop_id}.txt", "media_type": "text/plain",
                      "text": text})
    payload = {
        "schema_version": 1,
        "pack_id": PACK_ID,
        "kind": "guide",
        "label": "\u6d4b\u8bd5\u7248\u82cf\u5dde\u884c\u7a0b (fixture)",
        "fixture": True,
        "glyph_inventory": required_glyphs(),
        "guide_targets": {"guide_route": "fixture-route-001",
                          "stops": GUIDE_ROUTE["ordered_stop_ids"]},
        "files": files,
    }
    return payload
