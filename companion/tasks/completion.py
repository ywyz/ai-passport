"""Durable conditional VTODO completion; no creation, deletion or reopening."""
from datetime import datetime, timezone
import hashlib
import re
import threading
import time

from .common import TaskError, packed
from .model import parse_resource, task_id

TERMINAL={"SUCCEEDED","CONFLICT","NOT_FOUND","READ_ONLY","FAILED","AUTH_FAILED","RETRY_PAUSED"}


def strong_etag(value):
    return isinstance(value,str) and len(value)<=256 and bool(re.fullmatch(r'"[!#-~]+"', value))


def validate_intent(value):
    fields={"operation_id","task_id","base_etag","desired","created_at","device_sequence","payload_hash"}
    if not isinstance(value,dict) or set(value)!=fields:
        raise TaskError("INVALID_OPERATION")
    if any(not isinstance(value[k],str) or not re.fullmatch(pattern,value[k]) for k,pattern in (
            ("operation_id",r"[0-9a-f]{32}"),("task_id",r"[0-9a-f]{64}"),("payload_hash",r"[0-9a-f]{64}"))):
        raise TaskError("INVALID_OPERATION")
    if value["desired"]!="completed" or not strong_etag(value["base_etag"]):
        raise TaskError("INVALID_OPERATION")
    if type(value["device_sequence"]) is not int or not 0<value["device_sequence"]<=0xffffffff:
        raise TaskError("INVALID_OPERATION")
    if value["created_at"] is not None and (type(value["created_at"]) is not int or not 0<=value["created_at"]<0x7fffffffffffffff):
        raise TaskError("INVALID_OPERATION")
    original={k:v for k,v in value.items() if k!="payload_hash"}
    if hashlib.sha256(packed(original)).hexdigest()!=value["payload_hash"]:
        raise TaskError("PAYLOAD_MISMATCH")
    return value


def completed_calendar(raw, record, epoch):
    """Keep untouched content lines and folds byte-for-byte, including alarms."""
    items=parse_resource(raw, account_id=record["account_id"],collection_id=record["collection_id"],
                         href=record["href"],etag=record["etag"],display_zone="UTC")
    target=[item for item in items if task_id(item)==task_id(record)]
    if len(target)!=1 or target[0]["recurring"] or target[0]["has_parent"]:
        raise TaskError("READ_ONLY")
    if target[0]["status"] not in {"NEEDS-ACTION","IN-PROCESS"}:
        raise TaskError("CONFLICT")
    # Multi-instance resources are deliberately read-only in this increment.
    if len(items)!=1: raise TaskError("READ_ONLY")
    lines=raw.splitlines(keepends=True); groups=[]
    for line in lines:
        if line.startswith((b" ",b"\t")):
            if not groups: raise TaskError("INVALID_CALENDAR")
            groups[-1]+=line
        else: groups.append(line)
    stamp=datetime.fromtimestamp(epoch,timezone.utc).strftime("%Y%m%dT%H%M%SZ").encode()
    updates={b"STATUS":b"COMPLETED",b"PERCENT-COMPLETE":b"100",b"COMPLETED":stamp,
             b"LAST-MODIFIED":stamp,b"DTSTAMP":stamp}
    output=[]; depth=0; todo_depth=None; edited=False
    for group in groups:
        unfolded=re.sub(rb"\r?\n[ \t]",b"",group).strip(b"\r\n")
        upper=unfolded.upper()
        if upper==b"END:VTODO" and todo_depth==depth:
            for key,value in updates.items(): output.append(key+b":"+value+b"\r\n")
            todo_depth=None; edited=True
        if upper.startswith(b"END:"): depth-=1
        name=unfolded.split(b":",1)[0].split(b";",1)[0].upper()
        if not (todo_depth==depth and name in updates): output.append(group)
        if upper.startswith(b"BEGIN:"):
            depth+=1
            if upper==b"BEGIN:VTODO": todo_depth=depth
    result=b"".join(output)
    if not edited or depth: raise TaskError("INVALID_CALENDAR")
    # Independently validate the result before admitting a conditional PUT.
    check=parse_resource(result,account_id=record["account_id"],collection_id=record["collection_id"],
                         href=record["href"],etag=record["etag"],display_zone="UTC")
    if check[0]["status"]!="COMPLETED" or check[0]["percent_complete"]!=100:
        raise TaskError("INVALID_CALENDAR")
    return result


class CompletionWorker:
    def __init__(self, store, client):
        self.store,self.client=store,client
        self.stop_event=threading.Event()
        self.thread=threading.Thread(target=self.run, name="task-completion",daemon=True)

    def start(self): self.thread.start()
    def stop(self): self.stop_event.set(); self.thread.join(timeout=2)

    def run(self):
        while not self.stop_event.is_set():
            try:
                completed=self.step()
                creator=getattr(self,"creation_worker",None)
                created=creator.step() if creator is not None else False
                if completed or created: continue
            except Exception:
                pass  # Never print private calendar/transport exception text.
            self.stop_event.wait(1)

    def step(self):
        operation=self.store.claim_completion()
        if operation is None: return False
        identifier=operation["operation_id"]
        try:
            record=operation["record"]
            if self.store.state()["auth_halted"]: raise TaskError("AUTH_FAILED")
            selected=self.store.collections()["selected"]
            if record["collection_id"] not in selected: raise TaskError("READ_ONLY")
            collections=self.client.discover()
            collection=next((c for c in collections if c["id"]==record["collection_id"]),None)
            if collection is None or not collection.get("completion_allowed"): raise TaskError("READ_ONLY")
            raw,etag=self.client.transport.get_resource(record["href"])
            if not strong_etag(etag): raise TaskError("CONFLICT")
            if etag!=operation["base_etag"]:
                if not operation["submitted_sha"] or hashlib.sha256(raw).hexdigest()!=operation["submitted_sha"]:
                    raise TaskError("CONFLICT")
            else:
                payload=completed_calendar(raw,record,int(self.store.clock()))
                self.store.submitting(identifier,hashlib.sha256(payload).hexdigest())
                self.client.transport.put_existing(record["href"],payload,etag)
                raw,etag=self.client.transport.get_resource(record["href"])
                if not strong_etag(etag): raise TaskError("CONFLICT")
            records=parse_resource(raw,account_id=record["account_id"],collection_id=record["collection_id"],
                                   href=record["href"],etag=etag,display_zone=self.client.display_zone)
            actual=next((r for r in records if task_id(r)==operation["task_id"]),None)
            if actual is None or actual["status"]!="COMPLETED" or actual["percent_complete"]!=100 or actual["completed_at"]["kind"]!="utc":
                raise TaskError("CONFLICT")
            self.store.completion_succeeded(identifier,records,record["href"],etag,raw)
        except TaskError as error:
            self.store.completion_failed(identifier,error.code)
        except Exception:
            self.store.completion_failed(identifier,"UNAVAILABLE")
        return True
