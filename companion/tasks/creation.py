"""Durable, idempotent creation into one explicitly selected owned collection."""
from datetime import datetime, timezone
import hashlib
import json
import re
from .common import TaskError, packed, bounded, MAX_CACHE_BYTES, MAX_RECORD_BYTES, MAX_ITEMS
from .completion import strong_etag
from .model import parse_resource, sort_key
from .speech import title


def validate(value):
    if not isinstance(value, dict) or set(value) != {"operation_id","collection_id","summary","payload_hash"}:
        raise TaskError("INVALID_OPERATION")
    for key, size in (("operation_id",32),("collection_id",64),("payload_hash",64)):
        if not isinstance(value[key],str) or not re.fullmatch("[0-9a-f]{%d}" % size,value[key]):
            raise TaskError("INVALID_OPERATION")
    title(value["summary"])
    if hashlib.sha256(packed({k:v for k,v in value.items() if k!="payload_hash"})).hexdigest() != value["payload_hash"]:
        raise TaskError("PAYLOAD_MISMATCH")


def calendar(intent, epoch):
    summary=title(intent["summary"]).replace("\\","\\\\").replace(";","\\;").replace(",","\\,")
    stamp=datetime.fromtimestamp(epoch,timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    lines=["BEGIN:VCALENDAR","VERSION:2.0","PRODID:-//AI Passport//Tasks//EN","BEGIN:VTODO",
           "UID:"+intent["operation_id"]+"@ai-passport","DTSTAMP:"+stamp,"CREATED:"+stamp,
           "SUMMARY:"+summary,"STATUS:NEEDS-ACTION","PERCENT-COMPLETE:0","END:VTODO","END:VCALENDAR"]
    # RFC 5545 folds count octets; never split a UTF-8 code point.
    result=[]
    for line in lines:
        part=""
        for char in line:
            if len((part+char).encode())>75:
                result.append(part); part=" "
            part+=char
        result.append(part)
    return ("\r\n".join(result)+"\r\n").encode()


class CreationStore:
    def __init__(self, store):
        self.store=store
        with store.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS creations (
                  operation_id TEXT PRIMARY KEY, payload_hash TEXT NOT NULL,
                  intent TEXT NOT NULL, collection TEXT NOT NULL, state TEXT NOT NULL,
                  created INTEGER NOT NULL, updated INTEGER NOT NULL,
                  attempts INTEGER NOT NULL DEFAULT 0, due INTEGER NOT NULL,
                  lease INTEGER NOT NULL DEFAULT 0, error TEXT);
                CREATE TABLE IF NOT EXISTS speech_usage (day INTEGER PRIMARY KEY, seconds INTEGER NOT NULL);
            """)

    def target(self):
        state=self.store.state()
        selected=json.loads(state["selection"])
        eligible=[c for c in json.loads(state["collections"]) if c["id"] in selected and c.get("creation_allowed")]
        if state["auth_halted"]: raise TaskError("AUTH_FAILED")
        if len(selected)!=1 or len(eligible)!=1: raise TaskError("CREATION_TARGET_REQUIRED")
        return eligible[0]

    def charge(self, seconds, limit):
        day=int(self.store.clock())//86400
        with self.store.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            db.execute("DELETE FROM speech_usage WHERE day<?",(day-7,))
            db.execute("INSERT OR IGNORE INTO speech_usage VALUES(?,0)",(day,))
            if db.execute("SELECT seconds FROM speech_usage WHERE day=?",(day,)).fetchone()[0]+seconds>limit:
                raise TaskError("SPEECH_BUDGET_EXCEEDED")
            db.execute("UPDATE speech_usage SET seconds=seconds+? WHERE day=?",(seconds,day))

    @staticmethod
    def receipt(row):
        intent=json.loads(row["intent"])
        return {"schema":1,"operation_id":row["operation_id"],"payload_hash":row["payload_hash"],
                "collection_id":intent["collection_id"],"state":row["state"]}

    def accept(self, intent, retry=False):
        validate(intent); now=int(self.store.clock())
        with self.store.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            old=db.execute("SELECT * FROM creations WHERE operation_id=?",(intent["operation_id"],)).fetchone()
            if old is not None:
                if old["payload_hash"]!=intent["payload_hash"]: raise TaskError("OPERATION_CONFLICT")
                if retry and old["state"]=="RETRY_PAUSED":
                    db.execute("UPDATE creations SET state='SERVER_DURABLE',attempts=0,due=?,updated=?,lease=0,error=NULL WHERE operation_id=?",(now,now,intent["operation_id"]))
                    old=db.execute("SELECT * FROM creations WHERE operation_id=?",(intent["operation_id"],)).fetchone()
                return self.receipt(old)
            if db.execute("SELECT 1 FROM completions WHERE operation_id=?",(intent["operation_id"],)).fetchone():
                raise TaskError("OPERATION_CONFLICT")
            if db.execute("SELECT COUNT(*) FROM creations").fetchone()[0]>=1024: raise TaskError("QUEUE_FULL")
            target=self.target()
            if intent["collection_id"]!=target["id"]: raise TaskError("READ_ONLY")
            state=db.execute("SELECT snapshot FROM state WHERE id=1").fetchone()
            pending=db.execute("SELECT COUNT(*) FROM creations WHERE state IN ('SERVER_DURABLE','REMOTE_APPLYING','RETRY_WAIT','RETRY_PAUSED')").fetchone()[0]
            if state[0] is None or len(json.loads(state[0]))+pending>=MAX_ITEMS: raise TaskError("QUEUE_FULL")
            db.execute("INSERT INTO creations(operation_id,payload_hash,intent,collection,state,created,updated,due) VALUES(?,?,?,?,'SERVER_DURABLE',?,?,?)",
                       (intent["operation_id"],intent["payload_hash"],packed(intent).decode(),packed(target).decode(),now,now,now))
            return self.receipt(db.execute("SELECT * FROM creations WHERE operation_id=?",(intent["operation_id"],)).fetchone())

    def claim(self):
        now=int(self.store.clock())
        with self.store.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row=db.execute("SELECT * FROM creations WHERE (state IN ('SERVER_DURABLE','RETRY_WAIT') AND due<=?) OR (state='REMOTE_APPLYING' AND lease<=?) ORDER BY created,operation_id LIMIT 1",(now,now)).fetchone()
            if row is None: return None
            db.execute("UPDATE creations SET state='REMOTE_APPLYING',attempts=attempts+1,lease=?,updated=? WHERE operation_id=?",(now+600,now,row["operation_id"]))
            return dict(row)

    def finish(self, row, error=None, result=None):
        now=int(self.store.clock())
        with self.store.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            if error:
                status=error if error in {"CONFLICT","READ_ONLY","AUTH_FAILED","NOT_FOUND"} else "RETRY_PAUSED" if row["attempts"]>=4 else "RETRY_WAIT"
                db.execute("UPDATE creations SET state=?,error=?,due=?,updated=?,lease=0 WHERE operation_id=?",(status,error,now+min(300,2**(row["attempts"]+1)),now,row["operation_id"]))
                if error=="AUTH_FAILED": db.execute("UPDATE state SET auth_halted=1,last_error='AUTH_FAILED' WHERE id=1")
                return
            records,href,etag,raw=result
            state=db.execute("SELECT * FROM state WHERE id=1").fetchone()
            selected=json.loads(state["selection"])
            existing=json.loads(state["snapshot"] or "[]")
            intent=json.loads(row["intent"])
            # Never overwrite a concurrently refreshed or edited resource.
            if (state["snapshot_selection"]==state["selection_revision"] and intent["collection_id"] in selected
                    and not any(r["href"]==href for r in existing) and len(existing)+len(records)<=MAX_ITEMS):
                import uuid
                revision=uuid.uuid4().hex
                items=sorted(existing+records,key=sort_key)
                items=[dict(r,snapshot_revision=revision,downloaded_at=state["downloaded"]) for r in items]
                for item in items: bounded(item,MAX_RECORD_BYTES)
                payload=bounded(items,MAX_CACHE_BYTES).decode()
                db.execute("INSERT OR IGNORE INTO originals VALUES(?,?,?)",(href,etag,raw))
                db.execute("UPDATE state SET snapshot=?,revision=? WHERE id=1",(payload,revision))
            db.execute("UPDATE creations SET state='SUCCEEDED',error=NULL,updated=?,lease=0 WHERE operation_id=?",(now,row["operation_id"]))


class CreationWorker:
    def __init__(self, store, client): self.store,self.client=store,client

    def step(self):
        row=self.store.claim()
        if row is None: return False
        try:
            intent=json.loads(row["intent"]); target=self.store.target()
            live=next((c for c in self.client.discover() if c["id"]==intent["collection_id"]),None)
            if (not live or not live.get("creation_allowed") or target["id"]!=intent["collection_id"]
                    or live["href"]!=json.loads(row["collection"])["href"]): raise TaskError("READ_ONLY")
            href=live["href"]+"passport-"+intent["operation_id"]+".ics"
            payload=calendar(intent,row["created"])
            try:
                raw,etag=self.client.transport.get_resource(href,allow_missing=True)
            except TaskError as error:
                if error.code!="NOT_FOUND": raise
                try: self.client.transport.put_new(href,payload)
                except TaskError as error:
                    if error.code!="CONFLICT": raise
                raw,etag=self.client.transport.get_resource(href)
            if raw!=payload or not strong_etag(etag): raise TaskError("CONFLICT")
            records=parse_resource(raw,account_id=self.client.account_id,collection_id=intent["collection_id"],href=href,etag=etag,display_zone=self.client.display_zone)
            for record in records: record["completion_allowed"]=bool(live.get("completion_allowed"))
            self.store.finish(row,result=(records,href,etag,raw))
        except TaskError as error: self.store.finish(row,error="READ_ONLY" if error.code=="CREATION_TARGET_REQUIRED" else error.code)
        except Exception: self.store.finish(row,error="UNAVAILABLE")
        return True
